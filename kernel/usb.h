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
} mouse_state_t;

void         mouse_init(void);
mouse_state_t mouse_read(void);   /* non-blocking, returns last known state */
int          mouse_is_touchpad(void); /* 1 if a Synaptics PS/2 touchpad was detected */

/* For other drivers that consume AUX bytes (e.g. keyboard polling) */
void mouse_on_aux_byte(uint8_t b);

#endif
