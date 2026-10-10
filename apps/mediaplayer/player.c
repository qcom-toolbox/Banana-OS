/* Media Player - videos and music with FFmpeg (ports/ffmpeg): MP4, MKV,
 * WebM, AVI, MOV, MPEG, OGG, FLV, WMV... with H.264, H.265, VP8, VP9,
 * MPEG-4, MPEG-1/2, Theora video and AAC, MP3, Opus, Vorbis, FLAC, AC-3
 * sound.
 *
 * A decoder thread reads the file, sends the sound to the card's queue
 * and turns the pictures into window pixels (a few frames ahead); the
 * main thread shows each picture when the sound reaches its time, and
 * runs the window.
 *
 *     mediaplayer [file]        (no file: a list to pick one from)
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define WIN_W    800
#define WIN_H    500
#define BAR_H    56                       /* the controls under the picture */
#define RING     3                        /* pictures decoded ahead */
#define OUT_RATE 48000

#define C_BG     0x000000u
#define C_BAR    0x1D232Cu
#define C_TEXT   0xE8EEF6u
#define C_DIM    0x8A96A8u
#define C_ACCENT 0x68A8F0u
#define C_TRACK  0x3A4454u

static bwin_t win;

/* ── the decoder's state ──────────────────────────────────────────── */
static AVFormatContext* g_fmt;
static AVCodecContext*  g_vdec;
static AVCodecContext*  g_adec;
static struct SwsContext* g_sws;
static SwrContext*      g_swr;
static int     g_vidx = -1, g_aidx = -1, g_pic_idx = -1;
static double  g_duration;                /* seconds, 0 if unknown */
static char    g_title[160], g_artist[96], g_file[256];

typedef struct {
    unsigned int* px;
    int    w, h;
    double pts;
} frame_t;
static frame_t g_ring[RING];
static int     g_rhead, g_rcount;         /* read here, how many waiting */

static banana_mutex_t g_lock = BANANA_MUTEX_INIT;
static banana_cond_t  g_cond = BANANA_COND_INIT;

/* shared between the threads (under g_lock) */
static volatile int g_quit, g_paused, g_eof, g_seek_req;
static double  g_seek_to;
static double  g_skip_until = -1;         /* after a seek: drop what comes before this */
static int     g_tw = 320, g_th = 240;    /* the picture size the window wants */

/* the clock: with sound, how far the card has played; without, the wall clock */
static double  g_audio_base;              /* stream time of the first sample sent since the last reset */
static unsigned long long g_audio_sent;   /* samples (per channel) sent since then */
static int     g_has_audio_out;
static int     g_ended;                  /* played to the end (shown paused there) */
static volatile int g_audio_dead;        /* the card does not play: no sound */
static unsigned int g_wall_start;         /* banana_ticks() at stream time g_wall_base */
static double  g_wall_base;
static double  g_paused_at;
static double  g_level;                   /* loudness of the latest sound (0..1), for the bars */

static double now_clock(void) {
    if (g_paused) return g_paused_at;
    double t;
    if (g_has_audio_out) {
        t = g_audio_base + (double)g_audio_sent / OUT_RATE - banana_audio_queued_ms() / 1000.0;
        if (t < g_audio_base) t = g_audio_base;
    } else {
        t = g_wall_base + (banana_ticks() - g_wall_start) / 1000.0;
    }
    return g_duration > 0 && t > g_duration ? g_duration : t;
}

static void reset_clock(double t) {
    g_audio_base = t;
    g_audio_sent = 0;
    g_wall_base = t;
    g_wall_start = banana_ticks();
}

/* ── opening a file ───────────────────────────────────────────────── */
static AVCodecContext* open_decoder(int idx) {
    AVStream* st = g_fmt->streams[idx];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return NULL;
    AVCodecContext* d = avcodec_alloc_context3(c);
    if (!d) return NULL;
    if (avcodec_parameters_to_context(d, st->codecpar) < 0 || avcodec_open2(d, c, NULL) < 0) {
        avcodec_free_context(&d);
        return NULL;
    }
    d->pkt_timebase = st->time_base;
    return d;
}

static const char* base_name(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int open_media(const char* path, char* err, int ecap) {
    int r = avformat_open_input(&g_fmt, path, NULL, NULL);
    if (r < 0) { av_strerror(r, err, ecap); return -1; }
    if ((r = avformat_find_stream_info(g_fmt, NULL)) < 0) { av_strerror(r, err, ecap); return -1; }
    g_vidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_aidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (g_vidx >= 0 && (g_fmt->streams[g_vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        g_pic_idx = g_vidx;                        /* an album cover, not a video */
    }
    if (g_vidx >= 0 && !(g_vdec = open_decoder(g_vidx))) g_vidx = g_pic_idx = -1;
    if (g_aidx >= 0 && !(g_adec = open_decoder(g_aidx))) g_aidx = -1;
    if (g_vidx < 0 && g_aidx < 0) { snprintf(err, ecap, "no picture or sound this player can decode"); return -1; }
    if (g_adec) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&g_swr, &stereo, AV_SAMPLE_FMT_S16, OUT_RATE,
                                &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate, 0, NULL) < 0 ||
            (av_opt_set_sample_fmt(g_swr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0), swr_init(g_swr)) < 0) {
            swr_free(&g_swr);
            avcodec_free_context(&g_adec);
            g_aidx = -1;
        }
    }
    g_duration = g_fmt->duration > 0 ? g_fmt->duration / (double)AV_TIME_BASE : 0;
    const AVDictionaryEntry* t = av_dict_get(g_fmt->metadata, "title", NULL, 0);
    const AVDictionaryEntry* a = av_dict_get(g_fmt->metadata, "artist", NULL, 0);
    snprintf(g_title, sizeof(g_title), "%s", t && t->value[0] ? t->value : base_name(path));
    snprintf(g_artist, sizeof(g_artist), "%s", a ? a->value : "");
    snprintf(g_file, sizeof(g_file), "%s", path);
    return 0;
}

/* ── the decoder thread ───────────────────────────────────────────── */
static int16_t* g_pcm;
static int      g_pcm_cap;                 /* samples per channel */

static void play_audio(AVFrame* f) {
    if (g_audio_dead) return;
    double pts = f->best_effort_timestamp != AV_NOPTS_VALUE
               ? f->best_effort_timestamp * av_q2d(g_fmt->streams[g_aidx]->time_base) : -1;
    int out = swr_get_out_samples(g_swr, f->nb_samples);
    if (out > g_pcm_cap) {
        int16_t* n = realloc(g_pcm, (size_t)out * 4);
        if (!n) return;
        g_pcm = n;
        g_pcm_cap = out;
    }
    uint8_t* dst[1] = { (uint8_t*)g_pcm };
    int got = swr_convert(g_swr, dst, out, (const uint8_t**)f->extended_data, f->nb_samples);
    if (got <= 0) return;
    int skip = 0;
    if (g_skip_until >= 0 && pts >= 0) {           /* after a seek: only from the target on */
        double end = pts + (double)got / OUT_RATE;
        if (end <= g_skip_until) return;
        if (pts < g_skip_until) skip = (int)((g_skip_until - pts) * OUT_RATE);
        if (skip > got) skip = got;
    }
    if (g_audio_sent == 0 && pts >= 0) {
        g_audio_base = (skip ? g_skip_until : pts);
        g_has_audio_out = 1;
    }
    /* the loudness, for the bars shown with music */
    long long sum = 0;
    for (int i = skip * 2; i < got * 2; i += 16) sum += (long long)g_pcm[i] * g_pcm[i];
    int n = (got - skip) * 2 / 16;
    if (n > 0) g_level = __builtin_sqrt((double)sum / n) / 12000.0;
    if (banana_play(g_pcm + skip * 2, (unsigned long)(got - skip) * 4, OUT_RATE, 2, 16) < 0) {
        g_has_audio_out = 0;                          /* no sound card: the wall clock */
        return;
    }
    g_audio_sent += (unsigned long long)(got - skip);
}

/* the frame converted into window pixels, into the ring (waits for room) */
static void queue_picture(AVFrame* f, double pts) {
    banana_lock(&g_lock);
    while (g_rcount == RING && !g_quit && !g_seek_req) banana_cond_timedwait(&g_cond, &g_lock, 50);
    if (g_quit || g_seek_req) { banana_unlock(&g_lock); return; }
    int tw = g_tw, th = g_th;
    frame_t* fr = &g_ring[(g_rhead + g_rcount) % RING];
    banana_unlock(&g_lock);

    /* the picture's size in the window, keeping its proportions */
    double sar = f->sample_aspect_ratio.num && f->sample_aspect_ratio.den ? av_q2d(f->sample_aspect_ratio) : 1.0;
    double ar = f->width * sar / (double)f->height;
    int w = tw, h = (int)(tw / ar);
    if (h > th) { h = th; w = (int)(th * ar); }
    w &= ~1;
    if (w < 2 || h < 2) return;
    if (!fr->px || fr->w * fr->h < w * h) {
        free(fr->px);
        fr->px = malloc((size_t)w * h * 4);
        if (!fr->px) return;
    }
    g_sws = sws_getCachedContext(g_sws, f->width, f->height, f->format, w, h, AV_PIX_FMT_BGR0,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_sws) return;
    uint8_t* dst[4] = { (uint8_t*)fr->px, NULL, NULL, NULL };
    int dst_ls[4] = { w * 4, 0, 0, 0 };
    sws_scale(g_sws, (const uint8_t* const*)f->data, f->linesize, 0, f->height, dst, dst_ls);
    fr->w = w;
    fr->h = h;
    fr->pts = pts;
    banana_lock(&g_lock);
    if (!g_seek_req) g_rcount++;
    banana_cond_broadcast(&g_cond);
    banana_unlock(&g_lock);
}

static void decode_video(AVPacket* pkt) {
    if (avcodec_send_packet(g_vdec, pkt) < 0 && pkt) return;
    AVFrame* f = av_frame_alloc();
    while (f && avcodec_receive_frame(g_vdec, f) == 0) {
        double pts = f->best_effort_timestamp != AV_NOPTS_VALUE
                   ? f->best_effort_timestamp * av_q2d(g_fmt->streams[g_vidx]->time_base) : now_clock();
        int keep = 1;
        if (g_skip_until >= 0 && pts < g_skip_until - 0.001 && g_vidx != g_pic_idx) keep = 0;
        /* behind the sound by more than a few frames: this one is dropped */
        else if (g_has_audio_out && g_vidx != g_pic_idx && pts < now_clock() - 0.12) keep = 0;
        if (keep) queue_picture(f, g_vidx == g_pic_idx ? -1 : pts);
        av_frame_unref(f);
        if (g_seek_req || g_quit) break;
    }
    av_frame_free(&f);
}

static void decode_audio(AVPacket* pkt) {
    if (avcodec_send_packet(g_adec, pkt) < 0 && pkt) return;
    AVFrame* f = av_frame_alloc();
    while (f && avcodec_receive_frame(g_adec, f) == 0) {
        if (!g_paused) play_audio(f);
        av_frame_unref(f);
        if (g_seek_req || g_quit) break;
    }
    av_frame_free(&f);
}

static void do_seek(void) {
    double to = g_seek_to;
    if (to < 0) to = 0;
    if (g_duration > 0 && to > g_duration) to = g_duration;
    banana_audio_stop();
    int64_t ts = (int64_t)(to * AV_TIME_BASE);
    avformat_seek_file(g_fmt, -1, INT64_MIN, ts, ts, 0);
    if (g_vdec) avcodec_flush_buffers(g_vdec);
    if (g_adec) { avcodec_flush_buffers(g_adec); swr_init(g_swr); }
    banana_lock(&g_lock);
    g_rcount = 0;
    g_seek_req = 0;
    g_eof = 0;
    g_skip_until = to;
    reset_clock(to);
    g_has_audio_out = 0;
    banana_cond_broadcast(&g_cond);
    banana_unlock(&g_lock);
}

static int decoder_thread(void* arg) {
    (void)arg;
    AVPacket* pkt = av_packet_alloc();
    while (!g_quit) {
        if (g_seek_req) { do_seek(); continue; }
        if (g_paused || g_eof) {
            banana_lock(&g_lock);
            if ((g_paused || g_eof) && !g_seek_req && !g_quit) banana_cond_timedwait(&g_cond, &g_lock, 100);
            banana_unlock(&g_lock);
            continue;
        }
        int r = av_read_frame(g_fmt, pkt);
        if (r < 0) {                                   /* the end: what the decoders still hold */
            if (g_vdec) decode_video(NULL);
            if (g_adec) decode_audio(NULL);
            g_eof = 1;
            continue;
        }
        if (pkt->stream_index == g_vidx) decode_video(pkt);
        else if (pkt->stream_index == g_aidx) decode_audio(pkt);
        av_packet_unref(pkt);
        /* sound decoded once a seek target is reached: no more skipping */
        if (g_skip_until >= 0 && g_has_audio_out) g_skip_until = -1;
        if (g_skip_until >= 0 && g_aidx < 0 && g_rcount) g_skip_until = -1;
    }
    av_packet_free(&pkt);
    return 0;
}

/* ── the window ───────────────────────────────────────────────────── */
static frame_t g_shown;                   /* the picture on the screen (a copy) */
static int     g_dragging;                /* the seek bar */
static char    g_msg[128];

static void fmt_time(char* b, int cap, double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    if (s >= 3600) snprintf(b, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%d:%02d", s / 60, s % 60);
}

static void video_area(int* x, int* y, int* w, int* h) {
    *x = 0; *y = 0; *w = win.w; *h = win.h - BAR_H;
}

static void seek_bar(int* x, int* y, int* w) {
    *x = 64; *y = win.h - BAR_H + 12; *w = win.w - 64 - 16;
}

static void draw_picture(void) {
    int vx, vy, vw, vh;
    video_area(&vx, &vy, &vw, &vh);
    bwin_fill_rect(&win, vx, vy, vw, vh, C_BG);
    if (g_shown.px) {
        int x = vx + (vw - g_shown.w) / 2, y = vy + (vh - g_shown.h) / 2;
        bwin_blit(&win, x, y, g_shown.px, g_shown.w, g_shown.h);
    }
    if (g_vidx < 0 || g_vidx == g_pic_idx) {
        /* music: the title, the artist and the sound's level as bars */
        int ty = g_shown.px ? vy + vh - 70 : vy + vh / 2 - 40;
        if (g_shown.px) bwin_fill_rect(&win, vx, ty - 8, vw, 70, 0x101418u);
        int tw = banana_font_width(BANANA_FONT_SANS_BOLD, 22, g_title);
        bwin_font(&win, vx + (vw - tw) / 2, ty, BANANA_FONT_SANS_BOLD, 22, g_title, C_TEXT);
        if (g_artist[0]) {
            int aw = banana_font_width(BANANA_FONT_SANS, 16, g_artist);
            bwin_font(&win, vx + (vw - aw) / 2, ty + 30, BANANA_FONT_SANS, 16, g_artist, C_DIM);
        }
        if (!g_shown.px) {
            int bars = 24, bw = 10, gap = 4, total = bars * (bw + gap);
            int bx = vx + (vw - total) / 2, by = ty - 30;
            unsigned int t = banana_ticks();
            for (int i = 0; i < bars; i++) {
                double lv = g_paused ? 0.05 : g_level * (0.55 + 0.45 * __builtin_sin(t / 180.0 + i * 0.7));
                int h = (int)(lv * 120);
                if (h < 3) h = 3;
                if (h > 120) h = 120;
                bwin_fill_rect(&win, bx + i * (bw + gap), by - h, bw, h, C_ACCENT);
            }
        }
    }
    if (g_msg[0]) {
        int mw = banana_font_width(BANANA_FONT_SANS, 15, g_msg);
        bwin_font(&win, vx + (vw - mw) / 2, vy + vh / 2 - 8, BANANA_FONT_SANS, 15, g_msg, C_TEXT);
    }
}

static void draw_controls(void) {
    int y0 = win.h - BAR_H;
    bwin_fill_rect(&win, 0, y0, win.w, BAR_H, C_BAR);
    /* play / pause */
    int px = 18, py = y0 + 14;
    if (g_paused) {
        for (int i = 0; i < 20; i++) bwin_line(&win, px, py + i, px + 16, py + 10, C_TEXT);
    } else {
        bwin_fill_rect(&win, px, py, 6, 20, C_TEXT);
        bwin_fill_rect(&win, px + 10, py, 6, 20, C_TEXT);
    }
    /* the seek bar */
    int sx, sy, sw;
    seek_bar(&sx, &sy, &sw);
    double t = now_clock();
    bwin_fill_rect(&win, sx, sy, sw, 6, C_TRACK);
    if (g_duration > 0) {
        int fill = (int)(sw * (t / g_duration));
        if (fill > sw) fill = sw;
        if (fill < 0) fill = 0;
        bwin_fill_rect(&win, sx, sy, fill, 6, C_ACCENT);
        bwin_fill_circle(&win, sx + fill, sy + 3, 7, C_TEXT);
    }
    char a[24], b[24], line[64];
    fmt_time(a, sizeof(a), t);
    fmt_time(b, sizeof(b), g_duration);
    snprintf(line, sizeof(line), g_duration > 0 ? "%s / %s" : "%s", a, b);
    bwin_font(&win, sx, sy + 16, BANANA_FONT_SANS, 13, line, C_DIM);
    int vol = banana_volume(-1);
    char vl[32];
    if (vol >= 0) snprintf(vl, sizeof(vl), "Volume %d%%", vol); else vl[0] = 0;
    int ww = banana_font_width(BANANA_FONT_SANS, 13, vl);
    bwin_font(&win, sx + sw - ww, sy + 16, BANANA_FONT_SANS, 13, vl, C_DIM);
    char nm[80];
    snprintf(nm, sizeof(nm), "%.60s", g_title);
    int nw = banana_font_width(BANANA_FONT_SANS, 13, nm);
    bwin_font(&win, sx + (sw - nw) / 2, sy + 16, BANANA_FONT_SANS, 13, nm, C_TEXT);
}

static void request_seek(double to) {
    banana_lock(&g_lock);
    g_ended = 0;
    g_seek_to = to;
    g_seek_req = 1;
    g_rcount = 0;
    if (g_paused) g_paused_at = to < 0 ? 0 : to;
    banana_cond_broadcast(&g_cond);
    banana_unlock(&g_lock);
}

static void toggle_pause(void) {
    if (g_ended) {                         /* at the end: from the start */
        g_ended = 0;
        g_paused = 0;
        request_seek(0);
        return;
    }
    if (!g_paused) {
        g_paused_at = now_clock();
        g_paused = 1;
        banana_audio_stop();               /* what was queued is played again on resume */
    } else {
        g_paused = 0;
        request_seek(g_paused_at);
    }
    banana_lock(&g_lock);
    banana_cond_broadcast(&g_cond);
    banana_unlock(&g_lock);
}

static void tell_size(void) {
    int vx, vy, vw, vh;
    video_area(&vx, &vy, &vw, &vh);
    banana_lock(&g_lock);
    g_tw = vw > 16 ? vw : 16;
    g_th = vh > 16 ? vh : 16;
    banana_unlock(&g_lock);
}

/* the next picture whose time has come (later ones wait), into g_shown */
static int take_due_picture(void) {
    int got = 0;
    double t = now_clock();
    banana_lock(&g_lock);
    while (g_rcount) {
        frame_t* fr = &g_ring[g_rhead];
        if (fr->pts > t + 0.005) break;
        /* a later one is due too: this one is skipped */
        if (g_rcount > 1 && g_ring[(g_rhead + 1) % RING].pts <= t) {
            g_rhead = (g_rhead + 1) % RING;
            g_rcount--;
            continue;
        }
        if (!g_shown.px || g_shown.w * g_shown.h < fr->w * fr->h) {
            free(g_shown.px);
            g_shown.px = malloc((size_t)fr->w * fr->h * 4);
        }
        if (g_shown.px) {
            memcpy(g_shown.px, fr->px, (size_t)fr->w * fr->h * 4);
            g_shown.w = fr->w;
            g_shown.h = fr->h;
            got = 1;
        }
        g_rhead = (g_rhead + 1) % RING;
        g_rcount--;
        banana_cond_broadcast(&g_cond);
        break;
    }
    banana_unlock(&g_lock);
    return got;
}

/* ── picking a file (no file on the command line) ─────────────────── */
static const char* const MEDIA_EXT[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "mpg", "mpeg", "ts", "m2ts", "ogv", "flv", "wmv", "asf", "3gp",
    "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wav", "wma", "ac3", NULL
};

static int is_media(const char* name) {
    const char* d = strrchr(name, '.');
    if (!d) return 0;
    for (int i = 0; MEDIA_EXT[i]; i++) if (strcasecmp(d + 1, MEDIA_EXT[i]) == 0) return 1;
    return 0;
}

#define PICK_MAX 256
static banana_dirent_t g_ents[PICK_MAX];
static int  g_nents, g_pick_scroll;
static char g_dir[256] = "/home/banana";

static void pick_scan(void) {
    g_nents = 0;
    g_pick_scroll = 0;
    banana_dirent_t e;
    if (strcmp(g_dir, "/") != 0) {
        memset(&g_ents[0], 0, sizeof(g_ents[0]));
        strcpy(g_ents[0].name, "..");
        g_ents[0].is_dir = 1;
        g_nents = 1;
    }
    for (int i = 0; g_nents < PICK_MAX && __banana->readdir(g_dir, i, &e) == 0; i++)
        if (e.is_dir || is_media(e.name)) g_ents[g_nents++] = e;
}

static void pick_draw(void) {
    bwin_fill_rect(&win, 0, 0, win.w, win.h, 0x14181Eu);
    bwin_fill_rect(&win, 0, 0, win.w, 44, C_BAR);
    bwin_font(&win, 16, 12, BANANA_FONT_SANS_BOLD, 17, "Open a video or a song", C_TEXT);
    bwin_font(&win, 16 + banana_font_width(BANANA_FONT_SANS_BOLD, 17, "Open a video or a song") + 16, 14,
              BANANA_FONT_SANS, 14, g_dir, C_DIM);
    int rows = (win.h - 52) / 26;
    for (int i = 0; i < rows && g_pick_scroll + i < g_nents; i++) {
        banana_dirent_t* e = &g_ents[g_pick_scroll + i];
        int y = 52 + i * 26;
        bwin_fill_rect(&win, 16, y + 6, 14, 12, e->is_dir ? 0xE8C25Au : C_ACCENT);
        bwin_font(&win, 40, y + 4, BANANA_FONT_SANS, 15, e->name, C_TEXT);
    }
    if (!g_nents || (g_nents == 1 && g_ents[0].name[0] == '.'))
        bwin_font(&win, 40, 60 + 26, BANANA_FONT_SANS, 14, "(no videos or music here)", C_DIM);
}

/* 1 when a file was chosen (into path) */
static int pick_click(int x, int y, char* path, int cap) {
    (void)x;
    if (y < 52) return 0;
    int i = g_pick_scroll + (y - 52) / 26;
    if (i < 0 || i >= g_nents) return 0;
    banana_dirent_t* e = &g_ents[i];
    if (strcmp(e->name, "..") == 0) {
        char* s = strrchr(g_dir, '/');
        if (s && s != g_dir) *s = 0; else strcpy(g_dir, "/");
        pick_scan();
        return 0;
    }
    char p[256];
    snprintf(p, sizeof(p), "%s%s%s", g_dir, strcmp(g_dir, "/") ? "/" : "", e->name);
    if (e->is_dir) { snprintf(g_dir, sizeof(g_dir), "%s", p); pick_scan(); return 0; }
    snprintf(path, cap, "%s", p);
    return 1;
}

static int pick_file(char* path, int cap) {
    pick_scan();
    for (;;) {
        pick_draw();
        bwin_update(&win);
        banana_event_t ev;
        if (!bwin_wait_event(&win, &ev, -1)) continue;
        if (ev.type == BANANA_EV_CLOSE) return 0;
        if (ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1 && pick_click(ev.x, ev.y, path, cap)) return 1;
        if (ev.type == BANANA_EV_KEY) {
            int rows = (win.h - 52) / 26;
            if (ev.key == BANANA_KEY_DOWN || ev.key == BANANA_KEY_PGDN) {
                g_pick_scroll += ev.key == BANANA_KEY_PGDN ? rows : 3;
                if (g_pick_scroll > g_nents - rows) g_pick_scroll = g_nents - rows;
                if (g_pick_scroll < 0) g_pick_scroll = 0;
            } else if (ev.key == BANANA_KEY_UP || ev.key == BANANA_KEY_PGUP) {
                g_pick_scroll -= ev.key == BANANA_KEY_PGUP ? rows : 3;
                if (g_pick_scroll < 0) g_pick_scroll = 0;
            } else if (ev.key == 27) return 0;
        }
    }
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);       /* (no notices in the terminal) */
    if (bwin_open(&win, "Media Player", WIN_W, WIN_H) < 0) {
        printf("mediaplayer: the desktop is not running (startx)\n");
        return 1;
    }
    bwin_resizable(&win, 360, 240);

    char path[256];
    if (argc > 1) snprintf(path, sizeof(path), "%s", argv[1]);
    else if (!pick_file(path, sizeof(path))) { bwin_close(&win); return 0; }

    char err[128] = "";
    bwin_fill_rect(&win, 0, 0, win.w, win.h, C_BG);
    bwin_font(&win, 20, 20, BANANA_FONT_SANS, 15, "Opening...", C_TEXT);
    bwin_update(&win);
    if (open_media(path, err, sizeof(err)) < 0) {
        char m[300];
        snprintf(m, sizeof(m), "Cannot play %s: %s", base_name(path), err);
        printf("mediaplayer: %s\n", m);
        bwin_fill_rect(&win, 0, 0, win.w, win.h, C_BG);
        bwin_font(&win, 20, 20, BANANA_FONT_SANS, 15, m, C_TEXT);
        bwin_update(&win);
        banana_event_t ev;
        for (;;)
            if (bwin_wait_event(&win, &ev, -1) && (ev.type == BANANA_EV_CLOSE || ev.type == BANANA_EV_KEY)) break;
        bwin_close(&win);
        return 1;
    }
    char wt[200];
    snprintf(wt, sizeof(wt), "%s - Media Player", g_title);
    bwin_title(&win, wt);

    tell_size();
    reset_clock(0);
    int th = banana_thread(decoder_thread, NULL);
    if (th < 0) { printf("mediaplayer: no thread\n"); return 1; }

    unsigned int last_ui = 0;
    int dirty = 1;
    for (;;) {
        banana_event_t ev;
        int wait = 8;
        while (bwin_wait_event(&win, &ev, wait)) {
            wait = 0;
            if (ev.type == BANANA_EV_CLOSE) goto out;
            if (ev.type == BANANA_EV_RESIZE) { tell_size(); dirty = 1; continue; }
            int sx, sy, sw;
            seek_bar(&sx, &sy, &sw);
            if (ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1) {
                if (ev.y >= win.h - BAR_H && ev.x < 52) toggle_pause();
                else if (ev.y >= sy - 10 && ev.y <= sy + 14 && ev.x >= sx - 6 && ev.x <= sx + sw + 6 && g_duration > 0) {
                    g_dragging = 1;
                    request_seek(g_duration * (ev.x - sx) / (double)sw);
                } else if (ev.y < win.h - BAR_H) toggle_pause();
                dirty = 1;
            } else if (ev.type == BANANA_EV_MOUSE_MOVE && g_dragging && (ev.buttons & 1)) {
                /* (the seek lands when the button is released) */
            } else if (ev.type == BANANA_EV_MOUSE_UP && g_dragging) {
                g_dragging = 0;
                if (g_duration > 0) request_seek(g_duration * (ev.x - sx) / (double)sw);
            } else if (ev.type == BANANA_EV_KEY) {
                double t = now_clock();
                switch (ev.key) {
                case ' ': case 'k': case BANANA_KEY_PLAY: toggle_pause(); break;
                case BANANA_KEY_LEFT:  request_seek(t - 10); break;
                case BANANA_KEY_RIGHT: request_seek(t + 10); break;
                case BANANA_KEY_PGDN:  request_seek(t - 60); break;
                case BANANA_KEY_PGUP:  request_seek(t + 60); break;
                case BANANA_KEY_HOME:  request_seek(0); break;
                case BANANA_KEY_UP:    banana_volume(banana_volume(-1) + 5); break;
                case BANANA_KEY_DOWN:  { int v = banana_volume(-1) - 5; banana_volume(v < 0 ? 0 : v); break; }
                case BANANA_KEY_STOP:  if (!g_paused) toggle_pause(); request_seek(0); break;
                case 27: case 'q': goto out;
                default: break;
                }
                dirty = 1;
            }
        }
        /* a sound card that stopped playing (nothing leaves its queue): the
         * wall clock from here on, and no more sound */
        if (g_has_audio_out && !g_paused && !g_audio_dead) {
            static unsigned int last_q, stall_since;
            unsigned int q = banana_audio_queued_ms(), t = banana_ticks();
            if (q > 0 && q == last_q) {
                if (!stall_since) stall_since = t;
                else if (t - stall_since > 5000) {
                    double c = now_clock();
                    g_audio_dead = 1;
                    g_has_audio_out = 0;
                    g_wall_base = c;
                    g_wall_start = t;
                    banana_audio_stop();
                }
            } else stall_since = 0;
            last_q = q;
        }
        if (take_due_picture()) dirty = 1;
        /* the end: everything shown and heard */
        if (g_eof && !g_rcount && !g_paused && !g_seek_req && (!g_has_audio_out || banana_audio_queued_ms() == 0)) {
            g_paused_at = g_duration > 0 ? g_duration : now_clock();
            g_paused = 1;
            g_ended = 1;
            dirty = 1;
        }
        unsigned int now = banana_ticks();
        int music = g_vidx < 0 || g_vidx == g_pic_idx;
        if (dirty || now - last_ui >= (music ? 60u : 250u)) {
            draw_picture();
            draw_controls();
            bwin_update(&win);
            last_ui = now;
            dirty = 0;
        }
    }
out:
    banana_lock(&g_lock);
    g_quit = 1;
    banana_cond_broadcast(&g_cond);
    banana_unlock(&g_lock);
    banana_join(th);
    banana_audio_stop();
    bwin_close(&win);
    return 0;
}
