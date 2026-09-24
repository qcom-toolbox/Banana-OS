#include "aead.h"
#include "chacha20.h"
#include "kstring.h"

int ct_memcmp(const void* a, const void* b, uint32_t len) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (uint32_t i = 0; i < len; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d;
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ── AES-128 ────────────────────────────────────────────────────── */

static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

/* T-tables: Te0[x] = column (2s, s, s, 3s) for s = SBOX[x]; Te1..3 are
 * its byte rotations. Built once on first use. */
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static int g_tables_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint32_t ror8(uint32_t v) { return (v >> 8) | (v << 24); }

static void build_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i], s2 = xtime(s), s3 = (uint8_t)(s2 ^ s);
        uint32_t t = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        Te0[i] = t;
        Te1[i] = ror8(t);
        Te2[i] = ror8(Te1[i]);
        Te3[i] = ror8(Te2[i]);
    }
    g_tables_ready = 1;
}

void aes128_init(aes128_t* a, const uint8_t key[16]) {
    static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    if (!g_tables_ready) build_tables();
    for (int i = 0; i < 4; i++) a->rk[i] = be32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = a->rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);                                /* RotWord */
            t = ((uint32_t)SBOX[t >> 24] << 24) | ((uint32_t)SBOX[(t >> 16) & 0xff] << 16) |
                ((uint32_t)SBOX[(t >> 8) & 0xff] << 8) | SBOX[t & 0xff]; /* SubWord */
            t ^= (uint32_t)rcon[i / 4 - 1] << 24;
        }
        a->rk[i] = a->rk[i - 4] ^ t;
    }
}

void aes128_encrypt(const aes128_t* a, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* rk = a->rk;
    uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1];
    uint32_t s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    for (int r = 1; r < 10; r++) {
        rk += 4;
        uint32_t t0 = Te0[s0 >> 24] ^ Te1[(s1 >> 16) & 0xff] ^ Te2[(s2 >> 8) & 0xff] ^ Te3[s3 & 0xff] ^ rk[0];
        uint32_t t1 = Te0[s1 >> 24] ^ Te1[(s2 >> 16) & 0xff] ^ Te2[(s3 >> 8) & 0xff] ^ Te3[s0 & 0xff] ^ rk[1];
        uint32_t t2 = Te0[s2 >> 24] ^ Te1[(s3 >> 16) & 0xff] ^ Te2[(s0 >> 8) & 0xff] ^ Te3[s1 & 0xff] ^ rk[2];
        uint32_t t3 = Te0[s3 >> 24] ^ Te1[(s0 >> 16) & 0xff] ^ Te2[(s1 >> 8) & 0xff] ^ Te3[s2 & 0xff] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    /* final round: SubBytes + ShiftRows + AddRoundKey, no MixColumns */
#define FIN(a, b, c, d) (((uint32_t)SBOX[(a) >> 24] << 24) | ((uint32_t)SBOX[((b) >> 16) & 0xff] << 16) | \
                         ((uint32_t)SBOX[((c) >> 8) & 0xff] << 8) | SBOX[(d) & 0xff])
    put_be32(out,      FIN(s0, s1, s2, s3) ^ rk[0]);
    put_be32(out + 4,  FIN(s1, s2, s3, s0) ^ rk[1]);
    put_be32(out + 8,  FIN(s2, s3, s0, s1) ^ rk[2]);
    put_be32(out + 12, FIN(s3, s0, s1, s2) ^ rk[3]);
#undef FIN
}

/* ── GCM ────────────────────────────────────────────────────────── */

/* Shoup's 4-bit table method: htab[i] = i * H in GF(2^128) for every
 * 4-bit i, then a multiply is 32 table lookups + shifts. */
static const uint16_t LAST4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0,
};

void aes_gcm_init(aes_gcm_t* g, const uint8_t key[16]) {
    aes128_init(&g->aes, key);
    uint8_t h[16] = { 0 };
    aes128_encrypt(&g->aes, h, h);
    uint64_t vh = ((uint64_t)be32(h) << 32) | be32(h + 4);
    uint64_t vl = ((uint64_t)be32(h + 8) << 32) | be32(h + 12);

    g->htab_hi[8] = vh;
    g->htab_lo[8] = vl;
    g->htab_hi[0] = g->htab_lo[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (uint32_t)(vl & 1) * 0xe1000000u;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)t << 32);
        g->htab_hi[i] = vh;
        g->htab_lo[i] = vl;
    }
    for (int i = 2; i <= 8; i *= 2) {
        uint64_t bh = g->htab_hi[i], bl = g->htab_lo[i];
        for (int j = 1; j < i; j++) {
            g->htab_hi[i + j] = bh ^ g->htab_hi[j];
            g->htab_lo[i + j] = bl ^ g->htab_lo[j];
        }
    }
}

/* x = x * H */
static void gf_mult_h(const aes_gcm_t* g, uint8_t x[16]) {
    uint8_t lo = x[15] & 0xf;
    uint64_t zh = g->htab_hi[lo], zl = g->htab_lo[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0xf;
        uint8_t hi = (uint8_t)(x[i] >> 4);
        if (i != 15) {
            uint8_t rem = (uint8_t)(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ ((uint64_t)LAST4[rem] << 48);
            zh ^= g->htab_hi[lo];
            zl ^= g->htab_lo[lo];
        }
        uint8_t rem = (uint8_t)(zl & 0xf);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4) ^ ((uint64_t)LAST4[rem] << 48);
        zh ^= g->htab_hi[hi];
        zl ^= g->htab_lo[hi];
    }
    put_be32(x, (uint32_t)(zh >> 32));
    put_be32(x + 4, (uint32_t)zh);
    put_be32(x + 8, (uint32_t)(zl >> 32));
    put_be32(x + 12, (uint32_t)zl);
}

static void ghash_update(const aes_gcm_t* g, uint8_t y[16], const uint8_t* data, uint32_t len) {
    while (len) {
        uint32_t n = len < 16 ? len : 16;
        for (uint32_t i = 0; i < n; i++) y[i] ^= data[i];
        gf_mult_h(g, y);
        data += n;
        len -= n;
    }
}

static void gcm_ctr(const aes_gcm_t* g, const uint8_t nonce[12], uint8_t* buf, uint32_t len) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, nonce, 12);
    uint32_t c = 2;              /* counter 1 is J0, reserved for the tag */
    while (len) {
        put_be32(ctr + 12, c++);
        aes128_encrypt(&g->aes, ctr, ks);
        uint32_t n = len < 16 ? len : 16;
        for (uint32_t i = 0; i < n; i++) buf[i] ^= ks[i];
        buf += n;
        len -= n;
    }
}

static void gcm_tag(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                    const uint8_t* ct, uint32_t len, uint8_t tag[16]) {
    uint8_t y[16] = { 0 };
    ghash_update(g, y, aad, aad_len);
    ghash_update(g, y, ct, len);
    uint8_t lens[16] = { 0 };
    put_be32(lens + 4, aad_len * 8u);
    put_be32(lens + 12, len * 8u);
    ghash_update(g, y, lens, 16);

    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    put_be32(j0 + 12, 1);
    aes128_encrypt(&g->aes, j0, j0);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ j0[i];
}

void aes_gcm_seal(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                  uint8_t* buf, uint32_t len, uint8_t tag[16]) {
    gcm_ctr(g, nonce, buf, len);
    gcm_tag(g, nonce, aad, aad_len, buf, len, tag);
}

int aes_gcm_open(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                 uint8_t* buf, uint32_t len, const uint8_t tag[16]) {
    uint8_t expect[16];
    gcm_tag(g, nonce, aad, aad_len, buf, len, expect);
    if (ct_memcmp(expect, tag, 16) != 0) return -1;
    gcm_ctr(g, nonce, buf, len);
    return 0;
}

/* ── Poly1305 (26-bit limbs) ────────────────────────────────────── */

typedef struct {
    uint32_t r[5], s[4], h[5];
    uint8_t  buf[16];      /* pending partial block (streaming updates) */
    uint32_t buf_len;
} poly1305_t;

static void poly_init(poly1305_t* p, const uint8_t key[32]) {
    p->r[0] = (le32(key + 0)) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 4; i++) p->s[i] = le32(key + 16 + 4 * i);
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    p->buf_len = 0;
}

/* one 16-byte block; hibit = 1<<24 for full blocks, 0 for the padded last one */
static void poly_block(poly1305_t* p, const uint8_t m[16], uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];

    h0 += (le32(m + 0)) & 0x3ffffff;
    h1 += (le32(m + 3) >> 2) & 0x3ffffff;
    h2 += (le32(m + 6) >> 4) & 0x3ffffff;
    h3 += (le32(m + 9) >> 6) & 0x3ffffff;
    h4 += (le32(m + 12) >> 8) | hibit;

    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

/* Streaming: only the very last partial block gets the 0x01 pad, so
 * pieces fed separately (the AEAD's aad, padding, ciphertext...) are
 * joined into full blocks first. */
static void poly_update(poly1305_t* p, const uint8_t* m, uint32_t len) {
    if (p->buf_len) {
        uint32_t take = 16 - p->buf_len;
        if (take > len) take = len;
        memcpy(p->buf + p->buf_len, m, take);
        p->buf_len += take;
        m += take;
        len -= take;
        if (p->buf_len < 16) return;
        poly_block(p, p->buf, 1u << 24);
        p->buf_len = 0;
    }
    while (len >= 16) {
        poly_block(p, m, 1u << 24);
        m += 16;
        len -= 16;
    }
    if (len) {
        memcpy(p->buf, m, len);
        p->buf_len = len;
    }
}

static void poly_finish(poly1305_t* p, uint8_t tag[16]) {
    if (p->buf_len) {
        uint8_t last[16] = { 0 };
        memcpy(last, p->buf, p->buf_len);
        last[p->buf_len] = 1;
        poly_block(p, last, 0);
        p->buf_len = 0;
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* g = h + -p, pick g if h >= p (no borrow) */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;   /* all ones if g4 didn't go negative */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    uint64_t f;
    f = (uint64_t)h0 + p->s[0];             h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->s[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->s[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->s[3] + (f >> 32); h3 = (uint32_t)f;
    put_le32(tag, h0);
    put_le32(tag + 4, h1);
    put_le32(tag + 8, h2);
    put_le32(tag + 12, h3);
}

void poly1305_mac(uint8_t tag[16], const uint8_t* msg, uint32_t len, const uint8_t key[32]) {
    poly1305_t p;
    poly_init(&p, key);
    poly_update(&p, msg, len);
    poly_finish(&p, tag);
}

/* ── ChaCha20-Poly1305 AEAD ─────────────────────────────────────── */

static void cc_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                   const uint8_t* ct, uint32_t len, uint8_t tag[16]) {
    uint8_t block[64];
    chacha20_block(key, 0, nonce, block);   /* first 32 bytes = one-time Poly1305 key */
    poly1305_t p;
    poly_init(&p, block);
    static const uint8_t zeros[16] = { 0 };
    poly_update(&p, aad, aad_len);
    if (aad_len % 16) poly_update(&p, zeros, 16 - aad_len % 16);
    poly_update(&p, ct, len);
    if (len % 16) poly_update(&p, zeros, 16 - len % 16);
    uint8_t lens[16] = { 0 };
    put_le32(lens, aad_len);
    put_le32(lens + 8, len);
    poly_update(&p, lens, 16);
    poly_finish(&p, tag);
    memset(block, 0, sizeof(block));
}

void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* aad, uint32_t aad_len,
                           uint8_t* buf, uint32_t len, uint8_t tag[16]) {
    chacha20_xor(key, 1, nonce, buf, len);
    cc_tag(key, nonce, aad, aad_len, buf, len, tag);
}

int chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                          const uint8_t* aad, uint32_t aad_len,
                          uint8_t* buf, uint32_t len, const uint8_t tag[16]) {
    uint8_t expect[16];
    cc_tag(key, nonce, aad, aad_len, buf, len, expect);
    if (ct_memcmp(expect, tag, 16) != 0) return -1;
    chacha20_xor(key, 1, nonce, buf, len);
    return 0;
}
