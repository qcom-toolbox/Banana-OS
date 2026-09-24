#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/timer.h"
#include "../kernel/kheap.h"
#include "../kernel/fs.h"
#include "../kernel/rtc.h"
#include "../net/net.h"
#include "../net/tcp.h"
#include "../net/http.h"
#include "../net/tls.h"
#include "../crypto/selftest.h"
#include "wpcmd.h"
#include "../usb/usbcore.h"


/* ── helpers ────────────────────────────────────────────────────── */

int shell_split_args(char* buf, char** argv, int max_args) {
    int argc = 0;
    char* p = buf;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* out = p;
        argv[argc++] = out;
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; continue; }
            } else if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            } else if (*p == ' ' || *p == '\t') {
                break;
            }
            *out++ = *p++;
        }
        if (*p) p++;
        *out = '\0';
    }
    return argc;
}

static void err(const char* cmd, const char* msg) {
    terminal_write_color(cmd, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static void printf_line(const char* fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static int need_device(const char* cmd) {
    if (net_if()->dev) return 1;
    err(cmd, "no network card found (QEMU: -nic user,model=e1000)");
    return 0;
}

/* ── ifconfig / dhcp ────────────────────────────────────────────── */

static void show_ifconfig(void);

static void show_device(netdev_t* d) {
    netif_t* nif = net_if();
    int active = (d == nif->dev);
    char ip[16], mask[16], gw[16], dns[16];
    ip4_to_str(nif->ip, ip);
    ip4_to_str(nif->netmask, mask);
    ip4_to_str(nif->gateway, gw);
    ip4_to_str(nif->dns, dns);

    terminal_write_color(d->ifname, active ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf_line(": flags=<%sBROADCAST%s>  mtu 1500%s\n", active ? "UP," : "",
                d->link_up(d) ? ",RUNNING" : ",NO-CARRIER",
                active ? "" : "   (inactive - `ifconfig <name> up` to use it)");
    if (!active) {
        /* nothing else to show for a standby card */
    } else if (nif->configured) {
        printf_line("        inet %s  netmask %s  gateway %s\n", ip, mask, gw);
        printf_line("        dns %s", dns);
        if (nif->dhcp) {
            uint32_t age = (timer_ms() - nif->bound_ms) / 1000u;
            printf_line("  (dhcp %s, lease %us, obtained %us ago)\n", dhcp_state_str(), nif->lease_s, age);
        } else {
            terminal_writeln("  (static)");
        }
    } else {
        printf_line("        inet (none)  - dhcp: %s\n", nif->dhcp ? dhcp_state_str() : "off");
    }
    if (d->irq == 0xFF)
        printf_line("        ether %02x:%02x:%02x:%02x:%02x:%02x  (%s)\n",
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->model);
    else
        printf_line("        ether %02x:%02x:%02x:%02x:%02x:%02x  (%s, irq %u)\n",
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->model, d->irq);
    printf_line("        RX packets %u  bytes %u  errors %u\n", d->rx_packets, d->rx_bytes, d->rx_errors);
    printf_line("        TX packets %u  bytes %u  errors %u\n", d->tx_packets, d->tx_bytes, d->tx_errors);
}

static void show_ifconfig(void) {
    if (net_device_count() == 0)
        terminal_writeln("(no network card found - PCI e1000/RTL8139, or a USB RTL8152/CDC-ECM adapter)");
    for (int i = 0; i < net_device_count(); i++) show_device(net_device_at(i));
    terminal_writeln("lo: inet 127.0.0.1  netmask 255.0.0.0");
}

static netdev_t* find_ifname(const char* name) {
    for (int i = 0; i < net_device_count(); i++)
        if (strcmp(net_device_at(i)->ifname, name) == 0) return net_device_at(i);
    return NULL;
}

static void cmd_ifconfig(int argc, char** argv) {
    netdev_t* named = argc >= 2 ? find_ifname(argv[1]) : NULL;
    if (argc <= 1 || (argc == 2 && named)) {
        if (named) show_device(named);
        else show_ifconfig();
        return;
    }
    if (named && argc == 3 && strcmp(argv[2], "up") == 0) {
        /* make this card the active interface (DHCP starts on it) */
        net_select_device(named);
        printf_line("%s is now the active interface; requesting an address...\n", named->ifname);
        if (net_wait_configured(10000)) show_device(named);
        else err("ifconfig", "no DHCP answer yet (still trying in the background)");
        return;
    }
    if (!need_device("ifconfig")) return;
    /* ifconfig <if> <ip> [netmask <m>] [gw <g>] [dns <d>] */
    int i = 1;
    if (named) {
        if (named != net_if()->dev) net_select_device(named);
        i++;
    }
    ip4_t ip = 0, mask = IP4(255, 255, 255, 0), gw = 0, dns = 0;
    if (i >= argc || !str_to_ip4(argv[i], &ip)) {
        terminal_writeln("usage: ifconfig [<if> up] | [<if> <ip> [netmask <mask>] [gw <gateway>] [dns <server>]]");
        return;
    }
    for (i++; i + 1 < argc; i += 2) {
        ip4_t v;
        if (!str_to_ip4(argv[i + 1], &v)) { err("ifconfig", "bad address"); return; }
        if (strcmp(argv[i], "netmask") == 0) mask = v;
        else if (strcmp(argv[i], "gw") == 0) gw = v;
        else if (strcmp(argv[i], "dns") == 0) dns = v;
        else { err("ifconfig", "unknown option"); return; }
    }
    net_set_static(ip, mask, gw, dns);
    show_ifconfig();
}

static void cmd_dhcp(void) {
    if (!need_device("dhcp")) return;
    terminal_writeln("dhcp: requesting a lease...");
    dhcp_start();
    if (!net_wait_configured(15000)) {
        err("dhcp", "no answer from a DHCP server (still trying in the background)");
        return;
    }
    show_ifconfig();
}

/* ── ping ───────────────────────────────────────────────────────── */

static void cmd_ping(int argc, char** argv) {
    uint32_t count = 4, size = 56, interval = 1000;
    const char* host = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) k_parse_u32(argv[++i], &count);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) k_parse_u32(argv[++i], &size);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            uint32_t s; k_parse_u32(argv[++i], &s); interval = s ? s * 1000u : 200u;
        }
        else if (argv[i][0] == '-') { err("ping", "usage: ping [-c count] [-s size] [-i seconds] host"); return; }
        else host = argv[i];
    }
    if (!host) { terminal_writeln("usage: ping [-c count] [-s size] [-i seconds] host"); return; }
    if (size > 1472) size = 1472;
    if (!need_device("ping")) return;

    ip4_t dst;
    int rc = dns_resolve(host, &dst, 10000);
    if (rc != NET_OK) {
        char m[128];
        ksnprintf(m, sizeof(m), "%s: %s", host, net_strerror(rc));
        err("ping", m);
        return;
    }
    char ip[16];
    ip4_to_str(dst, ip);
    printf_line("PING %s (%s) %u(%u) bytes of data.\n", host, ip, size, size + 28);

    uint16_t id = (uint16_t)(timer_ms() ^ 0xBA7A);
    uint32_t sent = 0, received = 0, errors = 0;
    uint32_t rtt_min = 0xFFFFFFFFu, rtt_max = 0, rtt_sum = 0;
    uint32_t send_ms[64];
    uint32_t start = timer_ms();
    int interrupted = 0;

    for (uint32_t seq = 1; count == 0 || seq <= count; seq++) {
        send_ms[seq % 64] = timer_ms();
        int s = icmp_echo_send(dst, id, (uint16_t)seq, size);
        if (s != NET_OK) {
            printf_line("ping: sendmsg: %s\n", net_strerror(s));
            errors++;
        }
        sent++;
        /* wait for the reply (or the next interval) */
        uint32_t t0 = timer_ms();
        uint32_t wait = (count && seq == count) ? 2000u : interval;
        while (timer_ms() - t0 < wait) {
            icmp_event_t ev;
            while (icmp_poll_event(id, &ev)) {
                char from[16];
                ip4_to_str(ev.src, from);
                if (ev.type == 0) {
                    uint32_t rtt = ev.recv_ms - send_ms[ev.seq % 64];
                    received++;
                    rtt_sum += rtt;
                    if (rtt < rtt_min) rtt_min = rtt;
                    if (rtt > rtt_max) rtt_max = rtt;
                    printf_line("%u bytes from %s: icmp_seq=%u ttl=%u time=%u ms\n",
                                ev.bytes, from, ev.seq, ev.ttl, rtt);
                } else {
                    errors++;
                    printf_line("From %s icmp_seq=%u %s\n", from, ev.seq,
                                ev.type == 3 ? "Destination Unreachable" : "Time to live exceeded");
                }
            }
            if (net_interrupted()) { interrupted = 1; break; }
            if (count && seq == count && received + errors >= sent) break;
            net_wait(10);
        }
        if (interrupted) { terminal_writeln("^C"); break; }
    }

    uint32_t loss = sent ? ((sent - received) * 100u) / sent : 0;
    printf_line("\n--- %s ping statistics ---\n", host);
    printf_line("%u packets transmitted, %u received, %u%% packet loss, time %ums\n",
                sent, received, loss, timer_ms() - start);
    if (received)
        printf_line("rtt min/avg/max = %u/%u/%u ms\n", rtt_min, rtt_sum / received, rtt_max);
}

/* ── nslookup ───────────────────────────────────────────────────── */

static void cmd_nslookup(int argc, char** argv) {
    if (argc < 2) { terminal_writeln("usage: nslookup <hostname>"); return; }
    if (!need_device(argv[0])) return;
    ip4_t ip;
    int rc = dns_resolve(argv[1], &ip, 10000);
    char srv[16];
    ip4_to_str(net_if()->dns, srv);
    printf_line("Server:  %s\n", srv);
    if (rc != NET_OK) {
        char m[128];
        ksnprintf(m, sizeof(m), "%s: %s", argv[1], net_strerror(rc));
        err(argv[0], m);
        return;
    }
    char a[16];
    ip4_to_str(ip, a);
    printf_line("Name:    %s\nAddress: %s\n", argv[1], a);
}

/* ── shared transfer helpers ────────────────────────────────────── */

static void fmt_size(uint32_t bytes, char* out, uint32_t cap) {
    if (bytes < 1024) ksnprintf(out, cap, "%u", bytes);
    else if (bytes < 1024u * 1024u) ksnprintf(out, cap, "%u.%uK", bytes / 1024u, (bytes % 1024u) * 10u / 1024u);
    else ksnprintf(out, cap, "%u.%uM", bytes / (1024u * 1024u), (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u));
}

static void fmt_rate(uint32_t bytes, uint32_t ms, char* out, uint32_t cap) {
    if (ms == 0) ms = 1;
    uint32_t bps = (uint32_t)(((uint64_t)bytes * 1000u) / ms);
    char s[16];
    fmt_size(bps, s, sizeof(s));
    ksnprintf(out, cap, "%sB/s", s);
}

/* curl's exit codes for the common failures */
static int curl_code(int rc, int tls_failed) {
    if (tls_failed) return 35;
    switch (rc) {
    case NET_ERR_DNS:      return 6;
    case NET_ERR_REFUSED:  return 7;
    case NET_ERR_NOROUTE:  return 7;
    case NET_ERR_TIMEOUT:  return 28;
    case NET_ERR_NOTREADY: return 7;
    case NET_ERR_INTR:     return 42;
    default:               return 56;
    }
}

/* ── curl ───────────────────────────────────────────────────────── */

typedef struct {
    int      verbose, silent, include, head;
    int      file_idx;          /* -1: body goes to the terminal */
    int      write_failed;
    int      binary_refused;
    uint32_t start_ms, last_progress_ms;
    int      progress_shown;
} curl_ctx_t;

static void curl_info(void* ctx, const char* msg) {
    curl_ctx_t* c = (curl_ctx_t*)ctx;
    if (!c->verbose) return;
    terminal_write_color("* ", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    terminal_writeln(msg);
}

static void print_prefixed(const char* prefix, const char* text, uint8_t color) {
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        uint32_t n = nl ? (uint32_t)(nl - p) : (uint32_t)strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n || nl) {
            char line[256];
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            terminal_write_color(prefix, color, VGA_COLOR_BLACK);
            terminal_writeln(line);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void curl_request(void* ctx, const char* raw) {
    curl_ctx_t* c = (curl_ctx_t*)ctx;
    if (c->verbose) print_prefixed("> ", raw, VGA_COLOR_LIGHT_CYAN);
}

static void curl_headers(void* ctx, const http_response_t* r, const char* raw) {
    (void)r;
    curl_ctx_t* c = (curl_ctx_t*)ctx;
    if (c->verbose) print_prefixed("< ", raw, VGA_COLOR_LIGHT_GREEN);
    if ((c->include || c->head) && !c->verbose) {
        terminal_write(raw);
        terminal_putchar('\n');
    }
}

static int looks_binary(const uint8_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (d[i] == 0) return 1;
    return 0;
}

static int curl_body(void* ctx, const uint8_t* data, uint32_t len) {
    curl_ctx_t* c = (curl_ctx_t*)ctx;
    if (c->file_idx >= 0) {
        if (fs_append(c->file_idx, data, len) != 0) { c->write_failed = 1; return -1; }
        return 0;
    }
    if (looks_binary(data, len)) {
        c->binary_refused = 1;
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '\r' && i + 1 < len && data[i + 1] == '\n') continue;   /* CRLF -> LF */
        if (ch == '\t') { terminal_write("    "); continue; }
        if ((unsigned char)ch < 32 && ch != '\n') continue;
        terminal_putchar(ch);
    }
    return 0;
}

static void curl_progress(void* ctx, uint32_t got, int32_t total) {
    curl_ctx_t* c = (curl_ctx_t*)ctx;
    if (c->file_idx < 0 || c->silent) return;
    uint32_t now = timer_ms();
    if (now - c->last_progress_ms < 250 && (total < 0 || got < (uint32_t)total)) return;
    c->last_progress_ms = now;
    char g[16], rate[24];
    fmt_size(got, g, sizeof(g));
    fmt_rate(got, now - c->start_ms, rate, sizeof(rate));
    if (total > 0) {
        char t[16];
        fmt_size((uint32_t)total, t, sizeof(t));
        printf_line("\r  %3u%%  %8s / %-8s  %10s   ",
                    (uint32_t)(((uint64_t)got * 100u) / (uint32_t)total), g, t, rate);
    } else {
        printf_line("\r  %8s received  %10s   ", g, rate);
    }
    c->progress_shown = 1;
}

static const char* url_basename(const char* url, char* out, uint32_t cap) {
    url_t u;
    if (!url_parse(url, &u)) { kstrlcpy(out, "index.html", cap); return out; }
    char path[1024];
    kstrlcpy(path, u.path, sizeof(path));
    char* q = strchr(path, '?');
    if (q) *q = '\0';
    char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    if (!*name) name = "index.html";
    kstrlcpy(out, name, cap);
    if (strlen(out) >= FS_NAME_LEN) out[FS_NAME_LEN - 1] = '\0';
    return out;
}

static void cmd_curl(int argc, char** argv) {
    curl_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.file_idx = -1;
    const char* url = NULL;
    const char* out_path = NULL;
    const char* ua = "curl/8.0 (BananaOS 0.5)";
    int remote_name = 0, follow = 0, ciphers = TLS_CIPHERS_ALL;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] && a[1] != '-') {
            /* combined short flags: -sSL, -Lo file, ... */
            for (int j = 1; a[j]; j++) {
                char f = a[j];
                if (f == 'L') follow = 1;
                else if (f == 'I') c.head = 1;
                else if (f == 'i') c.include = 1;
                else if (f == 'v') c.verbose = 1;
                else if (f == 's') c.silent = 1;
                else if (f == 'S' || f == 'k' || f == 'f') {}          /* accepted, no-op */
                else if (f == 'O') remote_name = 1;
                else if ((f == 'o' || f == 'A') && !a[j + 1] && i + 1 < argc) {
                    if (f == 'o') out_path = argv[++i]; else ua = argv[++i];
                    break;
                } else {
                    terminal_writeln("usage: curl [-L] [-o file | -O] [-I] [-i] [-v] [-s] [-A agent] <url>");
                    return;
                }
            }
        } else if (strcmp(a, "--location") == 0) follow = 1;
        else if (strcmp(a, "--head") == 0) c.head = 1;
        else if (strcmp(a, "--insecure") == 0 || strcmp(a, "--silent") == 0) c.silent |= (a[2] == 's');
        else if (strcmp(a, "--output") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(a, "--remote-name") == 0) remote_name = 1;
        else if (strcmp(a, "--verbose") == 0) c.verbose = 1;
        else if (strcmp(a, "--tls13-ciphers") == 0 && i + 1 < argc) {
            const char* list = argv[++i];
            int aes = strstr(list, "AES_128_GCM") != NULL, cc = strstr(list, "CHACHA20") != NULL;
            ciphers = (aes && !cc) ? TLS_CIPHERS_AES : (cc && !aes) ? TLS_CIPHERS_CHACHA : TLS_CIPHERS_ALL;
        }
        else if (a[0] == '-') {
            terminal_writeln("usage: curl [-L] [-o file | -O] [-I] [-i] [-v] [-s] [-A agent] <url>");
            return;
        } else url = a;
    }
    if (!url) {
        terminal_writeln("usage: curl [-L] [-o file | -O] [-I] [-i] [-v] [-s] [-A agent] <url>");
        terminal_writeln("       e.g. curl http://example.com    curl -L -o page.html https://example.com");
        return;
    }
    if (!need_device("curl")) return;

    char name[FS_NAME_LEN];
    if (remote_name && !out_path) out_path = url_basename(url, name, sizeof(name));
    if (out_path && !c.head) {
        c.file_idx = fs_create(out_path);
        if (c.file_idx < 0 || fs_write(c.file_idx, "", 0) != 0) {
            char m[160];
            ksnprintf(m, sizeof(m), "(23) cannot create output file '%s'", out_path);
            err("curl", m);
            return;
        }
    }

    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = c.head ? "HEAD" : "GET";
    req.follow_redirects = follow;
    req.user_agent = ua;
    req.tls_ciphers = ciphers;
    req.ctx = &c;
    req.on_info = curl_info;
    req.on_request = curl_request;
    req.on_headers = curl_headers;
    req.on_body = curl_body;
    req.on_progress = curl_progress;
    c.start_ms = timer_ms();

    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    char emsg[200];
    if (!resp) { err("curl", "out of memory"); return; }
    int rc = http_fetch(url, &req, resp, emsg, sizeof(emsg));
    if (c.progress_shown && rc == NET_OK) {
        c.last_progress_ms = timer_ms() - 1000u;   /* force a final, complete line */
        curl_progress(&c, resp->body_bytes, resp->content_length);
    }
    if (c.progress_shown) terminal_putchar('\n');

    if (c.binary_refused) {
        terminal_writeln("");
        terminal_write_color("Warning: Binary output can mess up your terminal. Use \"--output <FILE>\" (-o)\n"
                             "Warning: to save it to a file instead.\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    } else if (c.write_failed) {
        err("curl", "(23) failure writing output (out of memory / file too big)");
    } else if (rc != NET_OK) {
        char m[240];
        ksnprintf(m, sizeof(m), "(%d) %s", curl_code(rc, strstr(emsg, "TLS") != NULL), emsg);
        err("curl", m);
    } else {
        if (!follow && resp->status >= 300 && resp->status < 400 && resp->location[0] && !c.silent && !c.head)
            printf_line("\n(curl: %d redirect to %s - use -L to follow)\n", resp->status, resp->location);
        if (c.file_idx >= 0 && !c.silent) {
            char sz[16], rate[24];
            uint32_t ms = timer_ms() - c.start_ms;
            fmt_size(resp->body_bytes, sz, sizeof(sz));
            fmt_rate(resp->body_bytes, ms, rate, sizeof(rate));
            printf_line("  saved %s bytes to '%s' in %u.%02us (%s)\n",
                        sz, out_path, ms / 1000u, (ms % 1000u) / 10u, rate);
        }
    }
    kfree(resp);
}

/* ── wget ───────────────────────────────────────────────────────── */

typedef struct {
    int      quiet;
    const char* out_path;
    int      file_idx;
    int      write_failed;
    uint32_t start_ms, last_ms;
    char     name[64];
    int      bar_shown;
} wget_ctx_t;

static void wget_info(void* ctx, const char* msg) {
    wget_ctx_t* w = (wget_ctx_t*)ctx;
    if (!w->quiet) terminal_writeln(msg);
}

static void wget_headers(void* ctx, const http_response_t* r, const char* raw) {
    (void)raw;
    wget_ctx_t* w = (wget_ctx_t*)ctx;
    if (!w->quiet) printf_line("HTTP request sent, awaiting response... %d %s\n", r->status, r->reason);
    if (r->status >= 300 && r->status < 400 && r->location[0]) return;   /* redirect */
    if (r->status < 200 || r->status >= 300) return;
    /* the real response: create the output file now */
    w->file_idx = fs_create(w->out_path);
    if (w->file_idx >= 0 && fs_write(w->file_idx, "", 0) != 0) w->file_idx = -1;
    if (!w->quiet) {
        if (r->content_length >= 0) {
            char s[16];
            fmt_size((uint32_t)r->content_length, s, sizeof(s));
            printf_line("Length: %d (%s) [%s]\n", r->content_length, s,
                        r->content_type[0] ? r->content_type : "unknown");
        } else {
            printf_line("Length: unspecified [%s]\n", r->content_type[0] ? r->content_type : "unknown");
        }
        printf_line("Saving to: '%s'\n\n", w->out_path);
    }
    w->start_ms = timer_ms();
}

static void wget_bar(wget_ctx_t* w, uint32_t got, int32_t total, int final) {
    uint32_t now = timer_ms();
    if (!final && now - w->last_ms < 250) return;
    w->last_ms = now;
    char g[16], rate[24];
    fmt_size(got, g, sizeof(g));
    fmt_rate(got, now - w->start_ms, rate, sizeof(rate));
    char bar[24];
    uint32_t pct = 0;
    if (total > 0) {
        pct = (uint32_t)(((uint64_t)got * 100u) / (uint32_t)total);
        if (pct > 100) pct = 100;
        uint32_t fill = pct * 20u / 100u;
        for (uint32_t i = 0; i < 20; i++) bar[i] = i < fill ? '=' : (i == fill ? '>' : ' ');
        bar[20] = '\0';
        printf_line("\r%-18s %3u%%[%s] %8s  %10s ", w->name, pct, bar, g, rate);
    } else {
        printf_line("\r%-18s      [ <=> ]  %8s  %10s ", w->name, g, rate);
    }
    w->bar_shown = 1;
}

static int wget_body(void* ctx, const uint8_t* data, uint32_t len) {
    wget_ctx_t* w = (wget_ctx_t*)ctx;
    if (w->file_idx < 0) return 0;          /* error page body: discard */
    if (fs_append(w->file_idx, data, len) != 0) { w->write_failed = 1; return -1; }
    return 0;
}

static void wget_progress(void* ctx, uint32_t got, int32_t total) {
    wget_ctx_t* w = (wget_ctx_t*)ctx;
    if (!w->quiet && w->file_idx >= 0) wget_bar(w, got, total, 0);
}

static void cmd_wget(int argc, char** argv) {
    wget_ctx_t w;
    memset(&w, 0, sizeof(w));
    w.file_idx = -1;
    const char* url = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "--output-document") == 0) && i + 1 < argc)
            w.out_path = argv[++i];
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) w.quiet = 1;
        else if (argv[i][0] == '-') {
            terminal_writeln("usage: wget [-q] [-O file] <url>");
            return;
        } else url = argv[i];
    }
    if (!url) {
        terminal_writeln("usage: wget [-q] [-O file] <url>");
        terminal_writeln("       e.g. wget -O ~/Pictures/wall.jpg https://example.com/wall.jpg");
        return;
    }
    if (!need_device("wget")) return;

    /* default name: the URL's last path segment; never clobber (name.1, ...) */
    char defname[FS_NAME_LEN + 4];
    if (!w.out_path) {
        char base[FS_NAME_LEN];
        url_basename(url, base, sizeof(base));
        kstrlcpy(defname, base, sizeof(defname));
        for (int n = 1; fs_find_file(defname) >= 0 && n < 100; n++)
            ksnprintf(defname, sizeof(defname), "%s.%d", base, n);
        w.out_path = defname;
    }
    const char* slash = strrchr(w.out_path, '/');
    kstrlcpy(w.name, slash ? slash + 1 : w.out_path, sizeof(w.name));

    if (!w.quiet) {
        rtc_datetime_t dt;
        if (rtc_read_datetime(&dt) == 0)
            printf_line("--%04u-%02u-%02u %02u:%02u:%02u--  %s\n", dt.year, dt.month, dt.day,
                        dt.hour, dt.minute, dt.second, url);
        else
            printf_line("--  %s\n", url);
    }

    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 20;
    req.user_agent = "Wget/1.21 (BananaOS 0.5)";
    req.ctx = &w;
    req.on_info = wget_info;
    req.on_headers = wget_headers;
    req.on_body = wget_body;
    req.on_progress = wget_progress;

    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { err("wget", "out of memory"); return; }
    char emsg[200];
    uint32_t t0 = timer_ms();
    int rc = http_fetch(url, &req, resp, emsg, sizeof(emsg));
    if (w.bar_shown && rc == NET_OK && !w.write_failed) wget_bar(&w, resp->body_bytes, resp->content_length, 1);
    if (w.bar_shown) terminal_putchar('\n');

    if (w.write_failed) {
        err("wget", "failed writing the file (out of memory, or bigger than 32 MiB)");
    } else if (rc != NET_OK) {
        err("wget", emsg);
    } else if (resp->status < 200 || resp->status >= 300) {
        char m[120];
        ksnprintf(m, sizeof(m), "ERROR %d: %s.", resp->status, resp->reason);
        err("wget", m);
    } else if (!w.quiet) {
        char rate[24];
        uint32_t ms = timer_ms() - (w.start_ms ? w.start_ms : t0);
        fmt_rate(resp->body_bytes, ms, rate, sizeof(rate));
        if (resp->content_length >= 0)
            printf_line("\n'%s' saved [%u/%d] (%s)\n", w.out_path, resp->body_bytes, resp->content_length, rate);
        else
            printf_line("\n'%s' saved [%u] (%s)\n", w.out_path, resp->body_bytes, rate);
    }
    kfree(resp);
}

/* ── cryptotest ─────────────────────────────────────────────────── */

static void report_test(const char* name, int ok) {
    terminal_write_color(ok ? "  PASS  " : "  FAIL  ",
                         ok ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(name);
}

static void cmd_cryptotest(void) {
    uint32_t t0 = timer_ms();
    int ok = crypto_selftest(report_test);
    printf_line("%s (%u ms)\n", ok ? "all crypto self-tests passed" : "SOME CRYPTO SELF-TESTS FAILED",
                timer_ms() - t0);
}

/* ── dispatch ───────────────────────────────────────────────────── */

int netcmd_dispatch(const char* line) {
    char buf[1024];   /* = SH_LINE_MAX in shell.c */
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[24];
    int argc = shell_split_args(buf, argv, 24);
    if (argc == 0) return 0;
    const char* c = argv[0];

    if (strcmp(c, "ifconfig") == 0)                  { cmd_ifconfig(argc, argv); return 1; }
    if (strcmp(c, "dhcp") == 0)                      { cmd_dhcp(); return 1; }
    if (strcmp(c, "ping") == 0)                      { cmd_ping(argc, argv); return 1; }
    if (strcmp(c, "nslookup") == 0 || strcmp(c, "host") == 0) { cmd_nslookup(argc, argv); return 1; }
    if (strcmp(c, "netstat") == 0)                   { tcp_dump(); return 1; }
    if (strcmp(c, "arp") == 0)                       { arp_dump(); return 1; }
    if (strcmp(c, "curl") == 0)                      { cmd_curl(argc, argv); return 1; }
    if (strcmp(c, "wget") == 0)                      { cmd_wget(argc, argv); return 1; }
    if (strcmp(c, "cryptotest") == 0)                { cmd_cryptotest(); return 1; }
    if (strcmp(c, "wallpaper") == 0)                 { cmd_wallpaper(argc, argv); return 1; }
    if (strcmp(c, "lsusb") == 0)                     { usb_list(); return 1; }
    if (strcmp(c, "usb") == 0) {
        if (argc >= 2 && strcmp(argv[1], "rescan") == 0) { usb_rescan(); usb_list(); }
        else terminal_writeln("usage: usb rescan     (lsusb lists devices)");
        return 1;
    }
    return 0;
}
