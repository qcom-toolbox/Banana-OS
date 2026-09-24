#include "timer.h"
#include "types.h"
#include "idt.h"
#include "io.h"
#include "terminal.h"
#include "serial.h"

/*
 * PIT channel 0, mode 2 (rate generator) at 1 kHz, counted by IRQ0.
 *
 * This used to poll the PIT counter for wraps, which only advanced time
 * while something was actively spinning on timer_poll() - so every wait
 * in the kernel had to be a busy loop. With the interrupt counting
 * milliseconds for us, waits can `hlt` and the CPU actually idles.
 * timer_ticks() keeps its old 100 Hz meaning, since the rest of the
 * kernel (uptime, clock, top's CPU%) is written against 10 ms ticks.
 */

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_HZ    1000
#define PIT_BASE  1193182u

static volatile uint32_t g_ms = 0;

static void timer_irq(void) {
    g_ms++;
    serial_kick();      /* drain the serial console's output queue */
}

void timer_init(void) {
    uint32_t divisor = PIT_BASE / PIT_HZ;
    outb(PIT_CMD, 0x34);                 /* channel 0, lo/hi byte, mode 2 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
    g_ms = 0;
    irq_install(0, timer_irq);
}

uint32_t timer_ms(void) {
    return g_ms;
}

uint32_t timer_ticks(void) {
    return g_ms / 10u;
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
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms) timer_idle();
}
