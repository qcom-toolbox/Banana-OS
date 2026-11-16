/* SPDX-License-Identifier: GPL-3.0-or-later */
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
/* everything queued, sent now, waiting for the UART (a panic: no timer to do it) */
void serial_drain(void);
/* Bounded, polled emergency output; bypasses the ring buffer and terminal. */
void serial_panic_write(const char* text);

/* printf to the serial port only (kernel debug log) */
void klog(const char* fmt, ...);
/* "verbose" boot: the log is also written on the screen (kernel/kernel.c) */
void klog_to_screen(int on);
/* the last 64 KiB of the log (dmesg): its bytes copied to out, how many */
uint32_t klog_read(char* out, uint32_t cap);

#endif
