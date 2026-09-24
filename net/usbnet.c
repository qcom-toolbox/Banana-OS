#include "usbnet.h"
#include "kstring.h"
#include "timer.h"
#include "serial.h"

static int usbnet_poll(netdev_t* nd) {
    usbnet_t* u = (usbnet_t*)nd;
    usb_poll();                        /* reap controller completions */
    int n = 0;
    for (int i = 0; i < USBNET_RX_N; i++) {
        usb_xfer_t* x = &u->rx[i];
        if (x->status == USB_XFER_PENDING || x->status == USB_XFER_GONE) continue;
        if (x->status == USB_XFER_OK && x->actual) {
            u->decap(u, x->buf, x->actual);
            n++;
        } else if (x->status != USB_XFER_OK) {
            nd->rx_errors++;
        }
        x->len = u->rx_size;
        usb_submit(x);                 /* straight back in the queue */
    }
    return n;
}

static int free_tx(usbnet_t* u) {
    for (int i = 0; i < USBNET_TX_N; i++)
        if (u->tx[i].status != USB_XFER_PENDING && u->txz[i].status != USB_XFER_PENDING) return i;
    return -1;
}

static int usbnet_send(netdev_t* nd, const void* frame, uint32_t len) {
    usbnet_t* u = (usbnet_t*)nd;
    if (!nd->present) return -1;
    int i = free_tx(u);
    uint32_t start = timer_ms();
    while (i < 0) {                    /* all in flight: wait for one (bounded) */
        if (timer_ms() - start > 100) { nd->tx_errors++; return -1; }
        usb_poll();
        i = free_tx(u);
    }
    int total = u->encap(u, u->tx[i].buf, (const uint8_t*)frame, len);
    if (total <= 0) { nd->tx_errors++; return -1; }
    u->tx[i].len = (uint32_t)total;
    if (usb_submit(&u->tx[i]) != 0) { nd->tx_errors++; return -1; }
    /* a transfer that ends on a packet boundary needs a zero-length
     * packet, or the device waits for more data */
    if (u->zlp && u->mps_out && (total % u->mps_out) == 0) {
        u->txz[i].len = 0;
        usb_submit(&u->txz[i]);
    }
    nd->tx_packets++;
    nd->tx_bytes += len;
    return 0;
}

static int usbnet_link_up(netdev_t* nd) {
    usbnet_t* u = (usbnet_t*)nd;
    if (!nd->present) return 0;
    if (u->get_link && timer_ms() - u->link_checked_ms > 1000) {
        u->link = u->get_link(u);
        u->link_checked_ms = timer_ms();
    }
    return u->link;
}

int usbnet_start(usbnet_t* u) {
    for (int i = 0; i < USBNET_RX_N; i++) {
        u->rx[i].dev = u->dev;
        u->rx[i].ep = u->ep_in;
        u->rx[i].buf = (uint8_t*)usb_dma_alloc(u->rx_size);
        u->rx[i].len = u->rx_size;
        if (!u->rx[i].buf) return -1;
    }
    for (int i = 0; i < USBNET_TX_N; i++) {
        u->tx[i].dev = u->txz[i].dev = u->dev;
        u->tx[i].ep = u->txz[i].ep = u->ep_out;
        u->tx[i].buf = (uint8_t*)usb_dma_alloc(u->tx_size);
        u->txz[i].buf = u->tx[i].buf;
        u->tx[i].status = u->txz[i].status = USB_XFER_OK;
        if (!u->tx[i].buf) return -1;
    }
    for (int i = 0; i < USBNET_RX_N; i++) usb_submit(&u->rx[i]);

    u->nd.is_usb = 1;
    u->nd.send = usbnet_send;
    u->nd.poll = usbnet_poll;
    u->nd.link_up = usbnet_link_up;
    u->nd.irq = 0xFF;
    u->link = 1;
    u->link_checked_ms = 0;
    net_register_device(&u->nd);
    return 0;
}

void usbnet_stop(usbnet_t* u) {
    if (!u->nd.present) return;
    net_unregister_device(&u->nd);
}
