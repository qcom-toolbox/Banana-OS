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
} mouse_state_t;

void         mouse_init(void);
mouse_state_t mouse_read(void);   /* non-blocking, returns last known state */
/* timer interrupt only (kernel/gui.c): the motion of the waiting PS/2
 * packets that change no button; 1 if there was any */
int          mouse_irq_motion(int* dx, int* dy);
/* pointer speed, 1 (slowest) .. 10 (fastest); 5 = the mouse's own speed */
void         mouse_set_speed(int speed);
int          mouse_get_speed(void);
/* USB mice report here (dy positive = up, buttons bit0 left/1 right/2 middle) */
void         mouse_inject(int dx, int dy, int buttons);
void         mouse_inject_wheel(int dz);      /* + = scroll down */
/* " | Touchpad: Synaptics ClickPad (firmware 8.1)" and the like, for usb_status() */
void         touchpad_describe(char* out, int cap);
int          mouse_is_touchpad(void); /* 1 if a Synaptics PS/2 touchpad was detected */

/* For other drivers that consume AUX bytes (e.g. keyboard polling) */
void mouse_on_aux_byte(uint8_t b);

#endif
