#include "splash.h"
#include "fb.h"
#include "timer.h"
#include "terminal.h"

/*
 * The boot screen. The banana is drawn from geometry here (no picture
 * file): a thick arc - the lower part of a circle - tapering to both ends,
 * shaded from a light top edge to a darker bottom one, with a dark tip on
 * the left and a stem on the right. Under it three dots take turns being
 * lit, from the timer interrupt (splash_tick), so they keep moving while
 * the boot is busy (loading the files, starting the servers). Integer
 * arithmetic only: the dots are drawn in an interrupt.
 */

#define BG        0x00000000u
#define DOT_ON    0x00FFE45Cu
#define DOT_OFF   0x00383838u
#define DOT_MS    350

static volatile int g_active;
static volatile int g_busy;           /* a task is drawing the whole screen */
static volatile int g_phase = -1;     /* the lit dot */
static int g_w, g_h;
static int g_dot_y, g_dot_x0, g_dot_step, g_dot_r;

static uint8_t* g_fb;
static uint32_t g_pitch;

static inline void put(int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)g_w && (unsigned)y < (unsigned)g_h)
        *(volatile uint32_t*)(g_fb + (uint32_t)y * g_pitch + (uint32_t)x * 4u) = c;
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int max) {
    int ra = (int)(a >> 16 & 255), ga = (int)(a >> 8 & 255), ba = (int)(a & 255);
    int rb = (int)(b >> 16 & 255), gb = (int)(b >> 8 & 255), bb = (int)(b & 255);
    return (uint32_t)((ra + (rb - ra) * t / max) << 16 | (ga + (gb - ga) * t / max) << 8 | (ba + (bb - ba) * t / max));
}

static void fill_circle(int cx, int cy, int r, uint32_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) put(cx + x, cy + y, c);
}

/* a thick line from (x0,y0) to (x1,y1), round ends */
static void thick_line(int x0, int y0, int x1, int y1, int r, uint32_t c0, uint32_t c1) {
    int64_t dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    if (!len2) len2 = 1;
    int minx = (x0 < x1 ? x0 : x1) - r, maxx = (x0 > x1 ? x0 : x1) + r;
    int miny = (y0 < y1 ? y0 : y1) - r, maxy = (y0 > y1 ? y0 : y1) + r;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int64_t px = x - x0, py = y - y0;
            int64_t t = (px * dx + py * dy) * 1024 / len2;    /* along the line, 0..1024 */
            if (t < 0) t = 0;
            if (t > 1024) t = 1024;
            int64_t qx = px - dx * t / 1024, qy = py - dy * t / 1024;
            if (qx * qx + qy * qy <= (int64_t)r * r) put(x, y, mix(c0, c1, (int)t, 1024));
        }
}

/* the banana is drawn tilted by 25 degrees, its stem end up, then mirrored left to right (the
 * stem on the left): cos and sin x 1024 */
#define TILT_C 928
#define TILT_S 433

static void draw_banana(void) {
    int s = g_w < g_h ? g_w : g_h;
    int R = s / 4;                          /* the arc's radius */
    if (R < 40) R = 40;
    int span = R * 8 / 10;                  /* from the middle to each tip */
    int half_max = R * 18 / 100;            /* half the thickness in the middle */
    int mx = g_w / 2, my = g_h * 40 / 100;  /* the banana's middle on the screen */
    int mid = R * 8 / 10;                   /* ... and in its own frame (the arc's centre at 0,0) */

    const uint32_t light = 0x00FFE866u, dark = 0x00D9A21Au, edge = 0x00805A08u, tip = 0x004A3310u;
    int box = R * 11 / 10;
    for (int y = my - box; y <= my + box; y++) {
        for (int x = mx - box; x <= mx + box; x++) {
            /* the screen point, turned back into the banana's own frame */
            int64_t vx = x - mx, vy = y - my;
            int64_t dx = (vx * TILT_C - vy * TILT_S) / 1024;
            int64_t dy = (vx * TILT_S + vy * TILT_C) / 1024 + mid;
            if (dy <= 0 || dx < -span || dx > span) continue;
            int u = (int)((dx + span) * 1024 / (2 * span));          /* 0 left tip .. 1024 right tip */
            int hh = (int)((int64_t)half_max * 4 * u * (1024 - u) / (1024 * 1024));
            hh = hh * (880 + u / 4) / 1024;                            /* a little fuller toward the stem */
            if (hh < 2) hh = 2;
            int64_t in = (int64_t)(R - hh) * (R - hh), out = (int64_t)(R + hh) * (R + hh);
            int64_t d2 = dx * dx + dy * dy;
            if (d2 < in || d2 > out) continue;
            int t = (int)((d2 - in) * 1024 / (out - in));             /* 0 top edge .. 1024 bottom edge */
            uint32_t c = mix(light, dark, t, 1024);
            if (t < 50 || t > 970) c = edge;                           /* outline */
            if (u < 30) c = tip;                                       /* the dark flower end */
            put(2 * mx - x, y, c);                                     /* mirrored: the stem on the left */
        }
    }
    /* the stem: from the right tip, onward and up (in the banana's frame, then turned) */
    int lx0 = span - R / 30, ly0 = R * 6 / 10 + R / 60 - mid;
    int lx1 = span + R / 9,  ly1 = R * 6 / 10 - R / 6 - mid;
    int sx0 = mx + (lx0 * TILT_C + ly0 * TILT_S) / 1024, sy0 = my + (-lx0 * TILT_S + ly0 * TILT_C) / 1024;
    int sx1 = mx + (lx1 * TILT_C + ly1 * TILT_S) / 1024, sy1 = my + (-lx1 * TILT_S + ly1 * TILT_C) / 1024;
    thick_line(2 * mx - sx0, sy0, 2 * mx - sx1, sy1, R / 22 > 2 ? R / 22 : 2, 0x009C8A2Cu, 0x005A4A14u);

    g_dot_r = R / 26 > 3 ? R / 26 : 3;
    g_dot_step = g_dot_r * 4;
    g_dot_x0 = mx - g_dot_step;
    g_dot_y = my + R * 75 / 100;            /* just under the banana (its lowest point is ~0.4 R below my) */
}

static void draw_dots(int phase) {
    for (int i = 0; i < 3; i++) fill_circle(g_dot_x0 + i * g_dot_step, g_dot_y, g_dot_r, i == phase ? DOT_ON : DOT_OFF);
}

static void draw_all(void) {
    const fb_info_t* fi = fb_info();
    if (!fi || fi->bpp != 32 || !fi->addr) return;
    g_busy = 1;
    g_fb = (uint8_t*)fi->addr;
    g_pitch = fi->pitch;
    g_w = (int)fi->width;
    g_h = (int)fi->height;
    for (int y = 0; y < g_h; y++) {
        volatile uint32_t* row = (volatile uint32_t*)(g_fb + (uint32_t)y * g_pitch);
        for (int x = 0; x < g_w; x++) row[x] = BG;
    }
    draw_banana();
    g_phase = (int)(timer_ms() / DOT_MS % 3);
    draw_dots(g_phase);
    g_busy = 0;
}

void splash_start(void) {
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi || fi->bpp != 32 || fi->width < 320 || fi->height < 240) return;
    g_active = 1;
    draw_all();
}

int splash_active(void) { return g_active; }

void splash_screen_changed(void) {
    if (g_active) draw_all();
}

void splash_tick(void) {
    if (!g_active || g_busy) return;
    int phase = (int)(timer_ms() / DOT_MS % 3);
    if (phase == g_phase) return;
    g_phase = phase;
    draw_dots(phase);
}

void splash_end(void) {
    if (!g_active) return;
    g_active = 0;
    g_busy = 1;                             /* (no dot drawn over the console) */
    for (int y = 0; y < g_h; y++) {
        volatile uint32_t* row = (volatile uint32_t*)(g_fb + (uint32_t)y * g_pitch);
        for (int x = 0; x < g_w; x++) row[x] = BG;
    }
    g_busy = 0;
    terminal_screen_changed();              /* the console, with everything printed meanwhile */
}
