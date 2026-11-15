/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The decoder of one web page <audio> / <video> stream. The kernel
 * (kernel/media.c) starts a plain web worker for each stream and drives it:
 * WW_MEDIA_OPEN gives the file, WW_MEDIA_AUDIO asks for sound (16-bit
 * stereo at WW_MEDIA_RATE), WW_MEDIA_VIDEO for the picture due at a time,
 * WW_MEDIA_SEEK goes elsewhere. FFmpeg (ports/ffmpeg) runs here, in the
 * restricted worker process, never in the kernel: a hostile file can only
 * crash this process. One thread: packets are read in file order, the
 * sound and the pictures they hold queue until asked for (bounded). */
#include <banana.h>
#include <banana_web_worker.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define MAX_W      1280               /* pictures are scaled down to fit */
#define MAX_H      720
#define PCM_MAX    (WW_MEDIA_RATE * 3)  /* frames of sound kept ahead, at most */
#define PICS       6                  /* pictures kept ahead */

typedef struct { int pts; uint32_t* px; } pic_t;

static const uint8_t* g_data;         /* the file */
static size_t   g_len, g_pos;
static AVIOContext* g_io;
static AVFormatContext* g_fmt;
static AVCodecContext *g_vdec, *g_adec;
static struct SwsContext* g_sws;
static SwrContext* g_swr;
static int g_vidx = -1, g_aidx = -1, g_w, g_h, g_eof;
static AVPacket* g_pkt;
static AVFrame* g_frame;
static int16_t* g_pcm;                /* queued sound: g_pcm_n frames from g_pcm_ms */
static int g_pcm_n, g_pcm_ms = -1;
static pic_t g_pics[PICS];
static int g_npics;
static int g_skip_ms = -1;            /* after a seek: nothing earlier than this */

static int io_read(void* o, uint8_t* buf, int n) {
    (void)o;
    if (g_pos >= g_len) return AVERROR_EOF;
    size_t k = g_len - g_pos < (size_t)n ? g_len - g_pos : (size_t)n;
    memcpy(buf, g_data + g_pos, k);
    g_pos += k;
    return (int)k;
}
static int64_t io_seek(void* o, int64_t off, int whence) {
    (void)o;
    if (whence == AVSEEK_SIZE) return (int64_t)g_len;
    int64_t base = (whence & 3) == SEEK_CUR ? (int64_t)g_pos : (whence & 3) == SEEK_END ? (int64_t)g_len : 0;
    int64_t p = base + off;
    if (p < 0 || p > (int64_t)g_len) return -1;
    g_pos = (size_t)p;
    return p;
}

static AVCodecContext* open_decoder(int idx) {
    AVStream* st = g_fmt->streams[idx];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return NULL;
    AVCodecContext* d = avcodec_alloc_context3(c);
    if (!d) return NULL;
    d->thread_count = 1;
    if (avcodec_parameters_to_context(d, st->codecpar) < 0 || avcodec_open2(d, c, NULL) < 0) {
        avcodec_free_context(&d);
        return NULL;
    }
    d->pkt_timebase = st->time_base;
    return d;
}

static int ms_of(int64_t ts, int idx) {
    if (ts == AV_NOPTS_VALUE) return -1;
    AVRational tb = g_fmt->streams[idx]->time_base;
    return (int)(ts * 1000 * tb.num / tb.den);
}

/* sound frames into the queue (the oldest go when it is full) */
static void got_audio(AVFrame* f) {
    int ms = ms_of(f->best_effort_timestamp, g_aidx);
    int out = swr_get_out_samples(g_swr, f->nb_samples);
    if (out <= 0) return;
    if (g_pcm_n + out > PCM_MAX) {
        int drop = g_pcm_n + out - PCM_MAX;
        if (drop > g_pcm_n) drop = g_pcm_n;
        memmove(g_pcm, g_pcm + drop * 2, (size_t)(g_pcm_n - drop) * 4);
        g_pcm_n -= drop;
        if (g_pcm_ms >= 0) g_pcm_ms += (int)((int64_t)drop * 1000 / WW_MEDIA_RATE);
    }
    if (g_pcm_n + out > PCM_MAX) out = PCM_MAX - g_pcm_n;
    uint8_t* dst[1] = { (uint8_t*)(g_pcm + g_pcm_n * 2) };
    int got = swr_convert(g_swr, dst, out, (const uint8_t**)f->extended_data, f->nb_samples);
    if (got <= 0) return;
    if (!g_pcm_n || g_pcm_ms < 0) g_pcm_ms = ms >= 0 ? ms : 0;
    g_pcm_n += got;
    if (g_skip_ms >= 0) {                                /* after a seek: from there on */
        int end = g_pcm_ms + (int)((int64_t)g_pcm_n * 1000 / WW_MEDIA_RATE);
        if (end <= g_skip_ms) { g_pcm_n = 0; g_pcm_ms = -1; return; }
        if (g_pcm_ms < g_skip_ms) {
            int drop = (int)((int64_t)(g_skip_ms - g_pcm_ms) * WW_MEDIA_RATE / 1000);
            if (drop > g_pcm_n) drop = g_pcm_n;
            memmove(g_pcm, g_pcm + drop * 2, (size_t)(g_pcm_n - drop) * 4);
            g_pcm_n -= drop;
            g_pcm_ms = g_skip_ms;
        }
    }
}

/* a decoded picture, scaled, into the queue (the oldest goes when it is full) */
static void got_video(AVFrame* f) {
    int ms = ms_of(f->best_effort_timestamp, g_vidx);
    if (ms < 0) ms = g_npics ? g_pics[g_npics - 1].pts + 40 : 0;
    if (g_skip_ms >= 0 && ms + 40 < g_skip_ms) return;
    g_sws = sws_getCachedContext(g_sws, f->width, f->height, (enum AVPixelFormat)f->format,
                                 g_w, g_h, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
    if (!g_sws) return;
    if (g_npics == PICS) {
        free(g_pics[0].px);
        memmove(g_pics, g_pics + 1, sizeof(pic_t) * (PICS - 1));
        g_npics--;
    }
    uint32_t* px = malloc((size_t)g_w * g_h * 4);
    if (!px) return;
    uint8_t* dst[4] = { (uint8_t*)px, NULL, NULL, NULL };
    int stride[4] = { g_w * 4, 0, 0, 0 };
    sws_scale(g_sws, (const uint8_t* const*)f->data, f->linesize, 0, f->height, dst, stride);
    for (int i = 0; i < g_w * g_h; i++) px[i] &= 0xFFFFFFu;
    g_pics[g_npics].pts = ms;
    g_pics[g_npics].px = px;
    g_npics++;
}

static void drain(AVCodecContext* d, int video) {
    while (avcodec_receive_frame(d, g_frame) == 0) {
        if (video) got_video(g_frame); else got_audio(g_frame);
        av_frame_unref(g_frame);
    }
}

/* one packet: 0, or -1 at the end of the file */
static int step(void) {
    if (g_eof) return -1;
    int r = av_read_frame(g_fmt, g_pkt);
    if (r < 0) {
        g_eof = 1;
        if (g_vdec) { avcodec_send_packet(g_vdec, NULL); drain(g_vdec, 1); }
        if (g_adec) { avcodec_send_packet(g_adec, NULL); drain(g_adec, 0); }
        return -1;
    }
    if (g_pkt->stream_index == g_vidx && g_vdec) {
        if (avcodec_send_packet(g_vdec, g_pkt) == 0) drain(g_vdec, 1);
    } else if (g_pkt->stream_index == g_aidx && g_adec) {
        if (avcodec_send_packet(g_adec, g_pkt) == 0) drain(g_adec, 0);
    }
    av_packet_unref(g_pkt);
    return 0;
}

static int media_open(const void* data, unsigned int n, unsigned char** out, unsigned int* out_len) {
    if (g_fmt || !data || n < 16) return -1;
    uint8_t* copy = malloc(n);
    if (!copy) return -1;
    memcpy(copy, data, n);
    g_data = copy;
    g_len = n;
    unsigned char* iobuf = av_malloc(32768);
    g_io = iobuf ? avio_alloc_context(iobuf, 32768, 0, NULL, io_read, NULL, io_seek) : NULL;
    g_fmt = avformat_alloc_context();
    if (!g_io || !g_fmt) return -1;
    g_fmt->pb = g_io;
    if (avformat_open_input(&g_fmt, NULL, NULL, NULL) < 0) { g_fmt = NULL; return -1; }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) return -1;
    g_vidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_aidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (g_vidx >= 0 && (g_fmt->streams[g_vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) g_vidx = -1;
    if (g_vidx >= 0 && !(g_vdec = open_decoder(g_vidx))) g_vidx = -1;
    if (g_aidx >= 0 && !(g_adec = open_decoder(g_aidx))) g_aidx = -1;
    if (g_adec) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&g_swr, &stereo, AV_SAMPLE_FMT_S16, WW_MEDIA_RATE,
                                &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate, 0, NULL) < 0 ||
            swr_init(g_swr) < 0) {
            swr_free(&g_swr);
            avcodec_free_context(&g_adec);
            g_aidx = -1;
        }
    }
    if (g_vidx < 0 && g_aidx < 0) return -1;
    if (g_vdec) {
        int w = g_vdec->width, h = g_vdec->height;
        if (w <= 0 || h <= 0) { avcodec_free_context(&g_vdec); g_vidx = -1; }
        else {
            /* the shown size: the stream's, at most MAX_W x MAX_H, even */
            AVRational sar = g_vdec->sample_aspect_ratio;
            if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = (int)((int64_t)w * sar.num / sar.den);
            if (w > MAX_W) { h = (int)((int64_t)h * MAX_W / w); w = MAX_W; }
            if (h > MAX_H) { w = (int)((int64_t)w * MAX_H / h); h = MAX_H; }
            g_w = w < 2 ? 2 : w & ~1;
            g_h = h < 2 ? 2 : h & ~1;
        }
    }
    g_pcm = malloc((size_t)PCM_MAX * 4);
    g_pkt = av_packet_alloc();
    g_frame = av_frame_alloc();
    if (!g_pcm || !g_pkt || !g_frame) return -1;
    ww_media_info_t* info = calloc(1, sizeof(*info));
    if (!info) return -1;
    info->has_audio = g_aidx >= 0;
    info->has_video = g_vidx >= 0;
    info->width = g_w;
    info->height = g_h;
    info->duration_ms = g_fmt->duration > 0 ? (int)(g_fmt->duration / 1000) : 0;
    *out = (unsigned char*)info;
    *out_len = sizeof(*info);
    return 0;
}

/* the queued sound earlier than ms goes (it was muted, or is late) */
static void pcm_drop_before(int ms) {
    if (ms <= 0 || g_pcm_ms < 0 || g_pcm_ms >= ms) return;
    int drop = (int)((int64_t)(ms - g_pcm_ms) * WW_MEDIA_RATE / 1000);
    if (drop >= g_pcm_n) { g_pcm_n = 0; g_pcm_ms = -1; return; }
    memmove(g_pcm, g_pcm + drop * 2, (size_t)(g_pcm_n - drop) * 4);
    g_pcm_n -= drop;
    g_pcm_ms = ms;
}

static int media_audio(int want, int from, unsigned char** out, unsigned int* out_len) {
    if (!g_adec) return -1;
    if (want <= 0 || want > WW_MEDIA_RATE) want = WW_MEDIA_RATE / 10;
    pcm_drop_before(from);
    for (int i = 0; i < 4000 && (g_pcm_ms < 0 || g_pcm_ms + 5000 < from) && !g_eof; i++) {   /* far behind: read on */
        step();
        pcm_drop_before(from);
    }
    for (int i = 0; i < 4000 && g_pcm_n < want && !g_eof; i++) {
        step();
        if (g_npics == PICS && g_pcm_n == 0 && i > 200) break;   /* a stretch of pictures only */
    }
    int n = g_pcm_n < want ? g_pcm_n : want;
    unsigned char* buf = malloc(sizeof(ww_media_audio_t) + (size_t)n * 4);
    if (!buf) return -1;
    ww_media_audio_t h = { g_pcm_ms < 0 ? 0 : g_pcm_ms, n, g_eof && g_pcm_n <= n };
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), g_pcm, (size_t)n * 4);
    memmove(g_pcm, g_pcm + n * 2, (size_t)(g_pcm_n - n) * 4);
    g_pcm_n -= n;
    if (g_pcm_ms >= 0) g_pcm_ms += (int)((int64_t)n * 1000 / WW_MEDIA_RATE);
    *out = buf;
    *out_len = sizeof(h) + (unsigned)n * 4;
    return 0;
}

static int media_video(int t, int had, unsigned char** out, unsigned int* out_len) {
    if (!g_vdec) return -1;
    /* read on until a picture later than t waits (or the file ends) */
    for (int i = 0; i < 4000 && !g_eof && (!g_npics || g_pics[g_npics - 1].pts <= t); i++) {
        step();
        if (g_pcm_n >= PCM_MAX - WW_MEDIA_RATE / 4 && !g_adec) break;
    }
    /* the last one due by t (or the first, before the first is due) */
    int k = -1;
    for (int i = 0; i < g_npics; i++) if (g_pics[i].pts <= t) k = i;
    if (k < 0 && g_npics) k = 0;
    ww_media_picture_t h = { 0, 0, 0, g_eof && g_npics <= (k < 0 ? 0 : k + 1) };
    unsigned char* buf;
    size_t len = sizeof(h);
    if (k >= 0 && g_pics[k].pts + 1 != had) {
        h.pts_ms = g_pics[k].pts;
        h.width = g_w;
        h.height = g_h;
        len += (size_t)g_w * g_h * 4;
        buf = malloc(len);
        if (!buf) return -1;
        memcpy(buf + sizeof(h), g_pics[k].px, (size_t)g_w * g_h * 4);
    } else {
        if (k >= 0) h.pts_ms = g_pics[k].pts;
        buf = malloc(len);
        if (!buf) return -1;
    }
    memcpy(buf, &h, sizeof(h));
    /* the ones before it are done with */
    if (k > 0) {
        for (int i = 0; i < k; i++) free(g_pics[i].px);
        memmove(g_pics, g_pics + k, sizeof(pic_t) * (size_t)(g_npics - k));
        g_npics -= k;
    }
    *out = buf;
    *out_len = (unsigned)len;
    return 0;
}

static int media_seek(int ms) {
    if (!g_fmt) return -1;
    if (ms < 0) ms = 0;
    int64_t ts = (int64_t)ms * (AV_TIME_BASE / 1000);
    if (av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0 &&
        av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) < 0) {
        /* no index: from the start */
        g_pos = 0;
        avio_seek(g_io, 0, SEEK_SET);
        av_seek_frame(g_fmt, -1, 0, AVSEEK_FLAG_BYTE);
    }
    if (g_vdec) avcodec_flush_buffers(g_vdec);
    if (g_adec) avcodec_flush_buffers(g_adec);
    for (int i = 0; i < g_npics; i++) free(g_pics[i].px);
    g_npics = 0;
    g_pcm_n = 0;
    g_pcm_ms = -1;
    g_eof = 0;
    g_skip_ms = ms;
    return 0;
}

int ww_media_execute(unsigned int op, const int* a, const void* data, unsigned int n,
                     unsigned char** out, unsigned int* out_len) {
    switch (op) {
    case WW_MEDIA_OPEN:  return media_open(data, n, out, out_len);
    case WW_MEDIA_AUDIO: return g_fmt ? media_audio(a[0], a[1], out, out_len) : -1;
    case WW_MEDIA_VIDEO: return g_fmt ? media_video(a[0], a[1], out, out_len) : -1;
    case WW_MEDIA_SEEK:  return media_seek(a[0]);
    default: return -1;
    }
}
