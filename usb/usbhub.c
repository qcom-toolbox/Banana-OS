#include "usbcore.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/*
 * USB hubs (class 9): external ones, and the "rate matching hub" that
 * Intel chipsets since 2011 put behind every EHCI port (there, every
 * device - mouse, keyboard, stick - is on a hub).
 *
 * The hub's ports are powered once, then looked at every 250 ms from the
 * usbd task (GET_PORT_STATUS; the hub's own interrupt endpoint is not
 * needed for that): a new connection is debounced, the port reset, its
 * speed read, and the controller asked to address and enumerate the
 * device (ops->attach_child) - which may itself be a hub. A disconnection
 * removes the device (and everything behind it, if it was a hub).
 */

#define HUB_MAX_PORTS 15

/* port features and status bits (USB 2.0, 11.24.2.7) */
#define F_PORT_RESET        4
#define F_PORT_POWER        8
#define F_C_PORT_CONNECTION 16
#define F_C_PORT_ENABLE     17
#define F_C_PORT_SUSPEND    18
#define F_C_PORT_OVERCURR   19
#define F_C_PORT_RESET      20
#define ST_CONNECTION (1u << 0)
#define ST_ENABLE     (1u << 1)
#define ST_LOW_SPEED  (1u << 9)
#define ST_HIGH_SPEED (1u << 10)
#define CH_CONNECTION (1u << 0)
#define CH_RESET      (1u << 4)

#define RT_PORT_OUT (USB_TYPE_CLASS | 0x03)              /* class, "other" (a port) */
#define RT_PORT_IN  (USB_DIR_IN | USB_TYPE_CLASS | 0x03)

typedef struct {
    int           nports;
    usb_device_t* child[HUB_MAX_PORTS + 1];
    uint32_t      next_ms;
} hub_t;

static int hub_probe(usb_device_t* d) {
    if (d->desc.bDeviceClass == 9) return 0;
    /* (a hub may say so only in its interface) */
    if (d->configs[0]) {
        const uint8_t* p = d->configs[0];
        uint32_t total = ((const usb_config_desc_t*)p)->wTotalLength;
        for (uint32_t off = 0; off + 9 <= total && p[off] >= 2; off += p[off])
            if (p[off + 1] == USB_DT_INTERFACE && p[off + 5] == 9) return 0;
    }
    return -1;
}

static int port_status(usb_device_t* d, int port, uint32_t* status) {
    uint8_t b[4];
    if (usb_control(d, RT_PORT_IN, USB_REQ_GET_STATUS, 0, (uint16_t)port, b, 4) < 4) return -1;
    *status = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

static void port_feature(usb_device_t* d, int set, int feature, int port) {
    usb_control(d, RT_PORT_OUT, set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE, (uint16_t)feature,
                (uint16_t)port, NULL, 0);
}

static int hub_attach(usb_device_t* d) {
    uint8_t desc[16];
    int n = usb_control(d, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                        0x2900, 0, desc, sizeof(desc));
    if (n < 7) { klog("usb-hub: no hub descriptor\n"); return -1; }
    hub_t* h = (hub_t*)kzalloc(sizeof(hub_t));
    if (!h) return -1;
    h->nports = desc[2] > HUB_MAX_PORTS ? HUB_MAX_PORTS : desc[2];
    d->drvdata = h;
    if (!d->hc->ops->attach_child) {
        klog("usb-hub: %d ports - hubs on %s are not supported yet: nothing behind it is used\n",
             h->nports, d->hc->name);
        return 0;
    }
    for (int p = 1; p <= h->nports; p++) port_feature(d, 1, F_PORT_POWER, p);
    usb_delay_ms((uint32_t)desc[5] * 2u + 20u);      /* power-on to power-good */
    klog("usb-hub: %d ports (%s)\n", h->nports, d->speed == USB_SPEED_HIGH ? "high-speed" : "full-speed");
    h->next_ms = timer_ms();                         /* look at the ports right away */
    return 0;
}

static void child_gone(usb_device_t* d, hub_t* h, int p) {
    usb_device_t* c = h->child[p];
    h->child[p] = NULL;
    if (!c) return;
    usb_device_gone(c);                              /* (a hub child takes its own children) */
    if (d->hc->ops->detach_child) d->hc->ops->detach_child(c);
}

/* a device on port p: reset it, then let the controller take it */
static void port_connect(usb_device_t* d, hub_t* h, int p) {
    usb_delay_ms(100);                               /* debounce */
    uint32_t st;
    if (port_status(d, p, &st) != 0 || !(st & ST_CONNECTION)) return;
    port_feature(d, 1, F_PORT_RESET, p);
    uint32_t start = timer_ms();
    for (;;) {
        usb_delay_ms(10);
        if (port_status(d, p, &st) != 0) return;
        if (st & (CH_RESET << 16)) break;
        if (timer_ms() - start > 500) { klog("usb-hub: port %d does not finish its reset\n", p); return; }
    }
    port_feature(d, 0, F_C_PORT_RESET, p);
    usb_delay_ms(10);                                /* reset recovery */
    if (port_status(d, p, &st) != 0 || !(st & ST_ENABLE)) { klog("usb-hub: port %d not enabled\n", p); return; }
    int speed = (st & ST_LOW_SPEED) ? USB_SPEED_LOW : (st & ST_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    h->child[p] = d->hc->ops->attach_child(d->hc, d, p, speed);
    if (!h->child[p]) klog("usb-hub: the device on port %d could not be set up\n", p);
}

static void hub_poll(usb_device_t* d) {
    hub_t* h = (hub_t*)d->drvdata;
    if (!h || !d->hc->ops->attach_child || !usb_in_usbd()) return;   /* (enumeration: usbd only) */
    if ((int32_t)(timer_ms() - h->next_ms) < 0) return;
    h->next_ms = timer_ms() + 250;
    for (int p = 1; p <= h->nports && d->present; p++) {
        uint32_t st;
        if (port_status(d, p, &st) != 0) continue;
        uint32_t ch = st >> 16;
        if (ch & CH_CONNECTION) port_feature(d, 0, F_C_PORT_CONNECTION, p);
        if (ch & (1u << 1)) port_feature(d, 0, F_C_PORT_ENABLE, p);
        if (ch & (1u << 2)) port_feature(d, 0, F_C_PORT_SUSPEND, p);
        if (ch & (1u << 3)) port_feature(d, 0, F_C_PORT_OVERCURR, p);
        if (ch & CH_RESET) port_feature(d, 0, F_C_PORT_RESET, p);
        int connected = (st & ST_CONNECTION) != 0;
        if (h->child[p] && (!connected || (ch & CH_CONNECTION) || !h->child[p]->present)) child_gone(d, h, p);
        if (connected && !h->child[p] && ((ch & CH_CONNECTION) || !(st & ST_ENABLE))) port_connect(d, h, p);
    }
}

static void hub_detach(usb_device_t* d) {
    hub_t* h = (hub_t*)d->drvdata;
    if (!h) return;
    for (int p = 1; p <= h->nports; p++) child_gone(d, h, p);
    kfree(h);
    d->drvdata = NULL;
}

const usb_driver_t hub_driver = {
    "usb-hub", hub_probe, hub_attach, hub_poll, hub_detach,
};
