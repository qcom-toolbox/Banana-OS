#include "usbnet.h"
#include "kstring.h"
#include "kheap.h"
#include "serial.h"

/*
 * USB CDC-ECM ("Ethernet Control Model", USB CDC spec 1.2 / ECM 1.2):
 * the standard class for USB Ethernet - QEMU's usb-net, many adapters in
 * their class-compliant configuration, phones doing USB tethering. Each
 * bulk transfer carries exactly one Ethernet frame.
 */

#define CDC_SUBCLASS_ECM          0x06
#define CDC_FUNC_UNION            0x06
#define CDC_FUNC_ETHERNET         0x0F
#define CDC_SET_PACKET_FILTER     0x43
#define CDC_FILTER_ALL_MULTICAST  0x02
#define CDC_FILTER_DIRECTED       0x04
#define CDC_FILTER_BROADCAST      0x08

typedef struct {
    int     found;
    uint8_t ctrl_if, data_if, data_alt, mac_index;
    int     in_ctrl, in_data;        /* walking state */
    uint8_t cur_alt;
    usb_ep_desc_t ep_in, ep_out;
    int     have_in, have_out, done;
} ecm_info_t;

static int walk_cb(const uint8_t* d, void* ctx) {
    ecm_info_t* e = (ecm_info_t*)ctx;
    if (e->done) return 1;
    if (d[1] == USB_DT_INTERFACE) {
        const usb_iface_desc_t* id = (const usb_iface_desc_t*)d;
        if (e->have_in && e->have_out) { e->done = 1; return 1; }
        e->have_in = e->have_out = 0;
        e->in_ctrl = (id->bInterfaceClass == 0x02 && id->bInterfaceSubClass == CDC_SUBCLASS_ECM);
        e->in_data = e->found && id->bInterfaceClass == 0x0A && id->bInterfaceNumber == e->data_if;
        if (e->in_ctrl) { e->found = 1; e->ctrl_if = id->bInterfaceNumber; e->data_if = 0xFF; }
        e->cur_alt = id->bAlternateSetting;
    } else if (d[1] == USB_DT_CS_INTERFACE && e->in_ctrl && d[0] >= 3) {
        if (d[2] == CDC_FUNC_UNION && d[0] >= 5) e->data_if = d[4];
        if (d[2] == CDC_FUNC_ETHERNET && d[0] >= 4) e->mac_index = d[3];
    } else if (d[1] == USB_DT_ENDPOINT && e->in_data) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
        if ((ep->bmAttributes & 3) != USB_EP_BULK) return 0;
        if (ep->bEndpointAddress & 0x80) { e->ep_in = *ep; e->have_in = 1; }
        else { e->ep_out = *ep; e->have_out = 1; }
        e->data_alt = e->cur_alt;
        if (e->have_in && e->have_out) { e->done = 1; return 1; }
    }
    return 0;
}

static int parse(usb_device_t* d, int cfg, ecm_info_t* e) {
    memset(e, 0, sizeof(*e));
    usb_walk_config(d, cfg, walk_cb, e);
    return (e->found && e->have_in && e->have_out) ? 0 : -1;
}

static int ecm_probe(usb_device_t* d) {
    ecm_info_t e;
    for (int c = 0; c < USB_MAX_CONFIGS; c++)
        if (d->configs[c] && parse(d, c, &e) == 0) return c;
    return -1;
}

static void ecm_decap(usbnet_t* u, const uint8_t* buf, uint32_t len) {
    if (len < 14 || len > ETH_FRAME_MAX + 4) { u->nd.rx_errors++; return; }
    if (len > ETH_FRAME_MAX) len = ETH_FRAME_MAX;
    u->nd.rx_packets++;
    u->nd.rx_bytes += len;
    net_rx(&u->nd, buf, len);
}

static int ecm_encap(usbnet_t* u, uint8_t* out, const uint8_t* frame, uint32_t len) {
    if (len > u->tx_size) return -1;
    memcpy(out, frame, len);
    return (int)len;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ecm_attach(usb_device_t* d) {
    ecm_info_t e;
    if (parse(d, d->active_config, &e) != 0) return -1;
    usbnet_t* u = (usbnet_t*)kzalloc(sizeof(usbnet_t));
    if (!u) return -1;
    u->dev = d;

    /* MAC address: a 12-hex-digit string descriptor */
    char mac[16];
    int ok = usb_get_string(d, e.mac_index, mac, sizeof(mac)) == 0 && strlen(mac) >= 12;
    for (int i = 0; ok && i < 6; i++) {
        int hi = hexval(mac[i * 2]), lo = hexval(mac[i * 2 + 1]);
        if (hi < 0 || lo < 0) { ok = 0; break; }
        u->nd.mac[i] = (uint8_t)(hi * 16 + lo);
    }
    if (!ok) {   /* made-up but valid: locally administered, unicast */
        static const uint8_t fallback[6] = { 0x02, 0xBA, 0x4A, 0x4A, 0x00, 0x01 };
        memcpy(u->nd.mac, fallback, 6);
    }

    /* the data interface's alternate setting with endpoints turns it on */
    usb_control(d, USB_TYPE_STANDARD | USB_RECIP_IFACE, USB_REQ_SET_INTERFACE, e.data_alt, e.data_if, NULL, 0);
    if (usb_open_endpoint(d, &e.ep_in) != 0 || usb_open_endpoint(d, &e.ep_out) != 0) { kfree(u); return -1; }
    usb_control(d, USB_TYPE_CLASS | USB_RECIP_IFACE, CDC_SET_PACKET_FILTER,
                CDC_FILTER_DIRECTED | CDC_FILTER_BROADCAST | CDC_FILTER_ALL_MULTICAST, e.ctrl_if, NULL, 0);

    u->ep_in = e.ep_in.bEndpointAddress;
    u->ep_out = e.ep_out.bEndpointAddress;
    u->mps_out = e.ep_out.wMaxPacketSize & 0x7FF;
    u->rx_size = 2048;                /* one frame per transfer */
    u->tx_size = 2048;
    u->zlp = 1;
    u->decap = ecm_decap;
    u->encap = ecm_encap;
    u->get_link = NULL;               /* (link notifications ignored: assume up) */
    u->nd.name = "cdc_ecm";
    u->nd.model = "USB CDC Ethernet (ECM)";
    d->drvdata = u;
    return usbnet_start(u);
}

static void ecm_detach(usb_device_t* d) {
    if (d->drvdata) usbnet_stop((usbnet_t*)d->drvdata);
}

const usb_driver_t cdc_ecm_driver = {
    "cdc_ecm", ecm_probe, ecm_attach, NULL, ecm_detach,
};
