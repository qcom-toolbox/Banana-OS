/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <SDL2/SDL.h>
#include <banana.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

/* Single-threaded, one-window compatibility experiment; see README.md. */
struct SDL_Window { bwin_t native; SDL_Surface surface; };
static SDL_Window *window;
static Uint32 initialized;
static char error[192];
static SDL_Event pending;
static int has_pending;
static int clock_started;
static Uint32 clock_previous;
static Uint64 clock_elapsed;
static SDL_PixelFormat rgb = { SDL_PIXELFORMAT_RGB888, 32, 4,
                               0x00ff0000, 0x0000ff00, 0x000000ff, 0 };
const char *SDL_GetError(void) { return error; }
void SDL_ClearError(void) { error[0] = 0; }
int SDL_SetError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vsnprintf(error, sizeof(error), fmt, ap); va_end(ap);
    return -1;
}
static Uint64 monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return (Uint64)t.tv_sec * 1000 + (Uint64)t.tv_nsec / 1000000;
}
Uint64 SDL_GetTicks64(void) {
    Uint32 now = (Uint32)monotonic_ms();
    if (!clock_started) { clock_previous = now; clock_elapsed = 0; clock_started = 1; }
    clock_elapsed += (Uint32)(now - clock_previous);
    clock_previous = now;
    return clock_elapsed;
}
Uint32 SDL_GetTicks(void) { return (Uint32)SDL_GetTicks64(); }
void SDL_Delay(Uint32 ms) { banana_sleep(ms); }
int SDL_InitSubSystem(Uint32 flags) {
    if (flags & ~(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS))
        return SDL_SetError("Unsupported subsystem in the experimental SDL2 subset");
    (void)SDL_GetTicks64();
    initialized |= flags;
    return 0;
}
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }
Uint32 SDL_WasInit(Uint32 flags) { return flags ? initialized & flags : initialized; }
void SDL_Quit(void) {
    SDL_DestroyWindow(window);
    initialized = 0; has_pending = 0; clock_started = 0;
}
static int dimensions(int w, int h) {
    return w > 0 && h > 0 && w <= INT_MAX / 4 &&
           (size_t)h <= (64u * 1024u * 1024u) / ((size_t)w * 4);
}
static void setup_surface(SDL_Surface *s, int w, int h, void *pixels, Uint32 flags) {
    memset(s, 0, sizeof(*s));
    s->flags = flags; s->format = &rgb; s->w = w; s->h = h;
    s->pitch = w * 4; s->pixels = pixels;
    s->clip_rect = (SDL_Rect){0, 0, w, h};
}
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    (void)flags;
    if (!dimensions(w, h) || depth != 32 || a ||
            !((!r && !g && !b) || (r == rgb.Rmask && g == rgb.Gmask && b == rgb.Bmask))) {
        SDL_SetError("Only bounded opaque 32-bit XRGB surfaces are supported"); return NULL;
    }
    SDL_Surface *s = malloc(sizeof(*s));
    if (!s) { SDL_SetError("Out of memory for surface"); return NULL; }
    void *pixels = calloc((size_t)w * (size_t)h, 4);
    if (!pixels) { free(s); SDL_SetError("Out of memory for pixels"); return NULL; }
    setup_surface(s, w, h, pixels, 0);
    return s;
}
void SDL_FreeSurface(SDL_Surface *s) {
    if (!s || (window && s == &window->surface)) return;
    free(s->pixels); free(s);
}
static int valid_surface(const SDL_Surface *s) {
    return s && dimensions(s->w, s->h) && s->pixels && s->format &&
           s->format->format == SDL_PIXELFORMAT_RGB888 &&
           s->pitch >= s->w * 4 && !(s->pitch & 3) &&
           (size_t)s->h <= SIZE_MAX / (size_t)s->pitch;
}
/* Public clip rectangles are intersected with the actual allocation. */
static void clip_bounds(const SDL_Surface *s, int64_t *left, int64_t *top,
                        int64_t *right, int64_t *bottom) {
    *left = s->clip_rect.x > 0 ? s->clip_rect.x : 0;
    *top = s->clip_rect.y > 0 ? s->clip_rect.y : 0;
    *right = (int64_t)s->clip_rect.x + s->clip_rect.w;
    *bottom = (int64_t)s->clip_rect.y + s->clip_rect.h;
    if (*right > s->w) *right = s->w;
    if (*bottom > s->h) *bottom = s->h;
    if (s->clip_rect.w <= 0 || s->clip_rect.h <= 0) *right = *left, *bottom = *top;
}
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *r, Uint32 color) {
    if (!valid_surface(s)) return SDL_SetError("Invalid surface");
    int64_t x = r ? r->x : 0, y = r ? r->y : 0;
    int64_t right = x + (r ? r->w : s->w), bottom = y + (r ? r->h : s->h);
    int64_t cl, ct, cr, cb;
    clip_bounds(s, &cl, &ct, &cr, &cb);
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (right > cr) right = cr;
    if (bottom > cb) bottom = cb;
    for (int64_t row = y; row < bottom; row++) {
        Uint32 *pixels = (Uint32 *)((Uint8 *)s->pixels + (size_t)row * s->pitch);
        for (int64_t col = x; col < right; col++) pixels[col] = color & 0x00ffffffu;
    }
    return 0;
}
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *r, SDL_Surface *dst, SDL_Rect *target) {
    if (!valid_surface(src) || !valid_surface(dst)) return SDL_SetError("Invalid blit surface");
    int64_t sx = r ? r->x : 0, sy = r ? r->y : 0;
    int64_t w = r ? r->w : src->w, h = r ? r->h : src->h;
    int64_t dx = target ? target->x : 0, dy = target ? target->y : 0;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    int64_t cl, ct, cr, cb;
    clip_bounds(dst, &cl, &ct, &cr, &cb);
    if (dx < cl) { int64_t n = cl - dx; w -= n; sx += n; dx = cl; }
    if (dy < ct) { int64_t n = ct - dy; h -= n; sy += n; dy = ct; }
    if (w > src->w - sx) w = src->w - sx;
    if (w > cr - dx) w = cr - dx;
    if (h > src->h - sy) h = src->h - sy;
    if (h > cb - dy) h = cb - dy;
    if (w <= 0 || h <= 0) { if (target) target->w = target->h = 0; return 0; }
    int backwards = src->pixels == dst->pixels && dy > sy;
    for (int64_t i = 0; i < h; i++) {
        int64_t row = backwards ? h - i - 1 : i;
        memmove((Uint8 *)dst->pixels + (size_t)(dy + row) * dst->pitch + (size_t)dx * 4,
                (Uint8 *)src->pixels + (size_t)(sy + row) * src->pitch + (size_t)sx * 4, (size_t)w * 4);
    }
    if (target) *target = (SDL_Rect){(int)dx, (int)dy, (int)w, (int)h};
    return 0;
}
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    if (!(initialized & SDL_INIT_VIDEO) || window || !dimensions(w, h) ||
            (flags & ~(SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)) ||
            (x != SDL_WINDOWPOS_UNDEFINED && x != SDL_WINDOWPOS_CENTERED) ||
            (y != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_CENTERED)) {
        SDL_SetError("Requires video init, one window, default position and supported flags"); return NULL;
    }
    SDL_Window *win = calloc(1, sizeof(*win));
    if (!win) { SDL_SetError("Out of memory for window"); return NULL; }
    if (bwin_open(&win->native, title ? title : "SDL2 subset", w, h) < 0) {
        free(win); SDL_SetError("Banana window creation failed"); return NULL;
    }
    if ((flags & SDL_WINDOW_RESIZABLE) && bwin_resizable(&win->native, 32, 32) < 0) {
        bwin_close(&win->native); free(win); SDL_SetError("Resizable windows unavailable"); return NULL;
    }
    bwin_wheel(&win->native);
    window = win;
    setup_surface(&win->surface, win->native.w, win->native.h, win->native.px, 1);
    return win;
}
void SDL_DestroyWindow(SDL_Window *win) {
    if (!win || win != window) return;
    bwin_close(&win->native); free(win); window = NULL; has_pending = 0;
}
Uint32 SDL_GetWindowID(SDL_Window *win) { return win && win == window ? (Uint32)win->native.id + 1u : 0; }
void SDL_GetWindowSize(SDL_Window *win, int *w, int *h) {
    if (!win || win != window) return;
    if (w) *w = win->native.w;
    if (h) *h = win->native.h;
}
void SDL_SetWindowTitle(SDL_Window *win, const char *title) {
    if (win && win == window && title) bwin_title(&win->native, title);
}
SDL_Surface *SDL_GetWindowSurface(SDL_Window *win) {
    if (!win || win != window) return NULL;
    if (win->surface.w != win->native.w || win->surface.h != win->native.h ||
            win->surface.pixels != win->native.px)
        setup_surface(&win->surface, win->native.w, win->native.h, win->native.px, 1);
    return &win->surface;
}
int SDL_UpdateWindowSurface(SDL_Window *win) {
    if (!win || win != window) return SDL_SetError("Invalid window");
    bwin_update(&win->native); return 0;
}
static int translate(const banana_event_t *b, SDL_Event *e) {
    memset(e, 0, sizeof(*e)); e->common.timestamp = SDL_GetTicks();
    switch (b->type) {
    case BANANA_EV_CLOSE: e->type = SDL_QUIT; break;
    case BANANA_EV_RESIZE:
        e->type = SDL_WINDOWEVENT; e->window.event = SDL_WINDOWEVENT_RESIZED;
        e->window.windowID = SDL_GetWindowID(window);
        e->window.data1 = window->native.w; e->window.data2 = window->native.h;
        (void)SDL_GetWindowSurface(window); break;
    case BANANA_EV_FOCUS:
        e->type = SDL_WINDOWEVENT; e->window.windowID = SDL_GetWindowID(window);
        e->window.event = b->x ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_FOCUS_LOST; break;
    case BANANA_EV_KEY:
        /* Only ASCII key presses: no invented KEYUP or scancode support. */
        if (b->key < 0 || b->key > 127) return 0;
        e->type = SDL_KEYDOWN; e->key.windowID = SDL_GetWindowID(window);
        e->key.state = SDL_PRESSED; e->key.keysym.sym = b->key; break;
    case BANANA_EV_MOUSE_MOVE:
        e->type = SDL_MOUSEMOTION; e->motion.windowID = SDL_GetWindowID(window);
        e->motion.x = b->x; e->motion.y = b->y; break;
    case BANANA_EV_MOUSE_DOWN: case BANANA_EV_MOUSE_UP:
        if (b->button < 1 || b->button > 3) return 0;
        e->type = b->type == BANANA_EV_MOUSE_DOWN ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e->button.windowID = SDL_GetWindowID(window);
        e->button.state = b->type == BANANA_EV_MOUSE_DOWN ? SDL_PRESSED : SDL_RELEASED;
        e->button.button = b->button == 2 ? SDL_BUTTON_RIGHT : b->button == 3 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
        e->button.clicks = 1; e->button.x = b->x; e->button.y = b->y; break;
    case BANANA_EV_WHEEL:
        e->type = SDL_MOUSEWHEEL; e->wheel.windowID = SDL_GetWindowID(window); /* Banana's positive direction is toward the user; SDL's is away. */
        e->wheel.y = b->y == INT_MIN ? INT_MAX : -b->y; break;
    default: return 0;
    }
    return 1;
}
int SDL_PollEvent(SDL_Event *e) {
    if (!has_pending && window) {
        banana_event_t b;
        while (bwin_event(&window->native, &b)) {
            if (translate(&b, &pending)) { has_pending = 1; break; }
        }
    }
    if (!has_pending) return 0;
    if (e) { *e = pending; has_pending = 0; }
    return 1;
}
int SDL_WaitEventTimeout(SDL_Event *e, int timeout) {
    Uint64 start = SDL_GetTicks64();
    do {
        if (SDL_PollEvent(e)) return 1;
        if (!window || timeout == 0) return 0;
        if (timeout > 0 && SDL_GetTicks64() - start >= (Uint64)timeout) return 0;
        SDL_Delay(1);
    } while (1);
}
