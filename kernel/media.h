/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEDIA_H
#define MEDIA_H

#include "types.h"

/*
 * The browser's <audio> / <video> / new Audio() streams: a stream downloads
 * a file and a restricted worker process decodes it with FFmpeg (MP4, WebM,
 * MKV, MP3, AAC, Ogg, FLAC... - apps/webworker/media.c); a "media" task
 * plays its sound and keeps its newest picture. One stream is heard at a
 * time. owner is the page (closed with it, and the only one that may
 * control or see its streams: media_owned).
 */

enum { MEDIA_EMPTY = 0, MEDIA_LOADING, MEDIA_READY, MEDIA_ERROR };

typedef struct {
    int      state;         /* MEDIA_* */
    int      playing;       /* 1 while it plays (not paused, not ended) */
    int      ended;         /* played to its end (and not looping) */
    uint32_t pos_ms, dur_ms;
    uint32_t seq;           /* bumps when a seek lands / it loops (timeupdate, seeked) */
    char     error[96];
} media_status_t;

int  media_open(void* owner, const char* url);        /* stream id, or -1 */
void media_close(int id);
void media_close_owner(void* owner);
void media_play(int id);
void media_pause(int id);
void media_seek(int id, uint32_t ms);
void media_set(int id, int volume_percent, int muted, int loop);
int  media_status(int id, media_status_t* st);         /* 0, or -1 for a bad id */
int  media_owned(int id, void* owner);
/* the newest picture if it is not have_seq: a kmalloc'd ww_media_frame_t and
 * its XRGB pixels (*bytes long), or NULL */
void* media_frame(int id, uint32_t have_seq, uint32_t* bytes);

#endif
