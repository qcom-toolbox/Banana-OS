#include "timer.h"
#include "types.h"
#include "idt.h"
#include "io.h"
#include "terminal.h"
#include "serial.h"
#include "task.h"
#include "gui.h"
#include "audio.h"
#include "splash.h"

/*
 * PIT channel 0, mode 2 (rate generator) at 1 kHz, counted by IRQ0.
 *
 * This used to poll the PIT counter for wraps, which only advanced time
 * while something was actively spinning on timer_poll() - so every wait
 * in the kernel had to be a busy loop. With the interrupt counting
 * milliseconds for us, waits can `hlt` and the CPU actually idles.
 * timer_ticks() keeps its old 100 Hz meaning, since the rest of the
 * kernel (uptime, clock, top's CPU%) is written against 10 ms ticks.
 *
 * The time itself (timer_ms) comes from the CPU's time stamp counter, not
 * from counting the interrupts: virtual machines deliver timer interrupts
 * late while the guest is idle and then several at once to catch up (on
 * VirtualBox, when the mouse moves), so a clock counting them ran slow,
 * then fast - and an app timing its music with ticks_ms() slowed down and
 * skipped. The TSC's rate is measured once at boot against PIT channel 2,
 * polled (no interrupts involved). The interrupt count is the fallback.
 */

#define PIT_CH0   0x40
#define PIT_CH2   0x42
#define PIT_CMD   0x43
#define PIT_HZ    1000
#define PIT_BASE  1193182u

static volatile uint32_t g_ms = 0;          /* timer interrupts so far */
static uint64_t g_tsc0, g_tsc_per_ms;       /* 0: no usable TSC, g_ms is the clock */

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* TSC cycles in 50 ms of PIT channel 2 (one-shot, its output polled), 0 if it never ends */
static uint64_t tsc_over_50ms(void) {
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));   /* gate on, speaker off */
    outb(PIT_CMD, 0xB0);                            /* channel 2, lo/hi, mode 0 */
    uint16_t count = (uint16_t)(PIT_BASE / 20u);    /* 50 ms */
    outb(PIT_CH2, (uint8_t)count);
    outb(PIT_CH2, (uint8_t)(count >> 8));           /* counting starts */
    uint64_t t0 = tsc_now();
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20))                     /* OUT2 goes high at 0 */
        if (++guard > 100000000u) { outb(0x61, p61); return 0; }
    uint64_t t1 = tsc_now();
    outb(0x61, p61);
    return t1 - t0;
}

static void calibrate_tsc(void) {
    uint64_t a = tsc_over_50ms(), b = tsc_over_50ms();
    uint64_t c = a < b ? a : b;                     /* (the less disturbed one) */
    uint64_t per = c / 50u;
    if (!a || !b || per < 10000u || per > 20000000u) return;    /* 10 MHz .. 20 GHz, else unusable */
    if ((a > b ? a - b : b - a) > c / 20u) return;              /* two measures 5 % apart: unusable */
    g_tsc_per_ms = per;
    g_tsc0 = tsc_now();
}

uint64_t timer_tsc_per_ms(void) { return g_tsc_per_ms; }
uint32_t timer_irq_count(void) { return g_ms; }

static void timer_irq(void) {
    g_ms++;
    serial_kick();      /* drain the serial console's output queue */
    task_smp_tick();    /* app code on the other cores takes turns (kernel/task.c) */
    gui_cursor_tick();  /* the mouse pointer moves even while the desktop's task is busy */
    audio_tick();       /* the sound card's ring stays fed, whatever the tasks do */
    splash_tick();      /* the boot screen's dots */
}

void timer_init(void) {
    calibrate_tsc();
    uint32_t divisor = PIT_BASE / PIT_HZ;
    outb(PIT_CMD, 0x34);                 /* channel 0, lo/hi byte, mode 2 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
    g_ms = 0;
    irq_install(0, timer_irq);
}

uint32_t timer_ms(void) {
    if (g_tsc_per_ms) return (uint32_t)((tsc_now() - g_tsc0) / g_tsc_per_ms);
    return g_ms;
}

uint32_t timer_ticks(void) {
    return timer_ms() / 10u;
}

void timer_poll(void) {
}

void timer_idle(void) {
    /* sti's one-instruction shadow makes "sti; hlt" atomic: an interrupt
     * can't sneak in between and leave us halted with nothing to wake us */
    __asm__ volatile("sti; hlt" ::: "memory");
}

void timer_sleep_ms(uint32_t ms) {
    terminal_flush();   /* whatever was printed before a blocking wait shows up */
    if (ms == 0) return;
    uint32_t start = timer_ms();
    while ((uint32_t)(timer_ms() - start) < ms) timer_idle();
}
