#include "usbcore.h"
#include "pci.h"
#include "idt.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * xHCI (eXtensible Host Controller Interface, USB 3.x) driver.
 *
 * One command ring, one event ring (interrupter 0), a transfer ring per
 * endpoint. Transfers are queued as TRBs and completed by polling the
 * event ring; the controller interrupt only wakes sleeping tasks.
 * Devices on root ports only (no hubs). Section numbers: xHCI spec 1.2.
 */

void net_irq_notify(void);   /* net/net.c: wakes netd + blocked waiters */

/* TRB types */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_SET_TR_DEQ    16
#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD_DONE   33
#define TRB_EV_PORT       34

#define TRB_CYCLE (1u << 0)
#define TRB_TC    (1u << 1)
#define TRB_ISP   (1u << 2)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)

#define CC_SUCCESS  1
#define CC_STALL    6
#define CC_SHORT    13

/* operational registers */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define CMD_INTE  (1u << 2)
#define STS_HCH   (1u << 0)
#define STS_EINT  (1u << 3)
#define STS_CNR   (1u << 11)

#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_CSC  (1u << 17)
#define PORT_PEC  (1u << 18)
#define PORT_WRC  (1u << 19)
#define PORT_OCC  (1u << 20)
#define PORT_PRC  (1u << 21)
#define PORT_PLC  (1u << 22)
#define PORT_CEC  (1u << 23)
#define PORT_CHANGE_BITS (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)
/* bits that are safe to write back unchanged (everything else is
 * write-1-to-clear, or disables the port when written as 1) */
#define PORT_PRESERVE 0x0E00C3E0u

#define RING_TRBS 256
#define EVT_TRBS  256
#define MAX_XHCI  2

typedef struct __attribute__((packed)) {
    uint32_t param_lo, param_hi;
    uint32_t status;
    uint32_t control;
} trb_t;

typedef struct {
    volatile trb_t* trbs;
    uint32_t        enq;
    uint32_t        cycle;
    usb_xfer_t*     owner[RING_TRBS];
} ring_t;

typedef struct {
    usb_device_t* udev;
    uint8_t       slot, port, speed;
    uint8_t*      out_ctx;
    uint8_t*      in_ctx;
    ring_t*       ep[32];              /* by DCI */
    volatile int  halted[32];          /* endpoint stalled: reset in poll */
    uint8_t*      ctrl_buf;            /* control data bounce buffer */
    volatile int      ctrl_done;
    volatile uint32_t ctrl_code, ctrl_residual;
    volatile int      ctrl_short;
} xdev_t;

typedef struct {
    usb_hc_t          hc;
    pci_dev_t         pci;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint32_t* db;
    uint32_t          max_slots, max_ports, ctx_size;
    volatile uint64_t* dcbaa;
    ring_t            cmd;
    volatile trb_t*   evt;
    uint32_t          evt_deq, evt_cycle;
    volatile int      cmd_done;
    volatile uint32_t cmd_code, cmd_slot, cmd_trb;
    xdev_t*           slots[256];
    xdev_t*           port_dev[256];
} xhci_t;

static xhci_t* g_xhci[MAX_XHCI];
static int     g_xhci_count;

static inline uint32_t rd(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void wr(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline void wr64(volatile uint8_t* b, uint32_t off, uint32_t lo) {
    wr(b, off, lo);
    wr(b, off + 4, 0);   /* everything we hand the controller is below 4 GiB */
}

static int wait_reg(volatile uint8_t* b, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
    uint32_t start = timer_ms();
    while ((rd(b, off) & mask) != want) {
        if (timer_ms() - start > ms) return -1;
        timer_idle();
    }
    return 0;
}

/* ── rings ──────────────────────────────────────────────────────── */

static ring_t* ring_new(void) {
    ring_t* r = (ring_t*)kzalloc(sizeof(ring_t));
    if (!r) return NULL;
    r->trbs = (volatile trb_t*)usb_dma_alloc(RING_TRBS * sizeof(trb_t));
    if (!r->trbs) { kfree(r); return NULL; }
    r->cycle = 1;
    /* the last TRB links back to the start and toggles the cycle bit */
    volatile trb_t* l = &r->trbs[RING_TRBS - 1];
    l->param_lo = (uint32_t)(uintptr_t)r->trbs;
    l->param_hi = 0;
    l->status = 0;
    l->control = (TRB_LINK << 10) | TRB_TC;
    return r;
}

static int ring_full(const ring_t* r) {
    return r->owner[r->enq] != NULL;
}

static volatile trb_t* ring_push(ring_t* r, uint32_t param_lo, uint32_t param_hi, uint32_t status,
                                 uint32_t control, usb_xfer_t* owner) {
    volatile trb_t* t = &r->trbs[r->enq];
    t->param_lo = param_lo;
    t->param_hi = param_hi;
    t->status = status;
    r->owner[r->enq] = owner;
    __asm__ volatile("" ::: "memory");
    t->control = (control & ~TRB_CYCLE) | r->cycle;   /* hand over last */
    if (++r->enq == RING_TRBS - 1) {
        volatile trb_t* l = &r->trbs[RING_TRBS - 1];
        l->control = (TRB_LINK << 10) | TRB_TC | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return t;
}

/* ── events ─────────────────────────────────────────────────────── */

static void handle_transfer_event(xhci_t* x, volatile trb_t* e) {
    uint32_t slot = e->control >> 24;
    uint32_t dci = (e->control >> 16) & 0x1F;
    uint32_t code = e->status >> 24;
    uint32_t residual = e->status & 0xFFFFFF;
    xdev_t* xd = x->slots[slot];
    if (!xd || dci == 0 || !xd->ep[dci]) return;
    ring_t* r = xd->ep[dci];
    uint32_t idx = (e->param_lo - (uint32_t)(uintptr_t)r->trbs) / sizeof(trb_t);
    if (idx >= RING_TRBS) return;

    if (dci == 1) {                   /* control endpoint */
        uint32_t type = (r->trbs[idx].control >> 10) & 0x3F;
        if (type == TRB_DATA && code == CC_SHORT) {
            xd->ctrl_short = 1;
            xd->ctrl_residual = residual;
            return;                   /* the status stage still follows */
        }
        xd->ctrl_code = code;
        xd->ctrl_done = 1;
        if (code == CC_STALL) xd->halted[1] = 1;
        return;
    }

    usb_xfer_t* xf = r->owner[idx];
    if (!xf) return;
    r->owner[idx] = NULL;
    if (code == CC_SUCCESS || code == CC_SHORT) {
        xf->actual = xf->len - (residual <= xf->len ? residual : xf->len);
        xf->status = USB_XFER_OK;
    } else if (code == CC_STALL) {
        xf->status = USB_XFER_STALL;
        xd->halted[dci] = 1;
    } else {
        klog("xhci: slot %u ep %u transfer error, completion code %u\n", slot, dci, code);
        xf->status = USB_XFER_ERROR;
        xd->halted[dci] = 1;          /* most errors halt the endpoint too */
    }
}

static void process_events(xhci_t* x) {
    int n = 0;
    for (;;) {
        volatile trb_t* e = &x->evt[x->evt_deq];
        uint32_t ctl = e->control;
        if ((ctl & TRB_CYCLE) != x->evt_cycle) break;
        uint32_t type = (ctl >> 10) & 0x3F;
        if (type == TRB_EV_CMD_DONE) {
            if (e->param_lo == x->cmd_trb) {
                x->cmd_code = e->status >> 24;
                x->cmd_slot = ctl >> 24;
                x->cmd_done = 1;
            }
        } else if (type == TRB_EV_TRANSFER) {
            handle_transfer_event(x, e);
        } else if (type == TRB_EV_PORT) {
            x->hc.port_change = 1;
        }
        if (++x->evt_deq == EVT_TRBS) {
            x->evt_deq = 0;
            x->evt_cycle ^= 1;
        }
        n++;
    }
    if (n) {
        /* tell the controller how far we got; bit 3 clears Event Handler Busy */
        wr(x->rt, 0x38, (uint32_t)(uintptr_t)&x->evt[x->evt_deq] | (1u << 3));
        wr(x->rt, 0x3C, 0);
    }
}

static uint32_t command(xhci_t* x, uint32_t param_lo, uint32_t status, uint32_t control) {
    x->cmd_done = 0;
    volatile trb_t* t = ring_push(&x->cmd, param_lo, 0, status, control, NULL);
    x->cmd_trb = (uint32_t)(uintptr_t)t;
    x->db[0] = 0;
    uint32_t start = timer_ms();
    while (!x->cmd_done) {
        process_events(x);
        if (x->cmd_done) break;
        if (timer_ms() - start > 3000) {
            klog("xhci: command %u timed out\n", (control >> 10) & 0x3F);
            return 0;
        }
        timer_idle();
    }
    return x->cmd_code;
}

static void xhci_irq(void) {
    for (int i = 0; i < g_xhci_count; i++) {
        xhci_t* x = g_xhci[i];
        uint32_t sts = rd(x->op, OP_USBSTS);
        if (!(sts & STS_EINT)) continue;
        wr(x->op, OP_USBSTS, STS_EINT);          /* write-1-to-clear */
        wr(x->rt, 0x20, rd(x->rt, 0x20) | 1u);   /* IMAN.IP (write-1-to-clear), keeps IE */
        net_irq_notify();
    }
}

/* ── contexts ───────────────────────────────────────────────────── */

static uint32_t* in_ctx(xhci_t* x, xdev_t* xd, int idx) {   /* 0 = control, 1 = slot, 1+dci = ep */
    return (uint32_t*)(xd->in_ctx + (uint32_t)idx * x->ctx_size);
}
static uint32_t* out_ctx(xhci_t* x, xdev_t* xd, int idx) {  /* 0 = slot, dci = ep */
    return (uint32_t*)(xd->out_ctx + (uint32_t)idx * x->ctx_size);
}

static void clear_input(xhci_t* x, xdev_t* xd) {
    memset(xd->in_ctx, 0, 33 * x->ctx_size);
}

/* ── endpoint recovery (after a stall/error) ───────────────────── */

static void reset_endpoint(xhci_t* x, xdev_t* xd, uint32_t dci) {
    ring_t* r = xd->ep[dci];
    if (!r) return;
    command(x, 0, 0, (TRB_RESET_EP << 10) | (dci << 16) | ((uint32_t)xd->slot << 24));
    /* skip whatever was left on the ring: continue from our enqueue point */
    for (int i = 0; i < RING_TRBS; i++) {
        if (r->owner[i]) { r->owner[i]->status = USB_XFER_ERROR; r->owner[i] = NULL; }
    }
    command(x, (uint32_t)(uintptr_t)&r->trbs[r->enq] | r->cycle, 0,
            (TRB_SET_TR_DEQ << 10) | (dci << 16) | ((uint32_t)xd->slot << 24));
    xd->halted[dci] = 0;
    if (dci > 1 && xd->udev) {
        uint8_t epaddr = (uint8_t)((dci >> 1) | ((dci & 1) ? 0x80 : 0));
        usb_setup_t s = { USB_RECIP_EP, USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, epaddr, 0 };
        usb_device_t* d = xd->udev;
        d->hc->ops->control(d, &s, NULL, 1000);
    }
}

/* ── USB core operations ────────────────────────────────────────── */

static int xhci_control(usb_device_t* d, const usb_setup_t* s, void* data, uint32_t timeout_ms) {
    xhci_t* x = (xhci_t*)d->hc->priv;
    xdev_t* xd = (xdev_t*)d->hcpriv;
    ring_t* r = xd->ep[1];
    uint32_t len = s->wLength;
    int in = (s->bmRequestType & USB_DIR_IN) != 0;
    if (len > 4096) return -1;
    if (!in && len) memcpy(xd->ctrl_buf, data, len);

    uint32_t setup[2];
    memcpy(setup, s, 8);
    uint32_t trt = len == 0 ? 0 : (in ? 3 : 2);
    xd->ctrl_done = 0;
    xd->ctrl_short = 0;
    xd->ctrl_residual = 0;
    ring_push(r, setup[0], setup[1], 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16), NULL);
    if (len)
        ring_push(r, (uint32_t)(uintptr_t)xd->ctrl_buf, 0, len,
                  (TRB_DATA << 10) | TRB_ISP | (in ? (1u << 16) : 0), NULL);
    ring_push(r, 0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : (1u << 16)), NULL);
    x->db[xd->slot] = 1;

    uint32_t start = timer_ms();
    while (!xd->ctrl_done) {
        process_events(x);
        if (xd->ctrl_done) break;
        if (timer_ms() - start > timeout_ms) {
            klog("xhci: control request %02x timed out (slot %u)\n", s->bRequest, xd->slot);
            return -1;
        }
        timer_idle();
    }
    if (xd->ctrl_code != CC_SUCCESS && xd->ctrl_code != CC_SHORT) {
        if (xd->halted[1]) reset_endpoint(x, xd, 1);
        return -1;
    }
    uint32_t actual = len - (xd->ctrl_short ? xd->ctrl_residual : 0);
    if (actual > len) actual = len;
    if (in && actual) memcpy(data, xd->ctrl_buf, actual);
    return (int)actual;
}

static int xhci_set_ep0_mps(usb_device_t* d, uint16_t mps) {
    xhci_t* x = (xhci_t*)d->hc->priv;
    xdev_t* xd = (xdev_t*)d->hcpriv;
    clear_input(x, xd);
    in_ctx(x, xd, 0)[1] = 1u << 1;                         /* add EP0 */
    memcpy(in_ctx(x, xd, 2), out_ctx(x, xd, 1), x->ctx_size);
    uint32_t* ep0 = in_ctx(x, xd, 2);
    ep0[1] = (ep0[1] & 0x0000FFFFu) | ((uint32_t)mps << 16);
    uint32_t cc = command(x, (uint32_t)(uintptr_t)xd->in_ctx, 0,
                          (TRB_EVAL_CTX << 10) | ((uint32_t)xd->slot << 24));
    return cc == CC_SUCCESS ? 0 : -1;
}

static int xhci_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) {
    xhci_t* x = (xhci_t*)d->hc->priv;
    xdev_t* xd = (xdev_t*)d->hcpriv;
    uint32_t num = ep->bEndpointAddress & 0x0F;
    int in = (ep->bEndpointAddress & 0x80) != 0;
    uint32_t dci = num * 2 + (in ? 1 : 0);
    uint32_t kind = ep->bmAttributes & 3;
    uint32_t type;
    if (kind == USB_EP_BULK) type = in ? 6 : 2;
    else if (kind == USB_EP_INTERRUPT) type = in ? 7 : 3;
    else return -1;                                          /* no isochronous */

    uint32_t mps = ep->wMaxPacketSize & 0x7FF;
    uint32_t burst = 0, interval = 0;
    if (kind == USB_EP_INTERRUPT) {
        if (d->speed == USB_SPEED_HIGH || d->speed == USB_SPEED_SUPER) {
            burst = (ep->wMaxPacketSize >> 11) & 3;
            interval = ep->bInterval ? ep->bInterval - 1u : 0;
        } else {
            /* full/low speed: bInterval is in 1 ms frames -> log2(125us units) */
            uint32_t us125 = (uint32_t)(ep->bInterval ? ep->bInterval : 1) * 8u;
            interval = 0;
            while ((2u << interval) <= us125) interval++;
            if (interval < 3) interval = 3;
            if (interval > 10) interval = 10;
        }
        if (interval > 15) interval = 15;
    }

    if (!xd->ep[dci]) xd->ep[dci] = ring_new();
    ring_t* r = xd->ep[dci];
    if (!r) return -1;

    clear_input(x, xd);
    in_ctx(x, xd, 0)[1] = 1u | (1u << dci);
    memcpy(in_ctx(x, xd, 1), out_ctx(x, xd, 0), x->ctx_size);
    uint32_t* slot = in_ctx(x, xd, 1);
    uint32_t entries = (slot[0] >> 27) & 0x1F;
    if (dci > entries) entries = dci;
    slot[0] = (slot[0] & 0x07FFFFFFu) | (entries << 27);
    slot[3] = 0;

    uint32_t* e = in_ctx(x, xd, (int)dci + 1);
    e[0] = interval << 16;
    e[1] = (3u << 1) | (type << 3) | (burst << 8) | (mps << 16);
    e[2] = (uint32_t)(uintptr_t)r->trbs | r->cycle;
    e[3] = 0;
    uint32_t esit = mps * (burst + 1);
    e[4] = (kind == USB_EP_INTERRUPT) ? (esit | (esit << 16)) : (mps ? mps : 512);

    uint32_t cc = command(x, (uint32_t)(uintptr_t)xd->in_ctx, 0,
                          (TRB_CONFIG_EP << 10) | ((uint32_t)xd->slot << 24));
    if (cc != CC_SUCCESS) {
        klog("xhci: configure endpoint %02x failed (%u)\n", ep->bEndpointAddress, cc);
        return -1;
    }
    return 0;
}

static int xhci_submit(usb_xfer_t* xf) {
    usb_device_t* d = xf->dev;
    xhci_t* x = (xhci_t*)d->hc->priv;
    xdev_t* xd = (xdev_t*)d->hcpriv;
    uint32_t num = xf->ep & 0x0F;
    int in = (xf->ep & 0x80) != 0;
    uint32_t dci = num * 2 + (in ? 1 : 0);
    ring_t* r = xd->ep[dci];
    uint32_t start = (uint32_t)(uintptr_t)xf->buf;
    if (!r || xf->len > 65536 || ((start & 0xFFFF) + xf->len > 65536 && xf->len)) {
        xf->status = USB_XFER_ERROR;   /* not open, or the buffer crosses a 64K boundary */
        return -1;
    }
    if (ring_full(r)) { xf->status = USB_XFER_ERROR; return -1; }
    xf->hcpriv = x;
    ring_push(r, start, 0, xf->len & 0x1FFFF, (TRB_NORMAL << 10) | TRB_IOC | (in ? TRB_ISP : 0), xf);
    x->db[xd->slot] = dci;
    return 0;
}

/* ── ports ──────────────────────────────────────────────────────── */

static uint32_t portsc(xhci_t* x, int p) { return rd(x->op, OP_PORTSC(p)); }

static void port_ack(xhci_t* x, int p, uint32_t bits) {
    wr(x->op, OP_PORTSC(p), (portsc(x, p) & PORT_PRESERVE) | bits);
}

static int port_reset(xhci_t* x, int p) {
    uint32_t v = portsc(x, p);
    if (!(v & PORT_CCS)) return -1;
    if (!(v & PORT_PED)) {
        /* USB 2 ports need a reset to enable; USB 3 ports come up by
         * themselves (and a reset does no harm) */
        wr(x->op, OP_PORTSC(p), (v & PORT_PRESERVE) | PORT_PR);
        uint32_t start = timer_ms();
        while (!(portsc(x, p) & PORT_PRC)) {
            if (timer_ms() - start > 500) return -1;
            timer_idle();
        }
        usb_delay_ms(20);             /* reset recovery (TRSTRCY is 10 ms) */
    }
    port_ack(x, p, PORT_CHANGE_BITS & portsc(x, p));
    return (portsc(x, p) & PORT_PED) ? 0 : -1;
}

static void attach_port(xhci_t* x, int p) {
    if (port_reset(x, p) != 0) {
        klog("xhci: port %d: reset failed\n", p);
        return;
    }
    uint32_t speed = (portsc(x, p) >> 10) & 0xF;
    if (speed < 1 || speed > 4) speed = USB_SPEED_HIGH;

    uint32_t cc = command(x, 0, 0, TRB_ENABLE_SLOT << 10);
    uint32_t slot = x->cmd_slot;
    if (cc != CC_SUCCESS || slot == 0 || slot > x->max_slots) {
        klog("xhci: port %d: no slot (%u)\n", p, cc);
        return;
    }
    xdev_t* xd = (xdev_t*)kzalloc(sizeof(xdev_t));
    if (!xd) return;
    xd->slot = (uint8_t)slot;
    xd->port = (uint8_t)p;
    xd->speed = (uint8_t)speed;
    xd->out_ctx = (uint8_t*)usb_dma_alloc(32 * x->ctx_size);
    xd->in_ctx = (uint8_t*)usb_dma_alloc(33 * x->ctx_size);
    xd->ctrl_buf = (uint8_t*)usb_dma_alloc(4096);
    xd->ep[1] = ring_new();
    if (!xd->out_ctx || !xd->in_ctx || !xd->ctrl_buf || !xd->ep[1]) return;
    x->dcbaa[slot] = (uint32_t)(uintptr_t)xd->out_ctx;
    x->slots[slot] = xd;

    uint16_t mps0 = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    clear_input(x, xd);
    in_ctx(x, xd, 0)[1] = 3;                                 /* add slot + EP0 */
    uint32_t* sc = in_ctx(x, xd, 1);
    sc[0] = (1u << 27) | (speed << 20);
    sc[1] = (uint32_t)p << 16;
    uint32_t* e0 = in_ctx(x, xd, 2);
    e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps0 << 16);   /* CErr 3, control */
    e0[2] = (uint32_t)(uintptr_t)xd->ep[1]->trbs | 1u;
    e0[4] = 8;
    cc = command(x, (uint32_t)(uintptr_t)xd->in_ctx, 0, (TRB_ADDRESS_DEV << 10) | (slot << 24));
    if (cc != CC_SUCCESS) {
        klog("xhci: port %d: address device failed (%u)\n", p, cc);
        command(x, 0, 0, (TRB_DISABLE_SLOT << 10) | (slot << 24));
        x->slots[slot] = NULL;
        return;
    }
    usb_delay_ms(10);                 /* SET_ADDRESS recovery */
    x->port_dev[p] = xd;
    xd->udev = usb_new_device(&x->hc, p, (int)speed, (uint8_t)slot, mps0, xd);
}

static void detach_port(xhci_t* x, int p) {
    xdev_t* xd = x->port_dev[p];
    if (!xd) return;
    x->port_dev[p] = NULL;
    for (int i = 0; i < 32; i++) {
        ring_t* r = xd->ep[i];
        if (!r) continue;
        for (int j = 0; j < RING_TRBS; j++)
            if (r->owner[j]) { r->owner[j]->status = USB_XFER_GONE; r->owner[j] = NULL; }
    }
    if (xd->udev) usb_device_gone(xd->udev);
    command(x, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)xd->slot << 24));
    x->slots[xd->slot] = NULL;
    x->dcbaa[xd->slot] = 0;
}

static void xhci_rescan(usb_hc_t* hc) {
    xhci_t* x = (xhci_t*)hc->priv;
    for (uint32_t p = 1; p <= x->max_ports; p++) {
        uint32_t v = portsc(x, (int)p);
        if (v & PORT_CHANGE_BITS) port_ack(x, (int)p, v & PORT_CHANGE_BITS);
        int connected = (v & PORT_CCS) != 0;
        if (connected && !x->port_dev[p]) attach_port(x, (int)p);
        else if (!connected && x->port_dev[p]) detach_port(x, (int)p);
    }
}

static void xhci_poll(usb_hc_t* hc) {
    xhci_t* x = (xhci_t*)hc->priv;
    process_events(x);
    for (uint32_t s = 1; s <= x->max_slots && s < 256; s++) {
        xdev_t* xd = x->slots[s];
        if (!xd) continue;
        for (uint32_t dci = 2; dci < 32; dci++)
            if (xd->halted[dci]) reset_endpoint(x, xd, dci);
    }
}

static const usb_hc_ops_t g_ops = {
    xhci_control, xhci_set_ep0_mps, xhci_open_endpoint, xhci_submit, xhci_poll, xhci_rescan,
};

/* ── bring-up ───────────────────────────────────────────────────── */

static void bios_handoff(xhci_t* x) {
    uint32_t xecp = (rd(x->cap, 0x10) >> 16) & 0xFFFF;
    uint32_t off = xecp << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t c = rd(x->cap, off);
        if ((c & 0xFF) == 1) {                 /* USB Legacy Support */
            wr(x->cap, off, c | (1u << 24));   /* OS owned */
            uint32_t start = timer_ms();
            while ((rd(x->cap, off) & (1u << 16)) && timer_ms() - start < 1000) timer_idle();
            wr(x->cap, off, rd(x->cap, off) & ~(1u << 16));
            /* no more SMIs: disable the sources, clear what's pending */
            wr(x->cap, off + 4, 0xE0000000u);
        }
        uint32_t next = (c >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

int xhci_init_controller(const pci_dev_t* pd) {
    if (g_xhci_count >= MAX_XHCI) return -1;
    int is_io = 0;
    uint32_t bar = pci_bar(pd, 0, &is_io);
    uint32_t raw = pci_read32(pd->bus, pd->dev, pd->fn, 0x10);
    if (is_io || !bar) return -1;
    if ((raw & 6) == 4 && pci_read32(pd->bus, pd->dev, pd->fn, 0x14) != 0) {
        klog("xhci: registers above 4 GiB, skipped\n");
        return -1;
    }
    pci_enable(pd);

    /* Intel chipsets route the shared USB 2.0 ports to EHCI until told
     * otherwise: hand every port (USB 2 and 3) to xHCI */
    if (pd->vendor == 0x8086) {
        pci_write32(pd->bus, pd->dev, pd->fn, 0xD8, pci_read32(pd->bus, pd->dev, pd->fn, 0xDC));
        pci_write32(pd->bus, pd->dev, pd->fn, 0xD0, pci_read32(pd->bus, pd->dev, pd->fn, 0xD4));
    }

    xhci_t* x = (xhci_t*)kzalloc(sizeof(xhci_t));
    if (!x) return -1;
    x->pci = *pd;
    x->cap = (volatile uint8_t*)(uintptr_t)bar;
    x->op = x->cap + (rd(x->cap, 0) & 0xFF);
    x->rt = x->cap + (rd(x->cap, 0x18) & ~0x1Fu);
    x->db = (volatile uint32_t*)(x->cap + (rd(x->cap, 0x14) & ~0x3u));
    uint32_t hcs1 = rd(x->cap, 0x04);
    x->max_slots = hcs1 & 0xFF;
    if (x->max_slots > 64) x->max_slots = 64;
    x->max_ports = (hcs1 >> 24) & 0xFF;
    x->ctx_size = (rd(x->cap, 0x10) & (1u << 2)) ? 64 : 32;

    bios_handoff(x);

    /* stop, then reset (sections 4.2 / 5.4.1) */
    wr(x->op, OP_USBCMD, rd(x->op, OP_USBCMD) & ~CMD_RS);
    if (wait_reg(x->op, OP_USBSTS, STS_HCH, STS_HCH, 100) != 0) klog("xhci: controller didn't halt\n");
    wr(x->op, OP_USBCMD, CMD_HCRST);
    if (wait_reg(x->op, OP_USBCMD, CMD_HCRST, 0, 1000) != 0 ||
        wait_reg(x->op, OP_USBSTS, STS_CNR, 0, 1000) != 0) {
        klog("xhci: reset failed\n");
        kfree(x);
        return -1;
    }

    wr(x->op, OP_CONFIG, x->max_slots);
    x->dcbaa = (volatile uint64_t*)usb_dma_alloc((x->max_slots + 1) * 8);
    uint32_t hcs2 = rd(x->cap, 0x08);
    uint32_t scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        uint64_t* arr = (uint64_t*)usb_dma_alloc(scratch * 8);
        for (uint32_t i = 0; i < scratch; i++) arr[i] = (uint32_t)(uintptr_t)usb_dma_alloc(4096);
        x->dcbaa[0] = (uint32_t)(uintptr_t)arr;
    }
    wr64(x->op, OP_DCBAAP, (uint32_t)(uintptr_t)x->dcbaa);

    ring_t* cr = ring_new();
    if (!cr) return -1;
    x->cmd = *cr;
    wr64(x->op, OP_CRCR, (uint32_t)(uintptr_t)x->cmd.trbs | 1u);

    x->evt = (volatile trb_t*)usb_dma_alloc(EVT_TRBS * sizeof(trb_t));
    uint32_t* erst = (uint32_t*)usb_dma_alloc(64);
    erst[0] = (uint32_t)(uintptr_t)x->evt;
    erst[1] = 0;
    erst[2] = EVT_TRBS;
    x->evt_cycle = 1;
    wr(x->rt, 0x28, 1);                                        /* ERSTSZ */
    wr64(x->rt, 0x38, (uint32_t)(uintptr_t)x->evt);            /* ERDP */
    wr64(x->rt, 0x30, (uint32_t)(uintptr_t)erst);              /* ERSTBA */
    wr(x->rt, 0x24, 160);                                      /* IMOD: 40 us */
    wr(x->rt, 0x20, 3);                                        /* IMAN: IE, clear IP */

    x->hc.name = "xhci";
    x->hc.ops = &g_ops;
    x->hc.priv = x;
    x->hc.ports = (int)x->max_ports;
    ksnprintf(x->hc.desc, sizeof(x->hc.desc), "xHCI %x.%02x, %u ports (PCI %02x:%02x.%x, %04x:%04x)",
              rd(x->cap, 0) >> 24, (rd(x->cap, 0) >> 16) & 0xFF, x->max_ports,
              pd->bus, pd->dev, pd->fn, pd->vendor, pd->device);
    g_xhci[g_xhci_count++] = x;
    usb_register_hc(&x->hc);

    if (pd->irq_line > 0 && pd->irq_line < 16) irq_install(pd->irq_line, xhci_irq);
    wr(x->op, OP_USBCMD, CMD_RS | CMD_INTE);
    if (wait_reg(x->op, OP_USBSTS, STS_HCH, 0, 100) != 0) klog("xhci: controller didn't start\n");
    klog("xhci: %s\n", x->hc.desc);

    /* power + scan the root ports (devices need ~100 ms after power-on) */
    for (uint32_t p = 1; p <= x->max_ports; p++) {
        if (!(portsc(x, (int)p) & PORT_PP)) wr(x->op, OP_PORTSC(p), PORT_PP);
    }
    usb_delay_ms(100);
    xhci_rescan(&x->hc);
    x->hc.port_change = 0;
    return 0;
}
