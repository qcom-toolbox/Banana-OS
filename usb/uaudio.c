/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbcore.h"
#include "uaudio.h"
#include "kstring.h"
#include "kheap.h"
#include "serial.h"
#include "audio.h"
#include "audioin.h"

/*
 * USB audio (USB Audio Class 1.0): headsets, USB speakers, USB sound
 * cards and DACs, USB microphones - full-speed ones, the class every
 * system drives without a driver of its own. While one is plugged in,
 * sound goes to it, and recording comes from it.
 *
 * Playing: the streaming interface's alternate setting with 16-bit stereo
 * PCM at 48 kHz is chosen; then every service interval (1 ms) one
 * isochronous packet of the ring's next bytes goes out. A few dozen
 * packets are queued at the controller; as they finish (looked at from
 * the timer interrupt, like the PCI cards' rings: audio.c's tick) they
 * are refilled from the ring and queued again.
 *
 * Recording: an input streaming interface with 16-bit PCM, mono or
 * stereo, at 48 kHz - or the nearest rate it has (webcams' microphones
 * often have 16 kHz only). Isochronous packets come in the same way; what
 * each brought is made 48 kHz stereo (linear interpolation) and written
 * into audioin.c's ring.
 *
 * The feature units are put to 0 dB, unmuted. xHCI controllers only
 * (isochronous transfers). usb/uvc.c binds the microphone of a webcam
 * (uaudio_bind): one driver per device.
 */

#define NPKT     32                       /* packets queued (~32 ms at 1 ms each) */
#define SLOT     256                      /* bytes per packet buffer (never crosses 64 KiB) */
#define NIN      32                       /* recording: packets queued */
#define SLOT_IN  1024
#define RATE     48000
#define MAX_FU   6

typedef struct {
    int           ok, in_ok;
    uint8_t       ac_iface;
    /* playing */
    uint8_t       as_iface, alt;
    usb_ep_desc_t ep;
    int           freq_ctl;
    /* recording */
    uint8_t       in_iface, in_alt, in_ch;
    usb_ep_desc_t in_ep;
    uint32_t      in_rate;
    int           in_freq_ctl;
    /* the interface / setting being looked at */
    int           cur_as, cur_alt, cur_num, cur_ac, cur_ch, cur_is_in;
    uint32_t      cur_rate;
    int           last_ep;                /* 1: out, 2: in (whose EP_GENERAL follows) */
    uint8_t       fu[MAX_FU];             /* feature units */
    int           nfu;
} scan_t;

typedef struct {
    usb_device_t* dev;
    usb_xfer_t    x[NPKT];
    uint8_t*      bufs;
    uint32_t      bytes;                  /* per packet */
    uint32_t      head;                   /* the oldest packet queued */
    uint64_t      played, sent;           /* ring bytes */
    volatile int  on;
    char          name[96];
} ua_t;

typedef struct {
    usb_device_t* dev;
    usb_xfer_t    x[NIN];
    uint8_t*      bufs;
    uint32_t      head;
    uint32_t      len;                    /* each packet's buffer */
    uint8_t       iface, alt, ch, freq_ctl;
    usb_ep_desc_t ep;
    uint32_t      rate;
    uint32_t      phase;                  /* where the next output frame falls, in 1/48000 of an input frame */
    int16_t       prev_l, prev_r;
    uint64_t      wpos;                   /* ring bytes written */
    volatile int  on, opened;
    char          name[96];
} uain_t;

static ua_t*   g_ua;                      /* the one playing */
static uain_t* g_uin;                     /* the one recording */

static uint32_t rate3(const uint8_t* f) { return (uint32_t)(f[0] | f[1] << 8 | f[2] << 16); }

/* the rate to ask for: 48 kHz if the format has it; else the highest
 * under it (or the lowest over it); 0 if none */
static uint32_t pick_rate(const uint8_t* d) {
    int n = d[7];
    if (n == 0) {                         /* a continuous range */
        if (d[0] < 14) return 0;
        uint32_t lo = rate3(d + 8), hi = rate3(d + 11);
        return lo <= RATE && RATE <= hi ? RATE : hi < RATE ? hi : lo;
    }
    uint32_t below = 0, above = 0;
    for (int i = 0; i < n && 8 + i * 3 + 2 < d[0]; i++) {
        uint32_t r = rate3(d + 8 + i * 3);
        if (r == RATE) return RATE;
        if (r < RATE && r > below) below = r;
        if (r > RATE && (!above || r < above)) above = r;
    }
    return below ? below : above;
}

static int scan_cb(const uint8_t* d, void* ctx) {
    scan_t* s = (scan_t*)ctx;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        s->cur_as = s->cur_ac = 0;
        s->cur_ch = 0;
        s->last_ep = 0;
        if (id->bInterfaceClass != 1 || id->bInterfaceProtocol != 0) return 0;   /* (UAC2: protocol 0x20) */
        if (id->bInterfaceSubClass == 1) { s->cur_ac = 1; s->ac_iface = id->bInterfaceNumber; }
        if (id->bInterfaceSubClass == 2 && id->bAlternateSetting != 0) {
            s->cur_as = 1;
            s->cur_alt = id->bAlternateSetting;
            s->cur_num = id->bInterfaceNumber;
        }
    } else if (d[1] == 0x24 && s->cur_ac && d[0] >= 6 && d[2] == 0x06 && s->nfu < MAX_FU) {
        s->fu[s->nfu++] = d[3];                             /* FEATURE_UNIT */
    } else if (d[1] == 0x24 && s->cur_as && d[0] >= 8 && d[2] == 0x02) {
        /* FORMAT_TYPE I, 2-byte subframes, 16 bits: the channels and the rate */
        s->cur_ch = 0;
        if (d[3] == 1 && (d[4] == 1 || d[4] == 2) && d[5] == 2 && d[6] == 16) {
            s->cur_ch = d[4];
            s->cur_rate = pick_rate(d);
            if (!s->cur_rate) s->cur_ch = 0;
        }
    } else if (d[1] == USB_DT_ENDPOINT && s->cur_as && s->cur_ch) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
        if ((ep->bmAttributes & 3) != USB_EP_ISOCH) return 0;
        if (ep->bEndpointAddress & 0x80) {                  /* a microphone */
            /* the best setting: 48 kHz over another rate, stereo over mono */
            int better = !s->in_ok || (s->cur_rate == RATE && s->in_rate != RATE) ||
                         (s->cur_rate == s->in_rate && s->cur_ch > s->in_ch);
            if (better) {
                s->in_ep = *ep;
                s->in_alt = (uint8_t)s->cur_alt;
                s->in_iface = (uint8_t)s->cur_num;
                s->in_ch = (uint8_t)s->cur_ch;
                s->in_rate = s->cur_rate;
                s->in_freq_ctl = 0;
                s->in_ok = 1;
                s->last_ep = 2;
            }
        } else if (!s->ok && s->cur_ch == 2 && s->cur_rate == RATE) {   /* speakers: 48 kHz stereo */
            s->ep = *ep;
            s->alt = (uint8_t)s->cur_alt;
            s->as_iface = (uint8_t)s->cur_num;
            s->ok = 1;
            s->last_ep = 1;
        }
    } else if (d[1] == 0x25 && d[0] >= 3 && d[2] == 0x01) {
        /* EP_GENERAL: sampling frequency control, for the endpoint just taken */
        if (s->last_ep == 1) s->freq_ctl = d[3] & 1;
        if (s->last_ep == 2) s->in_freq_ctl = d[3] & 1;
        s->last_ep = 0;
    }
    return 0;
}

static int ua_probe(usb_device_t* d) {
    scan_t s;
    memset(&s, 0, sizeof(s));
    usb_walk_config(d, 0, scan_cb, &s);
    return s.ok || s.in_ok ? 0 : -1;
}

static void set_rate(usb_device_t* d, uint8_t ep, uint32_t rate) {
    uint8_t r[3] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16) };
    usb_control(d, USB_TYPE_CLASS | 0x02 /* endpoint */, 0x01 /* SET_CUR */, 0x0100, ep, r, 3);
}

/* ══ playing ══════════════════════════════════════════════════════════ */

/* packet k: the ring's next bytes */
static void fill(ua_t* u, int k) {
    const uint8_t* ring = audio_ring();
    uint32_t rb = audio_ring_bytes();
    uint32_t at = (uint32_t)(u->sent % rb);
    uint8_t* b = u->bufs + (uint32_t)k * SLOT;
    uint32_t first = rb - at < u->bytes ? rb - at : u->bytes;
    memcpy(b, ring + at, first);
    if (first < u->bytes) memcpy(b + first, ring, u->bytes - first);
    u->sent += u->bytes;
    u->x[k].len = u->bytes;
    usb_submit(&u->x[k]);
}

static int card_start(void) {
    ua_t* u = g_ua;
    if (!u) return -1;
    u->played = u->sent = 0;
    u->head = 0;
    for (int k = 0; k < NPKT; k++) fill(u, k);
    u->on = 1;
    return 0;
}

static uint32_t card_position(void) {
    ua_t* u = g_ua;
    return u ? (uint32_t)(u->played % audio_ring_bytes()) : 0;
}

/* from the timer interrupt: finished packets go out again with new sound */
static void card_tick(void) {
    ua_t* u = g_ua;
    if (!u || !u->on || !u->dev->present) return;
    if (u->dev->hc->ops->events) u->dev->hc->ops->events(u->dev->hc);
    for (int n = 0; n < NPKT; n++) {
        usb_xfer_t* x = &u->x[u->head];
        if (x->status == USB_XFER_PENDING) break;
        if (x->status == USB_XFER_GONE) { u->on = 0; return; }
        u->played += u->bytes;
        fill(u, (int)u->head);
        u->head = (u->head + 1) % NPKT;
    }
}

static void card_stop(void) {
    if (g_ua) g_ua->on = 0;                 /* (the queued packets finish on their own) */
}

static const audio_card_t g_card = { "USB audio", card_start, card_position, card_tick, card_stop };

static int attach_out(usb_device_t* d, const scan_t* s) {
    if (g_ua) return -1;                                   /* one at a time */
    /* the packet: one service interval of sound */
    uint32_t b = s->ep.bInterval ? s->ep.bInterval : 1;
    uint32_t period_us = (d->speed == USB_SPEED_HIGH ? 125u : 1000u) << (b - 1 > 10 ? 10 : b - 1);
    uint32_t bytes = (uint32_t)((uint64_t)RATE * 4 * period_us / 1000000u);
    uint16_t mps = s->ep.wMaxPacketSize & 0x7FF;
    if (bytes == 0 || bytes % 4 || bytes > mps || bytes > SLOT) {
        klog("uaudio: unusable packet size (%u bytes every %u us, max %u)\n", bytes, period_us, mps);
        return -1;
    }
    ua_t* u = (ua_t*)kzalloc(sizeof(ua_t));
    if (!u) return -1;
    u->dev = d;
    u->bytes = bytes;
    u->bufs = (uint8_t*)usb_dma_alloc(NPKT * SLOT);
    if (!u->bufs) { kfree(u); return -1; }
    /* the streaming setting, 48 kHz */
    usb_control(d, USB_RECIP_IFACE, 0x0B /* SET_INTERFACE */, s->alt, s->as_iface, NULL, 0);
    set_rate(d, s->ep.bEndpointAddress, RATE);
    if (usb_open_endpoint(d, &s->ep) != 0) { klog("uaudio: the stream endpoint could not be opened\n"); kfree(u); return -1; }
    for (int k = 0; k < NPKT; k++) {
        u->x[k].dev = d;
        u->x[k].ep = s->ep.bEndpointAddress;
        u->x[k].buf = u->bufs + k * SLOT;
    }
    ksnprintf(u->name, sizeof(u->name), "USB audio: %s", d->product[0] ? d->product : "USB sound device");
    g_ua = u;
    klog("uaudio: %s, 48 kHz 16-bit stereo, %u-byte packets every %u us\n", u->name, bytes, period_us);
    audio_card_plug(&g_card, u->name);
    return 0;
}

/* ══ recording ════════════════════════════════════════════════════════ */

/* one input frame into the ring, made 48 kHz: as many output frames as fall before it */
static void put_frame(uain_t* u, int16_t l, int16_t r) {
    uint8_t* ring = audio_in_ring();
    uint32_t rb = audio_in_ring_bytes();
    /* exact: each output frame moves rate/48000 of an input frame on */
    while (u->phase < RATE) {
        int ol = u->prev_l + (int)((int64_t)(l - u->prev_l) * u->phase / RATE);
        int or_ = u->prev_r + (int)((int64_t)(r - u->prev_r) * u->phase / RATE);
        uint32_t k = (uint32_t)(u->wpos % rb);
        ring[k] = (uint8_t)ol; ring[k + 1] = (uint8_t)((uint16_t)ol >> 8);
        ring[k + 2] = (uint8_t)or_; ring[k + 3] = (uint8_t)((uint16_t)or_ >> 8);
        u->wpos += 4;
        u->phase += u->rate;
    }
    u->phase -= RATE;
    u->prev_l = l;
    u->prev_r = r;
}

/* what packet x brought */
static void take(uain_t* u, const usb_xfer_t* x) {
    uint32_t fsz = (uint32_t)u->ch * 2u;
    uint32_t n = (x->actual > u->len ? u->len : x->actual) / fsz;
    const uint8_t* p = x->buf;
    for (uint32_t i = 0; i < n; i++, p += fsz) {
        int16_t l = (int16_t)(p[0] | p[1] << 8);
        int16_t r = u->ch == 2 ? (int16_t)(p[2] | p[3] << 8) : l;
        put_frame(u, l, r);
    }
}

static void in_tick(void) {
    uain_t* u = g_uin;
    if (!u || !u->on || !u->dev->present) return;
    if (u->dev->hc->ops->events) u->dev->hc->ops->events(u->dev->hc);
    for (int n = 0; n < NIN; n++) {
        usb_xfer_t* x = &u->x[u->head];
        if (x->status == USB_XFER_PENDING) break;
        if (x->status == USB_XFER_GONE) { u->on = 0; return; }
        if (x->status == USB_XFER_OK) take(u, x);
        x->len = u->len;
        x->actual = 0;
        usb_submit(x);
        u->head = (u->head + 1) % NIN;
    }
}

static uint32_t in_position(void) {
    uain_t* u = g_uin;
    return u ? (uint32_t)(u->wpos % audio_in_ring_bytes()) : 0;
}

static int in_start(void) {
    uain_t* u = g_uin;
    if (!u || !u->dev->present || !audio_in_ring()) return -1;
    if (!u->opened) {
        usb_control(u->dev, USB_RECIP_IFACE, 0x0B /* SET_INTERFACE */, u->alt, u->iface, NULL, 0);
        if (u->freq_ctl) set_rate(u->dev, u->ep.bEndpointAddress, u->rate);
        if (usb_open_endpoint(u->dev, &u->ep) != 0) { klog("uaudio: the microphone's endpoint could not be opened\n"); return -1; }
        u->opened = 1;
    }
    /* packets still queued from the last recording finish first (they
     * are never queued twice) */
    for (int t = 0; t < 100; t++) {
        int busy = 0;
        for (int k = 0; k < NIN; k++) busy |= u->x[k].status == USB_XFER_PENDING;
        if (!busy) break;
        if (u->dev->hc->ops->events) u->dev->hc->ops->events(u->dev->hc);
        usb_delay_ms(1);
    }
    u->head = 0;
    u->phase = 0;
    u->prev_l = u->prev_r = 0;
    for (int k = 0; k < NIN; k++) {
        if (u->x[k].status == USB_XFER_PENDING) return -1;   /* (the controller never gave it back) */
        u->x[k].len = u->len;
        u->x[k].actual = 0;
        usb_submit(&u->x[k]);
    }
    u->on = 1;
    return 0;
}

static void in_stop(void) {
    if (g_uin) g_uin->on = 0;               /* (the queued packets finish on their own) */
}

static const audio_in_t g_in_src = { "USB microphone", in_start, in_position, in_tick, in_stop };

static int attach_in(usb_device_t* d, const scan_t* s) {
    if (g_uin) return -1;
    uint32_t mps = (uint32_t)(s->in_ep.wMaxPacketSize & 0x7FF) * (1u + ((s->in_ep.wMaxPacketSize >> 11) & 3));
    if (!mps || mps > SLOT_IN) { klog("uaudio: microphone packets of %u bytes: unusable\n", mps); return -1; }
    uain_t* u = (uain_t*)kzalloc(sizeof(uain_t));
    if (!u) return -1;
    u->bufs = (uint8_t*)usb_dma_alloc(NIN * SLOT_IN);
    if (!u->bufs) { kfree(u); return -1; }
    u->dev = d;
    u->len = mps;
    u->iface = s->in_iface;
    u->alt = s->in_alt;
    u->ch = s->in_ch;
    u->ep = s->in_ep;
    u->rate = s->in_rate;
    u->freq_ctl = (uint8_t)s->in_freq_ctl;
    if (u->rate < 1000 || u->rate > 192000) { kfree(u->bufs); kfree(u); return -1; }
    for (int k = 0; k < NIN; k++) {
        u->x[k].dev = d;
        u->x[k].ep = s->in_ep.bEndpointAddress;
        u->x[k].buf = u->bufs + k * SLOT_IN;
    }
    ksnprintf(u->name, sizeof(u->name), "USB microphone: %s", d->product[0] ? d->product : "USB audio device");
    g_uin = u;
    klog("uaudio: %s, %u Hz 16-bit %s, %u-byte packets\n", u->name, u->rate, u->ch == 2 ? "stereo" : "mono", mps);
    audio_in_plug(&g_in_src, u->name);
    return 0;
}

/* ══ the device ═══════════════════════════════════════════════════════ */

int uaudio_bind(usb_device_t* d) {
    if (!d->hc->ops->events) return -1;                    /* xHCI only */
    scan_t s;
    memset(&s, 0, sizeof(s));
    usb_walk_config(d, d->active_config, scan_cb, &s);
    if (!s.ok && !s.in_ok) return -1;
    /* every feature unit: 0 dB on every channel, unmuted */
    for (int i = 0; i < s.nfu; i++) {
        uint8_t unmute = 0, vol[2] = { 0, 0 };
        uint16_t where = (uint16_t)(s.fu[i] << 8 | s.ac_iface);
        usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, 0x01, 0x0100, where, &unmute, 1);   /* mute, master */
        for (int ch = 0; ch <= 2; ch++)
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, 0x01, (uint16_t)(0x0200 | ch), where, vol, 2);
    }
    int bound = 0;
    if (s.ok && attach_out(d, &s) == 0) bound = 1;
    if (s.in_ok && attach_in(d, &s) == 0) bound = 1;
    return bound ? 0 : -1;
}

void uaudio_unbind(usb_device_t* d) {
    if (g_ua && g_ua->dev == d) {
        g_ua->on = 0;
        audio_card_unplug(&g_card);
        g_ua = NULL;                        /* (its buffers stay: packets may still be queued) */
    }
    if (g_uin && g_uin->dev == d) {
        g_uin->on = 0;
        audio_in_unplug(&g_in_src);
        g_uin = NULL;
    }
}

static int ua_attach(usb_device_t* d) { return uaudio_bind(d); }

static void ua_poll(usb_device_t* d) { (void)d; }          /* (fed from the timer: the ticks) */

static void ua_detach(usb_device_t* d) { uaudio_unbind(d); }

const usb_driver_t uaudio_driver = {
    "uaudio", ua_probe, ua_attach, ua_poll, ua_detach,
};
