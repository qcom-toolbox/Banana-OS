#include "netconf.h"
#include "net.h"
#include "config.h"
#include "fs.h"
#include "kstring.h"
#include "serial.h"

#define HEADER "# Banana OS network settings - written by `ifconfig` and `dhcp`.\n" \
               "# Delete this file (or run `ifconfig reset`) to go back to DHCP.\n"

static struct {
    int   loaded;
    char  iface[8];
    int   is_static;
    ip4_t ip, mask, gw, dns;
} g_cfg;


static ip4_t get_ip(const char* key) {
    char v[20];
    ip4_t ip = 0;
    if (cfg_get(CFG_NETWORK, key, v, sizeof(v))) str_to_ip4(v, &ip);
    return ip;
}

static void load(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.loaded = 1;
    char v[20];
    if (cfg_get(CFG_NETWORK, "iface", v, sizeof(v))) kstrlcpy(g_cfg.iface, v, sizeof(g_cfg.iface));
    if (cfg_get(CFG_NETWORK, "mode", v, sizeof(v)) && strcmp(v, "static") == 0) {
        g_cfg.ip = get_ip("ip");
        g_cfg.mask = get_ip("netmask");
        g_cfg.gw = get_ip("gateway");
        g_cfg.dns = get_ip("dns");
        if (!g_cfg.mask) g_cfg.mask = IP4(255, 255, 255, 0);
        g_cfg.is_static = g_cfg.ip != 0;
    }
}

int netconf_claims(const netdev_t* nd) {
    return g_cfg.loaded && g_cfg.iface[0] && strcmp(g_cfg.iface, nd->ifname) == 0;
}

int netconf_pins_iface(void) {
    return g_cfg.loaded && g_cfg.iface[0];
}

int netconf_is_static(void) {
    return g_cfg.loaded && g_cfg.is_static;
}

void netconf_apply_to(netdev_t* nd) {
    if (!nd) return;
    if (net_if()->dev != nd) net_select_device(nd);     /* starts DHCP */
    if (g_cfg.is_static) {
        net_set_static(g_cfg.ip, g_cfg.mask, g_cfg.gw, g_cfg.dns);
        char ip[16];
        ip4_to_str(g_cfg.ip, ip);
        klog("net: %s: static %s (from %s)\n", nd->ifname, ip, CFG_NETWORK);
    }
}

void netconf_boot(void) {
    load();
    if (fs_find_file(CFG_NETWORK) < 0) return;
    netdev_t* target = NULL;
    for (int i = 0; i < net_device_count(); i++) {
        netdev_t* nd = net_device_at(i);
        if (g_cfg.iface[0] ? strcmp(nd->ifname, g_cfg.iface) == 0 : nd == net_if()->dev) target = nd;
    }
    if (!target && !g_cfg.iface[0]) target = net_if()->dev;
    if (target) netconf_apply_to(target);
    else klog("net: %s not present (yet) - %s applies once it is plugged in\n", g_cfg.iface, CFG_NETWORK);
}

static int save(const netdev_t* nd, int is_static, ip4_t ip, ip4_t mask, ip4_t gw, ip4_t dns) {
    char a[16];
    int bad = 0;
    bad |= cfg_set(CFG_NETWORK, "iface", nd ? nd->ifname : NULL, HEADER);
    bad |= cfg_set(CFG_NETWORK, "mode", is_static ? "static" : "dhcp", HEADER);
    ip4_to_str(ip, a);   bad |= cfg_set(CFG_NETWORK, "ip", is_static ? a : NULL, HEADER);
    ip4_to_str(mask, a); bad |= cfg_set(CFG_NETWORK, "netmask", is_static ? a : NULL, HEADER);
    ip4_to_str(gw, a);   bad |= cfg_set(CFG_NETWORK, "gateway", is_static && gw ? a : NULL, HEADER);
    ip4_to_str(dns, a);  bad |= cfg_set(CFG_NETWORK, "dns", is_static && dns ? a : NULL, HEADER);
    load();
    if (bad) return -1;
    return cfg_persist();
}

int netconf_save_static(const netdev_t* nd, ip4_t ip, ip4_t mask, ip4_t gw, ip4_t dns) {
    return save(nd, 1, ip, mask, gw, dns);
}

int netconf_save_dhcp(const netdev_t* nd) {
    return save(nd, 0, 0, 0, 0, 0);
}

int netconf_forget(void) {
    if (fs_find_file(CFG_NETWORK) >= 0) fs_delete(CFG_NETWORK, 0);
    load();
    return cfg_persist();
}
