/* The player core. Four threads:
 *
 *   demux   reads packets from the file and sorts them into two queues
 *   video   decodes video packets into a small queue of finished frames
 *   audio   decodes audio, converts to 48 kHz stereo and feeds the device
 *   main    shows each frame when the audio clock reaches its timestamp
 *
 * Audio is the master clock: sound plays at the device's own pace and the
 * picture follows it, so the two cannot drift apart.
 *
 * Seeking: the demux thread repositions the file, empties the packet queues
 * and increases P.serial. Everything in flight carries the serial it was
 * made under; a decoder that sees a new serial flushes itself, and frames or
 * sound with an old serial are thrown away. */
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
#include "smb.h"
#include "subfont.h"

#define PKTQ_CAP        2048
#define PKTQ_ENOUGH     48                  /* packets per stream considered "full" */
#define PKTQ_MAX_BYTES  (48 * 1024 * 1024)
#define FRAMEQ_CAP      6
#define VIDEO_THREADS   4
#define VIDEO_THREADS_HD 6                  /* for sources above 720p */
#define AUDIO_LATENCY   0.040               /* device + mixer delay, seconds (estimate) */
#define LATE_DROP       0.100               /* drop frames more than this late */
#define NET_BUFFER      (256 * 1024)        /* read-ahead for files on a share */
#define CUE_CAP         32                  /* subtitle lines kept ready */
#define MAX_TRACKS      32

typedef struct {
    AVPacket *pkt[PKTQ_CAP];
    int       head, n;
    int64_t   bytes;
} PktQ;

typedef struct {
    AVFrame *frame;
    double   pts;
    int      serial;
} QFrame;

typedef struct {
    double start, end;                  /* stream time */
    char   text[400];
} Cue;

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
    int              paused;

    /* Track changes are asked for through the *_want fields and carried out
     * by the demux thread: subtitles at once, audio together with a seek. */
    int              aidx_want;
    int              sidx, sidx_want;   /* subtitle stream, -1 = off */
    AVCodecContext  *sdec;              /* used by the demux thread only */
    Cue              cues[CUE_CAP];
    int              cue_next;          /* ring position for the next cue */

    int              serial;            /* increased by every completed seek */
    int              seek_req;
    double           seek_target;       /* seconds, in stream time */
    double           seek_from;         /* position the seek started from */

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

/* Returns 1 with a packet, 0 at end of stream, -1 when asked to quit.
 * *serial tells which seek generation the answer belongs to. */
static int pktq_pop(PktQ *q, AVPacket **out, int *serial)
{
    int rc;

    pthread_mutex_lock(&P.lock);
    while (!P.quit && q->n == 0 && !P.demux_eof)
        pthread_cond_wait(&P.cond, &P.lock);

    *serial = P.serial;
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
    q->bytes = 0;
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

/* --- subtitles (decoded by the demux thread: it is hardly any work) ------- */

static AVCodecContext *open_decoder(AVStream *st, int threads);

/* Turns one ASS event ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,
 * Effect,Text") into plain text: drops {\tags}, \N becomes a line break. */
static void ass_to_text(const char *ass, char *out, size_t out_size)
{
    size_t n = 0;

    for (int commas = 0; commas < 8 && *ass; ass++)
        if (*ass == ',')
            commas++;

    while (*ass && n + 1 < out_size) {
        if (*ass == '{') {
            const char *close = strchr(ass, '}');
            if (close) {
                ass = close + 1;
                continue;
            }
        }
        if (ass[0] == '\\' && (ass[1] == 'N' || ass[1] == 'n')) {
            out[n++] = '\n';
            ass += 2;
        } else if (ass[0] == '\\' && ass[1] == 'h') {
            out[n++] = ' ';
            ass += 2;
        } else {
            out[n++] = *ass++;
        }
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        n--;
    out[n] = '\0';
}

static void decode_subtitle(AVPacket *pkt)
{
    AVSubtitle sub;
    int got = 0;

    if (!P.sdec || avcodec_decode_subtitle2(P.sdec, &sub, &got, pkt) < 0 || !got)
        return;

    double tb = av_q2d(P.fmt->streams[pkt->stream_index]->time_base);
    double start = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * tb : 0.0;
    double length = pkt->duration > 0 ? pkt->duration * tb
                  : sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX
                  ? sub.end_display_time / 1000.0 : 4.0;
    start += sub.start_display_time / 1000.0;

    for (unsigned i = 0; i < sub.num_rects; i++) {
        Cue cue = { .start = start, .end = start + length };
        const AVSubtitleRect *r = sub.rects[i];

        if (r->type == SUBTITLE_ASS && r->ass)
            ass_to_text(r->ass, cue.text, sizeof(cue.text));
        else if (r->type == SUBTITLE_TEXT && r->text)
            snprintf(cue.text, sizeof(cue.text), "%s", r->text);
        if (!cue.text[0])
            continue;

        pthread_mutex_lock(&P.lock);
        P.cues[P.cue_next] = cue;
        P.cue_next = (P.cue_next + 1) % CUE_CAP;
        pthread_mutex_unlock(&P.lock);
    }
    avsubtitle_free(&sub);
}

/* Switches to subtitle stream `want` (-1 = none). Demux thread only. */
static void switch_subtitles(int want)
{
    if (P.sidx >= 0)
        P.fmt->streams[P.sidx]->discard = AVDISCARD_ALL;
    avcodec_free_context(&P.sdec);
    if (want >= 0) {
        P.sdec = open_decoder(P.fmt->streams[want], 1);
        if (P.sdec)
            P.fmt->streams[want]->discard = AVDISCARD_DEFAULT;
        else
            want = -1;
    }

    pthread_mutex_lock(&P.lock);
    P.sidx = P.sidx_want = want;
    memset(P.cues, 0, sizeof(P.cues));
    pthread_mutex_unlock(&P.lock);
}

/* --- demux thread ------------------------------------------------------- */

static void *demux_thread(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (!P.quit && !P.seek_req && P.sidx_want == P.sidx && (P.demux_eof || demux_must_wait()))
            pthread_cond_wait(&P.cond, &P.lock);
        int quit = P.quit, seek = P.seek_req;
        int sub_want = P.sidx_want, audio_want = P.aidx_want;
        double target = P.seek_target, from = P.seek_from;
        pthread_mutex_unlock(&P.lock);
        if (quit)
            break;

        if (sub_want != P.sidx) {
            switch_subtitles(sub_want);
            continue;
        }

        if (seek) {
            /* Land on a key frame on the far side of where we came from,
             * so that every press moves the picture. */
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            int64_t rel = (int64_t)((target - from) * AV_TIME_BASE);
            int64_t lo = rel > 0 ? ts - rel + 2 : INT64_MIN;
            int64_t hi = rel < 0 ? ts - rel - 2 : INT64_MAX;
            int rc = avformat_seek_file(P.fmt, -1, lo, ts, hi, 0);
            if (rc < 0)
                osd_log("seek to %.1f s failed: %s", target, av_err2str(rc));
            else
                osd_log("seek %+.0f s to %.1f s", target - from, target);

            if (rc >= 0 && audio_want != P.aidx && audio_want >= 0 && P.aidx >= 0) {  /* the other sound track from here on */
                P.fmt->streams[P.aidx]->discard = AVDISCARD_ALL;
                P.fmt->streams[audio_want]->discard = AVDISCARD_DEFAULT;
            }

            pthread_mutex_lock(&P.lock);
            if (rc >= 0) {
                pktq_clear(&P.vq);
                pktq_clear(&P.aq);
                memset(P.cues, 0, sizeof(P.cues));
                if (audio_want >= 0 && P.aidx >= 0)
                    P.aidx = audio_want;
                P.serial++;
                P.demux_eof = 0;
                P.vdec_eof = 0;                     /* the decoder is about to get new work */
                P.aclock_valid = 0;
            }
            if (P.seek_target == target)            /* otherwise a newer request is waiting */
                P.seek_req = 0;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            continue;
        }

        AVPacket *pkt = av_packet_alloc();
        int rc = av_read_frame(P.fmt, pkt);
        if (rc < 0) {
            if (rc != AVERROR_EOF)
                osd_log("read error: %s", av_err2str(rc));
            av_packet_free(&pkt);
            pthread_mutex_lock(&P.lock);            /* stay around: a seek may follow */
            P.demux_eof = 1;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            continue;
        }

        if (pkt->stream_index == P.sidx)
            decode_subtitle(pkt);

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
    avcodec_free_context(&P.sdec);
    return NULL;
}

/* A decoder thread has played out everything: reports it through *eof_flag
 * and sleeps until a seek gives it new work. Returns 0 to carry on, -1 to quit. */
static int wait_for_seek(int *eof_flag, int my_serial)
{
    pthread_mutex_lock(&P.lock);
    *eof_flag = 1;
    pthread_cond_broadcast(&P.cond);
    while (!P.quit && P.serial == my_serial)
        pthread_cond_wait(&P.cond, &P.lock);
    int quit = P.quit;
    if (!quit)
        *eof_flag = 0;
    pthread_mutex_unlock(&P.lock);
    return quit ? -1 : 0;
}

/* --- video decode thread ------------------------------------------------ */

static void *video_thread(void *arg)
{
    AVStream *st = P.fmt->streams[P.vidx];
    double tb = av_q2d(st->time_base);
    double frame_dur = st->avg_frame_rate.num > 0 ? 1.0 / av_q2d(st->avg_frame_rate) : 1.0 / 25.0;
    double next_pts = 0.0;
    AVFrame *frame = av_frame_alloc();
    int stop = 0, my_serial = 0;
    (void)arg;

    while (!stop) {
        AVPacket *pkt = NULL;
        int serial;
        int got = pktq_pop(&P.vq, &pkt, &serial);
        if (got < 0)
            break;
        if (serial != my_serial) {                  /* a seek happened: start afresh */
            avcodec_flush_buffers(P.vdec);
            my_serial = serial;
        }

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
            slot->serial = my_serial;
            P.fq_n++;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
        }

        if (!got && !stop && wait_for_seek(&P.vdec_eof, my_serial) < 0)
            break;                                  /* drained, then asked to quit */
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
    int my_idx = P.aidx;
    double tb = av_q2d(P.fmt->streams[my_idx]->time_base);
    AVFrame *frame = av_frame_alloc();
    SwrContext *swr = NULL;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout = { 0 };
    int in_rate = 0, in_fmt = -1;

    int16_t *buf = NULL;                 /* converted audio waiting to be played */
    int cap = 0, rd = 0, buffered = 0;   /* in stereo frames */
    double buf_pts = NAN;                /* timestamp of buf[rd] */
    int stop = 0, my_serial = 0;
    (void)arg;

    while (!stop) {
        AVPacket *pkt = NULL;
        int serial, stale = 0;
        int got = pktq_pop(&P.aq, &pkt, &serial);
        if (got < 0)
            break;
        if (serial != my_serial) {                  /* a seek happened: forget what was buffered */
            pthread_mutex_lock(&P.lock);
            int idx = P.aidx;
            pthread_mutex_unlock(&P.lock);
            if (idx != my_idx) {                    /* ...and it brought another sound track */
                AVStream *st = P.fmt->streams[idx];
                AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
                avcodec_free_context(&P.adec);
                P.adec = open_decoder(st, 1);
                my_idx = idx;
                tb = av_q2d(st->time_base);
                if (!P.adec) {
                    osd_log("cannot play this sound track - sound disabled");
                    av_packet_free(&pkt);
                    break;
                }
                osd_log("audio: %s %d Hz %d ch [%s]", avcodec_get_name(st->codecpar->codec_id),
                        st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels,
                        lang ? lang->value : "?");
            } else {
                avcodec_flush_buffers(P.adec);
            }
            swr_free(&swr);
            my_serial = serial;
            rd = buffered = 0;
            buf_pts = NAN;
        }

        int rc = avcodec_send_packet(P.adec, got ? pkt : NULL);
        av_packet_free(&pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && rc != AVERROR_INVALIDDATA)
            osd_log("audio decode error: %s", av_err2str(rc));

        while (!stop && !stale && avcodec_receive_frame(P.adec, frame) == 0) {
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
                pthread_mutex_lock(&P.lock);
                while (P.paused && !P.quit && P.serial == my_serial)
                    pthread_cond_wait(&P.cond, &P.lock);
                stop = P.quit;
                stale = P.serial != my_serial;      /* a seek overtook this sound */
                pthread_mutex_unlock(&P.lock);
                if (stop || stale)
                    break;

                plat_audio_write(buf + rd * 2);
                rd += AUDIO_GRAIN;
                buffered -= AUDIO_GRAIN;

                pthread_mutex_lock(&P.lock);
                if (!isnan(buf_pts)) {
                    buf_pts += (double)AUDIO_GRAIN / AUDIO_RATE;
                    if (P.serial == my_serial) {
                        P.aclock_pts = buf_pts;
                        P.aclock_time = plat_time_us();
                        P.aclock_valid = 1;
                    }
                }
                stop = P.quit;
                pthread_mutex_unlock(&P.lock);
                if (stop)
                    break;
            }
        }

        if (!got && !stop && !stale && wait_for_seek(&P.adec_eof, my_serial) < 0)
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

/* Current playback position in stream time, or NAN if not known yet.
 * wall_base/wall_start describe the fallback clock used without sound. */
static double master_clock(uint64_t wall_start, double wall_base)
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
            clock = wall_base + (double)(now - wall_start) / 1e6;   /* no sound (left) */
        }
        pthread_mutex_unlock(&P.lock);
    } else {
        clock = wall_base + (double)(now - wall_start) / 1e6;
    }
    return clock;
}

static void format_time(char *out, size_t out_size, double seconds)
{
    int s = seconds > 0 ? (int)seconds : 0;
    snprintf(out, out_size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
}

/* Position bar near the bottom of the picture. duration <= 0: unknown. */
static void draw_progress(uint32_t *fb, int W, int H, double position, double duration)
{
    int fh = osd_font_height(), fw = osd_font_width();
    int x = W / 20, w = W - 2 * x;
    int box_h = fh + 34, y = H - H / 20 - box_h;
    const uint32_t bg = 0xFF101428u;
    char a[32], b[32], text[80];

    format_time(a, sizeof(a), position);
    if (duration > 0) {
        format_time(b, sizeof(b), duration);
        snprintf(text, sizeof(text), "%s / %s", a, b);
    } else {
        snprintf(text, sizeof(text), "%s", a);
    }

    osd_fill(fb, W, H, x, y, w, box_h, bg);
    osd_text(fb, W, H, x + 2 * fw, y + 8, text, 0xFFFFFFFFu, bg);
    if (duration > 0) {
        int bar_w = w - 4 * fw;
        double part = position / duration;
        if (part < 0) part = 0;
        if (part > 1) part = 1;
        osd_fill(fb, W, H, x + 2 * fw, y + fh + 16, bar_w, 8, 0xFF404860u);
        osd_fill(fb, W, H, x + 2 * fw, y + fh + 16, (int)(bar_w * part), 8, 0xFF4F8FFFu);
    }
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

/* Describes a track for the on-screen note, e.g. "hun - Hun Full (subrip)". */
static void track_label(int stream, char *out, size_t out_size)
{
    AVStream *st = P.fmt->streams[stream];
    AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
    AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
    char detail[64];

    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        snprintf(detail, sizeof(detail), "%s, %d ch", avcodec_get_name(st->codecpar->codec_id),
                 st->codecpar->ch_layout.nb_channels);
    else
        snprintf(detail, sizeof(detail), "%s", avcodec_get_name(st->codecpar->codec_id));
    snprintf(out, out_size, "%s%s%s (%s)", lang ? lang->value : "", lang && title ? " - " : "",
             title ? title->value : lang ? "" : "unnamed", detail);
}

/* Text subtitles only: pictures (Blu-ray, DVD) cannot be drawn yet. */
static int is_text_subtitle(const AVStream *st)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
    return d && (d->props & AV_CODEC_PROP_TEXT_SUB) && avcodec_find_decoder(st->codecpar->codec_id);
}

/* --- files on a network share -------------------------------------------- */

static int net_read(void *opaque, uint8_t *buf, int size)
{
    int n = smb_read(opaque, buf, size);
    if (n == 0)
        return AVERROR_EOF;
    return n < 0 ? AVERROR(EIO) : n;
}

static int64_t net_seek(void *opaque, int64_t offset, int whence)
{
    if (whence & AVSEEK_SIZE)
        return smb_size(opaque);
    int64_t pos = smb_seek(opaque, offset, whence & ~AVSEEK_FORCE);
    return pos < 0 ? AVERROR(EIO) : pos;
}

/* Opens path (a local file or an smb:// address) for demuxing. */
static int open_input(const char *path, SmbFile **net)
{
    *net = NULL;
    if (!smb_is_url(path))
        return avformat_open_input(&P.fmt, path, NULL, NULL);

    char err[200];
    SmbFile *f = smb_open(path, err, sizeof(err));
    if (!f) {
        osd_log("%s", err);
        return AVERROR(EIO);
    }

    uint8_t *buffer = av_malloc(NET_BUFFER);
    AVIOContext *io = avio_alloc_context(buffer, NET_BUFFER, 0, f, net_read, NULL, net_seek);
    P.fmt = avformat_alloc_context();
    P.fmt->pb = io;

    char shown[1024];
    smb_display_name(path, shown, sizeof(shown));      /* the name helps format detection */
    int rc = avformat_open_input(&P.fmt, shown, NULL, NULL);
    if (rc < 0) {                                       /* P.fmt was freed, the I/O part is ours */
        av_freep(&io->buffer);
        avio_context_free(&io);
        smb_close(f);
        return rc;
    }
    *net = f;
    return 0;
}

static void close_input(SmbFile *net)
{
    AVIOContext *io = net ? P.fmt->pb : NULL;

    avformat_close_input(&P.fmt);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    smb_close(net);
}

/* --- main entry --------------------------------------------------------- */

int player_run(const char *path, double max_seconds, PlayerStats *stats)
{
    pthread_t demux_t, video_t, audio_t;
    Scaler scaler = { 0 };
    PlayerStats st = { 0 };
    SmbFile *net = NULL;
    int rc;

    memset(&P, 0, sizeof(P));
    pthread_mutex_init(&P.lock, NULL);
    pthread_cond_init(&P.cond, NULL);
    av_log_set_callback(log_to_screen);

    if (stats)
        *stats = st;

    rc = open_input(path, &net);
    if (rc < 0) {
        osd_log("cannot open file: %s", av_err2str(rc));
        return -1;
    }
    rc = avformat_find_stream_info(P.fmt, NULL);
    if (rc < 0) {
        osd_log("cannot read stream info: %s", av_err2str(rc));
        close_input(net);
        return -1;
    }

    P.vidx = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (P.vidx >= 0 && (P.fmt->streams[P.vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        P.vidx = -1;                                /* only cover art */
    P.aidx = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vidx, NULL, 0);

    if (P.vidx < 0) {
        osd_log("no video stream in this file");
        close_input(net);
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
        close_input(net);
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

    /* The tracks the user can switch between. Subtitles start switched off. */
    int audio_tracks[MAX_TRACKS], sub_tracks[MAX_TRACKS], n_audio = 0, n_subs = 0;
    for (unsigned i = 0; i < P.fmt->nb_streams; i++) {
        AVStream *t = P.fmt->streams[i];
        if (t->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && n_audio < MAX_TRACKS &&
            avcodec_find_decoder(t->codecpar->codec_id))
            audio_tracks[n_audio++] = (int)i;
        if (t->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && n_subs < MAX_TRACKS && is_text_subtitle(t))
            sub_tracks[n_subs++] = (int)i;
    }
    osd_log("%d audio track(s), %d subtitle track(s)", n_audio, n_subs);
    P.aidx_want = P.aidx;
    P.sidx = P.sidx_want = -1;

    pthread_create(&demux_t, NULL, demux_thread, NULL);
    pthread_create(&video_t, NULL, video_thread, NULL);
    if (P.aidx >= 0)
        pthread_create(&audio_t, NULL, audio_thread, NULL);

    /* --- display loop --- */
    static int show_stats;                          /* triangle toggles; kept between files */
    uint64_t wall_start = 0, last_stat = 0, pause_start = 0, bar_until = 0;
    double wall_base = NAN, last_diff = 0.0, convert_total = 0.0;
    double origin = P.fmt->start_time != AV_NOPTS_VALUE ? (double)P.fmt->start_time / AV_TIME_BASE : NAN;
    double duration = P.fmt->duration > 0 ? (double)P.fmt->duration / AV_TIME_BASE : 0.0;
    double pos = NAN;                               /* last known position, stream time */
    double seek_goal = 0.0;
    int seeking = 0, shown_serial = -1;
    int consecutive_drops = 0, shown_this_sec = 0, fps_now = 0;
    char stat_line[160] = "";
    int failed = 0, paused = 0;
    unsigned pad_prev = plat_pad();                 /* ignore buttons still held from the menu */
    char note[200] = "", sub_text[900] = "";
    uint64_t note_until = 0;
    unsigned overlay_sig = 0;                       /* what was drawn over the last picture */

    for (;;) {
        QFrame qf = { 0 };
        int have, done, serial_now, seek_pending;
        uint64_t now = plat_time_us();

        /* Controller: circle stops, cross pauses and continues, the d-pad
         * and L1/R1 jump, triangle shows the diagnostics line. */
        unsigned pad = plat_pad();
        unsigned pressed = pad & ~pad_prev;
        double step = 0.0;
        pad_prev = pad;

        if (pressed & PAD_CIRCLE)
            break;
        if (pressed & PAD_TRIANGLE)
            show_stats = !show_stats;

        /* Square: next sound track. It takes effect through a seek to where we are. */
        int audio_switch = -1;
        if ((pressed & PAD_SQUARE) && !isnan(pos)) {
            char label[120];
            int cur = 0;
            pthread_mutex_lock(&P.lock);
            for (int i = 0; i < n_audio; i++)
                if (audio_tracks[i] == P.aidx_want)
                    cur = i;
            pthread_mutex_unlock(&P.lock);
            if (n_audio > 1 && P.aidx >= 0) {
                cur = (cur + 1) % n_audio;
                audio_switch = audio_tracks[cur];
                track_label(audio_switch, label, sizeof(label));
                snprintf(note, sizeof(note), " Audio %d/%d: %s ", cur + 1, n_audio, label);
            } else {
                snprintf(note, sizeof(note), " No other audio track ");
            }
            note_until = now + 3000000;
        }

        /* OPTIONS: next subtitle track, then off. */
        if (pressed & PAD_OPTIONS) {
            char label[120];
            int cur = -1;
            pthread_mutex_lock(&P.lock);
            for (int i = 0; i < n_subs; i++)
                if (sub_tracks[i] == P.sidx_want)
                    cur = i;
            cur = cur + 1 < n_subs ? cur + 1 : -1;
            P.sidx_want = cur >= 0 ? sub_tracks[cur] : -1;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            if (cur >= 0) {
                track_label(sub_tracks[cur], label, sizeof(label));
                snprintf(note, sizeof(note), " Subtitles %d/%d: %s ", cur + 1, n_subs, label);
            } else {
                snprintf(note, sizeof(note), n_subs ? " Subtitles off " : " No subtitles in this file ");
            }
            note_until = now + 3000000;
        }
        if (pressed & PAD_RIGHT) step += 10.0;
        if (pressed & PAD_LEFT)  step -= 10.0;
        if (pressed & PAD_UP)    step += 60.0;
        if (pressed & PAD_DOWN)  step -= 60.0;
        if (pressed & PAD_R1)    step += 600.0;
        if (pressed & PAD_L1)    step -= 600.0;

        int toggle_pause = (pressed & PAD_CROSS) != 0;
        if ((step != 0.0 || audio_switch >= 0) && !isnan(pos)) {
            double base = seeking ? seek_goal : pos;
            double target = base + step;
            if (duration > 0 && target > origin + duration - 5.0)
                target = origin + duration - 5.0;
            if (target < origin)
                target = origin;

            if (fabs(target - base) > 0.5 || audio_switch >= 0) {   /* not already at that end of the file */
                pthread_mutex_lock(&P.lock);
                P.seek_target = target;
                P.seek_from = base;
                P.seek_req = 1;
                if (audio_switch >= 0)
                    P.aidx_want = audio_switch;
                pthread_cond_broadcast(&P.cond);
                pthread_mutex_unlock(&P.lock);
                seeking = 1;
                seek_goal = target;
            }
            if (step != 0.0)
                bar_until = now + 3000000;
            if (paused)
                toggle_pause = 1;                   /* jumping continues playback */
        }
        if (toggle_pause) {
            paused = !paused;
            pthread_mutex_lock(&P.lock);
            P.paused = paused;
            if (!paused)
                P.aclock_time = now;                /* the audio clock stood still meanwhile */
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            if (paused)
                pause_start = now;
            else
                wall_start += now - pause_start;
        }
        if (paused) {
            plat_sleep_us(20000);
            continue;
        }

        pthread_mutex_lock(&P.lock);
        have = P.fq_n > 0;
        if (have)
            qf = P.fq[P.fq_head];
        serial_now = P.serial;
        seek_pending = P.seek_req;
        done = P.vdec_eof && P.fq_n == 0 && !P.seek_req;
        pthread_mutex_unlock(&P.lock);

        if (!have) {
            if (done)
                break;
            plat_sleep_us(2000);
            continue;
        }

        int stale = qf.serial != serial_now;        /* decoded before the last seek */
        if (!stale && qf.serial != shown_serial) {  /* first picture, or first after a seek */
            shown_serial = qf.serial;
            wall_base = qf.pts;
            wall_start = now;
            consecutive_drops = 0;
            if (isnan(origin))
                origin = qf.pts;
            if (isnan(pos))
                pos = qf.pts;
            if (!seek_pending)
                seeking = 0;
        }

        double clock = NAN, diff = 0.0;
        if (!stale) {
            clock = master_clock(wall_start, wall_base);
            if (isnan(clock)) {                     /* audio has not (re)started yet */
                plat_sleep_us(2000);
                continue;
            }
            pos = clock;
            if (max_seconds > 0 && clock - origin > max_seconds)
                break;

            diff = qf.pts - clock;                  /* > 0: frame is early */
            if (diff > 0.008) {
                double wait = diff - 0.004;
                plat_sleep_us(wait > 0.010 ? 10000 : (unsigned)(wait * 1e6));
                continue;
            }
        }

        /* Take the frame out of the queue. */
        pthread_mutex_lock(&P.lock);
        P.fq_head = (P.fq_head + 1) % FRAMEQ_CAP;
        P.fq_n--;
        int more = P.fq_n > 0;
        pthread_cond_broadcast(&P.cond);
        pthread_mutex_unlock(&P.lock);

        if (stale) {
            av_frame_free(&qf.frame);
            continue;
        }

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

        /* Subtitle lines that belong to this moment. */
        sub_text[0] = '\0';
        pthread_mutex_lock(&P.lock);
        for (int k = 0; k < CUE_CAP; k++) {
            const Cue *cue = &P.cues[(P.cue_next + k) % CUE_CAP];       /* oldest first */
            if (cue->text[0] && clock >= cue->start && clock < cue->end &&
                strlen(sub_text) + strlen(cue->text) + 2 < sizeof(sub_text)) {
                if (sub_text[0])
                    strcat(sub_text, "\n");
                strcat(sub_text, cue->text);
            }
        }
        pthread_mutex_unlock(&P.lock);

        /* Things drawn over the picture may reach into the black borders,
         * where no new picture would cover them: wipe when they change. */
        int bar_on = now < bar_until, note_on = now < note_until;
        unsigned sig = (unsigned)bar_on | (unsigned)note_on << 1 | (unsigned)show_stats << 2;
        for (const char *c = note_on ? note : ""; *c; c++)
            sig = sig * 31 + (unsigned char)*c;
        for (const char *c = sub_text; *c; c++)
            sig = sig * 31 + (unsigned char)*c;
        if (sig != overlay_sig) {
            overlay_sig = sig;
            scaler.clear_pending = 2;
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
        if (now - last_stat >= 1000000) {
            fps_now = shown_this_sec;
            shown_this_sec = 0;
            last_stat = now;
            int secs = (int)(clock - origin);
            snprintf(stat_line, sizeof(stat_line),
                     " %02d:%02d:%02d  %d fps  dropped %d  convert %.1f ms  a/v %+.0f ms ",
                     secs / 3600, secs / 60 % 60, secs % 60, fps_now, st.frames_dropped,
                     conv_ms, last_diff * 1000.0);
        }
        if (show_stats)
            osd_text(fb, W, H, W / 20, H / 20, stat_line, 0xFFFFFFFFu, 0xFF000000u);
        if (note_on)
            osd_text(fb, W, H, W / 20, H / 20 + (show_stats ? osd_font_height() + 6 : 0), note,
                     0xFFFFFFFFu, 0xFF101428u);
        if (bar_on)
            draw_progress(fb, W, H, (seeking ? seek_goal : clock) - origin, duration);
        if (sub_text[0])
            subfont_draw(fb, W, H, sub_text,
                         H - H / 20 - (bar_on ? osd_font_height() + 34 + 12 : 0));

        plat_flip();
        st.seconds_played = clock - origin;
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
    close_input(net);
    pthread_mutex_destroy(&P.lock);
    pthread_cond_destroy(&P.cond);

    if (st.frames_shown > 0)
        st.avg_convert_ms = convert_total / st.frames_shown;
    if (stats)
        *stats = st;
    return failed ? -1 : 0;
}
