/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Camera - the webcam and the microphone.
 *
 * The webcam's picture live (a USB webcam: YUY2 pictures come as pixels,
 * MJPEG ones as JPEG files this app decodes), the microphone's level, and:
 *   Space, "Take photo"     a picture into ~/Pictures (Camera-<date>.bmp)
 *   R, "Record"             a voice memo into ~/Recordings (Memo-<date>.wav)
 *                           until "Stop" (10 minutes at most)
 *   M                       mirror the picture (as a mirror shows you)
 * Either works without the other: a computer with a microphone but no
 * webcam still records.
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

#define BAR_H   56
#define C_BG    0x101318u
#define C_BAR   0x1D232Cu
#define C_TEXT  0xE8EEF6u
#define C_DIM   0x9AA6B6u
#define C_REC   0xE5484Du
#define C_LEVEL 0x4CC38Au
#define MEMO_MAX (48000 * 600)              /* 10 minutes (mono frames) */

static bwin_t win;
static int    has_api, cam_on, mic_on, mirror = 1;
static unsigned char* raw;                  /* what the camera gives */
static unsigned int   raw_cap;
static unsigned int*  pic;                  /* the picture shown, 0x00RRGGBB */
static int    pw, ph;
static unsigned int seq;
static char   cam_name[96], mic_name[96], status[160];
static unsigned int status_until;
static short* memo;                         /* a voice memo being recorded */
static unsigned int memo_n;
static int    recording;
static unsigned int rec_start;
static int    level;
static unsigned int last_try;

static unsigned int now_ms(void) { return __banana->ticks_ms(); }

static void say(const char* s) {
    snprintf(status, sizeof(status), "%s", s);
    status_until = now_ms() + 4000;
}

/* ~/<sub>/<prefix>-<date>.<ext> (the folder made if it is not there) */
static void out_path(char* out, int cap, const char* sub, const char* prefix, const char* ext) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/home/banana";
    char d[300];
    snprintf(d, sizeof(d), "%s/%s", home, sub);
    mkdir(d, 0755);
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    snprintf(out, cap, "%s/%s-%04d%02d%02d-%02d%02d%02d.%s", d, prefix, tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, ext);
}

static void put32(unsigned char* p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

/* ── the camera ── */
static void cam_try_open(void) {
    if (cam_on || !has_api) return;
    banana_camframe_t info;
    if (__banana->camera(BANANA_CAM_OPEN, NULL, 0, &info) != 0) return;
    if (raw_cap < info.bytes) {
        free(raw);
        raw = malloc(info.bytes);
        raw_cap = raw ? info.bytes : 0;
    }
    if (!raw) { __banana->camera(BANANA_CAM_CLOSE, NULL, 0, NULL); return; }
    __banana->camera(BANANA_CAM_NAME, cam_name, sizeof(cam_name), NULL);
    cam_on = 1;
    seq = 0;
}

/* the newest picture, if one came: 1 */
static int cam_grab(void) {
    if (!cam_on) return 0;
    banana_camframe_t f;
    int rc = __banana->camera(BANANA_CAM_FRAME, raw, raw_cap, &f);
    if (rc < 0) {                           /* unplugged: try again now and then */
        __banana->camera(BANANA_CAM_CLOSE, NULL, 0, NULL);
        cam_on = 0;
        free(pic);
        pic = NULL;
        return 1;
    }
    if (rc == 0) return 0;
    seq = f.seq;
    if (f.format == BANANA_CAM_XRGB) {
        if (!pic || pw != f.width || ph != f.height) {
            free(pic);
            pic = malloc((size_t)f.width * f.height * 4);
            if (!pic) return 0;
        }
        pw = f.width;
        ph = f.height;
        memcpy(pic, raw, (size_t)pw * ph * 4);
        return 1;
    }
    int w, h, comp;
    unsigned char* rgb = stbi_load_from_memory(raw, (int)f.bytes, &w, &h, &comp, 3);
    if (!rgb) return 0;                     /* (a damaged picture: the next one comes soon) */
    if (!pic || pw != w || ph != h) {
        free(pic);
        pic = malloc((size_t)w * h * 4);
        if (!pic) { stbi_image_free(rgb); return 0; }
    }
    pw = w;
    ph = h;
    for (int i = 0; i < w * h; i++) pic[i] = (unsigned int)rgb[i * 3] << 16 | (unsigned int)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
    stbi_image_free(rgb);
    return 1;
}

static void take_photo(void) {
    if (!pic) { say("No picture to take: is a webcam plugged in?"); return; }
    char path[400];
    out_path(path, sizeof(path), "Pictures", "Camera", "bmp");
    unsigned int img = (unsigned int)pw * ph * 4, size = 54 + img;
    unsigned char h[54];
    memset(h, 0, sizeof(h));
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, size);
    h[10] = 54; h[14] = 40;
    put32(h + 18, (unsigned int)pw);
    put32(h + 22, (unsigned int)ph);
    h[26] = 1; h[28] = 32;
    put32(h + 34, img);
    FILE* f = fopen(path, "wb");
    if (!f) { say("The photo could not be saved"); return; }
    fwrite(h, 1, 54, f);
    for (int y = ph - 1; y >= 0; y--) {     /* (BMP rows: bottom up; as shown, mirrored or not) */
        unsigned int* row = pic + (long)y * pw;
        if (mirror) {
            static unsigned int tmp[4096];
            for (int x = 0; x < pw && x < 4096; x++) tmp[x] = row[pw - 1 - x];
            fwrite(tmp, 4, (size_t)pw, f);
        } else fwrite(row, 4, (size_t)pw, f);
    }
    fclose(f);
    char m[440];
    snprintf(m, sizeof(m), "Saved %s", path);
    say(m);
}

/* ── the microphone ── */
static void mic_try_open(void) {
    if (mic_on || !has_api) return;
    if (__banana->mic(BANANA_MIC_OPEN, NULL, 0) <= 0) return;
    __banana->mic(BANANA_MIC_NAME, mic_name, sizeof(mic_name));
    mic_on = 1;
}

static void mic_pump(void) {
    if (!mic_on) return;
    static short buf[4096];
    for (;;) {
        int n = __banana->mic(BANANA_MIC_READ_MONO, buf, sizeof(buf));
        if (n < 0) { __banana->mic(BANANA_MIC_CLOSE, NULL, 0); mic_on = 0; return; }
        if (n == 0) break;
        if (recording && memo) {
            unsigned int take = memo_n + (unsigned)n > MEMO_MAX ? MEMO_MAX - memo_n : (unsigned)n;
            memcpy(memo + memo_n, buf, take * 2);
            memo_n += take;
        }
    }
    level = __banana->mic(BANANA_MIC_LEVEL, NULL, 0);
}

static void stop_recording(void) {
    recording = 0;
    if (!memo) return;
    char path[400];
    out_path(path, sizeof(path), "Recordings", "Memo", "wav");
    unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                            0x80,0xBB,0,0, 0x00,0x77,0x01,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    put32(h + 4, 36 + memo_n * 2);
    put32(h + 40, memo_n * 2);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(h, 1, 44, f);
        fwrite(memo, 2, memo_n, f);
        fclose(f);
        char m[460];
        snprintf(m, sizeof(m), "Saved %s (%u s)", path, memo_n / 48000);
        say(m);
    } else say("The recording could not be saved");
    free(memo);
    memo = NULL;
}

static void toggle_recording(void) {
    if (recording) { stop_recording(); return; }
    if (!mic_on) { say("No microphone"); return; }
    memo = malloc((size_t)MEMO_MAX * 2);
    if (!memo) { say("Not enough memory to record"); return; }
    memo_n = 0;
    recording = 1;
    rec_start = now_ms();
}

/* ── drawing ── */
#define BTN_Y(winh) ((winh) - BAR_H + 14)

static void draw(void) {
    int vh = win.h - BAR_H;
    bwin_fill_rect(&win, 0, 0, win.w, vh, C_BG);
    if (pic && pw > 0 && ph > 0) {
        /* fitted, the aspect kept */
        int dw = win.w, dh = (int)((long)ph * win.w / pw);
        if (dh > vh) { dh = vh; dw = (int)((long)pw * vh / ph); }
        int ox = (win.w - dw) / 2, oy = (vh - dh) / 2;
        for (int y = 0; y < dh; y++) {
            const unsigned int* src = pic + (long)(y * ph / dh) * pw;
            unsigned int* dst = win.px + (long)(oy + y) * win.w + ox;
            for (int x = 0; x < dw; x++) {
                int sx = x * pw / dw;
                dst[x] = src[mirror ? pw - 1 - sx : sx];
            }
        }
    } else {
        const char* m = !has_api ? "This Banana OS is too old for the camera (it needs API version 34)."
                                 : "No webcam - plug in a USB webcam.";
        int tw = banana_font_width(BANANA_FONT_SANS, 15, m);
        bwin_font(&win, (win.w - tw) / 2, vh / 2 - 10, BANANA_FONT_SANS, 15, m, C_DIM);
    }
    if (recording) {
        bwin_fill_circle(&win, 22, 22, 7, C_REC);
        char t[32];
        unsigned int s = (now_ms() - rec_start) / 1000;
        snprintf(t, sizeof(t), "%u:%02u", s / 60, s % 60);
        bwin_font(&win, 36, 14, BANANA_FONT_SANS_BOLD, 15, t, C_TEXT);
    }
    /* the bar: buttons, the microphone's level, what happened */
    int y = vh;
    bwin_fill_rect(&win, 0, y, win.w, BAR_H, C_BAR);
    bwin_fill_rect(&win, 0, y, win.w, 1, 0x2C3440u);
    int by = BTN_Y(win.h);
    bwin_button(&win, 10, by, 110, 28, "Take photo", 0);
    bwin_button(&win, 126, by, 90, 28, recording ? "Stop" : "Record", 0);
    int mx = 230, mw = 120;
    bwin_rect(&win, mx, by + 9, mw, 10, 0x3A4452u);
    int lv = mic_on ? level * (mw - 2) / 32768 : 0;
    bwin_fill_rect(&win, mx + 1, by + 10, lv, 8, level > 30000 ? C_REC : C_LEVEL);
    char line[200];
    if (status[0] && now_ms() < status_until) snprintf(line, sizeof(line), "%s", status);
    else snprintf(line, sizeof(line), "%s%s%s", cam_on ? cam_name : "no webcam", "  |  mic: ", mic_on ? mic_name : "none");
    bwin_font(&win, mx + mw + 12, by + 6, BANANA_FONT_SANS, 13, line, C_DIM);
    bwin_update(&win);
}

int main(void) {
    if (bwin_open(&win, "Camera", 680, 540) != 0) {
        printf("camera: the desktop is not running\n");
        return 1;
    }
    bwin_resizable(&win, 420, 300);
    has_api = __banana->version >= 34 && BANANA_HAS(__banana, camera);
    cam_try_open();
    mic_try_open();
    draw();
    banana_event_t ev;
    unsigned int last_draw = 0;
    for (;;) {
        int got = bwin_wait_event(&win, &ev, 15);
        if (got && ev.type == BANANA_EV_CLOSE) break;
        if (got && ev.type == BANANA_EV_KEY) {
            if (ev.key == ' ') take_photo();
            else if (ev.key == 'r' || ev.key == 'R') toggle_recording();
            else if (ev.key == 'm' || ev.key == 'M') mirror = !mirror;
            else if (ev.key == 27) break;
        }
        if (got && ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1) {
            int by = BTN_Y(win.h);
            if (ev.y >= by && ev.y < by + 28) {
                if (ev.x >= 10 && ev.x < 120) take_photo();
                else if (ev.x >= 126 && ev.x < 216) toggle_recording();
            }
        }
        if (now_ms() - last_try > 1000) {   /* a webcam / microphone plugged in later */
            last_try = now_ms();
            cam_try_open();
            mic_try_open();
        }
        int fresh = cam_grab();
        mic_pump();
        if (recording && memo_n >= MEMO_MAX) stop_recording();
        if (fresh || got || now_ms() - last_draw > 100) { draw(); last_draw = now_ms(); }
    }
    if (recording) stop_recording();
    if (cam_on) __banana->camera(BANANA_CAM_CLOSE, NULL, 0, NULL);
    if (mic_on) __banana->mic(BANANA_MIC_CLOSE, NULL, 0);
    free(pic);
    free(raw);
    bwin_close(&win);
    return 0;
}
