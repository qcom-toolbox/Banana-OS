/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbcore.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"
#include "keyboard.h"
#include "usb.h"
#include "hidparse.h"
#include "gamepad.h"

/*
 * USB HID keyboards, mice, tablets, touchscreens and game controllers.
 *
 * Once Banana OS drives a USB controller itself, the BIOS stops faking
 * USB keyboards/mice as PS/2 devices - so this driver takes over: key
 * reports become set-1 scancodes fed through the PS/2 decoder
 * (keyboard_feed_scancode), mouse reports become mouse_inject() motion.
 * Keyboards and boot mice use the boot protocol (every one supports it);
 * other pointers are read through their report descriptor (hidparse.c):
 * absolute X / Y - a tablet (VirtualBox's and QEMU's "USB tablet", the
 * pointer that follows the host's mouse) or a touchscreen - move the
 * pointer to a place (mouse_inject_abs), relative ones move it by.
 * Joysticks and gamepads (their application collection says so) become
 * game controllers (gamepad.c): X / Y the left stick, Z / Rz or Rx / Ry
 * the right one, the hat switch the d-pad, buttons 1-13 in the usual
 * DirectInput order (PlayStation and most PC pads: X A B Y LB RB LT RT
 * Back Start LS RS Guide).
 */

#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_DT_HID       0x21
#define HID_DT_REPORT    0x22
#define MAX_FIELDS       96

#define MAX_IFACES 2
#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS  33

/* HID usage (0x04..0x81) -> set-1 scancode; 0xE0xx = extended */
static const uint16_t g_usage_sc[0x82] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C,
    [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0xE037, [0x47] = 0x46, [0x49] = 0xE052, [0x4A] = 0xE047, [0x4B] = 0xE049,
    [0x4C] = 0xE053, [0x4D] = 0xE04F, [0x4E] = 0xE051, [0x4F] = 0xE04D, [0x50] = 0xE04B,
    [0x51] = 0xE050, [0x52] = 0xE048, [0x53] = 0x45, [0x54] = 0xE035, [0x55] = 0x37,
    [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0xE01C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = 0xE05D,
    [0x7F] = 0xE020, [0x80] = 0xE030, [0x81] = 0xE02E,     /* mute, volume up, down */
};

/* modifier bits (report byte 0) -> scancode */
static const uint16_t g_mod_sc[8] = { 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C };
/* Apple keyboards (a Mac's own, or one plugged in): Command is Ctrl, so the
 * shortcuts are where Mac hands expect them (Cmd+C, Cmd+V, Cmd+Z...);
 * Control stays Control, Option is Alt */
static const uint16_t g_mod_sc_apple[8] = { 0x1D, 0x2A, 0x38, 0xE01D, 0xE01D, 0x36, 0xE038, 0xE01D };

typedef struct {
    int        is_kbd;
    int        apple;            /* an Apple keyboard (vendor 05ac) */
    int        report_mode;      /* a pointer read through its report descriptor */
    int        absolute;         /* ... with absolute X / Y */
    uint8_t    ep;
    usb_xfer_t xfer;
    uint8_t    prev[8];          /* keyboard: previous report */
    uint8_t    repeat_usage;
    uint32_t   repeat_at;
    /* report mode: where X, Y, the buttons, the wheel are */
    hid_field_t* fields;
    int        nfields;
    int        uses_rid;
    int        fx, fy, fwheel, ftip, frange, fbtn[3];
    /* a game controller: its fields, its slot in gamepad.c */
    int        is_pad, pad;
    int        src;              /* a pointer: its own buttons (kernel/usb.h), 0 until first used */
    uint8_t    pad_rid;
    int        fp[9], fpb[13];
    uint32_t   reports;          /* (lsusb: what it has sent) */
    int        last_x, last_y;   /* an absolute pointer's last place (0 - 65535) */
} hid_iface_t;

enum { PF_X, PF_Y, PF_Z, PF_RX, PF_RY, PF_RZ, PF_HAT, PF_BRAKE, PF_ACCEL };

static int is_pad_app(uint32_t a) { return a == HID_U(1, 4) || a == HID_U(1, 5) || a == HID_U(1, 8); }

typedef struct {
    hid_iface_t ifs[MAX_IFACES];
    int         count;
} hid_t;

static void send_sc(uint16_t sc, int release) {
    if (sc & 0xE000) keyboard_feed_scancode(0xE0);
    keyboard_feed_scancode((uint8_t)((sc & 0xFF) | (release ? 0x80 : 0)));
}

static int in_report(const uint8_t* r, uint8_t usage) {
    for (int i = 2; i < 8; i++) if (r[i] == usage) return 1;
    return 0;
}

static void keyboard_report(hid_iface_t* h, const uint8_t* r) {
    if (r[2] == 1) return;                     /* ErrorRollOver: ignore */
    uint8_t changed = (uint8_t)(r[0] ^ h->prev[0]);
    for (int b = 0; b < 8; b++)
        if (changed & (1u << b)) send_sc((h->apple ? g_mod_sc_apple : g_mod_sc)[b], !(r[0] & (1u << b)));
    for (int i = 2; i < 8; i++) {              /* released */
        uint8_t u = h->prev[i];
        if (u >= 4 && u < 0x82 && !in_report(r, u) && g_usage_sc[u]) send_sc(g_usage_sc[u], 1);
    }
    for (int i = 2; i < 8; i++) {              /* newly pressed */
        uint8_t u = r[i];
        if (u >= 4 && u < 0x82 && !in_report(h->prev, u) && g_usage_sc[u]) {
            send_sc(g_usage_sc[u], 0);
            h->repeat_usage = u;
            h->repeat_at = timer_ms() + REPEAT_DELAY_MS;
        }
    }
    if (h->repeat_usage && !in_report(r, h->repeat_usage)) h->repeat_usage = 0;
    memcpy(h->prev, r, 8);
}

/* ── probe / attach ─────────────────────────────────────────────── */

typedef struct {
    hid_t*  hid;          /* NULL while probing */
    int     found;
    int     cur_boot;     /* current interface: 1 kbd, 2 mouse, 3 another HID (a pointer?), 0 not HID */
    uint8_t cur_num;
    uint16_t cur_rlen;    /* its report descriptor's length (HID descriptor) */
    usb_device_t* dev;
} walk_t;

/* a joystick / gamepad: where its sticks, hat and buttons are (1), or -1 */
static int pad_setup(hid_iface_t* h, hid_field_t* f, int n) {
    for (int k = 0; k < 9; k++) h->fp[k] = -1;
    for (int k = 0; k < 13; k++) h->fpb[k] = -1;
    int rid = -1;
    for (int i = 0; i < n; i++) {
        if (f[i].rid) h->uses_rid = 1;
        if (f[i].type != HID_INPUT || (f[i].flags & 1) || !is_pad_app(f[i].app)) continue;
        if (rid < 0) rid = f[i].rid;
        else if (f[i].rid != rid) continue;               /* (its first input report only) */
        uint32_t u = f[i].usage;
        int k = u == HID_U(1, 0x30) ? PF_X : u == HID_U(1, 0x31) ? PF_Y : u == HID_U(1, 0x32) ? PF_Z :
                u == HID_U(1, 0x33) ? PF_RX : u == HID_U(1, 0x34) ? PF_RY : u == HID_U(1, 0x35) ? PF_RZ :
                u == HID_U(1, 0x39) ? PF_HAT : u == HID_U(2, 0xC5) ? PF_BRAKE : u == HID_U(2, 0xC4) ? PF_ACCEL : -1;
        if (k >= 0) { if (h->fp[k] < 0) h->fp[k] = i; }
        else if (u >= HID_U(9, 1) && u <= HID_U(9, 13) && h->fpb[u - HID_U(9, 1)] < 0) h->fpb[u - HID_U(9, 1)] = i;
    }
    if (rid < 0 || (h->fp[PF_X] < 0 && h->fp[PF_HAT] < 0 && h->fpb[0] < 0)) return -1;
    h->pad_rid = (uint8_t)rid;
    h->fields = f;
    h->nfields = n;
    h->is_pad = 1;
    h->report_mode = 1;
    return 1;
}

/* the fields of a pointer's report descriptor: 0 if it is one we can read
 * (1: it is a game controller - pad_setup) */
static int pointer_setup(usb_device_t* d, hid_iface_t* h, uint8_t iface, uint16_t rlen) {
    if (rlen == 0 || rlen > 4096) return -1;
    uint8_t* desc = (uint8_t*)kmalloc(rlen);
    if (!desc) return -1;
    int got = usb_control(d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_IFACE, USB_REQ_GET_DESCRIPTOR,
                          (uint16_t)(HID_DT_REPORT << 8), iface, desc, rlen);
    if (got <= 0) { kfree(desc); return -1; }
    hid_field_t* f = (hid_field_t*)kmalloc(sizeof(hid_field_t) * MAX_FIELDS);
    int n = f ? hid_parse(desc, got, f, MAX_FIELDS) : -1;
    kfree(desc);
    if (n <= 0) { if (f) kfree(f); return -1; }
    for (int i = 0; i < n; i++)
        if (f[i].type == HID_INPUT && is_pad_app(f[i].app)) {
            if (pad_setup(h, f, n) == 1) return 1;
            kfree(f);
            return -1;
        }
    h->fx = h->fy = h->fwheel = h->ftip = h->frange = -1;
    h->fbtn[0] = h->fbtn[1] = h->fbtn[2] = -1;
    for (int i = 0; i < n; i++) {
        if (f[i].type != HID_INPUT || (f[i].flags & 1)) continue;          /* inputs, not padding */
        if (f[i].rid) h->uses_rid = 1;
        uint32_t u = f[i].usage;
        if (u == HID_U(1, 0x30) && h->fx < 0) h->fx = i;
        else if (u == HID_U(1, 0x31) && h->fy < 0) h->fy = i;
        else if (u == HID_U(1, 0x38) && h->fwheel < 0) h->fwheel = i;
        else if (u == HID_U(0x0D, 0x42) && h->ftip < 0) h->ftip = i;       /* a touchscreen's finger down */
        else if (u == HID_U(0x0D, 0x32) && h->frange < 0) h->frange = i;   /* a pen / finger in range */
        else if (u >= HID_U(9, 1) && u <= HID_U(9, 3) && h->fbtn[u - HID_U(9, 1)] < 0) h->fbtn[u - HID_U(9, 1)] = i;
    }
    if (h->fx < 0 || h->fy < 0 || f[h->fx].rid != f[h->fy].rid) { kfree(f); return -1; }
    h->absolute = !(f[h->fx].flags & 4);
    h->fields = f;
    h->nfields = n;
    h->report_mode = 1;
    return 0;
}

static int field_val(hid_iface_t* h, int i, const uint8_t* r, int len, uint8_t rid) {
    if (i < 0 || h->fields[i].rid != rid) return 0;
    return (int)hid_value(r, len, &h->fields[i]);
}

/* a report of a pointer read through its descriptor */
static void pointer_report(hid_iface_t* h, const uint8_t* buf, int len) {
    uint8_t rid = 0;
    if (h->uses_rid) { if (len < 1) return; rid = buf[0]; buf++; len--; }
    if (h->fields[h->fx].rid != rid) return;       /* (another report of the device) */
    int b = 0;
    for (int k = 0; k < 3; k++) if (field_val(h, h->fbtn[k], buf, len, rid)) b |= 1 << k;
    if (h->ftip >= 0 && h->fbtn[0] < 0 && field_val(h, h->ftip, buf, len, rid)) b |= 1;
    int x = field_val(h, h->fx, buf, len, rid), y = field_val(h, h->fy, buf, len, rid);
    /* a digitizer out of range (no pen, no finger): its X / Y are not a
     * place - often a parked corner. Its buttons are let go, nothing moves. */
    if (h->absolute && h->frange >= 0 && h->fields[h->frange].rid == rid && !field_val(h, h->frange, buf, len, rid)) {
        mouse_inject_src(mouse_src(&h->src), 0, 0, 0);
        return;
    }
    if (h->absolute) {
        const hid_field_t* fx = &h->fields[h->fx];
        const hid_field_t* fy = &h->fields[h->fy];
        int32_t wx = fx->lmax > fx->lmin ? fx->lmax - fx->lmin : 1, wy = fy->lmax > fy->lmin ? fy->lmax - fy->lmin : 1;
        h->last_x = (int)((int64_t)(x - fx->lmin) * 65535 / wx);
        h->last_y = (int)((int64_t)(y - fy->lmin) * 65535 / wy);
        mouse_inject_abs_src(mouse_src(&h->src), h->last_x, h->last_y, b);
    } else mouse_inject_src(mouse_src(&h->src), x, -y, b);
    int wheel = field_val(h, h->fwheel, buf, len, rid);
    if (wheel) mouse_inject_wheel(-wheel);
}

static int pad_axis(hid_iface_t* h, int k, const uint8_t* r, int len) {
    if (h->fp[k] < 0) return 0;
    const hid_field_t* f = &h->fields[h->fp[k]];
    return gamepad_axis(hid_value(r, len, f), f->lmin, f->lmax);
}

static int pad_trigger(hid_iface_t* h, int k, const uint8_t* r, int len) {
    if (h->fp[k] < 0) return 0;
    const hid_field_t* f = &h->fields[h->fp[k]];
    int32_t v = hid_value(r, len, f);
    if (f->lmax <= f->lmin || v <= f->lmin) return 0;
    if (v >= f->lmax) return 32767;
    return (int)((int64_t)(v - f->lmin) * 32767 / (f->lmax - f->lmin));
}

/* a report of a joystick / gamepad */
static void pad_report(hid_iface_t* h, const uint8_t* buf, int len) {
    static const uint32_t BTN[13] = { GP_X, GP_A, GP_B, GP_Y, GP_LB, GP_RB, GP_LT, GP_RT,
                                      GP_BACK, GP_START, GP_LS, GP_RS, GP_GUIDE };
    static const uint32_t HAT[8] = { GP_UP, GP_UP | GP_RIGHT, GP_RIGHT, GP_DOWN | GP_RIGHT,
                                     GP_DOWN, GP_DOWN | GP_LEFT, GP_LEFT, GP_UP | GP_LEFT };
    uint8_t rid = 0;
    if (h->uses_rid) { if (len < 1) return; rid = buf[0]; buf++; len--; }
    if (rid != h->pad_rid || h->pad < 0) return;
    gamepad_t s;
    memset(&s, 0, sizeof(s));
    s.lx = pad_axis(h, PF_X, buf, len);
    s.ly = pad_axis(h, PF_Y, buf, len);
    int analog = 0;
    if (h->fp[PF_Z] >= 0 && h->fp[PF_RZ] >= 0) {          /* right stick Z / Rz, triggers Rx / Ry */
        s.rx = pad_axis(h, PF_Z, buf, len);
        s.ry = pad_axis(h, PF_RZ, buf, len);
        if (h->fp[PF_RX] >= 0 && h->fp[PF_RY] >= 0) {
            s.lt = pad_trigger(h, PF_RX, buf, len);
            s.rt = pad_trigger(h, PF_RY, buf, len);
            analog = 1;
        }
    } else {
        s.rx = pad_axis(h, PF_RX, buf, len);
        s.ry = pad_axis(h, PF_RY, buf, len);
    }
    if (h->fp[PF_BRAKE] >= 0 || h->fp[PF_ACCEL] >= 0) {
        s.lt = pad_trigger(h, PF_BRAKE, buf, len);
        s.rt = pad_trigger(h, PF_ACCEL, buf, len);
        analog = 1;
    }
    for (int k = 0; k < 13; k++)
        if (h->fpb[k] >= 0 && hid_value(buf, len, &h->fields[h->fpb[k]])) s.buttons |= BTN[k];
    if (!analog) {                                         /* digital triggers */
        if (s.buttons & GP_LT) s.lt = 32767;
        if (s.buttons & GP_RT) s.rt = 32767;
    }
    if (h->fp[PF_HAT] >= 0) {
        const hid_field_t* f = &h->fields[h->fp[PF_HAT]];
        int32_t v = hid_value(buf, len, f);
        int32_t span = f->lmax - f->lmin;
        if (v >= f->lmin && v <= f->lmax && (span == 7 || span == 3))
            s.buttons |= HAT[(v - f->lmin) * (span == 3 ? 2 : 1)];
    }
    gamepad_update(h->pad, &s);
}

static int walk_cb(const uint8_t* desc, void* ctx) {
    walk_t* w = (walk_t*)ctx;
    if (desc[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)desc;
        w->cur_boot = 0;
        w->cur_rlen = 0;
        if (id->bAlternateSetting == 0 && id->bInterfaceClass == 3) {
            if (id->bInterfaceSubClass == 1 && (id->bInterfaceProtocol == 1 || id->bInterfaceProtocol == 2))
                w->cur_boot = id->bInterfaceProtocol;
            else if (id->bInterfaceProtocol != 1)
                w->cur_boot = 3;                   /* maybe a pointer: its report descriptor says */
            w->cur_num = id->bInterfaceNumber;
        }
    } else if (desc[1] == HID_DT_HID && desc[0] >= 9 && w->cur_boot) {
        w->cur_rlen = (uint16_t)(desc[7] | desc[8] << 8);   /* the report descriptor's length */
    } else if (desc[1] == USB_DT_ENDPOINT && w->cur_boot) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)desc;
        if ((ep->bmAttributes & 3) != USB_EP_INTERRUPT || !(ep->bEndpointAddress & 0x80)) return 0;
        w->found++;
        if (w->hid && w->hid->count < MAX_IFACES) {
            usb_device_t* d = w->dev;
            hid_iface_t* h = &w->hid->ifs[w->hid->count];
            memset(h, 0, sizeof(*h));
            h->apple = d->desc.idVendor == 0x05AC;
            /* a mouse may really be a tablet: its descriptor tells (absolute X / Y) */
            int report = 0;
            int kind = w->cur_boot >= 2 ? pointer_setup(d, h, w->cur_num, w->cur_rlen) : -1;
            if (kind == 0) report = h->absolute || w->cur_boot == 3;   /* (relative boot mice stay on the boot protocol) */
            else if (kind == 1) report = 1;                            /* a game controller */
            if (!report && h->fields) { kfree(h->fields); h->fields = NULL; h->report_mode = 0; }
            if (w->cur_boot == 3 && !report) { w->cur_boot = 0; return 0; }   /* not a pointer */
            /* boot protocol (or the report protocol), and only report on change */
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, HID_SET_PROTOCOL, report ? 1 : 0, w->cur_num, NULL, 0);
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, HID_SET_IDLE, 0, w->cur_num, NULL, 0);
            if (usb_open_endpoint(d, ep) == 0) {
                w->hid->count++;
                h->is_kbd = (w->cur_boot == 1);
                h->ep = ep->bEndpointAddress;
                h->xfer.dev = d;
                h->xfer.ep = h->ep;
                h->xfer.buf = (uint8_t*)usb_dma_alloc(64);
                /* one packet per transfer: a report never spans packets (it is at
                 * most wMaxPacketSize), and asking for more holds a full-size
                 * report back until the next one fills the rest - a mouse with
                 * 4-byte packets (QEMU's, many real ones) then lags a report
                 * behind and drops every second one (seen on UHCI, which runs
                 * the transfer packet by packet). A boot keyboard needs its 8. */
                uint16_t mps = ep->wMaxPacketSize & 0x7FF;
                h->xfer.len = mps > 64 ? 64 : mps ? mps : 8;
                if (h->is_kbd && h->xfer.len < 8) h->xfer.len = 8;
                h->pad = h->is_pad ? gamepad_add(d->product) : -1;
                if (h->xfer.buf) usb_submit(&h->xfer);
            } else if (h->fields) { kfree(h->fields); h->fields = NULL; }
        }
        w->cur_boot = 0;
    }
    return 0;
}

static int hid_probe(usb_device_t* d) {
    walk_t w;
    memset(&w, 0, sizeof(w));
    usb_walk_config(d, 0, walk_cb, &w);
    return w.found ? 0 : -1;
}

static int hid_attach(usb_device_t* d) {
    hid_t* hid = (hid_t*)kzalloc(sizeof(hid_t));
    if (!hid) return -1;
    walk_t w;
    memset(&w, 0, sizeof(w));
    w.hid = hid;
    w.dev = d;
    usb_walk_config(d, d->active_config, walk_cb, &w);
    if (!hid->count) { kfree(hid); return -1; }
    d->drvdata = hid;
    for (int i = 0; i < hid->count; i++)
        klog("usbhid: %s on endpoint %02x\n", hid->ifs[i].is_kbd ? "keyboard" : hid->ifs[i].is_pad ? "game controller" :
             hid->ifs[i].report_mode ? (hid->ifs[i].absolute ? "tablet (absolute pointer)" : "mouse (report protocol)") : "mouse",
             hid->ifs[i].ep);
    return 0;
}

/* lsusb: each interface's role and what it has sent - which device moves
 * the pointer (an absolute one says where it last put it) */
void usbhid_describe(usb_device_t* d, char* out, int cap) {
    hid_t* hid = (hid_t*)d->drvdata;
    int n = 0;
    if (cap > 0) out[0] = 0;
    for (int i = 0; hid && i < hid->count && n < cap; i++) {
        hid_iface_t* h = &hid->ifs[i];
        const char* role = h->is_kbd ? "keyboard" : h->is_pad ? "game controller" :
                           h->report_mode ? (h->absolute ? "absolute pointer (tablet / touch)" : "mouse (report protocol)") : "mouse (boot protocol)";
        n += ksnprintf(out + n, (size_t)(cap - n), "%s    endpoint %02x: %s, %u reports", n ? "\n" : "", h->ep, role, h->reports);
        if (h->report_mode && h->absolute && n < cap)
            n += ksnprintf(out + n, (size_t)(cap - n), ", last at %d,%d of 65535", h->last_x, h->last_y);
    }
}

static void hid_poll(usb_device_t* d) {
    hid_t* hid = (hid_t*)d->drvdata;
    if (!hid) return;
    for (int i = 0; i < hid->count; i++) {
        hid_iface_t* h = &hid->ifs[i];
        if (h->is_kbd && h->repeat_usage && (int32_t)(timer_ms() - h->repeat_at) >= 0) {
            send_sc(g_usage_sc[h->repeat_usage], 0);       /* typematic repeat */
            h->repeat_at = timer_ms() + REPEAT_RATE_MS;
        }
        if (h->xfer.status == USB_XFER_PENDING || h->xfer.status == USB_XFER_GONE) continue;
        if (h->xfer.status == USB_XFER_OK) {
            h->reports++;
            if (h->is_kbd && h->xfer.actual >= 8) {
                keyboard_report(h, h->xfer.buf);
            } else if (h->is_pad) {
                pad_report(h, h->xfer.buf, (int)h->xfer.actual);
            } else if (h->report_mode) {
                pointer_report(h, h->xfer.buf, (int)h->xfer.actual);
            } else if (!h->is_kbd && h->xfer.actual >= 3) {
                int dx = (int8_t)h->xfer.buf[1], dy = (int8_t)h->xfer.buf[2];
                mouse_inject_src(mouse_src(&h->src), dx, -dy, h->xfer.buf[0] & 7);   /* USB: +y is down */
                if (h->xfer.actual >= 4 && h->xfer.buf[3])
                    mouse_inject_wheel(-(int8_t)h->xfer.buf[3]);   /* USB: + is away from the user */
            }
        }
        usb_submit(&h->xfer);
    }
}

static void hid_detach(usb_device_t* d) {
    hid_t* hid = (hid_t*)d->drvdata;
    if (!hid) return;
    /* release anything still held down */
    for (int i = 0; i < hid->count; i++) {
        if (hid->ifs[i].is_pad) gamepad_remove(hid->ifs[i].pad);
        if (hid->ifs[i].fields) { kfree(hid->ifs[i].fields); hid->ifs[i].fields = NULL; }
        if (hid->ifs[i].src) { mouse_source_free(hid->ifs[i].src); hid->ifs[i].src = 0; }
        if (!hid->ifs[i].is_kbd) continue;
        static const uint8_t none[8] = { 0 };
        keyboard_report(&hid->ifs[i], none);
    }
    d->drvdata = NULL;
}

const usb_driver_t hid_driver = {
    "usbhid", hid_probe, hid_attach, hid_poll, hid_detach,
};
