#include "random.h"
#include "sha256.h"
#include "chacha20.h"
#include "kstring.h"
#include "timer.h"
#include "io.h"

static uint8_t  g_key[32];      /* ChaCha20 output key */
static uint8_t  g_pool[32];     /* running SHA-256 of all entropy seen */
static uint32_t g_jitter;       /* folded into the pool on the next request */
static uint32_t g_jitter_count;
static int      g_have_rdrand;

static int rdrand32(uint32_t* out) {
    uint8_t ok;
    uint32_t v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    *out = v;
    return ok;
}

void random_add_entropy(const void* data, uint32_t len) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, g_pool, sizeof(g_pool));
    sha256_update(&c, data, len);
    uint64_t t = rdtsc();
    sha256_update(&c, &t, sizeof(t));
    sha256_final(&c, g_pool);
}

void random_add_jitter(void) {
    /* rotate-xor the low TSC bits in: interrupt/packet arrival timing
     * relative to the CPU clock is the least predictable thing we have */
    uint32_t t = (uint32_t)rdtsc();
    g_jitter = ((g_jitter << 5) | (g_jitter >> 27)) ^ t;
    g_jitter_count++;
}

void random_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    g_have_rdrand = (c >> 30) & 1;

    /* timing jitter of a few thousand port reads / timer reads */
    for (int i = 0; i < 64; i++) {
        uint32_t sample[4];
        sample[0] = (uint32_t)rdtsc();
        for (int j = 0; j < 50; j++) (void)inb(0x61);
        sample[1] = (uint32_t)rdtsc();
        sample[2] = timer_ms();
        sample[3] = 0;
        if (g_have_rdrand) rdrand32(&sample[3]);
        random_add_entropy(sample, sizeof(sample));
    }
    memcpy(g_key, g_pool, sizeof(g_key));
}

void random_bytes(void* out, uint32_t len) {
    if (g_jitter_count) {
        uint32_t j[2] = { g_jitter, g_jitter_count };
        random_add_entropy(j, sizeof(j));
        g_jitter_count = 0;
    }
    uint32_t hw = 0;
    if (g_have_rdrand) rdrand32(&hw);
    uint64_t t = rdtsc();

    /* key = H(pool || previous key || tsc || rdrand) */
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, g_pool, sizeof(g_pool));
    sha256_update(&c, g_key, sizeof(g_key));
    sha256_update(&c, &t, sizeof(t));
    sha256_update(&c, &hw, sizeof(hw));
    sha256_final(&c, g_key);

    static const uint8_t nonce[12] = { 0 };
    uint8_t* p = (uint8_t*)out;
    uint8_t block[64];
    uint32_t ctr = 1;
    while (len) {
        chacha20_block(g_key, ctr++, nonce, block);
        uint32_t n = len < 64 ? len : 64;
        memcpy(p, block, n);
        p += n;
        len -= n;
    }
    /* fast key erasure: the key that produced this output is gone */
    chacha20_block(g_key, 0, nonce, block);
    memcpy(g_key, block, 32);
    memset(block, 0, sizeof(block));
}

uint32_t random_u32(void) {
    uint32_t v;
    random_bytes(&v, sizeof(v));
    return v;
}
