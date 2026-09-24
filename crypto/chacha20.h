#ifndef CHACHA20_H
#define CHACHA20_H

#include "types.h"

/* ChaCha20 (RFC 8439): 256-bit key, 96-bit nonce, 32-bit block counter. */

/* One 64-byte keystream block. */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                    uint8_t out[64]);

/* XORs len bytes of keystream (starting at block `counter`) into buf. */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  uint8_t* buf, uint32_t len);

#endif
