/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audioin.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "terminal.h"
#include "serial.h"

/*
 * Recording (audioin.h). The source writes a 256 KiB ring (~1.36 s at
 * 48 kHz 16-bit stereo); the timer interrupt (every 2 ms) reads where it
 * got to and counts the bytes in a 64-bit total - g_abs, "everything ever
 * recorded". A reader's cursor is such a total too: what it has not read
 * is [cursor, g_abs), as long as that is still in the ring.
 */

#define RING     262144u
#define FRAME    4u
#define MARGIN   32768u                 /* (the part the source is about to write over) */

static const audio_in_t* g_src;         /* recording from this one */
static char              g_name[112] = "none";
static const audio_in_t* g_builtin;     /* the computer's own (while a USB mic is plugged in) */
static char              g_builtin_name[112];
static uint8_t*          g_ring;
static volatile int      g_users;       /* readers that have it open */
static volatile int      g_on;          /* the source records: the tick counts */
static uint32_t          g_last;        /* the source's position at the last tick */
static volatile uint64_t g_abs;
/* g_abs - g_off is where the source writes, as an offset into the ring: a
 * source starts writing at its own position (0 after a stream reset, a
 * new USB microphone), not where the count happens to be */
static uint64_t          g_off;
static int               g_peak, g_peak_shown;
static uint32_t          g_peak_ms;
static uint32_t          g_overruns;
static int               g_clflush = -1;

#ifndef AUDIOIN_HOST_TEST
static inline uintptr_t irq_off(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_on(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}
#else                                   /* (host tests: one thread, no interrupts) */
static inline uintptr_t irq_off(void) { return 0; }
static inline void irq_on(uintptr_t f) { (void)f; }
#endif

/* What the chip wrote by DMA is read from RAM, not from old cache lines
 * (a chip that writes without snooping: the same as audio.c's flush). */
static void invalidate(const uint8_t* p, uint32_t n) {
    if (g_clflush < 0) {
        uint32_t a = 1, b, c, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        g_clflush = (d >> 19) & 1;
    }
    if (!g_clflush || !n) return;
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n; a += 64)
        __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
}

uint8_t* audio_in_ring(void) {
    if (!g_ring) {
        /* 4 KiB-aligned (the HDA / AC'97 buffer lists point into it) */
        uint8_t* raw = (uint8_t*)kmalloc(RING + 4096);
        if (raw) {
            g_ring = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
            memset(g_ring, 0, RING);
        }
    }
    return g_ring;
}

uint32_t audio_in_ring_bytes(void) { return RING; }

/* the loudest sample of n new bytes at ring offset at */
static void measure(uint32_t at, uint32_t n) {
    int peak = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        uint32_t k = (at + i) % RING;
        int s = (int16_t)(g_ring[k] | (g_ring[(k + 1) % RING] << 8));
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    if (peak > 32767) peak = 32767;
    if (peak > g_peak) g_peak = peak;
}

/* how far the source got (interrupts off) */
static void update(void) {
    if (!g_on || !g_src) return;
    if (g_src->tick) g_src->tick();
    uint32_t pos = g_src->position() % RING;
    uint32_t moved = (pos - g_last + RING) % RING;
    if (!moved) return;
    uint32_t at = g_last;
    if (at + moved <= RING) invalidate(g_ring + at, moved);
    else { invalidate(g_ring + at, RING - at); invalidate(g_ring, moved - (RING - at)); }
    measure(at, moved);
    g_last = pos;
    g_abs += moved;
    uint32_t now = timer_ms();
    if (now - g_peak_ms >= 100) { g_peak_shown = g_peak; g_peak = 0; g_peak_ms = now; }
}

void audio_in_tick(void) {
    static uint32_t n;
    if (!g_on || ++n % 2) return;
    update();
}

static int start_source(void) {
    if (!g_src || !audio_in_ring()) return -1;
    memset(g_ring, 0, RING);
    invalidate(g_ring, RING);
    if (g_src->start() != 0) return -1;
    uintptr_t f = irq_off();
    g_last = g_src->position() % RING;
    g_off = g_abs - g_last;                     /* (a power-of-two ring: wrapping arithmetic is fine) */
    g_peak = g_peak_shown = 0;
    g_peak_ms = timer_ms();
    g_on = 1;
    irq_on(f);
    return 0;
}

static void stop_source(void) {
    uintptr_t f = irq_off();
    g_on = 0;
    irq_on(f);
    if (g_src && g_src->stop) g_src->stop();
}

static void switch_source(const audio_in_t* s, const char* name) {
    if (g_on) stop_source();
    g_src = s;
    kstrlcpy(g_name, s ? name : "none", sizeof(g_name));
    if (s && g_users > 0 && start_source() != 0) klog("audioin: %s does not start\n", g_name);
    klog("audioin: now %s\n", g_name);
}

void audio_in_register(const audio_in_t* src, const char* name) {
    if (!src) return;
    if (g_src && g_src != src) {                 /* a USB mic already plugged in: it stays */
        g_builtin = src;
        kstrlcpy(g_builtin_name, name, sizeof(g_builtin_name));
        return;
    }
    g_src = src;
    kstrlcpy(g_name, name, sizeof(g_name));
}

void audio_in_plug(const audio_in_t* src, const char* name) {
    if (!src) return;
    if (!g_builtin && g_src && g_src != src) {
        g_builtin = g_src;
        kstrlcpy(g_builtin_name, g_name, sizeof(g_builtin_name));
    }
    switch_source(src, name);
}

void audio_in_unplug(const audio_in_t* src) {
    if (g_src != src) return;
    const audio_in_t* back = g_builtin;
    g_builtin = NULL;
    switch_source(back, back ? g_builtin_name : "none");
}

int audio_in_available(void) { return g_src != NULL; }
const char* audio_in_name(void) { return g_name; }

int audio_in_open(uint64_t* cursor) {
    if (!g_src) return -1;
    if (g_users++ == 0 && start_source() != 0) { g_users--; return -1; }
    uintptr_t f = irq_off();
    update();
    if (cursor) *cursor = g_abs;
    irq_on(f);
    return 0;
}

void audio_in_close(void) {
    if (g_users <= 0) return;
    if (--g_users == 0) stop_source();
}

int audio_in_read(uint64_t* cursor, int16_t* out, uint32_t frames, int channels) {
    if (!g_src || !g_on || !cursor || (channels != 1 && channels != 2)) return -1;
    uintptr_t f = irq_off();
    update();
    uint64_t end = g_abs;
    irq_on(f);
    uint64_t c = *cursor;
    if (c > end) c = end;
    if (end - c > RING - MARGIN) {                /* fell behind: the oldest still there */
        c = end - (RING - MARGIN);
        c &= ~(uint64_t)(FRAME - 1);
        g_overruns++;
    }
    uint32_t avail = (uint32_t)((end - c) / FRAME);
    if (avail > frames) avail = frames;
    for (uint32_t i = 0; i < avail; i++) {
        uint32_t k = (uint32_t)((c - g_off + (uint64_t)i * FRAME) % RING);
        int16_t l = (int16_t)(g_ring[k] | (g_ring[k + 1] << 8));
        int16_t r = (int16_t)(g_ring[k + 2] | (g_ring[k + 3] << 8));
        if (channels == 2) { out[i * 2] = l; out[i * 2 + 1] = r; }
        else out[i] = (int16_t)(((int)l + (int)r) / 2);
    }
    *cursor = c + (uint64_t)avail * FRAME;
    return (int)avail;
}

int audio_in_level(void) {
    if (!g_on) return 0;
    return g_peak > g_peak_shown ? g_peak : g_peak_shown;
}

uint64_t audio_in_recorded(void) { return g_abs; }

void audio_in_list(void) {
    char line[160];
    ksnprintf(line, sizeof(line), "microphone: %s", g_name);
    terminal_writeln(line);
    if (!g_src) return;
    ksnprintf(line, sizeof(line), "recording:  48000 Hz, 16-bit stereo, %s, %d reader%s, %llu KiB recorded, %u overrun%s",
              g_on ? "on" : "off", g_users, g_users == 1 ? "" : "s", (unsigned long long)(g_abs >> 10),
              g_overruns, g_overruns == 1 ? "" : "s");
    terminal_writeln(line);
}
