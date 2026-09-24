#ifndef AEAD_H
#define AEAD_H

#include "types.h"

/* The two AEAD ciphers TLS 1.3 needs, with 96-bit nonces and 16-byte
 * tags appended to the ciphertext. */

#define AEAD_TAG_LEN 16

/* ── AES-128 block cipher (FIPS 197) ──────────────────────────── */
typedef struct {
    uint32_t rk[44];      /* expanded round keys */
} aes128_t;

void aes128_init(aes128_t* a, const uint8_t key[16]);
void aes128_encrypt(const aes128_t* a, const uint8_t in[16], uint8_t out[16]);

/* ── AES-128-GCM (NIST SP 800-38D) ────────────────────────────── */
typedef struct {
    aes128_t aes;
    uint64_t htab_hi[16], htab_lo[16];   /* 4-bit multiplication table for H */
} aes_gcm_t;

void aes_gcm_init(aes_gcm_t* g, const uint8_t key[16]);
/* in-place: encrypts buf[0..len) and writes the tag to tag[16] */
void aes_gcm_seal(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                  uint8_t* buf, uint32_t len, uint8_t tag[16]);
/* in-place decrypt; returns 0 if the tag verified, -1 otherwise (buf then garbage) */
int  aes_gcm_open(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                  uint8_t* buf, uint32_t len, const uint8_t tag[16]);

/* ── ChaCha20-Poly1305 (RFC 8439) ─────────────────────────────── */
void poly1305_mac(uint8_t tag[16], const uint8_t* msg, uint32_t len, const uint8_t key[32]);

void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* aad, uint32_t aad_len,
                           uint8_t* buf, uint32_t len, uint8_t tag[16]);
int  chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t* aad, uint32_t aad_len,
                           uint8_t* buf, uint32_t len, const uint8_t tag[16]);

/* constant-time comparison: 0 if equal */
int  ct_memcmp(const void* a, const void* b, uint32_t len);

#endif
