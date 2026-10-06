/* Banana OS SDK: windows, drawing, time, sound, network, clipboard */
#include <stdlib.h>
#include <string.h>
#include "banana.h"

void __stdio_flush_all(void);

/* ── windows ──────────────────────────────────────────────────────── */

int bwin_open(bwin_t* win, const char* title, int w, int h) {
    win->id = __banana->win_open(title, w, h);
    if (win->id < 0) { win->px = NULL; win->w = win->h = 0; return -1; }
    __banana->win_size(win->id, &win->w, &win->h);
    win->px = __banana->win_pixels(win->id);
    return 0;
}

void bwin_close(bwin_t* win) {
    if (win->id >= 0) __banana->win_close(win->id);
    win->id = -1;
    win->px = NULL;
}

void bwin_title(bwin_t* win, const char* title) { if (win->id >= 0) __banana->win_set_title(win->id, title); }
void bwin_update(bwin_t* win) { if (win->id >= 0) __banana->win_update(win->id); }
int bwin_event(bwin_t* win, banana_event_t* ev) {
    if (win->id < 0 || !__banana->win_event(win->id, ev)) return 0;
    if (ev->type == BANANA_EV_RESIZE) {
        /* the system gave the window a new buffer: use it from now on */
        __banana->win_size(win->id, &win->w, &win->h);
        win->px = __banana->win_pixels(win->id);
    }
    return 1;
}

int bwin_resizable(bwin_t* win, int min_w, int min_h) {
    /* a call of API version 2: older systems do not have it */
    if (win->id < 0 || __banana->size < __builtin_offsetof(banana_api_t, win_set_resizable) + sizeof(void*)) return -1;
    __banana->win_set_resizable(win->id, min_w, min_h);
    return 0;
}

int bwin_wait_event(bwin_t* win, banana_event_t* ev, int timeout_ms) {
    unsigned int start = __banana->ticks_ms();
    for (;;) {
        if (bwin_event(win, ev)) return 1;
        if (timeout_ms >= 0 && (int)(__banana->ticks_ms() - start) >= timeout_ms) return 0;
        __banana->sleep_ms(10);
    }
}

void bwin_clear(bwin_t* win, unsigned int c) {
    if (!win->px) return;
    for (int i = 0; i < win->w * win->h; i++) win->px[i] = c;
}

void bwin_pixel(bwin_t* win, int x, int y, unsigned int c) {
    if (win->px && x >= 0 && y >= 0 && x < win->w && y < win->h) win->px[y * win->w + x] = c;
}

unsigned int bwin_get_pixel(bwin_t* win, int x, int y) {
    if (win->px && x >= 0 && y >= 0 && x < win->w && y < win->h) return win->px[y * win->w + x];
    return 0;
}

void bwin_fill_rect(bwin_t* win, int x, int y, int w, int h, unsigned int c) {
    if (!win->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    for (int yy = y; yy < y + h; yy++) {
        unsigned int* row = win->px + yy * win->w;
        for (int xx = x; xx < x + w; xx++) row[xx] = c;
    }
}

void bwin_rect(bwin_t* win, int x, int y, int w, int h, unsigned int c) {
    bwin_fill_rect(win, x, y, w, 1, c);
    bwin_fill_rect(win, x, y + h - 1, w, 1, c);
    bwin_fill_rect(win, x, y, 1, h, c);
    bwin_fill_rect(win, x + w - 1, y, 1, h, c);
}

void bwin_line(bwin_t* win, int x0, int y0, int x1, int y1, unsigned int c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        bwin_pixel(win, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void bwin_circle(bwin_t* win, int cx, int cy, int r, unsigned int c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        bwin_pixel(win, cx + x, cy + y, c); bwin_pixel(win, cx - x, cy + y, c);
        bwin_pixel(win, cx + x, cy - y, c); bwin_pixel(win, cx - x, cy - y, c);
        bwin_pixel(win, cx + y, cy + x, c); bwin_pixel(win, cx - y, cy + x, c);
        bwin_pixel(win, cx + y, cy - x, c); bwin_pixel(win, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void bwin_fill_circle(bwin_t* win, int cx, int cy, int r, unsigned int c) {
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        bwin_fill_rect(win, cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

void bwin_text(bwin_t* win, int x, int y, const char* s, unsigned int fg, unsigned int bg) {
    if (win->px) __banana->draw_text(win->px, win->w, win->w, win->h, x, y, s, fg, bg);
}

void bwin_text_scaled(bwin_t* win, int x, int y, int scale, const char* s, unsigned int fg, unsigned int bg) {
    if (scale <= 1) { bwin_text(win, x, y, s, fg, bg); return; }
    const unsigned char* font = __banana->font8x8;
    for (; *s; s++, x += 8 * scale) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 128) ch = '?';
        const unsigned char* g = font + ch * 8;
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                int on = (g[gy] >> gx) & 1;
                if (on) bwin_fill_rect(win, x + gx * scale, y + gy * scale, scale, scale, fg);
                else if (bg != BANANA_TRANSPARENT) bwin_fill_rect(win, x + gx * scale, y + gy * scale, scale, scale, bg);
            }
    }
}

void bwin_button(bwin_t* win, int x, int y, int w, int h, const char* label, int pressed) {
    unsigned int base = pressed ? 0x252B33u : 0x303740u;
    bwin_fill_rect(win, x, y, w, h, base);
    bwin_fill_rect(win, x, y, w, 1, pressed ? 0x141920u : 0x535D6Eu);
    bwin_fill_rect(win, x, y, 1, h, pressed ? 0x141920u : 0x535D6Eu);
    bwin_fill_rect(win, x, y + h - 1, w, 1, pressed ? 0x535D6Eu : 0x15191Fu);
    bwin_fill_rect(win, x + w - 1, y, 1, h, pressed ? 0x535D6Eu : 0x15191Fu);
    int tw = (int)strlen(label) * 8;
    bwin_text(win, x + (w - tw) / 2, y + (h - 8) / 2, label, 0xE8EEF6u, BANANA_TRANSPARENT);
}

void bwin_blit(bwin_t* win, int x, int y, const unsigned int* img, int w, int h) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) bwin_pixel(win, x + xx, y + yy, img[yy * w + xx]);
}

/* ── time / terminal ──────────────────────────────────────────────── */

unsigned int banana_ticks(void) { return __banana->ticks_ms(); }
void banana_sleep(unsigned int ms) { __stdio_flush_all(); __banana->sleep_ms(ms); }
void banana_yield(void) { __banana->yield(); }
void banana_time(banana_time_t* t) { __banana->localtime(t); }
unsigned int banana_random(void) { return __banana->random(); }
void banana_color(int fg, int bg) { __stdio_flush_all(); __banana->set_color(fg, bg); }
void banana_clear_screen(void) { __stdio_flush_all(); __banana->clear_screen(); }
void banana_cursor(int row, int col) { __stdio_flush_all(); __banana->set_cursor(row, col); }
void banana_term_size(int* cols, int* rows) { __banana->term_size(cols, rows); }
int  banana_key(void) { __stdio_flush_all(); return __banana->getchar(); }
int  banana_try_key(void) { __stdio_flush_all(); return __banana->trygetchar(); }
int  banana_interrupted(void) { return __banana->interrupted(); }

/* ── network ──────────────────────────────────────────────────────── */

int banana_http_get(const char* url, char** data, unsigned long* len, char* err, int errcap) {
    return __banana->http_get(url, data, len, NULL, 0, err, errcap);
}

/* ── sound ────────────────────────────────────────────────────────── */

int  banana_play(const void* pcm, unsigned long bytes, int rate, int ch, int bits) {
    return __banana->audio_play(pcm, bytes, rate, ch, bits);
}
int  banana_playing(void) { return __banana->audio_busy(); }
void banana_beep(int hz, int ms) { __banana->audio_beep(hz, ms); }

void banana_tone(int hz, int ms, int volume) {
    const int rate = 22050;
    if (hz <= 0) hz = 1;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int n = rate * ms / 1000;
    short* buf = malloc((size_t)n * 2);
    if (!buf) return;
    int amp = 12000 * volume / 100;
    int period = rate / hz;
    if (period < 2) period = 2;
    for (int i = 0; i < n; i++) {
        int a = (i % period) < period / 2 ? amp : -amp;
        /* 5 ms fade in / out against clicks */
        int edge = rate / 200;
        if (i < edge) a = a * i / edge;
        if (n - i < edge) a = a * (n - i) / edge;
        buf[i] = (short)a;
    }
    if (__banana->audio_play(buf, (unsigned long)n * 2, rate, 1, 16) != 0) __banana->audio_beep(hz, ms);
    free(buf);
}

/* ── clipboard ────────────────────────────────────────────────────── */

void banana_copy(const char* text) { __banana->clipboard_set(text, strlen(text)); }
const char* banana_paste(void) { unsigned long n; return __banana->clipboard_get(&n); }
