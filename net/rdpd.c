/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rdpd.h"
#include "../kernel/users.h"
#include "rdp.h"
#include "tcp.h"
#include "tls12.h"
#include "task.h"
#include "timer.h"
#include "kstring.h"
#include "kheap.h"
#include "random.h"
#include "serial.h"
#include "fs.h"
#include "config.h"
#include "passwd.h"
#include "../kernel/terminal.h"
#include "../kernel/fb.h"
#include "../kernel/keyboard.h"
#include "../kernel/usb.h"

/*
 * Remote Desktop server - see rdpd.h. Section numbers: MS-RDPBCGR.
 *
 * The connection sequence runs over X.224 then TLS 1.2: X.224 negotiation
 * (PROTOCOL_SSL), MCS Connect / channel joins, the Client Info PDU, a
 * License Error PDU (valid client), Demand / Confirm Active and the
 * synchronize / control / font map exchange. After that the local desktop
 * is mirrored as changed 64x64 tiles (fast-path bitmap updates, 32bpp) and
 * the client's keyboard and mouse events are fed to the kernel.
 */

#define KEY_FILE    "/etc/rdp/server_key"
#define TILE        64
#define IN_CAP      (1u << 16)
#define OUT_CAP     (256u * 1024u)
#define SVR_CHANNEL 1002
#define SHARE_ID    0x000103EAu
#define CONNECT_TIMEOUT 30000u

typedef struct {
    tcp_conn_t*   tcp;
    tls12_conn_t* tls;
    int           slot, dead;
    char          peer[16];
    ip4_t         peer_ip;
    int           authed;

    uint8_t*      in;                   /* raw TLS bytes */
    uint32_t      in_len;
    uint8_t*      pdu;                   /* one received PDU */
    uint32_t      pdu_len;
    uint16_t      pdu_channel;
    int           pdu_fastpath;
    uint8_t*      out;                   /* scratch for building PDUs */

    uint32_t      requested;            /* the client's X.224 requestedProtocols */
    uint16_t      client_user;          /* channel id granted in Attach User Confirm */
    uint16_t      channels[16];         /* the client's virtual channels */
    int           channel_count;

    int           width, height;
    uint32_t*     shadow;               /* what we last sent, for diffing */
    uint8_t*      tile;                 /* one tile being compared / sent */
} rdp_t;

static tcp_listener_t* g_listener;
static volatile int    g_running;
static uint16_t        g_port;
static int             g_task_started;
static rsa_key_t       g_key;
static uint8_t         g_cert[2048];
static uint32_t        g_cert_len;
static int             g_have_key;
static struct { int active; char peer[16]; char user[32]; uint32_t since; int w, h; } g_sess;

/* ── the server identity ──────────────────────────────────────────── */

static int load_key(void) {
    if (!g_have_key) {
        int idx = fs_find_file(KEY_FILE);
        if (idx >= 0) {
            fs_file_t* f = fs_get_file(idx);
            if (f && f->size == sizeof(rsa_key_t)) { memcpy(&g_key, f->content, sizeof(rsa_key_t)); g_have_key = 1; }
        }
    }
    if (!g_have_key) {
        klog("rdpd: generating an RSA-2048 key (this takes a moment)...\n");
        if (rsa_generate(&g_key, random_bytes) != 0) return -1;
        fs_mkdir_p("/etc/rdp");
        if (fs_write_path(KEY_FILE, &g_key, sizeof(rsa_key_t)) < 0) {
            klog("rdpd: could not save %s\n", KEY_FILE);
            return -1;
        }
        cfg_persist();
        g_have_key = 1;
        klog("rdpd: key ready\n");
    }
    if (!g_cert_len) {
        int n = tls12_make_cert(&g_key, "Banana OS", g_cert, sizeof(g_cert));
        if (n < 0) return -1;
        g_cert_len = (uint32_t)n;
    }
    return 0;
}

int rdpd_fingerprint(char* out, int cap) {
    if (load_key() != 0) return -1;
    char fp[96];
    tls12_fingerprint(g_cert, g_cert_len, fp);
    ksnprintf(out, (size_t)cap, "SHA-256 %s", fp);
    return 0;
}

/* ── per-address lockout (as in sshd) ─────────────────────────────── */

#define LOCK_SLOTS     16
#define LOCK_FAILS     10
#define LOCK_WINDOW_MS (10u * 60u * 1000u)
#define LOCK_MS        (15u * 60u * 1000u)
static struct { ip4_t ip; uint32_t first_ms, until_ms; int fails; } g_lock[LOCK_SLOTS];

static int locked_out(ip4_t ip) {
    for (int i = 0; i < LOCK_SLOTS; i++)
        if (g_lock[i].ip == ip && g_lock[i].until_ms && (int32_t)(g_lock[i].until_ms - timer_ms()) > 0) return 1;
    return 0;
}
static void note_failure(ip4_t ip) {
    int k = -1, oldest = 0;
    for (int i = 0; i < LOCK_SLOTS; i++) {
        if (g_lock[i].ip == ip) { k = i; break; }
        if (g_lock[i].first_ms < g_lock[oldest].first_ms) oldest = i;
    }
    uint32_t now = timer_ms();
    if (k < 0 || now - g_lock[k].first_ms > LOCK_WINDOW_MS) {
        if (k < 0) k = oldest;
        g_lock[k].ip = ip; g_lock[k].first_ms = now; g_lock[k].fails = 0; g_lock[k].until_ms = 0;
    }
    if (++g_lock[k].fails >= LOCK_FAILS) {
        g_lock[k].until_ms = now + LOCK_MS;
        char t[16]; ip4_to_str(ip, t);
        klog("rdpd: %s: %d failed logins - refused for 15 minutes\n", t, g_lock[k].fails);
    }
}

/* ── transport framing ────────────────────────────────────────────── */

static int tls_send(rdp_t* s, const uint8_t* d, uint32_t len) {
    return tls12_write(s->tls, d, len, 30000) == (int)len ? 0 : -1;
}

/* s->in to at least `need` bytes (0: whatever is already there matters) */
static int fill(rdp_t* s, uint32_t need, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (s->in_len < need) {
        uint32_t left = timeout_ms;
        if (timeout_ms) {
            uint32_t el = timer_ms() - start;
            if (el >= timeout_ms) return NET_ERR_TIMEOUT;
            left = timeout_ms - el;
        }
        int r = tls12_read(s->tls, s->in + s->in_len, IN_CAP - s->in_len, left ? left : 1);
        if (r == 0) return NET_ERR_CLOSED;
        if (r < 0) return r;
        s->in_len += (uint32_t)r;
    }
    return 0;
}

static void in_consume(rdp_t* s, uint32_t n) {
    if (n > s->in_len) n = s->in_len;
    memmove(s->in, s->in + n, s->in_len - n);
    s->in_len -= n;
}

/*
 * One PDU, copied into s->pdu. Slow path (TPKT): the MCS user data, with
 * s->pdu_channel set; MCS control PDUs (no user data) come back with the
 * bytes after the X.224 header and channel 0xFFFF. Fast path: the whole
 * PDU with s->pdu_fastpath set. Returns 0, or a NET_ERR_*.
 */
static int recv_pdu(rdp_t* s, uint32_t timeout_ms) {
    int r = fill(s, 1, timeout_ms);
    if (r) return r;
    s->pdu_fastpath = 0;
    s->pdu_channel = 0xFFFF;

    if (s->in[0] == 0x03) {                          /* TPKT */
        if ((r = fill(s, 4, timeout_ms))) return r;
        uint32_t total = (uint32_t)s->in[2] << 8 | s->in[3];
        if (total < 5 || total > IN_CAP) return NET_ERR_PROTO;
        if ((r = fill(s, total, timeout_ms))) return r;
        uint32_t x = 4 + 1 + s->in[4];               /* past TPKT + X.224 header */
        if (x > total) return NET_ERR_PROTO;
        rdp_r_t rr = { s->in, total, x, 0 };
        if (rr.off < total) {
            uint8_t choice = rr_u8(&rr) >> 2;
            if (choice == MCS_SDRQ) {
                rr_skip(&rr, 2);                     /* initiator */
                s->pdu_channel = rr_be16(&rr);
                rr_skip(&rr, 1);                     /* dataPriority */
                uint32_t dl = rr_u8(&rr);
                if (dl & 0x80) dl = (dl & 0x7F) << 8 | rr_u8(&rr);
                if (rr.err || rr.off + dl > total) { in_consume(s, total); return NET_ERR_PROTO; }
                memcpy(s->pdu, s->in + rr.off, dl);
                s->pdu_len = dl;
                in_consume(s, total);
                return 0;
            }
            if (choice == MCS_DPUM) { in_consume(s, total); return NET_ERR_CLOSED; }
        }
        memcpy(s->pdu, s->in + x, total - x);        /* an MCS control PDU */
        s->pdu_len = total - x;
        in_consume(s, total);
        return 0;
    }

    if ((r = fill(s, 2, timeout_ms))) return r;      /* fast-path input */
    uint32_t h1 = s->in[1], total, lenb;
    if (h1 & 0x80) { if ((r = fill(s, 3, timeout_ms))) return r; total = (h1 & 0x7F) << 8 | s->in[2]; lenb = 2; }
    else { total = h1; lenb = 1; }
    (void)lenb;
    if (total < 2 || total > IN_CAP) return NET_ERR_PROTO;
    if ((r = fill(s, total, timeout_ms))) return r;
    memcpy(s->pdu, s->in, total);
    s->pdu_len = total;
    s->pdu_fastpath = 1;
    in_consume(s, total);
    return 0;
}

/* ── MCS / share writers ──────────────────────────────────────────── */

/* `payload` as a Send Data Indication on `channel` (TPKT + X.224 + MCS) */
static int send_sdin(rdp_t* s, uint16_t channel, const uint8_t* payload, uint32_t len) {
    uint8_t h[16];
    rdp_w_t w = { h, 0, sizeof(h), 0 };
    uint32_t total = 4 + 3 + 7 + (len >= 128 ? 1 : 0) + len;
    rw_u8(&w, 3); rw_u8(&w, 0); rw_be16(&w, (uint16_t)total);
    rw_u8(&w, 2); rw_u8(&w, 0xF0); rw_u8(&w, 0x80);  /* X.224 data */
    rw_u8(&w, (uint8_t)(MCS_SDIN << 2));
    rw_be16(&w, SVR_CHANNEL - MCS_USERCHANNEL_BASE); /* initiator, PER: minus 1001 */
    rw_be16(&w, channel);
    rw_u8(&w, 0x70);
    if (len >= 128) rw_u8(&w, (uint8_t)(0x80 | (len >> 8)));
    rw_u8(&w, (uint8_t)len);
    if (w.err) return -1;
    uint8_t small[2048];
    if (w.len + len <= sizeof(small)) {
        memcpy(small, h, w.len);
        memcpy(small + w.len, payload, len);
        return tls_send(s, small, w.len + len);
    }
    if (tls_send(s, h, w.len)) return -1;
    return tls_send(s, payload, len);
}

static void put_share_control(uint8_t* p, uint16_t type, uint16_t len) {
    rdp_w_t w = { p, 0, 6, 0 };
    rw_le16(&w, len);
    rw_le16(&w, (uint16_t)(type | PDU_PROTOCOL_VERSION));
    rw_le16(&w, SVR_CHANNEL);
}

/* ── capability sets (2.2.7) ──────────────────────────────────────── */

static void cap_end(rdp_w_t* w, uint32_t m) {
    uint16_t n = (uint16_t)(w->len - m);
    w->p[m + 2] = (uint8_t)n;
    w->p[m + 3] = (uint8_t)(n >> 8);
}
static void cap_general(rdp_w_t* w) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_GENERAL); rw_le16(w, 0);
    rw_le16(w, 1); rw_le16(w, 3);                    /* os WINDOWS / WINDOWSNT */
    rw_le16(w, 0x0200); rw_le16(w, 0);
    rw_le16(w, 0);                                   /* generalCompressionTypes */
    rw_le16(w, 0x0001 | 0x0400);                     /* FASTPATH_OUTPUT | NO_BITMAP_COMPRESSION_HDR */
    rw_le16(w, 0); rw_le16(w, 0); rw_le16(w, 0);
    rw_u8(w, 0); rw_u8(w, 0);                        /* refreshRect / suppressOutput */
    cap_end(w, m);
}
static void cap_bitmap(rdp_w_t* w, int width, int height) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_BITMAP); rw_le16(w, 0);
    rw_le16(w, 32);                                  /* preferredBitsPerPixel */
    rw_le16(w, 1); rw_le16(w, 1); rw_le16(w, 1);
    rw_le16(w, (uint16_t)width); rw_le16(w, (uint16_t)height);
    rw_le16(w, 0);
    rw_le16(w, 1);                                   /* desktopResizeFlag */
    rw_le16(w, 1);                                   /* bitmapCompressionFlag */
    rw_u8(w, 0); rw_u8(w, 0);
    rw_le16(w, 1);                                   /* multipleRectangleSupport */
    rw_le16(w, 0);
    cap_end(w, m);
}
static void cap_order(rdp_w_t* w) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_ORDER); rw_le16(w, 0);
    for (int i = 0; i < 16; i++) rw_u8(w, 0);        /* terminalDescriptor */
    rw_le32(w, 0);
    rw_le16(w, 1); rw_le16(w, 20);
    rw_le16(w, 0); rw_le16(w, 1);
    rw_le16(w, 0);                                   /* numberFonts */
    rw_le16(w, 0x0022);                              /* NEGOTIATE | ZEROBOUNDSDELTA */
    for (int i = 0; i < 32; i++) rw_u8(w, 0);        /* orderSupport: none (bitmaps only) */
    rw_le16(w, 0); rw_le16(w, 0);
    rw_le32(w, 0);
    rw_le32(w, 230400);                              /* desktopSaveSize */
    rw_le16(w, 0); rw_le16(w, 0);
    rw_le16(w, 0); rw_le16(w, 0);
    cap_end(w, m);
}
static void cap_pointer(rdp_w_t* w) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_POINTER); rw_le16(w, 0);
    rw_le16(w, 0);                                   /* colorPointerFlag */
    rw_le16(w, 20); rw_le16(w, 20);
    cap_end(w, m);
}
static void cap_input(rdp_w_t* w) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_INPUT); rw_le16(w, 0);
    rw_le16(w, 0x0001 | 0x0008 | 0x0020);            /* SCANCODES | FASTPATH | FASTPATH2 */
    rw_le16(w, 0);
    rw_le32(w, 0x00000409);                          /* keyboardLayout US */
    rw_le32(w, 4); rw_le32(w, 0); rw_le32(w, 12);
    for (int i = 0; i < 64; i++) rw_u8(w, 0);        /* imeFileName */
    cap_end(w, m);
}
static void cap_u32(rdp_w_t* w, uint16_t type, uint32_t body) {
    uint32_t m = w->len;
    rw_le16(w, type); rw_le16(w, 0);
    rw_le32(w, body);
    cap_end(w, m);
}
static void cap_share(rdp_w_t* w) {
    uint32_t m = w->len;
    rw_le16(w, CAPSET_SHARE); rw_le16(w, 0);
    rw_le16(w, SVR_CHANNEL); rw_le16(w, 0xB5E9);
    cap_end(w, m);
}

static int send_demand_active(rdp_t* s) {
    uint8_t* body = s->out + 6;
    rdp_w_t w = { body, 0, OUT_CAP - 64, 0 };
    rw_le32(&w, SHARE_ID);
    rw_le16(&w, 4);                                  /* lengthSourceDescriptor */
    uint32_t caplen_at = w.len;
    rw_le16(&w, 0);                                  /* lengthCombinedCapabilities */
    rw_bytes(&w, "RDP\0", 4);
    uint32_t ncap_at = w.len;
    rw_le16(&w, 0);                                  /* numberCapabilities */
    rw_le16(&w, 0);
    uint32_t caps_start = w.len;
    int n = 0;
    cap_general(&w); n++;
    cap_bitmap(&w, s->width, s->height); n++;
    cap_order(&w); n++;
    cap_pointer(&w); n++;
    cap_input(&w); n++;
    cap_share(&w); n++;
    cap_u32(&w, CAPSET_COLORCACHE, 6); n++;
    cap_u32(&w, CAPSET_VIRTUALCHANNEL, 0); n++;
    cap_u32(&w, CAPSET_FONT, 0x0001); n++;
    cap_u32(&w, CAPSET_MULTIFRAGMENT, 0xFFFF); n++;
    uint32_t combined = (w.len - caps_start) + 4;
    w.p[caplen_at] = (uint8_t)combined; w.p[caplen_at + 1] = (uint8_t)(combined >> 8);
    w.p[ncap_at] = (uint8_t)n; w.p[ncap_at + 1] = (uint8_t)(n >> 8);
    rw_le32(&w, 0);                                  /* sessionId */
    if (w.err) return -1;
    put_share_control(s->out, PDUTYPE_DEMANDACTIVE, (uint16_t)(w.len + 6));
    return send_sdin(s, MCS_GLOBAL_CHANNEL, s->out, w.len + 6);
}

/* a Data PDU (share data header) on the I/O channel */
static int send_data_pdu(rdp_t* s, uint8_t pdu_type2, const uint8_t* payload, uint32_t len) {
    uint8_t* p = s->out;
    uint32_t total = 6 + 12 + len;
    put_share_control(p, PDUTYPE_DATA, (uint16_t)total);
    rdp_w_t w = { p + 6, 0, 12, 0 };
    rw_le32(&w, SHARE_ID);
    rw_u8(&w, 0); rw_u8(&w, 1);                      /* pad, streamId LOW */
    rw_le16(&w, (uint16_t)(4 + len));                /* uncompressedLength: from pduType2 on
                                                        (MS-RDPBCGR 4.1.14 - Windows checks it) */
    rw_u8(&w, pdu_type2);
    rw_u8(&w, 0); rw_le16(&w, 0);                    /* compression */
    memcpy(p + 18, payload, len);
    return send_sdin(s, MCS_GLOBAL_CHANNEL, p, total);
}

static int send_sync(rdp_t* s) {
    uint8_t b[4]; rdp_w_t w = { b, 0, 4, 0 };
    rw_le16(&w, 1); rw_le16(&w, SVR_CHANNEL);        /* SYNCMSGTYPE_SYNC, targetUser */
    return send_data_pdu(s, PDUTYPE2_SYNCHRONIZE, b, 4);
}
static int send_control(rdp_t* s, uint16_t action) {
    uint8_t b[8]; rdp_w_t w = { b, 0, 8, 0 };
    /* Granted Control: grantId = the client's user channel, controlId = the server */
    int granted = action == CTRLACTION_GRANTED_CONTROL;
    rw_le16(&w, action); rw_le16(&w, granted ? s->client_user : 0);
    rw_le32(&w, granted ? SVR_CHANNEL : 0);
    return send_data_pdu(s, PDUTYPE2_CONTROL, b, 8);
}
static int send_font_map(rdp_t* s) {
    uint8_t b[8]; rdp_w_t w = { b, 0, 8, 0 };
    rw_le16(&w, 0); rw_le16(&w, 0); rw_le16(&w, 0x0003); rw_le16(&w, 4);
    return send_data_pdu(s, PDUTYPE2_FONTMAP, b, 8);
}

/* License Error PDU - STATUS_VALID_CLIENT (2.2.1.12) */
static int send_license_valid(rdp_t* s) {
    uint8_t b[24]; rdp_w_t w = { b, 0, sizeof(b), 0 };
    rw_le16(&w, SEC_LICENSE_PKT); rw_le16(&w, 0);
    rw_u8(&w, 0xFF);                                 /* ERROR_ALERT */
    rw_u8(&w, 3);                                    /* preamble version */
    rw_le16(&w, 16);
    rw_le32(&w, 0x00000007);                         /* STATUS_VALID_CLIENT */
    rw_le32(&w, 0x00000002);                         /* ST_NO_TRANSITION */
    rw_le32(&w, 0);                                  /* blob type + length */
    return send_sdin(s, MCS_GLOBAL_CHANNEL, b, w.len);
}

/* ── MCS connection sequence ──────────────────────────────────────── */
/* (parse_connect, build_connect_response, join handling, client info) */

/* find the GCC user data blocks (after the "Duca" h221 key) and read the
 * client's desktop size (CS_CORE 0xC001) and channels (CS_NET 0xC003) */
static void parse_connect(rdp_t* s, const uint8_t* p, uint32_t n) {
    uint32_t i = 0;
    while (i + 4 < n && !(p[i] == 'D' && p[i + 1] == 'u' && p[i + 2] == 'c' && p[i + 3] == 'a')) i++;
    if (i + 6 >= n) return;
    i += 4;
    i += 2;                                          /* PER length of the user data */
    while (i + 4 <= n) {
        uint16_t type = (uint16_t)(p[i] | p[i + 1] << 8);
        uint16_t len = (uint16_t)(p[i + 2] | p[i + 3] << 8);
        if (len < 4 || i + len > n) break;
        const uint8_t* b = p + i + 4;
        uint32_t bl = len - 4u;
        if (type == 0xC001 && bl >= 12) {            /* CS_CORE: desktopWidth/Height */
            int w = b[4] | b[5] << 8, h = b[6] | b[7] << 8;
            if (w >= 640 && w <= 4096 && h >= 480 && h <= 2048) { s->width = w; s->height = h; }
        } else if (type == 0xC003 && bl >= 4) {      /* CS_NET: channelCount, names */
            uint32_t cc = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
            for (uint32_t c = 0; c < cc && c < 16 && 4 + c * 12 + 12 <= bl; c++)
                s->channels[s->channel_count++] = MCS_GLOBAL_CHANNEL + 1 + (uint16_t)c;
            (void)cc;
        }
        i += len;
    }
}

/* MCS Connect Response with a GCC Conference Create Response (2.2.1.4) */
static int send_connect_response(rdp_t* s) {
    uint8_t ud[256];                                 /* the GCC user data blocks */
    rdp_w_t u = { ud, 0, sizeof(ud), 0 };
    /* SC_CORE (0x0C01) */
    rw_le16(&u, 0x0C01); rw_le16(&u, 16);
    rw_le32(&u, 0x00080004);                         /* version */
    rw_le32(&u, s->requested);                       /* clientRequestedProtocols: must echo
                                                        the request (Windows checks it) */
    rw_le32(&u, 0);                                  /* earlyCapabilityFlags */
    /* SC_NET (0x0C03): I/O channel + the client's virtual channels */
    int cc = s->channel_count;
    uint16_t netlen = (uint16_t)(8 + 2 * cc + (cc & 1 ? 2 : 0));
    rw_le16(&u, 0x0C03); rw_le16(&u, netlen);
    rw_le16(&u, MCS_GLOBAL_CHANNEL);
    rw_le16(&u, (uint16_t)cc);
    for (int i = 0; i < cc; i++) rw_le16(&u, s->channels[i]);
    if (cc & 1) rw_le16(&u, 0);
    /* SC_SEC (0x0C02): no RDP encryption (TLS does it) */
    rw_le16(&u, 0x0C02); rw_le16(&u, 12);
    rw_le32(&u, 0); rw_le32(&u, 0);
    if (u.err) return -1;

    uint8_t gcc[320];
    rdp_w_t g = { gcc, 0, sizeof(gcc), 0 };
    static const uint8_t HDR[] = { 0x00, 0x05, 0x00, 0x14, 0x7C, 0x00, 0x01,
                                   0x2A, 0x14, 0x76, 0x0A, 0x01, 0x01, 0x00, 0x01,
                                   0xC0, 0x00, 0x4D, 0x63, 0x44, 0x6E };     /* ...McDn */
    rw_bytes(&g, HDR, sizeof(HDR));
    if (u.len >= 128) rw_u8(&g, (uint8_t)(0x80 | (u.len >> 8)));   /* PER length of user data */
    rw_u8(&g, (uint8_t)u.len);
    rw_bytes(&g, ud, u.len);
    if (g.err) return -1;

    uint8_t mcs[400];
    rdp_w_t m = { mcs, 0, sizeof(mcs), 0 };
    rw_u8(&m, 0x7F); rw_u8(&m, 0x66);                /* Connect-Response [APPLICATION 102] */
    rw_u8(&m, 0x82); rw_be16(&m, (uint16_t)(3 + 3 + 28 + 4 + g.len));
    rw_u8(&m, 0x0A); rw_u8(&m, 0x01); rw_u8(&m, 0x00);           /* result successful */
    rw_u8(&m, 0x02); rw_u8(&m, 0x01); rw_u8(&m, 0x00);           /* calledConnectId */
    /* domainParameters: maxChannelIds 34, maxUserIds 3, maxTokenIds 0,
     * numPriorities 1, minThroughput 0, maxHeight 1, maxMCSPDUsize 65535,
     * protocolVersion 2 */
    static const uint8_t DP[] = { 0x30, 0x1A, 0x02, 0x01, 0x22, 0x02, 0x01, 0x03,
                                  0x02, 0x01, 0x00, 0x02, 0x01, 0x01, 0x02, 0x01,
                                  0x00, 0x02, 0x01, 0x01, 0x02, 0x03, 0x00, 0xFF,
                                  0xFF, 0x02, 0x01, 0x02 };       /* 28 bytes */
    rw_bytes(&m, DP, sizeof(DP));
    rw_u8(&m, 0x04); rw_u8(&m, 0x82); rw_be16(&m, (uint16_t)g.len);   /* userData OCTET STRING */
    rw_bytes(&m, gcc, g.len);
    if (m.err) return -1;

    uint8_t tp[8];
    rdp_w_t t = { tp, 0, sizeof(tp), 0 };
    rw_u8(&t, 3); rw_u8(&t, 0); rw_be16(&t, (uint16_t)(m.len + 7));
    rw_u8(&t, 2); rw_u8(&t, 0xF0); rw_u8(&t, 0x80);
    if (tls_send(s, tp, t.len)) return -1;
    return tls_send(s, mcs, m.len);
}

/* the credentials from the Client Info PDU (2.2.1.11.1.1) */
static int check_client_info(rdp_t* s) {
    rdp_r_t r = { s->pdu, s->pdu_len, 0, 0 };
    uint16_t flags = rr_le16(&r);
    rr_le16(&r);
    if (!(flags & SEC_INFO_PKT)) return 0;           /* not the info PDU */
    rr_le32(&r);                                     /* codePage */
    uint32_t infoflags = rr_le32(&r);
    int unicode = (infoflags & 0x00000010) != 0;     /* INFO_UNICODE */
    uint16_t cbDomain = rr_le16(&r);
    uint16_t cbUser = rr_le16(&r);
    uint16_t cbPassword = rr_le16(&r);
    uint16_t cbAltDir = rr_le16(&r);
    uint16_t cbWorkDir = rr_le16(&r);
    (void)cbAltDir; (void)cbWorkDir;
    rr_skip(&r, cbDomain + (unicode ? 2 : 1));       /* Domain + its terminator */
    char user[64];
    int ui = 0;
    for (uint16_t i = 0; i < cbUser && ui < 63; i += unicode ? 2 : 1) {
        uint8_t c = rr_u8(&r);
        if (unicode) rr_u8(&r);
        if (c) user[ui++] = (char)c;
    }
    user[ui] = 0;
    /* Windows clients may send ".\banana" or "PC\banana": drop the domain part */
    const char* name = user;
    for (const char* q = user; *q; q++) if (*q == '\\') name = q + 1;
    rr_skip(&r, unicode ? 2 : 1);
    char pw[128];
    int pi = 0;
    for (uint16_t i = 0; i < cbPassword && pi < 127; i += unicode ? 2 : 1) {
        uint8_t c = rr_u8(&r);
        if (unicode) rr_u8(&r);
        if (c) pw[pi++] = (char)c;
    }
    pw[pi] = 0;
    if (r.err) { memset(pw, 0, sizeof(pw)); return 0; }

    /* (the desktop is shared: the one logged in on it) */
    const user_t* me = session_user();
    const char* who = me ? me->name : PASSWD_USER;
    int ok = name[0] && strcasecmp(name, who) == 0 && (passwd_is_set(who) ? passwd_check(who, pw) : 1);
    memset(pw, 0, sizeof(pw));
    if (ok) {
        s->authed = 1;
        kstrlcpy(g_sess.user, user, sizeof(g_sess.user));
        klog("rdpd: %s: %s logged in\n", s->peer, user);
    } else if (user[0]) {
        klog("rdpd: %s: wrong credentials for %s\n", s->peer, user);
        note_failure(s->peer_ip);
    }
    return ok;
}

/* ── screen mirroring ─────────────────────────────────────────────── */

#ifdef RDPD_HOST_TEST
int rdpd_tiles_sent;
#endif

/* one 64x64 tile as a fast-path uncompressed 32bpp bitmap update */
/* one uncompressed 32bpp rectangle (bottom-up BGRX) as a fast-path bitmap update */
static int send_rect(rdp_t* s, int tx, int ty, int tw, int th, const uint8_t* bgrx) {
    uint32_t bytes = (uint32_t)tw * th * 4;
    uint32_t ulen = 2 + 2 + 18 + bytes;              /* updateType + numberRectangles + TS_BITMAP_DATA */
    uint32_t total = 1 + 2 + 1 + 2 + ulen;           /* header + len16 + updateHeader + size16 */
    uint8_t* p = s->out;
    rdp_w_t w = { p, 0, OUT_CAP, 0 };
    rw_u8(&w, 0x00);                                 /* fast-path, no encryption */
    rw_u8(&w, (uint8_t)(0x80 | (total >> 8)));
    rw_u8(&w, (uint8_t)total);
    rw_u8(&w, FASTPATH_UPDATETYPE_BITMAP);           /* single fragment, no compression */
    rw_le16(&w, (uint16_t)ulen);
    rw_le16(&w, UPDATETYPE_BITMAP);                  /* updateType (fast-path bitmap) */
    rw_le16(&w, 1);                                  /* numberRectangles */
    rw_le16(&w, (uint16_t)tx);
    rw_le16(&w, (uint16_t)ty);
    rw_le16(&w, (uint16_t)(tx + tw - 1));
    rw_le16(&w, (uint16_t)(ty + th - 1));
    rw_le16(&w, (uint16_t)tw);
    rw_le16(&w, (uint16_t)th);
    rw_le16(&w, 32);                                 /* bitsPerPixel */
    rw_le16(&w, 0);                                  /* flags: uncompressed */
    rw_le16(&w, (uint16_t)bytes);
    rw_bytes(&w, bgrx, bytes);
    if (w.err) return -1;
    return tls_send(s, p, w.len);
}

/* Windows clients refuse a fast-path PDU over 0x3FFF bytes (a 64x64 32bpp
 * tile is 16 KiB), so a tile goes as bands of rows that fit. The tile is
 * bottom-up: the band of rows [r, r + h) from the top starts at row th - r - h. */
#define FASTPATH_MAX 0x3FFF
static int send_tile(rdp_t* s, int tx, int ty, int tw, int th, const uint8_t* bgrx) {
#ifdef RDPD_HOST_TEST
    rdpd_tiles_sent++;
#endif
    int band = (FASTPATH_MAX - 32) / (tw * 4);
    for (int r = 0; r < th; r += band) {
        int h = th - r < band ? th - r : band;
        if (send_rect(s, tx, ty + r, tw, h, bgrx + (uint32_t)(th - r - h) * tw * 4) != 0) return -1;
    }
    return 0;
}

/* diff the screen against the shadow; send the changed tiles. Returns the
 * number sent, or -1 on a transport error. */
static int send_screen(rdp_t* s, int force) {
    int stride, fw, fh;
    uint32_t* fb = fb_target(&stride, &fw, &fh);
    int sent = 0;
    for (int ty = 0; ty < s->height; ty += TILE) {
        int th = s->height - ty < TILE ? s->height - ty : TILE;
        for (int tx = 0; tx < s->width; tx += TILE) {
            int tw = s->width - tx < TILE ? s->width - tx : TILE;
            uint32_t* dst = (uint32_t*)s->tile;
            for (int y = 0; y < th; y++) {
                uint32_t* row = dst + y * tw;
                if (fb && ty + y < fh) {
                    for (int x = 0; x < tw; x++) row[x] = tx + x < fw ? fb[(ty + y) * stride + tx + x] : 0;
                } else {
                    for (int x = 0; x < tw; x++) row[x] = 0;
                }
            }
            /* compare against the shadow */
            int changed = force;
            uint32_t* sh = s->shadow + ty * s->width + tx;
            for (int y = 0; y < th && !changed; y++)
                for (int x = 0; x < tw; x++)
                    if (sh[y * s->width + x] != dst[y * tw + x]) { changed = 1; break; }
            if (!changed) continue;
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++) sh[y * s->width + x] = dst[y * tw + x];
            /* RDP bitmap data is bottom-up: last row first */
            uint8_t* line = s->tile + (uint32_t)tw * th * 4;   /* scratch after the tile */
            for (int y = 0; y < th; y++)
                memcpy(line + (uint32_t)y * tw * 4, dst + (th - 1 - y) * tw, (uint32_t)tw * 4);
            if (send_tile(s, tx, ty, tw, th, line) != 0) return -1;
            sent++;
        }
    }
    return sent;
}

/* ── input from the client ────────────────────────────────────────── */

static int g_mouse_buttons;

static void feed_scancode(uint16_t code, uint16_t flags) {
    if (flags & KBDFLAGS_EXTENDED) keyboard_feed_set1(0xE0);
    uint8_t sc = (uint8_t)code;
    if (flags & KBDFLAGS_RELEASE) sc |= 0x80;
    keyboard_feed_set1(sc);
}

static void do_mouse(rdp_t* s, uint16_t flags, int x, int y) {
    if (flags & PTRFLAGS_WHEEL) {
        int d = flags & 0xFF;
        if (flags & PTRFLAGS_WHEEL_NEGATIVE) d = -(256 - d);
        mouse_inject_wheel(d < 0 ? 1 : -1);
        return;
    }
    if (flags & PTRFLAGS_BUTTON1) g_mouse_buttons = (flags & PTRFLAGS_DOWN) ? (g_mouse_buttons | 1) : (g_mouse_buttons & ~1);
    if (flags & PTRFLAGS_BUTTON2) g_mouse_buttons = (flags & PTRFLAGS_DOWN) ? (g_mouse_buttons | 2) : (g_mouse_buttons & ~2);
    if (flags & PTRFLAGS_BUTTON3) g_mouse_buttons = (flags & PTRFLAGS_DOWN) ? (g_mouse_buttons | 4) : (g_mouse_buttons & ~4);
    int ax = s->width > 1 ? x * 65535 / (s->width - 1) : 0;
    int ay = s->height > 1 ? y * 65535 / (s->height - 1) : 0;
    mouse_inject_abs(ax, ay, g_mouse_buttons);
}

static void handle_fastpath_input(rdp_t* s) {
    rdp_r_t r = { s->pdu, s->pdu_len, 0, 0 };
    uint8_t h0 = rr_u8(&r);
    uint32_t len = rr_u8(&r);
    if (len & 0x80) rr_u8(&r);
    int nev = h0 >> 2;
    if (nev == 0) nev = rr_u8(&r);
    for (int i = 0; i < nev && !r.err; i++) {
        uint8_t eh = rr_u8(&r);
        int code = eh >> 5;
        int ef = eh & 0x1F;
        if (code == FASTPATH_INPUT_EVENT_SCANCODE) {
            uint16_t kf = KBDFLAGS_DOWN;
            if (ef & 0x01) kf = KBDFLAGS_RELEASE;
            if (ef & 0x02) kf |= KBDFLAGS_EXTENDED;
            feed_scancode(rr_u8(&r), kf);
        } else if (code == FASTPATH_INPUT_EVENT_MOUSE) {
            uint16_t fl = rr_le16(&r);
            int x = rr_le16(&r), y = rr_le16(&r);
            do_mouse(s, fl, x, y);
        } else if (code == FASTPATH_INPUT_EVENT_MOUSEX) {
            rr_le16(&r); rr_le16(&r); rr_le16(&r);
        } else if (code == FASTPATH_INPUT_EVENT_UNICODE) {
            rr_le16(&r);
        } else if (code == FASTPATH_INPUT_EVENT_SYNC) {
            /* lock-key state: nothing to do */
        } else break;
    }
}

static void handle_slowpath_input(rdp_t* s, rdp_r_t* r) {
    uint16_t nev = rr_le16(r);
    rr_le16(r);                                      /* pad */
    for (uint16_t i = 0; i < nev && !r->err; i++) {
        rr_le32(r);                                  /* eventTime */
        uint16_t type = rr_le16(r);
        if (type == INPUT_EVENT_SCANCODE) {
            uint16_t fl = rr_le16(r);
            uint16_t code = rr_le16(r);
            feed_scancode(code, fl);
            rr_le16(r);                              /* pad / per spec: 2 bytes */
        } else if (type == INPUT_EVENT_MOUSE) {
            uint16_t fl = rr_le16(r);
            int x = rr_le16(r), y = rr_le16(r);
            do_mouse(s, fl, x, y);
        } else {                                     /* sync, unicode, mousex */
            rr_le16(r); rr_le16(r); rr_le16(r);
        }
    }
}

/* ── one share-data PDU during finalization / active ──────────────── */

/* returns 1 if the Font List arrived (finalization complete) */
static int handle_share_pdu(rdp_t* s) {
    rdp_r_t r = { s->pdu, s->pdu_len, 0, 0 };
    rr_le16(&r);                                     /* totalLength */
    uint16_t type = rr_le16(&r) & 0x000F;
    rr_le16(&r);                                     /* PDUSource */
    if (type == PDUTYPE_CONFIRMACTIVE) return 0;
    if (type != PDUTYPE_DATA) return 0;
    rr_le32(&r);                                     /* shareId */
    rr_u8(&r); rr_u8(&r);                            /* pad, streamId */
    rr_le16(&r);                                     /* uncompressedLength */
    uint8_t type2 = rr_u8(&r);
    rr_u8(&r); rr_le16(&r);                          /* compression */
    if (r.err) return 0;
    switch (type2) {
    case PDUTYPE2_INPUT:      handle_slowpath_input(s, &r); return 0;
    case PDUTYPE2_SYNCHRONIZE: send_sync(s); return 0;
    case PDUTYPE2_CONTROL: {
        uint16_t action = rr_le16(&r);
        if (action == CTRLACTION_COOPERATE) send_control(s, CTRLACTION_COOPERATE);
        else if (action == CTRLACTION_REQUEST_CONTROL) send_control(s, CTRLACTION_GRANTED_CONTROL);
        return 0;
    }
    case PDUTYPE2_FONTLIST:   send_font_map(s); return 1;
    default:                  return 0;             /* refresh rect, suppress output, ... */
    }
}

/* ── one connection ───────────────────────────────────────────────── */

/* read a raw TPKT over plaintext TCP (the X.224 negotiation), into buf */
static int recv_tcp_tpkt(tcp_conn_t* c, uint8_t* buf, uint32_t cap, uint32_t timeout_ms) {
    uint32_t have = 0;
    uint32_t start = timer_ms();
    while (have < 4 || have < ((uint32_t)buf[2] << 8 | buf[3])) {
        if (timer_ms() - start >= timeout_ms) return -1;
        int r = tcp_recv(c, buf + have, cap - have, 2000);
        if (r == 0 || (r < 0 && r != NET_ERR_TIMEOUT)) return -1;
        if (r > 0) have += (uint32_t)r;
        if (have >= 4) {
            uint32_t want = (uint32_t)buf[2] << 8 | buf[3];
            if (want < 4 || want > cap) return -1;
        }
    }
    return (int)have;
}

static int negotiate(tcp_conn_t* c, uint32_t* selected, uint32_t* requested) {
    uint8_t buf[256];
    int n = recv_tcp_tpkt(c, buf, sizeof(buf), 15000);
    if (n < 11 || buf[0] != 3) return -1;
    uint32_t req = PROTOCOL_RDP;
    int have_req = 0;
    for (int i = 11; i + 8 <= n; i++)
        if (buf[i] == RDP_NEG_REQ && buf[i + 2] == 8 && buf[i + 3] == 0) {
            req = (uint32_t)buf[i + 4] | buf[i + 5] << 8 | buf[i + 6] << 16 | (uint32_t)buf[i + 7] << 24;
            have_req = 1;
            break;
        }
    *requested = req;
    uint8_t out[19];
    rdp_w_t w = { out, 0, sizeof(out), 0 };
    rw_u8(&w, 3); rw_u8(&w, 0); rw_be16(&w, 19);
    rw_u8(&w, 14); rw_u8(&w, X224_CC); rw_be16(&w, 0); rw_be16(&w, 0x1234); rw_u8(&w, 0);
    if (!have_req || (req & PROTOCOL_SSL)) {
        rw_u8(&w, RDP_NEG_RSP); rw_u8(&w, 0); rw_le16(&w, 8); rw_le32(&w, PROTOCOL_SSL);
        *selected = PROTOCOL_SSL;
        tcp_send(c, out, w.len, 5000);
        return 0;
    }
    /* the client wants NLA / something we do not do: a negotiation failure */
    rw_u8(&w, RDP_NEG_FAILURE); rw_u8(&w, 0); rw_le16(&w, 8); rw_le32(&w, SSL_REQUIRED_BY_SERVER);
    tcp_send(c, out, w.len, 5000);
    return -1;
}

static void run_session(int slot, tcp_conn_t* c) {
    rdp_t* s = (rdp_t*)kzalloc(sizeof(rdp_t));
    uint8_t* bufs = s ? (uint8_t*)kmalloc(IN_CAP + IN_CAP + OUT_CAP) : NULL;
    if (!s || !bufs) { kfree(s); kfree(bufs); tcp_abort(c); return; }
    s->tcp = c; s->slot = slot;
    s->in = bufs; s->pdu = bufs + IN_CAP; s->out = bufs + 2 * IN_CAP;
    tcp_peer(c, &s->peer_ip, NULL);
    ip4_to_str(s->peer_ip, s->peer);
    kstrlcpy(g_sess.peer, s->peer, sizeof(g_sess.peer));
    g_sess.user[0] = 0;
    g_sess.since = timer_ms();
    klog("rdpd: connection from %s\n", s->peer);

    if (locked_out(s->peer_ip)) { klog("rdpd: %s: refused (too many failed logins)\n", s->peer); goto done; }

    uint32_t proto;
    if (negotiate(c, &proto, &s->requested) != 0) { klog("rdpd: %s: negotiation failed (needs TLS)\n", s->peer); goto done; }

    int err;
    char em[160];
    tls12_identity_t id = { &g_key, g_cert, g_cert_len };
    s->tls = tls12_accept(c, &id, CONNECT_TIMEOUT, &err, em, sizeof(em));
    if (!s->tls) { klog("rdpd: %s: TLS failed: %s\n", s->peer, em); goto done; }

    /* MCS Connect Initial -> Connect Response */
    s->width = 1024; s->height = 768;
    if (recv_pdu(s, CONNECT_TIMEOUT)) goto bye;
    parse_connect(s, s->pdu, s->pdu_len);
    if (send_connect_response(s)) goto bye;

    /* Erect Domain, Attach User -> confirm, Channel Joins -> confirm, then
     * the Client Info PDU (the first Send Data Request) */
    /* the user channel comes after the virtual channels (1004...): if it
     * shared an id with one, Windows clients would loop on Channel Join */
    s->client_user = MCS_GLOBAL_CHANNEL + 1 + (uint16_t)s->channel_count;
    if (s->client_user < MCS_USERCHANNEL_BASE + 6) s->client_user = MCS_USERCHANNEL_BASE + 6;   /* 1007 */
    for (;;) {
        if (recv_pdu(s, CONNECT_TIMEOUT)) goto bye;
        if (s->pdu_channel != 0xFFFF) {              /* Send Data Request: Client Info */
            if (!check_client_info(s)) { klog("rdpd: %s: login required\n", s->peer); goto bye; }
            break;
        }
        uint8_t choice = s->pdu_len ? s->pdu[0] >> 2 : 0;
        if (choice == MCS_AURQ) {
            uint8_t b[8]; rdp_w_t w = { b, 0, sizeof(b), 0 };
            rw_u8(&w, (uint8_t)((MCS_AUCF << 2) | 2)); rw_u8(&w, 0);   /* result ok, initiator present */
            rw_be16(&w, (uint16_t)(s->client_user - MCS_USERCHANNEL_BASE));
            uint8_t tp[11]; rdp_w_t t = { tp, 0, sizeof(tp), 0 };
            rw_u8(&t, 3); rw_u8(&t, 0); rw_be16(&t, (uint16_t)(7 + w.len));
            rw_u8(&t, 2); rw_u8(&t, 0xF0); rw_u8(&t, 0x80);
            if (tls_send(s, tp, t.len) || tls_send(s, b, w.len)) goto bye;
        } else if (choice == MCS_CJRQ) {
            uint16_t chan = (uint16_t)(s->pdu[3] << 8 | s->pdu[4]);
            uint8_t b[16]; rdp_w_t w = { b, 0, sizeof(b), 0 };
            rw_u8(&w, (uint8_t)((MCS_CJCF << 2) | 2)); rw_u8(&w, 0);
            rw_be16(&w, (uint16_t)(s->client_user - MCS_USERCHANNEL_BASE));   /* initiator */
            rw_be16(&w, chan);                       /* requested channelId */
            rw_be16(&w, chan);                       /* actual channelId */
            uint8_t tp[11]; rdp_w_t t = { tp, 0, sizeof(tp), 0 };
            rw_u8(&t, 3); rw_u8(&t, 0); rw_be16(&t, (uint16_t)(7 + w.len));
            rw_u8(&t, 2); rw_u8(&t, 0xF0); rw_u8(&t, 0x80);
            if (tls_send(s, tp, t.len) || tls_send(s, b, w.len)) goto bye;
        }
        /* Erect Domain Request and anything else: ignored */
    }

    if (send_license_valid(s)) goto bye;

    /* capability exchange + finalization */
    int stride, fw, fh;
    uint32_t* fb = fb_target(&stride, &fw, &fh);
    if (fb && fw >= 640 && fh >= 480) { s->width = fw; s->height = fh; }
    s->shadow = (uint32_t*)kzalloc((uint32_t)s->width * s->height * 4);
    s->tile = (uint8_t*)kmalloc(TILE * TILE * 4 * 2);
    if (!s->shadow || !s->tile) goto bye;
    g_sess.w = s->width; g_sess.h = s->height;

    if (send_demand_active(s)) goto bye;
    int finalized = 0;
    uint32_t t0 = timer_ms();
    while (!finalized) {
        if (recv_pdu(s, CONNECT_TIMEOUT)) goto bye;
        if (s->pdu_fastpath) continue;
        if (s->pdu_channel == MCS_GLOBAL_CHANNEL) finalized = handle_share_pdu(s);
        if (timer_ms() - t0 > CONNECT_TIMEOUT) goto bye;
    }
    klog("rdpd: %s: session active (%dx%d)\n", s->peer, s->width, s->height);
    g_sess.active = 1;

    /* the live session: push screen changes, pull input */
    send_screen(s, 1);
    uint32_t last_frame = timer_ms();
    while (g_running && !s->dead) {
        int busy = 0;
        int r = recv_pdu(s, 0);                      /* non-blocking */
        if (r == 0) {
            if (s->pdu_fastpath) handle_fastpath_input(s);
            else if (s->pdu_channel == MCS_GLOBAL_CHANNEL) handle_share_pdu(s);
            busy = 1;
        } else if (r != NET_ERR_TIMEOUT) {
            break;                                   /* closed / reset */
        }
        if (timer_ms() - last_frame >= 40) {
            int n = send_screen(s, 0);
            if (n < 0) break;
            if (n > 0) busy = 1;
            last_frame = timer_ms();
        }
        if (!busy) net_wait(5);
    }
    klog("rdpd: %s: session closed\n", s->peer);

bye:
    if (s->tls) tls12_close(s->tls);
done:
    tcp_close(c);
    kfree(s->shadow);
    kfree(s->tile);
    kfree(bufs);
    kfree(s);
    g_sess.active = 0;
    g_sess.peer[0] = 0;
}

/* ── task and control ─────────────────────────────────────────────── */

static void session_loop(void) {
    task_set_background();
    for (;;) {
        if (!g_running || !g_listener) { task_sleep_ms(200); continue; }
        tcp_conn_t* c = tcp_accept(g_listener, 300);
        if (c) run_session(0, c);
    }
}

int rdpd_start(uint16_t port, char* err, int errcap) {
    if (!passwd_is_set(PASSWD_USER))
        klog("rdpd: note - no password is set; `passwd` secures the login\n");
    if (load_key() != 0) {
        ksnprintf(err, (size_t)errcap, "could not generate or save the server key");
        return -1;
    }
    if (g_running) {
        if (port == g_port) return 0;
        rdpd_stop();
    }
    int e;
    tcp_listener_t* l = tcp_listen(port, &e);
    if (!l) {
        ksnprintf(err, (size_t)errcap, e == NET_ERR_PROTO ? "port %u is already in use" : "no free listener slot", port);
        return -1;
    }
    if (!g_task_started) {
        if (task_create("rdpd", session_loop) < 0) {
            tcp_unlisten(l);
            ksnprintf(err, (size_t)errcap, "no free task slot");
            return -1;
        }
        g_task_started = 1;
    }
    g_listener = l;
    g_port = port;
    g_running = 1;
    return 0;
}

void rdpd_stop(void) {
    g_running = 0;
    if (g_listener) tcp_unlisten(g_listener);
    g_listener = NULL;
}

int rdpd_running(void) { return g_running; }
uint16_t rdpd_port(void) { return g_port; }

#ifdef RDPD_HOST_TEST
/* the host harness (rdpwork/h) drives one connection itself, since the
 * shim has no real tasks */
void rdpd_test_serve(uint16_t port, int rounds) {
    if (load_key() != 0) { klog("rdpd: no key\n"); return; }
    int e;
    tcp_listener_t* l = tcp_listen(port, &e);
    if (!l) { klog("rdpd: listen failed\n"); return; }
    g_running = 1;
    for (int i = 0; i < rounds; i++) {
        tcp_conn_t* c = NULL;
        while (!c) c = tcp_accept(l, 2000);
        run_session(0, c);
    }
    tcp_unlisten(l);
}
#endif

void rdpd_print_status(void) {
    char line[160];
    if (!g_running) { terminal_writeln("rdpd: stopped"); }
    else {
        char ip[16];
        ip4_to_str(net_if()->ip, ip);
        ksnprintf(line, sizeof(line), "rdpd: running on port %u - connect to %s", g_port, ip);
        terminal_writeln(line);
    }
    char fp[128];
    if (g_have_key && rdpd_fingerprint(fp, sizeof(fp)) == 0) {
        ksnprintf(line, sizeof(line), "      certificate: %s", fp);
        terminal_writeln(line);
    }
    if (g_sess.peer[0]) {
        uint32_t secs = (timer_ms() - g_sess.since) / 1000u;
        ksnprintf(line, sizeof(line), "      session: %s from %s, %ux%u, %um%02us",
                  g_sess.user[0] ? g_sess.user : "(connecting)", g_sess.peer, g_sess.w, g_sess.h, secs / 60u, secs % 60u);
        terminal_writeln(line);
    }
}
