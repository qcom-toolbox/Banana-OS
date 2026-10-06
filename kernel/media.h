#ifndef MEDIA_H
#define MEDIA_H

#include "types.h"

/*
 * Sound streams for the browser's <audio> / new Audio() (and web views):
 * a stream downloads a file (http(s)://, file://, a path), opens its codec
 * (media/: WAV, FLAC, MP3) and, while playing, a "media" task decodes it
 * into the sound card's queue. One stream plays at a time: playing one
 * pauses the others. owner groups a page's streams (closed with it).
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

#endif
