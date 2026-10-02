#include "upnp.h"
#include "http.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"

#define SSDP_GROUP IP4(239, 255, 255, 250)
#define SSDP_PORT  1900
#define DESC_MAX   (64u * 1024u)

/* ── SSDP discovery ───────────────────────────────────────────────── */

static char g_location[512];
static volatile int g_found;

static int ci_find(const char* hay, uint32_t n, const char* needle) {
    uint32_t nl = (uint32_t)strlen(needle);
    for (uint32_t i = 0; i + nl <= n; i++)
        if (strncasecmp(hay + i, needle, nl) == 0) return (int)i;
    return -1;
}

static void ssdp_rx(ip4_t src, uint16_t sport, const uint8_t* data, uint32_t len, void* ctx) {
    (void)src; (void)sport; (void)ctx;
    if (g_found) return;
    const char* s = (const char*)data;
    int at = ci_find(s, len, "\nLOCATION:");
    if (at < 0) return;
    const char* v = s + at + 10;
    const char* end = s + len;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    uint32_t n = 0;
    while (v + n < end && v[n] != '\r' && v[n] != '\n' && n < sizeof(g_location) - 1) n++;
    memcpy(g_location, v, n);
    g_location[n] = 0;
    g_found = 1;
}

static int discover(char* err, int errcap) {
    netif_t* nif = net_if();
    if (!nif->dev || !nif->configured) { kstrlcpy(err, "the network is not configured", (size_t)errcap); return -1; }
    uint16_t port = udp_ephemeral_port();
    g_found = 0;
    if (udp_bind(port, ssdp_rx, NULL) != 0) { kstrlcpy(err, "no free UDP port", (size_t)errcap); return -1; }
    static const char* const targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "upnp:rootdevice",
    };
    for (int round = 0; round < 3 && !g_found; round++) {
        for (uint32_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
            char msg[256];
            int n = ksnprintf(msg, sizeof(msg),
                              "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                              "MX: 2\r\nST: %s\r\n\r\n", targets[t]);
            udp_send(SSDP_GROUP, port, SSDP_PORT, msg, (uint32_t)n);
            /* many routers also answer a search sent straight to them */
            if (nif->gateway) udp_send(nif->gateway, port, SSDP_PORT, msg, (uint32_t)n);
        }
        uint32_t t0 = timer_ms();
        while (!g_found && (uint32_t)(timer_ms() - t0) < 1500) net_wait(20);
    }
    udp_unbind(port);
    if (!g_found) {
        kstrlcpy(err, "no UPnP router answered (UPnP may be off in the router's settings)", (size_t)errcap);
        return -1;
    }
    return 0;
}

/* ── description and control URL ──────────────────────────────────── */

typedef struct { char* buf; uint32_t n; } grow_t;

static int on_body(void* ctx, const uint8_t* d, uint32_t n) {
    grow_t* g = (grow_t*)ctx;
    if (g->n + n >= DESC_MAX) return -1;
    memcpy(g->buf + g->n, d, n);
    g->n += n;
    g->buf[g->n] = 0;
    return 0;
}

/* text between <tag> and </tag> after `from`, into out */
static const char* tag_text(const char* from, const char* tag, char* out, int cap) {
    char open[64], close[64];
    ksnprintf(open, sizeof(open), "<%s>", tag);
    ksnprintf(close, sizeof(close), "</%s>", tag);
    const char* a = strstr(from, open);
    if (!a) return NULL;
    a += strlen(open);
    const char* b = strstr(a, close);
    if (!b) return NULL;
    int n = (int)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(out, a, (size_t)n);
    out[n] = 0;
    return b;
}

static char g_control[512];
static char g_service[96];

static int find_service(char* err, int errcap) {
    grow_t g;
    g.buf = (char*)kmalloc(DESC_MAX);
    g.n = 0;
    if (!g.buf) { kstrlcpy(err, "out of memory", (size_t)errcap); return -1; }
    g.buf[0] = 0;
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.timeout_ms = 5000;
    req.follow_redirects = 1;
    req.ctx = &g;
    req.on_body = on_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { kfree(g.buf); return -1; }
    char e[160];
    int rc = http_fetch(g_location, &req, resp, e, sizeof(e));
    kfree(resp);
    if (rc != NET_OK) { ksnprintf(err, (size_t)errcap, "router description: %s", e); kfree(g.buf); return -1; }

    static const char* const services[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    const char* at = NULL;
    for (uint32_t i = 0; i < sizeof(services) / sizeof(services[0]) && !at; i++) {
        at = strstr(g.buf, services[i]);
        if (at) kstrlcpy(g_service, services[i], sizeof(g_service));
    }
    char ctl[256];
    if (!at || !tag_text(at, "controlURL", ctl, sizeof(ctl))) {
        kstrlcpy(err, "the router has no WAN connection service", (size_t)errcap);
        kfree(g.buf);
        return -1;
    }
    char base[512];
    if (!tag_text(g.buf, "URLBase", base, sizeof(base)) || !base[0]) kstrlcpy(base, g_location, sizeof(base));
    kfree(g.buf);

    if (strncmp(ctl, "http://", 7) == 0) {
        kstrlcpy(g_control, ctl, sizeof(g_control));
    } else {
        /* scheme://host:port of the base, then the path */
        char* p = strstr(base, "://");
        char* slash = p ? strchr(p + 3, '/') : NULL;
        if (ctl[0] == '/') {
            if (slash) *slash = 0;
            ksnprintf(g_control, sizeof(g_control), "%s%s", base, ctl);
        } else {
            char* last = strrchr(base, '/');
            if (last && slash && last >= slash) last[1] = 0;
            else kstrlcat(base, "/", sizeof(base));
            ksnprintf(g_control, sizeof(g_control), "%s%s", base, ctl);
        }
    }
    return 0;
}

/* ── SOAP ─────────────────────────────────────────────────────────── */

static int soap(const char* action, const char* args, char* reply, int rcap, char* err, int errcap) {
    char* body = (char*)kmalloc(2048);
    char* hdr = (char*)kmalloc(256);
    grow_t g;
    g.buf = (char*)kmalloc(DESC_MAX);
    g.n = 0;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!body || !hdr || !g.buf || !resp) {
        kfree(body); kfree(hdr); kfree(g.buf); kfree(resp);
        kstrlcpy(err, "out of memory", (size_t)errcap);
        return -1;
    }
    g.buf[0] = 0;
    int bl = ksnprintf(body, 2048,
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
        action, g_service, args, action);
    ksnprintf(hdr, 256, "SOAPAction: \"%s#%s\"\r\n", g_service, action);
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.timeout_ms = 5000;
    req.body = body;
    req.body_len = (uint32_t)bl;
    req.content_type = "text/xml; charset=\"utf-8\"";
    req.extra_headers = hdr;
    req.ctx = &g;
    req.on_body = on_body;
    char e[160];
    int rc = http_fetch(g_control, &req, resp, e, sizeof(e));
    int status = resp->status;
    kfree(body);
    kfree(hdr);
    kfree(resp);
    int ok = rc == NET_OK && status == 200;
    if (rc != NET_OK) ksnprintf(err, (size_t)errcap, "%s: %s", action, e);
    else if (!ok) {
        char desc[128] = "", code[16] = "";
        tag_text(g.buf, "errorCode", code, sizeof(code));
        tag_text(g.buf, "errorDescription", desc, sizeof(desc));
        ksnprintf(err, (size_t)errcap, "the router refused %s (%s%s%s)", action,
                  code[0] ? code : "HTTP error", desc[0] ? ": " : "", desc);
    }
    if (ok && reply) kstrlcpy(reply, g.buf, (size_t)rcap);
    kfree(g.buf);
    return ok ? 0 : -1;
}

static int prepare(char* err, int errcap) {
    if (discover(err, errcap) != 0) return -1;
    return find_service(err, errcap);
}

int upnp_add_port(uint16_t ext, uint16_t internal, const char* description,
                  char* public_ip, int ipcap, char* err, int errcap) {
    public_ip[0] = 0;
    if (prepare(err, errcap) != 0) return -1;
    char me[16];
    ip4_to_str(net_if()->ip, me);
    char args[512];
    ksnprintf(args, sizeof(args),
              "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
              "<NewProtocol>TCP</NewProtocol><NewInternalPort>%u</NewInternalPort>"
              "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
              "<NewPortMappingDescription>%s</NewPortMappingDescription>"
              "<NewLeaseDuration>0</NewLeaseDuration>", ext, internal, me, description);
    if (soap("AddPortMapping", args, NULL, 0, err, errcap) != 0) return -1;
    char* reply = (char*)kmalloc(4096);
    if (reply && soap("GetExternalIPAddress", "", reply, 4096, err, errcap) == 0)
        tag_text(reply, "NewExternalIPAddress", public_ip, ipcap);
    kfree(reply);
    err[0] = 0;
    return 0;
}

int upnp_delete_port(uint16_t ext, char* err, int errcap) {
    if (prepare(err, errcap) != 0) return -1;
    char args[256];
    ksnprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
              "<NewProtocol>TCP</NewProtocol>", ext);
    return soap("DeletePortMapping", args, NULL, 0, err, errcap);
}

int upnp_is_private(ip4_t ip) {
    uint8_t a = (uint8_t)(ip >> 24), b = (uint8_t)(ip >> 16);
    return a == 10 || a == 127 || a == 0 || (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168) ||
           (a == 100 && (b & 0xC0) == 64) || (a == 169 && b == 254);
}
