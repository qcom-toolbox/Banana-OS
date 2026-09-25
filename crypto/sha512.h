#ifndef SHA512_H
#define SHA512_H

#include "types.h"

/* SHA-512 (FIPS 180-4) - used by Ed25519 */

#define SHA512_LEN   64
#define SHA512_BLOCK 128

typedef struct {
    uint64_t h[8];
    uint64_t total;
    uint8_t  buf[SHA512_BLOCK];
    uint32_t buf_len;
} sha512_ctx_t;

void sha512_init(sha512_ctx_t* c);
void sha512_update(sha512_ctx_t* c, const void* data, uint32_t len);
void sha512_final(sha512_ctx_t* c, uint8_t out[SHA512_LEN]);
void sha512(const void* data, uint32_t len, uint8_t out[SHA512_LEN]);

#endif
