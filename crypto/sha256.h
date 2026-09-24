#ifndef SHA256_H
#define SHA256_H

#include "types.h"

#define SHA256_LEN   32
#define SHA256_BLOCK 64

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[SHA256_BLOCK];
    uint32_t buf_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, uint32_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[SHA256_LEN]);
void sha256(const void* data, uint32_t len, uint8_t out[SHA256_LEN]);

/* HMAC-SHA256 (RFC 2104) */
typedef struct {
    sha256_ctx_t inner, outer;
} hmac_sha256_ctx_t;

void hmac_sha256_init(hmac_sha256_ctx_t* c, const uint8_t* key, uint32_t key_len);
void hmac_sha256_update(hmac_sha256_ctx_t* c, const void* data, uint32_t len);
void hmac_sha256_final(hmac_sha256_ctx_t* c, uint8_t out[SHA256_LEN]);
void hmac_sha256(const uint8_t* key, uint32_t key_len, const void* data, uint32_t len,
                 uint8_t out[SHA256_LEN]);

/* HKDF (RFC 5869) with SHA-256 */
void hkdf_extract(const uint8_t* salt, uint32_t salt_len, const uint8_t* ikm, uint32_t ikm_len,
                  uint8_t prk[SHA256_LEN]);
void hkdf_expand(const uint8_t prk[SHA256_LEN], const uint8_t* info, uint32_t info_len,
                 uint8_t* out, uint32_t out_len);

#endif
