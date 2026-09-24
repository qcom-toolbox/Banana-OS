#ifndef NETDEV_H
#define NETDEV_H

#include "types.h"

/* A network interface card driver, as seen by the network stack. Frames
 * are complete Ethernet frames without the FCS (the NIC appends/strips
 * the CRC). Received frames are handed to net_rx() from poll(). */

#define ETH_FRAME_MAX 1514

typedef struct netdev {
    const char* name;          /* driver name, e.g. "e1000" */
    const char* model;         /* human readable chip name */
    char        ifname[8];     /* "eth0", "usb0": assigned by net_register_device() */
    int         is_usb;
    int         present;       /* 0 once a USB adapter was unplugged */
    uint8_t     mac[6];
    uint8_t     irq;           /* legacy PCI IRQ line, 0xFF if none */

    int  (*send)(struct netdev* nd, const void* frame, uint32_t len); /* 0 = queued */
    int  (*poll)(struct netdev* nd);      /* delivers pending RX frames; returns count */
    int  (*link_up)(struct netdev* nd);

    uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped;
    void*    priv;
} netdev_t;

/* Drivers: each returns a ready-to-use device, or NULL if its hardware
 * isn't present. */
netdev_t* e1000_probe(void);
netdev_t* rtl8139_probe(void);

/* Implemented by the stack (net/net.c): one received Ethernet frame. */
void net_rx(netdev_t* nd, const uint8_t* frame, uint32_t len);

/* Drivers found at runtime (USB adapters) announce/withdraw themselves.
 * A newly registered USB adapter becomes the active interface. */
void net_register_device(netdev_t* nd);
void net_unregister_device(netdev_t* nd);

/* Implemented by the stack: a NIC interrupt arrived (wake whoever waits). */
void net_irq_notify(void);

#endif
