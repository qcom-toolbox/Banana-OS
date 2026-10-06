/* FLAC: frames of CONSTANT / VERBATIM / FIXED / LPC subframes with Rice
 * coded residuals, stereo decorrelation; any block size, 4..32 bits per
 * sample, mono or stereo (more channels: the first two). */
#include "codec.h"

#define MAX_BLOCK 65536

typedef struct {
    codec_t  c;
    const uint8_t* d;
    uint32_t len, first;            /* the stream, and where its first frame is */
    uint32_t pos;                   /* next frame */
    int      bps, in_ch;
    uint32_t sr;
    int32_t* buf[2];                /* the decoded block, per channel */
    int32_t* side;                  /* (decorrelation: the second channel may need a bit more) */
    int      blen, bat;             /* samples in it, and how many were handed out */
    uint64_t skip;                  /* after a seek: samples to drop from the next block */
} flac_t;

/* ── bits, most significant first ── */
typedef struct { const uint8_t* d; uint32_t len, pos; int bit; int bad; } br_t;

static uint32_t bits(br_t* b, int n) {
    uint32_t v = 0;
    while (n > 0) {
        if (b->pos >= b->len) { b->bad = 1; return v << n; }
        int avail = 8 - b->bit, take = n < avail ? n : avail;
        uint32_t chunk = ((uint32_t)b->d[b->pos] >> (avail - take)) & ((1u << take) - 1);
        v = (v << take) | chunk;
        n -= take;
        b->bit += take;
        if (b->bit == 8) { b->bit = 0; b->pos++; }
    }
    return v;
}

static int32_t sbits(br_t* b, int n) {
    if (n == 0) return 0;
    uint32_t v = n > 24 ? (bits(b, n - 24) << 24) | bits(b, 24) : bits(b, n);
    if (n < 32 && (v >> (n - 1)) & 1) v |= ~0u << n;
    return (int32_t)v;
}

static uint32_t unary(br_t* b) {             /* zeros before a one */
    uint32_t q = 0;
    for (;;) {
        if (b->pos >= b->len) { b->bad = 1; return q; }
        uint8_t byte = (uint8_t)(b->d[b->pos] << b->bit);
        if (byte) {
            int z = 0;
            while (!(byte & 0x80)) { byte <<= 1; z++; }
            q += (uint32_t)z;
            b->bit += z + 1;
            if (b->bit >= 8) { b->bit -= 8; b->pos++; }
            return q;
        }
        q += (uint32_t)(8 - b->bit);
        b->bit = 0;
        b->pos++;
    }
}

static void align(br_t* b) { if (b->bit) { b->bit = 0; b->pos++; } }

/* ── the frame header ── */
static uint8_t crc8(const uint8_t* p, uint32_t n) {
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

typedef struct { int block, ch_assign, bps; uint32_t sr; uint64_t number; int variable; uint32_t hdr_len; } fhdr_t;

/* a frame header at d[pos] (checked by its CRC) */
static int frame_header(const flac_t* f, uint32_t pos, fhdr_t* h) {
    if (pos + 6 > f->len) return -1;
    const uint8_t* p = f->d + pos;
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return -1;
    br_t b = { f->d, f->len, pos + 2, 0, 0 };
    h->variable = p[1] & 1;
    int bs = (int)bits(&b, 4), src = (int)bits(&b, 4);
    h->ch_assign = (int)bits(&b, 4);
    int ss = (int)bits(&b, 3);
    bits(&b, 1);
    if (bs == 0 || src == 15 || h->ch_assign > 10 || ss == 3) return -1;
    /* the frame / sample number, UTF-8 style */
    uint32_t c0 = bits(&b, 8);
    int more = 0;
    uint64_t num;
    if (!(c0 & 0x80)) num = c0;
    else if ((c0 & 0xE0) == 0xC0) { num = c0 & 0x1F; more = 1; }
    else if ((c0 & 0xF0) == 0xE0) { num = c0 & 0x0F; more = 2; }
    else if ((c0 & 0xF8) == 0xF0) { num = c0 & 0x07; more = 3; }
    else if ((c0 & 0xFC) == 0xF8) { num = c0 & 0x03; more = 4; }
    else if ((c0 & 0xFE) == 0xFC) { num = c0 & 0x01; more = 5; }
    else if (c0 == 0xFE) { num = 0; more = 6; }
    else return -1;
    for (int i = 0; i < more; i++) {
        uint32_t cb = bits(&b, 8);
        if ((cb & 0xC0) != 0x80) return -1;
        num = (num << 6) | (cb & 0x3F);
    }
    static const int fixed_bs[16] = { 0, 192, 576, 1152, 2304, 4608, -1, -2, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
    int block = fixed_bs[bs];
    if (block == -1) block = (int)bits(&b, 8) + 1;
    else if (block == -2) block = (int)bits(&b, 16) + 1;
    static const uint32_t rates[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    uint32_t sr = src < 12 ? rates[src] : 0;
    if (src == 0) sr = f->sr;
    else if (src == 12) sr = bits(&b, 8) * 1000;
    else if (src == 13) sr = bits(&b, 16);
    else if (src == 14) sr = bits(&b, 16) * 10;
    static const int sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    int bps = ss ? sizes[ss] : f->bps;
    if (b.bad || b.bit) return -1;
    uint8_t crc = (uint8_t)bits(&b, 8);
    if (b.bad || crc != crc8(p, b.pos - 1 - pos)) return -1;
    h->block = block;
    h->sr = sr;
    h->bps = bps;
    h->number = num;                                 /* a sample number (variable blocks) or a frame number */
    h->hdr_len = b.pos - pos;
    return block > 0 && block <= MAX_BLOCK ? 0 : -1;
}

/* ── subframes ── */
static int residual(br_t* b, int32_t* out, int block, int order) {
    int method = (int)bits(b, 2);
    if (method > 1) return -1;
    int pbits = method ? 5 : 4, esc = method ? 31 : 15;
    int porder = (int)bits(b, 4);
    int parts = 1 << porder;
    if ((block >> porder) < order) return -1;
    int i = order;
    for (int p = 0; p < parts; p++) {
        int n = (block >> porder) - (p == 0 ? order : 0);
        int k = (int)bits(b, pbits);
        if (k == esc) {
            int raw = (int)bits(b, 5);
            for (int j = 0; j < n; j++) out[i++] = sbits(b, raw);
        } else {
            for (int j = 0; j < n; j++) {
                uint32_t q = unary(b);
                uint32_t u = (q << k) | (k ? bits(b, k) : 0);
                out[i++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
        if (b->bad) return -1;
    }
    return 0;
}

static int subframe(br_t* b, int32_t* s, int block, int bps) {
    if (bits(b, 1)) return -1;                       /* the zero pad bit */
    int type = (int)bits(b, 6);
    int wasted = 0;
    if (bits(b, 1)) { wasted = (int)unary(b) + 1; bps -= wasted; }
    if (bps <= 0 || bps > 33) return -1;
    if (type == 0) {                                 /* CONSTANT */
        int32_t v = sbits(b, bps);
        for (int i = 0; i < block; i++) s[i] = v;
    } else if (type == 1) {                          /* VERBATIM */
        for (int i = 0; i < block; i++) s[i] = sbits(b, bps);
    } else if (type >= 8 && type <= 12) {            /* FIXED, order 0..4 */
        int order = type - 8;
        for (int i = 0; i < order; i++) s[i] = sbits(b, bps);
        if (residual(b, s, block, order)) return -1;
        for (int i = order; i < block; i++) {
            switch (order) {
            case 1: s[i] += s[i - 1]; break;
            case 2: s[i] += 2 * s[i - 1] - s[i - 2]; break;
            case 3: s[i] += 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
            case 4: s[i] += 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
            }
        }
    } else if (type >= 32) {                         /* LPC, order 1..32 */
        int order = type - 31;
        for (int i = 0; i < order; i++) s[i] = sbits(b, bps);
        int prec = (int)bits(b, 4) + 1;
        if (prec == 16) return -1;
        int shift = sbits(b, 5);
        if (shift < 0) return -1;
        int32_t coef[32];
        for (int i = 0; i < order; i++) coef[i] = sbits(b, prec);
        if (residual(b, s, block, order)) return -1;
        for (int i = order; i < block; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)coef[j] * s[i - 1 - j];
            s[i] += (int32_t)(sum >> shift);
        }
    } else {
        return -1;
    }
    if (wasted) for (int i = 0; i < block; i++) s[i] <<= wasted;
    return b->bad ? -1 : 0;
}

/* decodes the frame at f->pos into buf; 0, or -1 at the end / on a broken frame */
static int next_frame(flac_t* f, uint64_t* first_sample) {
    fhdr_t h;
    br_t b;
    int nch, ok;
    do {
        /* a broken frame: look for the next good header */
        while (f->pos + 6 <= f->len && frame_header(f, f->pos, &h) != 0) f->pos++;
        if (f->pos + 6 > f->len) return -1;
        b = (br_t){ f->d, f->len, f->pos + h.hdr_len, 0, 0 };
        nch = h.ch_assign < 8 ? h.ch_assign + 1 : 2;
        ok = 1;
        for (int ch = 0; ch < nch && ok; ch++) {
            int extra = (h.ch_assign == 8 && ch == 1) || (h.ch_assign == 9 && ch == 0) || (h.ch_assign == 10 && ch == 1);
            int32_t* dst = ch < 2 ? f->buf[ch] : f->side;   /* channels past two are decoded and dropped */
            if (subframe(&b, dst, h.block, h.bps + extra) != 0) ok = 0;
        }
        if (!ok) f->pos++;
    } while (!ok);
    align(&b);
    b.pos += 2;                                      /* the frame's CRC-16 */
    int32_t* l = f->buf[0], *r = f->buf[1];
    switch (h.ch_assign) {
    case 8:  for (int i = 0; i < h.block; i++) r[i] = l[i] - r[i]; break;        /* left / side */
    case 9:  for (int i = 0; i < h.block; i++) l[i] += r[i]; break;              /* side / right */
    case 10: for (int i = 0; i < h.block; i++) {                                 /* mid / side */
                 int32_t mid = (l[i] << 1) | (r[i] & 1), side = r[i];
                 l[i] = (mid + side) >> 1;
                 r[i] = (mid - side) >> 1;
             }
             break;
    }
    if (nch == 1) memcpy(f->buf[1], f->buf[0], (size_t)h.block * 4);
    /* to 16 bits */
    int sh = h.bps - 16;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < h.block; i++) {
            int32_t v = f->buf[ch][i];
            v = sh > 0 ? v >> sh : v << -sh;
            f->buf[ch][i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
    f->blen = h.block;
    f->bat = 0;
    f->pos = b.pos;
    if (first_sample) *first_sample = h.variable ? h.number : h.number * (uint64_t)h.block;
    return 0;
}

static int flac_decode(codec_t* c, int16_t* out, int max) {
    flac_t* f = (flac_t*)c;
    int n = 0;
    while (n < max) {
        if (f->bat >= f->blen) {
            uint64_t at;
            if (next_frame(f, &at) != 0) break;
            if (f->skip) {                           /* (after a seek) */
                uint64_t s = f->skip < (uint64_t)f->blen ? f->skip : (uint64_t)f->blen;
                f->bat = (int)s;
                f->skip -= s;
                continue;
            }
        }
        int take = f->blen - f->bat;
        if (take > max - n) take = max - n;
        for (int i = 0; i < take; i++) {
            if (c->channels == 2) {
                out[(n + i) * 2] = (int16_t)f->buf[0][f->bat + i];
                out[(n + i) * 2 + 1] = (int16_t)f->buf[1][f->bat + i];
            } else {
                out[n + i] = (int16_t)f->buf[0][f->bat + i];
            }
        }
        f->bat += take;
        n += take;
    }
    return n;
}

/* to a sample: jump near it by bytes, then frame by frame */
static int flac_seek(codec_t* c, uint64_t frame) {
    flac_t* f = (flac_t*)c;
    f->blen = f->bat = 0;
    f->skip = 0;
    uint32_t span = f->len - f->first;
    uint32_t guess = f->first;
    if (c->length && frame) {
        uint64_t g = (uint64_t)span * frame / c->length;
        guess = f->first + (uint32_t)(g > 8192 ? g - 8192 : 0);   /* a little before */
    }
    for (int tries = 0; tries < 2; tries++) {
        f->pos = guess;
        fhdr_t h;
        while (f->pos + 6 <= f->len && frame_header(f, f->pos, &h) != 0) f->pos++;
        if (f->pos + 6 > f->len) break;
        uint64_t at = h.variable ? h.number : h.number * (uint64_t)h.block;
        if (at <= frame) {
            /* walk forward frame by frame, without decoding the samples twice */
            for (;;) {
                uint32_t here = f->pos;
                uint64_t s;
                if (next_frame(f, &s) != 0) { f->pos = here; break; }
                if (s + (uint64_t)f->blen > frame) {
                    f->bat = (int)(frame - s);
                    return 0;
                }
            }
            return 0;
        }
        guess = f->first;                            /* overshot: from the start */
    }
    f->pos = f->first;
    f->skip = frame;
    return 0;
}

static void flac_close(codec_t* c) {
    flac_t* f = (flac_t*)c;
    kfree(f->buf[0]);
    kfree(f->buf[1]);
    kfree(f->side);
    kfree(f);
}

codec_t* flac_open(const uint8_t* d, uint32_t len, char* err, int ecap) {
    if (len < 42 || memcmp(d, "fLaC", 4) != 0) { codec_err(err, ecap, "a broken FLAC file"); return NULL; }
    uint32_t p = 4;
    uint32_t sr = 0;
    int ch = 0, bps = 0;
    uint64_t total = 0;
    for (;;) {                                       /* metadata blocks: STREAMINFO matters */
        if (p + 4 > len) { codec_err(err, ecap, "a broken FLAC file"); return NULL; }
        int last = d[p] & 0x80, type = d[p] & 0x7F;
        uint32_t sz = (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3];
        if (type == 0 && sz >= 34 && p + 4 + 34 <= len) {
            const uint8_t* s = d + p + 4;
            sr = (uint32_t)s[10] << 12 | (uint32_t)s[11] << 4 | s[12] >> 4;
            ch = ((s[12] >> 1) & 7) + 1;
            bps = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
            total = (uint64_t)(s[13] & 0x0F) << 32 | (uint32_t)s[14] << 24 | (uint32_t)s[15] << 16 | (uint32_t)s[16] << 8 | s[17];
        }
        p += 4 + sz;
        if (last) break;
    }
    if (!sr || !ch || p >= len) { codec_err(err, ecap, "a broken FLAC file"); return NULL; }
    flac_t* f = (flac_t*)kzalloc(sizeof(flac_t));
    if (!f) { codec_err(err, ecap, "out of memory"); return NULL; }
    f->d = d;
    f->len = len;
    f->first = f->pos = p;
    f->sr = sr;
    f->bps = bps;
    f->in_ch = ch;
    f->buf[0] = (int32_t*)kmalloc(MAX_BLOCK * 4);
    f->buf[1] = (int32_t*)kmalloc(MAX_BLOCK * 4);
    f->side = (int32_t*)kmalloc(MAX_BLOCK * 4);
    if (!f->buf[0] || !f->buf[1] || !f->side) { flac_close(&f->c); codec_err(err, ecap, "out of memory"); return NULL; }
    f->c.rate = (int)sr;
    f->c.channels = ch > 1 ? 2 : 1;
    f->c.length = total;
    f->c.name = "FLAC";
    f->c.decode = flac_decode;
    f->c.seek = flac_seek;
    f->c.close = flac_close;
    return &f->c;
}
