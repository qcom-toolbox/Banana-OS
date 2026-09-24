#ifndef USBNET_H
#define USBNET_H

#include "netdev.h"
#include "../usb/usbcore.h"

/* Common plumbing for USB Ethernet adapters (net/r8152.c, net/cdc_ecm.c):
 * a pool of bulk IN transfers kept queued for receiving, a pool of bulk
 * OUT transfers for sending, and the netdev glue. Chip drivers only
 * provide framing (encap/decap) and link status. */

#define USBNET_RX_N 4
#define USBNET_TX_N 8

typedef struct usbnet {
    netdev_t      nd;              /* first: netdev_t* <-> usbnet_t* */
    usb_device_t* dev;
    uint8_t       ep_in, ep_out;
    uint16_t      mps_out;
    uint32_t      rx_size;         /* bytes per bulk IN transfer */
    uint32_t      tx_size;
    int           zlp;             /* end transfers that fill whole packets with a ZLP */
    usb_xfer_t    rx[USBNET_RX_N];
    usb_xfer_t    tx[USBNET_TX_N];
    usb_xfer_t    txz[USBNET_TX_N];
    int           link;            /* cached link state */
    uint32_t      link_checked_ms;

    /* one completed bulk IN buffer: call net_rx() for each frame inside */
    void (*decap)(struct usbnet* u, const uint8_t* buf, uint32_t len);
    /* wrap one frame into out (tx_size bytes max); returns bytes or <0 */
    int  (*encap)(struct usbnet* u, uint8_t* out, const uint8_t* frame, uint32_t len);
    /* current link state (may do control transfers; called at most 1/s) */
    int  (*get_link)(struct usbnet* u);
    void* priv;
} usbnet_t;

/* allocates buffers, queues the receive transfers, registers the netdev */
int  usbnet_start(usbnet_t* u);
void usbnet_stop(usbnet_t* u);

#endif
