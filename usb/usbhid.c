#include "usbcore.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"
#include "keyboard.h"
#include "usb.h"

/*
 * USB HID boot-protocol keyboards and mice.
 *
 * Once Banana OS drives a USB controller itself, the BIOS stops faking
 * USB keyboards/mice as PS/2 devices - so this driver takes over: key
 * reports become set-1 scancodes fed through the PS/2 decoder
 * (keyboard_feed_scancode), mouse reports become mouse_inject() motion.
 * Boot protocol only (every keyboard and mouse supports it).
 */

#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B

#define MAX_IFACES 2
#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS  33

/* HID usage (0x04..0x65) -> set-1 scancode; 0xE0xx = extended */
static const uint16_t g_usage_sc[0x66] = {
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
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56,
};

/* modifier bits (report byte 0) -> scancode */
static const uint16_t g_mod_sc[8] = { 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C };

typedef struct {
    int        is_kbd;
    uint8_t    ep;
    usb_xfer_t xfer;
    uint8_t    prev[8];          /* keyboard: previous report */
    uint8_t    repeat_usage;
    uint32_t   repeat_at;
} hid_iface_t;

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
        if (changed & (1u << b)) send_sc(g_mod_sc[b], !(r[0] & (1u << b)));
    for (int i = 2; i < 8; i++) {              /* released */
        uint8_t u = h->prev[i];
        if (u >= 4 && u < 0x66 && !in_report(r, u) && g_usage_sc[u]) send_sc(g_usage_sc[u], 1);
    }
    for (int i = 2; i < 8; i++) {              /* newly pressed */
        uint8_t u = r[i];
        if (u >= 4 && u < 0x66 && !in_report(h->prev, u) && g_usage_sc[u]) {
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
    int     cur_boot;     /* current interface: 1 kbd, 2 mouse, 0 other */
    uint8_t cur_num;
    usb_device_t* dev;
} walk_t;

static int walk_cb(const uint8_t* desc, void* ctx) {
    walk_t* w = (walk_t*)ctx;
    if (desc[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)desc;
        w->cur_boot = 0;
        if (id->bAlternateSetting == 0 && id->bInterfaceClass == 3 && id->bInterfaceSubClass == 1 &&
            (id->bInterfaceProtocol == 1 || id->bInterfaceProtocol == 2)) {
            w->cur_boot = id->bInterfaceProtocol;
            w->cur_num = id->bInterfaceNumber;
        }
    } else if (desc[1] == USB_DT_ENDPOINT && w->cur_boot) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)desc;
        if ((ep->bmAttributes & 3) != USB_EP_INTERRUPT || !(ep->bEndpointAddress & 0x80)) return 0;
        w->found++;
        if (w->hid && w->hid->count < MAX_IFACES) {
            usb_device_t* d = w->dev;
            /* boot protocol, and only report on change */
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, HID_SET_PROTOCOL, 0, w->cur_num, NULL, 0);
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, HID_SET_IDLE, 0, w->cur_num, NULL, 0);
            if (usb_open_endpoint(d, ep) == 0) {
                hid_iface_t* h = &w->hid->ifs[w->hid->count++];
                h->is_kbd = (w->cur_boot == 1);
                h->ep = ep->bEndpointAddress;
                h->xfer.dev = d;
                h->xfer.ep = h->ep;
                h->xfer.buf = (uint8_t*)usb_dma_alloc(64);
                h->xfer.len = 8;
                if (h->xfer.buf) usb_submit(&h->xfer);
            }
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
        klog("usbhid: %s on endpoint %02x\n", hid->ifs[i].is_kbd ? "keyboard" : "mouse", hid->ifs[i].ep);
    return 0;
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
            if (h->is_kbd && h->xfer.actual >= 8) {
                keyboard_report(h, h->xfer.buf);
            } else if (!h->is_kbd && h->xfer.actual >= 3) {
                int dx = (int8_t)h->xfer.buf[1], dy = (int8_t)h->xfer.buf[2];
                mouse_inject(dx, -dy, h->xfer.buf[0] & 7);   /* USB: +y is down */
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
        if (!hid->ifs[i].is_kbd) continue;
        static const uint8_t none[8] = { 0 };
        keyboard_report(&hid->ifs[i], none);
    }
    d->drvdata = NULL;
}

const usb_driver_t hid_driver = {
    "usbhid", hid_probe, hid_attach, hid_poll, hid_detach,
};
