#include "usbcore.h"
#include "pci.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "serial.h"
#include "terminal.h"

/* host controller drivers */
int xhci_init_controller(const pci_dev_t* pd);
int ehci_init_controller(const pci_dev_t* pd);

/* device drivers, tried in this order */
extern const usb_driver_t r8152_driver;
extern const usb_driver_t cdc_ecm_driver;
extern const usb_driver_t hid_driver;
extern const usb_driver_t usb_storage_driver;
static const usb_driver_t* const g_drivers[] = {
    &r8152_driver, &cdc_ecm_driver, &hid_driver, &usb_storage_driver,
};
#define DRIVER_COUNT ((int)(sizeof(g_drivers) / sizeof(g_drivers[0])))

#define MAX_HC 4
static usb_hc_t*    g_hcs[MAX_HC];
static int          g_hc_count;
static usb_device_t g_devs[USB_MAX_DEVICES];
static int          g_in_poll;

void usb_delay_ms(uint32_t ms) {
    timer_sleep_ms(ms);
}

void* usb_dma_alloc(uint32_t size) {
    /* align to the next power of two >= 4 KiB: a buffer aligned to its own
     * (power-of-two) size can never straddle a 64 KiB boundary, which
     * both xHCI TRBs and EHCI qTD pages care about */
    uint32_t align = 4096;
    while (align < size && align < 65536) align <<= 1;
    uint8_t* raw = (uint8_t*)kmalloc(size + align);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uint32_t)(uintptr_t)raw + align - 1) & ~(align - 1));
    memset(p, 0, size);
    return p;
}

int usb_register_hc(usb_hc_t* hc) {
    if (g_hc_count >= MAX_HC) return -1;
    g_hcs[g_hc_count++] = hc;
    return 0;
}

int usb_device_count(void) {
    int n = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_devs[i].used && g_devs[i].present) n++;
    return n;
}

/* ── requests ───────────────────────────────────────────────────── */

int usb_control(usb_device_t* d, uint8_t reqtype, uint8_t req, uint16_t value,
                uint16_t index, void* data, uint16_t len) {
    if (!d->present) return -1;
    usb_setup_t s = { reqtype, req, value, index, len };
    return d->hc->ops->control(d, &s, data, 2000);
}

int usb_submit(usb_xfer_t* x) {
    if (!x->dev->present) { x->status = USB_XFER_GONE; return -1; }
    x->status = USB_XFER_PENDING;
    x->actual = 0;
    return x->dev->hc->ops->submit(x);
}

int usb_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) {
    return d->hc->ops->open_endpoint(d, ep);
}

static int get_descriptor(usb_device_t* d, uint8_t type, uint8_t index, uint16_t lang, void* buf, uint16_t len) {
    return usb_control(d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)((type << 8) | index), lang, buf, len);
}

int usb_get_string(usb_device_t* d, uint8_t index, char* out, uint32_t cap) {
    out[0] = '\0';
    if (!index || cap == 0) return -1;
    uint8_t buf[255];
    int n = get_descriptor(d, USB_DT_STRING, 0, 0, buf, 4);   /* language ids */
    uint16_t lang = (n >= 4) ? (uint16_t)(buf[2] | (buf[3] << 8)) : 0x0409;
    n = get_descriptor(d, USB_DT_STRING, index, lang, buf, sizeof(buf));
    if (n < 2 || buf[1] != USB_DT_STRING) return -1;
    uint32_t chars = (uint32_t)(buf[0] < n ? buf[0] : n);
    uint32_t o = 0;
    for (uint32_t i = 2; i + 1 < chars && o + 1 < cap; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        out[o++] = (c >= 32 && c < 127) ? (char)c : '?';   /* UTF-16LE -> ASCII */
    }
    out[o] = '\0';
    return 0;
}

int usb_walk_config(usb_device_t* d, int cfg, int (*cb)(const uint8_t* desc, void* ctx), void* ctx) {
    if (cfg < 0 || cfg >= USB_MAX_CONFIGS || !d->configs[cfg]) return 0;
    const uint8_t* p = d->configs[cfg];
    uint32_t total = ((const usb_config_desc_t*)p)->wTotalLength;
    for (uint32_t off = 0; off + 2 <= total;) {
        uint8_t l = p[off];
        if (l < 2 || off + l > total) break;
        int r = cb(p + off, ctx);
        if (r) return r;
        off += l;
    }
    return 0;
}

/* ── enumeration ────────────────────────────────────────────────── */

static const char* speed_name(int s) {
    switch (s) {
    case USB_SPEED_LOW:   return "low-speed";
    case USB_SPEED_FULL:  return "full-speed";
    case USB_SPEED_HIGH:  return "high-speed";
    case USB_SPEED_SUPER: return "SuperSpeed";
    }
    return "?";
}

static int enumerate(usb_device_t* d) {
    uint8_t buf[18];
    /* the first 8 bytes tell us EP0's real max packet size */
    int n = get_descriptor(d, USB_DT_DEVICE, 0, 0, buf, 8);
    if (n < 8) { klog("usb: port %d: no device descriptor (%d)\n", d->port, n); return -1; }
    uint16_t mps = (d->speed == USB_SPEED_SUPER) ? (uint16_t)(1u << buf[7]) : buf[7];
    if (mps && mps != d->ep0_mps && d->hc->ops->set_ep0_mps) {
        d->hc->ops->set_ep0_mps(d, mps);
        d->ep0_mps = mps;
    }
    n = get_descriptor(d, USB_DT_DEVICE, 0, 0, &d->desc, 18);
    if (n < 18) return -1;

    for (int c = 0; c < d->desc.bNumConfigurations && c < USB_MAX_CONFIGS; c++) {
        usb_config_desc_t cd;
        if (get_descriptor(d, USB_DT_CONFIG, (uint8_t)c, 0, &cd, 9) < 9) break;
        uint16_t total = cd.wTotalLength;
        if (total < 9 || total > 4096) break;
        uint8_t* full = (uint8_t*)kmalloc(total);
        if (!full) break;
        if (get_descriptor(d, USB_DT_CONFIG, (uint8_t)c, 0, full, total) < total) { kfree(full); break; }
        d->configs[c] = full;
    }
    usb_get_string(d, d->desc.iManufacturer, d->manufacturer, sizeof(d->manufacturer));
    usb_get_string(d, d->desc.iProduct, d->product, sizeof(d->product));
    klog("usb: %s port %d: %04x:%04x %s %s (%s)\n", d->hc->name, d->port, d->desc.idVendor,
         d->desc.idProduct, d->manufacturer, d->product, speed_name(d->speed));
    return 0;
}

static void bind_driver(usb_device_t* d) {
    for (int i = 0; i < DRIVER_COUNT; i++) {
        const usb_driver_t* drv = g_drivers[i];
        int cfg = drv->probe(d);
        if (cfg < 0 || cfg >= USB_MAX_CONFIGS || !d->configs[cfg]) continue;
        uint8_t value = ((usb_config_desc_t*)d->configs[cfg])->bConfigurationValue;
        if (usb_control(d, USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                        value, 0, NULL, 0) < 0) {
            klog("usb: SET_CONFIGURATION %u failed\n", value);
            continue;
        }
        d->active_config = cfg;
        if (drv->attach(d) == 0) {
            d->driver = drv;
            klog("usb: %04x:%04x bound to %s (config %u)\n", d->desc.idVendor, d->desc.idProduct,
                 drv->name, value);
            return;
        }
        klog("usb: %s declined %04x:%04x\n", drv->name, d->desc.idVendor, d->desc.idProduct);
    }
}

usb_device_t* usb_new_device(usb_hc_t* hc, int port, int speed, uint8_t address,
                             uint16_t ep0_mps, void* hcpriv) {
    usb_device_t* d = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (!g_devs[i].used) { d = &g_devs[i]; break; }
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->present = 1;
    d->hc = hc;
    d->hcpriv = hcpriv;
    d->port = port;
    d->speed = speed;
    d->address = address;
    d->ep0_mps = ep0_mps;
    d->active_config = -1;
    if (enumerate(d) != 0) {
        d->present = 0;
        return d;
    }
    bind_driver(d);
    return d;
}

void usb_device_gone(usb_device_t* d) {
    if (!d || !d->used) return;
    klog("usb: %04x:%04x on port %d disconnected\n", d->desc.idVendor, d->desc.idProduct, d->port);
    d->present = 0;
    if (d->driver && d->driver->detach) d->driver->detach(d);
    d->driver = NULL;
    for (int c = 0; c < USB_MAX_CONFIGS; c++) { kfree(d->configs[c]); d->configs[c] = NULL; }
    d->used = 0;
}

/* ── polling / startup ──────────────────────────────────────────── */

void usb_poll(void) {
    if (g_in_poll) return;
    g_in_poll = 1;
    for (int i = 0; i < g_hc_count; i++) g_hcs[i]->ops->poll(g_hcs[i]);
    /* hotplug: a controller saw a port change - (re)enumerate its ports */
    for (int i = 0; i < g_hc_count; i++) {
        if (g_hcs[i]->port_change && g_hcs[i]->ops->rescan) {
            g_hcs[i]->port_change = 0;
            g_hcs[i]->ops->rescan(g_hcs[i]);
        }
    }
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* d = &g_devs[i];
        if (d->used && d->present && d->driver && d->driver->poll) d->driver->poll(d);
    }
    g_in_poll = 0;
}

void usb_rescan(void) {
    if (g_in_poll) return;
    g_in_poll = 1;
    for (int i = 0; i < g_hc_count; i++)
        if (g_hcs[i]->ops->rescan) g_hcs[i]->ops->rescan(g_hcs[i]);
    g_in_poll = 0;
}

typedef struct { pci_dev_t list[8]; int n; uint8_t prog_if; } hc_scan_t;

static int scan_cb(const pci_dev_t* d, void* ctx) {
    hc_scan_t* s = (hc_scan_t*)ctx;
    if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == s->prog_if && s->n < 8)
        s->list[s->n++] = *d;
    return 0;
}

void usb_stack_init(void) {
    /* xHCI first: on Intel chipsets taking it over also switches the
     * shared USB 2.0 ports from the EHCI controller to it */
    hc_scan_t s;
    s.n = 0; s.prog_if = 0x30;
    pci_scan(scan_cb, &s);
    for (int i = 0; i < s.n; i++) xhci_init_controller(&s.list[i]);
    s.n = 0; s.prog_if = 0x20;
    pci_scan(scan_cb, &s);
    for (int i = 0; i < s.n; i++) ehci_init_controller(&s.list[i]);
}

/* ── lsusb ──────────────────────────────────────────────────────── */

void usb_list(void) {
    char line[160];
    if (g_hc_count == 0) {
        terminal_writeln("no USB host controller in use (xHCI and EHCI are supported)");
        return;
    }
    for (int h = 0; h < g_hc_count; h++) {
        ksnprintf(line, sizeof(line), "Bus %03d: %s", h + 1, g_hcs[h]->desc);
        terminal_writeln(line);
        int any = 0;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            usb_device_t* d = &g_devs[i];
            if (!d->used || !d->present || d->hc != g_hcs[h]) continue;
            ksnprintf(line, sizeof(line), "  Port %d: ID %04x:%04x %s %s  [%s%s%s]", d->port,
                      d->desc.idVendor, d->desc.idProduct,
                      d->manufacturer[0] ? d->manufacturer : "", d->product[0] ? d->product : "(unnamed)",
                      speed_name(d->speed), d->driver ? ", driver: " : ", no driver",
                      d->driver ? d->driver->name : "");
            terminal_writeln(line);
            any = 1;
        }
        if (!any) terminal_writeln("  (no devices)");
    }
}
