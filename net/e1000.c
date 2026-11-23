/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netdev.h"
#include "pci.h"
#include "idt.h"
#include "kstring.h"
#include "serial.h"
#include "timer.h"

/*
 * Intel gigabit Ethernet: three families of one design.
 *   e1000  - the 8254x: QEMU's default NIC, VirtualBox's "Intel PRO/1000 MT
 *            Desktop" (82540EM), VMware's 82545EM
 *   e1000e - 82571-82574, 82583 and the chipsets' own (ICH8-10, PCH: the
 *            I217 / I218 / I219 of most Intel laptops and desktops);
 *            QEMU's "e1000e" (82574L)
 *   igb    - 82575 / 82576, 82580, I350, I354, I210 / I211 (many boards
 *            and server cards); QEMU's "igb" (82576)
 * All three run the same legacy receive / transmit descriptors; igb keeps
 * its queue 0 in its own register block, wants the queues enabled one by
 * one and the receive buffer size in SRRCTL. The e1000e / igb NVM read
 * register has its bits in other places (the address is usually in
 * RAL/RAH already, loaded by the hardware).
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

#define REG_CTRL_EXT 0x0018
#define REG_MDIC     0x0020
#define REG_SRRCTL0  0xC00C          /* igb */
#define REG_RXDCTL0  0xC028
#define REG_TXDCTL0  0xE028

#define CTRL_SLU    (1u << 6)
#define CTRL_RST    (1u << 26)
#define CTRL_EXT_DRV_LOAD (1u << 28)  /* (e1000e / igb: a driver has the card, not the firmware) */
#define QCTL_ENABLE (1u << 25)
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

enum { K_E1000 = 0, K_E1000E, K_IGB };
static int g_kind;
/* where queue 0's ring registers are (igb: its own block; e1000(e): the classic place) */
static uint32_t g_rdbal = REG_RDBAL, g_rdbah = REG_RDBAH, g_rdlen = REG_RDLEN, g_rdh = REG_RDH, g_rdt = REG_RDT;
static uint32_t g_tdbal = REG_TDBAL, g_tdbah = REG_TDBAH, g_tdlen = REG_TDLEN, g_tdh = REG_TDH, g_tdt = REG_TDT;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t*)(g_mmio + reg); }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t*)(g_mmio + reg) = v; }

/* a PHY register (MDIC: the copper side's own registers, PHY address 1) */
static int phy_write(uint32_t reg, uint16_t v) {
    wr(REG_MDIC, (uint32_t)v | (reg << 16) | (1u << 21) | (1u << 26));
    for (int i = 0; i < 2000; i++) {
        uint32_t m = rd(REG_MDIC);
        if (m & (1u << 28)) return (m & (1u << 30)) ? -1 : 0;     /* ready / error */
        timer_sleep_ms(1);
    }
    return -1;
}

static int eeprom_read(uint8_t addr, uint16_t* out) {
    /* 8254x: address at bit 8, done bit 4; e1000e / igb: address at bit 2, done bit 1 */
    int newer = g_kind != K_E1000;
    wr(REG_EERD, ((uint32_t)addr << (newer ? 2 : 8)) | 1u);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (newer ? (1u << 1) : (1u << 4))) { *out = (uint16_t)(v >> 16); return 0; }
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
    wr(g_tdt, g_tx_cur);
    nd->tx_packets++;
    nd->tx_bytes += n;
    return 0;
}

/* igb: advanced one-buffer receive descriptors - the address and a zero
 * header address going in; coming back, the status in dword 2 and the
 * length at byte 12 (the address is overwritten: put back on recycling) */
static void adv_arm(int i) {
    volatile uint32_t* w = (volatile uint32_t*)&g_rx[i];
    w[0] = (uint32_t)(uintptr_t)g_rxbuf[i];
    w[1] = 0;
    w[2] = 0;
    w[3] = 0;
}

static int igb_poll(netdev_t* nd) {
    int count = 0;
    for (;;) {
        volatile uint32_t* w = (volatile uint32_t*)&g_rx[g_rx_cur];
        uint32_t st = w[2];
        if (!(st & 1)) break;                              /* DD */
        uint16_t len = (uint16_t)(w[3] & 0xFFFF);
        uint32_t err = st >> 20;                           /* (RXE and friends) */
        if ((st & 2) && !(err & 0x800) && len >= 14) {     /* EOP, no frame error */
            nd->rx_packets++;
            nd->rx_bytes += len;
            net_rx(nd, g_rxbuf[g_rx_cur], len);
        } else nd->rx_errors++;
        adv_arm((int)g_rx_cur);
        wr(g_rdt, g_rx_cur);
        g_rx_cur = (g_rx_cur + 1) % RX_DESC;
        if (++count >= RX_DESC) break;
    }
    return count;
}

static void e1000_multicast(netdev_t* nd, int all) {
    (void)nd;
    uint32_t v = rd(REG_RCTL);
    wr(REG_RCTL, all ? v | (1u << 4) : v & ~(1u << 4));   /* MPE: multicast promiscuous */
}

static int e1000_poll(netdev_t* nd) {
    int count = 0;
    if (g_kind == K_IGB) return igb_poll(nd);
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
        wr(g_rdt, g_rx_cur);
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
static const struct { uint16_t id; uint8_t kind; const char* model; } g_ids[] = {
    /* e1000 */
    { 0x100E, K_E1000, "Intel 82540EM (e1000)" },   /* QEMU default, VirtualBox PRO/1000 MT Desktop */
    { 0x100F, K_E1000, "Intel 82545EM (e1000)" },   /* VMware */
    { 0x1004, K_E1000, "Intel 82543GC (e1000)" },
    { 0x100C, K_E1000, "Intel 82544GC (e1000)" },
    { 0x107C, K_E1000, "Intel 82541PI (e1000)" },
    { 0x1076, K_E1000, "Intel 82541GI (e1000)" },
    { 0x1026, K_E1000, "Intel 82545GM (e1000)" },
    { 0x1019, K_E1000, "Intel 82547EI (e1000)" },
    { 0x10B9, K_E1000, "Intel 82572EI (e1000)" },
    /* e1000e */
    { 0x105E, K_E1000E, "Intel 82571EB (e1000e)" },
    { 0x107D, K_E1000E, "Intel 82572EI (e1000e)" },
    { 0x108B, K_E1000E, "Intel 82573V (e1000e)" },
    { 0x108C, K_E1000E, "Intel 82573E (e1000e)" },
    { 0x109A, K_E1000E, "Intel 82573L (e1000e)" },
    { 0x10D3, K_E1000E, "Intel 82574L (e1000e)" },   /* QEMU e1000e */
    { 0x10F6, K_E1000E, "Intel 82574L (e1000e)" },
    { 0x150C, K_E1000E, "Intel 82583V (e1000e)" },
    { 0x1049, K_E1000E, "Intel 82566MM (ICH8)" },
    { 0x104A, K_E1000E, "Intel 82566DM (ICH8)" },
    { 0x10BD, K_E1000E, "Intel 82566DM-2 (ICH9)" },
    { 0x10F5, K_E1000E, "Intel 82567LM (ICH9)" },
    { 0x10DE, K_E1000E, "Intel 82567LM-3 (ICH10)" },
    { 0x10EF, K_E1000E, "Intel 82578DM (PCH)" },
    { 0x10F0, K_E1000E, "Intel 82578DC (PCH)" },
    { 0x1502, K_E1000E, "Intel 82579LM (PCH2)" },
    { 0x1503, K_E1000E, "Intel 82579V (PCH2)" },
    { 0x153A, K_E1000E, "Intel I217-LM" },
    { 0x153B, K_E1000E, "Intel I217-V" },
    { 0x155A, K_E1000E, "Intel I218-LM" },
    { 0x1559, K_E1000E, "Intel I218-V" },
    { 0x15A0, K_E1000E, "Intel I218-LM" },
    { 0x15A1, K_E1000E, "Intel I218-V" },
    { 0x15A2, K_E1000E, "Intel I218-LM" },
    { 0x15A3, K_E1000E, "Intel I218-V" },
    { 0x156F, K_E1000E, "Intel I219-LM" },
    { 0x1570, K_E1000E, "Intel I219-V" },
    { 0x15B7, K_E1000E, "Intel I219-LM" },
    { 0x15B8, K_E1000E, "Intel I219-V" },
    { 0x15B9, K_E1000E, "Intel I219-LM" },
    { 0x15BB, K_E1000E, "Intel I219-LM" },
    { 0x15BC, K_E1000E, "Intel I219-V" },
    { 0x15BD, K_E1000E, "Intel I219-LM" },
    { 0x15BE, K_E1000E, "Intel I219-V" },
    { 0x15D6, K_E1000E, "Intel I219-V" },
    { 0x15D7, K_E1000E, "Intel I219-LM" },
    { 0x15D8, K_E1000E, "Intel I219-V" },
    { 0x15E3, K_E1000E, "Intel I219-LM" },
    { 0x15DF, K_E1000E, "Intel I219-LM" },
    { 0x15E0, K_E1000E, "Intel I219-V" },
    { 0x15E1, K_E1000E, "Intel I219-LM" },
    { 0x15E2, K_E1000E, "Intel I219-V" },
    { 0x0D4E, K_E1000E, "Intel I219-LM" },
    { 0x0D4F, K_E1000E, "Intel I219-V" },
    { 0x0D4C, K_E1000E, "Intel I219-LM" },
    { 0x0D4D, K_E1000E, "Intel I219-V" },
    { 0x0D53, K_E1000E, "Intel I219-LM" },
    { 0x0D55, K_E1000E, "Intel I219-V" },
    { 0x15F9, K_E1000E, "Intel I219-LM" },
    { 0x15FA, K_E1000E, "Intel I219-V" },
    { 0x15FB, K_E1000E, "Intel I219-LM" },
    { 0x15FC, K_E1000E, "Intel I219-V" },
    { 0x1A1C, K_E1000E, "Intel I219-LM" },
    { 0x1A1D, K_E1000E, "Intel I219-V" },
    { 0x1A1E, K_E1000E, "Intel I219-LM" },
    { 0x1A1F, K_E1000E, "Intel I219-V" },
    { 0x0DC5, K_E1000E, "Intel I219-LM" },
    { 0x0DC6, K_E1000E, "Intel I219-V" },
    { 0x0DC7, K_E1000E, "Intel I219-LM" },
    { 0x0DC8, K_E1000E, "Intel I219-V" },
    { 0x550A, K_E1000E, "Intel I219-LM" },
    { 0x550B, K_E1000E, "Intel I219-V" },
    { 0x550C, K_E1000E, "Intel I219-LM" },
    { 0x550D, K_E1000E, "Intel I219-V" },
    /* igb */
    { 0x10A7, K_IGB, "Intel 82575EB (igb)" },
    { 0x10A9, K_IGB, "Intel 82575EB (igb)" },
    { 0x10D6, K_IGB, "Intel 82575GB (igb)" },
    { 0x10C9, K_IGB, "Intel 82576 (igb)" },          /* QEMU igb */
    { 0x10E6, K_IGB, "Intel 82576 (igb)" },
    { 0x10E7, K_IGB, "Intel 82576 (igb)" },
    { 0x10E8, K_IGB, "Intel 82576 (igb)" },
    { 0x1526, K_IGB, "Intel 82576 (igb)" },
    { 0x150A, K_IGB, "Intel 82576NS (igb)" },
    { 0x150D, K_IGB, "Intel 82576 (igb)" },
    { 0x1518, K_IGB, "Intel 82576NS (igb)" },
    { 0x150E, K_IGB, "Intel 82580 (igb)" },
    { 0x150F, K_IGB, "Intel 82580 (igb)" },
    { 0x1510, K_IGB, "Intel 82580 (igb)" },
    { 0x1511, K_IGB, "Intel 82580 (igb)" },
    { 0x1516, K_IGB, "Intel 82580 (igb)" },
    { 0x1527, K_IGB, "Intel 82580 (igb)" },
    { 0x1521, K_IGB, "Intel I350 (igb)" },
    { 0x1522, K_IGB, "Intel I350 (igb)" },
    { 0x1523, K_IGB, "Intel I350 (igb)" },
    { 0x1524, K_IGB, "Intel I350 (igb)" },
    { 0x1546, K_IGB, "Intel I350 (igb)" },
    { 0x1533, K_IGB, "Intel I210 (igb)" },
    { 0x1536, K_IGB, "Intel I210 (igb)" },
    { 0x1537, K_IGB, "Intel I210 (igb)" },
    { 0x1538, K_IGB, "Intel I210 (igb)" },
    { 0x157B, K_IGB, "Intel I210 (igb)" },
    { 0x157C, K_IGB, "Intel I210 (igb)" },
    { 0x1539, K_IGB, "Intel I211 (igb)" },
    { 0x1F40, K_IGB, "Intel I354 (igb)" },
    { 0x1F41, K_IGB, "Intel I354 (igb)" },
    { 0x1F45, K_IGB, "Intel I354 (igb)" },
};

netdev_t* e1000_probe(void) {
    pci_dev_t pd;
    const char* model = NULL;
    for (uint32_t i = 0; i < sizeof(g_ids) / sizeof(g_ids[0]); i++) {
        if (pci_find(0x8086, g_ids[i].id, &pd)) { model = g_ids[i].model; g_kind = g_ids[i].kind; break; }
    }
    if (!model) return NULL;
    if (g_kind == K_IGB) {
        g_rdbal = 0xC000; g_rdbah = 0xC004; g_rdlen = 0xC008; g_rdh = 0xC010; g_rdt = 0xC018;
        g_tdbal = 0xE000; g_tdbah = 0xE004; g_tdlen = 0xE008; g_tdh = 0xE010; g_tdt = 0xE018;
    }

    int is_io = 0;
    uintptr_t bar0 = pci_bar(&pd, 0, &is_io);
    if (is_io || !bar0) return NULL;
    pci_enable(&pd);
    g_mmio = (volatile uint8_t*)(uintptr_t)bar0;

    /* full reset, then bring the link up */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    timer_sleep_ms(10);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) {}
    if (g_kind != K_E1000) timer_sleep_ms(20);     /* (the NVM is read in again after a reset) */
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);
    if (g_kind != K_E1000) {
        wr(REG_CTRL_EXT, rd(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
        /* the PHY: powered up, auto-negotiation (re)started - the link comes up
         * when it is done (the newer chips do not start it by themselves) */
        phy_write(0, 0x1000 | 0x0200 | 0x0100 | 0x0040);   /* AN enable, restart AN, full duplex, 1000 */
        for (int i = 0; i < 300 && !(rd(REG_STATUS) & STATUS_LU); i++) timer_sleep_ms(10);
    }

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
    wr(g_rdbal, (uint32_t)(uintptr_t)g_rx);
    wr(g_rdbah, 0);
    wr(g_rdlen, sizeof(g_rx));
    wr(g_rdh, 0);
    if (g_kind == K_IGB) {
        for (int i = 0; i < RX_DESC; i++) adv_arm(i);
        wr(REG_SRRCTL0, 2u | (1u << 25));            /* 2 KiB buffers, advanced one-buffer descriptors */
        wr(REG_RXDCTL0, rd(REG_RXDCTL0) | QCTL_ENABLE);
        for (int i = 0; i < 1000 && !(rd(REG_RXDCTL0) & QCTL_ENABLE); i++) timer_sleep_ms(1);
    }
    wr(g_rdt, RX_DESC - 1);
    g_rx_cur = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* 2 KiB buffers (BSIZE=00) */

    /* transmit ring: every slot starts "done" so the first sends don't wait */
    for (int i = 0; i < TX_DESC; i++) {
        g_tx[i].addr = 0;
        g_tx[i].cmd = 0;
        g_tx[i].status = DESC_DD;
    }
    wr(g_tdbal, (uint32_t)(uintptr_t)g_tx);
    wr(g_tdbah, 0);
    wr(g_tdlen, sizeof(g_tx));
    wr(g_tdh, 0);
    wr(g_tdt, 0);
    g_tx_cur = 0;
    if (g_kind == K_IGB) {
        wr(REG_TXDCTL0, rd(REG_TXDCTL0) | QCTL_ENABLE);
        for (int i = 0; i < 1000 && !(rd(REG_TXDCTL0) & QCTL_ENABLE); i++) timer_sleep_ms(1);
    }
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    wr(REG_TIPG, 0x0060200Au);

    g_nd.name = g_kind == K_IGB ? "igb" : g_kind == K_E1000E ? "e1000e" : "e1000";
    g_nd.model = model;
    g_nd.send = e1000_send;
    g_nd.poll = e1000_poll;
    g_nd.link_up = e1000_link_up;
    g_nd.set_multicast = e1000_multicast;
    g_nd.irq = pd.irq_line;

    if (pd.irq_line > 0 && pd.irq_line < 16) {
        irq_install(pd.irq_line, e1000_irq);
        wr(REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);
    } else {
        g_nd.irq = 0xFF;
    }

    klog("%s: %s at mmio 0x%lx irq %u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         g_nd.name, model, (unsigned long)bar0, pd.irq_line, g_nd.mac[0], g_nd.mac[1], g_nd.mac[2],
         g_nd.mac[3], g_nd.mac[4], g_nd.mac[5]);
    return &g_nd;
}
