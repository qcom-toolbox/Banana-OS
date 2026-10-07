#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

/*
 * Realtek RTL8111/8168 (PCI Express gigabit Ethernet - on most PC
 * mainboards), RTL8169 (PCI gigabit) and RTL8101/8102 (PCI Express fast
 * Ethernet): the "C+" family, one driver for all of them.
 *
 * Registers through the I/O BAR (BAR 0), or the memory BAR (BAR 2) when
 * there is no I/O one. A ring of receive and one of transmit descriptors
 * (16 bytes, 256-byte aligned rings), each pointing at a 2 KiB buffer;
 * OWN in a descriptor says whether the card or the driver has it, EOR
 * marks the ring's last one. The PHY is told to negotiate the link
 * (it also comes out of power-down that way); the link state is PHYstatus.
 */

#define R_IDR0      0x00      /* the MAC address, 6 bytes */
#define R_MAR0      0x08      /* multicast filter, 8 bytes */
#define R_TNPDS     0x20      /* transmit descriptors (normal priority), 64-bit */
#define R_CMD       0x37
#define R_TPPOLL    0x38
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_9346CR    0x50
#define R_PHYAR     0x60
#define R_PHYSTATUS 0x6C
#define R_RMS       0xDA      /* the biggest frame received */
#define R_CPCR      0xE0
#define R_RDSAR     0xE4      /* receive descriptors, 64-bit */
#define R_MTPS      0xEC      /* the biggest frame sent (x128 bytes) */

#define CMD_RST 0x10
#define CMD_RE  0x08
#define CMD_TE  0x04

#define D_OWN 0x80000000u
#define D_EOR 0x40000000u
#define D_FS  0x20000000u
#define D_LS  0x10000000u
#define RX_RES 0x00200000u    /* receive error summary */

#define RX_COUNT 32
#define TX_COUNT 16
#define BUF_SIZE 2048

typedef struct __attribute__((packed)) {
    uint32_t opts1;           /* OWN EOR FS LS ... | length */
    uint32_t opts2;           /* VLAN */
    uint32_t addr_lo, addr_hi;
} rtl_desc_t;

/* (the kernel image is identity mapped below 4 GB: these addresses are physical) */
static rtl_desc_t g_rx[RX_COUNT] __attribute__((aligned(256)));
static rtl_desc_t g_tx[TX_COUNT] __attribute__((aligned(256)));
static uint8_t    g_rxbuf[RX_COUNT][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t    g_txbuf[TX_COUNT][BUF_SIZE] __attribute__((aligned(16)));

static uint16_t          g_io;
static volatile uint8_t* g_mmio;
static int               g_rx_cur, g_tx_cur;
static netdev_t          g_nd;

static uint8_t  r8(uint32_t r)  { return g_mmio ? g_mmio[r] : inb((uint16_t)(g_io + r)); }
static uint16_t r16(uint32_t r) { return g_mmio ? *(volatile uint16_t*)(g_mmio + r) : inw((uint16_t)(g_io + r)); }
static uint32_t r32(uint32_t r) { return g_mmio ? *(volatile uint32_t*)(g_mmio + r) : inl((uint16_t)(g_io + r)); }
static void w8(uint32_t r, uint8_t v)   { if (g_mmio) g_mmio[r] = v; else outb((uint16_t)(g_io + r), v); }
static void w16(uint32_t r, uint16_t v) { if (g_mmio) *(volatile uint16_t*)(g_mmio + r) = v; else outw((uint16_t)(g_io + r), v); }
static void w32(uint32_t r, uint32_t v) { if (g_mmio) *(volatile uint32_t*)(g_mmio + r) = v; else outl((uint16_t)(g_io + r), v); }

/* a PHY register (MII): written through PHYAR, which clears its top bit when done */
static void phy_write(int reg, uint16_t v) {
    w32(R_PHYAR, 0x80000000u | ((uint32_t)(reg & 0x1F) << 16) | v);
    for (int i = 0; i < 2000 && (r32(R_PHYAR) & 0x80000000u); i++) io_wait();
}

static void give_rx(int i) {
    g_rx[i].opts2 = 0;
    __asm__ volatile("" ::: "memory");
    g_rx[i].opts1 = D_OWN | (i == RX_COUNT - 1 ? D_EOR : 0) | BUF_SIZE;
}

static void rtl_irq(void) {
    uint16_t s = r16(R_ISR);
    if (!s || s == 0xFFFF) return;              /* not ours (the line is shared) */
    w16(R_ISR, s);                              /* write-1-to-clear */
    net_irq_notify();
}

static int rtl_send(netdev_t* nd, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) { nd->tx_errors++; return -1; }
    rtl_desc_t* d = &g_tx[g_tx_cur];
    /* the card still owns this slot: wait for it to finish the frame before */
    for (int i = 0; d->opts1 & D_OWN; i++) {
        if (i > 2000000) { nd->tx_errors++; return -1; }
    }
    uint32_t n = len < 60 ? 60 : len;           /* the shortest Ethernet frame (without CRC) */
    memcpy(g_txbuf[g_tx_cur], frame, len);
    if (n > len) memset(g_txbuf[g_tx_cur] + len, 0, n - len);
    d->addr_lo = (uint32_t)(uintptr_t)g_txbuf[g_tx_cur];
    d->addr_hi = 0;
    d->opts2 = 0;
    __asm__ volatile("" ::: "memory");
    d->opts1 = D_OWN | D_FS | D_LS | (g_tx_cur == TX_COUNT - 1 ? D_EOR : 0) | n;   /* the card adds the CRC */
    g_tx_cur = (g_tx_cur + 1) % TX_COUNT;
    w8(R_TPPOLL, 0x40);                         /* "look at the normal-priority ring now" */
    nd->tx_packets++;
    nd->tx_bytes += n;
    return 0;
}

static int rtl_poll(netdev_t* nd) {
    int count = 0;
    while (!(g_rx[g_rx_cur].opts1 & D_OWN)) {
        uint32_t f = g_rx[g_rx_cur].opts1;
        uint32_t len = f & 0x3FFFu;             /* with the 4-byte CRC */
        if ((f & RX_RES) || !(f & D_FS) || !(f & D_LS) || len < 18 || len > BUF_SIZE) {
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
    if (nd->irq == 0xFF) {                      /* no interrupt line: the status is cleared here */
        uint16_t s = r16(R_ISR);
        if (s && s != 0xFFFF) w16(R_ISR, s);
    }
    return count;
}

static int rtl_link_up(netdev_t* nd) {
    (void)nd;
    return (r8(R_PHYSTATUS) & 0x02) != 0;
}

static const struct { uint16_t vendor, device; const char* model; } g_ids[] = {
    { 0x10EC, 0x8168, "Realtek RTL8111/8168 Gigabit Ethernet" },
    { 0x10EC, 0x8161, "Realtek RTL8111/8168 Gigabit Ethernet" },
    { 0x10EC, 0x8169, "Realtek RTL8169 Gigabit Ethernet" },
    { 0x10EC, 0x8167, "Realtek RTL8169SC Gigabit Ethernet" },
    { 0x10EC, 0x8136, "Realtek RTL8101/8102 Fast Ethernet" },
    { 0x1186, 0x4300, "D-Link DGE-528T (RTL8169)" },
    { 0x1186, 0x4302, "D-Link DGE-530T (RTL8169)" },
};

netdev_t* r8169_probe(void) {
    pci_dev_t pd;
    const char* model = NULL;
    for (uint32_t i = 0; i < sizeof(g_ids) / sizeof(g_ids[0]) && !model; i++)
        if (pci_find(g_ids[i].vendor, g_ids[i].device, &pd)) model = g_ids[i].model;
    if (!model) return NULL;

    int is_io = 0;
    uintptr_t bar0 = pci_bar(&pd, 0, &is_io);
    g_mmio = NULL;
    if (is_io && bar0) {
        g_io = (uint16_t)bar0;
    } else {
        int io2 = 0;
        uintptr_t bar2 = pci_bar(&pd, 2, &io2);
        if (io2 || !bar2) { klog("r8169: no usable register window\n"); return NULL; }
        g_mmio = (volatile uint8_t*)bar2;
    }
    pci_enable(&pd);

    /* reset: the command register's RST bit clears itself when done */
    w8(R_CMD, CMD_RST);
    int ok = 0;
    for (int i = 0; i < 100000; i++) if (!(r8(R_CMD) & CMD_RST)) { ok = 1; break; }
    if (!ok) { klog("r8169: the reset does not complete\n"); return NULL; }

    memset(&g_nd, 0, sizeof(g_nd));
    for (int i = 0; i < 6; i++) g_nd.mac[i] = r8(R_IDR0 + (uint32_t)i);
    uint32_t xid = (r32(R_TCR) >> 20) & 0x7CFu;  /* which revision of the chip */

    for (int i = 0; i < RX_COUNT; i++) {
        g_rx[i].addr_lo = (uint32_t)(uintptr_t)g_rxbuf[i];
        g_rx[i].addr_hi = 0;
        give_rx(i);
    }
    for (int i = 0; i < TX_COUNT; i++) memset(&g_tx[i], 0, sizeof(g_tx[i]));
    g_rx_cur = g_tx_cur = 0;

    w8(R_9346CR, 0xC0);                          /* configuration registers writable */
    w16(R_CPCR, (uint16_t)(r16(R_CPCR) | 0x0008));   /* C+ mode: PCI multiple read/write */
    w16(R_RMS, BUF_SIZE - 1);                    /* (longer frames are dropped, not written past a buffer) */
    w8(R_MTPS, 0x3B);
    w32(R_TNPDS, (uint32_t)(uintptr_t)g_tx);
    w32(R_TNPDS + 4, 0);
    w32(R_RDSAR, (uint32_t)(uintptr_t)g_rx);
    w32(R_RDSAR + 4, 0);
    w8(R_CMD, CMD_TE | CMD_RE);                  /* (some revisions take TCR/RCR only with these on) */
    w32(R_TCR, 0x03000700u);                     /* standard gap, unlimited DMA burst */
    w32(R_RCR, 0x0000E70Eu);                     /* no FIFO threshold, unlimited burst; ours, broadcast, multicast */
    w32(R_MAR0, 0xFFFFFFFFu);                    /* every multicast (mDNS, IPv6 ND...) */
    w32(R_MAR0 + 4, 0xFFFFFFFFu);
    w16(R_ISR, 0xFFFF);
    w16(R_IMR, 0x003F);                          /* receive/transmit OK and errors, link change, no buffer */
    w8(R_9346CR, 0x00);

    /* the PHY: auto-negotiation on and restarted (this also ends a power-down) */
    phy_write(0x1F, 0);                          /* (register page 0, on the chips that have pages) */
    phy_write(0x00, 0x1200);

    g_nd.name = "r8169";
    g_nd.model = model;
    g_nd.send = rtl_send;
    g_nd.poll = rtl_poll;
    g_nd.link_up = rtl_link_up;
    g_nd.irq = pd.irq_line;
    if (pd.irq_line > 0 && pd.irq_line < 16) irq_install(pd.irq_line, rtl_irq);
    else g_nd.irq = 0xFF;

    klog("r8169: %s (XID %03x) at %s 0x%x irq %u mac %02x:%02x:%02x:%02x:%02x:%02x\n", model, xid,
         g_mmio ? "mmio" : "io", g_mmio ? (uint32_t)(uintptr_t)g_mmio : g_io, pd.irq_line,
         g_nd.mac[0], g_nd.mac[1], g_nd.mac[2], g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
