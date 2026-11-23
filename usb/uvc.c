/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbcore.h"
#include "uvc.h"
#include "uaudio.h"
#include "kstring.h"
#include "kheap.h"
#include "serial.h"
#include "terminal.h"

/*
 * Webcams: the USB Video Class (1.0 - 1.5).
 *
 * A camera has a VideoControl interface and a VideoStreaming one. The
 * streaming interface's setting 0 lists the formats - YUY2 (uncompressed,
 * 2 bytes a pixel) and MJPEG are the ones taken - each with its frame
 * sizes; its other settings each have an isochronous IN endpoint of some
 * bandwidth. Opening the camera: the biggest size up to 640x480 - YUY2
 * first, then MJPEG - is proposed ("probe"); the camera answers how many
 * bytes it sends per (micro)frame at most; the smallest setting with that
 * bandwidth is chosen, the choice committed, and the stream starts.
 *
 * Every service interval (125 us at high speed) brings one packet: a
 * header (its length, flags: the frame ID that toggles with each picture,
 * end of frame, error) and part of a picture. Dozens of packets are queued
 * at the controller; the timer interrupt (uvc_tick, every 1 ms) takes the
 * finished ones, appends what they brought to the picture being put
 * together and queues them again. A complete picture swaps with the
 * "ready" one (three buffers: being filled, ready, being read), so a
 * reader takes the newest one without stopping the stream: YUY2 made
 * 0x00RRGGBB, MJPEG as a JPEG file - the standard Huffman tables put in,
 * which MJPEG leaves out - that the app decodes (the kernel has no image
 * codecs: they run in isolated workers).
 *
 * xHCI only (isochronous transfers), USB 2 speeds (high-bandwidth
 * endpoints: up to 3 packets a microframe). A webcam's microphone is the
 * USB audio driver's (uaudio_bind).
 */

#define MAXF   32                         /* frame sizes kept */
#define MAXA   16                         /* streaming settings kept */
#define NV     96                         /* packets queued (12 ms at high speed) */
#define SLOT   4096                       /* bytes per packet buffer (3 x 1024 at most) */
#define PER64K (65536 / SLOT)
#define MAXW   640
#define MAXH   480

enum { KIND_NONE, KIND_YUY2, KIND_MJPEG };

typedef struct {
    uint8_t  kind, fmt, frame;
    uint16_t w, h;
    uint32_t interval, maxsize;
} vframe_t;

typedef struct {
    uint8_t       alt;
    usb_ep_desc_t ep;
    uint32_t      bw;                     /* bytes per service interval */
} valt_t;

typedef struct {
    int      vc_iface, vs_iface;
    uint16_t bcd;
    int      cur_vc, cur_vs, cur_alt, cur_kind;
    uint8_t  cur_fmt;
    vframe_t f[MAXF];
    int      nf;
    valt_t   a[MAXA];
    int      na;
    int      bulk;                        /* (a bulk-streaming camera: not supported) */
} uscan_t;

typedef struct {
    usb_device_t* dev;
    uscan_t       s;
    usb_xfer_t    x[NV];
    uint8_t*      chunk[(NV + PER64K - 1) / PER64K];
    uint32_t      head;
    uint32_t      td_len;
    int           cur;                    /* the frame size streaming (index in s.f), -1 */
    int           opened_alt;
    volatile int  on;
    int           users;
    int           gone;                   /* unplugged (its device slot may be reused) */
    /* pictures */
    uint8_t*      fb[3];
    uint32_t      fbcap;
    int           asm_i, ready_i, read_i;
    uint32_t      asm_len, ready_len;
    int           asm_bad, fid;
    volatile uint32_t seq;
    uint32_t      dropped, packets, errors;
    char          name[96];
} cam_t;

static cam_t* g_cam;

#ifndef UVC_HOST_TEST
static inline uintptr_t irq_off(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_on(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}
#else                                   /* (host tests: one thread, no interrupts) */
static inline uintptr_t irq_off(void) { return 0; }
static inline void irq_on(uintptr_t f) { (void)f; }
#endif

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ══ the descriptors ══════════════════════════════════════════════════ */

static int scan_cb(const uint8_t* d, void* ctx) {
    uscan_t* s = (uscan_t*)ctx;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        s->cur_vc = s->cur_vs = 0;
        if (id->bInterfaceClass != 0x0E) return 0;
        if (id->bInterfaceSubClass == 1 && s->vc_iface < 0) s->vc_iface = id->bInterfaceNumber;
        if (id->bInterfaceSubClass == 1) s->cur_vc = id->bInterfaceNumber == s->vc_iface;
        if (id->bInterfaceSubClass == 2) {
            if (s->vs_iface < 0) s->vs_iface = id->bInterfaceNumber;   /* (the first one) */
            s->cur_vs = id->bInterfaceNumber == s->vs_iface;
            s->cur_alt = id->bAlternateSetting;
            s->cur_kind = KIND_NONE;
        }
        return 0;
    }
    if (d[1] == 0x24 && s->cur_vc && d[0] >= 5 && d[2] == 0x01) {   /* VC_HEADER */
        s->bcd = le16(d + 3);
        return 0;
    }
    if (d[1] == 0x24 && s->cur_vs && s->cur_alt == 0 && d[0] >= 4) {
        switch (d[2]) {
        case 0x04:                                          /* VS_FORMAT_UNCOMPRESSED */
            s->cur_fmt = d[3];
            s->cur_kind = d[0] >= 21 && d[5] == 'Y' && d[6] == 'U' && d[7] == 'Y' && d[8] == '2' ? KIND_YUY2 : KIND_NONE;
            break;
        case 0x06:                                          /* VS_FORMAT_MJPEG */
            s->cur_fmt = d[3];
            s->cur_kind = KIND_MJPEG;
            break;
        case 0x05: case 0x07:                               /* VS_FRAME_UNCOMPRESSED / _MJPEG */
            if (d[0] < 26 || s->nf >= MAXF) break;
            if ((d[2] == 0x05 && s->cur_kind != KIND_YUY2) || (d[2] == 0x07 && s->cur_kind != KIND_MJPEG)) break;
            {
                vframe_t* f = &s->f[s->nf++];
                f->kind = (uint8_t)s->cur_kind;
                f->fmt = s->cur_fmt;
                f->frame = d[3];
                f->w = le16(d + 5);
                f->h = le16(d + 7);
                f->maxsize = le32(d + 17);
                f->interval = le32(d + 21);
            }
            break;
        default:
            break;
        }
        return 0;
    }
    if (d[1] == USB_DT_ENDPOINT && s->cur_vs && d[0] >= 7) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
        if (!(ep->bEndpointAddress & 0x80)) return 0;
        if ((ep->bmAttributes & 3) == USB_EP_BULK) { s->bulk = 1; return 0; }
        if ((ep->bmAttributes & 3) != USB_EP_ISOCH || s->cur_alt == 0 || s->na >= MAXA) return 0;
        valt_t* a = &s->a[s->na++];
        a->alt = (uint8_t)s->cur_alt;
        a->ep = *ep;
        a->bw = (uint32_t)(ep->wMaxPacketSize & 0x7FF) * (1u + ((ep->wMaxPacketSize >> 11) & 3));
    }
    return 0;
}

static void scan(usb_device_t* d, int cfg, uscan_t* s) {
    memset(s, 0, sizeof(*s));
    s->vc_iface = s->vs_iface = -1;
    usb_walk_config(d, cfg, scan_cb, s);
}

/* ══ pictures ═════════════════════════════════════════════════════════ */

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUY2 (Y0 U Y1 V for each two pixels, BT.601 studio range) to 0x00RRGGBB */
void uvc_yuy2_to_xrgb(const uint8_t* src, uint32_t* dst, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (uint32_t)y * (uint32_t)w * 2u;
        uint32_t* o = dst + (uint32_t)y * (uint32_t)w;
        for (int x = 0; x + 1 < w; x += 2, s += 4) {
            int d = s[1] - 128, e = s[3] - 128;
            int rv = 409 * e + 128, gv = -100 * d - 208 * e + 128, bv = 516 * d + 128;
            for (int k = 0; k < 2; k++) {
                int c = 298 * (s[k * 2] - 16);
                o[x + k] = (uint32_t)clamp8((c + rv) >> 8) << 16 | (uint32_t)clamp8((c + gv) >> 8) << 8 | clamp8((c + bv) >> 8);
            }
        }
        if (w & 1) o[w - 1] = w > 1 ? o[w - 2] : 0;
    }
}

/* The Huffman tables of the JPEG standard (its Annex K.3): an MJPEG
 * picture has none of its own, a JPEG decoder needs them. */
static const uint8_t g_dht[] = {
    0xFF, 0xC4, 0x01, 0xA2,
    0x00, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,                  /* DC luminance */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    0x01, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,                  /* DC chrominance */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    0x10, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D,               /* AC luminance */
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
    0x11, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,               /* AC chrominance */
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};

/* An MJPEG picture as a JPEG file: the tables put in after SOI if no
 * DHT segment comes before the scan. kmalloc'd, or NULL. */
uint8_t* uvc_mjpeg_fix(const uint8_t* src, uint32_t len, uint32_t* out_len) {
    if (len < 4 || src[0] != 0xFF || src[1] != 0xD8) return NULL;
    int has_dht = 0;
    for (uint32_t i = 2; i + 4 <= len;) {
        if (src[i] != 0xFF) break;
        uint8_t m = src[i + 1];
        if (m == 0xC4) { has_dht = 1; break; }
        if (m == 0xDA) break;                               /* the scan: no tables before it */
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        i += 2u + ((uint32_t)src[i + 2] << 8 | src[i + 3]);    /* (segment lengths are big endian) */
    }
    uint32_t n = len + (has_dht ? 0 : (uint32_t)sizeof(g_dht));
    uint8_t* o = (uint8_t*)kmalloc(n);
    if (!o) return NULL;
    if (has_dht) memcpy(o, src, len);
    else {
        o[0] = 0xFF; o[1] = 0xD8;
        memcpy(o + 2, g_dht, sizeof(g_dht));
        memcpy(o + 2 + sizeof(g_dht), src + 2, len - 2);
    }
    *out_len = n;
    return o;
}

/* the picture being put together is complete: it becomes the ready one */
static void finish(cam_t* c) {
    const vframe_t* f = c->cur >= 0 ? &c->s.f[c->cur] : NULL;
    int ok = f && !c->asm_bad && c->asm_len;
    if (ok && f->kind == KIND_YUY2) ok = c->asm_len == (uint32_t)f->w * f->h * 2u;
    if (ok && f->kind == KIND_MJPEG) ok = c->asm_len > 4 && c->fb[c->asm_i][0] == 0xFF && c->fb[c->asm_i][1] == 0xD8;
    if (ok) {
        int t = c->asm_i;
        c->asm_i = c->ready_i;
        c->ready_i = t;
        c->ready_len = c->asm_len;
        c->seq++;
    } else if (c->asm_len) c->dropped++;
    c->asm_len = 0;
    c->asm_bad = 0;
}

/* one packet: its header, then part of a picture */
static void payload(cam_t* c, const uint8_t* p, uint32_t n) {
    if (n < 2) return;                                      /* (empty packets are common) */
    uint32_t hl = p[0];
    if (hl < 2 || hl > n) { c->errors++; return; }
    uint8_t flags = p[1];
    int fid = flags & 1;
    if (c->fid >= 0 && fid != c->fid && c->asm_len) finish(c);   /* a new picture began */
    c->fid = fid;
    if (flags & 0x40) c->asm_bad = 1;                       /* the camera says this one is bad */
    uint32_t dl = n - hl;
    if (dl) {
        if (c->asm_len + dl > c->fbcap) c->asm_bad = 1;
        else { memcpy(c->fb[c->asm_i] + c->asm_len, p + hl, dl); c->asm_len += dl; }
    }
    if (flags & 0x02) finish(c);                            /* end of frame */
}

void uvc_tick(void) {
    cam_t* c = g_cam;
    if (!c || c->gone || !c->on || !c->dev->present) return;
    if (c->dev->hc->ops->events) c->dev->hc->ops->events(c->dev->hc);
    for (int n = 0; n < NV; n++) {
        usb_xfer_t* x = &c->x[c->head];
        if (x->status == USB_XFER_PENDING) break;
        if (x->status == USB_XFER_GONE) { c->on = 0; return; }
        if (x->status == USB_XFER_OK) { c->packets++; payload(c, x->buf, x->actual > c->td_len ? c->td_len : x->actual); }
        else c->errors++;
        x->len = c->td_len;
        x->actual = 0;
        usb_submit(x);
        c->head = (c->head + 1) % NV;
    }
}

/* ══ streaming ════════════════════════════════════════════════════════ */

static uint16_t probe_len(const cam_t* c) {
    return c->s.bcd >= 0x0150 ? 48 : c->s.bcd >= 0x0110 ? 34 : 26;
}

/* VS_PROBE_CONTROL (cs 1) / VS_COMMIT_CONTROL (cs 2) */
static int vs_set(cam_t* c, int cs, uint8_t* b) {
    return usb_control(c->dev, USB_TYPE_CLASS | USB_RECIP_IFACE, 0x01 /* SET_CUR */, (uint16_t)(cs << 8),
                       (uint16_t)c->s.vs_iface, b, probe_len(c));
}
static int vs_get(cam_t* c, int cs, uint8_t* b) {
    return usb_control(c->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_IFACE, 0x81 /* GET_CUR */, (uint16_t)(cs << 8),
                       (uint16_t)c->s.vs_iface, b, probe_len(c));
}

/* the sizes to try: YUY2 then MJPEG, each the biggest first, none over 640x480 */
static int candidates(const cam_t* c, int* out) {
    int n = 0;
    for (int kind = KIND_YUY2; kind <= KIND_MJPEG; kind++) {
        int start = n;
        for (int i = 0; i < c->s.nf; i++) {
            const vframe_t* f = &c->s.f[i];
            if (f->kind != kind || !f->w || !f->h || f->w > MAXW || f->h > MAXH) continue;
            int at = n++;
            while (at > start && (uint32_t)c->s.f[out[at - 1]].w * c->s.f[out[at - 1]].h < (uint32_t)f->w * f->h) {
                out[at] = out[at - 1];
                at--;
            }
            out[at] = i;
        }
    }
    return n;
}

/* probe, choose the setting, commit: the index of the setting in s.a, or -1 */
static int negotiate(cam_t* c, int fi) {
    const vframe_t* f = &c->s.f[fi];
    uint8_t b[48];
    memset(b, 0, sizeof(b));
    b[0] = 1;                                               /* bmHint: the frame interval is fixed */
    b[2] = f->fmt;
    b[3] = f->frame;
    put32(b + 4, f->interval);
    if (vs_set(c, 1, b) < 0) return -1;
    if (vs_get(c, 1, b) < 0) return -1;
    if (b[2] != f->fmt || b[3] != f->frame) return -1;      /* it wants another size */
    uint32_t payload_max = le32(b + 22);
    int best = -1;
    for (int i = 0; i < c->s.na; i++) {
        const valt_t* a = &c->s.a[i];
        if (a->bw > SLOT) continue;
        if (a->bw >= payload_max && (best < 0 || a->bw < c->s.a[best].bw)) best = i;
    }
    if (best < 0) return -1;
    if (vs_set(c, 2, b) < 0) return -1;
    return best;
}

static int alloc_pictures(cam_t* c, uint32_t need) {
    if (need <= c->fbcap) return 0;
    for (int i = 0; i < 3; i++) { if (c->fb[i]) kfree(c->fb[i]); c->fb[i] = NULL; }
    c->fbcap = 0;
    for (int i = 0; i < 3; i++) if (!(c->fb[i] = (uint8_t*)kmalloc(need))) return -1;
    c->fbcap = need;
    return 0;
}

/* until the controller gave back every packet (they finish each interval) */
static void drain(cam_t* c) {
    for (int t = 0; t < 200; t++) {
        int busy = 0;
        for (int k = 0; k < NV; k++) busy |= c->x[k].status == USB_XFER_PENDING;
        if (!busy) return;
        if (c->dev->hc->ops->events) c->dev->hc->ops->events(c->dev->hc);
        usb_delay_ms(1);
    }
}

static int start(cam_t* c) {
    int cand[MAXF];
    int n = candidates(c, cand), ai = -1, fi = -1;
    for (int i = 0; i < n && i < 10 && ai < 0; i++) {
        ai = negotiate(c, cand[i]);
        if (ai >= 0) fi = cand[i];
    }
    if (ai < 0) { klog("uvc: %s: no size it can stream\n", c->name); return -1; }
    const vframe_t* f = &c->s.f[fi];
    const valt_t* a = &c->s.a[ai];
    uint32_t need = f->kind == KIND_YUY2 ? (uint32_t)f->w * f->h * 2u : f->maxsize;
    if (f->kind == KIND_MJPEG && (need < 65536 || need > (uint32_t)f->w * f->h * 3u)) need = (uint32_t)f->w * f->h * 2u;
    if (alloc_pictures(c, need) != 0) { klog("uvc: no memory for its pictures\n"); return -1; }
    usb_control(c->dev, USB_RECIP_IFACE, 0x0B /* SET_INTERFACE */, a->alt, (uint16_t)c->s.vs_iface, NULL, 0);
    if (c->opened_alt != a->alt) {
        if (usb_open_endpoint(c->dev, &a->ep) != 0) { klog("uvc: the stream endpoint could not be opened\n"); return -1; }
        c->opened_alt = a->alt;
    }
    drain(c);
    uintptr_t fl = irq_off();
    c->cur = fi;
    c->td_len = a->bw;
    c->asm_i = 0; c->ready_i = 1; c->read_i = 2;
    c->asm_len = c->ready_len = 0;
    c->asm_bad = 0;
    c->fid = -1;
    c->head = 0;
    irq_on(fl);
    for (int k = 0; k < NV; k++) {
        if (c->x[k].status == USB_XFER_PENDING) return -1;
        c->x[k].ep = a->ep.bEndpointAddress;
        c->x[k].len = c->td_len;
        c->x[k].actual = 0;
        usb_submit(&c->x[k]);
    }
    c->on = 1;
    klog("uvc: %s: %ux%u %s, setting %u (%u bytes per interval)\n", c->name, f->w, f->h,
         f->kind == KIND_YUY2 ? "YUY2" : "MJPEG", a->alt, a->bw);
    return 0;
}

static void stop(cam_t* c) {
    c->on = 0;
    if (c->dev->present) {
        usb_control(c->dev, USB_RECIP_IFACE, 0x0B, 0, (uint16_t)c->s.vs_iface, NULL, 0);   /* setting 0: no bandwidth */
        drain(c);
    }
}

/* ══ the API (uvc.h) ══════════════════════════════════════════════════ */

int camera_available(void) { return g_cam && !g_cam->gone && g_cam->dev->present; }
const char* camera_name(void) { return camera_available() ? g_cam->name : "none"; }

int camera_open(int* w, int* h) {
    cam_t* c = g_cam;
    if (!camera_available()) return -1;
    if (c->users++ == 0 && start(c) != 0) { c->users--; return -1; }
    if (w) *w = c->s.f[c->cur].w;
    if (h) *h = c->s.f[c->cur].h;
    return 0;
}

void camera_close(void) {
    cam_t* c = g_cam;
    if (!c || c->users <= 0) return;
    if (--c->users == 0) stop(c);
}

int camera_frame(void* out, uint32_t cap, camera_frame_t* info, uint32_t* seq) {
    cam_t* c = g_cam;
    if (!c || c->gone || !c->on || c->cur < 0) return -1;
    const vframe_t* f = &c->s.f[c->cur];
    uint32_t got_seq, len;
    uintptr_t fl = irq_off();
    got_seq = c->seq;
    if (!got_seq || (seq && *seq == got_seq)) { irq_on(fl); return 0; }
    int t = c->read_i;                                      /* the ready picture is ours now */
    c->read_i = c->ready_i;
    c->ready_i = t;
    len = c->ready_len;
    irq_on(fl);
    if (seq) *seq = got_seq;
    const uint8_t* p = c->fb[c->read_i];
    camera_frame_t fi = { f->w, f->h, CAMERA_XRGB, got_seq, 0 };
    if (f->kind == KIND_YUY2) {
        fi.bytes = (uint32_t)f->w * f->h * 4u;
        if (cap < fi.bytes) return -1;
        uvc_yuy2_to_xrgb(p, (uint32_t*)out, f->w, f->h);
    } else {
        /* MJPEG: a JPEG file for the app to decode (no codec in the kernel) */
        uint32_t jl = 0;
        uint8_t* jpg = uvc_mjpeg_fix(p, len, &jl);
        if (!jpg) { c->errors++; return -1; }
        if (cap < jl) { kfree(jpg); return -1; }
        memcpy(out, jpg, jl);
        kfree(jpg);
        fi.format = CAMERA_JPEG;
        fi.bytes = jl;
    }
    if (info) *info = fi;
    return 1;
}

uint32_t camera_frame_bytes(void) {
    cam_t* c = g_cam;
    if (!c || c->cur < 0) return 0;
    const vframe_t* f = &c->s.f[c->cur];
    uint32_t xrgb = (uint32_t)f->w * f->h * 4u;
    return f->kind == KIND_YUY2 ? xrgb : c->fbcap + (uint32_t)sizeof(g_dht);
}

void camera_list(void) {
    char line[160];
    cam_t* c = g_cam;
    if (!camera_available()) {
        terminal_writeln("camera: none (a USB webcam: USB Video Class, on an xHCI port)");
        return;
    }
    ksnprintf(line, sizeof(line), "camera: %s (UVC %x.%02x)", c->name, c->s.bcd >> 8, c->s.bcd & 0xFF);
    terminal_writeln(line);
    for (int i = 0; i < c->s.nf; i++) {
        const vframe_t* f = &c->s.f[i];
        uint32_t fps10 = f->interval ? 100000000u / f->interval : 0;
        ksnprintf(line, sizeof(line), "  %-5s %4ux%-4u %u.%u fps%s", f->kind == KIND_YUY2 ? "YUY2" : "MJPEG",
                  f->w, f->h, fps10 / 10, fps10 % 10, i == c->cur && c->on ? "  (streaming)" : "");
        terminal_writeln(line);
    }
    ksnprintf(line, sizeof(line), "  %s, %u pictures, %u dropped, %u packets, %u errors",
              c->on ? "on" : "off", c->seq, c->dropped, c->packets, c->errors);
    terminal_writeln(line);
}

/* ══ the device ═══════════════════════════════════════════════════════ */

static int uvc_probe(usb_device_t* d) {
    uscan_t s;
    scan(d, 0, &s);
    return s.vc_iface >= 0 && s.vs_iface >= 0 ? 0 : -1;
}

static int uvc_attach(usb_device_t* d) {
    if (!d->hc->ops->events) { klog("uvc: a webcam on a USB controller without isochronous transfers (xHCI only)\n"); return -1; }
    if (camera_available()) { klog("uvc: one camera at a time\n"); return -1; }
    cam_t* c = (cam_t*)kzalloc(sizeof(cam_t));
    if (!c) return -1;
    c->dev = d;
    scan(d, d->active_config, &c->s);
    c->cur = -1;
    c->opened_alt = -1;
    ksnprintf(c->name, sizeof(c->name), "%s", d->product[0] ? d->product : "USB camera");
    int n = 0, cand[MAXF];
    n = candidates(c, cand);
    if (!n || !c->s.na) {
        klog("uvc: %s: %s\n", c->name, c->s.bulk && !c->s.na ? "streams by bulk transfers (not supported)"
                                                              : "no YUY2 or MJPEG size up to 640x480");
        kfree(c);
        return -1;
    }
    for (int k = 0; k < NV; k++) {
        int ch = k / PER64K;
        if (!c->chunk[ch] && !(c->chunk[ch] = (uint8_t*)usb_dma_alloc(65536))) { kfree(c); return -1; }
        c->x[k].dev = d;
        c->x[k].buf = c->chunk[ch] + (k % PER64K) * SLOT;
    }
    usb_control(d, USB_RECIP_IFACE, 0x0B, 0, (uint16_t)c->s.vs_iface, NULL, 0);   /* not streaming yet */
    g_cam = c;
    klog("uvc: %s: %d sizes, %d streaming settings\n", c->name, c->s.nf, c->s.na);
    uaudio_bind(d);                                         /* its microphone */
    return 0;
}

static void uvc_poll(usb_device_t* d) { (void)d; }          /* (the timer: uvc_tick) */

static void uvc_detach(usb_device_t* d) {
    uaudio_unbind(d);
    if (g_cam && g_cam->dev == d) { g_cam->on = 0; g_cam->gone = 1; }   /* (kept: packets may still be queued) */
}

const usb_driver_t uvc_driver = {
    "uvc", uvc_probe, uvc_attach, uvc_poll, uvc_detach,
};
