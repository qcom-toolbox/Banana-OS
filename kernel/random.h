#ifndef RANDOM_H
#define RANDOM_H

#include "types.h"

/* Kernel CSPRNG: entropy (TSC timing jitter, RDRAND when the CPU has it,
 * NIC packet arrival times, the MAC address) is hashed into a SHA-256
 * pool; output comes from ChaCha20 keyed off that pool, re-keyed after
 * every request ("fast key erasure"). Used for TLS keys, TCP initial
 * sequence numbers, DNS/DHCP transaction ids and ephemeral ports. */

void     random_init(void);
void     random_add_entropy(const void* data, uint32_t len);
void     random_add_jitter(void);          /* cheap: call on external events */
void     random_bytes(void* out, uint32_t len);
uint32_t random_u32(void);

#endif
