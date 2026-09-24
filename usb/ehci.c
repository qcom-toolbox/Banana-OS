#include "usbcore.h"
#include "pci.h"
#include "idt.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * EHCI (Enhanced Host Controller Interface, USB 2.0) driver.
 *
 * High-speed devices on root ports, control and bulk transfers through
 * the asynchronous schedule (a ring of queue heads, one per endpoint).
 * Full/low-speed devices (most keyboards and mice) are handed to the
 * companion UHCI/OHCI controller, which the BIOS keeps driving - so they
 * keep working through its legacy PS/2 emulation. Section numbers: EHCI
 * spec 1.0.
 *
 * Each queue head keeps an inactive "dummy" qTD at the end of its queue:
 * queuing a transfer fills in the dummy, appends a fresh one, and flips
 * the old dummy's Active bit last - the controller never sees a
 * half-built transfer.
 */

void net_irq_notify(void);

#define OP_USBCMD    0x00
#define OP_USBSTS    0x04
#define OP_USBINTR   0x08
#define OP_CTRLDSSEG 0x10
#define OP_PERIODIC  0x14
#define OP_ASYNC     0x18
#define OP_CONFIG    0x40
#define OP_PORTSC(p) (0x44 + 4 * ((p) - 1))

#define CMD_RS     (1u << 0)
#define CMD_RESET  (1u << 1)
#define CMD_ASE    (1u << 5)
#define CMD_IAAD   (1u << 6)
#define STS_HALTED (1u << 12)
#define STS_ASS    (1u << 15)
#define STS_IAA    (1u << 5)

#define PORT_CCS   (1u << 0)
#define PORT_CSC   (1u << 1)
#define PORT_PED   (1u << 2)
#define PORT_PEDC  (1u << 3)
#define PORT_OCC   (1u << 5)
#define PORT_PR    (1u << 8)
#define PORT_LS_MASK (3u << 10)
#define PORT_LS_K  (1u << 10)            /* line state K: low-speed device */
#define PORT_PP    (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_RWC   (PORT_CSC | PORT_PEDC | PORT_OCC)

/* qTD token */
#define TOK_ACTIVE (1u << 7)
#define TOK_HALTED (1u << 6)
#define TOK_ERRORS 0x7Cu                 /* halted, buffer, babble, xact, missed uframe */
#define TOK_PID_OUT   (0u << 8)
#define TOK_PID_IN    (1u << 8)
#define TOK_PID_SETUP (2u << 8)
#define TOK_CERR3  (3u << 10)
#define TOK_IOC    (1u << 15)
#define TOK_TOGGLE (1u << 31)
#define LINK_T     1u
#define LINK_QH    (1u << 1)

#define MAX_EHCI  2
#define MAX_PEND  16
#define POOL_BLOCK 64

typedef struct __attribute__((packed)) {
    volatile uint32_t next, alt, token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];         /* 64-bit capable controllers */
    uint32_t pad[3];
} qtd_t;                                 /* 64 bytes */

typedef struct __attribute__((packed)) {
    volatile uint32_t hlink, ep_char, ep_caps, cur;
    volatile uint32_t next, alt, token;  /* overlay area */
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t pad[11];
} qh_hw_t;                               /* 128 bytes */

typedef struct {
    usb_xfer_t* xfer;                    /* NULL for synchronous control */
    qtd_t*      first;
    qtd_t*      last;
    int         ntd;
} pending_t;

typedef struct ehci ehci_t;

typedef struct {
    qh_hw_t*   hw;
    qtd_t*     dummy;
    pending_t  pend[MAX_PEND];
    uint8_t    ep;                       /* endpoint address */
    int        halted;
} qh_t;

typedef struct {
    usb_device_t* udev;
    uint8_t       port, addr;
    uint8_t*      ctrl_buf;              /* setup packet (64 B) + control data (4 KiB) */
    qh_t*         qh[32];                /* by (num*2 + in) */
} edev_t;

struct ehci {
    usb_hc_t          hc;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    int               ports;
    qh_hw_t*          head;              /* reclamation head of the async ring */
    edev_t*           port_dev[16];
    uint8_t           next_addr;
    uint8_t*          pool;              /* qTD/QH blocks */
    uint8_t*          pool_free;
};

static ehci_t* g_ehci[MAX_EHCI];
static int     g_ehci_count;

static inline uint32_t rd(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void wr(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }

/* ── 64-byte block pool (qTDs, QHs) ─────────────────────────────── */

static void* blk_alloc(ehci_t* e, uint32_t size) {
    uint32_t n = (size + POOL_BLOCK - 1) / POOL_BLOCK;
    if (n == 1 && e->pool_free) {
        uint8_t* p = e->pool_free;
        e->pool_free = *(uint8_t**)p;
        memset(p, 0, POOL_BLOCK);
        return p;
    }
    /* multi-block (QH) or empty free list: carve from a fresh 4 KiB page */
    uint8_t* page = (uint8_t*)usb_dma_alloc(4096);
    if (!page) return NULL;
    for (uint32_t off = n * POOL_BLOCK; off + POOL_BLOCK <= 4096; off += POOL_BLOCK) {
        *(uint8_t**)(page + off) = e->pool_free;
        e->pool_free = page + off;
    }
    return page;
}

static void td_free(ehci_t* e, qtd_t* t) {
    *(uint8_t**)t = e->pool_free;
    e->pool_free = (uint8_t*)t;
}

/* ── queue heads ────────────────────────────────────────────────── */

static qh_t* qh_new(ehci_t* e, uint8_t addr, uint8_t ep, uint16_t mps, int control) {
    qh_t* q = (qh_t*)kzalloc(sizeof(qh_t));
    if (!q) return NULL;
    q->hw = (qh_hw_t*)blk_alloc(e, sizeof(qh_hw_t));
    q->dummy = (qtd_t*)blk_alloc(e, sizeof(qtd_t));
    if (!q->hw || !q->dummy) return NULL;
    q->ep = ep;
    q->dummy->next = LINK_T;
    q->dummy->alt = LINK_T;
    q->dummy->token = TOK_HALTED;         /* inactive */
    /* high speed; control endpoints take the data toggle from each qTD */
    q->hw->ep_char = (uint32_t)addr | ((uint32_t)(ep & 0x0F) << 8) | (2u << 12) |
                     (control ? (1u << 14) : 0) | ((uint32_t)mps << 16) | (0u << 28);
    q->hw->ep_caps = 1u << 30;           /* Mult = 1 */
    q->hw->next = (uint32_t)(uintptr_t)q->dummy;
    q->hw->alt = LINK_T;
    q->hw->token = 0;
    /* insert right after the head: a single aligned pointer store is atomic */
    q->hw->hlink = e->head->hlink;
    __asm__ volatile("" ::: "memory");
    e->head->hlink = (uint32_t)(uintptr_t)q->hw | LINK_QH;
    return q;
}

static void qh_set_addr(qh_t* q, uint8_t addr) {
    q->hw->ep_char = (q->hw->ep_char & ~0x7Fu) | addr;
}

static void qh_unlink(ehci_t* e, qh_t* q) {
    uint32_t me = (uint32_t)(uintptr_t)q->hw | LINK_QH;
    qh_hw_t* p = e->head;
    for (int guard = 0; guard < 256; guard++) {
        if (p->hlink == me) { p->hlink = q->hw->hlink; break; }
        p = (qh_hw_t*)(uintptr_t)(p->hlink & ~0x1Fu);
        if (p == e->head) break;
    }
    /* let the controller drop any cached copy before the QH is reused */
    wr(e->op, OP_USBCMD, rd(e->op, OP_USBCMD) | CMD_IAAD);
    uint32_t start = timer_ms();
    while (!(rd(e->op, OP_USBSTS) & STS_IAA) && timer_ms() - start < 50) timer_idle();
    wr(e->op, OP_USBSTS, STS_IAA);
}

/* Points a qTD's buffer pages at buf (up to 16 KiB from any alignment,
 * 20 KiB when page aligned). */
static void td_set_buf(qtd_t* t, uint32_t buf) {
    t->buf[0] = buf;
    uint32_t page = buf & ~0xFFFu;
    for (int i = 1; i < 5; i++) t->buf[i] = page + (uint32_t)i * 4096u;
}

/* Queues a chain of n qTDs on q: tds[0] is written into the current dummy.
 * spec[i] = { token (without Active), buffer, length } */
typedef struct { uint32_t token, buf, len; int alt_to_last; } td_spec_t;

static int qh_queue(ehci_t* e, qh_t* q, const td_spec_t* spec, int n, usb_xfer_t* xfer) {
    int slot = -1;
    for (int i = 0; i < MAX_PEND; i++) if (!q->pend[i].first) { slot = i; break; }
    if (slot < 0) return -1;

    qtd_t* tds[4];
    tds[0] = q->dummy;
    for (int i = 1; i < n; i++) tds[i] = (qtd_t*)blk_alloc(e, sizeof(qtd_t));
    qtd_t* nd = (qtd_t*)blk_alloc(e, sizeof(qtd_t));    /* the next dummy */
    if (!nd) return -1;
    nd->next = LINK_T;
    nd->alt = LINK_T;
    nd->token = TOK_HALTED;

    for (int i = 0; i < n; i++) {
        qtd_t* t = tds[i];
        t->next = (uint32_t)(uintptr_t)(i + 1 < n ? tds[i + 1] : nd);
        /* short packet in a control data stage: still run the status stage */
        t->alt = spec[i].alt_to_last ? (uint32_t)(uintptr_t)tds[n - 1] : (uint32_t)(uintptr_t)nd;
        if (spec[i].len) td_set_buf(t, spec[i].buf);
        if (i > 0) t->token = spec[i].token | ((uint32_t)spec[i].len << 16) | TOK_ACTIVE;
    }
    q->dummy = nd;
    q->pend[slot].xfer = xfer;
    q->pend[slot].first = tds[0];
    q->pend[slot].last = tds[n - 1];
    q->pend[slot].ntd = n;
    __asm__ volatile("" ::: "memory");
    /* activating the old dummy hands the whole chain over */
    tds[0]->token = spec[0].token | ((uint32_t)spec[0].len << 16) | TOK_ACTIVE;

    return slot;
}

/* 1 when finished; *status / *actual (bytes moved by all stages) filled in.
 * Finished = the last qTD retired (a short IN data stage jumps straight
 * to it via the alternate pointer), or any qTD stopped with an error. */
static int pend_done(qh_t* q, int slot, const uint32_t* lens, int* status, uint32_t* actual) {
    pending_t* p = &q->pend[slot];
    qtd_t* t = p->first;
    uint32_t got = 0;
    int finished = !(p->last->token & TOK_ACTIVE);
    for (int i = 0; i < p->ntd && t; i++) {
        uint32_t tok = t->token;
        if (!(tok & TOK_ACTIVE)) {
            if (tok & TOK_ERRORS) {
                *status = ((tok & TOK_HALTED) && !(tok & 0x3Cu)) ? USB_XFER_STALL : USB_XFER_ERROR;
                *actual = got;
                q->halted = 1;
                return 1;
            }
            uint32_t remain = (tok >> 16) & 0x7FFF;
            got += lens[i] > remain ? lens[i] - remain : 0;
        }
        t = (i + 1 < p->ntd) ? (qtd_t*)(uintptr_t)(t->next & ~0x1Fu) : NULL;
    }
    if (!finished) return 0;
    *status = USB_XFER_OK;
    *actual = got;
    return 1;
}

static void pend_release(ehci_t* e, qh_t* q, int slot) {
    pending_t* p = &q->pend[slot];
    qtd_t* t = p->first;
    for (int i = 0; i < p->ntd && t; i++) {
        qtd_t* nx = (qtd_t*)(uintptr_t)(t->next & ~0x1Fu);
        td_free(e, t);
        t = nx;
    }
    p->first = p->last = NULL;
    p->xfer = NULL;
}

/* after a stall/error: drop what's queued, restart the queue at the dummy */
static void qh_recover(ehci_t* e, qh_t* q) {
    for (int i = 0; i < MAX_PEND; i++) {
        if (!q->pend[i].first) continue;
        if (q->pend[i].xfer && q->pend[i].xfer->status == USB_XFER_PENDING)
            q->pend[i].xfer->status = USB_XFER_ERROR;
        pend_release(e, q, i);
    }
    q->hw->next = (uint32_t)(uintptr_t)q->dummy;
    q->hw->alt = LINK_T;
    q->hw->token = 0;                    /* clear Halted, toggle back to DATA0 */
    q->halted = 0;
}

/* ── USB core operations ────────────────────────────────────────── */

static int ehci_control(usb_device_t* d, const usb_setup_t* s, void* data, uint32_t timeout_ms) {
    ehci_t* e = (ehci_t*)d->hc->priv;
    edev_t* ed = (edev_t*)d->hcpriv;
    qh_t* q = ed->qh[0];
    uint32_t len = s->wLength;
    int in = (s->bmRequestType & USB_DIR_IN) != 0;
    if (len > 4096) return -1;

    uint8_t* sb = ed->ctrl_buf;          /* setup packet + data */
    if (!sb) return -1;
    memcpy(sb, s, 8);
    uint8_t* db = sb + 64;
    if (!in && len) memcpy(db, data, len);

    td_spec_t spec[3];
    uint32_t lens[3];
    int n = 0;
    spec[n] = (td_spec_t){ TOK_PID_SETUP | TOK_CERR3, (uint32_t)(uintptr_t)sb, 8, 0 };
    lens[n++] = 8;
    if (len) {
        spec[n] = (td_spec_t){ (in ? TOK_PID_IN : TOK_PID_OUT) | TOK_CERR3 | TOK_TOGGLE,
                               (uint32_t)(uintptr_t)db, len, 1 };
        lens[n++] = len;
    }
    spec[n] = (td_spec_t){ ((len && in) ? TOK_PID_OUT : TOK_PID_IN) | TOK_CERR3 | TOK_TOGGLE | TOK_IOC, 0, 0, 0 };
    lens[n++] = 0;

    int slot = qh_queue(e, q, spec, n, NULL);
    if (slot < 0) return -1;
    int status = USB_XFER_ERROR;
    uint32_t actual = 0, start = timer_ms();
    while (!pend_done(q, slot, lens, &status, &actual)) {
        if (timer_ms() - start > timeout_ms) {
            klog("ehci: control request %02x timed out\n", s->bRequest);
            q->halted = 1;
            status = USB_XFER_ERROR;
            break;
        }
        timer_idle();
    }
    pend_release(e, q, slot);
    if (q->halted) qh_recover(e, q);
    int result = -1;
    if (status == USB_XFER_OK) {
        uint32_t data_got = actual >= 8 ? actual - 8 : 0;   /* minus the setup stage */
        if (data_got > len) data_got = len;
        if (in && data_got) memcpy(data, db, data_got);
        result = (int)data_got;
    }
    return result;
}

static int ehci_set_ep0_mps(usb_device_t* d, uint16_t mps) {
    edev_t* ed = (edev_t*)d->hcpriv;
    qh_t* q = ed->qh[0];
    q->hw->ep_char = (q->hw->ep_char & 0xF800FFFFu) | ((uint32_t)mps << 16);
    return 0;
}

static int ehci_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) {
    ehci_t* e = (ehci_t*)d->hc->priv;
    edev_t* ed = (edev_t*)d->hcpriv;
    if ((ep->bmAttributes & 3) != USB_EP_BULK) return -1;   /* interrupt/isoch: not on EHCI (yet) */
    int idx = (ep->bEndpointAddress & 0x0F) * 2 + ((ep->bEndpointAddress & 0x80) ? 1 : 0);
    if (ed->qh[idx]) return 0;
    ed->qh[idx] = qh_new(e, ed->addr, ep->bEndpointAddress, ep->wMaxPacketSize & 0x7FF, 0);
    return ed->qh[idx] ? 0 : -1;
}

static int ehci_submit(usb_xfer_t* x) {
    ehci_t* e = (ehci_t*)x->dev->hc->priv;
    edev_t* ed = (edev_t*)x->dev->hcpriv;
    int in = (x->ep & 0x80) != 0;
    qh_t* q = ed->qh[(x->ep & 0x0F) * 2 + (in ? 1 : 0)];
    if (!q || x->len > 16384) { x->status = USB_XFER_ERROR; return -1; }
    td_spec_t spec = { (in ? TOK_PID_IN : TOK_PID_OUT) | TOK_CERR3 | TOK_IOC,
                       (uint32_t)(uintptr_t)x->buf, x->len, 0 };
    if (qh_queue(e, q, &spec, 1, x) < 0) { x->status = USB_XFER_ERROR; return -1; }
    return 0;
}

static void ehci_poll(usb_hc_t* hc) {
    ehci_t* e = (ehci_t*)hc->priv;
    uint32_t sts = rd(e->op, OP_USBSTS);
    if (sts & (1u << 2)) {                          /* port change detect */
        wr(e->op, OP_USBSTS, 1u << 2);
        hc->port_change = 1;
    }
    for (int p = 1; p <= e->ports; p++) {
        edev_t* ed = e->port_dev[p];
        if (!ed) continue;
        for (int i = 1; i < 32; i++) {
            qh_t* q = ed->qh[i];
            if (!q) continue;
            for (int s = 0; s < MAX_PEND; s++) {
                pending_t* pe = &q->pend[s];
                if (!pe->first || !pe->xfer) continue;
                uint32_t lens[1] = { pe->xfer->len };
                int status;
                uint32_t actual;
                if (!pend_done(q, s, lens, &status, &actual)) continue;
                usb_xfer_t* x = pe->xfer;
                pend_release(e, q, s);
                x->actual = actual;
                x->status = status;
            }
            if (q->halted) {
                qh_recover(e, q);
                /* clear the halt on the device side too */
                usb_setup_t cf = { USB_RECIP_EP, USB_REQ_CLEAR_FEATURE, 0, q->ep, 0 };
                ehci_control(ed->udev, &cf, NULL, 500);
            }
        }
    }
}

/* ── ports ──────────────────────────────────────────────────────── */

static uint32_t portsc(ehci_t* e, int p) { return rd(e->op, OP_PORTSC(p)); }
static void port_write(ehci_t* e, int p, uint32_t v) { wr(e->op, OP_PORTSC(p), v); }

static void attach_port(ehci_t* e, int p) {
    uint32_t v = portsc(e, p);
    if (!(v & PORT_CCS)) return;
    if ((v & PORT_LS_MASK) == PORT_LS_K) {
        /* low-speed: belongs to the companion controller */
        port_write(e, p, (v & ~PORT_RWC) | PORT_OWNER);
        return;
    }
    /* reset: hold 50 ms, release, and see whether the device came up as
     * high speed (EHCI enables the port) - otherwise it's full speed */
    port_write(e, p, (v & ~(PORT_RWC | PORT_PED)) | PORT_PR);
    usb_delay_ms(50);
    port_write(e, p, portsc(e, p) & ~(PORT_RWC | PORT_PR));
    uint32_t start = timer_ms();
    while ((portsc(e, p) & PORT_PR) && timer_ms() - start < 10) timer_idle();
    usb_delay_ms(10);
    v = portsc(e, p);
    if (!(v & PORT_PED)) {
        klog("ehci: port %d: full/low-speed device, handed to the companion controller\n", p);
        port_write(e, p, (v & ~PORT_RWC) | PORT_OWNER);
        return;
    }

    edev_t* ed = (edev_t*)kzalloc(sizeof(edev_t));
    if (!ed) return;
    ed->port = (uint8_t)p;
    ed->qh[0] = qh_new(e, 0, 0, 64, 1);
    ed->ctrl_buf = (uint8_t*)usb_dma_alloc(8192);
    if (!ed->ctrl_buf) return;
    if (!ed->qh[0]) return;
    e->port_dev[p] = ed;

    /* SET_ADDRESS through address 0, then retarget the control QH */
    uint8_t addr = e->next_addr++;
    if (e->next_addr > 127) e->next_addr = 1;
    usb_device_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.hc = &e->hc;
    tmp.hcpriv = ed;
    tmp.present = 1;
    usb_setup_t sa = { USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, addr, 0, 0 };
    if (ehci_control(&tmp, &sa, NULL, 1000) < 0) {
        klog("ehci: port %d: SET_ADDRESS failed\n", p);
        qh_unlink(e, ed->qh[0]);
        e->port_dev[p] = NULL;
        return;
    }
    usb_delay_ms(5);
    ed->addr = addr;
    qh_set_addr(ed->qh[0], addr);
    ed->udev = usb_new_device(&e->hc, p, USB_SPEED_HIGH, addr, 64, ed);
}

static void detach_port(ehci_t* e, int p) {
    edev_t* ed = e->port_dev[p];
    if (!ed) return;
    e->port_dev[p] = NULL;
    for (int i = 0; i < 32; i++) {
        qh_t* q = ed->qh[i];
        if (!q) continue;
        qh_unlink(e, q);
        for (int s = 0; s < MAX_PEND; s++)
            if (q->pend[s].xfer) q->pend[s].xfer->status = USB_XFER_GONE;
    }
    if (ed->udev) usb_device_gone(ed->udev);
}

static void ehci_rescan(usb_hc_t* hc) {
    ehci_t* e = (ehci_t*)hc->priv;
    for (int p = 1; p <= e->ports; p++) {
        uint32_t v = portsc(e, p);
        if (v & PORT_RWC) port_write(e, p, v & ~PORT_PED);   /* ack change bits */
        if (v & PORT_OWNER) continue;                        /* companion's */
        int connected = (v & PORT_CCS) != 0;
        if (connected && !e->port_dev[p]) attach_port(e, p);
        else if (!connected && e->port_dev[p]) detach_port(e, p);
    }
}

static const usb_hc_ops_t g_ops = {
    ehci_control, ehci_set_ep0_mps, ehci_open_endpoint, ehci_submit, ehci_poll, ehci_rescan,
};

static void ehci_irq(void) {
    for (int i = 0; i < g_ehci_count; i++) {
        ehci_t* e = g_ehci[i];
        uint32_t sts = rd(e->op, OP_USBSTS) & 0x3F;
        if (!sts) continue;
        /* ack everything (a level-triggered line stays asserted otherwise);
         * port changes are remembered for the next usb_poll() */
        if (sts & (1u << 2)) e->hc.port_change = 1;
        wr(e->op, OP_USBSTS, sts);
        net_irq_notify();
    }
}

/* ── bring-up ───────────────────────────────────────────────────── */

static void bios_handoff(ehci_t* e, const pci_dev_t* pd) {
    uint32_t eecp = (rd(e->cap, 0x08) >> 8) & 0xFF;
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        uint32_t v = pci_read32(pd->bus, pd->dev, pd->fn, (uint8_t)eecp);
        if ((v & 0xFF) == 1) {                       /* USBLEGSUP */
            pci_write32(pd->bus, pd->dev, pd->fn, (uint8_t)eecp, v | (1u << 24));
            uint32_t start = timer_ms();
            while ((pci_read32(pd->bus, pd->dev, pd->fn, (uint8_t)eecp) & (1u << 16)) &&
                   timer_ms() - start < 1000)
                timer_idle();
            pci_write32(pd->bus, pd->dev, pd->fn, (uint8_t)(eecp + 4), 0);   /* no SMIs */
        }
        eecp = (v >> 8) & 0xFF;
    }
}

int ehci_init_controller(const pci_dev_t* pd) {
    if (g_ehci_count >= MAX_EHCI) return -1;
    int is_io = 0;
    uint32_t bar = pci_bar(pd, 0, &is_io);
    if (is_io || !bar) return -1;
    pci_enable(pd);

    ehci_t* e = (ehci_t*)kzalloc(sizeof(ehci_t));
    if (!e) return -1;
    e->cap = (volatile uint8_t*)(uintptr_t)bar;
    e->op = e->cap + (rd(e->cap, 0) & 0xFF);
    e->ports = (int)(rd(e->cap, 0x04) & 0x0F);
    if (e->ports > 15) e->ports = 15;
    e->next_addr = 1;

    bios_handoff(e, pd);

    wr(e->op, OP_USBCMD, rd(e->op, OP_USBCMD) & ~CMD_RS);
    uint32_t start = timer_ms();
    while (!(rd(e->op, OP_USBSTS) & STS_HALTED) && timer_ms() - start < 50) timer_idle();
    wr(e->op, OP_USBCMD, CMD_RESET);
    start = timer_ms();
    while ((rd(e->op, OP_USBCMD) & CMD_RESET) && timer_ms() - start < 250) timer_idle();
    if (rd(e->op, OP_USBCMD) & CMD_RESET) { klog("ehci: reset failed\n"); kfree(e); return -1; }

    /* the async ring starts as one "reclamation head" QH linked to itself */
    e->head = (qh_hw_t*)blk_alloc(e, sizeof(qh_hw_t));
    if (!e->head) return -1;
    e->head->hlink = (uint32_t)(uintptr_t)e->head | LINK_QH;
    e->head->ep_char = (1u << 15) | (2u << 12);      /* H bit, high speed */
    e->head->ep_caps = 1u << 30;
    e->head->next = LINK_T;
    e->head->alt = LINK_T;
    e->head->token = TOK_HALTED;

    wr(e->op, OP_CTRLDSSEG, 0);
    wr(e->op, OP_ASYNC, (uint32_t)(uintptr_t)e->head);
    wr(e->op, OP_USBINTR, 0x07);                     /* transfer done, error, port change */
    wr(e->op, OP_USBCMD, CMD_RS | CMD_ASE | (8u << 16));   /* 1 ms interrupt threshold */
    wr(e->op, OP_CONFIG, 1);                         /* route every port to EHCI */
    usb_delay_ms(5);

    e->hc.name = "ehci";
    e->hc.ops = &g_ops;
    e->hc.priv = e;
    e->hc.ports = e->ports;
    ksnprintf(e->hc.desc, sizeof(e->hc.desc), "EHCI (USB 2.0), %d ports (PCI %02x:%02x.%x, %04x:%04x)",
              e->ports, pd->bus, pd->dev, pd->fn, pd->vendor, pd->device);
    g_ehci[g_ehci_count++] = e;
    usb_register_hc(&e->hc);
    if (pd->irq_line > 0 && pd->irq_line < 16) irq_install(pd->irq_line, ehci_irq);
    klog("ehci: %s\n", e->hc.desc);

    /* power the ports (if software-controlled), give devices time to connect */
    if (rd(e->cap, 0x04) & (1u << 4))
        for (int p = 1; p <= e->ports; p++) port_write(e, p, (portsc(e, p) & ~PORT_RWC) | PORT_PP);
    usb_delay_ms(100);
    ehci_rescan(&e->hc);
    e->hc.port_change = 0;
    return 0;
}
