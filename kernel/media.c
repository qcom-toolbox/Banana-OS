/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Web page <audio> / <video> streams. Each stream downloads its file, then
 * hands it to a restricted worker process of its own (the media worker,
 * apps/webmedia, FFmpeg) that decodes it: no media decoder runs in
 * the kernel on what a page sent. A "media" task keeps each playing
 * stream's clock, feeds the sound card from the one stream heard (16-bit
 * stereo, WW_MEDIA_RATE) and keeps each video's newest picture, which its
 * page takes (media_frame) and draws in the <video> element. */
#include "media.h"
#include "audio.h"
#include "browser.h"
#include "kheap.h"
#include "kstring.h"
#include "remote_page.h"
#include "serial.h"
#include "spinlock.h"
#include "task.h"
#include "timer.h"
#include "../sdk/include/banana_web_worker.h"

#define MEDIA_MAX   16
#define AHEAD_MS    300                     /* sound queued ahead of the card at most */
#define CHUNK       (WW_MEDIA_RATE / 20)    /* frames asked for at a time (50 ms) */
#define PIC_MAX     (1280u * 720u * 4u)

typedef struct {
    int      used, closing;
    void*    owner;
    char     url[1024];
    int      state;
    char     error[96];
    void*    worker;
    ww_media_info_t info;
    int      want_play, playing, ended;
    int      volume, muted, loop;
    uint32_t t0, pos_ms;                    /* playing: the position is now - t0 */
    int      seek_pending;
    uint32_t seek_ms;
    uint32_t seq;
    int      audio_eof, video_eof;
    /* the newest picture */
    spinlock_t lock;
    uint32_t* px;
    int      pw, ph, pic_pts;
    uint32_t pic_seq;
    uint32_t next_pic;                      /* when to ask for the next one (ms) */
    uint8_t* buf;                           /* the worker's answers */
} stream_t;

static stream_t g_s[MEDIA_MAX];
static int      g_task = -1;
static int      g_current = -1;            /* the stream heard */

static stream_t* get(int id) { return id >= 0 && id < MEDIA_MAX && g_s[id].used && !g_s[id].closing ? &g_s[id] : NULL; }

static uint32_t position(const stream_t* s) {
    uint32_t p = s->playing ? timer_ms() - s->t0 : s->pos_ms;
    if (s->info.duration_ms > 0 && p > (uint32_t)s->info.duration_ms) p = (uint32_t)s->info.duration_ms;
    return p;
}

static void stop_output(int id) {
    if (g_current == id) { audio_stop(); g_current = -1; }
}

static void fail(stream_t* s, const char* why) {
    klog("media: %s: %s\n", s->url, why);
    kstrlcpy(s->error, why, sizeof(s->error));
    s->state = MEDIA_ERROR;
    s->playing = s->want_play = 0;
}

static void load(stream_t* s) {
    char* data = NULL;
    uint32_t len = 0;
    char err[96];
    if (browser_fetch_media(s->url, &data, &len, err, sizeof(err)) != 0) { fail(s, err[0] ? err : "the file could not be loaded"); return; }
    if (s->closing) { kfree(data); return; }
    if (len > WW_MEDIA_BYTES) { kfree(data); fail(s, "the file is too large (96 MB at most)"); return; }
    s->buf = kmalloc(sizeof(ww_media_picture_t) + PIC_MAX);
    s->worker = s->buf ? media_worker_start() : NULL;
    if (!s->worker) { kfree(data); fail(s, "no media decoder could start"); return; }
    uint32_t got = 0;
    int rc = media_worker_call(s->worker, WW_MEDIA_OPEN, NULL, data, len, s->buf, sizeof(ww_media_info_t), &got);
    kfree(data);
    if (rc != 0 || got != sizeof(ww_media_info_t)) { fail(s, "the format is not supported or the file is damaged"); return; }
    memcpy(&s->info, s->buf, sizeof(s->info));
    if (s->info.width > 1280 || s->info.height > 720 || s->info.width < 0 || s->info.height < 0) { fail(s, "bad picture size"); return; }
    s->state = MEDIA_READY;
}

static void release(stream_t* s, int id) {
    stop_output(id);
    if (s->worker) media_worker_stop(s->worker);
    spin_lock(&s->lock);
    if (s->px) kfree(s->px);
    s->px = NULL;
    spin_unlock(&s->lock);
    if (s->buf) kfree(s->buf);
    memset(s, 0, sizeof(*s));
}

static void go_to(stream_t* s, int id, uint32_t ms) {
    stop_output(id);
    if (media_worker_call(s->worker, WW_MEDIA_SEEK, (int[10]){ (int)ms }, NULL, 0, NULL, 0, NULL) != 0) {
        fail(s, "the decoder stopped");
        return;
    }
    s->pos_ms = ms;
    s->t0 = timer_ms() - ms;
    s->audio_eof = s->video_eof = 0;
    s->next_pic = 0;
    s->pic_pts = -1;
    s->ended = 0;
    s->seq++;
}

/* sound for the card, up to AHEAD_MS ahead */
static void feed(stream_t* s, int id) {
    uint32_t pos = position(s);
    for (int round = 0; round < 8 && audio_queued_ms() < AHEAD_MS && !s->audio_eof; round++) {
        uint32_t got = 0;
        /* (the position heard next: what the card still holds is ahead of it) */
        uint32_t from = pos + audio_queued_ms();
        if (media_worker_call(s->worker, WW_MEDIA_AUDIO, (int[10]){ CHUNK, (int)from }, NULL, 0, s->buf,
                              sizeof(ww_media_audio_t) + CHUNK * 4, &got) != 0 || got < sizeof(ww_media_audio_t)) {
            fail(s, "the decoder stopped");
            return;
        }
        ww_media_audio_t h;
        memcpy(&h, s->buf, sizeof(h));
        if (h.frames < 0 || h.frames > CHUNK || got != sizeof(h) + (uint32_t)h.frames * 4) { fail(s, "bad sound from the decoder"); return; }
        if (h.eof) s->audio_eof = 1;
        int16_t* pcm = (int16_t*)(s->buf + sizeof(h));
        int n = h.frames, skip = 0;
        /* what is already past (it was muted, or the decoder was late) is not played */
        if (h.start_ms >= 0 && (uint32_t)h.start_ms + 20 < pos) {
            skip = (int)((uint64_t)(pos - (uint32_t)h.start_ms) * WW_MEDIA_RATE / 1000);
            if (skip > n) skip = n;
        }
        n -= skip;
        if (n <= 0) continue;
        pcm += skip * 2;
        if (s->volume < 100)
            for (int i = 0; i < n * 2; i++) pcm[i] = (int16_t)(pcm[i] * s->volume / 100);
        audio_play(pcm, (uint32_t)n * 4, WW_MEDIA_RATE, 2, 16);
    }
    (void)id;
}

/* the picture due now, if a newer one is ready */
static void picture(stream_t* s) {
    uint32_t now = timer_ms();
    if ((int32_t)(now - s->next_pic) < 0 || s->video_eof) return;
    s->next_pic = now + 30;
    uint32_t got = 0;
    int had = s->pic_pts < 0 ? 0 : s->pic_pts + 1;
    if (media_worker_call(s->worker, WW_MEDIA_VIDEO, (int[10]){ (int)position(s), had }, NULL, 0, s->buf,
                          sizeof(ww_media_picture_t) + PIC_MAX, &got) != 0 || got < sizeof(ww_media_picture_t)) {
        fail(s, "the decoder stopped");
        return;
    }
    ww_media_picture_t h;
    memcpy(&h, s->buf, sizeof(h));
    if (h.eof) s->video_eof = 1;
    if (h.width <= 0 || h.height <= 0) return;
    uint32_t bytes = (uint32_t)h.width * (uint32_t)h.height * 4u;
    if (h.width > 1280 || h.height > 720 || got != sizeof(h) + bytes) return;
    uint32_t* px = kmalloc(bytes);
    if (!px) return;
    memcpy(px, s->buf + sizeof(h), bytes);
    spin_lock(&s->lock);
    uint32_t* old = s->px;
    s->px = px;
    s->pw = h.width;
    s->ph = h.height;
    s->pic_seq++;
    spin_unlock(&s->lock);
    if (old) kfree(old);
    s->pic_pts = h.pts_ms;
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
            if (s->seek_pending) { s->seek_pending = 0; go_to(s, i, s->seek_ms); if (s->state != MEDIA_READY) continue; }
            if (s->want_play && !s->playing) {
                if (s->ended) go_to(s, i, 0);
                s->playing = 1;
                s->t0 = timer_ms() - s->pos_ms;
            } else if (!s->want_play && s->playing) {
                s->pos_ms = position(s);
                s->playing = 0;
                stop_output(i);
            }
            if (!s->playing) {
                if (s->info.has_video && s->pic_pts < 0) picture(s);   /* the first picture, paused */
                continue;
            }
            busy = 1;
            int heard = s->info.has_audio && !s->muted && s->volume > 0;
            if (heard && g_current != i) {
                /* one sound at a time: the other one keeps going, silent */
                if (g_current >= 0) audio_stop();
                g_current = i;
            } else if (!heard && g_current == i) stop_output(i);
            if (g_current == i) feed(s, i);
            if (s->state == MEDIA_READY && s->info.has_video) picture(s);
            if (s->state != MEDIA_READY) { stop_output(i); continue; }
            /* the end: its length reached, or nothing left to decode */
            uint32_t pos = position(s);
            int done = s->info.duration_ms > 0 ? pos >= (uint32_t)s->info.duration_ms
                                               : (!s->info.has_video || s->video_eof) && (!heard || s->audio_eof);
            if (done && g_current == i && audio_busy()) done = 0;       /* the last of it is still playing */
            if (done) {
                if (s->loop) go_to(s, i, 0);
                else {
                    s->pos_ms = pos;
                    s->playing = s->want_play = 0;
                    s->ended = 1;
                    stop_output(i);
                }
            }
        }
        task_sleep_ms(busy ? 10 : 50);
    }
}

int media_open(void* owner, const char* url) {
    int id = -1;
    for (int i = 0; i < MEDIA_MAX; i++) if (!g_s[i].used) { id = i; break; }
    if (id < 0 || !url || !*url || strlen(url) >= sizeof(g_s[0].url)) return -1;
    stream_t* s = &g_s[id];
    memset(s, 0, sizeof(*s));
    s->owner = owner;
    kstrlcpy(s->url, url, sizeof(s->url));
    s->volume = 100;
    s->pic_pts = -1;
    s->state = MEDIA_LOADING;
    __sync_synchronize();
    s->used = 1;
    if (g_task < 0) g_task = task_create_stack("media", media_task, 256u << 10);
    return id;
}

int media_owned(int id, void* owner) { stream_t* s = get(id); return s && s->owner == owner; }

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
    if (s->state == MEDIA_READY) {
        st->dur_ms = s->info.duration_ms > 0 ? (uint32_t)s->info.duration_ms : 0;
        st->pos_ms = s->seek_pending ? s->seek_ms : position(s);
    }
    return 0;
}

void* media_frame(int id, uint32_t have_seq, uint32_t* bytes) {
    stream_t* s = get(id);
    if (!s) return NULL;
    spin_lock(&s->lock);
    void* out = NULL;
    if (s->px && s->pic_seq != have_seq) {
        uint32_t n = (uint32_t)s->pw * (uint32_t)s->ph * 4u;
        out = kmalloc(sizeof(ww_media_frame_t) + n);
        if (out) {
            ww_media_frame_t f = { s->pic_seq, s->pw, s->ph };
            memcpy(out, &f, sizeof(f));
            memcpy((uint8_t*)out + sizeof(f), s->px, n);
            *bytes = sizeof(f) + n;
        }
    }
    spin_unlock(&s->lock);
    return out;
}
