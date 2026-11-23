/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "adbcmd.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/fs.h"
#include "../kernel/timer.h"
#include "../kernel/random.h"
#include "../kernel/keyboard.h"
#include "../kernel/gui.h"
#include "../kernel/rtc.h"
#include "../kernel/task.h"
#include "../usb/adbdev.h"
#include "../crypto/rsa.h"

/*
 * adb and fastboot, the Android platform tools, talking to a phone over
 * USB (usb/adbdev.c) - no adb server: each command connects, does its
 * work and lets go.
 *
 * adb: CNXN, then the phone's AUTH token signed with this computer's RSA
 * key (made once, kept in ~/.android); a phone that does not know the key
 * yet gets the public key and asks its user ("Allow USB debugging?").
 * Then streams: OPEN a service ("shell:...", "sync:", "reboot:"), WRTE
 * data both ways (each acknowledged with OKAY), CLSE. push / pull speak
 * the sync protocol (SEND / RECV, DATA, DONE) inside a "sync:" stream.
 *
 * fastboot: text commands, answered INFO... / TEXT... (shown) and finally
 * OKAY / FAIL / DATA (send that many bytes, as for download:).
 */

/* ══ output ════════════════════════════════════════════════════════ */

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[400];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static void err(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char* fmt, ...) {
    char buf[400];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write_color(buf, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static void out_bytes(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) terminal_putchar((char)p[i]);
}

/* Ctrl+C in the window running the command (other keys are dropped) */
static int interrupted(void) {
    if (gui_focused_vt() != terminal_vt_get_active()) return 0;
    char c;
    int hit = 0;
    while ((c = keyboard_try_getchar()) != 0) if (c == 3) hit = 1;
    return hit;
}

/* ══ files ═════════════════════════════════════════════════════════ */

static const char* base_name(const char* p) {
    const char* b = p;
    for (; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static int file_size(const char* path, uint32_t* size) {
    int idx = fs_find_file(path);
    if (idx < 0) return -1;
    fs_file_t* f = fs_file_info(idx);
    if (!f) return -1;
    *size = f->size;
    return idx;
}

/* ══ adb: messages ═════════════════════════════════════════════════ */

#define A_CNXN 0x4e584e43u
#define A_AUTH 0x48545541u
#define A_OPEN 0x4e45504fu
#define A_OKAY 0x59414b4fu
#define A_CLSE 0x45534c43u
#define A_WRTE 0x45545257u

#define A_VERSION  0x01000001u
#define A_MAXDATA  (256u * 1024u)
#define AUTH_TOKEN 1
#define AUTH_SIG   2
#define AUTH_PUB   3

typedef struct { uint32_t cmd, arg0, arg1, len, check, magic; } amsg_t;

static int      g_h = -1;
static uint32_t g_maxdata = 4096;
static uint8_t* g_pay;                       /* the last message's payload */
static char     g_banner[512];
static uint32_t g_next_local = 1;

static int send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void* data, uint32_t len) {
    amsg_t m;
    m.cmd = cmd; m.arg0 = a0; m.arg1 = a1; m.len = len; m.magic = cmd ^ 0xFFFFFFFFu;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += ((const uint8_t*)data)[i];
    m.check = sum;
    if (adbdev_write(g_h, &m, sizeof(m)) < 0) return -1;
    if (len && adbdev_write(g_h, data, len) < 0) return -1;
    return 0;
}

/* exactly len bytes: 0, -1 gone, -2 timeout, -3 Ctrl+C */
static int read_full(void* buf, uint32_t len, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0, t0 = timer_ms();
    while (got < len) {
        int n = adbdev_read(g_h, p + got, len - got, 50);
        if (n < 0) return -1;
        got += (uint32_t)n;
        if (n == 0) {
            if (timer_ms() - t0 >= timeout_ms) return -2;
            if (interrupted()) return -3;
        }
    }
    return 0;
}

/* the next message (its payload in g_pay): 0, or as read_full */
static int recv_msg(amsg_t* m, uint32_t timeout_ms) {
    for (;;) {
        int r = read_full(m, sizeof(*m), timeout_ms);
        if (r) return r;
        if (m->magic != (m->cmd ^ 0xFFFFFFFFu) || m->len > A_MAXDATA) {
            adbdev_flush_input(g_h);           /* out of step: start again from the next message */
            continue;
        }
        if (m->len && (r = read_full(g_pay, m->len, 5000)) != 0) return r;
        return 0;
    }
}

/* ══ adb: this computer's key ══════════════════════════════════════ */

#define KEY_DIR  "/home/banana/.android"
#define KEY_FILE KEY_DIR "/adbkey"
#define PUB_FILE KEY_DIR "/adbkey.pub"

static rsa_key_t g_key;
static int       g_have_key;

static void rnd(void* buf, uint32_t len) { random_bytes(buf, len); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64(const uint8_t* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        if (o + 4 >= cap) break;
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int load_key(void) {
    if (g_have_key) return 0;
    uint32_t size;
    int idx = file_size(KEY_FILE, &size);
    if (idx >= 0 && size == sizeof(g_key.n) + sizeof(g_key.d) &&
        fs_read(idx, 0, g_key.n, sizeof(g_key.n)) == (int)sizeof(g_key.n) &&
        fs_read(idx, sizeof(g_key.n), g_key.d, sizeof(g_key.d)) == (int)sizeof(g_key.d)) {
        g_key.e = 65537;
        g_have_key = 1;
        return 0;
    }
    say("adb: making this computer's key (once) - a few seconds...\n");
    if (rsa_generate(&g_key, rnd) != 0) { err("adb: the key could not be made"); return -1; }
    fs_mkdir_p(KEY_DIR);
    static uint8_t buf[sizeof(g_key.n) + sizeof(g_key.d)];
    memcpy(buf, g_key.n, sizeof(g_key.n));
    memcpy(buf + sizeof(g_key.n), g_key.d, sizeof(g_key.d));
    fs_write_path(KEY_FILE, buf, sizeof(buf));
    memset(buf, 0, sizeof(buf));
    static uint8_t pub[RSA_ANDROID_PUB_LEN];
    static char text[760];
    rsa_android_pubkey(&g_key, pub);
    int n = b64(pub, sizeof(pub), text, sizeof(text));
    kstrlcpy(text + n, " banana@banana-os\n", sizeof(text) - (size_t)n);
    fs_write_path(PUB_FILE, text, (uint32_t)strlen(text));
    g_have_key = 1;
    return 0;
}

/* ══ adb: connecting ═══════════════════════════════════════════════ */

static int pick(const char* serial, int quiet) {
    g_h = adbdev_open(ADBDEV_ADB, serial);
    if (g_h >= 0) return 0;
    if (!quiet) {
        if (adbdev_open(ADBDEV_FASTBOOT, serial) >= 0) err("adb: the phone is in its bootloader - use fastboot (or: fastboot reboot)");
        else if (serial && serial[0]) err("adb: device '%s' not found", serial);
        else err("adb: no devices/emulators found (USB debugging on, the USB cable plugged in?)");
    }
    return -1;
}

static int adb_connect(const char* serial) {
    if (pick(serial, 0) < 0) return -1;
    if (!g_pay) g_pay = (uint8_t*)kmalloc(A_MAXDATA + 16);
    if (!g_pay || load_key() < 0) return -1;
    adbdev_flush_input(g_h);
    static const char hello[] = "host::features=shell_v1";
    if (send_msg(A_CNXN, A_VERSION, A_MAXDATA, hello, sizeof(hello)) < 0) { err("adb: the phone does not answer"); return -1; }
    int sent_sig = 0, sent_pub = 0;
    uint32_t t0 = timer_ms();
    for (;;) {
        amsg_t m;
        int r = recv_msg(&m, sent_pub ? 1000 : 6000);
        if (r == -3) { err("adb: interrupted"); return -1; }
        if (r == -1) { err("adb: the device went away"); return -1; }
        if (r == -2) {
            if (sent_pub && timer_ms() - t0 < 120000) continue;
            err(sent_pub ? "adb: device unauthorized - allow USB debugging on the phone, then try again"
                         : "adb: the phone does not answer");
            return -1;
        }
        if (m.cmd == A_CNXN) {
            g_maxdata = m.arg1 > A_MAXDATA ? A_MAXDATA : m.arg1 < 256 ? 256 : m.arg1;
            uint32_t n = m.len < sizeof(g_banner) - 1 ? m.len : sizeof(g_banner) - 1;
            memcpy(g_banner, g_pay, n);
            g_banner[n] = 0;
            return 0;
        }
        if (m.cmd == A_AUTH && m.arg0 == AUTH_TOKEN) {
            if (!sent_sig && m.len == 20) {
                static uint8_t sig[RSA_BYTES];
                rsa_sign_sha1(&g_key, g_pay, sig);
                send_msg(A_AUTH, AUTH_SIG, 0, sig, sizeof(sig));
                sent_sig = 1;
            } else if (!sent_pub) {
                static uint8_t pub[RSA_ANDROID_PUB_LEN];
                static char text[760];
                rsa_android_pubkey(&g_key, pub);
                int n = b64(pub, sizeof(pub), text, sizeof(text));
                kstrlcpy(text + n, " banana@banana-os", sizeof(text) - (size_t)n);
                send_msg(A_AUTH, AUTH_PUB, 0, text, (uint32_t)strlen(text) + 1);
                sent_pub = 1;
                t0 = timer_ms();
                say("adb: allow USB debugging on the phone (this computer's key is shown there)...\n");
            }
        }
    }
}

/* the phone's description: "device::ro.product.name=x;ro.product.model=y;..." */
static void banner_prop(const char* key, char* out, int cap) {
    out[0] = 0;
    const char* p = strstr(g_banner, key);
    if (!p) return;
    p += strlen(key);
    int n = 0;
    while (*p && *p != ';' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
}

/* ══ adb: streams ══════════════════════════════════════════════════ */

#define INBUF (A_MAXDATA * 2)

typedef struct {
    uint32_t local, remote;
    int      ready;                          /* the phone took our last WRTE */
    int      closed;
    uint8_t* in;                             /* what the phone wrote, not taken yet */
    uint32_t inlen;
} stream_t;

/* one message from the phone, for stream s: 0, or as read_full */
static int pump(stream_t* s, uint32_t timeout_ms) {
    amsg_t m;
    int r = recv_msg(&m, timeout_ms);
    if (r) return r;
    if (m.arg1 != s->local) return 0;        /* (another stream's) */
    if (m.cmd == A_WRTE) {
        if (s->inlen + m.len <= INBUF) {
            memcpy(s->in + s->inlen, g_pay, m.len);
            s->inlen += m.len;
        }
        send_msg(A_OKAY, s->local, s->remote, NULL, 0);
    } else if (m.cmd == A_OKAY) s->ready = 1;
    else if (m.cmd == A_CLSE) s->closed = 1;
    return 0;
}

static int stream_open(stream_t* s, const char* service) {
    memset(s, 0, sizeof(*s));
    s->in = (uint8_t*)kmalloc(INBUF);
    if (!s->in) return -1;
    s->local = ++g_next_local;
    if (send_msg(A_OPEN, s->local, 0, service, (uint32_t)strlen(service) + 1) < 0) return -1;
    uint32_t t0 = timer_ms();
    for (;;) {
        amsg_t m;
        int r = recv_msg(&m, 10000);
        if (r) return -1;
        if (m.arg1 != s->local) continue;
        if (m.cmd == A_OKAY) { s->remote = m.arg0; s->ready = 1; return 0; }
        if (m.cmd == A_CLSE) return -1;
        if (timer_ms() - t0 > 10000) return -1;
    }
}

static void stream_close(stream_t* s) {
    if (!s->closed && s->remote) send_msg(A_CLSE, s->local, s->remote, NULL, 0);
    s->closed = 1;
    if (s->in) kfree(s->in);
    s->in = NULL;
}

/* writes len bytes in WRTEs the phone acknowledges one by one: 0 / -1 */
static int stream_write(stream_t* s, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len) {
        while (!s->ready) {
            if (s->closed) return -1;
            if (pump(s, 30000)) return -1;
        }
        uint32_t c = len > g_maxdata ? g_maxdata : len;
        s->ready = 0;
        if (send_msg(A_WRTE, s->local, s->remote, p, c) < 0) return -1;
        p += c;
        len -= c;
    }
    return 0;
}

/* exactly n bytes of what the phone writes: 0, -1 (closed / gone / Ctrl+C) */
static int stream_read(stream_t* s, void* buf, uint32_t n) {
    while (s->inlen < n) {
        if (s->closed) return -1;
        if (pump(s, 30000)) return -1;
    }
    memcpy(buf, s->in, n);
    memmove(s->in, s->in + n, s->inlen - n);
    s->inlen -= n;
    return 0;
}

/* ══ adb: commands ═════════════════════════════════════════════════ */

/* a service whose output is shown until the phone closes it (or Ctrl+C) */
static int run_service(const char* service) {
    stream_t s;
    if (stream_open(&s, service) < 0) { err("adb: the phone refused '%s'", service); stream_close(&s); return -1; }
    while (!s.closed) {
        if (s.inlen) { out_bytes(s.in, s.inlen); s.inlen = 0; }
        int r = pump(&s, 200);
        if (r == -3 || r == -1) break;
    }
    if (s.inlen) out_bytes(s.in, s.inlen);
    stream_close(&s);
    return 0;
}

/* the phone's shell, typed into: until it ends ("exit") */
static int interactive_shell(void) {
    stream_t s;
    if (stream_open(&s, "shell:") < 0) { err("adb: the phone refused a shell"); stream_close(&s); return -1; }
    int vt = terminal_vt_get_active();
    while (!s.closed) {
        if (s.inlen) { out_bytes(s.in, s.inlen); s.inlen = 0; }
        char keys[32];
        int nk = 0;
        char c;
        while (nk < (int)sizeof(keys) && gui_focused_vt() == vt && (c = keyboard_try_getchar()) != 0) {
            if (c == '\n') c = '\r';
            else if (c == '\b') c = 0x7F;
            keys[nk++] = c;
        }
        if (nk && stream_write(&s, keys, (uint32_t)nk) < 0) break;
        int r = pump(&s, 20);
        if (r == -1) break;
    }
    if (s.inlen) out_bytes(s.in, s.inlen);
    stream_close(&s);
    terminal_putchar('\n');
    return 0;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int sync_reply(stream_t* s, const char* what) {
    uint8_t h[8];
    if (stream_read(s, h, 8) < 0) { err("adb: %s: no answer", what); return -1; }
    if (!memcmp(h, "OKAY", 4)) return 0;
    if (!memcmp(h, "FAIL", 4)) {
        uint32_t n = get32(h + 4);
        char msg[256];
        if (n > sizeof(msg) - 1) n = sizeof(msg) - 1;
        stream_read(s, msg, n);
        msg[n] = 0;
        err("adb: %s failed: %s", what, msg);
        return -1;
    }
    err("adb: %s: unexpected answer", what);
    return -1;
}

#define SYNC_DATA 65536u

static int do_push(const char* local, const char* remote_in) {
    uint32_t size;
    int idx = file_size(local, &size);
    if (idx < 0) { err("adb: cannot stat '%s': No such file", local); return -1; }
    char remote[512];
    int rl = (int)strlen(remote_in);
    if (rl && remote_in[rl - 1] == '/') ksnprintf(remote, sizeof(remote), "%s%s", remote_in, base_name(local));
    else kstrlcpy(remote, remote_in, sizeof(remote));
    stream_t s;
    if (stream_open(&s, "sync:") < 0) { err("adb: the phone refused sync"); stream_close(&s); return -1; }
    uint32_t t0 = timer_ms();
    char spec[560];
    int sl = ksnprintf(spec, sizeof(spec), "%s,%u", remote, 0100644u);
    uint8_t* buf = (uint8_t*)kmalloc(SYNC_DATA + 8);
    int rc = -1;
    if (!buf) goto done;
    memcpy(buf, "SEND", 4);
    put32(buf + 4, (uint32_t)sl);
    memcpy(buf + 8, spec, (uint32_t)sl);
    if (stream_write(&s, buf, 8 + (uint32_t)sl) < 0) goto done;
    for (uint32_t off = 0; off < size; ) {
        uint32_t c = size - off > SYNC_DATA ? SYNC_DATA : size - off;
        if (fs_read(idx, off, buf + 8, c) != (int)c) { err("adb: cannot read '%s'", local); goto done; }
        memcpy(buf, "DATA", 4);
        put32(buf + 4, c);
        if (stream_write(&s, buf, 8 + c) < 0) { err("adb: the transfer stopped"); goto done; }
        off += c;
    }
    memcpy(buf, "DONE", 4);
    put32(buf + 4, rtc_now());
    if (stream_write(&s, buf, 8) < 0) goto done;
    if (sync_reply(&s, "push") < 0) goto done;
    {
        uint32_t ms = timer_ms() - t0;
        say("%s: 1 file pushed. %u bytes in %u.%03us\n", local, size, ms / 1000, ms % 1000);
    }
    memcpy(buf, "QUIT", 4);
    put32(buf + 4, 0);
    stream_write(&s, buf, 8);
    rc = 0;
done:
    if (buf) kfree(buf);
    stream_close(&s);
    return rc;
}

static int do_pull(const char* remote, const char* local_in) {
    char local[FS_PATH_LEN];
    if (!local_in || !local_in[0] || !strcmp(local_in, ".")) kstrlcpy(local, base_name(remote), sizeof(local));
    else if (fs_find_dir(local_in) >= 0) ksnprintf(local, sizeof(local), "%s/%s", local_in, base_name(remote));
    else kstrlcpy(local, local_in, sizeof(local));
    stream_t s;
    if (stream_open(&s, "sync:") < 0) { err("adb: the phone refused sync"); stream_close(&s); return -1; }
    uint32_t t0 = timer_ms();
    uint8_t h[8];
    int rl = (int)strlen(remote);
    uint8_t* buf = (uint8_t*)kmalloc(SYNC_DATA + 8);
    uint8_t* data = NULL;
    uint32_t have = 0, cap = 0;
    int rc = -1;
    if (!buf) goto done;
    memcpy(buf, "RECV", 4);
    put32(buf + 4, (uint32_t)rl);
    memcpy(buf + 8, remote, (uint32_t)rl);
    if (stream_write(&s, buf, 8 + (uint32_t)rl) < 0) goto done;
    for (;;) {
        if (stream_read(&s, h, 8) < 0) { err("adb: the transfer stopped"); goto done; }
        uint32_t n = get32(h + 4);
        if (!memcmp(h, "DONE", 4)) break;
        if (!memcmp(h, "FAIL", 4)) {
            char msg[256];
            if (n > sizeof(msg) - 1) n = sizeof(msg) - 1;
            stream_read(&s, msg, n);
            msg[n] = 0;
            err("adb: error: failed to copy '%s' to '%s': remote %s", remote, local, msg);
            goto done;
        }
        if (memcmp(h, "DATA", 4) || n > SYNC_DATA) { err("adb: pull: unexpected answer"); goto done; }
        if (have + n > cap) {
            uint32_t nc = cap ? cap * 2 : 65536;
            while (nc < have + n) nc *= 2;
            uint8_t* nd = (uint8_t*)kmalloc(nc);
            if (!nd) { err("adb: not enough memory for '%s'", remote); goto done; }
            if (data) { memcpy(nd, data, have); kfree(data); }
            data = nd;
            cap = nc;
        }
        if (stream_read(&s, data + have, n) < 0) { err("adb: the transfer stopped"); goto done; }
        have += n;
    }
    if (fs_write_path(local, data ? (const void*)data : "", have) < 0) { err("adb: cannot write '%s'", local); goto done; }
    {
        uint32_t ms = timer_ms() - t0;
        say("%s: 1 file pulled. %u bytes in %u.%03us\n", remote, have, ms / 1000, ms % 1000);
    }
    memcpy(buf, "QUIT", 4);
    put32(buf + 4, 0);
    stream_write(&s, buf, 8);
    rc = 0;
done:
    if (buf) kfree(buf);
    if (data) kfree(data);
    stream_close(&s);
    return rc;
}

/* argv[from..] joined with spaces, words with spaces quoted for the phone's shell */
static void join_args(int argc, char** argv, int from, char* out, int cap) {
    int o = 0;
    out[0] = 0;
    for (int i = from; i < argc && o < cap - 4; i++) {
        int q = strchr(argv[i], ' ') != NULL;
        o += ksnprintf(out + o, (size_t)(cap - o), q ? "%s'%s'" : "%s%s", i > from ? " " : "", argv[i]);
    }
}

static void adb_usage(void) {
    say("adb - Android Debug Bridge (phones over USB, USB debugging on)\n"
        "  adb devices [-l]              the phones plugged in\n"
        "  adb [-s SERIAL] shell [CMD]   a shell on the phone (or run CMD)\n"
        "  adb push LOCAL REMOTE         copy a file to the phone\n"
        "  adb pull REMOTE [LOCAL]       copy a file from the phone\n"
        "  adb install FILE.apk          install an app\n"
        "  adb uninstall PACKAGE         remove one\n"
        "  adb logcat                    the phone's log (Ctrl+C stops)\n"
        "  adb reboot [bootloader|recovery|sideload]\n"
        "  adb get-state | get-serialno | version\n");
}

void cmd_adb(int argc, char** argv) {
    const char* serial = NULL;
    int a = 1;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-s") && a + 1 < argc) { serial = argv[a + 1]; a += 2; }
        else if (!strcmp(argv[a], "-d")) a++;
        else break;
    }
    if (a >= argc || !strcmp(argv[a], "help") || !strcmp(argv[a], "--help")) { adb_usage(); return; }
    const char* c = argv[a];

    if (!strcmp(c, "version")) { say("Android Debug Bridge version 1.0.41 (Banana OS)\n"); return; }
    if (!strcmp(c, "start-server") || !strcmp(c, "kill-server")) return;   /* (no server here) */
    if (!strcmp(c, "devices")) {
        int longf = a + 1 < argc && !strcmp(argv[a + 1], "-l");
        adbdev_info_t d[ADBDEV_MAX];
        int n = adbdev_list(d, ADBDEV_MAX);
        say("List of devices attached\n");
        for (int i = 0; i < n; i++) {
            if (d[i].kind != ADBDEV_ADB) continue;
            if (!longf) { say("%s\tdevice\n", d[i].serial); continue; }
            if (adb_connect(d[i].serial) < 0) { say("%s\tunauthorized\n", d[i].serial); continue; }
            char prod[64], model[64], dev[64];
            banner_prop("ro.product.name=", prod, sizeof(prod));
            banner_prop("ro.product.model=", model, sizeof(model));
            banner_prop("ro.product.device=", dev, sizeof(dev));
            say("%s\tdevice usb:%04x:%04x product:%s model:%s device:%s\n", d[i].serial, d[i].vid, d[i].pid, prod, model, dev);
        }
        say("\n");
        return;
    }
    if (!strcmp(c, "get-serialno") || !strcmp(c, "get-state")) {
        if (pick(serial, 0) < 0) return;
        adbdev_info_t d[ADBDEV_MAX];
        int n = adbdev_list(d, ADBDEV_MAX);
        for (int i = 0; i < n; i++)
            if (d[i].kind == ADBDEV_ADB && (!serial || !strcmp(serial, d[i].serial))) {
                say("%s\n", c[4] == 's' && c[5] == 'e' ? d[i].serial : "device");
                break;
            }
        return;
    }

    if (adb_connect(serial) < 0) return;
    static char cmd[1024];
    if (!strcmp(c, "shell")) {
        if (a + 1 >= argc) { interactive_shell(); return; }
        static char svc[1100];
        join_args(argc, argv, a + 1, cmd, sizeof(cmd));
        ksnprintf(svc, sizeof(svc), "shell:%s", cmd);
        run_service(svc);
    } else if (!strcmp(c, "logcat")) {
        join_args(argc, argv, a, cmd, sizeof(cmd));
        static char svc[1100];
        ksnprintf(svc, sizeof(svc), "shell:%s", cmd);
        run_service(svc);
    } else if (!strcmp(c, "push")) {
        if (a + 2 >= argc) { err("adb: usage: adb push LOCAL REMOTE"); return; }
        for (int i = a + 1; i < argc - 1; i++) do_push(argv[i], argv[argc - 1]);
    } else if (!strcmp(c, "pull")) {
        if (a + 1 >= argc) { err("adb: usage: adb pull REMOTE [LOCAL]"); return; }
        do_pull(argv[a + 1], a + 2 < argc ? argv[a + 2] : NULL);
    } else if (!strcmp(c, "install")) {
        const char* apk = NULL;
        char flags[64] = "";
        for (int i = a + 1; i < argc; i++) {
            if (argv[i][0] == '-') { kstrlcpy(flags + strlen(flags), argv[i], sizeof(flags) - strlen(flags)); kstrlcpy(flags + strlen(flags), " ", sizeof(flags) - strlen(flags)); }
            else apk = argv[i];
        }
        if (!apk) { err("adb: usage: adb install [-r] FILE.apk"); return; }
        char tmp[300], svc[700];
        ksnprintf(tmp, sizeof(tmp), "/data/local/tmp/%s", base_name(apk));
        if (do_push(apk, tmp) < 0) return;
        say("Performing Push Install\n");
        ksnprintf(svc, sizeof(svc), "shell:pm install %s'%s'", flags, tmp);
        run_service(svc);
        ksnprintf(svc, sizeof(svc), "shell:rm -f '%s'", tmp);
        stream_t s;
        if (stream_open(&s, svc) == 0) while (!s.closed && pump(&s, 2000) == 0) s.inlen = 0;
        stream_close(&s);
    } else if (!strcmp(c, "uninstall")) {
        if (a + 1 >= argc) { err("adb: usage: adb uninstall PACKAGE"); return; }
        char svc[300];
        ksnprintf(svc, sizeof(svc), "shell:pm uninstall %s", argv[a + 1]);
        run_service(svc);
    } else if (!strcmp(c, "reboot")) {
        char svc[64];
        ksnprintf(svc, sizeof(svc), "reboot:%s", a + 1 < argc ? argv[a + 1] : "");
        stream_t s;
        if (stream_open(&s, svc) == 0) pump(&s, 2000);    /* (it restarts: the connection just ends) */
        stream_close(&s);
    } else if (!strcmp(c, "root") || !strcmp(c, "unroot") || !strcmp(c, "remount") || !strcmp(c, "disable-verity") || !strcmp(c, "enable-verity")) {
        char svc[32];
        ksnprintf(svc, sizeof(svc), "%s:", c);
        run_service(svc);
    } else if (!strcmp(c, "sideload")) {
        err("adb: sideload is not supported yet");
    } else {
        err("adb: unknown command %s", c);
        adb_usage();
    }
}

/* ══ fastboot ══════════════════════════════════════════════════════ */

static int g_fh = -1;

/* a command; its final answer's text in resp: 0 OKAY, 1 DATA (*len), -1 FAIL / error */
static int fb_command(const char* cmd, char* resp, int cap, uint32_t* len, uint32_t timeout_ms) {
    if (resp && cap) resp[0] = 0;
    if (adbdev_write(g_fh, cmd, (uint32_t)strlen(cmd)) < 0) { err("fastboot: the device does not answer"); return -1; }
    uint32_t t0 = timer_ms();
    for (;;) {
        char r[260];
        int n = adbdev_read(g_fh, r, 256, 100);
        if (n < 0) { err("fastboot: the device went away"); return -1; }
        if (n == 0) {
            if (timer_ms() - t0 > timeout_ms) { err("fastboot: no answer from the device"); return -1; }
            if (interrupted()) { err("fastboot: interrupted"); return -1; }
            continue;
        }
        r[n] = 0;
        t0 = timer_ms();
        if (n < 4) continue;
        if (!memcmp(r, "INFO", 4)) { say("(bootloader) %s\n", r + 4); continue; }
        if (!memcmp(r, "TEXT", 4)) { say("%s", r + 4); continue; }
        if (resp && cap) kstrlcpy(resp, r + 4, (size_t)cap);
        if (!memcmp(r, "OKAY", 4)) return 0;
        if (!memcmp(r, "DATA", 4)) {
            uint32_t v = 0;
            for (int i = 4; i < 12 && i < n; i++) {
                char ch = r[i];
                v = v * 16 + (uint32_t)(ch >= 'a' ? ch - 'a' + 10 : ch >= 'A' ? ch - 'A' + 10 : ch - '0');
            }
            if (len) *len = v;
            return 1;
        }
        if (!memcmp(r, "FAIL", 4)) return -1;
    }
}

static uint32_t parse_num(const char* s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; *s; s++) {
            char ch = *s;
            if (ch >= '0' && ch <= '9') v = v * 16 + (uint32_t)(ch - '0');
            else if ((ch | 32) >= 'a' && (ch | 32) <= 'f') v = v * 16 + (uint32_t)((ch | 32) - 'a' + 10);
            else break;
        }
    } else for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint32_t)(*s - '0');
    return v;
}

static void timed(const char* what, int rc, uint32_t t0, const char* resp) {
    uint32_t ms = timer_ms() - t0;
    if (rc >= 0) say("%-40s OKAY [%3u.%03us]\n", what, ms / 1000, ms % 1000);
    else {
        say("%-40s FAILED (remote: '%s')\n", what, resp);
    }
}

/* download: the file's bytes to the device's memory */
static int fb_download(const char* path, const char* label) {
    uint32_t size;
    int idx = file_size(path, &size);
    if (idx < 0) { err("fastboot: cannot load '%s': No such file", path); return -1; }
    char resp[128];
    uint32_t max = 0;
    if (fb_command("getvar:max-download-size", resp, sizeof(resp), NULL, 5000) == 0) max = parse_num(resp);
    if (max && size > max) {
        err("fastboot: '%s' is %u bytes, more than the device takes at once (%u) - sparse images are not supported yet", path, size, max);
        return -1;
    }
    char cmd[32], what[96];
    ksnprintf(cmd, sizeof(cmd), "download:%08x", size);
    ksnprintf(what, sizeof(what), "Sending '%s' (%u KB)", label, (size + 1023) / 1024);
    uint32_t t0 = timer_ms(), want = 0;
    int r = fb_command(cmd, resp, sizeof(resp), &want, 5000);
    if (r != 1 || want != size) { timed(what, -1, t0, resp); return -1; }
    uint8_t* buf = (uint8_t*)kmalloc(1u << 20);
    if (!buf) return -1;
    for (uint32_t off = 0; off < size; ) {
        uint32_t c = size - off > (1u << 20) ? (1u << 20) : size - off;
        if (fs_read(idx, off, buf, c) != (int)c || adbdev_write(g_fh, buf, c) < 0) { kfree(buf); timed(what, -1, t0, "transfer failed"); return -1; }
        off += c;
    }
    kfree(buf);
    r = fb_command("", resp, sizeof(resp), NULL, 30000);   /* (nothing sent: the answer to the data) */
    timed(what, r, t0, resp);
    return r == 0 ? 0 : -1;
}

/* a command whose answer is just OKAY / FAIL, shown like fastboot does */
static int fb_simple(const char* cmd, const char* what, uint32_t timeout_ms) {
    char resp[128];
    uint32_t t0 = timer_ms();
    int r = fb_command(cmd, resp, sizeof(resp), NULL, timeout_ms);
    timed(what, r, t0, resp);
    return r;
}

static void fastboot_usage(void) {
    say("fastboot - the bootloader of Android phones over USB\n"
        "  fastboot devices                    the phones in their bootloader\n"
        "  fastboot getvar NAME | all          a bootloader variable\n"
        "  fastboot flash PARTITION FILE       write an image\n"
        "  fastboot erase PARTITION\n"
        "  fastboot boot FILE                  start an image without flashing it\n"
        "  fastboot set_active a|b\n"
        "  fastboot flashing unlock|lock|get_unlock_ability\n"
        "  fastboot oem COMMAND...\n"
        "  fastboot reboot [bootloader|recovery|fastboot]\n"
        "  fastboot continue\n");
}

void cmd_fastboot(int argc, char** argv) {
    const char* serial = NULL;
    int a = 1;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-s") && a + 1 < argc) { serial = argv[a + 1]; a += 2; }
        else break;
    }
    if (a >= argc || !strcmp(argv[a], "help") || !strcmp(argv[a], "--help")) { fastboot_usage(); return; }
    const char* c = argv[a];
    if (!strcmp(c, "devices")) {
        adbdev_info_t d[ADBDEV_MAX];
        int n = adbdev_list(d, ADBDEV_MAX);
        for (int i = 0; i < n; i++) if (d[i].kind == ADBDEV_FASTBOOT) say("%s\tfastboot\n", d[i].serial);
        return;
    }
    g_fh = adbdev_open(ADBDEV_FASTBOOT, serial);
    if (g_fh < 0) {
        say("< waiting for %s >\n", serial ? serial : "any device");
        uint32_t t0 = timer_ms();
        while ((g_fh = adbdev_open(ADBDEV_FASTBOOT, serial)) < 0) {
            if (interrupted() || timer_ms() - t0 > 60000) { err("fastboot: no device in its bootloader (adb reboot bootloader?)"); return; }
            task_sleep_ms(200);
        }
    }
    adbdev_flush_input(g_fh);
    char resp[128], cmd[300], what[128];
    uint32_t t0 = timer_ms();
    if (!strcmp(c, "getvar")) {
        if (a + 1 >= argc) { err("fastboot: usage: fastboot getvar NAME"); return; }
        ksnprintf(cmd, sizeof(cmd), "getvar:%s", argv[a + 1]);
        int r = fb_command(cmd, resp, sizeof(resp), NULL, 10000);
        if (r == 0 && strcmp(argv[a + 1], "all")) say("%s: %s\n", argv[a + 1], resp);
        else if (r < 0) say("getvar:%s FAILED (remote: '%s')\n", argv[a + 1], resp);
        say("Finished. Total time: %u.%03us\n", (timer_ms() - t0) / 1000, (timer_ms() - t0) % 1000);
        return;
    }
    if (!strcmp(c, "flash")) {
        if (a + 2 >= argc) { err("fastboot: usage: fastboot flash PARTITION FILE"); return; }
        if (fb_download(argv[a + 2], argv[a + 1]) < 0) return;
        ksnprintf(cmd, sizeof(cmd), "flash:%s", argv[a + 1]);
        ksnprintf(what, sizeof(what), "Writing '%s'", argv[a + 1]);
        fb_simple(cmd, what, 600000);
    } else if (!strcmp(c, "erase")) {
        if (a + 1 >= argc) { err("fastboot: usage: fastboot erase PARTITION"); return; }
        ksnprintf(cmd, sizeof(cmd), "erase:%s", argv[a + 1]);
        ksnprintf(what, sizeof(what), "Erasing '%s'", argv[a + 1]);
        fb_simple(cmd, what, 600000);
    } else if (!strcmp(c, "boot")) {
        if (a + 1 >= argc) { err("fastboot: usage: fastboot boot FILE"); return; }
        if (fb_download(argv[a + 1], "boot.img") < 0) return;
        fb_simple("boot", "Booting", 30000);
    } else if (!strcmp(c, "set_active")) {
        if (a + 1 >= argc) { err("fastboot: usage: fastboot set_active a|b"); return; }
        ksnprintf(cmd, sizeof(cmd), "set_active:%s", argv[a + 1]);
        ksnprintf(what, sizeof(what), "Setting current slot to '%s'", argv[a + 1]);
        fb_simple(cmd, what, 30000);
    } else if (!strcmp(c, "flashing") || !strcmp(c, "oem")) {
        int o = ksnprintf(cmd, sizeof(cmd), "%s", c);
        for (int i = a + 1; i < argc; i++) o += ksnprintf(cmd + o, sizeof(cmd) - (size_t)o, " %s", argv[i]);
        fb_simple(cmd, " ", 120000);
    } else if (!strcmp(c, "reboot")) {
        const char* to = a + 1 < argc ? argv[a + 1] : "";
        if (!to[0]) fb_simple("reboot", "Rebooting", 10000);
        else {
            ksnprintf(cmd, sizeof(cmd), "reboot-%s", to);
            ksnprintf(what, sizeof(what), "Rebooting into %s", to);
            fb_simple(cmd, what, 10000);
        }
    } else if (!strcmp(c, "reboot-bootloader")) {
        fb_simple("reboot-bootloader", "Rebooting into bootloader", 10000);
    } else if (!strcmp(c, "continue")) {
        fb_simple("continue", "Resuming boot", 10000);
    } else {
        err("fastboot: unknown command %s", c);
        fastboot_usage();
        return;
    }
    say("Finished. Total time: %u.%03us\n", (timer_ms() - t0) / 1000, (timer_ms() - t0) % 1000);
}
