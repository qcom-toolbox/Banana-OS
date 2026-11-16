/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BANANA_WEB_WORKER_H
#define BANANA_WEB_WORKER_H
/* Versioned wire data only: no pointers or native size_t values. */
#define WW_VERSION 1u
#define WW_CHUNK 60000u
#define WW_DOCUMENT (8u << 20)
#define WW_PIXELS 8000000u
#define WW_POST (1u << 20)
#define WW_REQUEST_BODY (48u << 10)
enum { WW_WAIT=1, WW_INPUT, WW_OUTPUT, WW_FINISH, WW_RESOURCE, WW_RESPONSE };
enum { WW_INIT=1, WW_LOAD, WW_UPDATE, WW_CLICK, WW_KEY, WW_LIVE, WW_TICK,
       WW_LINK, WW_PICK, WW_RENDER, WW_TEXTPOS, WW_TEXTRANGE, WW_EVAL, WW_POSTMSG,
       WW_DECODE, WW_TEST,
       /* a worker started for one <audio> / <video> stream (kernel/media.c,
        * apps/webworker/media.c): the file, then sound and pictures */
       WW_MEDIA_OPEN, WW_MEDIA_AUDIO, WW_MEDIA_VIDEO, WW_MEDIA_SEEK,
       /* a worker running one page's WebGL context on the GPU (apps/webgl):
        * a[0] a[1] the drawing buffer's size; a command stream in, results out */
       WW_GL_INIT, WW_GL_EXEC };
/* WW_GL_EXEC: a[0] = 1 to have the drawing buffer read back after the
 * commands (RGBA, top row first, width * height * 4 bytes after the result) */
typedef struct { int result_bytes, width, height, gl_error; } ww_gl_reply_t;
#define WW_GL_COMMANDS (32u << 20)    /* a context's queued commands, at most */
#define WW_MEDIA_BYTES (96u << 20)    /* the largest media file a stream takes */
#define WW_MEDIA_RATE 48000           /* sound comes as 16-bit stereo at this rate */
/* WW_MEDIA_OPEN's result */
typedef struct { int has_audio, has_video, width, height, duration_ms; } ww_media_info_t;
/* WW_MEDIA_AUDIO (a[0]: frames wanted; a[1]: the time from which it is wanted, ms):
 * this, then frames * 2 int16 */
typedef struct { int start_ms, frames, eof; } ww_media_audio_t;
/* WW_MEDIA_VIDEO (a[0]: the time, ms; a[1]: the picture already had, its pts + 1):
 * this, then width * height XRGB pixels when there is a newer one */
typedef struct { int pts_ms, width, height, eof; } ww_media_picture_t;
/* the page's media requests (WW_MEDIA resources, arg = op) */
enum { WWM_OPEN = 1, WWM_CMD, WWM_STATUS, WWM_FRAME };
/* the page's WebGL requests (WW_GL resources, arg = op): OPEN (body: w, h) -> id;
 * APPEND (body: id, then command bytes); EXEC (body: id, read back?) -> the
 * worker's reply; CLOSE (body: id) */
enum { WWG_OPEN = 1, WWG_APPEND, WWG_EXEC, WWG_CLOSE };
typedef struct { int state, playing, ended; unsigned int pos_ms, dur_ms, seq; char error[96]; } ww_media_status_t;
typedef struct { unsigned int seq; int width, height; } ww_media_frame_t;
enum { WW_RESULT=0, WW_NAVBODY=1 };
enum { WW_FETCH=1, WW_REQUEST, WW_COOKIE_GET, WW_COOKIE_SET, WW_FILE, WW_MESSAGE, WW_FONT, WW_MEDIA, WW_GL };
typedef struct {
    unsigned int version, op, length;
    int a[12];
    char url[8192];
} ww_command_t;
typedef struct { unsigned int offset, kind; } ww_io_t;
typedef struct {
    unsigned int version;
    int result, height, dirty, focus, textarea, js, live_keys, oom;
    int alert_pending, nav_pending, nav_newtab, file_pick_pending, scroll_req;
    unsigned int nav_post_len;
    char title[128], status[200], alert[256], nav[8192], nav_post_type[96];
} ww_state_t;
typedef struct {
    unsigned int kind, arg, length;
    char url[8192], method[8], type[96];
    unsigned char body[WW_REQUEST_BODY];
} ww_resource_t;
typedef struct {
    int result;
    unsigned int length;
    char type[96], url[8192], error[160];
} ww_response_t;
#endif
