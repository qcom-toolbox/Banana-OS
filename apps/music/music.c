/* Music - a music library player for Banana OS, after Amethyst
 * (github.com/Geoxor/Amethyst): the songs of your folders with their
 * tags and covers, albums and artists, a queue with shuffle and repeat,
 * gapless playback, an equalizer and a spectrum visualizer. Decoding is
 * FFmpeg's (ports/ffmpeg, the "audio" build): MP3, FLAC, AAC/M4A, OGG
 * Vorbis, Opus, WAV, WMA, AC-3, ALAC.
 *
 *     music [file...]       (files: played right away)
 *
 * The library is ~/Music, ~/Downloads, the home folder and USB sticks
 * (/mnt), remembered in ~/.config/music/ with the liked songs and the
 * settings.
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define WIN_W     920
#define WIN_H     580
#define SIDE_W    200
#define TOP_H     52
#define BAR_H     84
#define ROW_H     30
#define RATE      48000
#define CONF_DIR  "/home/banana/.config/music"

/* colors: a dark theme with an amethyst accent */
#define C_BG      0x14161Cu
#define C_SIDE    0x0F1116u
#define C_BAR     0x1B1E26u
#define C_ROW     0x1A1D24u
#define C_SEL     0x2E2A40u
#define C_HOVER   0x22252Eu
#define C_TEXT    0xECEEF4u
#define C_DIM     0x8C93A4u
#define C_FAINT   0x4A5060u
#define C_ACCENT  0x9B6CF0u          /* amethyst */
#define C_ACCENT2 0xC9A8FFu
#define C_HEART   0xF0577Au

#define F_SANS    BANANA_FONT_SANS
#define F_BOLD    BANANA_FONT_SANS_BOLD

static bwin_t win;

/* ── the library ──────────────────────────────────────────────────── */
typedef struct {
    char     path[200];
    char     title[96];
    char     artist[64];
    char     album[64];
    int      track;
    int      dur;               /* seconds */
    unsigned size;              /* bytes (the cache's check) */
    int      liked;
    int      seen;              /* found by the latest scan */
    int      gone;              /* its file is no more (kept: the queue holds indices) */
} song_t;

#define MAX_SONGS 8192

static song_t* g_songs;
static volatile int g_nsongs;
static banana_mutex_t g_lib_lock = BANANA_MUTEX_INIT;
static volatile int g_scanning;
static volatile int g_lib_gen;             /* bumps when songs change */
static char    g_scan_status[96];

static void lib_add(const song_t* s) {
    if (!g_songs) g_songs = calloc(MAX_SONGS, sizeof(song_t));
    if (!g_songs || g_nsongs == MAX_SONGS) return;
    g_songs[g_nsongs++] = *s;
}

static int lib_find(const char* path) {
    for (int i = 0; i < g_nsongs; i++) if (strcmp(g_songs[i].path, path) == 0) return i;
    return -1;
}

static const char* const AUDIO_EXT[] = { "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wav", "wma", "ac3", "alac", NULL };

static int is_audio(const char* name) {
    const char* d = strrchr(name, '.');
    if (!d) return 0;
    for (int i = 0; AUDIO_EXT[i]; i++) if (strcasecmp(d + 1, AUDIO_EXT[i]) == 0) return 1;
    return 0;
}

static const char* base_name(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void copy_tag(char* dst, int cap, AVDictionary* m, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(m, key, NULL, 0);
    if (e && e->value[0]) snprintf(dst, cap, "%s", e->value);
}

/* the tags and the length of one file (0, or -1 if FFmpeg cannot read it) */
static int probe(const char* path, song_t* s) {
    AVFormatContext* f = NULL;
    if (avformat_open_input(&f, path, NULL, NULL) < 0) return -1;
    if (f->duration <= 0) avformat_find_stream_info(f, NULL);
    memset(s->title, 0, sizeof(s->title));
    memset(s->artist, 0, sizeof(s->artist));
    memset(s->album, 0, sizeof(s->album));
    copy_tag(s->title, sizeof(s->title), f->metadata, "title");
    copy_tag(s->artist, sizeof(s->artist), f->metadata, "artist");
    if (!s->artist[0]) copy_tag(s->artist, sizeof(s->artist), f->metadata, "album_artist");
    copy_tag(s->album, sizeof(s->album), f->metadata, "album");
    /* Ogg / Opus keep their tags on the stream */
    for (unsigned i = 0; i < f->nb_streams && !s->title[0]; i++) {
        copy_tag(s->title, sizeof(s->title), f->streams[i]->metadata, "title");
        copy_tag(s->artist, sizeof(s->artist), f->streams[i]->metadata, "artist");
        copy_tag(s->album, sizeof(s->album), f->streams[i]->metadata, "album");
    }
    const AVDictionaryEntry* t = av_dict_get(f->metadata, "track", NULL, 0);
    s->track = t ? atoi(t->value) : 0;
    s->dur = f->duration > 0 ? (int)(f->duration / AV_TIME_BASE) : 0;
    if (!s->title[0]) {                         /* the file's name, without its extension */
        snprintf(s->title, sizeof(s->title), "%s", base_name(path));
        char* d = strrchr(s->title, '.');
        if (d) *d = 0;
    }
    if (!s->artist[0]) snprintf(s->artist, sizeof(s->artist), "Unknown artist");
    if (!s->album[0]) snprintf(s->album, sizeof(s->album), "Unknown album");
    avformat_close_input(&f);
    return 0;
}

/* ── the cache in ~/.config/music ─────────────────────────────────── */
static void conf_dir(void) {
    mkdir("/home/banana/.config", 0755);
    mkdir(CONF_DIR, 0755);
}

/* fields are separated by tabs (which tags never hold here: they are cut) */
static void clean(char* s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static void lib_save(void) {
    conf_dir();
    FILE* f = fopen(CONF_DIR "/library.tsv", "w");
    if (!f) return;
    banana_lock(&g_lib_lock);
    for (int i = 0; i < g_nsongs; i++) {
        song_t* s = &g_songs[i];
        if (s->gone) continue;
        clean(s->title); clean(s->artist); clean(s->album);
        fprintf(f, "%s\t%u\t%d\t%d\t%s\t%s\t%s\n", s->path, s->size, s->dur, s->track, s->title, s->artist, s->album);
    }
    banana_unlock(&g_lib_lock);
    fclose(f);
    f = fopen(CONF_DIR "/liked.txt", "w");
    if (!f) return;
    for (int i = 0; i < g_nsongs; i++) if (g_songs[i].liked) fprintf(f, "%s\n", g_songs[i].path);
    fclose(f);
}

static char* next_field(char** p) {
    char* s = *p;
    if (!s) return "";
    char* t = strchr(s, '\t');
    if (t) { *t = 0; *p = t + 1; } else *p = NULL;
    return s;
}

static void lib_load(void) {
    FILE* f = fopen(CONF_DIR "/library.tsv", "r");
    char line[600];
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char* p = line;
            song_t s;
            memset(&s, 0, sizeof(s));
            snprintf(s.path, sizeof(s.path), "%s", next_field(&p));
            s.size = (unsigned)strtoul(next_field(&p), NULL, 10);
            s.dur = atoi(next_field(&p));
            s.track = atoi(next_field(&p));
            snprintf(s.title, sizeof(s.title), "%s", next_field(&p));
            snprintf(s.artist, sizeof(s.artist), "%s", next_field(&p));
            snprintf(s.album, sizeof(s.album), "%s", next_field(&p));
            if (s.path[0]) lib_add(&s);
        }
        fclose(f);
    }
    f = fopen(CONF_DIR "/liked.txt", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            int i = lib_find(line);
            if (i >= 0) g_songs[i].liked = 1;
        }
        fclose(f);
    }
}

/* ── scanning the folders (a thread of its own) ───────────────────── */
static int g_scan_found;

static void scan_dir(const char* dir, int depth) {
    banana_dirent_t e;
    for (int i = 0; __banana->readdir(dir, i, &e) == 0; i++) {
        if (e.name[0] == '.') continue;
        char p[256];
        snprintf(p, sizeof(p), "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e.name);
        if (e.is_dir) {
            if (depth < 6 && strcmp(p, "/home/banana/Examples") != 0) scan_dir(p, depth + 1);
            continue;
        }
        if (!is_audio(e.name) || strlen(p) >= sizeof(g_songs[0].path)) continue;
        banana_lock(&g_lib_lock);
        int k = lib_find(p);
        if (k >= 0 && g_songs[k].size == e.size) { g_songs[k].seen = 1; banana_unlock(&g_lib_lock); continue; }
        banana_unlock(&g_lib_lock);
        song_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.path, sizeof(s.path), "%s", p);
        snprintf(g_scan_status, sizeof(g_scan_status), "Reading %s", e.name);
        if (probe(p, &s) < 0) continue;
        s.size = e.size;
        s.seen = 1;
        banana_lock(&g_lib_lock);
        k = lib_find(p);
        if (k >= 0) { s.liked = g_songs[k].liked; g_songs[k] = s; }
        else lib_add(&s);
        g_lib_gen++;
        banana_unlock(&g_lib_lock);
        g_scan_found++;
    }
}

static int scan_thread(void* arg) {
    (void)arg;
    g_scanning = 1;
    g_scan_found = 0;
    for (int i = 0; i < g_nsongs; i++) g_songs[i].seen = 0;
    mkdir("/home/banana/Music", 0755);
    scan_dir("/home/banana/Music", 0);
    scan_dir("/home/banana/Downloads", 0);
    /* the home folder itself, without going into its other folders again */
    banana_dirent_t e;
    for (int i = 0; __banana->readdir("/home/banana", i, &e) == 0; i++) {
        if (e.is_dir || !is_audio(e.name)) continue;
        char p[256];
        snprintf(p, sizeof(p), "/home/banana/%s", e.name);
        banana_lock(&g_lib_lock);
        int k = lib_find(p);
        if (k >= 0 && g_songs[k].size == e.size) { g_songs[k].seen = 1; banana_unlock(&g_lib_lock); continue; }
        banana_unlock(&g_lib_lock);
        song_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.path, sizeof(s.path), "%s", p);
        if (probe(p, &s) < 0) continue;
        s.size = e.size;
        s.seen = 1;
        banana_lock(&g_lib_lock);
        if (k >= 0) { s.liked = g_songs[k].liked; g_songs[k] = s; } else lib_add(&s);
        g_lib_gen++;
        banana_unlock(&g_lib_lock);
    }
    scan_dir("/mnt", 0);
    /* songs whose file is gone leave the library */
    banana_lock(&g_lib_lock);
    for (int i = 0; i < g_nsongs; i++) {
        int gone = !g_songs[i].seen;
        if (gone != g_songs[i].gone) { g_songs[i].gone = gone; g_lib_gen++; }
    }
    banana_unlock(&g_lib_lock);
    lib_save();
    g_scan_status[0] = 0;
    g_scanning = 0;
    g_lib_gen++;
    return 0;
}

/* ── the queue ────────────────────────────────────────────────────── */
enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE };
static int  g_queue[4096];               /* song indices, in play order */
static int  g_qlen, g_qpos = -1;
static int  g_shuffle, g_repeat;
static banana_mutex_t g_q_lock = BANANA_MUTEX_INIT;

/* what comes after the current one (-1: the end); step < 0: before it */
static int queue_step(int step, int manual) {
    if (g_qlen == 0) return -1;
    if (g_repeat == REPEAT_ONE && !manual) return g_qpos;
    int p = g_qpos + step;
    if (p >= g_qlen) return g_repeat == REPEAT_ALL || manual ? 0 : -1;
    if (p < 0) return g_repeat == REPEAT_ALL ? g_qlen - 1 : 0;
    return p;
}

static void shuffle_queue(void) {
    if (g_qlen < 2) return;
    int cur = g_qpos >= 0 ? g_queue[g_qpos] : -1;
    for (int i = g_qlen - 1; i > 0; i--) {
        int j = (int)(banana_random() % (unsigned)(i + 1));
        int t = g_queue[i]; g_queue[i] = g_queue[j]; g_queue[j] = t;
    }
    if (cur >= 0)                        /* the current song stays first */
        for (int i = 0; i < g_qlen; i++) if (g_queue[i] == cur) { g_queue[i] = g_queue[0]; g_queue[0] = cur; g_qpos = 0; break; }
}

/* ── the equalizer: peaking biquads (RBJ cookbook), per channel ───── */
#define EQ_BANDS 6
static const double EQ_FREQ[EQ_BANDS] = { 60, 170, 600, 3000, 6000, 14000 };
static const char* const EQ_LABEL[EQ_BANDS] = { "60", "170", "600", "3k", "6k", "14k" };
static volatile int g_eq_db[EQ_BANDS];          /* -12..12 */
static volatile int g_eq_gen;
typedef struct { double b0, b1, b2, a1, a2; double z1[2], z2[2]; } biquad_t;
static biquad_t g_bq[EQ_BANDS];
static int g_bq_gen = -1;

static void eq_design(void) {
    for (int b = 0; b < EQ_BANDS; b++) {
        double A = pow(10.0, g_eq_db[b] / 40.0), w0 = 2 * M_PI * EQ_FREQ[b] / RATE;
        double alpha = sin(w0) / (2 * 1.0), c = cos(w0);
        double a0 = 1 + alpha / A;
        biquad_t* q = &g_bq[b];
        q->b0 = (1 + alpha * A) / a0;
        q->b1 = (-2 * c) / a0;
        q->b2 = (1 - alpha * A) / a0;
        q->a1 = (-2 * c) / a0;
        q->a2 = (1 - alpha / A) / a0;
    }
    g_bq_gen = g_eq_gen;
}

static void eq_process(float* x, int frames) {
    if (g_bq_gen != g_eq_gen) eq_design();
    for (int b = 0; b < EQ_BANDS; b++) {
        if (!g_eq_db[b]) continue;
        biquad_t* q = &g_bq[b];
        for (int ch = 0; ch < 2; ch++) {
            double z1 = q->z1[ch], z2 = q->z2[ch];
            for (int i = 0; i < frames; i++) {             /* transposed direct form II */
                double in = x[i * 2 + ch];
                double out = q->b0 * in + z1;
                z1 = q->b1 * in - q->a1 * out + z2;
                z2 = q->b2 * in - q->a2 * out;
                x[i * 2 + ch] = (float)out;
            }
            q->z1[ch] = z1;
            q->z2[ch] = z2;
        }
    }
}

/* ── the player engine (a thread) ─────────────────────────────────── */
/* What was sent to the sound card is counted in samples: the song heard
 * now is the one whose start the card's play position has passed. */
#define VIZ_RING 131072                       /* mono samples, for the visualizer */
static float g_viz[VIZ_RING];
static volatile unsigned long long g_sent;   /* samples sent to the card since the start */

typedef struct { unsigned long long at; int song; double offset; } mark_t;   /* song starts at sample `at` (offset seconds in) */
static mark_t g_marks[8];
static int    g_nmarks;

static banana_mutex_t g_e_lock = BANANA_MUTEX_INIT;
static banana_cond_t  g_e_cond = BANANA_COND_INIT;
enum { E_IDLE = 0, E_PLAY };
static volatile int g_cmd;                    /* E_PLAY: (re)start g_cmd_song at g_cmd_pos */
static volatile int g_cmd_song = -1;
static volatile double g_cmd_pos;
static volatile int g_playing;                /* the engine feeds the card */
static volatile int g_paused;
static double g_paused_pos;
static int    g_paused_song = -1;
static volatile int g_quit;
static volatile int g_no_card;

static unsigned long long played_samples(void) {
    unsigned long long q = (unsigned long long)banana_audio_queued_ms() * RATE / 1000;
    return g_sent > q ? g_sent - q : 0;
}

/* the song heard now and where in it (seconds); -1 if none */
static int now_playing(double* pos) {
    if (g_paused) { if (pos) *pos = g_paused_pos; return g_paused_song; }
    banana_lock(&g_e_lock);
    unsigned long long p = played_samples();
    int song = -1;
    double at = 0;
    for (int i = 0; i < g_nmarks; i++)
        if (g_marks[i].at <= p) { song = g_marks[i].song; at = g_marks[i].offset + (double)(p - g_marks[i].at) / RATE; }
    banana_unlock(&g_e_lock);
    if (pos) *pos = at;
    return song;
}

static void add_mark(int song, double offset) {
    banana_lock(&g_e_lock);
    unsigned long long p = played_samples();
    /* the marks the card has gone past, except the latest of them, go */
    int keep = 0;
    for (int i = 0; i < g_nmarks; i++) {
        int later_passed = 0;
        for (int j = 0; j < g_nmarks; j++) if (g_marks[j].at > g_marks[i].at && g_marks[j].at <= p) later_passed = 1;
        if (!later_passed) g_marks[keep++] = g_marks[i];
    }
    g_nmarks = keep;
    if (g_nmarks == 8) { memmove(g_marks, g_marks + 1, 7 * sizeof(mark_t)); g_nmarks = 7; }
    g_marks[g_nmarks++] = (mark_t){ g_sent, song, offset };
    banana_unlock(&g_e_lock);
}

typedef struct {
    AVFormatContext* fmt;
    AVCodecContext*  dec;
    SwrContext*      swr;
    int              idx;
} track_t;

static void track_close(track_t* t) {
    if (t->swr) swr_free(&t->swr);
    if (t->dec) avcodec_free_context(&t->dec);
    if (t->fmt) avformat_close_input(&t->fmt);
    memset(t, 0, sizeof(*t));
}

static int track_open(track_t* t, const char* path, double pos) {
    memset(t, 0, sizeof(*t));
    if (avformat_open_input(&t->fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(t->fmt, NULL) < 0) { track_close(t); return -1; }
    t->idx = av_find_best_stream(t->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (t->idx < 0) { track_close(t); return -1; }
    AVStream* st = t->fmt->streams[t->idx];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c || !(t->dec = avcodec_alloc_context3(c)) || avcodec_parameters_to_context(t->dec, st->codecpar) < 0 ||
        avcodec_open2(t->dec, c, NULL) < 0) { track_close(t); return -1; }
    t->dec->pkt_timebase = st->time_base;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&t->swr, &stereo, AV_SAMPLE_FMT_S16, RATE, &t->dec->ch_layout, t->dec->sample_fmt,
                            t->dec->sample_rate, 0, NULL) < 0 || (av_opt_set_sample_fmt(t->swr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0), swr_init(t->swr)) < 0) { track_close(t); return -1; }
    if (pos > 0.5) {
        int64_t ts = (int64_t)(pos * AV_TIME_BASE);
        avformat_seek_file(t->fmt, -1, INT64_MIN, ts, ts, 0);
        avcodec_flush_buffers(t->dec);
    }
    return 0;
}

static float*   g_fbuf;
static int16_t* g_sbuf;
static int      g_bufcap;

/* converted, through the equalizer, to the card; 0, or -1 to stop */
static int send_frame(track_t* t, AVFrame* f, double* skip) {
    int out = swr_get_out_samples(t->swr, f->nb_samples);
    if (out > g_bufcap) {
        float* a = realloc(g_fbuf, (size_t)out * 8);
        int16_t* b = realloc(g_sbuf, (size_t)out * 4);
        if (!a || !b) return -1;
        g_fbuf = a; g_sbuf = b; g_bufcap = out;
    }
    uint8_t* dst[1] = { (uint8_t*)g_sbuf };
    int n = swr_convert(t->swr, dst, out, (const uint8_t**)f->extended_data, f->nb_samples);
    for (int i = 0; i < n * 2 && n > 0; i++) g_fbuf[i] = g_sbuf[i] * (1.0f / 32768.0f);   /* (the equalizer works in float) */
    if (n <= 0) return 0;
    int from = 0;
    /* a seek lands on a packet before the target: the rest is cut */
    double pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp * av_q2d(t->fmt->streams[t->idx]->time_base) : -1;
    if (*skip > 0 && pts >= 0) {
        if (pts + (double)n / RATE <= *skip) return 0;
        if (pts < *skip) from = (int)((*skip - pts) * RATE);
        if (from > n) from = n;
        *skip = 0;
    }
    float* x = g_fbuf + from * 2;
    int frames = n - from;
    eq_process(x, frames);
    for (int i = 0; i < frames; i++) {
        float l = x[i * 2], r = x[i * 2 + 1];
        g_viz[(g_sent + (unsigned long long)i) % VIZ_RING] = (l + r) * 0.5f;
        int a = (int)(l * 32767.0f), b = (int)(r * 32767.0f);
        g_sbuf[i * 2] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
        g_sbuf[i * 2 + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
    }
    if (g_no_card) {                       /* no sound card: the time goes by anyway */
        banana_sleep((unsigned)(frames * 1000 / RATE));
    } else if (banana_play(g_sbuf, (unsigned long)frames * 4, RATE, 2, 16) < 0) {
        g_no_card = 1;
    }
    g_sent += (unsigned long long)frames;
    return 0;
}

static int engine_thread(void* arg) {
    (void)arg;
    track_t t;
    memset(&t, 0, sizeof(t));
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    double skip = 0;
    int cur_song = -1;
    while (!g_quit) {
        if (g_cmd == E_PLAY) {                 /* a new song or a seek: from a quiet card */
            banana_lock(&g_e_lock);
            int song = g_cmd_song;
            double pos = g_cmd_pos;
            g_cmd = E_IDLE;
            banana_unlock(&g_e_lock);
            banana_audio_stop();
            track_close(&t);
            banana_lock(&g_e_lock);
            g_nmarks = 0;
            g_sent = 0;
            banana_unlock(&g_e_lock);
            memset(g_bq, 0, sizeof(g_bq));
            g_bq_gen = -1;
            cur_song = -1;
            g_playing = 0;
            if (song >= 0 && song < g_nsongs && track_open(&t, g_songs[song].path, pos) == 0) {
                cur_song = song;
                skip = pos;
                add_mark(song, pos);
                g_playing = 1;
            }
            continue;
        }
        if (!g_playing || cur_song < 0) {
            banana_lock(&g_e_lock);
            if (g_cmd != E_PLAY && !g_quit) banana_cond_timedwait(&g_e_cond, &g_e_lock, 200);
            banana_unlock(&g_e_lock);
            continue;
        }
        int r = av_read_frame(t.fmt, pkt);
        if (r >= 0) {
            if (pkt->stream_index == t.idx && avcodec_send_packet(t.dec, pkt) == 0)
                while (avcodec_receive_frame(t.dec, fr) == 0) {
                    send_frame(&t, fr, &skip);
                    av_frame_unref(fr);
                    if (g_cmd == E_PLAY || g_quit) break;
                }
            av_packet_unref(pkt);
            continue;
        }
        /* the end of the song: the next one right after it (gapless) */
        avcodec_send_packet(t.dec, NULL);
        while (avcodec_receive_frame(t.dec, fr) == 0) { send_frame(&t, fr, &skip); av_frame_unref(fr); }
        track_close(&t);
        banana_lock(&g_q_lock);
        int np = queue_step(1, 0);
        int next = np >= 0 ? g_queue[np] : -1;
        if (np >= 0) g_qpos = np;
        banana_unlock(&g_q_lock);
        if (next < 0 || track_open(&t, g_songs[next].path, 0) < 0) { g_playing = 0; cur_song = -1; add_mark(-1, 0); continue; }
        cur_song = next;
        skip = 0;
        add_mark(next, 0);
    }
    track_close(&t);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    return 0;
}

static void engine_play(int song, double pos) {
    banana_lock(&g_e_lock);
    g_cmd_song = song;
    g_cmd_pos = pos < 0 ? 0 : pos;
    g_cmd = E_PLAY;
    g_paused = 0;
    banana_cond_broadcast(&g_e_cond);
    banana_unlock(&g_e_lock);
}

static void engine_pause(void) {
    double pos;
    int s = now_playing(&pos);
    if (s < 0) return;
    g_paused_song = s;
    g_paused_pos = pos;
    g_paused = 1;
    engine_play(-1, 0);                        /* (the card goes quiet) */
    g_paused = 1;
}

/* ── covers (the attached picture of a song), cached by album ─────── */
typedef struct { char key[132]; int size; unsigned int* px; int none; unsigned int used; } cover_t;
#define NCOVERS 48
static cover_t g_covers[NCOVERS];

/* a size x size picture of the song's cover, NULL if it has none */
static unsigned int* cover_of(int song, int size) {
    if (song < 0 || song >= g_nsongs) return NULL;
    char key[132];
    snprintf(key, sizeof(key), "%s|%s", g_songs[song].album, g_songs[song].artist);
    if (!strcmp(g_songs[song].album, "Unknown album")) snprintf(key, sizeof(key), "%s", g_songs[song].path);
    int lru = 0;
    for (int i = 0; i < NCOVERS; i++) {
        if (g_covers[i].key[0] && g_covers[i].size == size && !strcmp(g_covers[i].key, key)) {
            g_covers[i].used = banana_ticks();
            return g_covers[i].none ? NULL : g_covers[i].px;
        }
        if (g_covers[i].used < g_covers[lru].used) lru = i;
    }
    cover_t* c = &g_covers[lru];
    free(c->px);
    memset(c, 0, sizeof(*c));
    snprintf(c->key, sizeof(c->key), "%s", key);
    c->size = size;
    c->used = banana_ticks();
    c->none = 1;
    AVFormatContext* f = NULL;
    if (avformat_open_input(&f, g_songs[song].path, NULL, NULL) < 0) return NULL;
    for (unsigned i = 0; i < f->nb_streams; i++) {
        AVStream* st = f->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || !st->attached_pic.size) continue;
        const AVCodec* cd = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext* d = cd ? avcodec_alloc_context3(cd) : NULL;
        if (d && avcodec_parameters_to_context(d, st->codecpar) >= 0 && avcodec_open2(d, cd, NULL) >= 0) {
            AVFrame* fr = av_frame_alloc();
            if (avcodec_send_packet(d, &st->attached_pic) >= 0 && avcodec_receive_frame(d, fr) >= 0) {
                /* the middle square of the picture */
                int s = fr->width < fr->height ? fr->width : fr->height;
                int ox = (fr->width - s) / 2 & ~1, oy = (fr->height - s) / 2 & ~1;
                struct SwsContext* sw = sws_getContext(s, s, fr->format, size, size, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
                c->px = malloc((size_t)size * size * 4);
                if (sw && c->px) {
                    const uint8_t* src[4];
                    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fr->format);
                    for (int p = 0; p < 4; p++) {
                        int sx = p == 1 || p == 2 ? ox >> desc->log2_chroma_w : ox;
                        int sy = p == 1 || p == 2 ? oy >> desc->log2_chroma_h : oy;
                        src[p] = fr->data[p] ? fr->data[p] + sy * fr->linesize[p] + sx : NULL;   /* (planar 8-bit: JPEG) */
                    }
                    uint8_t* dst[4] = { (uint8_t*)c->px, NULL, NULL, NULL };
                    int dls[4] = { size * 4, 0, 0, 0 };
                    sws_scale(sw, src, fr->linesize, 0, s, dst, dls);
                    c->none = 0;
                }
                if (sw) sws_freeContext(sw);
            }
            av_frame_free(&fr);
        }
        if (d) avcodec_free_context(&d);
        break;
    }
    avformat_close_input(&f);
    if (c->none) { free(c->px); c->px = NULL; }
    return c->px;
}

/* a placeholder: the album's initial on a color of its own */
static void draw_no_cover(int x, int y, int size, const char* album) {
    unsigned h = 5381;
    for (const char* s = album; *s; s++) h = h * 33 + (unsigned char)*s;
    unsigned int col = BANANA_RGB(60 + h % 80, 40 + (h >> 8) % 60, 90 + (h >> 16) % 100);
    bwin_fill_rect(&win, x, y, size, size, col);
    char ini[8] = { album[0] ? album[0] : '?', 0 };
    int fs = size / 2 > 10 ? size / 2 : 10;
    int w = banana_font_width(F_BOLD, fs, ini);
    bwin_font(&win, x + (size - w) / 2, y + size / 2 - fs * 2 / 3, F_BOLD, fs, ini, 0xFFFFFFu);
}

static void draw_cover(int song, int x, int y, int size) {
    unsigned int* px = cover_of(song, size);
    if (px) bwin_blit(&win, x, y, px, size, size);
    else draw_no_cover(x, y, size, song >= 0 ? g_songs[song].album : "");
}

/* ── text that fits ───────────────────────────────────────────────── */
static void text_fit(int x, int y, int font, int size, const char* s, int maxw, unsigned int color) {
    if (maxw <= 8) return;
    if (banana_font_width(font, size, s) <= maxw) { bwin_font(&win, x, y, font, size, s, color); return; }
    char b[200];
    int n = (int)strlen(s);
    if (n > 190) n = 190;
    while (n > 0) {
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;     /* (whole UTF-8 characters) */
        memcpy(b, s, (size_t)n);
        strcpy(b + n, "...");
        if (banana_font_width(font, size, b) <= maxw) break;
        n--;
    }
    bwin_font(&win, x, y, font, size, b, color);
}

static void fmt_time(char* b, int cap, double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    if (s >= 3600) snprintf(b, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%d:%02d", s / 60, s % 60);
}

/* ── the views ────────────────────────────────────────────────────── */
enum { V_SONGS = 0, V_ALBUMS, V_ARTISTS, V_LIKED, V_QUEUE, V_NOW, V_EQ, V_COUNT };
static const char* const VIEW_NAME[V_COUNT] = { "Songs", "Albums", "Artists", "Liked songs", "Queue", "Now playing", "Equalizer" };
static int  g_view = V_SONGS;
static char g_filter_album[64], g_filter_artist[64];   /* an album / artist opened from its view */
static char g_search[64];
static int  g_scroll, g_sel = -1, g_hover = -1;
static int  g_sort;                       /* 0 title, 1 artist, 2 album, 3 time */
static unsigned int g_last_click;
static int  g_last_click_row = -1;

/* what the list shows: song indices (songs views) or group heads (albums / artists) */
static int* g_list;
static int  g_nlist, g_list_cap;
static int  g_list_gen = -1;

static int contains_ci(const char* hay, const char* needle) {
    if (!needle[0]) return 1;
    size_t n = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, n) == 0) return 1;
    return 0;
}

static int cmp_songs(const void* a, const void* b) {
    const song_t* x = &g_songs[*(const int*)a];
    const song_t* y = &g_songs[*(const int*)b];
    int r = 0;
    switch (g_sort) {
    case 1: r = strcasecmp(x->artist, y->artist); if (!r) r = strcasecmp(x->album, y->album); if (!r) r = x->track - y->track; break;
    case 2: r = strcasecmp(x->album, y->album); if (!r) r = x->track - y->track; break;
    case 3: r = x->dur - y->dur; break;
    default: break;
    }
    if (!r) r = strcasecmp(x->title, y->title);
    return r;
}
static int cmp_album(const void* a, const void* b) {
    return strcasecmp(g_songs[*(const int*)a].album, g_songs[*(const int*)b].album);
}
static int cmp_artist(const void* a, const void* b) {
    return strcasecmp(g_songs[*(const int*)a].artist, g_songs[*(const int*)b].artist);
}

static void list_push(int v) {
    if (g_nlist == g_list_cap) {
        int nc = g_list_cap ? g_list_cap * 2 : 256;
        int* n = realloc(g_list, (size_t)nc * sizeof(int));
        if (!n) return;
        g_list = n;
        g_list_cap = nc;
    }
    g_list[g_nlist++] = v;
}

static int list_is_groups(void) {
    return (g_view == V_ALBUMS && !g_filter_album[0]) || (g_view == V_ARTISTS && !g_filter_artist[0]);
}

static void build_list(void) {
    static char last_key[220];
    char key[220];
    snprintf(key, sizeof(key), "%d|%d|%s|%s|%s", g_view, g_sort, g_search, g_filter_album, g_filter_artist);
    if (g_list_gen == g_lib_gen && !strcmp(last_key, key) && g_view != V_QUEUE) return;
    g_list_gen = g_lib_gen;
    strcpy(last_key, key);
    g_nlist = 0;
    banana_lock(&g_lib_lock);
    if (g_view == V_QUEUE) {
        banana_lock(&g_q_lock);
        for (int i = 0; i < g_qlen; i++) list_push(g_queue[i]);
        banana_unlock(&g_q_lock);
    } else if (list_is_groups()) {
        int* tmp = malloc((size_t)(g_nsongs ? g_nsongs : 1) * sizeof(int));
        if (tmp) {
            int n = 0;
            for (int i = 0; i < g_nsongs; i++)
                if (!g_songs[i].gone && contains_ci(g_view == V_ALBUMS ? g_songs[i].album : g_songs[i].artist, g_search)) tmp[n++] = i;
            qsort(tmp, (size_t)n, sizeof(int), g_view == V_ALBUMS ? cmp_album : cmp_artist);
            for (int i = 0; i < n; i++) {
                const char* k = g_view == V_ALBUMS ? g_songs[tmp[i]].album : g_songs[tmp[i]].artist;
                if (g_nlist && !strcasecmp(k, g_view == V_ALBUMS ? g_songs[g_list[g_nlist - 1]].album : g_songs[g_list[g_nlist - 1]].artist)) continue;
                list_push(tmp[i]);
            }
            free(tmp);
        }
    } else {
        for (int i = 0; i < g_nsongs; i++) {
            song_t* s = &g_songs[i];
            if (s->gone) continue;
            if (g_view == V_LIKED && !s->liked) continue;
            if (g_view == V_ALBUMS && strcasecmp(s->album, g_filter_album)) continue;
            if (g_view == V_ARTISTS && strcasecmp(s->artist, g_filter_artist)) continue;
            if (g_search[0] && !contains_ci(s->title, g_search) && !contains_ci(s->artist, g_search) && !contains_ci(s->album, g_search)) continue;
            list_push(i);
        }
        int save = g_sort;
        if (g_view == V_ALBUMS) g_sort = 2;        /* an album: in its track order */
        qsort(g_list, (size_t)g_nlist, sizeof(int), cmp_songs);
        g_sort = save;
    }
    banana_unlock(&g_lib_lock);
    if (g_sel >= g_nlist) g_sel = -1;
}

/* plays the list shown from row i on (the queue becomes that list) */
static void play_from_list(int row) {
    if (row < 0 || row >= g_nlist) return;
    banana_lock(&g_q_lock);
    if (g_view == V_QUEUE) {
        g_qpos = row;
    } else {
        g_qlen = 0;
        for (int i = 0; i < g_nlist && g_qlen < 4096; i++) g_queue[g_qlen++] = g_list[i];
        g_qpos = row;
        if (g_shuffle) shuffle_queue();
    }
    int song = g_queue[g_qpos];
    banana_unlock(&g_q_lock);
    engine_play(song, 0);
}

static void play_pause(void) {
    double pos;
    int s = now_playing(&pos);
    if (g_paused) { engine_play(g_paused_song, g_paused_pos); return; }
    if (s >= 0 && g_playing) { engine_pause(); return; }
    /* nothing playing: the selected song, or the list from the top */
    if (g_qlen && g_qpos >= 0) { engine_play(g_queue[g_qpos], 0); return; }
    build_list();
    if (!list_is_groups() && g_nlist) play_from_list(g_sel >= 0 ? g_sel : 0);
}

static void skip_track(int step) {
    double pos;
    now_playing(&pos);
    banana_lock(&g_q_lock);
    if (step < 0 && pos > 3) {                   /* back: to the start of this one first */
        int s = g_qpos >= 0 ? g_queue[g_qpos] : -1;
        banana_unlock(&g_q_lock);
        if (s >= 0) engine_play(s, 0);
        return;
    }
    int np = queue_step(step, 1);
    if (np >= 0) g_qpos = np;
    int s = np >= 0 ? g_queue[np] : -1;
    banana_unlock(&g_q_lock);
    if (s >= 0) engine_play(s, 0);
}

static void toggle_like(int song) {
    if (song < 0 || song >= g_nsongs) return;
    g_songs[song].liked = !g_songs[song].liked;
    g_lib_gen++;
    lib_save();
}

/* ── settings ─────────────────────────────────────────────────────── */
static void settings_save(void) {
    conf_dir();
    FILE* f = fopen(CONF_DIR "/settings", "w");
    if (!f) return;
    fprintf(f, "shuffle=%d\nrepeat=%d\nsort=%d\neq=", g_shuffle, g_repeat, g_sort);
    for (int b = 0; b < EQ_BANDS; b++) fprintf(f, "%d%s", g_eq_db[b], b < EQ_BANDS - 1 ? "," : "\n");
    fclose(f);
}

static void settings_load(void) {
    FILE* f = fopen(CONF_DIR "/settings", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "shuffle=", 8)) g_shuffle = atoi(line + 8);
        else if (!strncmp(line, "repeat=", 7)) g_repeat = atoi(line + 7) % 3;
        else if (!strncmp(line, "sort=", 5)) g_sort = atoi(line + 5) & 3;
        else if (!strncmp(line, "eq=", 3)) {
            char* p = line + 3;
            for (int b = 0; b < EQ_BANDS && p; b++) {
                int v = atoi(p);
                g_eq_db[b] = v < -12 ? -12 : v > 12 ? 12 : v;
                p = strchr(p, ',');
                if (p) p++;
            }
            g_eq_gen++;
        }
    }
    fclose(f);
}

/* ── the visualizer: a spectrum of what is heard now ──────────────── */
#define FFT_N  1024
#define NBARS  48
static float g_bars[NBARS], g_peaks[NBARS];

static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void update_spectrum(void) {
    static float re[FFT_N], im[FFT_N], win_fn[FFT_N];
    static int init;
    if (!init) { for (int i = 0; i < FFT_N; i++) win_fn[i] = 0.5f - 0.5f * (float)cos(2 * M_PI * i / (FFT_N - 1)); init = 1; }
    int heard = !g_paused && g_playing;
    unsigned long long p = played_samples();
    for (int i = 0; i < FFT_N; i++) {
        unsigned long long k = p + (unsigned long long)i;
        re[i] = heard && p > FFT_N ? g_viz[(k - FFT_N) % VIZ_RING] * win_fn[i] : 0;
        im[i] = 0;
    }
    fft(re, im, FFT_N);
    /* log-spaced bands from 40 Hz to 16 kHz */
    for (int b = 0; b < NBARS; b++) {
        double f0 = 40 * pow(400.0, (double)b / NBARS), f1 = 40 * pow(400.0, (double)(b + 1) / NBARS);
        int i0 = (int)(f0 * FFT_N / RATE), i1 = (int)(f1 * FFT_N / RATE);
        if (i1 <= i0) i1 = i0 + 1;
        float m = 0;
        for (int i = i0; i < i1 && i < FFT_N / 2; i++) { float v = re[i] * re[i] + im[i] * im[i]; if (v > m) m = v; }
        float db = m > 1e-9f ? 10.0f * (float)log10(m) : -90;
        float v = (db + 30) / 60.0f;          /* -30..30 dB -> 0..1 */
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        g_bars[b] = v > g_bars[b] ? v : g_bars[b] * 0.82f + v * 0.18f;
        g_peaks[b] = g_bars[b] > g_peaks[b] ? g_bars[b] : g_peaks[b] - 0.012f;
        if (g_peaks[b] < 0) g_peaks[b] = 0;
    }
}

static void draw_spectrum(int x, int y, int w, int h) {
    int bw = w / NBARS;
    for (int b = 0; b < NBARS; b++) {
        int bh = (int)(g_bars[b] * h);
        if (bh < 2) bh = 2;
        /* from amethyst at the bottom to lilac at the top */
        bwin_fill_rect(&win, x + b * bw + 1, y + h - bh, bw - 2, bh, C_ACCENT);
        if (bh > h / 2) bwin_fill_rect(&win, x + b * bw + 1, y + h - bh, bw - 2, bh - h / 2, C_ACCENT2);
        int pk = (int)(g_peaks[b] * h);
        bwin_fill_rect(&win, x + b * bw + 1, y + h - pk - 3, bw - 2, 2, C_TEXT);
    }
}

/* ── drawing ──────────────────────────────────────────────────────── */
/* the clickable places of the frame drawn last */
typedef struct { int x, y, w, h, id; } hit_t;
static hit_t g_hits[160];
static int   g_nhits;
enum {
    H_NAV = 1,           /* + view */
    H_PLAY = 20, H_PREV, H_NEXT, H_SHUFFLE, H_REPEAT, H_SEEK, H_VOLUME, H_LIKE_NOW, H_BACK, H_COVER, H_RESCAN,
    H_SORT = 40,         /* + column */
    H_EQ = 50,           /* + band */
    H_PRESET = 70,       /* + preset */
    H_ROW = 1000,        /* + row */
    H_HEART = 20000,     /* + row */
};
static void hit(int x, int y, int w, int h, int id) {
    if (g_nhits < 160) g_hits[g_nhits++] = (hit_t){ x, y, w, h, id };
}
static int hit_at(int mx, int my) {
    for (int i = g_nhits - 1; i >= 0; i--)
        if (mx >= g_hits[i].x && mx < g_hits[i].x + g_hits[i].w && my >= g_hits[i].y && my < g_hits[i].y + g_hits[i].h) return g_hits[i].id;
    return 0;
}

static void icon_nav(int v, int x, int y, unsigned int c) {
    switch (v) {
    case V_SONGS:   bwin_fill_rect(&win, x + 6, y, 2, 11, c); bwin_fill_circle(&win, x + 4, y + 11, 3, c); bwin_fill_rect(&win, x + 6, y, 6, 2, c); break;
    case V_ALBUMS:  bwin_circle(&win, x + 7, y + 7, 7, c); bwin_fill_circle(&win, x + 7, y + 7, 2, c); break;
    case V_ARTISTS: bwin_fill_circle(&win, x + 7, y + 4, 4, c); bwin_fill_rect(&win, x + 1, y + 10, 12, 4, c); break;
    case V_LIKED:   bwin_fill_circle(&win, x + 4, y + 5, 3, c); bwin_fill_circle(&win, x + 10, y + 5, 3, c);
                    for (int i = 0; i < 7; i++) { bwin_fill_rect(&win, x + 1 + i, y + 6 + i, 13 - 2 * i, 1, c); }
                    break;
    case V_QUEUE:   for (int i = 0; i < 3; i++) { bwin_fill_rect(&win, x, y + 2 + i * 5, 14, 2, c); }
                    break;
    case V_NOW:     for (int i = 0; i < 4; i++) { bwin_fill_rect(&win, x + i * 4, y + 12 - (i * 7 % 11), 3, 2 + (i * 7 % 11), c); }
                    break;
    case V_EQ:      for (int i = 0; i < 3; i++) { bwin_fill_rect(&win, x + 2 + i * 5, y, 1, 14, c); bwin_fill_rect(&win, x + i * 5, y + 3 + i * 4, 5, 3, c); }
                    break;
    }
}

static void draw_heart(int x, int y, int on) {
    unsigned int c = on ? C_HEART : C_FAINT;
    bwin_fill_circle(&win, x + 4, y + 4, 3, c);
    bwin_fill_circle(&win, x + 10, y + 4, 3, c);
    for (int i = 0; i < 7; i++) bwin_fill_rect(&win, x + 1 + i, y + 5 + i, 13 - 2 * i, 1, c);
}

static void draw_sidebar(int playing_song) {
    bwin_fill_rect(&win, 0, 0, SIDE_W, win.h - BAR_H, C_SIDE);
    bwin_fill_circle(&win, 26, 28, 9, C_ACCENT);
    bwin_fill_circle(&win, 26, 28, 4, C_SIDE);
    bwin_font(&win, 44, 17, F_BOLD, 20, "Music", C_TEXT);
    int y = 64;
    for (int v = 0; v < V_COUNT; v++) {
        int on = g_view == v;
        if (on) { bwin_fill_rect(&win, 8, y, SIDE_W - 16, 32, C_SEL); bwin_fill_rect(&win, 8, y, 3, 32, C_ACCENT); }
        icon_nav(v, 22, y + 9, on ? C_ACCENT2 : C_DIM);
        bwin_font(&win, 46, y + 8, on ? F_BOLD : F_SANS, 14, VIEW_NAME[v], on ? C_TEXT : C_DIM);
        hit(8, y, SIDE_W - 16, 32, H_NAV + v);
        y += 36;
        if (v == V_QUEUE) { bwin_fill_rect(&win, 20, y + 2, SIDE_W - 40, 1, 0x262A34u); y += 8; }
    }
    char n[48];
    snprintf(n, sizeof(n), "%d song%s", g_nsongs, g_nsongs == 1 ? "" : "s");
    bwin_font(&win, 20, y + 8, F_SANS, 12, n, C_FAINT);
    bwin_font(&win, 20, y + 26, F_SANS, 12, g_scanning ? "Scanning..." : "Rescan the folders", g_scanning ? C_ACCENT2 : C_DIM);
    if (!g_scanning) hit(16, y + 22, 150, 20, H_RESCAN);
    /* the cover of what plays, at the bottom */
    int cs = SIDE_W - 32, cy = win.h - BAR_H - cs - 16;
    if (playing_song >= 0 && cy > y + 50) {
        draw_cover(playing_song, 16, cy, cs);
        hit(16, cy, cs, cs, H_COVER);
    }
}

static void draw_top(void) {
    int x = SIDE_W, w = win.w - SIDE_W;
    bwin_fill_rect(&win, x, 0, w, TOP_H, C_BG);
    char title[160];
    int back = 0;
    if (g_view == V_ALBUMS && g_filter_album[0]) { snprintf(title, sizeof(title), "%s", g_filter_album); back = 1; }
    else if (g_view == V_ARTISTS && g_filter_artist[0]) { snprintf(title, sizeof(title), "%s", g_filter_artist); back = 1; }
    else snprintf(title, sizeof(title), "%s", VIEW_NAME[g_view]);
    int tx = x + 24;
    if (back) {
        bwin_font(&win, tx, 15, F_BOLD, 18, "<", C_ACCENT2);
        hit(tx - 6, 8, 26, 36, H_BACK);
        tx += 24;
    }
    text_fit(tx, 14, F_BOLD, 20, title, w - 330, C_TEXT);
    /* the search box: just type */
    int sx = win.w - 260, sw = 236;
    bwin_fill_rect(&win, sx, 12, sw, 28, C_ROW);
    bwin_rect(&win, sx, 12, sw, 28, g_search[0] ? C_ACCENT : 0x2A2E38u);
    bwin_circle(&win, sx + 14, 24, 5, C_DIM);
    bwin_line(&win, sx + 18, 28, sx + 21, 31, C_DIM);
    if (g_search[0]) { char b[80]; snprintf(b, sizeof(b), "%s_", g_search); text_fit(sx + 28, 18, F_SANS, 13, b, sw - 36, C_TEXT); }
    else bwin_font(&win, sx + 28, 18, F_SANS, 13, "Type to search", C_FAINT);
}

static void draw_song_rows(int x, int y, int w, int h, int playing_song) {
    /* the column heads */
    int cw_title = w * 40 / 100, cw_artist = w * 24 / 100, cw_album = w * 24 / 100;
    int cx_title = x + 44, cx_artist = cx_title + cw_title, cx_album = cx_artist + cw_artist, cx_time = cx_album + cw_album;
    static const char* const HEAD[4] = { "TITLE", "ARTIST", "ALBUM", "TIME" };
    int hx[4] = { cx_title, cx_artist, cx_album, cx_time };
    int sortable = g_view != V_QUEUE && !(g_view == V_ALBUMS);
    for (int c = 0; c < 4; c++) {
        bwin_font(&win, hx[c], y + 4, F_BOLD, 11, HEAD[c], sortable && g_sort == c ? C_ACCENT2 : C_FAINT);
        if (sortable) hit(hx[c], y, 90, 20, H_SORT + c);
    }
    bwin_fill_rect(&win, x + 16, y + 22, w - 32, 1, 0x262A34u);
    y += 26;
    h -= 26;
    int rows = h / ROW_H;
    if (g_scroll > g_nlist - rows) g_scroll = g_nlist - rows;
    if (g_scroll < 0) g_scroll = 0;
    int qcur = g_view == V_QUEUE ? g_qpos : -1;
    for (int r = 0; r < rows && g_scroll + r < g_nlist; r++) {
        int i = g_scroll + r, s = g_list[i];
        song_t* so = &g_songs[s];
        int ry = y + r * ROW_H;
        int cur = (g_view == V_QUEUE) ? (i == qcur) : (s == playing_song);
        unsigned int bg = i == g_sel ? C_SEL : i == g_hover ? C_HOVER : C_BG;
        bwin_fill_rect(&win, x + 8, ry, w - 16, ROW_H - 2, bg);
        char num[12];
        if (cur && g_playing && !g_paused) {
            for (int k = 0; k < 3; k++) {
                int bh = 4 + (int)(g_bars[4 + k * 6] * 12);
                bwin_fill_rect(&win, x + 18 + k * 5, ry + 22 - bh, 3, bh, C_ACCENT2);
            }
        } else {
            snprintf(num, sizeof(num), "%d", g_view == V_ALBUMS && so->track ? so->track : i + 1);
            bwin_font(&win, x + 16, ry + 7, F_SANS, 12, num, cur ? C_ACCENT2 : C_FAINT);
        }
        unsigned int tc = cur ? C_ACCENT2 : C_TEXT;
        text_fit(cx_title, ry + 6, cur ? F_BOLD : F_SANS, 14, so->title, cw_title - 34, tc);
        draw_heart(cx_title + cw_title - 28, ry + 8, so->liked);
        hit(cx_title + cw_title - 30, ry, 22, ROW_H, H_HEART + i);
        text_fit(cx_artist, ry + 7, F_SANS, 13, so->artist, cw_artist - 12, C_DIM);
        text_fit(cx_album, ry + 7, F_SANS, 13, so->album, cw_album - 12, C_DIM);
        char t[16];
        if (so->dur) fmt_time(t, sizeof(t), so->dur); else snprintf(t, sizeof(t), "-");
        bwin_font(&win, cx_time, ry + 7, F_SANS, 13, t, C_DIM);
        hit(x + 8, ry, w - 16, ROW_H - 2, H_ROW + i);
    }
    if (!g_nlist) {
        const char* m = g_scanning ? "Looking for music..." :
                        g_search[0] ? "Nothing matches the search" :
                        g_view == V_LIKED ? "Click the heart of a song to add it here" :
                        g_view == V_QUEUE ? "Nothing is queued - double-click a song" :
                        "No music yet: put songs in the Music folder (or Downloads, or a USB stick)";
        int mw = banana_font_width(F_SANS, 14, m);
        bwin_font(&win, x + (w - mw) / 2, y + 60, F_SANS, 14, m, C_DIM);
    }
}

/* albums: a grid of covers; artists: a list of names with their song count */
static void draw_groups(int x, int y, int w, int h) {
    if (g_view == V_ALBUMS) {
        int tile = 150, gap = 22, cols = (w - 32 + gap) / (tile + gap);
        if (cols < 1) cols = 1;
        int th = tile + 46, rows = h / th + 1;
        int first = g_scroll * cols;
        if (first >= g_nlist) { g_scroll = 0; first = 0; }
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                int i = first + r * cols + c;
                if (i >= g_nlist) break;
                int s = g_list[i];
                int tx = x + 24 + c * (tile + gap), ty = y + 8 + r * th;
                if (ty + th > y + h + th) break;
                if (i == g_hover) bwin_fill_rect(&win, tx - 6, ty - 6, tile + 12, th - 4, C_HOVER);
                draw_cover(s, tx, ty, tile);
                text_fit(tx, ty + tile + 6, F_BOLD, 13, g_songs[s].album, tile, C_TEXT);
                text_fit(tx, ty + tile + 24, F_SANS, 12, g_songs[s].artist, tile, C_DIM);
                hit(tx - 6, ty - 6, tile + 12, th - 4, H_ROW + i);
            }
        return;
    }
    int rows = h / 44;
    if (g_scroll > g_nlist - rows) g_scroll = g_nlist - rows;
    if (g_scroll < 0) g_scroll = 0;
    for (int r = 0; r < rows && g_scroll + r < g_nlist; r++) {
        int i = g_scroll + r, s = g_list[i];
        int ry = y + r * 44;
        if (i == g_hover) bwin_fill_rect(&win, x + 8, ry, w - 16, 42, C_HOVER);
        bwin_fill_circle(&win, x + 36, ry + 21, 16, 0x2A2E38u);
        char ini[4] = { g_songs[s].artist[0], 0 };
        bwin_font(&win, x + 31, ry + 13, F_BOLD, 15, ini, C_ACCENT2);
        int n = 0;
        for (int k = 0; k < g_nsongs; k++) if (!strcasecmp(g_songs[k].artist, g_songs[s].artist)) n++;
        char c[24];
        snprintf(c, sizeof(c), "%d song%s", n, n == 1 ? "" : "s");
        text_fit(x + 64, ry + 6, F_BOLD, 14, g_songs[s].artist, w - 200, C_TEXT);
        bwin_font(&win, x + 64, ry + 24, F_SANS, 12, c, C_DIM);
        hit(x + 8, ry, w - 16, 42, H_ROW + i);
    }
}

static void draw_now(int x, int y, int w, int h, int song, double pos) {
    (void)pos;
    if (song < 0) {
        const char* m = "Nothing is playing";
        int mw = banana_font_width(F_SANS, 15, m);
        bwin_font(&win, x + (w - mw) / 2, y + h / 2 - 10, F_SANS, 15, m, C_DIM);
        return;
    }
    song_t* s = &g_songs[song];
    int cs = h - 150 < w / 2 - 40 ? h - 150 : w / 2 - 40;
    if (cs > 300) cs = 300;
    if (cs < 80) cs = 80;
    int cx = x + 32, cy = y + 16;
    draw_cover(song, cx, cy, cs);
    int tx = cx + cs + 32, tw = x + w - tx - 24;
    text_fit(tx, cy + 10, F_BOLD, 26, s->title, tw, C_TEXT);
    text_fit(tx, cy + 50, F_SANS, 17, s->artist, tw, C_ACCENT2);
    text_fit(tx, cy + 76, F_SANS, 15, s->album, tw, C_DIM);
    draw_heart(tx, cy + 108, s->liked);
    hit(tx - 4, cy + 102, 24, 24, H_LIKE_NOW);
    /* what comes next */
    bwin_font(&win, tx, cy + 140, F_BOLD, 12, "UP NEXT", C_FAINT);
    banana_lock(&g_q_lock);
    for (int k = 1; k <= 4; k++) {
        int p = g_qpos + k;
        if (p >= g_qlen) { if (g_repeat == REPEAT_ALL && g_qlen) p %= g_qlen; else break; }
        song_t* n = &g_songs[g_queue[p]];
        char b[200];
        snprintf(b, sizeof(b), "%s  -  %s", n->title, n->artist);
        text_fit(tx, cy + 140 + k * 22, F_SANS, 13, b, tw, C_DIM);
    }
    banana_unlock(&g_q_lock);
    /* the spectrum under it all */
    draw_spectrum(x + 32, cy + cs + 20, w - 64, h - cs - 52);
}

static const char* const PRESET_NAME[] = { "Flat", "Bass boost", "Treble boost", "Vocal", "Loudness", "Rock", "Classical" };
static const signed char PRESET[][EQ_BANDS] = {
    { 0, 0, 0, 0, 0, 0 }, { 8, 6, 2, 0, 0, 0 }, { 0, 0, 0, 2, 6, 8 }, { -2, -1, 4, 5, 2, 0 },
    { 6, 3, 0, 0, 3, 6 }, { 5, 3, -2, 2, 4, 5 }, { 3, 2, 0, 0, 2, 3 },
};
#define NPRESETS 7

static void draw_eq(int x, int y, int w, int h) {
    bwin_font(&win, x + 32, y + 8, F_SANS, 13, "Shape the sound: drag the sliders, or pick a preset. (Applies to everything Music plays.)", C_DIM);
    int sh = h - 140 > 260 ? 260 : h - 140, top = y + 50;
    int bw = (w - 64) / EQ_BANDS;
    for (int b = 0; b < EQ_BANDS; b++) {
        int bx = x + 32 + b * bw + bw / 2;
        bwin_fill_rect(&win, bx - 2, top, 4, sh, 0x262A34u);
        bwin_fill_rect(&win, bx - 10, top + sh / 2, 20, 1, C_FAINT);
        int ky = top + sh / 2 - g_eq_db[b] * (sh / 2) / 12;
        int y0 = ky < top + sh / 2 ? ky : top + sh / 2, y1 = ky < top + sh / 2 ? top + sh / 2 : ky;
        bwin_fill_rect(&win, bx - 2, y0, 4, y1 - y0, C_ACCENT);
        bwin_fill_circle(&win, bx, ky, 9, C_ACCENT2);
        char v[12];
        snprintf(v, sizeof(v), "%+d dB", g_eq_db[b]);
        int vw = banana_font_width(F_SANS, 12, v);
        bwin_font(&win, bx - vw / 2, top + sh + 10, F_SANS, 12, v, C_TEXT);
        char hz[12];
        snprintf(hz, sizeof(hz), "%s Hz", EQ_LABEL[b]);
        int hw = banana_font_width(F_BOLD, 12, hz);
        bwin_font(&win, bx - hw / 2, top + sh + 28, F_BOLD, 12, hz, C_DIM);
        hit(bx - bw / 2, top - 10, bw, sh + 20, H_EQ + b);
    }
    int py = top + sh + 60, px = x + 32;
    for (int p = 0; p < NPRESETS; p++) {
        int pw = banana_font_width(F_SANS, 13, PRESET_NAME[p]) + 24;
        if (px + pw > x + w - 16) { px = x + 32; py += 34; }
        int on = 1;
        for (int b = 0; b < EQ_BANDS; b++) if (PRESET[p][b] != g_eq_db[b]) on = 0;
        bwin_fill_rect(&win, px, py, pw, 26, on ? C_ACCENT : C_ROW);
        bwin_font(&win, px + 12, py + 5, on ? F_BOLD : F_SANS, 13, PRESET_NAME[p], C_TEXT);
        hit(px, py, pw, 26, H_PRESET + p);
        px += pw + 10;
    }
}

static void tri(int x, int y, int w, int h, int right, unsigned int c) {   /* a filled triangle */
    for (int i = 0; i < w; i++) {
        int hh = right ? h * (w - i) / w : h * i / w;
        bwin_fill_rect(&win, x + i, y + (h - hh) / 2, 1, hh, c);
    }
}

static void draw_bar(int song, double pos) {
    int y0 = win.h - BAR_H;
    bwin_fill_rect(&win, 0, y0, win.w, BAR_H, C_BAR);
    bwin_fill_rect(&win, 0, y0, win.w, 1, 0x2A2E38u);
    /* the song */
    int infow = win.w / 4;
    if (song >= 0) {
        draw_cover(song, 14, y0 + 12, 60);
        hit(14, y0 + 12, 60, 60, H_COVER);
        text_fit(86, y0 + 20, F_BOLD, 14, g_songs[song].title, infow - 90, C_TEXT);
        text_fit(86, y0 + 42, F_SANS, 12, g_songs[song].artist, infow - 90, C_DIM);
    } else {
        bwin_font(&win, 20, y0 + 32, F_SANS, 13, g_scan_status[0] ? g_scan_status : "Pick a song", C_DIM);
    }
    /* the buttons */
    int cx = win.w / 2, by = y0 + 14;
    int playing = g_playing && !g_paused;
    bwin_fill_circle(&win, cx, by + 16, 17, C_TEXT);
    if (playing) { bwin_fill_rect(&win, cx - 6, by + 8, 4, 16, C_BAR); bwin_fill_rect(&win, cx + 2, by + 8, 4, 16, C_BAR); }
    else tri(cx - 4, by + 7, 13, 18, 1, C_BAR);
    hit(cx - 18, by - 2, 36, 36, H_PLAY);
    /* previous / next */
    tri(cx - 58, by + 9, 10, 14, 0, C_TEXT); bwin_fill_rect(&win, cx - 62, by + 9, 3, 14, C_TEXT);
    hit(cx - 68, by, 28, 32, H_PREV);
    tri(cx + 48, by + 9, 10, 14, 1, C_TEXT); bwin_fill_rect(&win, cx + 59, by + 9, 3, 14, C_TEXT);
    hit(cx + 42, by, 28, 32, H_NEXT);
    /* shuffle: two crossing arrows; repeat: a loop (with a 1 for one song) */
    unsigned int shc = g_shuffle ? C_ACCENT2 : C_DIM;
    bwin_line(&win, cx - 104, by + 10, cx - 88, by + 22, shc); bwin_line(&win, cx - 104, by + 22, cx - 88, by + 10, shc);
    bwin_fill_rect(&win, cx - 90, by + 9, 3, 3, shc); bwin_fill_rect(&win, cx - 90, by + 21, 3, 3, shc);
    if (g_shuffle) bwin_fill_circle(&win, cx - 96, by + 30, 2, C_ACCENT2);
    hit(cx - 110, by, 28, 34, H_SHUFFLE);
    unsigned int rc = g_repeat ? C_ACCENT2 : C_DIM;
    bwin_rect(&win, cx + 86, by + 9, 18, 13, rc);
    tri(cx + 100, by + 6, 6, 7, 1, rc);
    if (g_repeat == REPEAT_ONE) bwin_font(&win, cx + 92, by + 9, F_BOLD, 10, "1", C_ACCENT2);
    if (g_repeat) bwin_fill_circle(&win, cx + 95, by + 30, 2, C_ACCENT2);
    hit(cx + 80, by, 30, 34, H_REPEAT);
    /* the position */
    int sw = win.w / 2 - 40, sx = cx - sw / 2, sy = y0 + 62;
    double dur = song >= 0 ? g_songs[song].dur : 0;
    char a[16], b[16];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    bwin_font(&win, sx - 40, sy - 6, F_SANS, 11, song >= 0 ? a : "", C_DIM);
    bwin_font(&win, sx + sw + 8, sy - 6, F_SANS, 11, song >= 0 && dur ? b : "", C_DIM);
    bwin_fill_rect(&win, sx, sy, sw, 4, 0x343846u);
    if (dur > 0) {
        int f = (int)(sw * pos / dur);
        if (f > sw) f = sw;
        bwin_fill_rect(&win, sx, sy, f, 4, C_ACCENT);
        bwin_fill_circle(&win, sx + f, sy + 2, 6, C_TEXT);
    }
    hit(sx - 6, sy - 8, sw + 12, 20, H_SEEK);
    /* the volume (the system's) and a little spectrum */
    int vx = win.w - 170, vw = 110, vy = y0 + 40;
    int vol = banana_volume(-1);
    if (vol >= 0) {
        bwin_fill_rect(&win, vx - 22, vy - 3, 5, 8, C_DIM);
        tri(vx - 18, vy - 7, 7, 16, 1, C_DIM);
        bwin_fill_rect(&win, vx, vy, vw, 4, 0x343846u);
        bwin_fill_rect(&win, vx, vy, vw * vol / 100, 4, C_ACCENT);
        bwin_fill_circle(&win, vx + vw * vol / 100, vy + 2, 5, C_TEXT);
        hit(vx - 6, vy - 10, vw + 12, 24, H_VOLUME);
    }
    for (int k = 0; k < 12; k++) {
        int bh = 2 + (int)(g_bars[k * 4] * 18);
        bwin_fill_rect(&win, win.w - 44 + k * 3 - 6, y0 + 24 - bh / 2 + 6, 2, bh, playing ? C_ACCENT : C_FAINT);
    }
}

static int g_song_now = -1;
static double g_pos_now;

static void draw_all(void) {
    g_nhits = 0;
    g_song_now = now_playing(&g_pos_now);
    build_list();
    bwin_fill_rect(&win, SIDE_W, TOP_H, win.w - SIDE_W, win.h - TOP_H - BAR_H, C_BG);
    draw_sidebar(g_song_now);
    draw_top();
    int x = SIDE_W, y = TOP_H, w = win.w - SIDE_W, h = win.h - TOP_H - BAR_H;
    if (g_view == V_NOW) draw_now(x, y, w, h, g_song_now, g_pos_now);
    else if (g_view == V_EQ) draw_eq(x, y, w, h);
    else if (list_is_groups()) draw_groups(x, y, w, h);
    else draw_song_rows(x, y, w, h, g_song_now);
    draw_bar(g_song_now, g_pos_now);
}

/* ── input ────────────────────────────────────────────────────────── */
static int g_drag;                          /* H_SEEK, H_VOLUME, H_EQ + band */

static void drag_to(int id, int mx, int my) {
    for (int i = 0; i < g_nhits; i++) {
        if (g_hits[i].id != id) continue;
        hit_t* h = &g_hits[i];
        if (id == H_SEEK && g_song_now >= 0 && g_songs[g_song_now].dur > 0) {
            double f = (mx - h->x - 6) / (double)(h->w - 12);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (g_paused) g_paused_pos = f * g_songs[g_song_now].dur;
            else engine_play(g_song_now, f * g_songs[g_song_now].dur);
        } else if (id == H_VOLUME) {
            int v = (mx - h->x - 6) * 100 / (h->w - 12);
            banana_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        } else if (id >= H_EQ && id < H_EQ + EQ_BANDS) {
            int mid = h->y + h->h / 2, half = (h->h - 20) / 2;
            int db = (mid - my) * 12 / (half ? half : 1);
            g_eq_db[id - H_EQ] = db < -12 ? -12 : db > 12 ? 12 : db;
            g_eq_gen++;
        }
        return;
    }
}

static void set_view(int v) {
    g_view = v;
    g_scroll = 0;
    g_sel = -1;
    g_filter_album[0] = g_filter_artist[0] = 0;
}

static void click(int mx, int my, int dbl) {
    int id = hit_at(mx, my);
    if (id >= H_HEART) { int row = id - H_HEART; if (row < g_nlist) toggle_like(g_list[row]); return; }
    if (id >= H_ROW) {
        int row = id - H_ROW;
        if (row >= g_nlist) return;
        if (list_is_groups()) {                      /* an album or an artist: its songs */
            int s = g_list[row];
            if (g_view == V_ALBUMS) snprintf(g_filter_album, sizeof(g_filter_album), "%s", g_songs[s].album);
            else snprintf(g_filter_artist, sizeof(g_filter_artist), "%s", g_songs[s].artist);
            g_scroll = 0;
            g_sel = -1;
            g_search[0] = 0;
            return;
        }
        g_sel = row;
        if (dbl) play_from_list(row);
        return;
    }
    if (id >= H_PRESET && id < H_PRESET + NPRESETS) {
        for (int b = 0; b < EQ_BANDS; b++) g_eq_db[b] = PRESET[id - H_PRESET][b];
        g_eq_gen++;
        settings_save();
        return;
    }
    if (id >= H_EQ && id < H_EQ + EQ_BANDS) { g_drag = id; drag_to(id, mx, my); return; }
    if (id >= H_SORT && id < H_SORT + 4) { g_sort = id - H_SORT; settings_save(); return; }
    if (id >= H_NAV && id < H_NAV + V_COUNT) { set_view(id - H_NAV); return; }
    switch (id) {
    case H_PLAY: play_pause(); break;
    case H_PREV: skip_track(-1); break;
    case H_NEXT: skip_track(1); break;
    case H_SHUFFLE:
        g_shuffle = !g_shuffle;
        banana_lock(&g_q_lock);
        if (g_shuffle) shuffle_queue();
        banana_unlock(&g_q_lock);
        settings_save();
        break;
    case H_REPEAT: g_repeat = (g_repeat + 1) % 3; settings_save(); break;
    case H_SEEK: g_drag = id; break;               /* (it lands when the button is released) */
    case H_VOLUME: g_drag = id; drag_to(id, mx, my); break;
    case H_LIKE_NOW: toggle_like(g_song_now); break;
    case H_BACK: g_filter_album[0] = g_filter_artist[0] = 0; g_scroll = 0; break;
    case H_COVER: set_view(V_NOW); break;
    case H_RESCAN: if (!g_scanning) banana_thread(scan_thread, NULL); break;
    }
}

static void key(int k) {
    int rows = (win.h - TOP_H - BAR_H - 26) / ROW_H;
    switch (k) {
    case BANANA_KEY_PLAY: play_pause(); return;
    case BANANA_KEY_NEXT: skip_track(1); return;
    case BANANA_KEY_PREV: skip_track(-1); return;
    case BANANA_KEY_STOP: if (g_playing && !g_paused) engine_pause(); return;
    case BANANA_KEY_UP:   if (g_sel > 0) g_sel--; else g_scroll -= 1; if (g_sel >= 0 && g_sel < g_scroll) g_scroll = g_sel; return;
    case BANANA_KEY_DOWN: if (g_sel < g_nlist - 1 && g_sel >= 0) g_sel++; else g_scroll += 1; if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1; return;
    case BANANA_KEY_PGUP: g_scroll -= rows; return;
    case BANANA_KEY_PGDN: g_scroll += rows; return;
    case BANANA_KEY_HOME: g_scroll = 0; g_sel = g_nlist ? 0 : -1; return;
    case BANANA_KEY_END:  g_scroll = g_nlist; g_sel = g_nlist - 1; return;
    case BANANA_KEY_LEFT:  if (g_song_now >= 0 && !g_paused) engine_play(g_song_now, g_pos_now - 10); return;
    case BANANA_KEY_RIGHT: if (g_song_now >= 0 && !g_paused) engine_play(g_song_now, g_pos_now + 10); return;
    case '\n': case '\r': if (g_sel >= 0 && !list_is_groups()) play_from_list(g_sel); return;
    case 27:
        if (g_search[0]) g_search[0] = 0;
        else if (g_filter_album[0] || g_filter_artist[0]) g_filter_album[0] = g_filter_artist[0] = 0;
        return;
    case '\b': case 127: { size_t n = strlen(g_search); if (n) g_search[n - 1] = 0; g_scroll = 0; return; }
    case ' ': if (!g_search[0]) { play_pause(); return; } break;
    default: break;
    }
    if (k >= 32 && k < 127 && g_view != V_EQ && g_view != V_NOW) {        /* typing searches */
        size_t n = strlen(g_search);
        if (n < sizeof(g_search) - 1) { g_search[n] = (char)k; g_search[n + 1] = 0; }
        g_scroll = 0;
        g_sel = -1;
    }
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_QUIET);
    if (bwin_open(&win, "Music", WIN_W, WIN_H) < 0) {
        printf("music: the desktop is not running (startx)\n");
        return 1;
    }
    bwin_resizable(&win, 700, 440);
    bwin_media_keys(&win);
    bwin_fill_rect(&win, 0, 0, win.w, win.h, C_BG);
    bwin_update(&win);

    lib_load();
    settings_load();
    /* files given: into the library (if new) and played */
    int first = -1;
    for (int i = 1; i < argc; i++) {
        int k = lib_find(argv[i]);
        if (k < 0) {
            song_t s;
            memset(&s, 0, sizeof(s));
            snprintf(s.path, sizeof(s.path), "%s", argv[i]);
            if (probe(argv[i], &s) < 0) continue;
            struct stat st;
            if (stat(argv[i], &st) == 0) s.size = (unsigned)st.st_size;
            s.seen = 1;
            lib_add(&s);
            k = g_nsongs - 1;
        }
        if (g_qlen < 4096) g_queue[g_qlen++] = k;
        if (first < 0) first = k;
    }

    int eng = banana_thread(engine_thread, NULL);
    if (eng < 0) { printf("music: no thread\n"); return 1; }
    if (first >= 0) { g_qpos = 0; engine_play(first, 0); g_view = V_NOW; }
    banana_thread(scan_thread, NULL);

    unsigned int last = 0;
    int dirty = 1, lib_seen = -1;
    for (;;) {
        banana_event_t ev;
        int timeout = 30;
        while (bwin_wait_event(&win, &ev, timeout)) {
            timeout = 0;
            if (ev.type == BANANA_EV_CLOSE) goto out;
            if (ev.type == BANANA_EV_RESIZE) { dirty = 1; continue; }
            if (ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1) {
                unsigned int now = banana_ticks();
                int id = hit_at(ev.x, ev.y);
                int dbl = id == g_last_click_row && now - g_last_click < 450;
                g_last_click = now;
                g_last_click_row = id;
                click(ev.x, ev.y, dbl);
                dirty = 1;
            } else if (ev.type == BANANA_EV_MOUSE_MOVE) {
                if (g_drag && g_drag != H_SEEK && (ev.buttons & 1)) { drag_to(g_drag, ev.x, ev.y); dirty = 1; }
                int id = hit_at(ev.x, ev.y);
                int hv = id >= H_ROW && id < H_HEART ? id - H_ROW : -1;
                if (hv != g_hover) { g_hover = hv; dirty = 1; }
            } else if (ev.type == BANANA_EV_MOUSE_UP) {
                if (g_drag >= H_EQ && g_drag < H_EQ + EQ_BANDS) settings_save();
                if (g_drag == H_SEEK) drag_to(H_SEEK, ev.x, ev.y);
                g_drag = 0;
            } else if (ev.type == BANANA_EV_KEY) {
                key(ev.key);
                dirty = 1;
            }
        }
        unsigned int now = banana_ticks();
        if (lib_seen != g_lib_gen) { lib_seen = g_lib_gen; dirty = 1; }
        int animate = g_playing && !g_paused;
        if (animate) update_spectrum();
        if (dirty || now - last >= (animate ? 33u : 500u)) {
            draw_all();
            bwin_update(&win);
            last = now;
            dirty = 0;
        }
    }
out:
    settings_save();
    g_quit = 1;
    banana_lock(&g_e_lock);
    banana_cond_broadcast(&g_e_cond);
    banana_unlock(&g_e_lock);
    banana_audio_stop();
    banana_join(eng);
    bwin_close(&win);
    return 0;
}
