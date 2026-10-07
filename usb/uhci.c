#include "usbcore.h"
#include "pci.h"
#include "io.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * UHCI (Universal Host Controller Interface, USB 1.1) driver: Intel
 * chipsets up to ICH10 and VIA. Next to an EHCI controller these are its
 * "companions": EHCI hands them its low/full-speed devices - mice and
 * keyboards above all.
 *
 * The controller walks a frame list (1024 entries, one per millisecond);
 * every entry points at one anchor queue head, and behind it hangs a QH per
 * endpoint (control, bulk and interrupt alike - an interrupt endpoint is
 * then polled every frame, which is allowed). A transfer is a chain of
 * transfer descriptors (one per packet) on its endpoint's QH; the driver
 * notices it is over when the chain's TDs are inactive (or one stopped
 * with an error, or came back short). Data toggles are kept per endpoint.
 *
 * Taken over from the BIOS (its legacy keyboard emulation, PCI LEGSUP) and
 * run with its interrupts off: everything is polled (usb_poll), so these
 * controllers no longer raise the interrupt storms of before.
 * Section numbers: UHCI design guide 1.1.
 */

#define U_CMD    0x00
#define U_STS    0x02
#define U_INTR   0x04
#define U_FRNUM  0x06
#define U_FLBASE 0x08
#define U_SOF    0x0C
#define U_PORT(p) (0x10 + 2 * ((p) - 1))

#define CMD_RS      0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF      0x0040
#define CMD_MAXP    0x0080
#define STS_HALTED  0x0020

#define P_CCS  0x0001
#define P_CSC  0x0002
#define P_PE   0x0004
#define P_PEC  0x0008
#define P_LSDA 0x0100
#define P_PR   0x0200
#define P_WC   (P_CSC | P_PEC)                    /* write-1-to-clear */

#define LINK_T  1u
#define LINK_Q  2u
#define LINK_VF 4u

/* TD control/status */
#define TD_ACTIVE (1u << 23)
#define TD_STALL  (1u << 22)
#define TD_ERRMASK (0x7Eu << 16)                  /* stalled, buffer, babble, NAK(ignored), CRC/timeout, bitstuff */
#define TD_IOC    (1u << 24)
#define TD_LS     (1u << 26)
#define TD_CERR3  (3u << 27)
#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define MAX_UHCI 8
#define MAX_UDEV 16

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t link, ctrl, token, buf;
    uint32_t len;                                  /* (software: the bytes asked for) */
    uint32_t pad[3];
} utd_t;                                           /* 32 bytes */

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t head, elem;
    uint32_t pad[2];
} uqh_hw_t;

typedef struct {
    uqh_hw_t*   qh;
    uint8_t     ep;                                /* endpoint address */
    uint16_t    mps;
    int         toggle;
    utd_t*      tds;                               /* the transfer under way */
    uint8_t*    tds_mem;
    int         ntd;
    usb_xfer_t* xfer;
} uep_t;

typedef struct uhci uhci_t;

typedef struct {
    usb_device_t* udev;
    uint8_t       port, addr, low;
    uint8_t*      ctrl_buf;                        /* setup packet (64 B) + data (4 KiB) */
    uep_t*        ep[32];                          /* by (num*2 + in); [0] = control */
} udev_t;

struct uhci {
    usb_hc_t  hc;
    uint16_t  io;
    int       ports;
    uint32_t* frames;
    uqh_hw_t* anchor;
    udev_t*   port_dev[8];
    udev_t*   devs[MAX_UDEV];
    uint8_t   next_addr;
};

static uhci_t* g_uhci[MAX_UHCI];
static int     g_uhci_count;

static uint16_t r16(uhci_t* u, int r) { return inw((uint16_t)(u->io + r)); }
static void     w16(uhci_t* u, int r, uint16_t v) { outw((uint16_t)(u->io + r), v); }
static void     w32(uhci_t* u, int r, uint32_t v) { outl((uint16_t)(u->io + r), v); }

static void* dma16(uint32_t size, uint8_t** raw) {
    *raw = (uint8_t*)kmalloc(size + 16);
    if (!*raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)*raw + 15) & ~(uintptr_t)15);
    memset(p, 0, size);
    return p;
}

/* ── endpoints and transfers ─────────────────────────────────────── */

static uep_t* ep_new(uhci_t* u, uint8_t ep, uint16_t mps) {
    uep_t* e = (uep_t*)kzalloc(sizeof(uep_t));
    uint8_t* raw;
    if (!e) return NULL;
    e->qh = (uqh_hw_t*)dma16(sizeof(uqh_hw_t), &raw);
    if (!e->qh) { kfree(e); return NULL; }
    e->ep = ep;
    e->mps = mps ? mps : 8;
    e->qh->elem = LINK_T;
    /* behind the anchor: one aligned store links it in */
    e->qh->head = u->anchor->head;
    __asm__ volatile("" ::: "memory");
    u->anchor->head = (uint32_t)(uintptr_t)e->qh | LINK_Q;
    return e;
}

static void ep_unlink(uhci_t* u, uep_t* e) {
    uint32_t me = (uint32_t)(uintptr_t)e->qh | LINK_Q;
    uqh_hw_t* p = u->anchor;
    for (int guard = 0; guard < 256 && !(p->head & LINK_T); guard++) {
        if (p->head == me) { p->head = e->qh->head; break; }
        p = (uqh_hw_t*)(uintptr_t)(p->head & ~0xFu);
    }
    e->qh->elem = LINK_T;
    usb_delay_ms(2);                               /* (the controller may still be on it this frame) */
}

static void ep_drop_tds(uep_t* e) {
    e->qh->elem = LINK_T;
    if (e->tds_mem) kfree(e->tds_mem);
    e->tds_mem = NULL;
    e->tds = NULL;
    e->ntd = 0;
}

static void td_fill(utd_t* t, utd_t* next, int low, uint8_t pid, uint8_t addr, uint8_t ep, int toggle,
                    uint32_t buf, uint32_t len) {
    t->link = next ? ((uint32_t)(uintptr_t)next | LINK_VF) : LINK_T;
    t->ctrl = TD_CERR3 | TD_ACTIVE | (low ? TD_LS : 0);
    t->token = pid | ((uint32_t)addr << 8) | ((uint32_t)(ep & 0x0F) << 15) | ((uint32_t)(toggle & 1) << 19) |
               (((len ? len - 1 : 0x7FFu) & 0x7FFu) << 21);
    t->buf = buf;
    t->len = len;
}

/* The state of the TD chain under way on e: 0 still running, 1 done
 * (*status, *actual - the data bytes, setup/status stages not counted for
 * a control transfer as the caller skips them), toggle updated. */
static int ep_check(uep_t* e, int first_data, int last_data, int* status, uint32_t* actual) {
    uint32_t got = 0;
    for (int i = 0; i < e->ntd; i++) {
        utd_t* t = &e->tds[i];
        uint32_t c = t->ctrl;
        if (c & TD_ACTIVE) return 0;
        if (c & (TD_ERRMASK & ~(1u << 19))) {
            *status = (c & TD_STALL) ? USB_XFER_STALL : USB_XFER_ERROR;
            *actual = got;
            return 1;
        }
        uint32_t n = (c + 1) & 0x7FF;              /* actual length (n - 1 encoded) */
        if (n == 0x800) n = 0;
        if (n > t->len) n = t->len;
        if (i >= first_data && i <= last_data) {
            got += n;
            e->toggle ^= 1;                        /* (data packets only: setup/status set their own) */
            if (n < t->len) {                      /* short packet: the transfer is over */
                *status = USB_XFER_OK;
                *actual = got;
                return 2;
            }
        }
    }
    *status = USB_XFER_OK;
    *actual = got;
    return 1;
}

/* packets of a data stage from toggle onward; returns the TD count */
static int build_data(utd_t* tds, int low, uint8_t pid, uint8_t addr, uint8_t ep, uint16_t mps,
                      int toggle, uint32_t buf, uint32_t len) {
    int n = 0;
    uint32_t off = 0;
    do {
        uint32_t l = len - off < mps ? len - off : mps;
        td_fill(&tds[n], NULL, low, pid, addr, ep, toggle, buf + off, l);
        if (n) tds[n - 1].link = (uint32_t)(uintptr_t)&tds[n] | LINK_VF;
        toggle ^= 1;
        off += l;
        n++;
    } while (off < len);
    return n;
}

static int uhci_control(usb_device_t* d, const usb_setup_t* s, void* data, uint32_t timeout_ms) {
    udev_t* ud = (udev_t*)d->hcpriv;
    uep_t* e = ud->ep[0];
    uint32_t len = s->wLength;
    int in = (s->bmRequestType & USB_DIR_IN) != 0;
    if (len > 4096 || !ud->ctrl_buf) return -1;
    uint8_t* sb = ud->ctrl_buf;
    memcpy(sb, s, 8);
    uint8_t* db = sb + 64;
    if (!in && len) memcpy(db, data, len);

    int max = 2 + (int)((len + e->mps - 1) / e->mps);
    e->tds = (utd_t*)dma16((uint32_t)max * sizeof(utd_t), &e->tds_mem);
    if (!e->tds) return -1;
    int n = 0;
    td_fill(&e->tds[n++], NULL, ud->low, PID_SETUP, ud->addr, 0, 0, (uint32_t)(uintptr_t)sb, 8);
    int first = n, last = n - 1;
    if (len) {
        n += build_data(&e->tds[n], ud->low, in ? PID_IN : PID_OUT, ud->addr, 0, e->mps, 1,
                        (uint32_t)(uintptr_t)db, len);
        last = n - 1;
    }
    td_fill(&e->tds[n++], NULL, ud->low, (len && in) ? PID_OUT : PID_IN, ud->addr, 0, 1, 0, 0);
    for (int i = 0; i + 1 < n; i++) e->tds[i].link = (uint32_t)(uintptr_t)&e->tds[i + 1] | LINK_VF;
    e->ntd = n;
    __asm__ volatile("" ::: "memory");
    e->qh->elem = (uint32_t)(uintptr_t)&e->tds[0];

    int status = USB_XFER_ERROR, r = 0;
    uint32_t actual = 0, start = timer_ms();
    for (;;) {
        r = ep_check(e, first, last, &status, &actual);
        if (r == 2) {
            /* a short IN data stage: the status stage still has to run */
            utd_t* st = &e->tds[n - 1];
            if (st->ctrl & TD_ACTIVE) {
                e->qh->elem = (uint32_t)(uintptr_t)st;
                while ((st->ctrl & TD_ACTIVE) && timer_ms() - start <= timeout_ms) usb_wait();
            }
            break;
        }
        if (r) break;
        if (timer_ms() - start > timeout_ms) { status = USB_XFER_ERROR; break; }
        usb_wait();
    }
    ep_drop_tds(e);
    if (status != USB_XFER_OK) return -1;
    if (in && actual) memcpy(data, db, actual);
    return (int)actual;
}

static int uhci_set_ep0_mps(usb_device_t* d, uint16_t mps) {
    udev_t* ud = (udev_t*)d->hcpriv;
    ud->ep[0]->mps = mps;
    return 0;
}

static int uhci_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) {
    uhci_t* u = (uhci_t*)d->hc->priv;
    udev_t* ud = (udev_t*)d->hcpriv;
    int type = ep->bmAttributes & 3;
    if (type != USB_EP_BULK && type != USB_EP_INTERRUPT) return -1;
    int idx = (ep->bEndpointAddress & 0x0F) * 2 + ((ep->bEndpointAddress & 0x80) ? 1 : 0);
    if (ud->ep[idx]) return 0;
    ud->ep[idx] = ep_new(u, ep->bEndpointAddress, ep->wMaxPacketSize & 0x7FF);
    return ud->ep[idx] ? 0 : -1;
}

static int uhci_submit(usb_xfer_t* x) {
    udev_t* ud = (udev_t*)x->dev->hcpriv;
    int in = (x->ep & 0x80) != 0;
    uep_t* e = ud->ep[(x->ep & 0x0F) * 2 + (in ? 1 : 0)];
    if (!e || e->xfer || x->len > 16384) { x->status = USB_XFER_ERROR; return -1; }
    int max = (int)((x->len + e->mps - 1) / e->mps);
    if (max < 1) max = 1;
    e->tds = (utd_t*)dma16((uint32_t)max * sizeof(utd_t), &e->tds_mem);
    if (!e->tds) { x->status = USB_XFER_ERROR; return -1; }
    e->ntd = build_data(e->tds, ud->low, in ? PID_IN : PID_OUT, ud->addr, x->ep, e->mps, e->toggle,
                        (uint32_t)(uintptr_t)x->buf, x->len);
    e->xfer = x;
    __asm__ volatile("" ::: "memory");
    e->qh->elem = (uint32_t)(uintptr_t)&e->tds[0];
    return 0;
}

static void uhci_poll(usb_hc_t* hc) {
    uhci_t* u = (uhci_t*)hc->priv;
    for (int p = 1; p <= u->ports; p++)
        if (r16(u, U_PORT(p)) & P_CSC) hc->port_change = 1;
    for (int k = 0; k < MAX_UDEV; k++) {
        udev_t* ud = u->devs[k];
        if (!ud) continue;           /* (also while it is being set up: its driver already transfers) */
        for (int i = 1; i < 32; i++) {
            uep_t* e = ud->ep[i];
            if (!e || !e->xfer) continue;
            int status;
            uint32_t actual;
            uint32_t saved = (uint32_t)e->toggle;
            if (!ep_check(e, 0, e->ntd - 1, &status, &actual)) { e->toggle = (int)saved; continue; }
            usb_xfer_t* x = e->xfer;
            e->xfer = NULL;
            ep_drop_tds(e);
            if (status != USB_XFER_OK) e->toggle = 0;    /* (after a stall: CLEAR_FEATURE resets it on both sides) */
            x->actual = actual;
            x->status = status;
        }
    }
}

/* ── devices ────────────────────────────────────────────────────── */

static udev_t* udev_new(uhci_t* u, uint8_t port, int low) {
    int slot = -1;
    for (int i = 0; i < MAX_UDEV; i++) if (!u->devs[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    udev_t* ud = (udev_t*)kzalloc(sizeof(udev_t));
    if (!ud) return NULL;
    ud->port = port;
    ud->low = (uint8_t)low;
    ud->ep[0] = ep_new(u, 0, 8);
    ud->ctrl_buf = (uint8_t*)usb_dma_alloc(8192);
    if (!ud->ep[0] || !ud->ctrl_buf) { if (ud->ep[0]) ep_unlink(u, ud->ep[0]); kfree(ud); return NULL; }
    u->devs[slot] = ud;
    return ud;
}

static int udev_address(uhci_t* u, udev_t* ud) {
    uint8_t addr = u->next_addr++;
    if (u->next_addr > 127) u->next_addr = 1;
    usb_device_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.hc = &u->hc;
    tmp.hcpriv = ud;
    tmp.present = 1;
    usb_setup_t sa = { USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, addr, 0, 0 };
    if (uhci_control(&tmp, &sa, NULL, 1000) < 0) return -1;
    usb_delay_ms(5);
    ud->addr = addr;
    return 0;
}

static void udev_free(uhci_t* u, udev_t* ud) {
    if (!ud) return;
    for (int i = 0; i < MAX_UDEV; i++) if (u->devs[i] == ud) u->devs[i] = NULL;
    for (int i = 0; i < 32; i++) {
        uep_t* e = ud->ep[i];
        if (!e) continue;
        ep_unlink(u, e);
        if (e->xfer) e->xfer->status = USB_XFER_GONE;
        ep_drop_tds(e);
    }
    kfree(ud);
}

static void port_write(uhci_t* u, int p, uint16_t set, uint16_t clear) {
    uint16_t v = r16(u, U_PORT(p));
    w16(u, U_PORT(p), (uint16_t)(((v & ~P_WC) | set) & ~clear));
}

static void attach_port(uhci_t* u, int p) {
    /* reset 50 ms, then enable (the controller does not do it by itself) */
    port_write(u, p, P_PR, 0);
    usb_delay_ms(50);
    port_write(u, p, 0, P_PR);
    usb_delay_ms(1);
    for (int i = 0; i < 10 && !(r16(u, U_PORT(p)) & P_PE); i++) {
        port_write(u, p, P_PE, 0);
        usb_delay_ms(10);
    }
    uint16_t v = r16(u, U_PORT(p));
    w16(u, U_PORT(p), (uint16_t)(v | P_WC));       /* clear the change bits */
    if (!(v & P_PE) || !(v & P_CCS)) { klog("uhci: port %d does not enable\n", p); return; }
    int low = (v & P_LSDA) != 0;
    udev_t* ud = udev_new(u, (uint8_t)p, low);
    if (!ud) return;
    u->port_dev[p] = ud;
    if (udev_address(u, ud) < 0) {
        klog("uhci: port %d: SET_ADDRESS failed\n", p);
        u->port_dev[p] = NULL;
        udev_free(u, ud);
        return;
    }
    ud->udev = usb_new_device(&u->hc, p, low ? USB_SPEED_LOW : USB_SPEED_FULL, ud->addr, 8, ud);
}

static void detach_port(uhci_t* u, int p) {
    udev_t* ud = u->port_dev[p];
    if (!ud) return;
    u->port_dev[p] = NULL;
    if (ud->udev) usb_device_gone(ud->udev);
    udev_free(u, ud);
}

static void uhci_rescan(usb_hc_t* hc) {
    uhci_t* u = (uhci_t*)hc->priv;
    for (int p = 1; p <= u->ports; p++) {
        uint16_t v = r16(u, U_PORT(p));
        int changed = (v & P_CSC) != 0;
        if (v & P_WC) w16(u, U_PORT(p), (uint16_t)(v | P_WC) & (uint16_t)~P_PE);
        int connected = (v & P_CCS) != 0;
        if (u->port_dev[p] && (!connected || changed)) detach_port(u, p);
        if (connected && !u->port_dev[p]) attach_port(u, p);
    }
}

/* a device reset on a (full-speed) hub's port */
static usb_device_t* uhci_attach_child(usb_hc_t* hc, usb_device_t* hub, int port, int speed) {
    uhci_t* u = (uhci_t*)hc->priv;
    if (speed == USB_SPEED_HIGH) speed = USB_SPEED_FULL;   /* (a USB 1.1 bus has no high speed) */
    udev_t* ud = udev_new(u, (uint8_t)port, speed == USB_SPEED_LOW);
    if (!ud) return NULL;
    if (udev_address(u, ud) < 0) { klog("uhci: hub port %d: SET_ADDRESS failed\n", port); udev_free(u, ud); return NULL; }
    ud->udev = usb_new_child(hc, hub, port, speed, ud->addr, 8, ud);
    if (!ud->udev) { udev_free(u, ud); return NULL; }
    return ud->udev;
}

static void uhci_detach_child(usb_device_t* d) {
    udev_free((uhci_t*)d->hc->priv, (udev_t*)d->hcpriv);
}

static const usb_hc_ops_t g_ops = {
    uhci_control, uhci_set_ep0_mps, uhci_open_endpoint, uhci_submit, uhci_poll, uhci_rescan,
    uhci_attach_child, uhci_detach_child,
};

/* ── bring-up ───────────────────────────────────────────────────── */

int uhci_init_controller(const pci_dev_t* pd) {
    if (g_uhci_count >= MAX_UHCI) return -1;
    int is_io = 0;
    uintptr_t bar = pci_bar(pd, 4, &is_io);
    if (!is_io || !bar) return -1;
    pci_enable(pd);

    uhci_t* u = (uhci_t*)kzalloc(sizeof(uhci_t));
    if (!u) return -1;
    u->io = (uint16_t)bar;
    u->next_addr = 1;

    /* from the BIOS: legacy support off (no more SMIs, no keyboard
     * emulation - the HID driver takes the keyboards), its status cleared */
    pci_write16(pd->bus, pd->dev, pd->fn, 0xC0, 0x8F00);
    w16(u, U_INTR, 0);
    w16(u, U_CMD, 0);
    uint32_t start = timer_ms();
    while (!(r16(u, U_STS) & STS_HALTED) && timer_ms() - start < 50) timer_idle();
    w16(u, U_CMD, CMD_HCRESET);
    start = timer_ms();
    while ((r16(u, U_CMD) & CMD_HCRESET) && timer_ms() - start < 50) timer_idle();
    if (r16(u, U_CMD) & CMD_HCRESET) { klog("uhci: reset failed\n"); kfree(u); return -1; }
    w16(u, U_INTR, 0);                             /* polled: no interrupts at all */
    w16(u, U_STS, 0x3F);

    uint8_t* raw;
    u->frames = (uint32_t*)usb_dma_alloc(4096);
    u->anchor = (uqh_hw_t*)dma16(sizeof(uqh_hw_t), &raw);
    if (!u->frames || !u->anchor) { kfree(u); return -1; }
    u->anchor->head = LINK_T;
    u->anchor->elem = LINK_T;
    for (int i = 0; i < 1024; i++) u->frames[i] = (uint32_t)(uintptr_t)u->anchor | LINK_Q;
    w16(u, U_FRNUM, 0);
    w32(u, U_FLBASE, (uint32_t)(uintptr_t)u->frames);
    w16(u, U_CMD, CMD_RS | CMD_CF | CMD_MAXP);

    /* the root ports: 2 on every chip (a register that reads with bit 7 set is a port) */
    u->ports = 0;
    for (int p = 1; p <= 4; p++) {
        uint16_t v = r16(u, U_PORT(p));
        if (v == 0xFFFF || !(v & 0x0080)) break;
        u->ports = p;
    }
    if (u->ports < 2) u->ports = 2;

    u->hc.name = "uhci";
    u->hc.ops = &g_ops;
    u->hc.priv = u;
    u->hc.ports = u->ports;
    ksnprintf(u->hc.desc, sizeof(u->hc.desc), "UHCI (USB 1.1), %d ports (PCI %02x:%02x.%x, %04x:%04x)",
              u->ports, pd->bus, pd->dev, pd->fn, pd->vendor, pd->device);
    g_uhci[g_uhci_count++] = u;
    usb_register_hc(&u->hc);
    klog("uhci: %s\n", u->hc.desc);
    return 0;
}
