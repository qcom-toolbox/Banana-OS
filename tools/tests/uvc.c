/* SPDX-License-Identifier: GPL-3.0-or-later */
/* usb/uvc.c against a model webcam: its descriptors (YUY2 1280x720 /
 * 640x480 / 320x240, MJPEG 640x480, two isochronous settings of 1024 and
 * 3 x 1024 bytes, a microphone's audio interfaces), the UVC 1.1 probe /
 * commit exchange, and isochronous packets carrying pictures - headers,
 * frame ID toggles, end-of-frame and error bits, empty packets. A YUY2
 * picture must come out as the right 0x00RRGGBB pixels; an MJPEG one (a
 * real JPEG with its Huffman tables taken out, as cameras send it) must
 * decode exactly as the original. This is a protocol model, not a camera. */
#include "../../usb/usbcore.h"
#include "../../kernel/kstring.h"
extern int printf(const char*, ...);
extern void abort(void);
extern int fflush(void*);
extern void* calloc(size_t, size_t);
extern void free(void*);
extern int host_read_file(const char* path, unsigned char** data, unsigned int* len);
extern unsigned char* host_jpeg_decode(const unsigned char* data, int len, int* w, int* h);
extern void host_free(void* p);

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fflush(0); abort(); } } while (0)

extern int vsnprintf(char*, size_t, const char*, __builtin_va_list);
extern size_t strlen(const char*);
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
void usb_delay_ms(uint32_t n) { (void)n; }

static int binds, unbinds;
int uaudio_bind(usb_device_t* d) { (void)d; binds++; return 0; }
void uaudio_unbind(usb_device_t* d) { (void)d; unbinds++; }

int usb_walk_config(usb_device_t* d, int c, int (*cb)(const uint8_t*, void*), void* arg) {
    const uint8_t* b = d->configs[c < 0 ? 0 : c];
    if (!b) return -1;
    uint32_t n = b[2] | (uint32_t)b[3] << 8;
    for (uint32_t off = 0; off < n;) {
        if (n - off < 2 || b[off] < 2 || b[off] > n - off) return -1;
        if (cb(b + off, arg)) return 0;
        off += b[off];
    }
    return 0;
}

static int opened;
static usb_ep_desc_t opened_ep;
int usb_open_endpoint(usb_device_t* d, const usb_ep_desc_t* ep) { (void)d; opened++; opened_ep = *ep; return 0; }

/* ── the model camera's controls ── */
static int alt_now = -1, committed_fmt, committed_frame, probes, yuy2_too_big;
static uint8_t probe[48];
static int first_probe_frame, first_probe_fmt;

static uint32_t payload_for(int fmt, int frame) {
    if (fmt == 1) {
        if (yuy2_too_big) return 5000;
        return frame == 1 ? 9000 : frame == 2 ? 3000 : 800;
    }
    return 1000;                                     /* MJPEG */
}

int usb_control(usb_device_t* d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void* b, uint16_t n) {
    (void)d;
    if (type == USB_RECIP_IFACE && req == 0x0B) { CHECK(index == 1); alt_now = value; return 0; }
    CHECK(index == 1);                               /* the streaming interface */
    if (type == (USB_TYPE_CLASS | USB_RECIP_IFACE) && req == 0x01) {
        CHECK(n == 34);                              /* UVC 1.1: the 34-byte probe */
        if (value == 0x0100) {
            memcpy(probe, b, n);
            if (!probes++) { first_probe_fmt = probe[2]; first_probe_frame = probe[3]; }
            uint32_t p = payload_for(probe[2], probe[3]);
            probe[22] = (uint8_t)p; probe[23] = (uint8_t)(p >> 8); probe[24] = (uint8_t)(p >> 16); probe[25] = 0;
            return n;
        }
        if (value == 0x0200) { committed_fmt = ((uint8_t*)b)[2]; committed_frame = ((uint8_t*)b)[3]; return n; }
    }
    if (type == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_IFACE) && req == 0x81 && value == 0x0100) {
        CHECK(n == 34);
        memcpy(b, probe, n);
        return n;
    }
    CHECK(0);
    return -1;
}

/* ── isochronous packets: the queue at the "controller" ── */
#define QMAX 512
static usb_xfer_t* q[QMAX];
static unsigned qh, qt;
int usb_submit(usb_xfer_t* x) {
    CHECK(x->status != USB_XFER_PENDING);            /* never queued twice */
    CHECK(qt - qh < QMAX);
    x->status = USB_XFER_PENDING;
    q[qt++ % QMAX] = x;
    return 0;
}
static int alt_now_seen(void);
/* an xHCI gives back every queued isochronous TD each interval: with the
 * camera not streaming (setting 0), empty */
static void hc_events(usb_hc_t* hc) {
    (void)hc;
    if (alt_now_seen() != 0) return;
    while (qh != qt) { usb_xfer_t* x = q[qh++ % QMAX]; x->actual = 0; x->status = USB_XFER_OK; }
}
static void hc_poll(usb_hc_t* hc) { (void)hc; }
static const usb_hc_ops_t hc_ops = { .poll = hc_poll, .events = hc_events };
static usb_hc_t hc = { .ops = &hc_ops };

#include "../../usb/uvc.c"
static int alt_now_seen(void) { return alt_now; }

/* one packet in: a 12-byte header (flags), then data */
static int delivered;
static void packet(const uint8_t* data, uint32_t n, uint8_t flags, int empty) {
    CHECK(qt != qh);
    usb_xfer_t* x = q[qh++ % QMAX];
    CHECK(x->len == g_cam->td_len);
    if (empty) x->actual = 0;
    else {
        CHECK(12 + n <= x->len);
        x->buf[0] = 12;
        x->buf[1] = (uint8_t)(flags | 0x80 | 0x04);  /* EOH, PTS */
        memset(x->buf + 2, 0xAB, 10);
        memcpy(x->buf + 12, data, n);
        x->actual = 12 + n;
    }
    x->status = USB_XFER_OK;
    if (++delivered % 24 == 0) uvc_tick();           /* (the timer every ~3 ms of packets) */
}

/* a whole picture, split the way a camera does: payloads of up to max bytes */
static void picture(const uint8_t* data, uint32_t len, int fid, int eof, int err, uint32_t max) {
    for (uint32_t off = 0; off < len;) {
        uint32_t n = len - off < max ? len - off : max;
        int last = off + n == len;
        packet(data + off, n, (uint8_t)(fid | (last && eof ? 0x02 : 0) | (err ? 0x40 : 0)), 0);
        if (off % (max * 7) == 0) packet(NULL, 0, (uint8_t)fid, 1);   /* empty ones between */
        off += n;
    }
    uvc_tick();
}

/* ── descriptors ── */
static uint8_t cfg[1024];
static uint32_t cl;
static void add(const uint8_t* b, uint32_t n) { memcpy(cfg + cl, b, n); cl += n; }
#define ADD(...) do { static const uint8_t t_[] = { __VA_ARGS__ }; add(t_, sizeof(t_)); } while (0)
static void frame_desc(uint8_t sub, uint8_t idx, uint16_t w, uint16_t h) {
    uint8_t f[30] = { 30, 0x24, sub, idx, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8) };
    uint32_t buf = (uint32_t)w * h * 2, iv = 333333;
    f[17] = (uint8_t)buf; f[18] = (uint8_t)(buf >> 8); f[19] = (uint8_t)(buf >> 16);
    f[21] = (uint8_t)iv; f[22] = (uint8_t)(iv >> 8); f[23] = (uint8_t)(iv >> 16);
    f[25] = 1; f[26] = (uint8_t)iv; f[27] = (uint8_t)(iv >> 8); f[28] = (uint8_t)(iv >> 16);
    add(f, 30);
}
static void build_config(void) {
    cl = 0;
    ADD(9, 2, 0, 0, 4, 1, 0, 0x80, 0xFA);
    ADD(8, 0x0B, 0, 2, 0x0E, 3, 0, 0);                              /* IAD: video */
    ADD(9, 4, 0, 0, 1, 0x0E, 1, 0, 0);                              /* VideoControl */
    ADD(13, 0x24, 1, 0x10, 0x01, 0x4D, 0, 0x80, 0x8D, 0x5B, 0, 1, 1);  /* VC header: UVC 1.1 */
    ADD(7, 5, 0x83, 3, 16, 0, 8);
    ADD(9, 4, 1, 0, 0, 0x0E, 2, 0, 0);                              /* VideoStreaming, setting 0 */
    ADD(14, 0x24, 1, 2, 0, 0, 0x81, 0, 2, 0, 0, 0, 1, 0);           /* input header */
    ADD(27, 0x24, 4, 1, 3, 'Y', 'U', 'Y', '2', 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71, 16, 1, 0, 0, 0, 0);
    frame_desc(5, 1, 1280, 720);
    frame_desc(5, 2, 640, 480);
    frame_desc(5, 3, 320, 240);
    ADD(11, 0x24, 6, 2, 1, 1, 1, 0, 0, 0, 0);                       /* MJPEG */
    frame_desc(7, 1, 640, 480);
    ADD(9, 4, 1, 1, 1, 0x0E, 2, 0, 0);                              /* setting 1: 1024 bytes */
    ADD(7, 5, 0x81, 5, 0x00, 0x04, 1);
    ADD(9, 4, 1, 2, 1, 0x0E, 2, 0, 0);                              /* setting 2: 3 x 1024 */
    ADD(7, 5, 0x81, 5, 0x00, 0x14, 1);
    ADD(9, 4, 2, 0, 0, 1, 1, 0, 0);                                 /* the microphone's audio interfaces */
    ADD(9, 4, 3, 0, 0, 1, 2, 0, 0);
    cfg[2] = (uint8_t)cl; cfg[3] = (uint8_t)(cl >> 8);
}

static usb_device_t dev;
static void plug(void) {
    memset(&dev, 0, sizeof(dev));
    dev.used = dev.present = 1;
    dev.hc = &hc;
    dev.speed = USB_SPEED_HIGH;
    dev.configs[0] = cfg;
    dev.active_config = 0;
    kstrlcpy(dev.product, "Model Webcam", sizeof(dev.product));
    qh = qt = 0;
    probes = 0;
    CHECK(uvc_probe(&dev) == 0);
    CHECK(uvc_attach(&dev) == 0);
    dev.driver = &uvc_driver;
}

static int clampi(double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5); }

static void yuy2_test(void) {
    build_config();
    plug();
    CHECK(binds == 1 && camera_available());
    CHECK(alt_now == 0);                             /* not streaming until opened */
    int w = 0, h = 0;
    CHECK(camera_open(&w, &h) == 0);
    CHECK(w == 640 && h == 480);
    CHECK(first_probe_fmt == 1 && first_probe_frame == 2);   /* 1280x720 is never proposed */
    CHECK(committed_fmt == 1 && committed_frame == 2);
    CHECK(alt_now == 2 && opened == 1 && opened_ep.wMaxPacketSize == 0x1400);   /* 3000 bytes: the 3 x 1024 setting */
    CHECK(g_cam->td_len == 3072 && qt - qh == NV);

    /* a picture: a gradient of colours */
    uint32_t len = 640 * 480 * 2;
    uint8_t* yuv = calloc(1, len);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x += 2) {
            uint8_t* p = yuv + (y * 640 + x) * 2;
            p[0] = (uint8_t)(16 + (x * 219) / 640);
            p[1] = (uint8_t)(16 + (y * 224) / 480);
            p[2] = (uint8_t)(16 + ((x + 1) * 219) / 640);
            p[3] = (uint8_t)(240 - (y * 224) / 480);
        }
    uint32_t* rgb = calloc(4, 640 * 480);
    camera_frame_t fi;
    uint32_t seq = 0;
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == 0);   /* nothing yet */
    picture(yuv, len, 0, 1, 0, 3060);
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == 1);
    CHECK(fi.format == CAMERA_XRGB && fi.width == 640 && fi.height == 480 && fi.bytes == 640 * 480 * 4 && seq == 1);
    int worst = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            const uint8_t* p = yuv + (y * 640 + (x & ~1)) * 2;
            double Y = p[(x & 1) * 2] - 16, U = p[1] - 128, V = p[3] - 128;
            int r = clampi(1.164 * Y + 1.596 * V), g = clampi(1.164 * Y - 0.392 * U - 0.813 * V), b = clampi(1.164 * Y + 2.017 * U);
            uint32_t got = rgb[y * 640 + x];
            int dr = (int)(got >> 16 & 255) - r, dg = (int)(got >> 8 & 255) - g, db = (int)(got & 255) - b;
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            if (dr > worst) worst = dr;
            if (dg > worst) worst = dg;
            if (db > worst) worst = db;
            CHECK((got >> 24) == 0);
        }
    CHECK(worst <= 2);
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == 0);   /* the same one is not given twice */

    /* a picture the camera marks bad, one cut short (the frame ID toggles
     * without an end-of-frame), then a good one: only the good one comes */
    uint32_t before = g_cam->seq;
    picture(yuv, len, 1, 1, 1, 3060);
    picture(yuv, len / 2, 0, 0, 0, 3060);
    for (uint32_t i = 0; i < len; i++) yuv[i] = (uint8_t)(yuv[i] ^ 0x10);
    picture(yuv, len, 1, 1, 0, 1000);                /* (smaller payloads this time) */
    CHECK(g_cam->seq == before + 1);
    CHECK(g_cam->dropped >= 2);
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == 1 && seq == before + 1);
    /* a picture ended only by the next one's frame ID (no end-of-frame bit) */
    picture(yuv, len, 0, 0, 0, 3060);
    packet(yuv, 100, 1, 0);
    uvc_tick();
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == 1 && seq == before + 2);
    /* a buffer too small for the picture is refused */
    picture(yuv, len, 1, 1, 0, 3060);
    CHECK(camera_frame(rgb, 640 * 480 * 4 - 1, &fi, NULL) == -1);
    /* garbage headers: counted, nothing breaks */
    usb_xfer_t* x = q[qh++ % QMAX];
    x->buf[0] = 200; x->buf[1] = 0; x->actual = 20; x->status = USB_XFER_OK;
    uvc_tick();
    CHECK(g_cam->errors >= 1);
    camera_list();
    camera_close();
    CHECK(alt_now == 0 && !g_cam->on);
    /* opened again: the same setting, the endpoint not configured twice */
    CHECK(camera_open(&w, &h) == 0 && alt_now == 2 && opened == 1);
    camera_close();
    uvc_detach(&dev);
    CHECK(unbinds == 1 && !camera_available());
    CHECK(camera_open(&w, &h) == -1);
    CHECK(camera_frame(rgb, 640 * 480 * 4, &fi, &seq) == -1);
    free(yuv);
    free(rgb);
}

static void mjpeg_test(const char* dir) {
    char path[512];
    unsigned char *orig, *mj;
    unsigned int olen, mlen;
    ksnprintf(path, sizeof(path), "%s/orig.jpg", dir);
    CHECK(host_read_file(path, &orig, &olen) == 0);
    ksnprintf(path, sizeof(path), "%s/mjpeg.bin", dir);
    CHECK(host_read_file(path, &mj, &mlen) == 0);
    int has = 0;
    for (unsigned i = 0; i + 1 < mlen; i++) has |= mj[i] == 0xFF && mj[i + 1] == 0xC4;
    CHECK(!has);                                     /* (as a camera sends it: no tables) */

    yuy2_too_big = 1;                                /* no setting is big enough for YUY2: MJPEG */
    build_config();
    alt_now = -1;
    opened = 0;
    plug();
    int w, h;
    CHECK(camera_open(&w, &h) == 0);
    CHECK(w == 640 && h == 480 && committed_fmt == 2 && committed_frame == 1);
    CHECK(alt_now == 1 && g_cam->td_len == 1024);    /* 1000 bytes: the small setting */
    CHECK(probes == 3);                              /* 640x480 and 320x240 YUY2 refused, then MJPEG */
    uint32_t cap = camera_frame_bytes();
    CHECK(cap >= mlen + 420);
    uint8_t* out = calloc(1, cap);
    camera_frame_t fi;
    uint32_t seq = 0;
    picture(mj, mlen, 1, 1, 0, 1012);
    CHECK(camera_frame(out, cap, &fi, &seq) == 1);
    CHECK(fi.format == CAMERA_JPEG && fi.bytes == mlen + 420 && fi.width == 640);
    int w1, h1, w2, h2;
    unsigned char* a = host_jpeg_decode(orig, (int)olen, &w1, &h1);
    unsigned char* b = host_jpeg_decode(out, (int)fi.bytes, &w2, &h2);
    CHECK(a && b && w1 == 640 && h1 == 480 && w2 == 640 && h2 == 480);
    CHECK(memcmp(a, b, 640 * 480 * 3) == 0);         /* the tables put in are the standard's */
    /* a picture that already has tables is left as it is */
    uint32_t fl = 0;
    uint8_t* same = uvc_mjpeg_fix(orig, olen, &fl);
    CHECK(same && fl == olen && !memcmp(same, orig, olen));
    kfree(same);
    CHECK(!uvc_mjpeg_fix((const uint8_t*)"\x00\x01\x02\x03", 4, &fl));
    camera_close();
    uvc_detach(&dev);
    host_free(a);
    host_free(b);
    free(out);
    free(orig);
    free(mj);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    yuy2_test();
    mjpeg_test(argv[1]);
    printf("uvc: %u checks passed\n", checks);
    return 0;
}
