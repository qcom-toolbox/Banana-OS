/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WEB_GRADIENT_H
#define WEB_GRADIENT_H
/* CSS linear gradients: any angle, up to WEB_GRAD_STOPS colour stops (with
 * transparency), repeating-linear-gradient, background-size / -position
 * tiles. Integer only: this code also runs in the kernel's legacy renderer,
 * which never touches the FPU. layout.c parses, render.c paints. */
#include "types.h"

#define WEB_GRAD_STOPS 12

typedef struct web_grad {
    int      n;
    uint32_t col[WEB_GRAD_STOPS];   /* css_color_blend: 255 - alpha in the top byte */
    int      pos[WEB_GRAD_STOPS];   /* along the gradient line: 0..65536 = 0%..100% */
    int      deg;                   /* CSS angle: 0 up, 90 right, 180 down */
    uint8_t  repeating;
    uint8_t  tile;                  /* bit 0: repeats across, bit 1: down */
    int      ox, oy, gw, gh;        /* the gradient image: from the item's corner, its size */
} web_grad_t;

/* sin(deg) * 1024 */
static inline int web_sin1024(int deg) {
    static const int16_t q[91] = {
        0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195, 213, 230, 248, 265, 282, 299, 316, 333, 350,
        367, 384, 400, 416, 433, 449, 465, 481, 496, 512, 527, 543, 558, 573, 587, 602, 616, 630, 644, 658, 672,
        685, 698, 711, 724, 737, 749, 761, 773, 784, 796, 807, 818, 828, 839, 849, 859, 868, 878, 887, 896, 904,
        912, 920, 928, 935, 943, 949, 956, 962, 968, 974, 979, 984, 989, 994, 998, 1002, 1005, 1008, 1011, 1014, 1016,
        1018, 1020, 1022, 1023, 1023, 1024, 1024 };
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return q[deg];
    if (deg <= 180) return q[180 - deg];
    if (deg <= 270) return -q[deg - 180];
    return -q[360 - deg];
}
static inline int web_cos1024(int deg) { return web_sin1024(deg + 90); }

/* the colour at t (0..65536 along the line), alpha kept in the top byte */
static inline uint32_t web_grad_at(const web_grad_t* g, int t) {
    if (g->n <= 0) return 0;
    if (g->repeating && g->n > 1) {
        int a = g->pos[0], b = g->pos[g->n - 1], period = b - a;
        if (period > 0) { t = (t - a) % period; if (t < 0) t += period; t += a; }
    }
    if (t <= g->pos[0]) return g->col[0];
    for (int i = 1; i < g->n; i++) {
        if (t > g->pos[i]) continue;
        int span = g->pos[i] - g->pos[i - 1];
        uint32_t c0 = g->col[i - 1], c1 = g->col[i];
        if (span <= 0) return c1;
        int f = (int)(((int64_t)(t - g->pos[i - 1]) << 8) / span);   /* 0..256 */
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            int a = (int)((c0 >> sh) & 255), b = (int)((c1 >> sh) & 255);
            out |= (uint32_t)(a + ((b - a) * f >> 8)) << sh;
        }
        return out;
    }
    return g->col[g->n - 1];
}

/* the gradient line's length for a gw x gh image at this angle, px */
static inline int web_grad_len(int deg, int gw, int gh) {
    int s = web_sin1024(deg), c = web_cos1024(deg);
    if (s < 0) s = -s;
    if (c < 0) c = -c;
    int len = (gw * s + gh * c) >> 10;
    return len > 0 ? len : 1;
}
#endif
