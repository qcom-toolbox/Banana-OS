#include "touchpad.h"
#include "kstring.h"
#include "i2chid.h"
#include "usb.h"
#include "../usb/usbcore.h"
#include "terminal.h"
#include "types.h"
#include "timer.h"
#include "serial.h"

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

    char tdesc[96];
    touchpad_describe(tdesc, sizeof(tdesc));
    const char* t = tdesc;
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
/* an answer that can take long (a reset: up to ~1 s on some touchpads);
 * -1 if none came */
static int ps2_mouse_read_long(int ms) {
    for (int i = 0; i < ms * 1000; i++) {
        if (inb(PS2_STATUS) & 0x01) return inb(PS2_DATA);
        inb(0x80);                                 /* ~1 us */
    }
    return -1;
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

/* ── Synaptics identify, queries, mode ───────────────────────────────
 * A query: its number sent as a "sliced command" (Set Scale 1:1, then
 * four Set Resolution carrying 2 bits each, high bits first), then a
 * Status Request (0xE9) whose 3 bytes are the answer. Identify (query 0)
 * answers 0x47 in the middle byte on a Synaptics pad; a plain mouse
 * gives its normal status. */
static void ps2_mouse_sliced_write(uint8_t val) {
    ps2_mouse_cmd0(0xE6); /* Set Scale 1:1 */
    for (int shift = 6; shift >= 0; shift -= 2) {
        uint8_t d = (uint8_t)((val >> shift) & 0x3u);
        ps2_mouse_cmd1(0xE8, d);
    }
}

static void syn_query(uint8_t q, uint8_t r[3]) {
    ps2_mouse_sliced_write(q);
    ps2_mouse_write(0xE9);
    ps2_mouse_read();          /* ACK */
    r[0] = ps2_mouse_read();
    r[1] = ps2_mouse_read();
    r[2] = ps2_mouse_read();
}

static int synaptics_detect(void) {
    uint8_t r[3];
    syn_query(0x00, r);
    return r[1] == 0x47;
}

#define SYN_BIT_ABSOLUTE_MODE    0x80u
#define SYN_BIT_HIGH_RATE        0x40u
#define SYN_BIT_DISABLE_GESTURE  0x04u     /* no taps by the pad itself: ours instead */
#define SYN_BIT_W_MODE           0x01u
#define SYN_PS_SET_MODE2         0x14u

/* what the pad says about itself */
static uint32_t syn_caps, syn_ext, syn_ext0c;
static int      syn_fw_major, syn_fw_minor;
static touchpad_t g_syn_tp;

#define SYN_CAP_EXTENDED(c)     ((c) & 0x800000u)
#define SYN_EXT_QUERIES(c)      (((c) >> 20) & 7u)
#define SYN_CAP_MIDDLE(c)       ((c) & 0x040000u)
#define SYN_CAP_PASSTHROUGH(c)  ((c) & 0x80u)
#define SYN_CAP_PALM(c)         ((c) & 0x01u)
#define SYN_CAP_MULTIFINGER(c)  ((c) & 0x02u)
#define SYN_CAP_CLICKPAD(e)     ((e) & 0x100000u)   /* one button: the whole pad */
#define SYN_CAP_CLICKPAD2(e)    ((e) & 0x000100u)   /* two-button ClickPad */
#define SYN_CAP_MAX_DIM(e)      ((e) & 0x020000u)
#define SYN_CAP_MIN_DIM(e)      ((e) & 0x002000u)

static void synaptics_setup(void) {
    uint8_t r[3];
    syn_query(0x00, r);                         /* identify: firmware version */
    syn_fw_minor = r[0];
    syn_fw_major = r[2] & 0x0F;
    syn_query(0x02, r);                         /* capabilities */
    syn_caps = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
    if (!SYN_CAP_EXTENDED(syn_caps)) syn_caps = 0;
    syn_ext = syn_ext0c = 0;
    if (SYN_EXT_QUERIES(syn_caps) >= 1) { syn_query(0x09, r); syn_ext = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2]; }
    if (SYN_EXT_QUERIES(syn_caps) >= 4) { syn_query(0x0C, r); syn_ext0c = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2]; }

    /* the pad's area (the usual values when it cannot tell) */
    touchpad_t* t = &g_syn_tp;
    int xmin = 1472, xmax = 5472, ymin = 1408, ymax = 4448;
    if (SYN_CAP_MAX_DIM(syn_ext0c)) {
        syn_query(0x0D, r);
        int mx = (r[0] << 5) | ((r[1] & 0x0F) << 1), my = (r[2] << 5) | ((r[1] & 0xF0) >> 3);
        if (mx > 1000 && my > 1000) { xmax = mx; ymax = my; }
    }
    if (SYN_CAP_MIN_DIM(syn_ext0c) && (syn_fw_major > 7 || (syn_fw_major == 7 && syn_fw_minor >= 5))) {
        syn_query(0x0F, r);
        int mx = (r[0] << 5) | ((r[1] & 0x0F) << 1), my = (r[2] << 5) | ((r[1] & 0xF0) >> 3);
        if (mx > 0 && mx < xmax - 1000 && my > 0 && my < ymax - 1000) { xmin = mx; ymin = my; }
    }
    int upm = 0;
    if (syn_fw_major >= 4) {
        syn_query(0x08, r);                     /* resolution: units per mm */
        if ((r[1] & 0x80) && r[0] >= 10 && r[0] < 200) upm = r[0];
    }
    if (!upm) upm = (xmax - xmin) / 95;         /* (a pad is about 95 mm wide) */
    t->xmin = xmin; t->xmax = xmax; t->ymin = ymin; t->ymax = ymax;
    t->upm = upm > 0 ? upm : 40;
    t->clickpad = (SYN_CAP_CLICKPAD(syn_ext0c) || SYN_CAP_CLICKPAD2(syn_ext0c)) ? 1 : 0;
    klog("touchpad: Synaptics fw %d.%d caps %06x ext %06x ext0c %06x, x %d-%d y %d-%d, %d/mm%s\n",
         syn_fw_major, syn_fw_minor, syn_caps, syn_ext, syn_ext0c, xmin, xmax, ymin, ymax, t->upm,
         t->clickpad ? ", ClickPad" : "");
}

static void synaptics_enable_absolute_mode(void) {
    uint8_t mode = SYN_BIT_ABSOLUTE_MODE | SYN_BIT_HIGH_RATE | SYN_BIT_DISABLE_GESTURE;
    if (syn_caps) mode |= SYN_BIT_W_MODE;      /* finger count / width in every packet */
    ps2_mouse_sliced_write(mode);
    ps2_mouse_cmd1(0xF3, SYN_PS_SET_MODE2); /* Set Sample Rate <- SET_MODE2 */
}

/* "Synaptics ClickPad (firmware 7.5)", "PS/2 mouse", ... for usb_status() */
void touchpad_describe(char* out, int cap) {
    char i2c[64];
    if (i2chid_describe(i2c, sizeof(i2c))) { ksnprintf(out, (size_t)cap, " | Touchpad: %s", i2c); return; }
    if (syn_detected)
        ksnprintf(out, (size_t)cap, " | Touchpad: Synaptics %s (firmware %d.%d)",
                  g_syn_tp.clickpad ? "ClickPad" : "TouchPad", syn_fw_major, syn_fw_minor);
    else
        ksnprintf(out, (size_t)cap, " | Touchpad: none (PS/2 mouse)");
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

    /* Reset mouse (a touchpad's self test can take most of a second) */
    while (inb(PS2_STATUS) & 0x01) inb(PS2_DATA);
    ps2_mouse_write(0xFF);
    ps2_mouse_read_long(100);  /* ACK */
    ps2_mouse_read_long(1000); /* 0xAA */
    ps2_mouse_read_long(100);  /* 0x00 */

    syn_detected = synaptics_detect();

    if (syn_detected) {
        synaptics_setup();
        synaptics_enable_absolute_mode();
        mouse_pkt_size = 6;
    } else {
        /* Standard PS/2 mouse: set defaults before enabling. */
        ps2_mouse_cmd0(0xF6);
        mouse_pkt_size = 3;
        /* IntelliMouse knock: sample rates 200, 100, 80 - a wheel mouse
         * then reports ID 3 (or 4) and sends 4-byte packets */
        ps2_mouse_cmd1(0xF3, 200);
        ps2_mouse_cmd1(0xF3, 100);
        ps2_mouse_cmd1(0xF3, 80);
        ps2_mouse_write(0xF2);
        ps2_mouse_read();                  /* ACK */
        uint8_t id = ps2_mouse_read();
        if (id == 3 || id == 4) mouse_pkt_size = 4;
        klog("mouse: PS/2 id %u, %d-byte packets\n", id, mouse_pkt_size);
        ps2_mouse_cmd1(0xF3, 100);         /* back to a normal rate */
    }

    ps2_mouse_cmd0(0xF4); /* enable data reporting */

    mouse_enabled = 1;
}

int mouse_is_touchpad(void) {
    return syn_detected;
}

static mouse_state_t last_mouse = {0,0,0,0,0,0};



/* Complete packets wait in a small queue: bytes can arrive through
 * keyboard_try_getchar() as well as mouse_read(), and a quick press +
 * release must not be lost because the first packet was not read yet. */
#define MPKT_RING 32
static uint8_t mouse_asm[6];
static int     mouse_asm_i = 0;
static uint8_t mouse_ring[MPKT_RING][6];
static int     mouse_ring_head = 0, mouse_ring_len = 0;

void mouse_on_aux_byte(uint8_t b) {
    if (!mouse_enabled) return;

    if (syn_detected) {
        /* Synaptics absolute packets: byte 0 is 10xx0xxx, byte 3 11xx0xxx
         * - a byte that breaks this drops the packet (and may start one) */
        if (mouse_asm_i == 0 && (b & 0xC8) != 0x80) return;
        if (mouse_asm_i == 3 && (b & 0xC8) != 0xC0) {
            mouse_asm_i = 0;
            if ((b & 0xC8) != 0x80) return;
        }
    } else if (mouse_asm_i == 0) {
        /* validate sync bit (bit 3 of the first byte is always 1) */
        if (!(b & 0x08)) return;
    }
    mouse_asm[mouse_asm_i++] = b;
    if (mouse_asm_i >= mouse_pkt_size) {
        if (mouse_ring_len == MPKT_RING) {          /* full: drop the oldest */
            mouse_ring_head = (mouse_ring_head + 1) % MPKT_RING;
            mouse_ring_len--;
        }
        int slot = (mouse_ring_head + mouse_ring_len) % MPKT_RING;
        for (int i = 0; i < 6; i++) mouse_ring[slot][i] = mouse_asm[i];
        mouse_ring_len++;
        mouse_asm_i = 0;
    }
}

static int mouse_pkt_ready(void) {
    return mouse_ring_len > 0;
}

/* the oldest queued packet -> mouse_pkt, for the consume functions */
static void mouse_pop(void) {
    for (int i = 0; i < 6; i++) mouse_pkt[i] = mouse_ring[mouse_ring_head][i];
    mouse_ring_head = (mouse_ring_head + 1) % MPKT_RING;
    mouse_ring_len--;
}

/* one absolute packet -> the gesture engine (touchpad.c) */
static void synaptics_consume_ready(void) {
    const uint8_t* buf = mouse_pkt;
    int w = ((buf[0] & 0x30) >> 2) | ((buf[0] & 0x04) >> 1) | ((buf[3] & 0x04) >> 2);
    if (syn_caps && w == 3) {
        /* pass-through: a packet of the TrackPoint behind the pad */
        if (SYN_CAP_PASSTHROUGH(syn_caps)) {
            int dx = (int)buf[4] - ((buf[1] & 0x10) ? 256 : 0);
            int dy = (int)buf[5] - ((buf[1] & 0x20) ? 256 : 0);
            mouse_inject(dx, dy, (buf[1] & 7) | g_syn_tp.out);
        }
        return;
    }
    if (syn_caps && w == 2) return;             /* an extended-W packet (not asked for) */

    int x = (int)(((buf[3] & 0x10u) << 8) | ((buf[1] & 0x0fu) << 8) | buf[4]);
    int y = (int)(((buf[3] & 0x20u) << 7) | ((buf[1] & 0xf0u) << 4) | buf[5]);
    int z = buf[2];
    int buttons = (buf[0] & 0x01) | (buf[0] & 0x02);
    int mid = (buf[0] ^ buf[3]) & 0x01;
    if (g_syn_tp.clickpad || !SYN_CAP_MIDDLE(syn_caps)) buttons |= mid;   /* a ClickPad's press */
    else if (mid) buttons |= 4;

    /* fingers: a touch is z >= 30 (25 to stay down); W tells how many */
    static int down;
    int touching = z >= (down ? 25 : 30);
    down = touching;
    int fingers = touching ? 1 : 0;
    if (touching && syn_caps && SYN_CAP_MULTIFINGER(syn_caps)) {
        if (w == 0) fingers = 2;
        else if (w == 1) fingers = 3;
    }
    int flags = 0;
    if (touching && syn_caps && SYN_CAP_PALM(syn_caps) && w >= 12 && z >= 200) flags |= TPF_PALM;
    /* the pad's y grows upwards, the screen's downwards */
    y = g_syn_tp.ymax + g_syn_tp.ymin - y;
    tp_frame(&g_syn_tp, fingers, x, y, buttons, flags);
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
    if (mouse_pkt_size == 4) {
        int z = mouse_pkt[3] & 0x0F;                /* 4-bit signed (the high bits are buttons 4/5 on ID 4) */
        if (z & 8) z -= 16;
        last_mouse.dz = z;
    }

    mouse_pkt_i = 0;
}

/*
 * Non-blocking mouse read: if a full packet (3 bytes plain, 6 bytes
 * Synaptics absolute) is available in the 8042 output buffer (bit 5 of
 * status = AUX data), consume it.
 */
/* USB mice (usb/usbhid.c) add their motion here; it's merged into the
 * next mouse_read(). dy follows the PS/2 convention (positive = up). */
static int g_usb_dx, g_usb_dy, g_usb_dz;
/* every button change, in order: each mouse_read() gives one, so a quick
 * double-click (press, release, press between two frames of the desktop -
 * VirtualBox sends its reports in bursts) is still two clicks */
#define USB_BTNQ 16
static int g_usb_btnq[USB_BTNQ], g_usb_btnq_n, g_usb_btn_last = -1;

void mouse_inject_wheel(int dz) { g_usb_dz += dz; }

void mouse_inject(int dx, int dy, int buttons) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    g_usb_dx += dx;
    g_usb_dy += dy;
    if (buttons != g_usb_btn_last) {
        if (g_usb_btnq_n < USB_BTNQ) g_usb_btnq[g_usb_btnq_n++] = buttons;
        else g_usb_btnq[USB_BTNQ - 1] = buttons;
        g_usb_btn_last = buttons;
    }
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* ── the pointer from the timer interrupt (kernel/gui.c) ─────────────
 * The desktop reads the mouse only when its task runs; while another task
 * holds the CPU (a save, a page being laid out) the pointer would stop.
 * The timer interrupt takes the motion of the queued plain packets here
 * and moves the pointer itself. Packets that press or release a button
 * or turn the wheel are left for mouse_read(), so every click is still
 * seen by the desktop, in order. A keyboard byte waiting first in the
 * controller is left alone too (it is the keyboard's). The task side
 * (mouse_read, keyboard_try_getchar) runs with interrupts off while it
 * touches the controller or the packet queue. */
/* pointer speed: the motion times SPEED_X20[speed] / 20, the fractions
 * kept for the next move (slow speeds still move by single pixels) */
static const int SPEED_X20[11] = { 20, 5, 8, 12, 16, 20, 26, 34, 44, 56, 70 };
static int g_mouse_speed = 5, g_frac_x, g_frac_y;

void mouse_set_speed(int speed) {
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;
    g_mouse_speed = speed;
    g_frac_x = g_frac_y = 0;
}
int mouse_get_speed(void) { return g_mouse_speed; }

static int scale_axis(int d, int* frac) {
    if (!d) return 0;
    if ((d > 0 && *frac < 0) || (d < 0 && *frac > 0)) *frac = 0;     /* turned back */
    int v = d * SPEED_X20[g_mouse_speed] + *frac;
    *frac = v % 20;
    return v / 20;
}
static void scale_motion(int* dx, int* dy) {
    if (g_mouse_speed == 5) return;
    *dx = scale_axis(*dx, &g_frac_x);
    *dy = scale_axis(*dy, &g_frac_y);
}

int mouse_irq_motion(int* dx, int* dy) {
    if (!mouse_enabled || syn_detected) return 0;
    for (;;) {
        uint8_t st = inb(PS2_STATUS);
        if (!((st & 0x01) && (st & 0x20))) break;
        mouse_on_aux_byte(inb(PS2_DATA));
    }
    int sx = 0, sy = 0, any = 0;
    while (mouse_ring_len > 0) {
        const uint8_t* p = mouse_ring[mouse_ring_head];
        uint8_t btn = (uint8_t)(last_mouse.btn_left | last_mouse.btn_right << 1 | last_mouse.btn_middle << 2);
        if ((p[0] & 7) != btn) break;                         /* a click: the desktop's */
        if (mouse_pkt_size == 4 && (p[3] & 0x0F)) break;      /* the wheel: the desktop's */
        sx += (int)p[1] - ((p[0] & 0x10) ? 256 : 0);
        sy += (int)p[2] - ((p[0] & 0x20) ? 256 : 0);
        mouse_ring_head = (mouse_ring_head + 1) % MPKT_RING;
        mouse_ring_len--;
        any = 1;
    }
    scale_motion(&sx, &sy);
    *dx = sx;
    *dy = sy;
    return any;
}

static inline uintptr_t irq_off(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_on(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static mouse_state_t mouse_read_locked(void);

mouse_state_t mouse_read(void) {
    usb_poll();                  /* USB mice report through here too: every frame */
    i2chid_poll();               /* and I2C touchpads */
    uintptr_t f = irq_off();
    mouse_state_t m = mouse_read_locked();
    scale_motion(&m.dx, &m.dy);
    irq_on(f);
    return m;
}

static mouse_state_t mouse_read_locked(void) {
    if (mouse_enabled && syn_detected) {
        /* the touchpad's packets become motion / clicks (mouse_inject) */
        for (;;) {
            uint8_t st = inb(PS2_STATUS);
            if (!((st & 0x01) && (st & 0x20))) break;
            mouse_on_aux_byte(inb(PS2_DATA));
        }
        while (mouse_pkt_ready()) { mouse_pop(); synaptics_consume_ready(); }
        tp_tick(&g_syn_tp);
    }
    if (g_usb_dx || g_usb_dy || g_usb_btnq_n || g_usb_dz) {
        mouse_state_t m = last_mouse;
        m.dx = g_usb_dx;
        m.dy = g_usb_dy;
        m.dz = g_usb_dz;
        g_usb_dz = 0;
        if (g_usb_btnq_n) {                       /* the oldest change; the others next time */
            int b = g_usb_btnq[0];
            for (int i = 1; i < g_usb_btnq_n; i++) g_usb_btnq[i - 1] = g_usb_btnq[i];
            g_usb_btnq_n--;
            m.btn_left = b & 1;
            m.btn_right = (b >> 1) & 1;
            m.btn_middle = (b >> 2) & 1;
            last_mouse.btn_left = m.btn_left;
            last_mouse.btn_right = m.btn_right;
            last_mouse.btn_middle = m.btn_middle;
        }
        g_usb_dx = g_usb_dy = 0;
        return m;
    }
    if (!mouse_enabled) return last_mouse;

    /* If no new packet arrives, deltas must be 0 (avoid cursor drift). */
    last_mouse.dx = 0;
    last_mouse.dy = 0;
    last_mouse.dz = 0;

    /* Drain the AUX bytes waiting in the controller (status: bit 0 =
     * data available, bit 5 = from the mouse) into the packet queue */
    for (;;) {
        uint8_t st = inb(PS2_STATUS);
        if (!((st & 0x01) && (st & 0x20))) break;
        mouse_on_aux_byte(inb(PS2_DATA));
    }

    /* Motion of all queued packets adds up; a button change ends the
     * batch, so every press and release is seen by some mouse_read()
     * (a double-click is two separate presses, however quick). */
    int sdx = 0, sdy = 0, sdz = 0;
    while (mouse_pkt_ready()) {
        int bl = last_mouse.btn_left, br = last_mouse.btn_right, bm = last_mouse.btn_middle;
        mouse_pop();
        last_mouse.dx = 0;            /* a touchpad packet may carry no motion */
        last_mouse.dy = 0;
        last_mouse.dz = 0;
        mouse_consume_ready();
        sdx += last_mouse.dx;
        sdy += last_mouse.dy;
        sdz += last_mouse.dz;
        if (last_mouse.btn_left != bl || last_mouse.btn_right != br || last_mouse.btn_middle != bm) break;
    }
    last_mouse.dx = sdx;
    last_mouse.dy = sdy;
    last_mouse.dz = sdz;
    return last_mouse;
}
