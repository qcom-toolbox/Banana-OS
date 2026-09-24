#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

/* COM1 (0x3F8) at 115200 8N1. The console shell's output is mirrored
 * here and bytes received are fed to the shell as keystrokes, so Banana
 * OS can be driven headless (`qemu -serial stdio`) - handy for scripted
 * testing, and a lifeline when the framebuffer isn't visible. */

void serial_init(void);
int  serial_present(void);
void serial_putc(char c);
void serial_write(const char* s);
int  serial_try_getc(void);          /* -1 if nothing received */
/* pushes queued output into the UART FIFO; IRQ-safe (the timer IRQ calls it) */
void serial_kick(void);

/* printf to the serial port only (kernel debug log) */
void klog(const char* fmt, ...);

#endif
