#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "kstring.h"
#include "serial.h"
#include "timer.h"

/*
 * Intel 8254x ("e1000") gigabit Ethernet driver - QEMU's default NIC for
 * i386 PCs and VirtualBox's "Intel PRO/1000 MT Desktop" (82540EM).
 *
 * Classic legacy-descriptor mode: two DMA rings in RAM (no paging, so
 * virtual == physical and the kernel's addresses go straight into the
 * descriptors). Receive completion raises an interrupt only to wake the
 * CPU from hlt; the frames themselves are pulled by e1000_poll() in task
 * context, never inside the IRQ.
 */

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU    (1u << 6)
#define CTRL_RST    (1u << 26)
#define STATUS_LU   (1u << 1)

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)   /* accept broadcast (ARP, DHCP) */
#define RCTL_SECRC  (1u << 26)   /* strip Ethernet CRC */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)

#define ICR_TXDW    (1u << 0)
#define ICR_LSC     (1u << 2)
#define ICR_RXDMT0  (1u << 4)
#define ICR_RXO     (1u << 6)
#define ICR_RXT0    (1u << 7)

#define RX_DESC     64
#define TX_DESC     32
#define BUF_SIZE    2048

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc_t;

#define DESC_DD   0x01
#define DESC_EOP  0x02
#define TXCMD_EOP  0x01
#define TXCMD_IFCS 0x02
#define TXCMD_RS   0x08

static volatile rx_desc_t g_rx[RX_DESC] __attribute__((aligned(128)));
static volatile tx_desc_t g_tx[TX_DESC] __attribute__((aligned(128)));
static uint8_t   g_rxbuf[RX_DESC][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t   g_txbuf[TX_DESC][BUF_SIZE] __attribute__((aligned(16)));

static volatile uint8_t* g_mmio;
static uint32_t g_rx_cur, g_tx_cur;
static netdev_t g_nd;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t*)(g_mmio + reg); }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t*)(g_mmio + reg) = v; }

static int eeprom_read(uint8_t addr, uint16_t* out) {
    wr(REG_EERD, ((uint32_t)addr << 8) | 1u);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4)) { *out = (uint16_t)(v >> 16); return 0; }
    }
    return -1;
}

static void read_mac(uint8_t mac[6]) {
    uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
    if (rah & (1u << 31)) {  /* receive address valid: firmware filled it */
        for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(ral >> (i * 8));
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
        return;
    }
    for (int i = 0; i < 3; i++) {
        uint16_t w = 0;
        eeprom_read((uint8_t)i, &w);
        mac[i * 2] = (uint8_t)w;
        mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
}

static void e1000_irq(void) {
    /* reading ICR acknowledges (clears) the interrupt at the device, which
     * a level-triggered PCI line needs before the EOI */
    uint32_t icr = rd(REG_ICR);
    if (icr) net_irq_notify();
}

static int e1000_send(netdev_t* nd, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) { nd->tx_errors++; return -1; }
    volatile tx_desc_t* d = &g_tx[g_tx_cur];
    /* ring full: the slot we'd reuse hasn't been sent yet */
    for (int i = 0; !(d->status & DESC_DD); i++) {
        if (i > 200000) { nd->tx_errors++; return -1; }
    }
    memcpy(g_txbuf[g_tx_cur], frame, len);
    uint32_t n = len < 60 ? 60 : len;   /* pad runts to the Ethernet minimum */
    if (n > len) memset(g_txbuf[g_tx_cur] + len, 0, n - len);
    d->addr = (uint32_t)(uintptr_t)g_txbuf[g_tx_cur];
    d->length = (uint16_t)n;
    d->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    d->status = 0;
    g_tx_cur = (g_tx_cur + 1) % TX_DESC;
    __asm__ volatile("" ::: "memory");
    wr(REG_TDT, g_tx_cur);
    nd->tx_packets++;
    nd->tx_bytes += n;
    return 0;
}

static int e1000_poll(netdev_t* nd) {
    int count = 0;
    while (g_rx[g_rx_cur].status & DESC_DD) {
        volatile rx_desc_t* d = &g_rx[g_rx_cur];
        uint16_t len = d->length;
        if ((d->status & DESC_EOP) && d->errors == 0 && len >= 14) {
            nd->rx_packets++;
            nd->rx_bytes += len;
            net_rx(nd, g_rxbuf[g_rx_cur], len);
        } else {
            nd->rx_errors++;
        }
        d->status = 0;
        /* hand the descriptor back: RDT points at the last one we own */
        wr(REG_RDT, g_rx_cur);
        g_rx_cur = (g_rx_cur + 1) % RX_DESC;
        if (++count >= RX_DESC) break;
    }
    return count;
}

static int e1000_link_up(netdev_t* nd) {
    (void)nd;
    return (rd(REG_STATUS) & STATUS_LU) ? 1 : 0;
}

/* device ids handled by this driver (all speak the legacy descriptor format) */
static const struct { uint16_t id; const char* model; } g_ids[] = {
    { 0x100E, "Intel 82540EM (e1000)" },   /* QEMU default, VirtualBox PRO/1000 MT Desktop */
    { 0x100F, "Intel 82545EM (e1000)" },   /* VMware */
    { 0x1004, "Intel 82543GC (e1000)" },
    { 0x107C, "Intel 82541PI (e1000)" },
};

netdev_t* e1000_probe(void) {
    pci_dev_t pd;
    const char* model = NULL;
    for (uint32_t i = 0; i < sizeof(g_ids) / sizeof(g_ids[0]); i++) {
        if (pci_find(0x8086, g_ids[i].id, &pd)) { model = g_ids[i].model; break; }
    }
    if (!model) return NULL;

    int is_io = 0;
    uint32_t bar0 = pci_bar(&pd, 0, &is_io);
    if (is_io || !bar0) return NULL;
    pci_enable(&pd);
    g_mmio = (volatile uint8_t*)(uintptr_t)bar0;

    /* full reset, then bring the link up */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    timer_sleep_ms(10);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) {}
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);

    memset(&g_nd, 0, sizeof(g_nd));
    read_mac(g_nd.mac);
    /* program our address into receive filter 0 */
    wr(REG_RAL0, (uint32_t)g_nd.mac[0] | ((uint32_t)g_nd.mac[1] << 8) |
                 ((uint32_t)g_nd.mac[2] << 16) | ((uint32_t)g_nd.mac[3] << 24));
    wr(REG_RAH0, (uint32_t)g_nd.mac[4] | ((uint32_t)g_nd.mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    /* receive ring: all descriptors owned by the NIC */
    for (int i = 0; i < RX_DESC; i++) {
        g_rx[i].addr = (uint32_t)(uintptr_t)g_rxbuf[i];
        g_rx[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)(uintptr_t)g_rx);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, sizeof(g_rx));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESC - 1);
    g_rx_cur = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* 2 KiB buffers (BSIZE=00) */

    /* transmit ring: every slot starts "done" so the first sends don't wait */
    for (int i = 0; i < TX_DESC; i++) {
        g_tx[i].addr = 0;
        g_tx[i].cmd = 0;
        g_tx[i].status = DESC_DD;
    }
    wr(REG_TDBAL, (uint32_t)(uintptr_t)g_tx);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof(g_tx));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    g_tx_cur = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    wr(REG_TIPG, 0x0060200Au);

    g_nd.name = "e1000";
    g_nd.model = model;
    g_nd.send = e1000_send;
    g_nd.poll = e1000_poll;
    g_nd.link_up = e1000_link_up;
    g_nd.irq = pd.irq_line;

    if (pd.irq_line > 0 && pd.irq_line < 16) {
        irq_install(pd.irq_line, e1000_irq);
        wr(REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);
    } else {
        g_nd.irq = 0xFF;
    }

    klog("e1000: %s at mmio 0x%x irq %u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         model, bar0, pd.irq_line, g_nd.mac[0], g_nd.mac[1], g_nd.mac[2],
         g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
