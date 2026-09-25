#ifndef ED25519_H
#define ED25519_H

#include "types.h"

/* Ed25519 signatures (RFC 8032) - the SSH server's host key. */

/* public key for a 32-byte secret seed */
void ed25519_public_key(uint8_t pk[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[64], const uint8_t* msg, uint32_t len,
                  const uint8_t seed[32], const uint8_t pk[32]);

#endif
