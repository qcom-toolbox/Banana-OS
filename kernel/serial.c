#include "serial.h"
#include "kstring.h"
#include "io.h"

#define COM1 0x3F8

static int g_present = 0;

/* Transmit ring buffer. The UART sends ~11.5 KB/s at 115200 baud (QEMU
 * paces its emulated 16550 at the programmed rate), and waiting for it
 * per character made every bit of console output crawl - `cat` of an
 * 8000-line file took ~25 s just in serial waits. Output is queued here
 * instead and fed to the UART's 16-byte FIFO from the timer interrupt
 * (and opportunistically from serial_putc). If a huge burst overflows
 * the buffer, the oldest bytes are dropped, so the newest output (the
 * shell prompt) always makes it out. */
#define TX_SIZE 65536u
static char              g_tx[TX_SIZE];
static volatile uint32_t g_tx_head;   /* next write */
static volatile uint32_t g_tx_tail;   /* next byte to send */

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* no interrupts: polled */
    outb(COM1 + 3, 0x80);    /* DLAB on */
    outb(COM1 + 0, 0x01);    /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);    /* FIFO on, cleared, 14-byte threshold */
    outb(COM1 + 4, 0x13);    /* loopback test mode (DTR|RTS|LOOP) */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { g_present = 0; return; }
    outb(COM1 + 4, 0x03);    /* normal operation, DTR|RTS */
    g_present = 1;
}

int serial_present(void) { return g_present; }

void serial_kick(void) {
    if (!g_present) return;
    uint32_t f = irq_save();
    /* LSR bit 5: transmit FIFO empty - room for 16 bytes */
    while (g_tx_tail != g_tx_head && (inb(COM1 + 5) & 0x20)) {
        for (int n = 0; n < 16 && g_tx_tail != g_tx_head; n++) {
            outb(COM1, (uint8_t)g_tx[g_tx_tail]);
            g_tx_tail = (g_tx_tail + 1) % TX_SIZE;
        }
    }
    irq_restore(f);
}

static void enqueue(char c) {
    uint32_t f = irq_save();
    uint32_t next = (g_tx_head + 1) % TX_SIZE;
    if (next == g_tx_tail) g_tx_tail = (g_tx_tail + 1) % TX_SIZE;   /* full: drop oldest */
    g_tx[g_tx_head] = c;
    g_tx_head = next;
    irq_restore(f);
}

void serial_putc(char c) {
    if (!g_present) return;
    if (c == '\n') enqueue('\r');
    enqueue(c);
    serial_kick();
}

void serial_write(const char* s) {
    while (*s) serial_putc(*s++);
}

int serial_try_getc(void) {
    if (!g_present) return -1;
    if (!(inb(COM1 + 5) & 0x01)) return -1;
    return inb(COM1);
}

void klog(const char* fmt, ...) {
    if (!g_present) return;
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    serial_write(buf);
}
