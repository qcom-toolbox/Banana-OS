#ifndef X25519_H
#define X25519_H

#include "types.h"

/* X25519 Diffie-Hellman (RFC 7748). */

/* out = scalar * point (both 32 bytes, little endian) */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* out = scalar * base point (9): the public key for private key `scalar` */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif
