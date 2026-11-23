/* SPDX-License-Identifier: GPL-3.0-or-later */
/* usb/uaudio.c's recording and kernel/audioin.c against model USB audio
 * devices: a webcam's 16 kHz mono microphone (made 48 kHz: the sine it
 * hears must come out as the same sine), a headset with 48 kHz stereo both
 * ways (every sample kept, left and right apart), the rate chosen from the
 * ones a device offers, a reader that falls behind, and the computer's own
 * input coming back when the USB one is unplugged. A protocol model. */
#include "../../usb/usbcore.h"
#include "../../kernel/kstring.h"
#include "../../kernel/audio.h"
#include "../../kernel/audioin.h"
extern int printf(const char*, ...);
extern int fflush(void*);
extern void abort(void);
extern void* calloc(size_t, size_t);
extern void free(void*);
extern double sin(double);
extern int vsnprintf(char*, size_t, const char*, __builtin_va_list);
extern size_t strlen(const char*);

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fflush(0); abort(); } } while (0)

int ksnprintf(char* b, size_t n, const char* f, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    __builtin_va_end(ap);
    return r;
}
size_t kstrlcpy(char* d, const char* s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
void* kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void* kzalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void* p) { free(p); }
void* usb_dma_alloc(uint32_t n) { return calloc(1, n); }
void klog(const char* f, ...) { (void)f; }
void terminal_writeln(const char* s) { printf("    | %s\n", s); }
static uint32_t now;
uint32_t timer_ms(void) { return now; }
void usb_delay_ms(uint32_t n) { now += n; }

/* the playing side (audio.c) */
static uint8_t out_ring[65536];
static const audio_card_t* plugged_card;
uint8_t* audio_ring(void) { return out_ring; }
uint32_t audio_ring_bytes(void) { return sizeof(out_ring); }
void audio_card_plug(const audio_card_t* c, const char* name) { (void)name; plugged_card = c; }
void audio_card_unplug(const audio_card_t* c) { if (plugged_card == c) plugged_card = NULL; }

int usb_walk_config(usb_device_t* d, int c, int (*cb)(const uint8_t*, void*), void* arg) {
    const uint8_t* b = d->configs[c < 0 ? 0 : c];
    uint32_t n = b[2] | (uint32_t)b[3] << 8;
    for (uint32_t off = 0; off < n;) {
        if (n - off < 2 || b[off] < 2 || b[off] > n - off) return -1;
        if (cb(b + off, arg)) return 0;
        off += b[off];
    }
    return 0;
}

static int opened, alt_now[8], rate_set, rate_ep, unmutes;
int usb_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) { (void)d; (void)ep; opened++; return 0; }
int usb_control(usb_device_t* d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void* b, uint16_t n) {
    (void)d;
    if (type == USB_RECIP_IFACE && req == 0x0B) { CHECK(index < 8); alt_now[index] = value; return 0; }
    if (type == (USB_TYPE_CLASS | 0x02) && req == 0x01 && value == 0x0100) {
        CHECK(n == 3);
        uint8_t* r = (uint8_t*)b;
        rate_set = r[0] | r[1] << 8 | r[2] << 16;
        rate_ep = index;
        return n;
    }
    if (type == (USB_TYPE_CLASS | USB_RECIP_IFACE) && req == 0x01) { if (value == 0x0100) unmutes++; return n; }
    CHECK(0);
    return -1;
}

/* the "controller": queued isochronous packets, completed by the test */
#define QMAX 256
static usb_xfer_t* q[QMAX];
static unsigned qh, qt, streaming = 1;
int usb_submit(usb_xfer_t* x) {
    CHECK(x->status != USB_XFER_PENDING);
    CHECK(qt - qh < QMAX);
    x->status = USB_XFER_PENDING;
    q[qt++ % QMAX] = x;
    return 0;
}
static void hc_events(usb_hc_t* hc) {
    (void)hc;
    if (streaming) return;
    while (qh != qt) { usb_xfer_t* x = q[qh++ % QMAX]; x->actual = 0; x->status = USB_XFER_OK; }
}
static const usb_hc_ops_t hc_ops = { .events = hc_events };
static usb_hc_t hc = { .ops = &hc_ops };

#define AUDIOIN_HOST_TEST
#include "../../kernel/audioin.c"
#include "../../usb/uaudio.c"

/* ── descriptors ── */
static uint8_t cfg[512];
static uint32_t cl;
#define ADD(...) do { static const uint8_t t_[] = { __VA_ARGS__ }; memcpy(cfg + cl, t_, sizeof(t_)); cl += sizeof(t_); } while (0)
static void begin(void) { cl = 0; ADD(9, 2, 0, 0, 3, 1, 0, 0x80, 50); ADD(9, 4, 0, 0, 0, 1, 1, 0, 0); ADD(10, 0x24, 6, 2, 1, 1, 0x03, 0, 0, 0); }
static void end(void) { cfg[2] = (uint8_t)cl; cfg[3] = (uint8_t)(cl >> 8); }
/* an input stream: interface iface, setting 1; ch channels; the rates; endpoint ep of mps bytes */
static void as_in(uint8_t iface, uint8_t ch, const uint32_t* rates, int nr, uint8_t ep, uint16_t mps) {
    uint8_t d[9 + 9 + 7 + 8 + 3 * 8 + 9 + 7];
    uint32_t n = 0;
    uint8_t a0[9] = { 9, 4, iface, 0, 0, 1, 2, 0, 0 }, a1[9] = { 9, 4, iface, 1, 1, 1, 2, 0, 0 };
    memcpy(d + n, a0, 9); n += 9;
    memcpy(d + n, a1, 9); n += 9;
    uint8_t gen[7] = { 7, 0x24, 1, 3, 1, 1, 0 };
    memcpy(d + n, gen, 7); n += 7;
    uint8_t* f = d + n;
    f[0] = (uint8_t)(8 + 3 * nr); f[1] = 0x24; f[2] = 2; f[3] = 1; f[4] = ch; f[5] = 2; f[6] = 16; f[7] = (uint8_t)nr;
    for (int i = 0; i < nr; i++) { f[8 + i * 3] = (uint8_t)rates[i]; f[9 + i * 3] = (uint8_t)(rates[i] >> 8); f[10 + i * 3] = (uint8_t)(rates[i] >> 16); }
    n += f[0];
    uint8_t e[9] = { 9, 5, ep, 0x05, (uint8_t)mps, (uint8_t)(mps >> 8), 1, 0, 0 };
    memcpy(d + n, e, 9); n += 9;
    uint8_t g[7] = { 7, 0x25, 1, 1, 0, 0, 0 };              /* EP_GENERAL: sampling frequency control */
    memcpy(d + n, g, 7); n += 7;
    memcpy(cfg + cl, d, n); cl += n;
}
static void as_out48(uint8_t iface) {
    uint8_t a0[9] = { 9, 4, iface, 0, 0, 1, 2, 0, 0 }, a1[9] = { 9, 4, iface, 1, 1, 1, 2, 0, 0 };
    memcpy(cfg + cl, a0, 9); cl += 9;
    memcpy(cfg + cl, a1, 9); cl += 9;
    ADD(7, 0x24, 1, 1, 1, 1, 0);
    ADD(11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xBB, 0);
    ADD(9, 5, 0x01, 0x05, 200, 0, 1, 0, 0);
    ADD(7, 0x25, 1, 1, 0, 0, 0);
}

static usb_device_t dev;
static void plug(const char* name) {
    end();
    memset(&dev, 0, sizeof(dev));
    dev.used = dev.present = 1;
    dev.hc = &hc;
    dev.speed = USB_SPEED_FULL;
    dev.configs[0] = cfg;
    kstrlcpy(dev.product, name, sizeof(dev.product));
    qh = qt = 0;
    streaming = 1;
    CHECK(ua_probe(&dev) == 0);
    CHECK(ua_attach(&dev) == 0);
}

/* one packet of frames from gen(i) */
static uint64_t sent_frames;
static void packet(int ch, int frames, int16_t (*gen)(uint64_t, int)) {
    CHECK(qt != qh);
    usb_xfer_t* x = q[qh++ % QMAX];
    CHECK((uint32_t)(frames * ch * 2) <= x->len);
    uint8_t* p = x->buf;
    for (int i = 0; i < frames; i++, sent_frames++)
        for (int c = 0; c < ch; c++) {
            int16_t v = gen(sent_frames, c);
            *p++ = (uint8_t)v; *p++ = (uint8_t)((uint16_t)v >> 8);
        }
    x->actual = (uint32_t)(frames * ch * 2);
    x->status = USB_XFER_OK;
}
static void ticks(int n) { for (int i = 0; i < n; i++) { now++; audio_in_tick(); } }

static int16_t sine16k(uint64_t i, int c) { (void)c; return (int16_t)(12000.0 * sin(2 * 3.14159265358979 * 440.0 * (double)i / 16000.0)); }
static int16_t lr(uint64_t i, int c) { return (int16_t)(c ? -(int)(i % 30000) : (int)(i % 30000)); }

static void builtin_start_stop(void) { }
static int builtin_start(void) { return 0; }
static uint32_t builtin_pos(void) { return 0; }
static const audio_in_t g_builtin_src = { "built-in", builtin_start, builtin_pos, NULL, builtin_start_stop };

static void webcam_mic(void) {
    audio_in_register(&g_builtin_src, "HD Audio, mic");          /* the computer's own, at boot */
    CHECK(!strcmp(audio_in_name(), "HD Audio, mic"));
    begin();
    static const uint32_t r16[] = { 16000 };
    as_in(1, 1, r16, 1, 0x84, 32);
    plug("Model Webcam");
    CHECK(!plugged_card);                                       /* no speakers: playing stays where it was */
    CHECK(!strcmp(audio_in_name(), "USB microphone: Model Webcam"));
    CHECK(g_uin->rate == 16000 && g_uin->ch == 1 && g_uin->len == 32 && unmutes == 1);
    uint64_t cur;
    CHECK(audio_in_open(&cur) == 0);
    CHECK(alt_now[1] == 1 && rate_set == 16000 && rate_ep == 0x84 && opened == 1);
    CHECK(qt - qh == NIN);
    /* one second: 1000 packets of 16 frames (1 ms each), read now and then */
    static int16_t got[60000];
    int n = 0;
    for (int ms = 0; ms < 1000; ms++) {
        packet(1, 16, sine16k);
        ticks(1);
        if (ms % 20 == 19) { int r = audio_in_read(&cur, got + n, 60000 - (uint32_t)n, 1); CHECK(r >= 0); n += r; }
    }
    ticks(2);
    int r = audio_in_read(&cur, got + n, 60000 - (uint32_t)n, 1);
    CHECK(r >= 0);
    n += r;
    CHECK(n >= 47997 && n <= 48000);                            /* 16000 frames made 48000, exactly */
    /* the same sine, one input frame later (the interpolation's delay) */
    double worst = 0;
    for (int k = 48; k < n; k++) {
        double want = 12000.0 * sin(2 * 3.14159265358979 * 440.0 * ((double)k / 48000.0 - 1.0 / 16000.0));
        double e = got[k] - want;
        if (e < 0) e = -e;
        if (e > worst) worst = e;
    }
    CHECK(worst < 120);                                         /* (1% of the amplitude) */
    CHECK(audio_in_level() > 11000 && audio_in_level() <= 12000);
    audio_in_list();
    audio_in_close();
    /* opened again: the packets given back first, none queued twice */
    streaming = 0;
    CHECK(audio_in_open(&cur) == 0);
    streaming = 1;
    CHECK(qt - qh == NIN);
    /* a reader that does not read for 2 s: it skips to what is still there */
    for (int ms = 0; ms < 2000; ms++) { packet(1, 16, sine16k); ticks(1); }
    uint32_t before = g_overruns;
    r = audio_in_read(&cur, got, 60000, 1);
    CHECK(r > 0 && g_overruns == before + 1);
    CHECK((uint64_t)r * 4 <= audio_in_ring_bytes() - 32768);
    audio_in_close();
    /* unplugged: the computer's own input again */
    dev.present = 0;
    ua_detach(&dev);
    CHECK(!strcmp(audio_in_name(), "HD Audio, mic") && !g_uin);
}

static void headset(void) {
    begin();
    as_out48(1);
    static const uint32_t rates[] = { 44100, 48000 };
    as_in(2, 2, rates, 2, 0x82, 196);
    plug("Model Headset");
    CHECK(plugged_card != NULL && g_ua != NULL);                /* the speakers play */
    CHECK(g_uin && g_uin->rate == 48000 && g_uin->ch == 2);
    uint64_t cur;
    CHECK(audio_in_open(&cur) == 0 && alt_now[2] == 1 && rate_set == 48000 && rate_ep == 0x82);
    static int16_t got[2 * 50000];
    int n = 0;
    sent_frames = 0;
    for (int ms = 0; ms < 500; ms++) {
        packet(2, 48, lr);
        ticks(1);
        int r = audio_in_read(&cur, got + 2 * n, 50000 - (uint32_t)n, 2);
        CHECK(r >= 0);
        n += r;
    }
    CHECK(n >= 23990 && n <= 24000);
    /* at 48 kHz nothing is made up: each frame is the one sent before it */
    for (int k = 1; k < n; k++) {
        CHECK(got[2 * k] == lr((uint64_t)k - 1, 0));
        CHECK(got[2 * k + 1] == lr((uint64_t)k - 1, 1));
    }
    audio_in_close();
    dev.present = 0;
    ua_detach(&dev);
    CHECK(!plugged_card && !g_ua && !g_uin);
}

static void rates(void) {
    uint8_t d[8 + 3 * 4] = { 0, 0x24, 2, 1, 1, 2, 16, 0 };
    struct { int n; uint32_t r[4]; uint32_t want; } t[] = {
        { 2, { 8000, 16000 }, 16000 }, { 3, { 16000, 44100, 48000 }, 48000 }, { 1, { 96000 }, 96000 },
        { 3, { 96000, 32000, 192000 }, 32000 }, { 2, { 44100, 22050 }, 44100 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        d[0] = (uint8_t)(8 + 3 * t[i].n);
        d[7] = (uint8_t)t[i].n;
        for (int k = 0; k < t[i].n; k++) { d[8 + k * 3] = (uint8_t)t[i].r[k]; d[9 + k * 3] = (uint8_t)(t[i].r[k] >> 8); d[10 + k * 3] = (uint8_t)(t[i].r[k] >> 16); }
        CHECK(pick_rate(d) == t[i].want);
    }
    uint8_t range[14] = { 14, 0x24, 2, 1, 1, 2, 16, 0, 0x40, 0x1F, 0, 0x00, 0x77, 0x01 };   /* 8000 - 96000 */
    CHECK(pick_rate(range) == 48000);
    range[11] = 0x80; range[12] = 0x3E; range[13] = 0;                                   /* 8000 - 16000 */
    CHECK(pick_rate(range) == 16000);
    /* 96 kHz made 48 kHz: half the frames */
    begin();
    static const uint32_t r96[] = { 96000 };
    as_in(1, 1, r96, 1, 0x81, 192);
    plug("Model Mic 96k");
    uint64_t cur;
    CHECK(audio_in_open(&cur) == 0);
    static int16_t got[30000];
    int n = 0;
    for (int ms = 0; ms < 500; ms++) {
        packet(1, 96, sine16k);
        ticks(1);
        int r = audio_in_read(&cur, got + n, 30000 - (uint32_t)n, 1);
        n += r;
    }
    CHECK(n >= 23990 && n <= 24000);
    audio_in_close();
    dev.present = 0;
    ua_detach(&dev);
}

int main(void) {
    webcam_mic();
    headset();
    rates();
    printf("uaudio_in: %u checks passed\n", checks);
    return 0;
}
