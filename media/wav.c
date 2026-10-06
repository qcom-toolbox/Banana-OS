/* WAV: RIFF/WAVE with PCM (8, 16, 24, 32 bits), IEEE float or
 * WAVE_FORMAT_EXTENSIBLE around either. */
#include "codec.h"

typedef struct {
    codec_t  c;
    const uint8_t* p;               /* the sample data */
    uint32_t bytes, pos;            /* its size, and where decoding is */
    int      bits, fmt, in_ch, align;
} wav_t;

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* one sample as 16 bits */
static int16_t sample(const wav_t* w, const uint8_t* s) {
    switch (w->bits) {
    case 8:  return (int16_t)(((int)s[0] - 128) << 8);
    case 16: return (int16_t)le16(s);
    case 24: return (int16_t)le16(s + 1);
    case 32:
        if (w->fmt == 3) {                           /* IEEE float, -1..1, read from its bits */
            uint32_t u = le32(s);
            int e = (int)((u >> 23) & 0xFF) - 127;
            if (e < -16) return 0;
            if (e >= 0) return (u >> 31) ? -32768 : 32767;
            int32_t m = (int32_t)((u & 0x7FFFFF) | 0x800000);   /* 1.m as 24 bits */
            int32_t r = m >> (8 - e);                            /* x 2^15, e < 0 */
            if (r > 32767) r = 32767;
            return (int16_t)((u >> 31) ? -r : r);
        }
        return (int16_t)le16(s + 2);
    }
    return 0;
}

static int wav_decode(codec_t* c, int16_t* out, int max) {
    wav_t* w = (wav_t*)c;
    int n = 0;
    while (n < max && w->pos + (uint32_t)w->align <= w->bytes) {
        const uint8_t* f = w->p + w->pos;
        int bps = w->bits / 8;
        int16_t l = sample(w, f), r = w->in_ch > 1 ? sample(w, f + bps) : l;
        if (c->channels == 2) { out[n * 2] = l; out[n * 2 + 1] = r; }
        else out[n] = l;
        w->pos += (uint32_t)w->align;
        n++;
    }
    return n;
}

static int wav_seek(codec_t* c, uint64_t frame) {
    wav_t* w = (wav_t*)c;
    uint64_t at = frame * (uint64_t)w->align;
    if (at > w->bytes) at = w->bytes;
    w->pos = (uint32_t)at;
    return 0;
}

static void wav_close(codec_t* c) { kfree(c); }

codec_t* wav_open(const uint8_t* d, uint32_t len, char* err, int ecap) {
    if (len < 12) { codec_err(err, ecap, "a broken WAV file"); return NULL; }
    int fmt = 0, ch = 0, bits = 0, align = 0;
    uint32_t rate = 0;
    const uint8_t* data = NULL;
    uint32_t dlen = 0;
    for (uint32_t p = 12; p + 8 <= len; ) {
        uint32_t sz = le32(d + p + 4);
        const uint8_t* body = d + p + 8;
        uint32_t avail = len - p - 8;
        if (memcmp(d + p, "fmt ", 4) == 0 && sz >= 16 && avail >= 16) {
            fmt = le16(body);
            ch = le16(body + 2);
            rate = le32(body + 4);
            align = le16(body + 12);
            bits = le16(body + 14);
            if (fmt == 0xFFFE && sz >= 40 && avail >= 26) fmt = le16(body + 24);   /* the subformat GUID's first bytes */
        } else if (memcmp(d + p, "data", 4) == 0) {
            data = body;
            dlen = sz < avail ? sz : avail;
            break;
        }
        p += 8 + sz + (sz & 1);
        if (sz > len) break;
    }
    if (!data || !ch || !rate || !align) { codec_err(err, ecap, "a broken WAV file"); return NULL; }
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) {
        codec_err(err, ecap, "a WAV encoding Banana OS cannot play (only PCM and float)");
        return NULL;
    }
    if (align < ch * bits / 8) { codec_err(err, ecap, "a broken WAV file"); return NULL; }
    wav_t* w = (wav_t*)kzalloc(sizeof(wav_t));
    if (!w) { codec_err(err, ecap, "out of memory"); return NULL; }
    w->p = data;
    w->bytes = dlen;
    w->bits = bits;
    w->fmt = fmt;
    w->in_ch = ch;
    w->align = align;
    w->c.rate = (int)rate;
    w->c.channels = ch > 1 ? 2 : 1;
    w->c.length = dlen / (uint32_t)align;
    w->c.name = "WAV";
    w->c.decode = wav_decode;
    w->c.seek = wav_seek;
    w->c.close = wav_close;
    return &w->c;
}
