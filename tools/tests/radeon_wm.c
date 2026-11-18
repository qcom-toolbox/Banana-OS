/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Evergreen display watermarks (third_party/radeon/evergreen_wm.c, Linux's
 * arithmetic) for modes an HD 7450 drives: they must give the display enough
 * of a head start - at least the memory's 2 us latency, a priority mark that
 * is latency x pixel clock / 16 (not wrapped, as Linux's fixed point does
 * past ~7 us) and fits the hardware's field - or the picture breaks up under
 * 3D load. */
#include <stdio.h>
#include "../../third_party/radeon/evergreen_wm.h"
static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)
int main(void) {
    /* Caicos, DDR3: 800 MHz memory, 625 MHz engine, a 64-bit bus (2 channels), the whole line buffer */
    struct { const char* name; unsigned khz, ht, hd; } m[] = {
        { "1920x1080@60", 148500, 2200, 1920 }, { "1280x1024@60", 108000, 1688, 1280 },
        { "1024x768@60", 65000, 1344, 1024 }, { "2560x1440@60", 241500, 2720, 2560 } };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        struct evergreen_wm_in in = { m[i].khz, m[i].ht, m[i].hd, 800000, 625000, 2, 16384, 1 };
        struct evergreen_wm_out w;
        evergreen_wm_compute(&in, &w);
        unsigned line = (unsigned)((unsigned long long)m[i].ht * 1000000 / m[i].khz);
        CHECK(w.line_time == line && w.latency > 2000 && w.latency < 4 * w.line_time && w.priority_mark > 0 && w.priority_mark <= 0x7FFF,
              "%s: line %u ns, latency %u ns, priority mark %u%s", m[i].name, w.line_time, w.latency, w.priority_mark,
              w.linux_forces_high ? " (Linux: priority high)" : "");
        /* the mark: latency x pixel clock / 16, as Linux */
        unsigned want = (unsigned)((unsigned long long)w.latency * m[i].khz / 1000 / 1000 / 16);
        CHECK(w.priority_mark == want, "  mark %u = latency x MHz / 16 (%u)", w.priority_mark, want);
    }
    /* a slow memory clock (power saving): more latency */
    struct evergreen_wm_in fast = { 148500, 2200, 1920, 800000, 625000, 2, 16384, 1 }, slow = fast;
    slow.mclk_khz = 150000; slow.sclk_khz = 157000;
    struct evergreen_wm_out a, b;
    evergreen_wm_compute(&fast, &a);
    evergreen_wm_compute(&slow, &b);
    CHECK(b.latency > a.latency, "lower clocks, a longer latency (%u > %u ns)", b.latency, a.latency);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
