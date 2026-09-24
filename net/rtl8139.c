#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

/*
 * Realtek RTL8139 Fast Ethernet driver (QEMU: -nic user,model=rtl8139).
 *
 * The 8139 receives into one contiguous ring buffer (each packet is
 * prefixed by a 4-byte status/length header) and transmits from four
 * fixed descriptor slots used round robin. Programmed through I/O ports.
 */

#define R_IDR0    0x00
#define R_TSD0    0x10    /* transmit status, 4 x 32-bit */
#define R_TSAD0   0x20    /* transmit start address, 4 x 32-bit */
#define R_RBSTART 0x30
#define R_CR      0x37
#define R_CAPR    0x38
#define R_IMR     0x3C
#define R_ISR     0x3E
#define R_TCR     0x40
#define R_RCR     0x44
#define R_CONFIG1 0x52
#define R_MSR     0x58

#define CR_BUFE   0x01
#define CR_TE     0x04
#define CR_RE     0x08
#define CR_RST    0x10

#define ISR_ROK   0x0001
#define ISR_RER   0x0002
#define ISR_TOK   0x0004
#define ISR_TER   0x0008
#define ISR_RXOVW 0x0010

#define TSD_OWN   (1u << 13)
#define TSD_TOK   (1u << 15)

#define RX_RING   8192
/* +16 for the header of a packet that starts at the very end, +1500 of
 * overflow room since RCR.WRAP lets a packet run past the ring's end */
static uint8_t g_rxbuf[RX_RING + 16 + 1536] __attribute__((aligned(16)));
static uint8_t g_txbuf[4][1536] __attribute__((aligned(16)));

static uint16_t g_io;
static uint32_t g_rx_off;
static int      g_tx_cur;
static netdev_t g_nd;

static void rtl_irq(void) {
    uint16_t isr = inw(g_io + R_ISR);
    outw(g_io + R_ISR, isr);   /* write-1-to-clear */
    if (isr) net_irq_notify();
}

static int rtl_send(netdev_t* nd, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) { nd->tx_errors++; return -1; }
    uint16_t tsd = (uint16_t)(g_io + R_TSD0 + g_tx_cur * 4);
    /* OWN set = the chip is done with this slot's previous frame */
    for (int i = 0; !(inl(tsd) & TSD_OWN); i++) {
        if (i > 200000) { nd->tx_errors++; return -1; }
    }
    memcpy(g_txbuf[g_tx_cur], frame, len);
    uint32_t n = len < 60 ? 60 : len;
    if (n > len) memset(g_txbuf[g_tx_cur] + len, 0, n - len);
    outl((uint16_t)(g_io + R_TSAD0 + g_tx_cur * 4), (uint32_t)(uintptr_t)g_txbuf[g_tx_cur]);
    outl(tsd, n);   /* size + OWN=0 starts the transmission */
    g_tx_cur = (g_tx_cur + 1) & 3;
    nd->tx_packets++;
    nd->tx_bytes += n;
    return 0;
}

static int rtl_poll(netdev_t* nd) {
    int count = 0;
    while (!(inb(g_io + R_CR) & CR_BUFE)) {
        uint8_t* p = g_rxbuf + g_rx_off;
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len    = (uint16_t)(p[2] | (p[3] << 8));   /* includes 4-byte CRC */

        if (!(status & 0x01) || len < 18 || len > 1522) {
            /* corrupt header: the only safe recovery is a receiver reset */
            nd->rx_errors++;
            outb(g_io + R_CR, CR_TE);
            outb(g_io + R_CR, CR_TE | CR_RE);
            g_rx_off = 0;
            outw(g_io + R_CAPR, (uint16_t)(g_rx_off - 16));
            break;
        }
        nd->rx_packets++;
        nd->rx_bytes += len - 4u;
        net_rx(nd, p + 4, len - 4u);

        g_rx_off = (g_rx_off + len + 4 + 3) & ~3u;
        g_rx_off %= RX_RING;
        outw(g_io + R_CAPR, (uint16_t)(g_rx_off - 16));
        if (++count >= 64) break;
    }
    return count;
}

static int rtl_link_up(netdev_t* nd) {
    (void)nd;
    return (inb(g_io + R_MSR) & 0x04) ? 0 : 1;   /* LINKB = link fail */
}

netdev_t* rtl8139_probe(void) {
    pci_dev_t pd;
    if (!pci_find(0x10EC, 0x8139, &pd)) return NULL;

    int is_io = 0;
    uint32_t bar0 = pci_bar(&pd, 0, &is_io);
    if (!is_io || !bar0) return NULL;
    pci_enable(&pd);
    g_io = (uint16_t)bar0;

    outb(g_io + R_CONFIG1, 0x00);          /* power on */
    outb(g_io + R_CR, CR_RST);
    for (int i = 0; i < 1000000 && (inb(g_io + R_CR) & CR_RST); i++) {}

    memset(&g_nd, 0, sizeof(g_nd));
    for (int i = 0; i < 6; i++) g_nd.mac[i] = inb((uint16_t)(g_io + R_IDR0 + i));

    outl(g_io + R_RBSTART, (uint32_t)(uintptr_t)g_rxbuf);
    g_rx_off = 0;
    g_tx_cur = 0;
    outw(g_io + R_IMR, ISR_ROK | ISR_RER | ISR_RXOVW);
    outw(g_io + R_ISR, 0xFFFF);
    /* accept broadcast + multicast + our MAC; WRAP; 8K ring; unlimited DMA burst */
    outl(g_io + R_RCR, 0x02u | 0x04u | 0x08u | (1u << 7) | (7u << 8));
    outl(g_io + R_TCR, (6u << 8));         /* 1024-byte max DMA burst */
    outb(g_io + R_CR, CR_TE | CR_RE);

    g_nd.name = "rtl8139";
    g_nd.model = "Realtek RTL8139";
    g_nd.send = rtl_send;
    g_nd.poll = rtl_poll;
    g_nd.link_up = rtl_link_up;
    g_nd.irq = pd.irq_line;
    if (pd.irq_line > 0 && pd.irq_line < 16) irq_install(pd.irq_line, rtl_irq);
    else g_nd.irq = 0xFF;

    klog("rtl8139: io 0x%x irq %u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         g_io, pd.irq_line, g_nd.mac[0], g_nd.mac[1], g_nd.mac[2],
         g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
