#include "usbnet.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * Realtek RTL8152 / RTL8152B USB 2.0 Fast Ethernet (e.g. Lanberg
 * NC-0100-01, TP-Link UE200, many no-name "USB 2.0 to RJ45" adapters).
 *
 * The chip is programmed through vendor control requests that read and
 * write two register spaces ("PLA" MAC registers, "USB" bridge
 * registers) in aligned 4-byte words with byte enables. Frames travel
 * over bulk endpoints with a small Realtek header, several received
 * frames packed into each bulk IN transfer.
 *
 * Register map and bring-up sequence derived from OpenBSD's if_ure.c
 * (Copyright (c) 2015-2019 Kevin Lo, 2020 Jonathon Fletcher, BSD
 * 2-Clause licence - see THIRD-PARTY-NOTICES).
 */

#define RTL_REQ_REGS    0x05          /* bRequest for both reads and writes */
#define MCU_PLA         0x0100
#define MCU_USB         0x0000
#define BYTE_EN_DWORD   0xff
#define BYTE_EN_WORD    0x33
#define BYTE_EN_BYTE    0x11
#define BYTE_EN_SIX     0x3f

/* PLA registers */
#define PLA_IDR          0xc000
#define PLA_RCR          0xc010
#define PLA_RMS          0xc016
#define PLA_RXFIFO_CTRL0 0xc0a0
#define PLA_RXFIFO_CTRL1 0xc0a4
#define PLA_RXFIFO_CTRL2 0xc0a8
#define PLA_FMC          0xc0b4
#define PLA_TEREDO_CFG   0xc0bc
#define PLA_MAR          0xcd00
#define PLA_BACKUP       0xd000
#define PLA_TEREDO_TIMER 0xd2cc
#define PLA_REALWOW_TIMER 0xd2e8
#define PLA_LED_FEATURE  0xdd92
#define PLA_GPHY_INTR_IMR 0xe022
#define PLA_MAC_PWR_CTRL 0xe0c0
#define PLA_WDT6_CTRL    0xe428
#define PLA_TCR0         0xe610
#define PLA_TCR1         0xe612
#define PLA_TXFIFO_CTRL  0xe618
#define PLA_RSTTALLY     0xe800
#define PLA_CR           0xe813
#define PLA_CRWECR       0xe81c
#define PLA_PHY_PWR      0xe84c
#define PLA_OOB_CTRL     0xe84f
#define PLA_CPCR         0xe854
#define PLA_MISC_1       0xe85a
#define PLA_OCP_GPHY_BASE 0xe86c
#define PLA_SFF_STS_7    0xe8de
#define PLA_PHYSTATUS    0xe908

/* USB registers */
#define USB_USB_CTRL     0xd406
#define USB_TX_AGG       0xd40a
#define USB_RX_BUF_TH    0xd40c
#define USB_PM_CTRL_STATUS 0xd432
#define USB_TX_DMA       0xd434
#define USB_UPS_CTRL     0xd800

/* PHY (OCP) registers */
#define OCP_ALDPS_CONFIG 0x2010
#define OCP_BASE_MII     0xa400

/* bits */
#define RCR_AAP 0x01
#define RCR_APM 0x02
#define RCR_AM  0x04
#define RCR_AB  0x08
#define RCR_ACPT_ALL 0x0f
#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04
#define CRWECR_NORMAL 0x00
#define CRWECR_CONFIG 0xc0
#define LINK_LIST_READY 0x02
#define NOW_IS_OOB   0x80
#define RXDY_GATED_EN 0x0008
#define MCU_BORW_EN  0x4000
#define RE_INIT_LL   0x8000
#define CPCR_RX_VLAN 0x0040
#define TEREDO_SEL   0x8000
#define TEREDO_RS_EVENT_MASK 0x00fe
#define OOB_TEREDO_EN 0x0001
#define WDT6_SET_MODE 0x0010
#define TCR0_AUTO_FIFO 0x0080
#define VERSION_MASK 0x7cf0
#define TALLY_RESET  0x0001
#define LED_MODE_MASK 0x0700
#define TX_10M_IDLE_EN 0x0080
#define PFM_PWM_SWITCH 0x0040
#define D3_CLK_GATED_EN 0x00004000
#define MCU_CLK_RATIO 0x07010f07
#define MCU_CLK_RATIO_MASK 0x0f0f0f0f
#define GPHY_STS_MSK 0x0001
#define SPEED_DOWN_MSK 0x0002
#define SPDWN_RXDV_MSK 0x0004
#define SPDWN_LINKCHG_MSK 0x0008
#define POWER_CUT    0x0100
#define RESUME_INDICATE 0x0001
#define RX_AGG_DISABLE 0x0010
#define RX_ZERO_EN   0x0080
#define FMC_FCR_MCU_EN 0x0001
#define PHYSTATUS_LINK 0x0002
#define PHYSTATUS_100 0x0008
#define PHYSTATUS_FDX 0x0001
#define ENPWRSAVE    0x8000
#define ENPDNPS      0x0200
#define LINKENA      0x0100
#define DIS_SDSAVE   0x0010

#define RXFIFO_THR1_NORMAL 0x00080002
#define RXFIFO_THR2_FULL   0x00000060
#define RXFIFO_THR2_HIGH   0x00000038
#define RXFIFO_THR3_FULL   0x00000078
#define RXFIFO_THR3_HIGH   0x00000048
#define TXFIFO_THR_NORMAL  0x00400008
#define TX_AGG_MAX_THRESHOLD 0x03
#define RX_THR_HIGH        0x7a120180
#define TEST_MODE_DISABLE  0x00000001
#define TX_SIZE_ADJUST1    0x00000100

#define TXPKT_FS (1u << 31)
#define TXPKT_LS (1u << 30)
#define RXPKT_LEN_MASK 0x7fff
#define RX_HDR   24
#define TX_HDR   8
#define RX_ALIGN 8
#define RX_BUFSZ 16384
#define TX_BUFSZ 2048

/* MII */
#define MII_BMCR 0
#define MII_ANAR 4
#define BMCR_AUTOEN   0x1000
#define BMCR_STARTNEG 0x0200
#define ANAR_10       0x0020
#define ANAR_10_FD    0x0040
#define ANAR_TX       0x0080
#define ANAR_TX_FD    0x0100
#define ANAR_PAUSE    0x0400
#define ANAR_CSMA     0x0001

typedef struct {
    usb_device_t* d;
    uint16_t      version;
    int           ok;                 /* no control transfer failed */
} rtl_t;

/* ── register access (vendor requests) ─────────────────────────── */

static int ctl(rtl_t* r, int write, uint16_t addr, uint16_t index, void* buf, uint16_t len) {
    uint8_t type = (uint8_t)(USB_TYPE_VENDOR | USB_RECIP_DEVICE | (write ? 0 : USB_DIR_IN));
    int n = usb_control(r->d, type, RTL_REQ_REGS, addr, index, buf, len);
    if (n < 0) r->ok = 0;
    return n;
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t rd4(rtl_t* r, uint16_t reg, uint16_t type) {
    uint8_t b[4] = { 0 };
    ctl(r, 0, reg, type, b, 4);
    return le32(b);
}
static uint16_t rd2(rtl_t* r, uint16_t reg, uint16_t type) {
    return (uint16_t)(rd4(r, reg & ~3u, type) >> ((reg & 2) * 8));
}
static uint8_t rd1(rtl_t* r, uint16_t reg, uint16_t type) {
    return (uint8_t)(rd4(r, reg & ~3u, type) >> ((reg & 3) * 8));
}

static void wr_mem(rtl_t* r, uint16_t reg, uint16_t index, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    ctl(r, 1, reg, index, b, 4);
}
static void wr4(rtl_t* r, uint16_t reg, uint16_t type, uint32_t v) {
    wr_mem(r, reg, type | BYTE_EN_DWORD, v);
}
static void wr2(rtl_t* r, uint16_t reg, uint16_t type, uint32_t v) {
    uint32_t shift = reg & 2;
    wr_mem(r, reg & ~3u, (uint16_t)(type | (BYTE_EN_WORD << shift)), (v & 0xffff) << (shift * 8));
}
static void wr1(rtl_t* r, uint16_t reg, uint16_t type, uint32_t v) {
    uint32_t shift = reg & 3;
    wr_mem(r, reg & ~3u, (uint16_t)(type | (BYTE_EN_BYTE << shift)), (v & 0xff) << (shift * 8));
}

#define SET2(r, reg, t, bits) wr2(r, reg, t, rd2(r, reg, t) | (bits))
#define CLR2(r, reg, t, bits) wr2(r, reg, t, rd2(r, reg, t) & ~(uint32_t)(bits))
#define SET1(r, reg, t, bits) wr1(r, reg, t, rd1(r, reg, t) | (bits))
#define CLR1(r, reg, t, bits) wr1(r, reg, t, rd1(r, reg, t) & ~(uint32_t)(bits))

/* PHY registers go through a movable window in PLA space */
static void phy_write(rtl_t* r, uint16_t addr, uint16_t data) {
    wr2(r, PLA_OCP_GPHY_BASE, MCU_PLA, addr & 0xf000);
    wr2(r, (uint16_t)((addr & 0x0fff) | 0xb000), MCU_PLA, data);
}

/* ── bring-up ───────────────────────────────────────────────────── */

static void chip_init(rtl_t* r) {
    phy_write(r, OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);   /* ALDPS off */
    usb_delay_ms(20);
    if (r->version == 0x4c00) CLR2(r, PLA_LED_FEATURE, MCU_PLA, LED_MODE_MASK);
    CLR2(r, USB_UPS_CTRL, MCU_USB, POWER_CUT);
    CLR2(r, USB_PM_CTRL_STATUS, MCU_USB, RESUME_INDICATE);
    SET2(r, PLA_PHY_PWR, MCU_PLA, TX_10M_IDLE_EN | PFM_PWM_SWITCH);
    uint32_t pwr = rd4(r, PLA_MAC_PWR_CTRL, MCU_PLA);
    pwr = (pwr & ~(uint32_t)MCU_CLK_RATIO_MASK) | MCU_CLK_RATIO | D3_CLK_GATED_EN;
    wr4(r, PLA_MAC_PWR_CTRL, MCU_PLA, pwr);
    wr2(r, PLA_GPHY_INTR_IMR, MCU_PLA, GPHY_STS_MSK | SPEED_DOWN_MSK | SPDWN_RXDV_MSK | SPDWN_LINKCHG_MSK);
    SET2(r, PLA_RSTTALLY, MCU_PLA, TALLY_RESET);
    CLR2(r, USB_USB_CTRL, MCU_USB, RX_AGG_DISABLE | RX_ZERO_EN);     /* RX aggregation on */
}

static int wait_oob(rtl_t* r) {
    for (int i = 0; i < 1000; i++) {
        if (rd1(r, PLA_OOB_CTRL, MCU_PLA) & LINK_LIST_READY) return 0;
        usb_delay_ms(1);
    }
    return -1;
}

static int nic_reset(rtl_t* r, int full_speed) {
    phy_write(r, OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);
    usb_delay_ms(20);
    wr4(r, PLA_RCR, MCU_PLA, rd4(r, PLA_RCR, MCU_PLA) & ~(uint32_t)RCR_ACPT_ALL);
    SET2(r, PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);

    /* no Teredo offload / wake-on-LAN timers */
    CLR2(r, PLA_TEREDO_CFG, MCU_PLA, TEREDO_SEL | TEREDO_RS_EVENT_MASK | OOB_TEREDO_EN);
    wr2(r, PLA_WDT6_CTRL, MCU_PLA, WDT6_SET_MODE);
    wr2(r, PLA_REALWOW_TIMER, MCU_PLA, 0);
    wr4(r, PLA_TEREDO_TIMER, MCU_PLA, 0);

    wr1(r, PLA_CRWECR, MCU_PLA, CRWECR_NORMAL);
    wr1(r, PLA_CR, MCU_PLA, 0);
    CLR1(r, PLA_OOB_CTRL, MCU_PLA, NOW_IS_OOB);
    CLR2(r, PLA_SFF_STS_7, MCU_PLA, MCU_BORW_EN);
    if (wait_oob(r) != 0) return -1;
    SET2(r, PLA_SFF_STS_7, MCU_PLA, RE_INIT_LL);
    if (wait_oob(r) != 0) return -1;

    wr1(r, PLA_CR, MCU_PLA, CR_RST);
    for (int i = 0; i < 1000 && (rd1(r, PLA_CR, MCU_PLA) & CR_RST); i++) usb_delay_ms(1);

    wr4(r, PLA_RXFIFO_CTRL0, MCU_PLA, RXFIFO_THR1_NORMAL);
    wr4(r, PLA_RXFIFO_CTRL1, MCU_PLA, full_speed ? RXFIFO_THR2_FULL : RXFIFO_THR2_HIGH);
    wr4(r, PLA_RXFIFO_CTRL2, MCU_PLA, full_speed ? RXFIFO_THR3_FULL : RXFIFO_THR3_HIGH);
    wr4(r, PLA_TXFIFO_CTRL, MCU_PLA, TXFIFO_THR_NORMAL);
    wr1(r, USB_TX_AGG, MCU_USB, TX_AGG_MAX_THRESHOLD);
    wr4(r, USB_RX_BUF_TH, MCU_USB, RX_THR_HIGH);
    wr4(r, USB_TX_DMA, MCU_USB, TEST_MODE_DISABLE | TX_SIZE_ADJUST1);
    CLR2(r, PLA_CPCR, MCU_PLA, CPCR_RX_VLAN);                 /* keep VLAN tags in the frame */
    wr2(r, PLA_RMS, MCU_PLA, 1518 + 4);                       /* max frame incl. VLAN tag */
    SET2(r, PLA_TCR0, MCU_PLA, TCR0_AUTO_FIFO);
    phy_write(r, OCP_ALDPS_CONFIG, ENPWRSAVE | ENPDNPS | LINKENA | DIS_SDSAVE);
    return r->ok ? 0 : -1;
}

static void enable_rx_tx(rtl_t* r, const uint8_t mac[6]) {
    /* station address (register writes need CRWECR config mode) */
    wr1(r, PLA_CRWECR, MCU_PLA, CRWECR_CONFIG);
    uint8_t m[8] = { mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 0, 0 };
    ctl(r, 1, PLA_IDR, MCU_PLA | BYTE_EN_SIX, m, 8);
    wr1(r, PLA_CRWECR, MCU_PLA, CRWECR_NORMAL);

    CLR2(r, PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);                /* reset the filter */
    SET2(r, PLA_FMC, MCU_PLA, FMC_FCR_MCU_EN);
    SET1(r, PLA_CR, MCU_PLA, CR_RE | CR_TE);
    CLR2(r, PLA_MISC_1, MCU_PLA, RXDY_GATED_EN);

    /* our address + broadcast + (all) multicast */
    uint32_t mar[2] = { 0xffffffffu, 0xffffffffu };
    ctl(r, 1, PLA_MAR, MCU_PLA | BYTE_EN_DWORD, mar, 8);
    uint32_t rcr = rd4(r, PLA_RCR, MCU_PLA) & ~(uint32_t)RCR_ACPT_ALL;
    wr4(r, PLA_RCR, MCU_PLA, rcr | RCR_APM | RCR_AB | RCR_AM);

    /* autonegotiate 10/100, half/full duplex, with flow control */
    phy_write(r, OCP_BASE_MII + MII_ANAR * 2,
              ANAR_TX_FD | ANAR_TX | ANAR_10_FD | ANAR_10 | ANAR_PAUSE | ANAR_CSMA);
    phy_write(r, OCP_BASE_MII + MII_BMCR * 2, BMCR_AUTOEN | BMCR_STARTNEG);
}

/* ── framing ────────────────────────────────────────────────────── */

static void rtl_decap(usbnet_t* u, const uint8_t* buf, uint32_t len) {
    uint32_t off = 0;
    while (off + RX_HDR <= len) {
        uint32_t pktlen = le32(buf + off) & RXPKT_LEN_MASK;    /* includes the 4-byte CRC */
        if (pktlen < 14 + 4 || off + RX_HDR + pktlen > len) {
            u->nd.rx_errors++;
            break;
        }
        u->nd.rx_packets++;
        u->nd.rx_bytes += pktlen - 4;
        net_rx(&u->nd, buf + off + RX_HDR, pktlen - 4);
        off += RX_HDR + ((pktlen + RX_ALIGN - 1) & ~(uint32_t)(RX_ALIGN - 1));
    }
}

static int rtl_encap(usbnet_t* u, uint8_t* out, const uint8_t* frame, uint32_t len) {
    if (len + TX_HDR > u->tx_size) return -1;
    uint32_t w0 = len | TXPKT_FS | TXPKT_LS;
    out[0] = (uint8_t)w0; out[1] = (uint8_t)(w0 >> 8); out[2] = (uint8_t)(w0 >> 16); out[3] = (uint8_t)(w0 >> 24);
    out[4] = out[5] = out[6] = out[7] = 0;                     /* no checksum offload / VLAN */
    memcpy(out + TX_HDR, frame, len);
    return (int)(len + TX_HDR);
}

static int rtl_get_link(usbnet_t* u) {
    rtl_t* r = (rtl_t*)u->priv;
    uint16_t st = rd2(r, PLA_PHYSTATUS, MCU_PLA);
    int up = (st & PHYSTATUS_LINK) != 0;
    if (up) SET1(r, PLA_CR, MCU_PLA, CR_RE | CR_TE);           /* keep RX/TX on after renegotiation */
    return up;
}

/* ── USB driver ─────────────────────────────────────────────────── */

typedef struct {
    usb_ep_desc_t in, out;
    int have_in, have_out, in_vendor;
} eps_t;

static int ep_cb(const uint8_t* d, void* ctx) {
    eps_t* e = (eps_t*)ctx;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        e->in_vendor = (id->bInterfaceClass == 0xFF && id->bAlternateSetting == 0);
    } else if (d[1] == USB_DT_ENDPOINT && e->in_vendor) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
        if ((ep->bmAttributes & 3) != USB_EP_BULK) return 0;
        if ((ep->bEndpointAddress & 0x80) && !e->have_in) { e->in = *ep; e->have_in = 1; }
        if (!(ep->bEndpointAddress & 0x80) && !e->have_out) { e->out = *ep; e->have_out = 1; }
    }
    return 0;
}

static int is_rtl8152(const usb_device_t* d) {
    /* Realtek's own id - what the Lanberg NC-0100-01 and most generic
     * RTL8152 adapters report. (The chip version read in attach is the
     * real check; this just avoids poking unrelated devices.) */
    return d->desc.idVendor == 0x0bda && d->desc.idProduct == 0x8152;
}

static int rtl_probe(usb_device_t* d) {
    if (!is_rtl8152(d)) return -1;
    /* the vendor-specific configuration (the chip may also offer a
     * CDC-ECM one, which cdc_ecm would take if this driver declines) */
    for (int c = 0; c < USB_MAX_CONFIGS; c++) {
        eps_t e;
        memset(&e, 0, sizeof(e));
        if (!d->configs[c]) continue;
        usb_walk_config(d, c, ep_cb, &e);
        if (e.have_in && e.have_out) return c;
    }
    return -1;
}

static int rtl_attach(usb_device_t* d) {
    eps_t e;
    memset(&e, 0, sizeof(e));
    usb_walk_config(d, d->active_config, ep_cb, &e);
    if (!e.have_in || !e.have_out) return -1;

    rtl_t* r = (rtl_t*)kzalloc(sizeof(rtl_t));
    usbnet_t* u = (usbnet_t*)kzalloc(sizeof(usbnet_t));
    if (!r || !u) { kfree(r); kfree(u); return -1; }
    r->d = d;
    r->ok = 1;

    r->version = rd2(r, PLA_TCR1, MCU_PLA) & VERSION_MASK;
    if (!r->ok || (r->version != 0x4c00 && r->version != 0x4c10)) {
        klog("r8152: unsupported chip version %04x (only RTL8152 0x4c00/0x4c10)\n", r->version);
        kfree(r); kfree(u);
        return -1;
    }
    chip_init(r);

    uint8_t mac[8];
    ctl(r, 0, r->version == 0x4c00 ? PLA_IDR : PLA_BACKUP, MCU_PLA, mac, 8);
    memcpy(u->nd.mac, mac, 6);

    if (nic_reset(r, d->speed == USB_SPEED_FULL) != 0) {
        klog("r8152: chip reset failed\n");
        kfree(r); kfree(u);
        return -1;
    }
    if (usb_open_endpoint(d, &e.in) != 0 || usb_open_endpoint(d, &e.out) != 0) {
        kfree(r); kfree(u);
        return -1;
    }
    enable_rx_tx(r, u->nd.mac);
    klog("r8152: RTL8152%s (version %04x), mac %02x:%02x:%02x:%02x:%02x:%02x\n",
         r->version == 0x4c10 ? "B" : "", r->version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    u->dev = d;
    u->priv = r;
    u->ep_in = e.in.bEndpointAddress;
    u->ep_out = e.out.bEndpointAddress;
    u->mps_out = e.out.wMaxPacketSize & 0x7FF;
    u->rx_size = RX_BUFSZ;
    u->tx_size = TX_BUFSZ;
    u->zlp = 1;
    u->decap = rtl_decap;
    u->encap = rtl_encap;
    u->get_link = rtl_get_link;
    u->nd.name = "r8152";
    u->nd.model = r->version == 0x4c10 ? "Realtek RTL8152B (USB)" : "Realtek RTL8152 (USB)";
    d->drvdata = u;
    return usbnet_start(u);
}

static void rtl_detach(usb_device_t* d) {
    if (d->drvdata) usbnet_stop((usbnet_t*)d->drvdata);
}

const usb_driver_t r8152_driver = {
    "r8152", rtl_probe, rtl_attach, NULL, rtl_detach,
};
