/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef USB_H
#define USB_H

#include "types.h"

/*
 * USB keyboard/mouse support via UHCI/OHCI/EHCI legacy emulation.
 *
 * Modern BIOSes (and VirtualBox/QEMU) implement "USB Legacy Support":
 * they emulate USB HID devices as PS/2 through the 8042 controller,
 * so our PS/2 driver already handles USB keyboards transparently.
 *
 * For xHCI (USB 3.x) hosts that don't emulate PS/2, we need to
 * hand off to xHCI legacy support via the xHCI Extended Capabilities.
 * We do that here by scanning PCI for xHCI and writing the OS Owned
 * semaphore so the BIOS releases the controller to us — after which
 * the BIOS's SMI handler continues to feed scancodes into port 0x60.
 *
 * Mouse: In text mode there is no graphical cursor, so we read mouse
 * packets and expose delta/button state for future use, but we don't
 * render anything (no pixel framebuffer in text mode).
 */

void usb_init(void);        /* claim xHCI from BIOS, enable legacy KB/mouse */
const char* usb_status(void);

/* Mouse state (updated by PS/2 aux port interrupt / polling) */
typedef struct {
    int dx, dy;             /* last delta */
    int btn_left;
    int btn_right;
    int btn_middle;
    int dz;                 /* wheel notches since the last read: + = toward the user (scroll down) */
    int abs;                /* 1: ax, ay are where the pointer is (a tablet: VirtualBox / QEMU USB tablet, touchscreens) */
    int ax, ay;             /* 0..65535 across / down the screen */
} mouse_state_t;

void         mouse_init(void);
/* a PS/2 (8042) controller is there - Macs and some new PCs have none: its
 * ports then read 0xFF, which looks like a byte waiting forever */
int          ps2_controller_present(void);
/* the 8042's status, 0 once it has gone away (three 0xFF reads: the
 * firmware's emulation ended) - every poll reads it through this */
uint8_t      ps2_status(void);
/* lsinput: every way into the pointer and what it has sent */
void         input_list(void);
mouse_state_t mouse_read(void);   /* non-blocking, returns last known state */
mouse_state_t mouse_read_raw(void); /* games: same devices, no desktop speed scaling */
/* timer interrupt only (kernel/gui.c): the motion of the waiting PS/2
 * packets that change no button; 1 if there was any */
int          mouse_irq_motion(int* dx, int* dy);
/* pointer speed, 1 (slowest) .. 10 (fastest); 5 = the mouse's own speed */
void         mouse_set_speed(int speed);
int          mouse_get_speed(void);
/* USB mice report here (dy positive = up, buttons bit0 left/1 right/2 middle) */
void         mouse_inject(int dx, int dy, int buttons);
/* Each pointing device keeps its own buttons, and the desktop sees all of
 * them together (ORed): a touchpad saying "no button" no longer lets go of
 * the button held on a USB mouse. A device asks for its source once
 * (0 when all are taken: the shared one), reports through it, and gives it
 * back when it goes away (its buttons are let go then). */
int          mouse_source(void);
void         mouse_source_free(int src);
void         mouse_inject_src(int src, int dx, int dy, int buttons);
void         mouse_inject_abs_src(int src, int x, int y, int buttons);
/* the source in *slot, asked for on first use */
static inline int mouse_src(int* slot) { if (!*slot) *slot = mouse_source(); return *slot; }
/* selftest mouse: the sources checked */
void         mouse_source_tests(void (*report)(const char*, int, const char*));
void         mouse_inject_wheel(int dz);      /* + = scroll down */
/* absolute pointers (tablets): x, y 0..65535 across the screen, top-left 0 */
void         mouse_inject_abs(int x, int y, int buttons);
/* the position alone, 0..65535 (VirtualBox: its buttons come through PS/2) */
void         mouse_set_abs(int x, int y);
/* 1: PS/2 mouse packets are dropped (VMMouse reports the pointer instead) */
void         mouse_ps2_mute(int on);
/* " | Touchpad: Synaptics ClickPad (firmware 8.1)" and the like, for usb_status() */
void         touchpad_describe(char* out, int cap);
int          mouse_is_touchpad(void); /* 1 if a Synaptics PS/2 touchpad was detected */

/* For other drivers that consume AUX bytes (e.g. keyboard polling) */
void mouse_on_aux_byte(uint8_t b);

#endif
