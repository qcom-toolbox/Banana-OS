#include "usbcore.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"
#include "blockdev.h"

/*
 * USB mass storage (Bulk-Only Transport, SCSI): USB sticks and disks.
 * Each drive becomes a block device (kernel/blockdev.h) with sector
 * reads and writes (SCSI READ(10) / WRITE(10)); kernel/fat32.c mounts
 * the FAT32 volume on it under /mnt/usb.
 */

#define BOT_CBW_SIG 0x43425355u
#define BOT_CSW_SIG 0x53425355u
/* bytes per SCSI command: EHCI takes at most 16 KiB per transfer here */
#define MSC_CHUNK   16384u

typedef struct {
    usb_device_t* dev;
    uint8_t  ep_in, ep_out;
    uint8_t  iface;
    uint8_t* buf;              /* CBW at 0, CSW at 512, small data at 64 */
    uint8_t* data;             /* MSC_CHUNK bytes, DMA-able */
    uint32_t tag;
    uint32_t block_size;
    blockdev_t* bd;
    char     info[64];
} msc_t;

typedef struct {
    usb_ep_desc_t in, out;
    int in_msc, have_in, have_out;
    uint8_t iface, msc_iface;
} msc_eps_t;

static int ep_cb(const uint8_t* d, void* ctx) {
    msc_eps_t* m = (msc_eps_t*)ctx;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        m->in_msc = id->bInterfaceClass == 0x08 && id->bInterfaceSubClass == 0x06 &&
                    id->bInterfaceProtocol == 0x50 && id->bAlternateSetting == 0;
        if (m->in_msc) m->msc_iface = id->bInterfaceNumber;
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
        if (timer_ms() - start > 5000 || !d->present) return -1;
        /* the controller itself, not usb_poll(): this also runs from
         * inside usb_poll() (a stick plugged in), where that is a no-op */
        d->hc->ops->poll(d->hc);
        timer_idle();
    }
    return x.status == USB_XFER_OK ? (int)x.actual : -1;
}

static void clear_halt(usb_device_t* d, uint8_t ep) {
    usb_control(d, USB_TYPE_STANDARD | USB_RECIP_EP, USB_REQ_CLEAR_FEATURE, 0, ep, NULL, 0);
}

/* Bulk-Only Mass Storage Reset + clearing both endpoints (BOT 5.3.4) */
static void reset_recovery(msc_t* m) {
    usb_control(m->dev, USB_TYPE_CLASS | USB_RECIP_IFACE, 0xFF, 0, m->iface, NULL, 0);
    clear_halt(m->dev, m->ep_in);
    clear_halt(m->dev, m->ep_out);
}

/* One SCSI command: CBW out, optional data phase, CSW in. Returns the
 * data bytes moved, -1 on a transport error, -2 if the drive reported
 * the command failed (CSW status 1). */
static int scsi(msc_t* m, const uint8_t* cdb, uint8_t cdb_len, uint8_t* data, uint32_t data_len, int dir_in) {
    usb_device_t* d = m->dev;
    uint8_t* b = m->buf;
    memset(b, 0, 31);
    uint32_t v;
    v = BOT_CBW_SIG; memcpy(b, &v, 4);
    v = ++m->tag;    memcpy(b + 4, &v, 4);
    memcpy(b + 8, &data_len, 4);
    b[12] = dir_in ? 0x80 : 0x00;
    b[14] = cdb_len;
    memcpy(b + 15, cdb, cdb_len);
    if (bulk(d, m->ep_out, b, 31) != 31) { reset_recovery(m); return -1; }
    int got = 0;
    if (data_len) {
        got = bulk(d, dir_in ? m->ep_in : m->ep_out, data, data_len);
        if (got < 0) {
            /* a stalled data phase: clear it, then the CSW still follows */
            clear_halt(d, dir_in ? m->ep_in : m->ep_out);
            got = 0;
        }
    }
    int n = bulk(d, m->ep_in, b + 512, 13);
    if (n < 0) {                         /* stalled CSW: clear and retry once */
        clear_halt(d, m->ep_in);
        n = bulk(d, m->ep_in, b + 512, 13);
    }
    if (n != 13) { reset_recovery(m); return -1; }
    memcpy(&v, b + 512, 4);
    if (v != BOT_CSW_SIG) { reset_recovery(m); return -1; }
    if (b[512 + 12] == 1) return -2;
    if (b[512 + 12] != 0) { reset_recovery(m); return -1; }
    return got;
}

static int scsi_in(msc_t* m, const uint8_t* cdb, uint8_t cdb_len, uint32_t data_len) {
    return scsi(m, cdb, cdb_len, m->buf + 64, data_len, 1);
}

/* the drive's sense data after a failed command (clears a unit attention) */
static void request_sense(msc_t* m) {
    static const uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    scsi_in(m, cdb, 6, 18);
}

static int test_unit_ready(msc_t* m) {
    static const uint8_t cdb[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 10; i++) {
        int r = scsi(m, cdb, 6, NULL, 0, 0);
        if (r >= 0) return 0;
        request_sense(m);
        usb_delay_ms(100);
    }
    return -1;
}

/* READ(10) / WRITE(10) of up to MSC_CHUNK bytes, retried once */
static int rw10(msc_t* m, int write, uint32_t lba, uint32_t count) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count;
    uint32_t bytes = count * m->block_size;
    for (int tries = 0; tries < 3; tries++) {
        int r = scsi(m, cdb, 10, m->data, bytes, !write);
        if (r == (int)bytes) return 0;
        if (r == -2) request_sense(m);
    }
    return -1;
}

static int msc_read(blockdev_t* bd, uint32_t lba, uint32_t count, void* buf) {
    msc_t* m = (msc_t*)bd->priv;
    uint8_t* out = (uint8_t*)buf;
    uint32_t per = MSC_CHUNK / m->block_size;
    while (count) {
        if (!bd->present) return -1;
        uint32_t n = count < per ? count : per;
        if (rw10(m, 0, lba, n) != 0) { klog("usb-storage: read error at sector %u\n", lba); return -1; }
        memcpy(out, m->data, n * m->block_size);
        out += n * m->block_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int msc_write(blockdev_t* bd, uint32_t lba, uint32_t count, const void* buf) {
    msc_t* m = (msc_t*)bd->priv;
    const uint8_t* in = (const uint8_t*)buf;
    uint32_t per = MSC_CHUNK / m->block_size;
    while (count) {
        if (!bd->present) return -1;
        uint32_t n = count < per ? count : per;
        memcpy(m->data, in, n * m->block_size);
        if (rw10(m, 1, lba, n) != 0) { klog("usb-storage: write error at sector %u\n", lba); return -1; }
        in += n * m->block_size;
        lba += n;
        count -= n;
    }
    return 0;
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
    m->dev = d;
    m->ep_in = e.in.bEndpointAddress;
    m->ep_out = e.out.bEndpointAddress;
    m->iface = e.msc_iface;
    m->buf = (uint8_t*)usb_dma_alloc(4096);
    m->data = (uint8_t*)usb_dma_alloc(MSC_CHUNK);
    if (!m->buf || !m->data) return -1;
    m->tag = 0xBA7A0000u;
    d->drvdata = m;

    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t capacity[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char vendor[9] = "", product[17] = "";
    if (scsi_in(m, inquiry, 6, 36) >= 36) {
        memcpy(vendor, m->buf + 64 + 8, 8);
        memcpy(product, m->buf + 64 + 16, 16);
    }
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    test_unit_ready(m);
    uint32_t last = 0, bs = 0;
    for (int tries = 0; tries < 3 && !bs; tries++) {
        if (scsi_in(m, capacity, 10, 8) >= 8) {
            const uint8_t* c = m->buf + 64;
            last = ((uint32_t)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
            bs = ((uint32_t)c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7];
        } else {
            request_sense(m);
        }
    }
    uint32_t mib = (uint32_t)(((uint64_t)(last + 1) * bs) >> 20);
    ksnprintf(m->info, sizeof(m->info), "%s %s, %u MiB", vendor, product, mib);
    klog("usb-storage: %s\n", m->info);
    if (bs != 512) {
        /* FAT32 here assumes 512-byte sectors (4K-sector USB disks are rare) */
        klog("usb-storage: %u-byte sectors are not supported - not mounted\n", bs);
        return 0;
    }
    m->block_size = bs;
    char model[40];
    ksnprintf(model, sizeof(model), "%s %s", vendor, product);
    m->bd = blockdev_register(model, last + 1, bs, msc_read, msc_write, m);
    return 0;
}

static void msc_detach(usb_device_t* d) {
    msc_t* m = (msc_t*)d->drvdata;
    if (!m) return;
    if (m->bd) blockdev_unregister(m->bd);
    m->bd = NULL;
    /* m and its DMA buffers stay allocated: a transfer may still be
     * referencing them inside the host controller driver */
}

const usb_driver_t usb_storage_driver = {
    "usb-storage", msc_probe, msc_attach, NULL, msc_detach,
};
