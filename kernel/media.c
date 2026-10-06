#include "media.h"
#include "audio.h"
#include "browser.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "../media/codec.h"

#define MEDIA_MAX   16
#define CHUNK       2048                    /* frames decoded at a time */
#define AHEAD_MS    500                     /* queued ahead of the card at most */

typedef struct {
    int      used, closing;
    void*    owner;
    char     url[1024];
    int      state;
    char     error[96];
    uint8_t* data;                          /* the whole file */
    uint32_t len;
    codec_t* codec;
    int      want_play, playing, ended;
    int      volume, muted, loop;
    uint64_t frame;                         /* the next frame to decode */
    int      seek_pending;
    uint32_t seek_ms;
    uint32_t seq;
} stream_t;

static stream_t g_s[MEDIA_MAX];
static int      g_task = -1;
static int      g_current = -1;            /* the stream feeding the card */

static stream_t* get(int id) { return id >= 0 && id < MEDIA_MAX && g_s[id].used && !g_s[id].closing ? &g_s[id] : NULL; }

static uint32_t frames_to_ms(const stream_t* s, uint64_t f) {
    return s->codec && s->codec->rate ? (uint32_t)(f * 1000 / (uint64_t)s->codec->rate) : 0;
}

/* frames still queued for the card, in this stream's rate */
static uint64_t queued_frames(const stream_t* s) {
    if (!s->codec) return 0;
    return (uint64_t)audio_queued_ms() * (uint64_t)s->codec->rate / 1000;
}

static void stop_output(int id) {
    if (g_current == id) { audio_stop(); g_current = -1; }
}

static void load(stream_t* s) {
    char* data = NULL;
    uint32_t len = 0;
    char err[96];
    if (browser_fetch_media(s->url, &data, &len, err, sizeof(err)) != 0) {
        kstrlcpy(s->error, err[0] ? err : "the file could not be loaded", sizeof(s->error));
        s->state = MEDIA_ERROR;
        return;
    }
    if (s->closing) { kfree(data); return; }
    s->data = (uint8_t*)data;
    s->len = len;
    s->codec = codec_open(s->data, s->len, err, sizeof(err));
    if (!s->codec) {
        kstrlcpy(s->error, err, sizeof(s->error));
        s->state = MEDIA_ERROR;
        return;
    }
    s->state = MEDIA_READY;
}

static void release(stream_t* s, int id) {
    stop_output(id);
    if (s->codec) s->codec->close(s->codec);
    if (s->data) kfree(s->data);
    memset(s, 0, sizeof(*s));
}

/* decodes one chunk of the current stream into the card's queue */
static void feed(stream_t* s, int id) {
    if (audio_queued_ms() > AHEAD_MS) return;
    static int16_t buf[CHUNK * 2];
    int n = s->codec->decode(s->codec, buf, CHUNK);
    if (n <= 0) {
        if (s->loop && s->codec->seek(s->codec, 0) == 0) { s->frame = 0; s->seq++; return; }
        if (!audio_busy()) {                         /* the last of it has played */
            s->playing = 0;
            s->want_play = 0;
            s->ended = 1;
            g_current = -1;
        }
        return;
    }
    int ch = s->codec->channels;
    if (s->muted || s->volume < 100) {
        int v = s->muted ? 0 : s->volume;
        for (int i = 0; i < n * ch; i++) buf[i] = (int16_t)(buf[i] * v / 100);
    }
    audio_play(buf, (uint32_t)n * (uint32_t)ch * 2, s->codec->rate, ch, 16);
    s->frame += (uint64_t)n;
    (void)id;
}

static void media_task(void) {
    for (;;) {
        int busy = 0;
        for (int i = 0; i < MEDIA_MAX; i++) {
            stream_t* s = &g_s[i];
            if (!s->used) continue;
            if (s->closing) { release(s, i); continue; }
            if (s->state == MEDIA_LOADING) { load(s); busy = 1; continue; }
            if (s->state != MEDIA_READY) continue;
            if (s->seek_pending) {
                s->seek_pending = 0;
                int was_current = g_current == i;
                stop_output(i);
                if (was_current && s->playing) g_current = i;   /* it goes on playing from there */
                uint64_t f = (uint64_t)s->seek_ms * (uint64_t)s->codec->rate / 1000;
                if (s->codec->length && f > s->codec->length) f = s->codec->length;
                if (s->codec->seek(s->codec, f) == 0) s->frame = f;
                s->ended = 0;
                s->seq++;
            }
            if (s->want_play && !s->playing) {
                if (g_current >= 0 && g_current != i) {   /* one at a time */
                    stream_t* o = &g_s[g_current];
                    o->want_play = 0;
                    o->playing = 0;
                    o->frame -= queued_frames(o) < o->frame ? queued_frames(o) : o->frame;   /* resume where it was heard */
                    o->codec->seek(o->codec, o->frame);
                    stop_output(g_current);
                }
                if (s->ended) { s->codec->seek(s->codec, 0); s->frame = 0; s->ended = 0; s->seq++; }
                s->playing = 1;
                g_current = i;
            } else if (!s->want_play && s->playing) {
                uint64_t q = queued_frames(s);
                s->frame -= q < s->frame ? q : s->frame;   /* back to what was heard */
                s->codec->seek(s->codec, s->frame);
                s->playing = 0;
                stop_output(i);
            }
            if (s->playing && g_current == i) { feed(s, i); busy = 1; }
        }
        task_sleep_ms(busy ? 10 : 50);
    }
}

int media_open(void* owner, const char* url) {
    int id = -1;
    for (int i = 0; i < MEDIA_MAX; i++) if (!g_s[i].used) { id = i; break; }
    if (id < 0 || !url || !*url) return -1;
    stream_t* s = &g_s[id];
    memset(s, 0, sizeof(*s));
    s->owner = owner;
    kstrlcpy(s->url, url, sizeof(s->url));
    s->volume = 100;
    s->state = MEDIA_LOADING;
    s->used = 1;
    if (g_task < 0) g_task = task_create_stack("media", media_task, 256u << 10);
    return id;
}

void media_close(int id) { stream_t* s = get(id); if (s) { s->want_play = 0; s->closing = 1; } }

void media_close_owner(void* owner) {
    for (int i = 0; i < MEDIA_MAX; i++)
        if (g_s[i].used && g_s[i].owner == owner) { g_s[i].want_play = 0; g_s[i].closing = 1; }
}

void media_play(int id) { stream_t* s = get(id); if (s) s->want_play = 1; }
void media_pause(int id) { stream_t* s = get(id); if (s) s->want_play = 0; }

void media_seek(int id, uint32_t ms) {
    stream_t* s = get(id);
    if (!s) return;
    s->seek_ms = ms;
    s->seek_pending = 1;
}

void media_set(int id, int volume, int muted, int loop) {
    stream_t* s = get(id);
    if (!s) return;
    s->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    s->muted = muted;
    s->loop = loop;
}

int media_status(int id, media_status_t* st) {
    stream_t* s = get(id);
    if (!s || !st) return -1;
    memset(st, 0, sizeof(*st));
    st->state = s->state;
    st->playing = s->playing || (s->want_play && s->state == MEDIA_LOADING);
    st->ended = s->ended;
    st->seq = s->seq;
    kstrlcpy(st->error, s->error, sizeof(st->error));
    if (s->codec) {
        st->dur_ms = frames_to_ms(s, s->codec->length);
        uint64_t f = s->frame;
        if (s->playing && g_current == (int)(s - g_s)) { uint64_t q = queued_frames(s); f = q < f ? f - q : 0; }
        if (s->seek_pending) f = (uint64_t)s->seek_ms * (uint64_t)s->codec->rate / 1000;
        st->pos_ms = frames_to_ms(s, f);
        if (st->dur_ms && st->pos_ms > st->dur_ms) st->pos_ms = st->dur_ms;
    }
    return 0;
}
