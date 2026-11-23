/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "qga.h"
#include "virtio_serial.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "rtc.h"
#include "tz.h"
#include "serial.h"
#include "smp.h"
#include "fs.h"
#include "fsdisk.h"
#include "../net/net.h"
#include "../net/ipv6.h"

/*
 * The QEMU guest agent's protocol (qemu-ga, docs/interop/qemu-ga-ref): JSON
 * commands from the host on the virtio-serial port org.qemu.guest_agent.0,
 * one JSON reply each. Proxmox VE uses it to show the VM's addresses, to
 * shut it down or restart it cleanly, to set its clock after a resume and to
 * freeze its filesystem while a backup is made. What runs commands or files
 * (guest-exec, guest-file-*) is left out: the host gets no shell this way.
 */

#define PORT_NAME "org.qemu.guest_agent.0"
#define IN_MAX    (64u * 1024u)
#define OUT_MAX   (16u * 1024u)

static int       g_port = -1;
static char*     g_in;
static uint32_t  g_in_len;
static char*     g_out;
static uint32_t  g_out_len;
static uint32_t  g_cmds;
static char      g_last[48];
static int       g_power_mode;          /* guest-shutdown: 0 powerdown/halt, 1 reboot */

/* ── a small JSON writer ─────────────────────────────────────────────── */

static void put(const char* s) {
    while (*s && g_out_len + 1 < OUT_MAX) g_out[g_out_len++] = *s++;
    g_out[g_out_len] = 0;
}

static void putf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void putf(const char* fmt, ...) {
    char b[160];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    put(b);
}

static void put_str(const char* s) {
    put("\"");
    for (; *s && g_out_len + 8 < OUT_MAX; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[3] = { '\\', (char)c, 0 }; put(e); }
        else if (c < 0x20) putf("\\u%04x", c);
        else { char e[2] = { (char)c, 0 }; put(e); }
    }
    put("\"");
}

static void send_out(void) {
    put("\n");
    vser_write(g_port, g_out, g_out_len);
    g_out_len = 0;
    g_out[0] = 0;
}

static void error_reply(const char* cls, const char* desc) {
    put("{\"error\":{\"class\":");
    put_str(cls);
    put(",\"desc\":");
    put_str(desc);
    put("}}");
    send_out();
}

/* ── reading the request ─────────────────────────────────────────────── */

/* the value of "key" in the object at o (the first one anywhere in it: the
 * keys this agent reads are unique within a request) - points at it, or NULL */
static const char* find_key(const char* o, uint32_t len, const char* key) {
    uint32_t kl = (uint32_t)strlen(key);
    for (uint32_t i = 0; i + kl + 2 < len; i++) {
        if (o[i] != '"' || memcmp(o + i + 1, key, kl) != 0 || o[i + 1 + kl] != '"') continue;
        uint32_t j = i + kl + 2;
        while (j < len && (o[j] == ' ' || o[j] == '\t' || o[j] == '\n' || o[j] == '\r')) j++;
        if (j >= len || o[j] != ':') continue;
        j++;
        while (j < len && (o[j] == ' ' || o[j] == '\t' || o[j] == '\n' || o[j] == '\r')) j++;
        return j < len ? o + j : NULL;
    }
    return NULL;
}

static int get_str(const char* o, uint32_t len, const char* key, char* out, int cap) {
    const char* v = find_key(o, len, key);
    if (!v || *v != '"') return 0;
    v++;
    int n = 0;
    while (v < o + len && *v != '"' && n < cap - 1) {
        if (*v == '\\' && v + 1 < o + len) v++;
        out[n++] = *v++;
    }
    out[n] = 0;
    return 1;
}

static int get_num(const char* o, uint32_t len, const char* key, int64_t* out) {
    const char* v = find_key(o, len, key);
    if (!v) return 0;
    int neg = 0;
    if (*v == '-') { neg = 1; v++; }
    if (*v < '0' || *v > '9') return 0;
    int64_t n = 0;
    while (v < o + len && *v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    *out = neg ? -n : n;
    return 1;
}

/* ── the commands ────────────────────────────────────────────────────── */

static const char* const CMDS[] = {
    "guest-sync-delimited", "guest-sync", "guest-ping", "guest-info", "guest-get-osinfo",
    "guest-get-host-name", "guest-get-time", "guest-set-time", "guest-get-timezone",
    "guest-network-get-interfaces", "guest-get-vcpus", "guest-get-fsinfo", "guest-get-users",
    "guest-shutdown", "guest-fsfreeze-status", "guest-fsfreeze-freeze", "guest-fsfreeze-freeze-list",
    "guest-fsfreeze-thaw", "guest-fstrim", NULL
};

static void cmd_info(void) {
    put("{\"return\":{\"version\":\"8.2.0-banana\",\"supported_commands\":[");
    for (int i = 0; CMDS[i]; i++) {
        if (i) put(",");
        put("{\"enabled\":true,\"name\":");
        put_str(CMDS[i]);
        put(",\"success-response\":");
        put(strcmp(CMDS[i], "guest-shutdown") == 0 ? "false}" : "true}");
    }
    put("]}}");
    send_out();
}

static void hostname(char* out, int cap) {
    kstrlcpy(out, "banana-os", (size_t)cap);
    int f = fs_find_file("/etc/hostname");
    fs_file_t* fi = f >= 0 ? fs_get_file(f) : NULL;
    if (!fi || !fi->content || !fi->size) return;
    int n = 0;
    for (uint32_t i = 0; i < fi->size && n < cap - 1; i++) {
        char c = fi->content[i];
        if (c == '\n' || c == '\r' || c == 0) break;
        out[n++] = c;
    }
    if (n) out[n] = 0;
}

static void cmd_osinfo(void) {
#if defined(__x86_64__)
    const char* machine = "x86_64";
#else
    const char* machine = "i686";
#endif
    put("{\"return\":{\"id\":\"banana-os\",\"name\":\"Banana OS\",\"pretty-name\":\"Banana OS 0.6\","
        "\"version\":\"0.6\",\"version-id\":\"0.6\",\"kernel-release\":\"0.6\",\"kernel-version\":\"Banana OS kernel\",\"machine\":");
    put_str(machine);
    put("}}");
    send_out();
}

static void put_ip4(ip4_t ip) {
    putf("\"%u.%u.%u.%u\"", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
}

static int prefix_of(ip4_t mask) {
    int n = 0;
    while (mask & 0x80000000u) { n++; mask <<= 1; }
    return n;
}

static void put_ip6(const ip6_t* a, int prefix) {
    char b[48];
    int n = 0;
    for (int i = 0; i < 16; i += 2) {
        n += ksnprintf(b + n, sizeof(b) - (size_t)n, "%s%x", i ? ":" : "", (unsigned)((a->b[i] << 8) | a->b[i + 1]));
    }
    put("{\"ip-address-type\":\"ipv6\",\"ip-address\":");
    put_str(b);
    putf(",\"prefix\":%d}", prefix);
}

static void cmd_interfaces(void) {
    netif_t* nif = net_if();
    put("{\"return\":[{\"name\":\"lo\",\"hardware-address\":\"00:00:00:00:00:00\",\"ip-addresses\":"
        "[{\"ip-address-type\":\"ipv4\",\"ip-address\":\"127.0.0.1\",\"prefix\":8}]}");
    for (int i = 0; i < net_device_count(); i++) {
        netdev_t* d = net_device_at(i);
        if (!d) continue;
        putf(",{\"name\":\"eth%d\",\"hardware-address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip-addresses\":[", i,
             d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        int any = 0;
        if (nif && nif->dev == d && nif->configured) {
            put("{\"ip-address-type\":\"ipv4\",\"ip-address\":");
            put_ip4(nif->ip);
            putf(",\"prefix\":%d}", prefix_of(nif->netmask));
            any = 1;
            if (ipv6_enabled()) {
                ip6_t a;
                int pl = 64;
                if (ipv6_link_local(&a)) { put(any ? "," : ""); put_ip6(&a, 64); any = 1; }
                if (ipv6_global(&a, &pl)) { put(any ? "," : ""); put_ip6(&a, pl); any = 1; }
            }
        }
        put("]}");
    }
    put("]}");
    send_out();
}

static void cmd_vcpus(void) {
    put("{\"return\":[");
    int n = cpu_count();
    for (int i = 0; i < n; i++) putf("%s{\"logical-id\":%d,\"online\":true,\"can-offline\":false}", i ? "," : "", i);
    put("]}");
    send_out();
}

static void cmd_fsinfo(void) {
    uint64_t used = 0, total = 0;
    int inst = fsdisk_is_installed();
    if (inst) fsdisk_space(&used, &total);
    else used = fs_ram_used_bytes();
    put("{\"return\":[{\"name\":\"banana0\",\"mountpoint\":\"/\",\"type\":");
    put_str(inst ? fsdisk_fs_type() : "tmpfs");
    putf(",\"used-bytes\":%llu,\"total-bytes\":%llu,\"disk\":[]}]}", (unsigned long long)used,
         (unsigned long long)(total ? total : used));
    send_out();
}

static void power_task(void) {
    extern void shell_power(int restart);
    task_sleep_ms(300);                          /* (the host sees the request taken) */
    klog("qga: %s requested by the host\n", g_power_mode ? "restart" : "shut down");
    shell_power(g_power_mode);
}

static void handle(const char* o, uint32_t len) {
    char cmd[48];
    if (!get_str(o, len, "execute", cmd, sizeof(cmd))) {
        error_reply("GenericError", "Invalid JSON: no \"execute\" member");
        return;
    }
    g_cmds++;
    kstrlcpy(g_last, cmd, sizeof(g_last));
    int64_t n;
    char s[24];
    if (strcmp(cmd, "guest-sync-delimited") == 0 || strcmp(cmd, "guest-sync") == 0) {
        if (!get_num(o, len, "id", &n)) { error_reply("GenericError", "Parameter 'id' is missing"); return; }
        if (cmd[10] == '-') vser_write(g_port, "\xff", 1);           /* the delimited kind */
        putf("{\"return\":%lld}", (long long)n);
        send_out();
    } else if (strcmp(cmd, "guest-ping") == 0 || strcmp(cmd, "guest-fstrim") == 0) {
        put(cmd[6] == 'f' ? "{\"return\":{\"paths\":[]}}" : "{\"return\":{}}");
        send_out();
    } else if (strcmp(cmd, "guest-info") == 0) {
        cmd_info();
    } else if (strcmp(cmd, "guest-get-osinfo") == 0) {
        cmd_osinfo();
    } else if (strcmp(cmd, "guest-get-host-name") == 0) {
        char h[64];
        hostname(h, sizeof(h));
        put("{\"return\":{\"host-name\":");
        put_str(h);
        put("}}");
        send_out();
    } else if (strcmp(cmd, "guest-get-time") == 0) {
        putf("{\"return\":%llu}", (unsigned long long)rtc_now_utc() * 1000000000ull);
        send_out();
    } else if (strcmp(cmd, "guest-set-time") == 0) {
        /* no time: "set the clock from the hardware clock" - it already is */
        if (get_num(o, len, "time", &n) && rtc_set_utc((uint32_t)((uint64_t)n / 1000000000ull)) != 0) {
            error_reply("InvalidParameter", "the time is outside what the clock holds (2000-2099)");
            return;
        }
        put("{\"return\":{}}");
        send_out();
    } else if (strcmp(cmd, "guest-get-timezone") == 0) {
        {
            const tz_zone_t* z = tz_get(tz_current());
            putf("{\"return\":{\"zone\":\"%s\",\"offset\":%d}}", z ? z->id : "UTC", tz_offset_at(tz_current(), rtc_now_utc()));
        }
        send_out();
    } else if (strcmp(cmd, "guest-network-get-interfaces") == 0) {
        cmd_interfaces();
    } else if (strcmp(cmd, "guest-get-vcpus") == 0) {
        cmd_vcpus();
    } else if (strcmp(cmd, "guest-get-fsinfo") == 0) {
        cmd_fsinfo();
    } else if (strcmp(cmd, "guest-get-users") == 0) {
        put("{\"return\":[]}");
        send_out();
    } else if (strcmp(cmd, "guest-shutdown") == 0) {
        if (!get_str(o, len, "mode", s, sizeof(s))) kstrlcpy(s, "powerdown", sizeof(s));
        if (strcmp(s, "powerdown") && strcmp(s, "halt") && strcmp(s, "reboot")) {
            error_reply("InvalidParameter", "mode: powerdown, halt or reboot");
            return;
        }
        g_power_mode = strcmp(s, "reboot") == 0;
        task_create("qga-power", power_task);          /* (no reply: qemu-ga sends none on success) */
    } else if (strcmp(cmd, "guest-fsfreeze-status") == 0) {
        put(fsdisk_frozen() ? "{\"return\":\"frozen\"}" : "{\"return\":\"thawed\"}");
        send_out();
    } else if (strcmp(cmd, "guest-fsfreeze-freeze") == 0 || strcmp(cmd, "guest-fsfreeze-freeze-list") == 0) {
        int rc = fsdisk_freeze(1);
        if (rc != FSDISK_OK) { error_reply("GenericError", "the filesystem could not be written to the disk"); return; }
        put("{\"return\":1}");
        send_out();
    } else if (strcmp(cmd, "guest-fsfreeze-thaw") == 0) {
        int was = fsdisk_frozen();
        fsdisk_freeze(0);
        putf("{\"return\":%d}", was ? 1 : 0);
        send_out();
    } else {
        char d[96];
        ksnprintf(d, sizeof(d), "The command %s has not been found", cmd);
        error_reply("CommandNotFound", d);
    }
}

/* bytes from the host: complete JSON objects are handled, the rest waits;
 * 0xFF (guest-sync-delimited's flush) throws away what came before it */
static void on_rx(int port, const uint8_t* data, uint32_t len, void* ctx) {
    (void)port; (void)ctx;
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] == 0xFF) { g_in_len = 0; continue; }
        if (g_in_len < IN_MAX) g_in[g_in_len++] = (char)data[i];
    }
    for (;;) {
        uint32_t start = 0;
        while (start < g_in_len && g_in[start] != '{') start++;
        if (start == g_in_len) { g_in_len = 0; return; }
        int depth = 0, str = 0, esc = 0;
        uint32_t end = 0;
        for (uint32_t i = start; i < g_in_len; i++) {
            char c = g_in[i];
            if (str) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') str = 0;
                continue;
            }
            if (c == '"') str = 1;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { end = i + 1; break; }
        }
        if (!end) {                                     /* not whole yet */
            if (g_in_len >= IN_MAX) g_in_len = 0;       /* (too big: dropped) */
            else if (start) { memmove(g_in, g_in + start, g_in_len - start); g_in_len -= start; }
            return;
        }
        handle(g_in + start, end - start);
        memmove(g_in, g_in + end, g_in_len - end);
        g_in_len -= end;
    }
}

/* the port is named once the host has said so: waits for it */
static void qga_task(void) {
    task_set_background();
    for (int tries = 0; tries < 600 && g_port < 0; tries++) {       /* (2 minutes) */
        int p = vser_find(PORT_NAME);
        if (p >= 0 && vser_open(p, on_rx, NULL) == 0) {
            g_port = p;
            klog("qga: the QEMU guest agent answers on virtio-serial port %d\n", p);
            break;
        }
        task_sleep_ms(200);
    }

}

int qga_init(void) {
    if (!vser_present()) return 0;
    g_in = (char*)kmalloc(IN_MAX);
    g_out = (char*)kmalloc(OUT_MAX);
    if (!g_in || !g_out) return 0;
    task_create("qga", qga_task);
    return 1;
}

int qga_active(void) { return g_port >= 0; }

void qga_describe(char* out, int cap) {
    if (g_port < 0) { kstrlcpy(out, vser_present() ? "waiting for its port (" PORT_NAME ")" : "no virtio-serial device", (size_t)cap); return; }
    ksnprintf(out, (size_t)cap, "on port %d%s, %u commands answered%s%s", g_port,
              vser_host_connected(g_port) ? " (host connected)" : "", g_cmds, g_last[0] ? ", last: " : "", g_last);
}
