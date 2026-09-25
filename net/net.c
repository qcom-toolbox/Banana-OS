#include "net.h"
#include "netconf.h"
#include "tcp.h"
#include "kstring.h"
#include "serial.h"
#include "timer.h"
#include "task.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "random.h"

const uint8_t ETH_BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static netif_t  g_if;
static int      g_netd_pid = -1;
static uint32_t g_waiters;          /* bitmask of pids sleeping in net_wait() */
static uint32_t g_last_timer_ms;
static int      g_in_poll;

#define NET_MAX_DEVS 4
static netdev_t* g_devs[NET_MAX_DEVS];
static int       g_ndevs;

netif_t* net_if(void) { return &g_if; }

const char* net_strerror(int err) {
    switch (err) {
    case NET_OK:           return "success";
    case NET_ERR_TIMEOUT:  return "connection timed out";
    case NET_ERR_RESET:    return "connection reset by peer";
    case NET_ERR_REFUSED:  return "connection refused";
    case NET_ERR_NOROUTE:  return "no route to host";
    case NET_ERR_NOMEM:    return "out of memory";
    case NET_ERR_CLOSED:   return "connection closed";
    case NET_ERR_DNS:      return "could not resolve host";
    case NET_ERR_NOTREADY: return "network is not configured (no DHCP lease yet?)";
    case NET_ERR_NODEV:    return "no network card found";
    case NET_ERR_PROTO:    return "protocol error";
    case NET_ERR_INTR:     return "interrupted";
    default:               return "unknown error";
    }
}

/* ── checksums / formatting ─────────────────────────────────────── */

uint32_t net_csum_add(uint32_t sum, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t net_csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void* data, uint32_t len) {
    return net_csum_fold(net_csum_add(0, data, len));
}

void ip4_to_str(ip4_t ip, char out[16]) {
    ksnprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

int str_to_ip4(const char* s, ip4_t* out) {
    ip4_t v = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t n;
        int used = k_parse_u32(s, &n);
        if (used == 0 || used > 3 || n > 255) return 0;
        v = (v << 8) | n;
        s += used;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

/* ── Ethernet ───────────────────────────────────────────────────── */

int eth_send(const uint8_t dst[6], uint16_t type, const void* payload, uint32_t len) {
    if (!g_if.dev) return NET_ERR_NODEV;
    if (len > ETH_FRAME_MAX - 14) return NET_ERR_PROTO;
    static uint8_t frame[ETH_FRAME_MAX];   /* stack is non-reentrant: see net.h */
    memcpy(frame, dst, 6);
    memcpy(frame + 6, g_if.dev->mac, 6);
    wr16(frame + 12, type);
    memcpy(frame + 14, payload, len);
    return g_if.dev->send(g_if.dev, frame, len + 14) == 0 ? NET_OK : NET_ERR_NOMEM;
}

void net_rx(netdev_t* nd, const uint8_t* frame, uint32_t len) {
    random_add_jitter();
    if (nd != g_if.dev) return;       /* only the active interface talks IP */
    if (len < 14) return;
    uint16_t type = rd16(frame + 12);
    if (type == ETH_TYPE_ARP)     arp_rx(frame + 14, len - 14);
    else if (type == ETH_TYPE_IP) ip_rx(frame + 14, len - 14);
}

/* ── polling / waiting ──────────────────────────────────────────── */

void net_irq_notify(void) {
    /* IRQ context: just make sure someone runs net_poll() soon */
    if (g_netd_pid >= 0) task_wake(g_netd_pid);
    uint32_t w = g_waiters;
    for (int pid = 0; w; pid++, w >>= 1)
        if (w & 1) task_wake(pid);
}

void net_poll(void) {
    if (g_ndevs == 0 || g_in_poll) return;
    g_in_poll = 1;
    /* every card is drained (inactive ones just drop their frames) */
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i]->present) g_devs[i]->poll(g_devs[i]);
    if (!g_if.dev) { g_in_poll = 0; return; }
    ip_loopback_drain();
    uint32_t now = timer_ms();
    if ((uint32_t)(now - g_last_timer_ms) >= 10) {
        g_last_timer_ms = now;
        arp_timer();
        dhcp_timer();
        tcp_timer();
    }
    tcp_flush();   /* delayed ACKs / window updates owed after this batch */
    g_in_poll = 0;
}

void net_wait(uint32_t max_ms) {
    int vt = terminal_vt_get_active();
    net_poll();
    /* keep the desktop alive while a network command blocks a window's shell */
    gui_poll();
    terminal_vt_set_active(vt);
    if (max_ms) {
        int pid = task_current_pid();
        if (pid >= 0 && pid < 32) g_waiters |= (1u << pid);
        task_sleep_ms(max_ms);
        if (pid >= 0 && pid < 32) g_waiters &= ~(1u << pid);
        terminal_vt_set_active(vt);
    }
    net_poll();
}

int net_wait_configured(uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (!g_if.configured) {
        if (!g_if.dev) return 0;
        if ((uint32_t)(timer_ms() - start) >= timeout_ms) return 0;
        if (net_interrupted()) return 0;
        net_wait(20);
    }
    return 1;
}

int net_interrupted(void) {
    /* only the window (or console) the user is typing into can be ^C'd */
    if (gui_focused_vt() != terminal_vt_get_active()) return 0;
    return keyboard_try_getchar() == 3;
}

static void netd_entry(void) {
    for (;;) {
        net_poll();
        /* NIC interrupts cut this short; otherwise it's the TCP/DHCP/ARP
         * timer resolution */
        task_sleep_ms(20);
    }
}

void net_set_static(ip4_t ip, ip4_t mask, ip4_t gw, ip4_t dns) {
    dhcp_stop();   /* or the next lease renewal would overwrite this */
    g_if.ip = ip;
    g_if.netmask = mask;
    g_if.gateway = gw;
    if (dns) g_if.dns = dns;
    g_if.dhcp = 0;
    g_if.configured = (ip != 0);
    g_if.bound_ms = timer_ms();
}

/* ── interfaces ─────────────────────────────────────────────────── */

int net_device_count(void) { return g_ndevs; }
netdev_t* net_device_at(int i) { return (i >= 0 && i < g_ndevs) ? g_devs[i] : NULL; }

void net_select_device(netdev_t* nd) {
    g_if.dev = nd;
    g_if.configured = 0;
    g_if.ip = 0;
    arp_flush();                       /* neighbours of the old network */
    if (!nd) { dhcp_stop(); return; }
    klog("net: using %s (%s)\n", nd->ifname, nd->model);
    dhcp_start();
}

void net_register_device(netdev_t* nd) {
    if (g_ndevs >= NET_MAX_DEVS) return;
    int n = 0;
    for (int i = 0; i < g_ndevs; i++) if (g_devs[i]->is_usb == nd->is_usb) n++;
    ksnprintf(nd->ifname, sizeof(nd->ifname), "%s%d", nd->is_usb ? "usb" : "eth", n);
    nd->present = 1;
    g_devs[g_ndevs++] = nd;
    random_add_entropy(nd->mac, 6);
    klog("net: %s: %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n", nd->ifname, nd->model,
         nd->mac[0], nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
    /* the saved configuration (/etc/network.conf) names this card: use it
     * with its saved addresses. Otherwise a USB adapter plugged in on
     * purpose is used - unless the configuration pins another card. */
    if (netconf_claims(nd)) { netconf_apply_to(nd); return; }
    if (!g_if.dev || (nd->is_usb && !netconf_pins_iface())) net_select_device(nd);
}

void net_unregister_device(netdev_t* nd) {
    int k = 0;
    for (int i = 0; i < g_ndevs; i++) if (g_devs[i] != nd) g_devs[k++] = g_devs[i];
    g_ndevs = k;
    nd->present = 0;
    if (g_if.dev == nd) net_select_device(g_ndevs ? g_devs[0] : NULL);
}

void net_init(void) {
    memset(&g_if, 0, sizeof(g_if));
    netdev_t* nd = e1000_probe();
    if (nd) net_register_device(nd);
    nd = rtl8139_probe();
    if (nd) net_register_device(nd);
    if (!g_ndevs) klog("net: no PCI network card found\n");
    /* always running: a USB adapter may show up later */
    g_netd_pid = task_create("netd", netd_entry);
}
