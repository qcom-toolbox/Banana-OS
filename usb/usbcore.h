#ifndef USBCORE_H
#define USBCORE_H

#include "types.h"

/*
 * Banana OS USB stack.
 *
 *   usbcore.c   device enumeration, descriptors, driver matching, lsusb
 *   xhci.c      xHCI (USB 3.x) host controller driver
 *   ehci.c      EHCI (USB 2.0) host controller driver
 *   usbhid.c    boot-protocol keyboard + mouse
 *   (net/r8152.c, net/cdc_ecm.c: USB Ethernet adapters)
 *
 * Devices directly on a root port are supported (no external hubs yet).
 * Everything is polled from task context (usb_poll()); controller
 * interrupts only wake sleeping tasks.
 */

#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* standard requests */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE     0x0B

#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_STRING    3
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT  5
#define USB_DT_CS_INTERFACE 0x24

/* bmRequestType */
#define USB_DIR_IN        0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_TYPE_VENDOR   0x40
#define USB_RECIP_DEVICE  0x00
#define USB_RECIP_IFACE   0x01
#define USB_RECIP_EP      0x02

#define USB_EP_CONTROL   0
#define USB_EP_ISOCH     1
#define USB_EP_BULK      2
#define USB_EP_INTERRUPT 3

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength, bDescriptorType;
    uint8_t bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} usb_iface_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_ep_desc_t;

#define USB_MAX_CONFIGS 4
#define USB_MAX_DEVICES 16

struct usb_hc;
struct usb_driver;

typedef struct usb_device {
    int               used;
    int               present;        /* 0 once unplugged */
    struct usb_hc*    hc;
    void*             hcpriv;         /* controller's per-device state */
    int               port;           /* root port, 1-based */
    int               speed;          /* USB_SPEED_* */
    uint8_t           address;        /* USB address / xHCI slot */
    uint16_t          ep0_mps;
    usb_device_desc_t desc;
    uint8_t*          configs[USB_MAX_CONFIGS];   /* full config descriptors */
    int               active_config;  /* index into configs, -1 = none */
    char              manufacturer[40];
    char              product[48];
    const struct usb_driver* driver;
    void*             drvdata;
} usb_device_t;

/* One bulk / interrupt transfer. Submitted with usb_submit(); `status`
 * leaves USB_XFER_PENDING once the controller finished it. */
#define USB_XFER_PENDING 1
#define USB_XFER_OK      0
#define USB_XFER_STALL   (-2)
#define USB_XFER_ERROR   (-3)
#define USB_XFER_GONE    (-4)

typedef struct usb_xfer {
    usb_device_t*  dev;
    uint8_t        ep;        /* endpoint address, bit 7 = IN */
    uint8_t*       buf;       /* DMA-able (usb_dma_alloc) */
    uint32_t       len;
    uint32_t       actual;
    volatile int   status;
    void*          hcpriv;
} usb_xfer_t;

typedef struct usb_hc_ops {
    /* control transfer on EP0; returns bytes transferred or <0 */
    int  (*control)(usb_device_t* d, const usb_setup_t* s, void* data, uint32_t timeout_ms);
    /* change EP0's max packet size once the real one is known */
    int  (*set_ep0_mps)(usb_device_t* d, uint16_t mps);
    /* make an endpoint usable (xHCI: Configure Endpoint) */
    int  (*open_endpoint)(usb_device_t* d, const usb_ep_desc_t* ep);
    /* queue a bulk/interrupt transfer */
    int  (*submit)(usb_xfer_t* x);
    /* process completions / port changes */
    void (*poll)(struct usb_hc* hc);
    /* re-check root ports, enumerating new devices */
    void (*rescan)(struct usb_hc* hc);
} usb_hc_ops_t;

typedef struct usb_hc {
    const char*         name;       /* "xhci", "ehci" */
    const usb_hc_ops_t* ops;
    void*               priv;
    int                 ports;
    char                desc[64];   /* for lsusb */
    volatile int        port_change; /* set by poll(): usb_poll() will rescan */
} usb_hc_t;

typedef struct usb_driver {
    const char* name;
    /* returns the config index to use, or -1 if not ours */
    int  (*probe)(usb_device_t* d);
    /* after SET_CONFIGURATION: 0 = bound */
    int  (*attach)(usb_device_t* d);
    /* called from usb_poll() */
    void (*poll)(usb_device_t* d);
    void (*detach)(usb_device_t* d);
} usb_driver_t;

/* ── core API ─────────────────────────────────────────────────── */
void usb_stack_init(void);                 /* find controllers, enumerate */
void usb_poll(void);                       /* safe to call often */
void usb_rescan(void);
int  usb_register_hc(usb_hc_t* hc);
/* HC drivers call this for a newly addressed device on a root port */
usb_device_t* usb_new_device(usb_hc_t* hc, int port, int speed, uint8_t address,
                             uint16_t ep0_mps, void* hcpriv);
void usb_device_gone(usb_device_t* d);

int usb_control(usb_device_t* d, uint8_t reqtype, uint8_t req, uint16_t value,
                uint16_t index, void* data, uint16_t len);
int usb_submit(usb_xfer_t* x);
int usb_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep);

/* descriptor walking inside d->configs[cfg]: calls cb for each descriptor
 * (header byte 0 = length, byte 1 = type); stop early with non-zero */
int usb_walk_config(usb_device_t* d, int cfg, int (*cb)(const uint8_t* desc, void* ctx), void* ctx);
/* ASCII of a string descriptor (0 on success) */
int usb_get_string(usb_device_t* d, uint8_t index, char* out, uint32_t cap);

/* 4 KiB-aligned zeroed memory for DMA (never crosses a 64 KiB boundary
 * when size <= 64 KiB) */
void* usb_dma_alloc(uint32_t size);

void usb_list(void);                      /* lsusb */
int  usb_device_count(void);

/* delays for the controller drivers */
void usb_delay_ms(uint32_t ms);

#endif
