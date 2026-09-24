#include "usbcore.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * USB mass storage (Bulk-Only Transport, SCSI): identifies the drive and
 * its capacity for `lsusb`. Banana OS has no disk filesystem to mount it
 * with yet - this is information only (and it exercises bulk IN/OUT on
 * every host controller).
 */

#define BOT_CBW_SIG 0x43425355u
#define BOT_CSW_SIG 0x53425355u

typedef struct {
    uint8_t  ep_in, ep_out;
    uint8_t* buf;
    char     info[64];
} msc_t;

typedef struct {
    usb_ep_desc_t in, out;
    int in_msc, have_in, have_out;
} msc_eps_t;

static int ep_cb(const uint8_t* d, void* ctx) {
    msc_eps_t* m = (msc_eps_t*)ctx;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        m->in_msc = id->bInterfaceClass == 0x08 && id->bInterfaceSubClass == 0x06 &&
                    id->bInterfaceProtocol == 0x50 && id->bAlternateSetting == 0;
    } else if (d[1] == USB_DT_ENDPOINT && m->in_msc) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
        if ((ep->bmAttributes & 3) != USB_EP_BULK) return 0;
        if ((ep->bEndpointAddress & 0x80) && !m->have_in) { m->in = *ep; m->have_in = 1; }
        if (!(ep->bEndpointAddress & 0x80) && !m->have_out) { m->out = *ep; m->have_out = 1; }
    }
    return 0;
}

static int bulk(usb_device_t* d, uint8_t ep, uint8_t* buf, uint32_t len) {
    usb_xfer_t x;
    memset(&x, 0, sizeof(x));
    x.dev = d;
    x.ep = ep;
    x.buf = buf;
    x.len = len;
    if (usb_submit(&x) != 0) return -1;
    uint32_t start = timer_ms();
    while (x.status == USB_XFER_PENDING) {
        if (timer_ms() - start > 2000) return -1;
        usb_poll();
        timer_idle();
    }
    return x.status == USB_XFER_OK ? (int)x.actual : -1;
}

/* one SCSI command: CBW out, optional data in, CSW in */
static int scsi_in(usb_device_t* d, msc_t* m, const uint8_t* cdb, uint8_t cdb_len, uint32_t data_len) {
    uint8_t* b = m->buf;
    memset(b, 0, 31);
    uint32_t v;
    v = BOT_CBW_SIG; memcpy(b, &v, 4);
    v = 0xBA7A0001u; memcpy(b + 4, &v, 4);        /* tag */
    memcpy(b + 8, &data_len, 4);
    b[12] = 0x80;                                  /* data in */
    b[14] = cdb_len;
    memcpy(b + 15, cdb, cdb_len);
    if (bulk(d, m->ep_out, b, 31) != 31) return -1;
    int got = bulk(d, m->ep_in, b + 64, data_len);
    if (got < 0) return -1;
    if (bulk(d, m->ep_in, b + 512, 13) != 13) return -1;
    memcpy(&v, b + 512, 4);
    if (v != BOT_CSW_SIG || b[512 + 12] != 0) return -1;
    return got;
}

static int msc_probe(usb_device_t* d) {
    msc_eps_t m;
    memset(&m, 0, sizeof(m));
    usb_walk_config(d, 0, ep_cb, &m);
    return (m.have_in && m.have_out) ? 0 : -1;
}

static int msc_attach(usb_device_t* d) {
    msc_eps_t e;
    memset(&e, 0, sizeof(e));
    usb_walk_config(d, d->active_config, ep_cb, &e);
    if (!e.have_in || !e.have_out) return -1;
    if (usb_open_endpoint(d, &e.in) != 0 || usb_open_endpoint(d, &e.out) != 0) return -1;
    msc_t* m = (msc_t*)kzalloc(sizeof(msc_t));
    if (!m) return -1;
    m->ep_in = e.in.bEndpointAddress;
    m->ep_out = e.out.bEndpointAddress;
    m->buf = (uint8_t*)usb_dma_alloc(4096);
    if (!m->buf) return -1;
    d->drvdata = m;

    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t capacity[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char vendor[9] = "", product[17] = "";
    if (scsi_in(d, m, inquiry, 6, 36) >= 36) {
        memcpy(vendor, m->buf + 64 + 8, 8);
        memcpy(product, m->buf + 64 + 16, 16);
    }
    uint32_t mib = 0;
    if (scsi_in(d, m, capacity, 10, 8) >= 8) {
        const uint8_t* c = m->buf + 64;
        uint32_t last = ((uint32_t)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
        uint32_t bs = ((uint32_t)c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7];
        mib = (uint32_t)(((uint64_t)(last + 1) * bs) >> 20);
    }
    ksnprintf(m->info, sizeof(m->info), "%s %s, %u MiB", vendor, product, mib);
    klog("usb-storage: %s\n", m->info);
    return 0;
}

const usb_driver_t usb_storage_driver = {
    "usb-storage", msc_probe, msc_attach, NULL, NULL,
};
