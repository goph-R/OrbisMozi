/* The player core. Four threads:
 *
 *   demux   reads packets from the file and sorts them into two queues
 *   video   decodes video packets into a small queue of finished frames
 *   audio   decodes audio, converts to 48 kHz stereo and feeds the device
 *   main    shows each frame when the audio clock reaches its timestamp
 *
 * Audio is the master clock: sound plays at the device's own pace and the
 * picture follows it, so the two cannot drift apart. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "osd.h"
#include "plat.h"
#include "player.h"

#define PKTQ_CAP        2048
#define PKTQ_ENOUGH     48                  /* packets per stream considered "full" */
#define PKTQ_MAX_BYTES  (48 * 1024 * 1024)
#define FRAMEQ_CAP      6
#define VIDEO_THREADS   4
#define VIDEO_THREADS_HD 6                  /* for sources above 720p */
#define AUDIO_LATENCY   0.040               /* device + mixer delay, seconds (estimate) */
#define LATE_DROP       0.100               /* drop frames more than this late */

typedef struct {
    AVPacket *pkt[PKTQ_CAP];
    int       head, n;
    int64_t   bytes;
} PktQ;

typedef struct {
    AVFrame *frame;
    double   pts;
} QFrame;

static struct {
    AVFormatContext *fmt;
    AVCodecContext  *vdec, *adec;
    int              vidx, aidx;

    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    PktQ             vq, aq;
    QFrame           fq[FRAMEQ_CAP];
    int              fq_head, fq_n;
    int              demux_eof, vdec_eof, adec_eof, quit;

    /* Audio clock: timestamp of the next sample to be queued, and when. */
    double           aclock_pts;
    uint64_t         aclock_time;
    int              aclock_valid;
} P;

/* --- packet queues (all guarded by P.lock) ------------------------------ */

static void pktq_push(PktQ *q, AVPacket *pkt)
{
    q->pkt[(q->head + q->n) % PKTQ_CAP] = pkt;
    q->n++;
    q->bytes += pkt->size;
}

/* Returns 1 with a packet, 0 at end of stream, -1 when asked to quit. */
static int pktq_pop(PktQ *q, AVPacket **out)
{
    int rc;

    pthread_mutex_lock(&P.lock);
    while (!P.quit && q->n == 0 && !P.demux_eof)
        pthread_cond_wait(&P.cond, &P.lock);

    if (P.quit) {
        rc = -1;
    } else if (q->n > 0) {
        *out = q->pkt[q->head];
        q->head = (q->head + 1) % PKTQ_CAP;
        q->n--;
        q->bytes -= (*out)->size;
        pthread_cond_broadcast(&P.cond);
        rc = 1;
    } else {
        rc = 0;
    }
    pthread_mutex_unlock(&P.lock);
    return rc;
}

static void pktq_clear(PktQ *q)
{
    while (q->n > 0) {
        av_packet_free(&q->pkt[q->head]);
        q->head = (q->head + 1) % PKTQ_CAP;
        q->n--;
    }
}

static int demux_must_wait(void)
{
    int v_ok = P.vidx < 0 || P.vq.n >= PKTQ_ENOUGH;
    int a_ok = P.aidx < 0 || P.aq.n >= PKTQ_ENOUGH;

    if (P.vq.n >= PKTQ_CAP - 1 || P.aq.n >= PKTQ_CAP - 1)
        return 1;
    if (P.vq.bytes + P.aq.bytes > PKTQ_MAX_BYTES)
        return 1;
    return v_ok && a_ok;
}

/* --- demux thread ------------------------------------------------------- */

static void *demux_thread(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (!P.quit && demux_must_wait())
            pthread_cond_wait(&P.cond, &P.lock);
        int quit = P.quit;
        pthread_mutex_unlock(&P.lock);
        if (quit)
            break;

        AVPacket *pkt = av_packet_alloc();
        int rc = av_read_frame(P.fmt, pkt);
        if (rc < 0) {
            if (rc != AVERROR_EOF)
                osd_log("read error: %s", av_err2str(rc));
            av_packet_free(&pkt);
            break;
        }

        pthread_mutex_lock(&P.lock);
        if (pkt->stream_index == P.vidx)
            pktq_push(&P.vq, pkt);
        else if (pkt->stream_index == P.aidx)
            pktq_push(&P.aq, pkt);
        else
            av_packet_free(&pkt);
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);
    }

    pthread_mutex_lock(&P.lock);
    P.demux_eof = 1;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    return NULL;
}

/* --- video decode thread ------------------------------------------------ */

static void *video_thread(void *arg)
{
    AVStream *st = P.fmt->streams[P.vidx];
    double tb = av_q2d(st->time_base);
    double frame_dur = st->avg_frame_rate.num > 0 ? 1.0 / av_q2d(st->avg_frame_rate) : 1.0 / 25.0;
    double next_pts = 0.0;
    AVFrame *frame = av_frame_alloc();
    int stop = 0;
    (void)arg;

    while (!stop) {
        AVPacket *pkt = NULL;
        int got = pktq_pop(&P.vq, &pkt);
        if (got < 0)
            break;

        int rc = avcodec_send_packet(P.vdec, got ? pkt : NULL);   /* NULL = drain */
        av_packet_free(&pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && rc != AVERROR_INVALIDDATA)
            osd_log("video decode error: %s", av_err2str(rc));

        while (avcodec_receive_frame(P.vdec, frame) == 0) {
            double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                       ? frame->best_effort_timestamp * tb : next_pts;
            next_pts = pts + frame_dur;

            pthread_mutex_lock(&P.lock);
            while (!P.quit && P.fq_n == FRAMEQ_CAP)
                pthread_cond_wait(&P.cond, &P.lock);
            if (P.quit) {
                pthread_mutex_unlock(&P.lock);
                av_frame_unref(frame);
                stop = 1;
                break;
            }
            QFrame *slot = &P.fq[(P.fq_head + P.fq_n) % FRAMEQ_CAP];
            slot->frame = av_frame_alloc();
            av_frame_move_ref(slot->frame, frame);
            slot->pts = pts;
            P.fq_n++;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
        }

        if (!got)
            break;                                  /* drained */
    }

    av_frame_free(&frame);
    pthread_mutex_lock(&P.lock);
    P.vdec_eof = 1;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    return NULL;
}

/* --- audio thread ------------------------------------------------------- */

static void *audio_thread(void *arg)
{
    AVStream *st = P.fmt->streams[P.aidx];
    double tb = av_q2d(st->time_base);
    AVFrame *frame = av_frame_alloc();
    SwrContext *swr = NULL;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout = { 0 };
    int in_rate = 0, in_fmt = -1;

    int16_t *buf = NULL;                 /* converted audio waiting to be played */
    int cap = 0, rd = 0, buffered = 0;   /* in stereo frames */
    double buf_pts = NAN;                /* timestamp of buf[rd] */
    int stop = 0;
    (void)arg;

    while (!stop) {
        AVPacket *pkt = NULL;
        int got = pktq_pop(&P.aq, &pkt);
        if (got < 0)
            break;

        int rc = avcodec_send_packet(P.adec, got ? pkt : NULL);
        av_packet_free(&pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && rc != AVERROR_INVALIDDATA)
            osd_log("audio decode error: %s", av_err2str(rc));

        while (!stop && avcodec_receive_frame(P.adec, frame) == 0) {
            /* (Re)create the converter when the input format changes. */
            if (!swr || frame->sample_rate != in_rate || frame->format != in_fmt ||
                av_channel_layout_compare(&frame->ch_layout, &in_layout) != 0) {
                swr_free(&swr);
                av_channel_layout_uninit(&in_layout);
                av_channel_layout_copy(&in_layout, &frame->ch_layout);
                in_rate = frame->sample_rate;
                in_fmt = frame->format;
                rc = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE,
                                         &in_layout, in_fmt, in_rate, 0, NULL);
                if (rc < 0 || swr_init(swr) < 0) {
                    osd_log("cannot convert audio format - sound disabled");
                    swr_free(&swr);
                    stop = 1;
                    break;
                }
            }

            /* Make room: compact, then grow if needed. */
            int max_out = swr_get_out_samples(swr, frame->nb_samples);
            if (rd > 0) {
                memmove(buf, buf + rd * 2, (size_t)buffered * 2 * sizeof(int16_t));
                rd = 0;
            }
            if (buffered + max_out > cap) {
                cap = buffered + max_out + AUDIO_GRAIN;
                buf = av_realloc(buf, (size_t)cap * 2 * sizeof(int16_t));
            }

            if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                buf_pts = frame->best_effort_timestamp * tb - (double)buffered / AUDIO_RATE;

            uint8_t *out = (uint8_t *)(buf + buffered * 2);
            int n = swr_convert(swr, &out, max_out,
                                (const uint8_t **)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
            if (n > 0)
                buffered += n;

            while (buffered >= AUDIO_GRAIN) {
                plat_audio_write(buf + rd * 2);
                rd += AUDIO_GRAIN;
                buffered -= AUDIO_GRAIN;

                pthread_mutex_lock(&P.lock);
                if (!isnan(buf_pts)) {
                    buf_pts += (double)AUDIO_GRAIN / AUDIO_RATE;
                    P.aclock_pts = buf_pts;
                    P.aclock_time = plat_time_us();
                    P.aclock_valid = 1;
                }
                stop = P.quit;
                pthread_mutex_unlock(&P.lock);
                if (stop)
                    break;
            }
        }

        if (!got)
            break;
    }

    av_frame_free(&frame);
    swr_free(&swr);
    av_channel_layout_uninit(&in_layout);
    av_free(buf);

    pthread_mutex_lock(&P.lock);
    P.adec_eof = 1;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    return NULL;
}

/* --- setup helpers ------------------------------------------------------ */

static AVCodecContext *open_decoder(AVStream *st, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        osd_log("no decoder for codec '%s'", avcodec_get_name(st->codecpar->codec_id));
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (threads > 1) {
        ctx->thread_count = threads;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    int rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0) {
        osd_log("cannot open decoder '%s': %s", codec->name, av_err2str(rc));
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

typedef struct {
    struct SwsContext *sws;
    int src_w, src_h, src_fmt;
    int dst_x, dst_y, dst_w, dst_h;
    int clear_pending;                  /* buffers that still need black borders */
} Scaler;

static int scaler_setup(Scaler *s, const AVFrame *f, AVRational stream_sar)
{
    int W = plat_width(), H = plat_height();
    AVRational sar = f->sample_aspect_ratio;

    if (sar.num <= 0 || sar.den <= 0) sar = stream_sar;
    if (sar.num <= 0 || sar.den <= 0) sar = (AVRational){ 1, 1 };

    double aspect = (double)f->width * sar.num / ((double)f->height * sar.den);
    int dw = W, dh = (int)lrint(W / aspect);
    if (dh > H) {
        dh = H;
        dw = (int)lrint(H * aspect);
    }
    dw &= ~1;
    dh &= ~1;

    sws_freeContext(s->sws);
    s->sws = sws_getContext(f->width, f->height, f->format, dw, dh, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->sws) {
        osd_log("cannot set up picture conversion for %s", av_get_pix_fmt_name(f->format));
        return -1;
    }

    /* HD material uses BT.709 colours, SD uses BT.601. */
    int cs = f->colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED)
        cs = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    sws_setColorspaceDetails(s->sws, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG,
                             sws_getCoefficients(AVCOL_SPC_BT709), 1, 0, 1 << 16, 1 << 16);

    s->src_w = f->width;
    s->src_h = f->height;
    s->src_fmt = f->format;
    s->dst_w = dw;
    s->dst_h = dh;
    s->dst_x = (W - dw) / 2;
    s->dst_y = (H - dh) / 2;
    s->clear_pending = 2;
    return 0;
}

/* Current playback position in seconds, or NAN if not known yet. */
static double master_clock(uint64_t wall_start, double first_pts)
{
    uint64_t now = plat_time_us();
    double clock = NAN;

    if (P.aidx >= 0) {
        pthread_mutex_lock(&P.lock);
        if (P.aclock_valid) {
            double since = (double)(now - P.aclock_time) / 1e6;
            /* While audio is running, never run ahead of it by much. */
            if (!P.adec_eof && since > 0.1)
                since = 0.1;
            clock = P.aclock_pts - AUDIO_LATENCY + since;
        } else if (P.adec_eof) {
            clock = first_pts + (double)(now - wall_start) / 1e6;   /* audio failed */
        }
        pthread_mutex_unlock(&P.lock);
    } else {
        clock = first_pts + (double)(now - wall_start) / 1e6;
    }
    return clock;
}

static void log_to_screen(void *avcl, int level, const char *fmt, va_list ap)
{
    static int shown;
    char line[200];
    (void)avcl;

    if (level > AV_LOG_ERROR || shown >= 30)
        return;
    vsnprintf(line, sizeof(line), fmt, ap);
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0]) {
        shown++;
        osd_log("ffmpeg: %s", line);
    }
}

/* --- main entry --------------------------------------------------------- */

int player_run(const char *path, double max_seconds, PlayerStats *stats)
{
    pthread_t demux_t, video_t, audio_t;
    Scaler scaler = { 0 };
    PlayerStats st = { 0 };
    int rc;

    memset(&P, 0, sizeof(P));
    pthread_mutex_init(&P.lock, NULL);
    pthread_cond_init(&P.cond, NULL);
    av_log_set_callback(log_to_screen);

    rc = avformat_open_input(&P.fmt, path, NULL, NULL);
    if (rc < 0) {
        osd_log("cannot open file: %s", av_err2str(rc));
        return -1;
    }
    rc = avformat_find_stream_info(P.fmt, NULL);
    if (rc < 0) {
        osd_log("cannot read stream info: %s", av_err2str(rc));
        avformat_close_input(&P.fmt);
        return -1;
    }

    P.vidx = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (P.vidx >= 0 && (P.fmt->streams[P.vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        P.vidx = -1;                                /* only cover art */
    P.aidx = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vidx, NULL, 0);

    if (P.vidx < 0) {
        osd_log("no video stream in this file");
        avformat_close_input(&P.fmt);
        return -1;
    }

    AVStream *vst = P.fmt->streams[P.vidx];

    /* Sources larger than 720p get a 1080p screen and more decoder threads;
     * everything else stays at 720p, which is less work per frame. */
    int big = vst->codecpar->width > 1280 || vst->codecpar->height > 720;
    if (plat_set_mode(big ? 1920 : 1280, big ? 1080 : 720) == 0)
        osd_log("screen set to %dx%d", plat_width(), plat_height());

    P.vdec = open_decoder(vst, big ? VIDEO_THREADS_HD : VIDEO_THREADS);
    if (!P.vdec) {
        avformat_close_input(&P.fmt);
        return -1;
    }
    osd_log("video: %s %dx%d %.3f fps", avcodec_get_name(vst->codecpar->codec_id),
            vst->codecpar->width, vst->codecpar->height, av_q2d(vst->avg_frame_rate));

    if (P.aidx >= 0) {
        AVStream *ast = P.fmt->streams[P.aidx];
        P.adec = open_decoder(ast, 1);
        if (P.adec) {
            AVDictionaryEntry *lang = av_dict_get(ast->metadata, "language", NULL, 0);
            osd_log("audio: %s %d Hz %d ch [%s]", avcodec_get_name(ast->codecpar->codec_id),
                    ast->codecpar->sample_rate, ast->codecpar->ch_layout.nb_channels,
                    lang ? lang->value : "?");
        } else {
            osd_log("playing without sound");
            P.aidx = -1;
        }
    } else {
        osd_log("no audio stream");
    }

    /* Ignore every stream we do not play, so the demuxer skips them cheaply. */
    for (unsigned i = 0; i < P.fmt->nb_streams; i++)
        if ((int)i != P.vidx && (int)i != P.aidx)
            P.fmt->streams[i]->discard = AVDISCARD_ALL;

    pthread_create(&demux_t, NULL, demux_thread, NULL);
    pthread_create(&video_t, NULL, video_thread, NULL);
    if (P.aidx >= 0)
        pthread_create(&audio_t, NULL, audio_thread, NULL);

    /* --- display loop --- */
    uint64_t wall_start = 0, last_stat = 0;
    double first_pts = NAN, last_diff = 0.0, convert_total = 0.0;
    int consecutive_drops = 0, shown_this_sec = 0, fps_now = 0;
    char stat_line[160] = "";
    int failed = 0;

    for (;;) {
        QFrame qf = { 0 };
        int have, done;

        pthread_mutex_lock(&P.lock);
        have = P.fq_n > 0;
        if (have)
            qf = P.fq[P.fq_head];
        done = P.vdec_eof && P.fq_n == 0;
        pthread_mutex_unlock(&P.lock);

        if (!have) {
            if (done)
                break;
            plat_sleep_us(2000);
            continue;
        }

        if (isnan(first_pts)) {
            first_pts = qf.pts;
            wall_start = plat_time_us();
        }

        double clock = master_clock(wall_start, first_pts);
        if (isnan(clock)) {                         /* audio has not started yet */
            plat_sleep_us(2000);
            continue;
        }
        if (max_seconds > 0 && clock - first_pts > max_seconds)
            break;

        double diff = qf.pts - clock;               /* > 0: frame is early */
        if (diff > 0.008) {
            double wait = diff - 0.004;
            plat_sleep_us(wait > 0.010 ? 10000 : (unsigned)(wait * 1e6));
            continue;
        }

        /* Take the frame out of the queue. */
        pthread_mutex_lock(&P.lock);
        P.fq_head = (P.fq_head + 1) % FRAMEQ_CAP;
        P.fq_n--;
        int more = P.fq_n > 0;
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);

        /* Far behind: skip showing it so the picture can catch up. */
        if (diff < -LATE_DROP && more && consecutive_drops < 10) {
            consecutive_drops++;
            st.frames_dropped++;
            av_frame_free(&qf.frame);
            continue;
        }
        consecutive_drops = 0;

        AVFrame *f = qf.frame;
        if (!scaler.sws || f->width != scaler.src_w || f->height != scaler.src_h ||
            f->format != scaler.src_fmt) {
            if (scaler_setup(&scaler, f, vst->sample_aspect_ratio) < 0) {
                av_frame_free(&qf.frame);
                failed = 1;
                break;
            }
            osd_log("picture: %dx%d %s -> %dx%d on a %dx%d screen", f->width, f->height,
                    av_get_pix_fmt_name(f->format), scaler.dst_w, scaler.dst_h,
                    plat_width(), plat_height());
        }

        uint32_t *fb = plat_backbuffer();
        int W = plat_width(), H = plat_height();
        if (scaler.clear_pending > 0) {
            for (size_t i = 0; i < (size_t)W * H; i++)
                fb[i] = 0xFF000000u;
            scaler.clear_pending--;
        }

        uint64_t t0 = plat_time_us();
        uint8_t *dst[4] = { (uint8_t *)(fb + (size_t)scaler.dst_y * W + scaler.dst_x), NULL, NULL, NULL };
        int dst_stride[4] = { W * 4, 0, 0, 0 };
        sws_scale(scaler.sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
        double conv_ms = (double)(plat_time_us() - t0) / 1000.0;
        convert_total += conv_ms;
        av_frame_free(&qf.frame);

        st.frames_shown++;
        shown_this_sec++;
        last_diff = diff;
        if (st.frames_shown > 48 && fabs(diff) * 1000.0 > st.max_abs_av_diff_ms)
            st.max_abs_av_diff_ms = fabs(diff) * 1000.0;

        /* Diagnostics line, refreshed once a second. */
        uint64_t now = plat_time_us();
        if (now - last_stat >= 1000000) {
            fps_now = shown_this_sec;
            shown_this_sec = 0;
            last_stat = now;
            int secs = (int)(clock - first_pts);
            snprintf(stat_line, sizeof(stat_line),
                     " %02d:%02d:%02d  %d fps  dropped %d  convert %.1f ms  a/v %+.0f ms ",
                     secs / 3600, secs / 60 % 60, secs % 60, fps_now, st.frames_dropped,
                     conv_ms, last_diff * 1000.0);
        }
        osd_text(fb, W, H, 24, 16, stat_line, 0xFFFFFFFFu, 0xFF000000u);

        plat_flip();
        st.seconds_played = clock - first_pts;
    }

    /* --- shutdown --- */
    pthread_mutex_lock(&P.lock);
    P.quit = 1;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);

    pthread_join(demux_t, NULL);
    pthread_join(video_t, NULL);
    if (P.aidx >= 0)
        pthread_join(audio_t, NULL);

    while (P.fq_n > 0) {
        av_frame_free(&P.fq[P.fq_head].frame);
        P.fq_head = (P.fq_head + 1) % FRAMEQ_CAP;
        P.fq_n--;
    }
    pktq_clear(&P.vq);
    pktq_clear(&P.aq);
    sws_freeContext(scaler.sws);
    avcodec_free_context(&P.vdec);
    avcodec_free_context(&P.adec);
    avformat_close_input(&P.fmt);
    pthread_mutex_destroy(&P.lock);
    pthread_cond_destroy(&P.cond);

    if (st.frames_shown > 0)
        st.avg_convert_ms = convert_total / st.frames_shown;
    if (stats)
        *stats = st;
    return failed ? -1 : 0;
}
