#include "usb.h"
#include "terminal.h"
#include "types.h"
#include "timer.h"

/* ── I/O helpers ─────────────────────────────────────────────────── */
static inline uint8_t  inb (uint16_t p){ uint8_t  v; __asm__ volatile("inb  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw (uint16_t p){ uint16_t v; __asm__ volatile("inw  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl (uint16_t p){ uint32_t v; __asm__ volatile("inl  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outb(uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }

static char usb_status_buf[160];
static int usb_xhci_found = 0, usb_xhci_handoff_ok = 0;
static int usb_ehci_found = 0, usb_ehci_handoff_ok = 0;
static int usb_uhci_found = 0, usb_ohci_found = 0;
static int syn_detected = 0; /* Synaptics PS/2 touchpad detected? (see usb_status() and mouse_init() below) */

/* ── PCI helpers ─────────────────────────────────────────────────── */
#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, val);
}

static void pci_enable_usb_decode(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cmd = pci_read(bus, dev, fn, 0x04);
    cmd |= 0x00000007u; /* I/O space + memory space + bus master */
    pci_write(bus, dev, fn, 0x04, cmd);
}

/* ── xHCI legacy handoff ─────────────────────────────────────────── */
/*
 * Scan all PCI buses for an xHCI controller (class 0x0C, sub 0x03, prog 0x30).
 * If found, read the xHCI Extended Capabilities pointer, find the
 * USB Legacy Support Capability (cap ID 1), set the OS Owned bit,
 * and wait for BIOS to release it.  After that the BIOS SMI handler
 * keeps routing USB HID → PS/2 port 0x60 for us.
 */
static void __attribute__((unused)) xhci_handoff(uint8_t bus, uint8_t dev, uint8_t fn) {
    /*
     * BAR0 holds xHCI MMIO base and may be 32-bit or 64-bit memory BAR.
     * In this kernel we only use 32-bit identity-mapped addresses.
     */
    uint32_t bar0_lo = pci_read(bus, dev, fn, 0x10);
    if (!(bar0_lo & 0x1) && ((bar0_lo & 0x6) == 0x4)) {
        /* 64-bit BAR: include upper dword if present */
        uint32_t bar0_hi = pci_read(bus, dev, fn, 0x14);
        if (bar0_hi != 0) return; /* MMIO above 4 GiB is not reachable here */
    }
    uint32_t bar0 = bar0_lo & ~0xFu;
    if (!bar0) return;

    /* HCCPARAMS1 is at offset 0x10 in the capability registers */
    volatile uint32_t* base = (volatile uint32_t*)(uintptr_t)bar0;
    uint32_t hccparams1 = base[4];  /* offset 0x10 / 4 */
    uint32_t xecp_off   = (hccparams1 >> 16) & 0xFFFF;
    if (!xecp_off) return;

    volatile uint32_t* xecp = base + xecp_off;
    /* Walk extended capability list */
    for (int iter = 0; iter < 32; iter++) {
        uint32_t cap = *xecp;
        uint8_t  id  = cap & 0xFF;
        if (id == 1) {
            /* USB Legacy Support cap found */
            /* Set OS Owned Semaphore (bit 24) */
            *xecp = cap | (1u << 24);
            /* Wait for BIOS Owned (bit 16) to clear */
            for (int t = 0; t < 100000; t++) {
                if (!(*xecp & (1u << 16))) break;
                /* small spin delay */
                for (int d = 0; d < 100; d++)
                    __asm__ volatile("pause");
            }
            usb_xhci_handoff_ok++;
            return;
        }
        uint8_t next = (cap >> 8) & 0xFF;
        if (!next) break;
        xecp += next;
    }
}

/* ── EHCI legacy handoff ─────────────────────────────────────────── */
/*
 * EHCI exposes an "Extended Capabilities Pointer" (EECP) in HCCPARAMS.
 * If the USB Legacy Support capability is present, set OS Owned semaphore
 * and wait for BIOS Owned to clear so firmware SMI can hand over cleanly.
 */
static void __attribute__((unused)) ehci_handoff(uint8_t bus, uint8_t dev, uint8_t fn) {
    /* BAR0 can be 32-bit memory BAR for EHCI operational registers. */
    uint32_t bar0 = pci_read(bus, dev, fn, 0x10) & ~0xFu;
    if (!bar0) return;

    volatile uint32_t* base = (volatile uint32_t*)(uintptr_t)bar0;
    /* EHCI HCCPARAMS at offset 0x08 */
    uint32_t hccparams = base[2];
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFF);
    if (!eecp) return;

    /*
     * USBLEGSUP is a PCI config dword at EECP.
     * bit16 = BIOS Owned, bit24 = OS Owned
     */
    uint32_t legsup = pci_read(bus, dev, fn, eecp);
    if (!(legsup & (1u << 24))) {
        legsup |= (1u << 24);
        pci_write(bus, dev, fn, eecp, legsup);
    }

    for (int t = 0; t < 100000; t++) {
        uint32_t now = pci_read(bus, dev, fn, eecp);
        if (!(now & (1u << 16))) break;
        for (int d = 0; d < 100; d++) __asm__ volatile("pause");
    }

    /*
     * Disable legacy USB SMI sources that can steal interrupts/events.
     * USBLEGCTLSTS is at EECP + 4 for EHCI.
     */
    pci_write(bus, dev, fn, (uint8_t)(eecp + 4), 0);
    usb_ehci_handoff_ok++;
}

void usb_init(void) {
    usb_xhci_found = usb_xhci_handoff_ok = 0;
    usb_ehci_found = usb_ehci_handoff_ok = 0;
    usb_uhci_found = usb_ohci_found = 0;

    /*
     * Scan PCI for USB controllers.
     *
     * IMPORTANT:
     * This kernel currently uses PS/2 scancodes only (port 0x60) and does not
     * implement native EHCI/xHCI transfers. Therefore we keep BIOS/firmware
     * legacy emulation in control instead of claiming OS ownership.
     *
     * On QEMU with `-device usb-kbd`, this keeps keyboard events translated
     * to PS/2 so the existing keyboard driver continues to work.
     */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn==0) break; continue; }
                uint32_t cc = pci_read(bus, dev, fn, 8) >> 8;
                if ((cc & 0xFFFF00) == 0x0C0300) {
                    pci_enable_usb_decode((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
                }
                /* class=0x0C serial bus, sub=0x03 USB, prog=0x30 xHCI */
                if (cc == 0x0C0330) {
                    usb_xhci_found++;
                    /* preserve BIOS-owned legacy emulation for now */
                }
                /* class=0x0C serial bus, sub=0x03 USB, prog=0x20 EHCI */
                if (cc == 0x0C0320) {
                    usb_ehci_found++;
                    /* preserve BIOS-owned legacy emulation for now */
                }
                if (cc == 0x0C0300) usb_uhci_found++;
                if (cc == 0x0C0310) usb_ohci_found++;
                if (fn == 0) {
                    uint32_t hdr = pci_read(bus, dev, fn, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break; /* not multi-function */
                }
            }
        }
    }
    /*
     * After xHCI handoff, the BIOS continues to translate USB HID
     * reports to PS/2 scancodes through port 0x60.  Our existing
     * PS/2 keyboard driver therefore works for USB keyboards with
     * zero extra code.
     */
}

const char* usb_status(void) {
    int p = 0;
    char nbuf[16];
    const char* s;
    const char* a = "USB: xHCI ";
    const char* b = " EHCI ";
    const char* c = " UHCI ";
    const char* d = " OHCI ";
    const char* e = " (BIOS legacy preserved)";

    for (int i = 0; a[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = a[i];
    s = u32_to_str((uint32_t)usb_xhci_handoff_ok, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];
    if (p < (int)sizeof(usb_status_buf) - 1) usb_status_buf[p++] = '/';
    s = u32_to_str((uint32_t)usb_xhci_found, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];

    for (int i = 0; b[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = b[i];
    s = u32_to_str((uint32_t)usb_ehci_handoff_ok, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];
    if (p < (int)sizeof(usb_status_buf) - 1) usb_status_buf[p++] = '/';
    s = u32_to_str((uint32_t)usb_ehci_found, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];

    for (int i = 0; c[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = c[i];
    s = u32_to_str((uint32_t)usb_uhci_found, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];

    for (int i = 0; d[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = d[i];
    s = u32_to_str((uint32_t)usb_ohci_found, nbuf, sizeof(nbuf));
    for (int i = 0; s[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = s[i];

    for (int i = 0; e[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = e[i];

    const char* t = syn_detected ? " | Touchpad: Synaptics (absolute+tap)"
                                  : " | Touchpad: PS/2 (relative)";
    for (int i = 0; t[i] && p < (int)sizeof(usb_status_buf) - 1; i++) usb_status_buf[p++] = t[i];

    usb_status_buf[p < (int)sizeof(usb_status_buf) ? p : (int)sizeof(usb_status_buf) - 1] = '\0';
    return usb_status_buf;
}

/* ─────────────────────────────────────────────────────────────────
 * PS/2 Mouse (AUX port)
 * The mouse is connected to the 8042 AUX channel.
 * We send the "enable" command and then poll.
 *
 * Also detects and drives a Synaptics PS/2 touchpad (the near-universal
 * pointing device in ~2000s laptops, e.g. Panasonic Toughbook CF-18) in
 * its native absolute-position protocol, instead of just treating it as
 * a plain 3-byte relative mouse. This unlocks real tap-to-click and
 * finer, native-resolution tracking instead of whatever coarse relative
 * packets the touchpad's PS/2-compatibility fallback would produce.
 *
 * Protocol reference: Linux kernel drivers/input/mouse/synaptics.c -
 * the identify sequence, the "sliced command" mode-byte encoding, and
 * the absolute packet bit layout below are all taken directly from
 * that (GPL, but only the wire-protocol facts are used here - this is
 * an independent implementation, not copied code).
 * ───────────────────────────────────────────────────────────────── */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

static int mouse_enabled = 0;
static uint8_t mouse_pkt[6];
static int mouse_pkt_i = 0;
static int mouse_pkt_size = 3;

static void ps2_wait_write(void) {
    int t = 100000;
    while ((inb(PS2_STATUS) & 0x02) && t--);
}
static void ps2_wait_read(void) {
    int t = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && t--);
}
static uint8_t ps2_mouse_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}
static void ps2_mouse_write(uint8_t val) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);   /* next byte goes to AUX port */
    ps2_wait_write();
    outb(PS2_DATA, val);
}
/* Send a command byte and its ack, or a command+parameter pair each
 * with their own ack - every standard PS/2 mouse command byte is
 * acked individually. */
static void ps2_mouse_cmd0(uint8_t cmd) {
    ps2_mouse_write(cmd);
    ps2_mouse_read(); /* ACK */
}
static void ps2_mouse_cmd1(uint8_t cmd, uint8_t param) {
    ps2_mouse_write(cmd);
    ps2_mouse_read(); /* ACK */
    ps2_mouse_write(param);
    ps2_mouse_read(); /* ACK */
}

/* ── Synaptics identify + mode set ──────────────────────────────────
 * "Magic knock" identify: send Set Resolution (0xE8) with argument 0,
 * four times, then a Status Request (0xE9, 3 response bytes). A plain
 * PS/2 mouse just reports normal status; a Synaptics touchpad
 * recognizes this exact pattern and echoes 0x47 in the middle response
 * byte instead. */
static int synaptics_detect(void) {
    ps2_mouse_cmd1(0xE8, 0x00);
    ps2_mouse_cmd1(0xE8, 0x00);
    ps2_mouse_cmd1(0xE8, 0x00);
    ps2_mouse_cmd1(0xE8, 0x00);

    ps2_mouse_write(0xE9);
    ps2_mouse_read();          /* ACK */
    uint8_t r0 = ps2_mouse_read();
    uint8_t r1 = ps2_mouse_read();
    uint8_t r2 = ps2_mouse_read();
    (void)r0; (void)r2;

    return r1 == 0x47;
}

/* "Sliced command": smuggle an arbitrary byte to the touchpad firmware
 * through the standard Set Resolution command, 2 bits at a time (MSB
 * pair first), preceded by Set Scale 1:1 - the whole trick a plain
 * PS/2 mouse interface has no vendor-specific command byte to carry
 * one directly. */
static void ps2_mouse_sliced_write(uint8_t val) {
    ps2_mouse_cmd0(0xE6); /* Set Scale 1:1 */
    for (int shift = 6; shift >= 0; shift -= 2) {
        uint8_t d = (uint8_t)((val >> shift) & 0x3u);
        ps2_mouse_cmd1(0xE8, d);
    }
}

#define SYN_BIT_ABSOLUTE_MODE 0x80u
#define SYN_BIT_W_MODE        0x01u
#define SYN_PS_SET_MODE2      0x14u

static void synaptics_enable_absolute_mode(void) {
    uint8_t mode = SYN_BIT_ABSOLUTE_MODE | SYN_BIT_W_MODE;
    ps2_mouse_sliced_write(mode);
    ps2_mouse_cmd1(0xF3, SYN_PS_SET_MODE2); /* Set Sample Rate <- SET_MODE2 */
}

void mouse_init(void) {
    /* Enable AUX port */
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);

    /* Enable AUX interrupt in 8042 command byte */
    ps2_wait_write();
    outb(PS2_CMD, 0x20);           /* read command byte */
    ps2_wait_read();
    uint8_t cb = inb(PS2_DATA);
    cb |= 0x02;                    /* enable IRQ12 (AUX) */
    cb &= ~0x20;                   /* clear "disable mouse" bit */
    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cb);

    /* Reset mouse */
    ps2_mouse_write(0xFF);
    ps2_mouse_read();  /* ACK */
    ps2_mouse_read();  /* 0xAA */
    ps2_mouse_read();  /* 0x00 */

    syn_detected = synaptics_detect();

    if (syn_detected) {
        synaptics_enable_absolute_mode();
        mouse_pkt_size = 6;
    } else {
        /* Standard PS/2 mouse: set defaults before enabling. */
        ps2_mouse_cmd0(0xF6);
        mouse_pkt_size = 3;
    }

    ps2_mouse_cmd0(0xF4); /* enable data reporting */

    mouse_enabled = 1;
}

int mouse_is_touchpad(void) {
    return syn_detected;
}

static mouse_state_t last_mouse = {0,0,0,0,0};

/* Synaptics absolute-stroke tracking, converted to the same relative
 * dx/dy the rest of the OS already consumes, plus software tap-to-click
 * (taps aren't a packet bit - they're a brief, low-movement touch
 * detected by watching the stream, same as every real Synaptics driver
 * does it). */
static int syn_touch_active = 0;
static int syn_prev_x = 0, syn_prev_y = 0;
static int syn_start_x = 0, syn_start_y = 0;
static uint32_t syn_start_tick = 0;
static int syn_tap_click_pulse = 0;

#define SYN_TAP_MAX_TICKS  25   /* ~250ms at the 100Hz PIT rate */
#define SYN_TAP_MAX_MOVE   100  /* touchpad position units, not pixels */
#define SYN_SENSITIVITY_SHIFT 2 /* divide raw units -> cursor mickeys;
                                   raise this if the cursor feels too
                                   fast on real hardware */

static int abs_i(int v) { return v < 0 ? -v : v; }

void mouse_on_aux_byte(uint8_t b) {
    if (!mouse_enabled) return;

    if (mouse_pkt_i == 0) {
        /* validate sync bit (bit 3 of the first byte is always 1,
         * in both the plain and Synaptics packet formats) */
        if (!(b & 0x08)) return;
    }
    if (mouse_pkt_i < mouse_pkt_size) {
        mouse_pkt[mouse_pkt_i++] = b;
    }
}

static int mouse_pkt_ready(void) {
    return mouse_pkt_i >= mouse_pkt_size;
}

static void synaptics_consume_ready(void) {
    const uint8_t* buf = mouse_pkt;

    int x = (int)(((buf[3] & 0x10u) << 8) | ((buf[1] & 0x0fu) << 8) | buf[4]);
    int y = (int)(((buf[3] & 0x20u) << 7) | ((buf[1] & 0xf0u) << 4) | buf[5]);
    int z = buf[2];
    int phys_left  = buf[0] & 0x01;
    int phys_right = (buf[0] >> 1) & 0x01;

    int touching = (z > 0);

    if (touching) {
        if (!syn_touch_active) {
            syn_touch_active = 1;
            syn_start_tick = timer_ticks();
            syn_start_x = x;
            syn_start_y = y;
            syn_prev_x = x;
            syn_prev_y = y;
        } else {
            int dx = (x - syn_prev_x) >> SYN_SENSITIVITY_SHIFT;
            /* Synaptics Y increases upward on the pad; screen Y
             * increases downward, so flip it here. */
            int dy = -((y - syn_prev_y) >> SYN_SENSITIVITY_SHIFT);
            last_mouse.dx = dx;
            last_mouse.dy = dy;
            syn_prev_x = x;
            syn_prev_y = y;
        }
    } else if (syn_touch_active) {
        syn_touch_active = 0;
        uint32_t held = timer_ticks() - syn_start_tick;
        if (held <= SYN_TAP_MAX_TICKS &&
            abs_i(x - syn_start_x) < SYN_TAP_MAX_MOVE &&
            abs_i(y - syn_start_y) < SYN_TAP_MAX_MOVE) {
            syn_tap_click_pulse = 1; /* synthesize one click frame */
        }
    }

    last_mouse.btn_left  = phys_left  || syn_tap_click_pulse;
    last_mouse.btn_right = phys_right;
    syn_tap_click_pulse = 0; /* one mouse_read() frame only */

    mouse_pkt_i = 0;
}

static void mouse_consume_ready(void) {
    if (syn_detected) {
        synaptics_consume_ready();
        return;
    }

    uint8_t b0 = mouse_pkt[0];
    uint8_t b1 = mouse_pkt[1];
    uint8_t b2 = mouse_pkt[2];

    last_mouse.btn_left   = b0 & 0x01;
    last_mouse.btn_right  = (b0 >> 1) & 0x01;
    last_mouse.btn_middle = (b0 >> 2) & 0x01;

    last_mouse.dx = (int)b1 - ((b0 & 0x10) ? 256 : 0);
    last_mouse.dy = (int)b2 - ((b0 & 0x20) ? 256 : 0);

    mouse_pkt_i = 0;
}

/*
 * Non-blocking mouse read: if a full packet (3 bytes plain, 6 bytes
 * Synaptics absolute) is available in the 8042 output buffer (bit 5 of
 * status = AUX data), consume it.
 */
mouse_state_t mouse_read(void) {
    if (!mouse_enabled) return last_mouse;

    /* If no new packet arrives, deltas must be 0 (avoid cursor drift). */
    last_mouse.dx = 0;
    last_mouse.dy = 0;

    if (mouse_pkt_ready()) {
        mouse_consume_ready();
        return last_mouse;
    }

    /* Check if data is from AUX (bit 5 set) and available (bit 0 set) */
    uint8_t st = inb(PS2_STATUS);
    if (!((st & 0x01) && (st & 0x20))) return last_mouse;

    /* Drain as many AUX bytes as available into the packet assembler */
    while (1) {
        st = inb(PS2_STATUS);
        if (!((st & 0x01) && (st & 0x20))) break;
        uint8_t b = inb(PS2_DATA);
        mouse_on_aux_byte(b);
        if (mouse_pkt_ready()) break;
    }

    if (mouse_pkt_ready()) {
        mouse_consume_ready();
    }

    return last_mouse;
}
