#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

/*
 * AMD PCnet driver: Am79C970A (PCnet-PCI II) and Am79C973 (PCnet-FAST
 * III) - VirtualBox's AMD network cards (PCnet-FAST III is its default
 * for many guest types), and QEMU's -nic user,model=pcnet.
 *
 * Programmed through I/O ports in 32-bit mode (DWIO): RAP selects a
 * register, RDP reads/writes a CSR, BDP a BCR. Software style 2: 32-bit
 * initialization block and 16-byte descriptors. Rings of receive and
 * transmit descriptors, each pointing at its own 1.5 KB buffer; OWN in a
 * descriptor's status says whether the card or the driver has it.
 */

#define P_APROM 0x00      /* the MAC address, bytes 0-5 */
#define P_RDP   0x10
#define P_RAP   0x14
#define P_RESET 0x18
#define P_BDP   0x1C
#define W_RAP   0x12      /* the same, before the switch to 32-bit mode */
#define W_RESET 0x14

#define CSR0_INIT 0x0001
#define CSR0_STRT 0x0002
#define CSR0_STOP 0x0004
#define CSR0_TDMD 0x0008
#define CSR0_IENA 0x0040
#define CSR0_IDON 0x0100
#define CSR0_TINT 0x0200
#define CSR0_RINT 0x0400
#define CSR0_ACKS (CSR0_IDON | CSR0_TINT | CSR0_RINT | 0x0800 /*MERR*/ | 0x1000 /*MISS*/ | 0x2000 /*CERR*/ | 0x4000 /*BABL*/)

#define D_OWN 0x80000000u
#define D_ERR 0x40000000u
#define D_STP 0x02000000u
#define D_ENP 0x01000000u

#define RX_COUNT 16       /* (powers of two: the init block takes log2) */
#define RX_LOG2  4
#define TX_COUNT 8
#define TX_LOG2  3
#define BUF_SIZE 1536

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t flags;       /* OWN ERR ... STP ENP | 1111 | -byte count (12 bits) */
    uint32_t misc;        /* receive: message length (bits 0-11, with the CRC) */
    uint32_t user;
} pcnet_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint8_t  rlen;        /* log2(receive ring) << 4 */
    uint8_t  tlen;        /* log2(transmit ring) << 4 */
    uint8_t  mac[6];
    uint16_t reserved;
    uint8_t  ladr[8];     /* multicast filter */
    uint32_t rdra;        /* rings' physical addresses */
    uint32_t tdra;
} pcnet_init_t;

/* (the kernel image is identity mapped below 4 GB: these addresses are physical) */
static pcnet_desc_t g_rx[RX_COUNT] __attribute__((aligned(16)));
static pcnet_desc_t g_tx[TX_COUNT] __attribute__((aligned(16)));
static uint8_t      g_rxbuf[RX_COUNT][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t      g_txbuf[TX_COUNT][BUF_SIZE] __attribute__((aligned(16)));
static pcnet_init_t g_init __attribute__((aligned(16)));

static uint16_t g_io;
static int      g_rx_cur, g_tx_cur;
static netdev_t g_nd;

static void     wr_rap(uint32_t r)              { outl((uint16_t)(g_io + P_RAP), r); }
static uint32_t rd_csr(uint32_t r)              { wr_rap(r); return inl((uint16_t)(g_io + P_RDP)) & 0xFFFF; }
static void     wr_csr(uint32_t r, uint32_t v)  { wr_rap(r); outl((uint16_t)(g_io + P_RDP), v); }
static void     wr_bcr(uint32_t r, uint32_t v)  { wr_rap(r); outl((uint16_t)(g_io + P_BDP), v); }

/* a byte count as the descriptor wants it: two's complement, top four bits set */
static uint32_t bcnt(uint32_t n) { return (0u - n) & 0x0FFFu; }

static void give_rx(int i) {
    g_rx[i].misc = 0;
    g_rx[i].flags = D_OWN | 0xF000u | bcnt(BUF_SIZE);
}

static void pcnet_irq(void) {
    uint32_t rap = inl((uint16_t)(g_io + P_RAP));
    uint32_t csr0 = rd_csr(0);
    if (csr0 & CSR0_ACKS) wr_csr(0, (csr0 & CSR0_ACKS) | CSR0_IENA);    /* write-1-to-clear */
    wr_rap(rap);
    if (csr0 & (CSR0_RINT | CSR0_TINT)) net_irq_notify();
}

static int pcnet_send(netdev_t* nd, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) { nd->tx_errors++; return -1; }
    pcnet_desc_t* d = &g_tx[g_tx_cur];
    /* the card still owns this slot: wait for it to finish the frame before */
    for (int i = 0; d->flags & D_OWN; i++) {
        if (i > 2000000) { nd->tx_errors++; return -1; }
    }
    if (d->flags & D_ERR) nd->tx_errors++;
    uint32_t n = len < 60 ? 60 : len;              /* the shortest Ethernet frame (without CRC) */
    memcpy(g_txbuf[g_tx_cur], frame, len);
    if (n > len) memset(g_txbuf[g_tx_cur] + len, 0, n - len);
    d->addr = (uint32_t)(uintptr_t)g_txbuf[g_tx_cur];
    d->misc = 0;
    __asm__ volatile("" ::: "memory");
    d->flags = D_OWN | D_STP | D_ENP | 0xF000u | bcnt(n);   /* one buffer per frame; the card adds the CRC */
    g_tx_cur = (g_tx_cur + 1) % TX_COUNT;
    uint32_t rap = inl((uint16_t)(g_io + P_RAP));
    wr_csr(0, CSR0_TDMD | CSR0_IENA);              /* "look at the transmit ring now" */
    wr_rap(rap);
    nd->tx_packets++;
    nd->tx_bytes += n;
    return 0;
}

static int pcnet_poll(netdev_t* nd) {
    int count = 0;
    while (!(g_rx[g_rx_cur].flags & D_OWN)) {
        pcnet_desc_t* d = &g_rx[g_rx_cur];
        uint32_t f = d->flags;
        uint32_t len = d->misc & 0x0FFFu;          /* with the 4-byte CRC */
        if ((f & D_ERR) || !(f & D_STP) || !(f & D_ENP) || len < 18 || len > BUF_SIZE) {
            nd->rx_errors++;
        } else {
            nd->rx_packets++;
            nd->rx_bytes += len - 4u;
            net_rx(nd, g_rxbuf[g_rx_cur], len - 4u);
        }
        give_rx(g_rx_cur);
        g_rx_cur = (g_rx_cur + 1) % RX_COUNT;
        if (++count >= 64) break;
    }
    return count;
}

static int pcnet_link_up(netdev_t* nd) {
    (void)nd;
    return 1;                                       /* (virtual cards always have a link) */
}

netdev_t* pcnet_probe(void) {
    pci_dev_t pd;
    if (!pci_find(0x1022, 0x2000, &pd) && !pci_find(0x1022, 0x2001, &pd)) return NULL;

    int is_io = 0;
    uintptr_t bar0 = pci_bar(&pd, 0, &is_io);
    if (!is_io || !bar0) return NULL;
    pci_enable(&pd);
    g_io = (uint16_t)bar0;

    /* reset (a read of the reset register, 16- and 32-bit forms), then
     * 32-bit I/O mode: a 32-bit write to RDP switches it */
    (void)inw((uint16_t)(g_io + W_RESET));
    (void)inl((uint16_t)(g_io + P_RESET));
    for (volatile int i = 0; i < 100000; i++) {}
    /* the MAC address, while byte reads of the address PROM still work (16-bit mode) */
    memset(&g_nd, 0, sizeof(g_nd));
    for (int i = 0; i < 6; i++) g_nd.mac[i] = inb((uint16_t)(g_io + P_APROM + i));
    outw((uint16_t)(g_io + W_RAP), 0);
    outl((uint16_t)(g_io + P_RDP), 0);
    wr_csr(0, CSR0_STOP);

    /* which chip: CSR88/89 hold its part number */
    uint32_t part = ((rd_csr(89) << 16) | rd_csr(88)) >> 12 & 0xFFFF;
    const char* model = part == 0x2625 ? "AMD PCnet-FAST III (Am79C973)"
                      : part == 0x2621 ? "AMD PCnet-PCI II (Am79C970A)"
                      : part == 0x2627 ? "AMD PCnet-FAST III (Am79C975)" : "AMD PCnet";

    wr_bcr(20, 2);                                   /* software style 2: 32-bit structures */
    wr_csr(4, rd_csr(4) | 0x0800u);                  /* APAD_XMT: pad short frames */

    for (int i = 0; i < RX_COUNT; i++) { g_rx[i].addr = (uint32_t)(uintptr_t)g_rxbuf[i]; g_rx[i].user = 0; give_rx(i); }
    for (int i = 0; i < TX_COUNT; i++) { memset(&g_tx[i], 0, sizeof(g_tx[i])); }
    g_rx_cur = g_tx_cur = 0;

    memset(&g_init, 0, sizeof(g_init));
    g_init.mode = 0;                                 /* normal: our address, broadcast */
    g_init.rlen = RX_LOG2 << 4;
    g_init.tlen = TX_LOG2 << 4;
    memcpy(g_init.mac, g_nd.mac, 6);
    memset(g_init.ladr, 0xFF, 8);                    /* every multicast (mDNS, IPv6 ND...) */
    g_init.rdra = (uint32_t)(uintptr_t)g_rx;
    g_init.tdra = (uint32_t)(uintptr_t)g_tx;
    uint32_t ib = (uint32_t)(uintptr_t)&g_init;
    wr_csr(1, ib & 0xFFFF);
    wr_csr(2, ib >> 16);

    wr_csr(0, CSR0_INIT);
    int ok = 0;
    for (int i = 0; i < 1000000; i++) if (rd_csr(0) & CSR0_IDON) { ok = 1; break; }
    if (!ok) { klog("pcnet: initialization did not complete\n"); return NULL; }
    wr_csr(0, CSR0_IDON | CSR0_STRT | CSR0_IENA);   /* (IDON written back clears it) */
    wr_rap(0);

    g_nd.name = "pcnet";
    g_nd.model = model;
    g_nd.send = pcnet_send;
    g_nd.poll = pcnet_poll;
    g_nd.link_up = pcnet_link_up;
    g_nd.irq = pd.irq_line;
    if (pd.irq_line > 0 && pd.irq_line < 16) irq_install(pd.irq_line, pcnet_irq);
    else g_nd.irq = 0xFF;

    klog("pcnet: %s at io 0x%x irq %u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         model, g_io, pd.irq_line, g_nd.mac[0], g_nd.mac[1], g_nd.mac[2],
         g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
