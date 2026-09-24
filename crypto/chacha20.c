#include "chacha20.h"

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                         \
    a += b; d ^= a; d = ROTL(d, 16);           \
    c += d; b ^= c; b = ROTL(b, 12);           \
    a += b; d ^= a; d = ROTL(d, 8);            \
    c += d; b ^= c; b = ROTL(b, 7)

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                    uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + i * 4);
    s[12] = counter;
    s[13] = le32(nonce);
    s[14] = le32(nonce + 4);
    s[15] = le32(nonce + 8);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[i * 4]     = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  uint8_t* buf, uint32_t len) {
    uint8_t ks[64];
    while (len) {
        chacha20_block(key, counter++, nonce, ks);
        uint32_t n = len < 64 ? len : 64;
        for (uint32_t i = 0; i < n; i++) buf[i] ^= ks[i];
        buf += n;
        len -= n;
    }
}
