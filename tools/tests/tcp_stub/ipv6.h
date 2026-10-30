/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tools/test_tcp.py: what net/tcp.c needs of net/ipv6.h (IPv6 is not simulated) */
#ifndef IPV6_H
#define IPV6_H
#include "net.h"
typedef struct { uint8_t b[16]; } ip6_t;
int  ipv6_enabled(void);
int ip6_is_loopback(const ip6_t* ip);
int  ipv6_source(const ip6_t* dst, ip6_t* src);
int  ipv6_send(const ip6_t* src, const ip6_t* dst, uint8_t proto, const void* data, uint32_t len);
uint16_t ip6_checksum(const ip6_t* src, const ip6_t* dst, uint8_t proto, const void* data, uint32_t len);
void ip6_format(const ip6_t* a, char* out, int cap);
#endif
