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

/* ── threads ──────────────────────────────────────────────────────── */

static int has_threads(void) { return __banana->version >= 3 && __banana->size > (unsigned)((const char*)&__banana->thread_create - (const char*)__banana); }

int  banana_thread(int (*fn)(void*), void* arg) { return has_threads() ? __banana->thread_create(fn, arg) : -1; }
int  banana_join(int id) { return has_threads() ? __banana->thread_join(id) : -1; }
int  banana_thread_id(void) { return has_threads() ? __banana->thread_id() : 0; }
void banana_thread_exit(int ret) { if (has_threads()) __banana->thread_exit(ret); __banana->exit(ret); }
int  banana_cpus(void) { return has_threads() ? __banana->cpu_count() : 1; }

int banana_trylock(banana_mutex_t* m) { return __sync_lock_test_and_set(m, 1) == 0; }

void banana_lock(banana_mutex_t* m) {
    int spins = 0;
    while (__sync_lock_test_and_set(m, 1)) {
        while (*m) {
            if (++spins > 200) { __banana->yield(); spins = 0; }   /* the holder needs the CPU */
            __asm__ volatile("pause");
        }
    }
}

void banana_unlock(banana_mutex_t* m) { __sync_lock_release(m); }

/* ── waiting between threads ──────────────────────────────────────── */

static int has_wait(void) { return __banana->version >= 4 && __banana->size > (unsigned)((const char*)&__banana->wait - (const char*)__banana); }

int banana_wait_value(volatile int* addr, int expected, int timeout_ms) {
    if (has_wait()) return __banana->wait(addr, expected, timeout_ms);
    unsigned end = __banana->ticks_ms() + (unsigned)(timeout_ms > 0 ? timeout_ms : 0);
    while (*addr == expected) {                 /* an older system: poll */
        if (timeout_ms >= 0 && (int)(__banana->ticks_ms() - end) >= 0) return 1;
        __banana->sleep_ms(1);
    }
    return 0;
}

int banana_wake(volatile int* addr, int count) { return has_wait() ? __banana->wake(addr, count) : 0; }

void banana_cond_wait(banana_cond_t* c, banana_mutex_t* m) { banana_cond_timedwait(c, m, -1); }

int banana_cond_timedwait(banana_cond_t* c, banana_mutex_t* m, int ms) {
    int seq = c->seq;
    banana_unlock(m);
    int r = banana_wait_value(&c->seq, seq, ms);
    banana_lock(m);
    return r;
}

void banana_cond_signal(banana_cond_t* c) { __sync_fetch_and_add(&c->seq, 1); banana_wake(&c->seq, 1); }
void banana_cond_broadcast(banana_cond_t* c) { __sync_fetch_and_add(&c->seq, 1); banana_wake(&c->seq, 0); }

void banana_sem_init(banana_sem_t* s, int count) { s->count = count; }

int banana_sem_trywait(banana_sem_t* s) {
    for (;;) {
        int v = s->count;
        if (v <= 0) return 0;
        if (__sync_bool_compare_and_swap(&s->count, v, v - 1)) return 1;
    }
}

int banana_sem_timedwait(banana_sem_t* s, int ms) {
    unsigned end = __banana->ticks_ms() + (unsigned)(ms > 0 ? ms : 0);
    for (;;) {
        if (banana_sem_trywait(s)) return 0;
        int left = -1;
        if (ms >= 0) {
            left = (int)(end - __banana->ticks_ms());
            if (left <= 0) return 1;
        }
        banana_wait_value(&s->count, 0, left);
    }
}

void banana_sem_wait(banana_sem_t* s) { banana_sem_timedwait(s, -1); }

void banana_sem_post(banana_sem_t* s) { __sync_fetch_and_add(&s->count, 1); banana_wake(&s->count, 1); }

/* ── web views (API version 5) ── */
int bweb_available(void) {
    return __banana->version >= 5 && __banana->size >= __builtin_offsetof(banana_api_t, web_post) + sizeof(void*);
}
int  bweb_open(int w, int h) { return bweb_available() ? __banana->web_open(w, h) : -1; }
void bweb_close(int v) { if (bweb_available()) __banana->web_close(v); }
int  bweb_load(int v, const char* url) { return bweb_available() ? __banana->web_load(v, url) : -1; }
int  bweb_html(int v, const char* html, const char* base) { return bweb_available() ? __banana->web_load_html(v, html, base) : -1; }
void bweb_resize(int v, int w, int h) { if (bweb_available()) __banana->web_resize(v, w, h); }
int  bweb_poll(int v) { return bweb_available() ? __banana->web_poll(v) : 0; }
void bweb_draw(int v, bwin_t* win, int x, int y) {
    if (bweb_available() && win->px) __banana->web_draw(v, win->px, win->w, x, y, win->w, win->h);
}
void bweb_event(int v, const banana_event_t* ev) { if (bweb_available()) __banana->web_event(v, ev); }
void bweb_scroll(int v, int dy) { if (bweb_available()) __banana->web_scroll(v, dy); }
void bweb_back(int v) { if (bweb_available()) __banana->web_go(v, -1); }
void bweb_forward(int v) { if (bweb_available()) __banana->web_go(v, 1); }
void bweb_reload(int v) { if (bweb_available()) __banana->web_go(v, 0); }
int  bweb_info(int v, char* t, int tc, char* u, int uc) { return bweb_available() ? __banana->web_info(v, t, tc, u, uc) : -1; }
int  bweb_eval(int v, const char* js, char* out, int cap) { return bweb_available() ? __banana->web_eval(v, js, out, cap) : -1; }
int  bweb_message(int v, char* out, int cap) { return bweb_available() ? __banana->web_message(v, out, cap) : -1; }
int  bweb_post(int v, const char* text) { return bweb_available() ? __banana->web_post(v, text) : -1; }

/* ── fonts (API version 6) ── */
int banana_has_fonts(void) {
    return __banana->version >= 6 && __banana->size >= __builtin_offsetof(banana_api_t, font_metrics) + sizeof(void*);
}
int bwin_font(bwin_t* win, int x, int y, int font, int size, const char* text, unsigned int color) {
    if (!win->px || !text) return x;
    if (banana_has_fonts()) return __banana->font_draw(win->px, win->w, win->w, win->h, x, y, font, size, text, color);
    int scale = size >= 16 ? size / 8 : 1;          /* an older system: the 8x8 font */
    bwin_text_scaled(win, x, y, scale, text, color, BANANA_TRANSPARENT);
    int n = 0;
    while (text[n]) n++;
    return x + n * 8 * scale;
}
int banana_font_width(int font, int size, const char* text) {
    if (banana_has_fonts()) return __banana->font_width(font, size, text);
    int n = 0;
    while (text && text[n]) n++;
    return n * 8 * (size >= 16 ? size / 8 : 1);
}
void banana_font_metrics(int font, int size, int* ascent, int* descent, int* line_h) {
    if (banana_has_fonts()) { __banana->font_metrics(font, size, ascent, descent, line_h); return; }
    int s = size >= 16 ? size / 8 : 1;
    if (ascent) *ascent = 7 * s;
    if (descent) *descent = 1 * s;
    if (line_h) *line_h = 10 * s;
}

static int has_v7(void) {
    return __banana->version >= 7 && __banana->size >= __builtin_offsetof(banana_api_t, audio_volume) + sizeof(void*);
}
unsigned int banana_audio_queued_ms(void) { return has_v7() ? __banana->audio_queued_ms() : 0; }
void banana_audio_stop(void) { if (has_v7()) __banana->audio_stop(); }
int banana_volume(int percent) { return has_v7() ? __banana->audio_volume(percent) : -1; }
void bwin_media_keys(bwin_t* win) {
    if (has_v7() && __banana->size >= __builtin_offsetof(banana_api_t, win_media_keys) + sizeof(void*)) __banana->win_media_keys(win->id);
}

static int has_v8(void) {
    return __banana->version >= 8 && __banana->size >= __builtin_offsetof(banana_api_t, http_request) + sizeof(void*);
}
int banana_http_request(const char* method, const char* url, const char* body, unsigned long body_len,
                        const char* content_type, char** data, unsigned long* len, char* err, int errcap) {
    if (!has_v8()) {
        if (err && errcap > 0) { const char* m = "needs a newer Banana OS"; int i = 0; for (; m[i] && i < errcap - 1; i++) err[i] = m[i]; err[i] = 0; }
        return -1;
    }
    return __banana->http_request(method, url, body, body_len, content_type, data, len, 0, 0, err, errcap);
}
int banana_http_post(const char* url, const char* body, char** data, unsigned long* len, char* err, int errcap) {
    unsigned long n = 0;
    while (body && body[n]) n++;
    return banana_http_request("POST", url, body ? body : "", n, 0, data, len, err, errcap);
}
int banana_url_encode(const char* s, char* out, int cap) {
    static const char HEX[] = "0123456789ABCDEF";
    int n = 0;
    for (; s && *s && n < cap - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else if (n < cap - 3) { out[n++] = '%'; out[n++] = HEX[c >> 4]; out[n++] = HEX[c & 15]; }
        else break;
    }
    if (cap > 0) out[n] = 0;
    return n;
}
