#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

/*
 * virtio-net driver, legacy interface: VirtualBox's "Paravirtualized
 * Network (virtio-net)" and QEMU's -nic user,model=virtio-net-pci.
 *
 * The device's registers are an I/O port block (BAR0); frames go through
 * two virtqueues - 0 receive, 1 transmit - rings in memory shared with the
 * device: a descriptor table (buffer address + length), the "available"
 * ring (buffers handed to the device) and the "used" ring (buffers it is
 * done with). Every frame is preceded by a 10-byte virtio-net header.
 */

#define VIO_DEV_FEATURES   0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_SIZE     0x0C
#define VIO_QUEUE_SEL      0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_NET_MAC        0x14      /* device config (no MSI-X): the MAC address */

#define ST_ACK      1
#define ST_DRIVER   2
#define ST_DRIVER_OK 4
#define ST_FAILED   128

#define F_NET_MAC   (1u << 5)

#define D_NEXT  1
#define D_WRITE 2                     /* the device writes into this buffer */

#define QMAX     1024                 /* the largest queue the static rings can hold */
#define QBYTES   (32u * 1024u)        /* a 1024-entry ring: 16K descriptors, avail, used (page aligned) */
#define HDR      10                   /* struct virtio_net_hdr (legacy, no merged buffers) */
#define BUF_SIZE (HDR + 1514 + 2)
#define RX_BUFS  64
#define TX_BUFS  16

typedef struct __attribute__((packed)) { uint64_t addr; uint32_t len; uint16_t flags, next; } vdesc_t;
typedef struct __attribute__((packed)) { uint32_t id, len; } vused_elem_t;

typedef struct {
    vdesc_t*            desc;
    volatile uint16_t*  avail;        /* flags, idx, ring[size], used_event */
    volatile uint8_t*   used;         /* flags, idx, ring[size] of vused_elem_t */
    uint16_t            size, last_used, avail_idx;
} vq_t;

/* (the kernel image is identity mapped below 4 GB: these addresses are physical) */
static uint8_t g_ring[2][QBYTES] __attribute__((aligned(4096)));
static uint8_t g_rxbuf[RX_BUFS][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_txbuf[TX_BUFS][BUF_SIZE] __attribute__((aligned(16)));

static uint16_t g_io;
static vq_t     g_q[2];
static int      g_tx_next;
static netdev_t g_nd;

static uint16_t used_idx(vq_t* q) { return *(volatile uint16_t*)(q->used + 2); }
static vused_elem_t used_elem(vq_t* q, uint16_t i) {
    return *(volatile vused_elem_t*)(q->used + 4 + (uint32_t)(i % q->size) * 8u);
}

/* sets up virtqueue n in its static ring; 0 if the device's size is unusable */
static int setup_queue(int n) {
    vq_t* q = &g_q[n];
    outw((uint16_t)(g_io + VIO_QUEUE_SEL), (uint16_t)n);
    uint16_t size = inw((uint16_t)(g_io + VIO_QUEUE_SIZE));
    if (size == 0 || size > QMAX) return 0;
    memset(g_ring[n], 0, QBYTES);
    q->size = size;
    q->desc = (vdesc_t*)g_ring[n];
    q->avail = (volatile uint16_t*)(g_ring[n] + 16u * size);
    uint32_t used_off = (16u * size + 6u + 2u * size + 4095u) & ~4095u;
    q->used = g_ring[n] + used_off;
    q->last_used = q->avail_idx = 0;
    outl((uint16_t)(g_io + VIO_QUEUE_PFN), (uint32_t)((uintptr_t)g_ring[n] >> 12));
    return 1;
}

/* hands descriptor d to the device (into the available ring) */
static void offer(vq_t* q, uint16_t d) {
    q->avail[2 + (q->avail_idx % q->size)] = d;
    __asm__ volatile("" ::: "memory");
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
}

static void vio_irq(void) {
    uint8_t isr = inb((uint16_t)(g_io + VIO_ISR));      /* reading it clears it */
    if (isr & 1) net_irq_notify();
}

static int vio_send(netdev_t* nd, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) { nd->tx_errors++; return -1; }
    vq_t* q = &g_q[1];
    /* frames the device has sent free their buffers */
    for (int i = 0; (uint16_t)(q->avail_idx - used_idx(q)) >= TX_BUFS; i++) {
        if (i > 2000000) { nd->tx_errors++; return -1; }
    }
    q->last_used = used_idx(q);
    int slot = g_tx_next;
    g_tx_next = (g_tx_next + 1) % TX_BUFS;
    memset(g_txbuf[slot], 0, HDR);                       /* no checksum offload, no GSO */
    memcpy(g_txbuf[slot] + HDR, frame, len);
    q->desc[slot].addr = (uint64_t)(uintptr_t)g_txbuf[slot];
    q->desc[slot].len = HDR + len;
    q->desc[slot].flags = 0;
    q->desc[slot].next = 0;
    offer(q, (uint16_t)slot);
    __asm__ volatile("" ::: "memory");
    outw((uint16_t)(g_io + VIO_QUEUE_NOTIFY), 1);
    nd->tx_packets++;
    nd->tx_bytes += len;
    return 0;
}

static int vio_poll(netdev_t* nd) {
    vq_t* q = &g_q[0];
    int count = 0;
    while (q->last_used != used_idx(q)) {
        vused_elem_t e = used_elem(q, q->last_used);
        q->last_used++;
        if (e.id < RX_BUFS && e.len > HDR && e.len <= BUF_SIZE) {
            nd->rx_packets++;
            nd->rx_bytes += e.len - HDR;
            net_rx(nd, g_rxbuf[e.id] + HDR, e.len - HDR);
        } else {
            nd->rx_errors++;
        }
        if (e.id < RX_BUFS) offer(q, (uint16_t)e.id);    /* the buffer goes back to the device */
        if (++count >= 64) break;
    }
    if (count) outw((uint16_t)(g_io + VIO_QUEUE_NOTIFY), 0);
    return count;
}

static int vio_link_up(netdev_t* nd) { (void)nd; return 1; }

netdev_t* virtio_net_probe(void) {
    pci_dev_t pd;
    if (!pci_find(0x1AF4, 0x1000, &pd)) return NULL;     /* (transitional: has the legacy interface) */

    int is_io = 0;
    uintptr_t bar0 = pci_bar(&pd, 0, &is_io);
    if (!is_io || !bar0) return NULL;
    pci_enable(&pd);
    g_io = (uint16_t)bar0;

    outb((uint16_t)(g_io + VIO_STATUS), 0);              /* reset */
    outb((uint16_t)(g_io + VIO_STATUS), ST_ACK);
    outb((uint16_t)(g_io + VIO_STATUS), ST_ACK | ST_DRIVER);
    uint32_t feat = inl((uint16_t)(g_io + VIO_DEV_FEATURES));
    outl((uint16_t)(g_io + VIO_GUEST_FEATURES), feat & F_NET_MAC);

    memset(&g_nd, 0, sizeof(g_nd));
    if (feat & F_NET_MAC) {
        for (int i = 0; i < 6; i++) g_nd.mac[i] = inb((uint16_t)(g_io + VIO_NET_MAC + i));
    } else {
        static const uint8_t def[6] = { 0x02, 0x42, 0x41, 0x4E, 0x41, 0x4E };   /* locally administered */
        memcpy(g_nd.mac, def, 6);
    }

    if (!setup_queue(0) || !setup_queue(1)) {
        outb((uint16_t)(g_io + VIO_STATUS), ST_FAILED);
        klog("virtio-net: unusable queue size\n");
        return NULL;
    }
    /* every receive buffer to the device */
    vq_t* rq = &g_q[0];
    int nrx = RX_BUFS < rq->size ? RX_BUFS : rq->size;
    for (int i = 0; i < nrx; i++) {
        rq->desc[i].addr = (uint64_t)(uintptr_t)g_rxbuf[i];
        rq->desc[i].len = BUF_SIZE;
        rq->desc[i].flags = D_WRITE;
        offer(rq, (uint16_t)i);
    }
    g_tx_next = 0;
    outb((uint16_t)(g_io + VIO_STATUS), ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    outw((uint16_t)(g_io + VIO_QUEUE_NOTIFY), 0);

    g_nd.name = "virtio-net";
    g_nd.model = "virtio-net (paravirtualized)";
    g_nd.send = vio_send;
    g_nd.poll = vio_poll;
    g_nd.link_up = vio_link_up;
    g_nd.irq = pd.irq_line;
    if (pd.irq_line > 0 && pd.irq_line < 16) irq_install(pd.irq_line, vio_irq);
    else g_nd.irq = 0xFF;

    klog("virtio-net: io 0x%x irq %u queues %u/%u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         g_io, pd.irq_line, g_q[0].size, g_q[1].size, g_nd.mac[0], g_nd.mac[1], g_nd.mac[2],
         g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
