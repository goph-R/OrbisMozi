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
#include "extsub.h"
#include "menu.h"
#include "subfont.h"
#include "ui.h"

#define PKTQ_CAP        2048
#define PKTQ_ENOUGH     48                  /* packets per stream considered "full" */
#define PKTQ_MAX_BYTES  (48 * 1024 * 1024)
#define FRAMEQ_CAP      6
#define VIDEO_THREADS   4
#define VIDEO_THREADS_HD 6                  /* for sources above 720p */
#define AUDIO_LATENCY   0.040               /* device + mixer delay, seconds (estimate) */
#define LATE_DROP       0.100               /* drop frames more than this late */
#define NET_BUFFER      (256 * 1024)        /* read-ahead for files on a share */
#define CUE_CAP         256                 /* subtitle lines kept ready, all tracks together */
#define MAX_TRACKS      32
#define RESUME_MARGIN   15.0                /* seconds: no continuing this close to either end */

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
    int    stream;                      /* the subtitle track it belongs to */
    char   text[400];
} Cue;

static int g_volume = 100;              /* percent, 0..200; kept between files */

/* How the picture is put on the screen; kept between files. */
enum { PICTURE_FIT, PICTURE_FILL, PICTURE_STRETCH };
static int g_picture = PICTURE_FIT;

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

    /* A change of sound track is asked for through aidx_want and carried
     * out by the demux thread together with a seek. */
    int              aidx_want;

    /* Every text subtitle track is decoded all the time (it costs next to
     * nothing), so switching between them is instant and loses no line. */
    AVCodecContext  *sdec[MAX_TRACKS];  /* used by the demux thread only */
    int              sdec_stream[MAX_TRACKS], n_sdec;
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

/* Decodes the packet if it belongs to a subtitle track. */
static void decode_subtitle(AVPacket *pkt)
{
    AVCodecContext *dec = NULL;
    AVSubtitle sub;
    int got = 0;

    for (int k = 0; k < P.n_sdec; k++)
        if (P.sdec_stream[k] == pkt->stream_index)
            dec = P.sdec[k];
    if (!dec || avcodec_decode_subtitle2(dec, &sub, &got, pkt) < 0 || !got)
        return;

    double tb = av_q2d(P.fmt->streams[pkt->stream_index]->time_base);
    double start = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * tb : 0.0;
    double length = pkt->duration > 0 ? pkt->duration * tb
                  : sub.end_display_time > 0 && sub.end_display_time != UINT32_MAX
                  ? sub.end_display_time / 1000.0 : 4.0;
    start += sub.start_display_time / 1000.0;

    for (unsigned i = 0; i < sub.num_rects; i++) {
        Cue cue = { .start = start, .end = start + length, .stream = pkt->stream_index };
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

/* --- demux thread ------------------------------------------------------- */

static void *demux_thread(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (!P.quit && !P.seek_req && (P.demux_eof || demux_must_wait()))
            pthread_cond_wait(&P.cond, &P.lock);
        int quit = P.quit, seek = P.seek_req;
        int audio_want = P.aidx_want;
        double target = P.seek_target, from = P.seek_from;
        pthread_mutex_unlock(&P.lock);
        if (quit)
            break;


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

        if (pkt->stream_index != P.vidx && pkt->stream_index != P.aidx)
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

                int volume = g_volume;
                if (volume == 100) {
                    plat_audio_write(buf + rd * 2);
                } else {                            /* scale a copy, limiting instead of wrapping */
                    int16_t scaled[AUDIO_GRAIN * 2];
                    for (int i = 0; i < AUDIO_GRAIN * 2; i++) {
                        int v = buf[rd * 2 + i] * volume / 100;
                        scaled[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                    plat_audio_write(scaled);
                }
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
    int mode;                           /* the PICTURE_* it was set up for */
    int crop_x, crop_y, crop_w, crop_h; /* the part of the source that is shown */
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

    /* Fit shows the whole picture with black borders; stretch fills the
     * screen and bends the picture; fill keeps the shape and cuts off what
     * does not fit, the same amount on both sides. */
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
    int cx = 0, cy = 0, cw = f->width, ch = f->height;
    s->mode = g_picture;
    if (s->mode != PICTURE_FIT) {
        dw = W;
        dh = H;
    }
    if (s->mode == PICTURE_FILL && desc && !(desc->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL))) {
        double screen = (double)W / H;
        int ax = 1 << desc->log2_chroma_w, ay = 1 << desc->log2_chroma_h;
        if (aspect > screen)
            cw = (int)lrint(f->width * screen / aspect);
        else
            ch = (int)lrint(f->height * aspect / screen);
        cw = cw / (2 * ax) * (2 * ax);              /* whole colour samples, and an even size */
        ch = ch / (2 * ay) * (2 * ay);
        if (cw < 16 || ch < 16) {
            cw = f->width;
            ch = f->height;
        }
        cx = (f->width - cw) / 2 / ax * ax;
        cy = (f->height - ch) / 2 / ay * ay;
    }
    s->crop_x = cx;
    s->crop_y = cy;
    s->crop_w = cw;
    s->crop_h = ch;

    sws_freeContext(s->sws);
    s->sws = sws_getContext(cw, ch, f->format, dw, dh, AV_PIX_FMT_BGRA,
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

/* --- the overlay: what any button press brings up over the picture ---------- */

static int overlay_height(int H) { return H * 120 / 720; }

/* A strip at the bottom that fades in from nothing, with the subtitle and
 * sound track in use and the position in the film. duration <= 0: unknown. */
static void draw_overlay(uint32_t *fb, int W, int H, double position, double duration,
                         const char *sub_name, const char *audio_name)
{
    int font = H >= 1000 ? UI_FONT_TEXT : UI_FONT_SMALL;
    int hh = overlay_height(H), y0 = H - hh, mx = W / 20;
    int line1 = y0 + hh * 52 / 120, line2 = y0 + hh * 92 / 120;
    char a[32], b[32] = "", left[200], right[200], fitted[200];

    for (int row = 0; row < hh; row++) {            /* eased: soft at the top, 80% black at the bottom */
        double t = (double)(row + 1) / hh;
        ui_dim(fb, W, H, 0, y0 + row, W, 1, 256 - (int)(205.0 * t * t * (3.0 - 2.0 * t)));
    }

    snprintf(left, sizeof(left), "Subtitles: %s", sub_name);
    snprintf(right, sizeof(right), "Audio: %s", audio_name);
    ui_fit(font, left, W / 2 - mx - 20, fitted, sizeof(fitted));
    ui_text(fb, W, H, mx, line1, font, fitted, 0xFFFFFFFFu);
    ui_fit(font, right, W / 2 - mx - 20, fitted, sizeof(fitted));
    ui_text(fb, W, H, W - mx - ui_text_width(font, fitted), line1, font, fitted, 0xFFFFFFFFu);

    if (position < 0)
        position = 0;
    format_time(a, sizeof(a), position);
    ui_text(fb, W, H, mx, line2, font, a, 0xFFFFFFFFu);
    if (duration > 0) {
        format_time(b, sizeof(b), duration);
        int tw = ui_text_width(font, b), gap = H * 18 / 720, bar_h = H >= 1000 ? 6 : 4;
        int bx = mx + ui_text_width(font, "0:00:00") + gap, bw = W - mx - tw - gap - bx;
        int by = line2 - (H >= 1000 ? 12 : 8);
        double part = position / duration;
        if (part > 1) part = 1;
        ui_text(fb, W, H, W - mx - tw, line2, font, b, 0xFFFFFFFFu);
        ui_dim(fb, W, H, bx, by, bw, bar_h, 90);
        osd_fill(fb, W, H, bx, by, bw, bar_h, 0xFF5A5F6Eu);
        osd_fill(fb, W, H, bx, by, (int)(bw * part), bar_h, 0xFFFFFFFFu);
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

/* --- track names ------------------------------------------------------------ */

static const char *language_name(const char *code)
{
    static const char *const known[][2] = {
        { "eng", "English" }, { "hun", "Hungarian" }, { "ger", "German" }, { "deu", "German" },
        { "fre", "French" }, { "fra", "French" }, { "spa", "Spanish" }, { "ita", "Italian" },
        { "dut", "Dutch" }, { "nld", "Dutch" }, { "pol", "Polish" }, { "por", "Portuguese" },
        { "rus", "Russian" }, { "ukr", "Ukrainian" }, { "cze", "Czech" }, { "ces", "Czech" },
        { "slo", "Slovak" }, { "slk", "Slovak" }, { "rum", "Romanian" }, { "ron", "Romanian" },
        { "swe", "Swedish" }, { "nor", "Norwegian" }, { "dan", "Danish" }, { "fin", "Finnish" },
        { "gre", "Greek" }, { "ell", "Greek" }, { "tur", "Turkish" }, { "jpn", "Japanese" },
        { "chi", "Chinese" }, { "zho", "Chinese" }, { "kor", "Korean" }, { "vie", "Vietnamese" },
        { "hrv", "Croatian" }, { "srp", "Serbian" }, { "slv", "Slovenian" }, { "bul", "Bulgarian" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(code, known[i][0]) == 0)
            return known[i][1];
    return code;
}

/* A track as a person would name it: "English · AC3 5.1", "Hungarian · Forced". */
static void track_name(int stream, char *out, size_t out_size)
{
    AVStream *st = P.fmt->streams[stream];
    AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
    AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
    const char *name = lang && strcmp(lang->value, "und") != 0 ? language_name(lang->value) : NULL;
    char detail[80] = "";

    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        int ch = st->codecpar->ch_layout.nb_channels;
        char codec[32];
        snprintf(codec, sizeof(codec), "%s", avcodec_get_name(st->codecpar->codec_id));
        for (char *c = codec; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32;
        snprintf(detail, sizeof(detail), "%s %s", codec,
                 ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : "multichannel");
    } else if (title) {
        snprintf(detail, sizeof(detail), "%s", title->value);
    }
    if (name && detail[0])
        snprintf(out, out_size, "%s \xC2\xB7 %s", name, detail);
    else
        snprintf(out, out_size, "%s", name ? name : detail[0] ? detail : "Unnamed");
}

/* --- the pause menu ------------------------------------------------------------ */

typedef struct {
    int       *sub_sel;                 /* -1 = off, 0 = the subtitle file if there is one, then the tracks */
    int        have_ext, n_subs, n_audio;
    const int *sub_tracks, *audio_tracks;
    int        audio_now;               /* the sound track in use (stream index) */
    int        audio_pick;              /* out: the sound track asked for, -1 = no change */
    int        rewind, leave;           /* out */
} PauseMenu;

/* Stops the film, shows the menu over `frame` (the screen as it is; NULL:
 * black) and starts the film again. Returns how long that took, in
 * microseconds: the clocks stood still meanwhile. */
static uint64_t pause_menu(PauseMenu *m, const uint32_t *frame)
{
    static const char *const MAIN[] = { "Continue", "Subtitles", "Audio", "Picture", "Rewind", "Help", "Exit" };
    static const char *const PICTURE[] = { "Fit: the whole picture", "Fill: no borders, edges cut off",
                                           "Stretch: no borders, shape changed" };
    static const char *const KEYS[] = { "D-pad left / right", "L1 / R1", "D-pad up / down", "Circle",
                                        "Square", "OPTIONS", "Triangle" };
    static const char *const DOES[] = { "10 seconds back / forward", "10 minutes back / forward", "Volume",
                                        "Pause, this menu", "Next audio track", "Next subtitle",
                                        "Diagnostics line" };
    int W = plat_width(), H = plat_height(), sel = 0;
    uint64_t t0 = plat_time_us();
    uint32_t *still = malloc((size_t)W * H * sizeof(*still));
    char labels[MAX_TRACKS + 2][160];
    const char *items[MAX_TRACKS + 2];

    m->audio_pick = -1;
    m->rewind = m->leave = 0;
    if (!still)
        return 0;
    if (frame) {
        memcpy(still, frame, (size_t)W * H * sizeof(*still));
        ui_dim(still, W, H, 0, 0, W, H, 110);
    } else {
        for (size_t i = 0; i < (size_t)W * H; i++)
            still[i] = 0xFF000000u;
    }

    pthread_mutex_lock(&P.lock);
    P.paused = 1;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);

    for (;;) {
        int n = 0, cur = 0, pick = menu_pick(still, "Paused", MAIN, 7, sel);

        if (pick <= 0)                              /* Continue, or circle */
            break;
        sel = pick;
        if (pick == 1) {                            /* Subtitles */
            snprintf(labels[n++], sizeof(labels[0]), "No subtitle");
            if (m->have_ext)
                snprintf(labels[n++], sizeof(labels[0]), "Subtitle file");
            for (int i = 0; i < m->n_subs; i++)
                track_name(m->sub_tracks[i], labels[n++], sizeof(labels[0]));
            for (int i = 0; i < n; i++)
                items[i] = labels[i];
            pick = menu_pick(still, "Subtitles", items, n, *m->sub_sel + 1);
            if (pick >= 0) {
                *m->sub_sel = pick - 1;
                sel = 0;
            }
        } else if (pick == 2) {                     /* Audio */
            int now_stream = m->audio_pick >= 0 ? m->audio_pick : m->audio_now;
            for (int i = 0; i < m->n_audio; i++) {
                track_name(m->audio_tracks[i], labels[n], sizeof(labels[0]));
                if (m->audio_tracks[i] == now_stream)
                    cur = n;
                n++;
            }
            if (n == 0)
                snprintf(labels[n++], sizeof(labels[0]), "No audio in this file");
            for (int i = 0; i < n; i++)
                items[i] = labels[i];
            pick = menu_pick(still, "Audio", items, n, cur);
            if (pick >= 0) {
                if (m->n_audio > 0 && m->audio_now >= 0)
                    m->audio_pick = m->audio_tracks[pick] != m->audio_now ? m->audio_tracks[pick] : -1;
                sel = 0;
            }
        } else if (pick == 3) {                     /* Picture; the film shows it once it runs again */
            pick = menu_pick(still, "Picture", PICTURE, 3, g_picture);
            if (pick >= 0) {
                g_picture = pick;
                sel = 0;
            }
        } else if (pick == 4) {                     /* Rewind: from the start, and on with the film */
            m->rewind = 1;
            break;
        } else if (pick == 5) {
            menu_table(still, "Help", KEYS, DOES, 7);
        } else {                                    /* Exit */
            m->leave = 1;
            break;
        }
    }
    free(still);

    uint64_t t1 = plat_time_us();
    pthread_mutex_lock(&P.lock);
    P.paused = 0;
    P.aclock_time = t1;                             /* the audio clock stood still meanwhile */
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    return t1 - t0;
}

/* --- continuing where the last video was left ----------------------------- */

/* One file, one video: only the video played last is remembered. The first
 * line is the position in seconds, the second the video's location (without
 * the password of a share). */
static const char *resume_file(void)
{
    const char *env = getenv("MOZI_RESUME");
    return env ? env : "/data/orbismozi/resume.txt";
}

/* The position stored for this video, or 0 if it is not the one stored. */
static double resume_read(const char *name)
{
    char line[1100];
    double seconds = 0.0;
    FILE *f = fopen(resume_file(), "r");

    if (!f)
        return 0.0;
    if (fgets(line, sizeof(line), f))
        seconds = atof(line);
    if (!fgets(line, sizeof(line), f))
        seconds = 0.0;
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
    return strcmp(line, name) == 0 && seconds > 0.0 ? seconds : 0.0;
}

static void resume_write(const char *name, double seconds)
{
    FILE *f = fopen(resume_file(), "w");

    if (!f)
        return;
    fprintf(f, "%.1f\n%s\n", seconds, name);
    fclose(f);
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
    for (int i = 0; i < n_subs; i++) {
        AVStream *t = P.fmt->streams[sub_tracks[i]];
        AVCodecContext *dec = open_decoder(t, 1);
        if (dec) {
            t->discard = AVDISCARD_DEFAULT;
            P.sdec[P.n_sdec] = dec;
            P.sdec_stream[P.n_sdec++] = sub_tracks[i];
        }
    }
    osd_log("%d audio track(s), %d subtitle track(s)", n_audio, n_subs);
    P.aidx_want = P.aidx;

    /* A subtitle file beside the video comes first and is switched on:
     * whoever put it there wants it. The tracks inside the file follow. */
    ExtSub ext_sub;
    int have_ext = extsub_load(path, &ext_sub) == 0;
    int sub_total = n_subs + have_ext;
    int sub_sel = have_ext ? 0 : -1;                /* 0 = the file if there is one, then sub_tracks; -1 = off */

    /* The video that was left last time continues where it was; the start
     * and the last moments of a film are not worth going back to. */
    char resume_name[1024];
    double file_start = P.fmt->start_time != AV_NOPTS_VALUE ? (double)P.fmt->start_time / AV_TIME_BASE : 0.0;
    double file_length = P.fmt->duration > 0 ? (double)P.fmt->duration / AV_TIME_BASE : 0.0;
    smb_display_name(path, resume_name, sizeof(resume_name));
    double resume_at = max_seconds > 0 ? 0.0 : resume_read(resume_name);
    if (resume_at < RESUME_MARGIN || (file_length > 0 && resume_at > file_length - RESUME_MARGIN))
        resume_at = 0.0;
    if (resume_at > 0.0) {                          /* the demux thread starts with this jump */
        P.seek_target = file_start + resume_at;
        P.seek_from = P.seek_target + 1.0;          /* "backwards": land before the place, not after it */
        P.seek_req = 1;
        osd_log("continuing at %.0f s", resume_at);
    }

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

    uint64_t note_until = 0, repeat_at = 0, resume_saved = 0;
    int ended = 0;                                  /* played to the end, not stopped */

    if (resume_at > 0.0) {                          /* say so, and show where that is */
        char at[32];
        format_time(at, sizeof(at), resume_at);
        snprintf(note, sizeof(note), " Continuing from %s ", at);
        note_until = bar_until = plat_time_us() + 4000000;
        seeking = 1;
        seek_goal = file_start + resume_at;
    }
    unsigned overlay_sig = 0;                       /* what was drawn over the last picture */
    uint64_t menu_wanted = 0;                       /* when circle was pressed; 0 = not */
    PauseMenu menu = { .sub_sel = &sub_sel, .have_ext = have_ext, .n_subs = n_subs, .n_audio = n_audio,
                       .sub_tracks = sub_tracks, .audio_tracks = audio_tracks, .audio_pick = -1 };

    for (;;) {
        QFrame qf = { 0 };
        int have, done, serial_now, seek_pending;
        uint64_t now = plat_time_us();

        /* Controller: circle pauses and opens the menu, d-pad left/right and
         * L1/R1 jump, d-pad up/down is the volume, square and OPTIONS change
         * tracks, triangle shows the diagnostics line. Every button brings
         * the overlay up for a few seconds. */
        unsigned pad = plat_pad();
        unsigned pressed = pad & ~pad_prev;
        double step = 0.0;
        const unsigned repeating = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;

        if (pressed & repeating) {                  /* holding the d-pad repeats it */
            repeat_at = now + 400000;
        } else if ((pad & repeating) && now >= repeat_at) {
            pressed |= pad & repeating;
            repeat_at = now + ((pad & (PAD_LEFT | PAD_RIGHT)) ? 250000 : 80000);
        }
        pad_prev = pad;

        if (pressed)                                /* any button brings the overlay up */
            bar_until = now + 3000000;
        if ((pressed & PAD_CIRCLE) && !menu_wanted)
            menu_wanted = now;                      /* opens over the next picture */
        if (pressed & PAD_TRIANGLE)
            show_stats = !show_stats;

        /* Square: next sound track. It takes effect through a seek to where we are. */
        int audio_switch = -1;
        if ((pressed & PAD_SQUARE) && !isnan(pos)) {
            int cur = 0;
            pthread_mutex_lock(&P.lock);
            for (int i = 0; i < n_audio; i++)
                if (audio_tracks[i] == P.aidx_want)
                    cur = i;
            pthread_mutex_unlock(&P.lock);
            if (n_audio > 1 && P.aidx >= 0) {
                cur = (cur + 1) % n_audio;
                audio_switch = audio_tracks[cur];
            }
        }
        if (menu.audio_pick >= 0) {                 /* chosen in the pause menu */
            audio_switch = menu.audio_pick;
            menu.audio_pick = -1;
        }

        /* OPTIONS: next subtitle track, then off. */
        if (pressed & PAD_OPTIONS)
            sub_sel = sub_sel + 1 < sub_total ? sub_sel + 1 : -1;
        if (pressed & PAD_RIGHT) step += 10.0;
        if (pressed & PAD_LEFT)  step -= 10.0;
        if (pressed & (PAD_UP | PAD_DOWN)) {        /* volume; above 100% boosts quiet films */
            g_volume += (pressed & PAD_UP) ? 5 : -5;
            if (g_volume > 200) g_volume = 200;
            if (g_volume < 0)   g_volume = 0;
            snprintf(note, sizeof(note), " Volume %d%% ", g_volume);
            note_until = now + 2000000;
        }
        if (pressed & PAD_R1)    step += 600.0;
        if (pressed & PAD_L1)    step -= 600.0;
        if (menu.rewind) {                          /* chosen in the pause menu: back to the start */
            step = -1e9;
            menu.rewind = 0;
        }

        /* No picture arrives (the end of the file, a slow share): the menu
         * does not wait for one for ever. */
        if (menu_wanted && now - menu_wanted > 700000) {
            menu_wanted = 0;
            menu.audio_now = P.aidx_want;
            wall_start += pause_menu(&menu, NULL);
            if (menu.leave)
                break;
            pad_prev = plat_pad();
            bar_until = plat_time_us() + 3000000;
            scaler.clear_pending = 2;
            continue;
        }

        int toggle_pause = 0;
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
            if (done) {
                ended = 1;
                break;
            }
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
            f->format != scaler.src_fmt || scaler.mode != g_picture) {
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
        if (have_ext && sub_sel == 0) {             /* the file counts from the start of the video */
            double t = clock - (isnan(origin) ? 0.0 : origin);
            for (int k = 0; k < ext_sub.count; k++) {
                const ExtCue *cue = &ext_sub.cues[k];
                if (t >= cue->start && t < cue->end &&
                    strlen(sub_text) + strlen(cue->text) + 2 < sizeof(sub_text)) {
                    if (sub_text[0])
                        strcat(sub_text, "\n");
                    strcat(sub_text, cue->text);
                }
            }
        }
        pthread_mutex_lock(&P.lock);
        for (int k = 0; k < CUE_CAP; k++) {
            const Cue *cue = &P.cues[(P.cue_next + k) % CUE_CAP];       /* oldest first */
            if (sub_sel >= have_ext && cue->stream == sub_tracks[sub_sel - have_ext] &&
                cue->text[0] && clock >= cue->start && clock < cue->end &&
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
        sig = sig * 31 + (unsigned)(sub_sel + 1) * 64 + (unsigned)P.aidx_want;
        if (sig != overlay_sig || menu_wanted) {
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
        if (bar_on)                                 /* the strip's text changes: fresh black under it, outside the picture */
            osd_fill(fb, W, H, 0, H - overlay_height(H), W, overlay_height(H), 0xFF000000u);

        uint64_t t0 = plat_time_us();
        uint8_t *dst[4] = { (uint8_t *)(fb + (size_t)scaler.dst_y * W + scaler.dst_x), NULL, NULL, NULL };
        int dst_stride[4] = { W * 4, 0, 0, 0 };
        const uint8_t *src[4] = { f->data[0], f->data[1], f->data[2], f->data[3] };
        if (scaler.crop_x || scaler.crop_y) {       /* start each plane at the corner of the part shown */
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
            for (int c = 0; desc && c < desc->nb_components; c++) {
                int pl = desc->comp[c].plane, chroma = pl == 1 || pl == 2;
                if (desc->comp[c].offset == 0 && f->data[pl])
                    src[pl] = f->data[pl]
                            + (size_t)(scaler.crop_y >> (chroma ? desc->log2_chroma_h : 0)) * f->linesize[pl]
                            + (size_t)(scaler.crop_x >> (chroma ? desc->log2_chroma_w : 0)) * desc->comp[c].step;
            }
        }
        sws_scale(scaler.sws, src, f->linesize, 0, scaler.crop_h, dst, dst_stride);
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
        if (menu_wanted) {                          /* the menu stands on this picture, with its subtitle */
            menu_wanted = 0;
            if (sub_text[0])
                subfont_draw(fb, W, H, sub_text, H - H / 20);
            menu.audio_now = P.aidx_want;
            wall_start += pause_menu(&menu, fb);
            if (menu.leave)
                break;
            pad_prev = plat_pad();
            bar_until = plat_time_us() + 3000000;
            scaler.clear_pending = 2;
            overlay_sig = ~0u;
            continue;
        }

        if (show_stats)
            osd_text(fb, W, H, W / 20, H / 20, stat_line, 0xFFFFFFFFu, 0xFF000000u);
        if (note_on)
            osd_text(fb, W, H, W / 20, H / 20 + (show_stats ? osd_font_height() + 6 : 0), note,
                     0xFFFFFFFFu, 0xFF101428u);
        if (bar_on) {
            char sub_name[160] = "No subtitle", audio_name[160] = "No audio";
            if (have_ext && sub_sel == 0)
                snprintf(sub_name, sizeof(sub_name), "Subtitle file");
            else if (sub_sel >= 0)
                track_name(sub_tracks[sub_sel - have_ext], sub_name, sizeof(sub_name));
            if (P.aidx_want >= 0)
                track_name(P.aidx_want, audio_name, sizeof(audio_name));
            draw_overlay(fb, W, H, (seeking ? seek_goal : clock) - origin, duration, sub_name, audio_name);
        }
        if (sub_text[0])
            subfont_draw(fb, W, H, sub_text, bar_on ? H - overlay_height(H) : H - H / 20);

        plat_flip();
        st.seconds_played = clock - origin;

        if (now - resume_saved > 15000000 && !isnan(pos) && max_seconds <= 0) {    /* also survives a closed application */
            resume_saved = now;
            resume_write(resume_name, pos - file_start);
        }
    }

    if (max_seconds <= 0 && !failed) {
        double at = isnan(pos) ? 0.0 : (seeking ? seek_goal : pos) - file_start;
        if (ended || (file_length > 0 && at > file_length - RESUME_MARGIN))
            at = 0.0;                               /* seen to the end: next time from the start */
        resume_write(resume_name, at);
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
    for (int k = 0; k < P.n_sdec; k++)
        avcodec_free_context(&P.sdec[k]);
    if (have_ext)
        extsub_free(&ext_sub);
    close_input(net);
    pthread_mutex_destroy(&P.lock);
    pthread_cond_destroy(&P.cond);

    if (st.frames_shown > 0)
        st.avg_convert_ms = convert_total / st.frames_shown;
    if (stats)
        *stats = st;
    return failed ? -1 : 0;
}
