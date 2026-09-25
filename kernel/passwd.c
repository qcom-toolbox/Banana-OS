#include "passwd.h"
#include "config.h"
#include "random.h"
#include "kstring.h"
#include "../crypto/sha256.h"
#include "../crypto/aead.h"

#define ITERATIONS 10000
#define HEADER "# Banana OS password hashes (PBKDF2-HMAC-SHA256) - set with `passwd`.\n"

static void to_hex(const uint8_t* d, int n, char* out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[2 * n] = '\0';
}

static int from_hex(const char* s, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return s[2 * n] == '\0' || s[2 * n] == '$';
}

int passwd_is_set(const char* user) {
    char v[160];
    return cfg_get(PASSWD_FILE, user, v, sizeof(v)) && strncmp(v, "pbkdf2-sha256$", 14) == 0;
}

int passwd_set(const char* user, const char* password) {
    uint8_t salt[16], hash[32];
    char salt_hex[33], hash_hex[65], v[160];
    random_bytes(salt, sizeof(salt));
    pbkdf2_sha256((const uint8_t*)password, (uint32_t)strlen(password), salt, sizeof(salt),
                  ITERATIONS, hash);
    to_hex(salt, 16, salt_hex);
    to_hex(hash, 32, hash_hex);
    ksnprintf(v, sizeof(v), "pbkdf2-sha256$%u$%s$%s", ITERATIONS, salt_hex, hash_hex);
    int r = cfg_set(PASSWD_FILE, user, v, HEADER);
    cfg_persist();
    return r;
}

int passwd_clear(const char* user) {
    int r = cfg_set(PASSWD_FILE, user, NULL, HEADER);
    cfg_persist();
    return r;
}

int passwd_check(const char* user, const char* password) {
    char v[160];
    if (!cfg_get(PASSWD_FILE, user, v, sizeof(v)) || strncmp(v, "pbkdf2-sha256$", 14) != 0) return 0;
    const char* p = v + 14;
    uint32_t iters;
    int n = k_parse_u32(p, &iters);
    if (n == 0 || p[n] != '$' || iters == 0 || iters > 1000000) return 0;
    p += n + 1;
    uint8_t salt[16], want[32], got[32];
    if (!from_hex(p, salt, 16) || p[32] != '$') return 0;
    if (!from_hex(p + 33, want, 32)) return 0;
    pbkdf2_sha256((const uint8_t*)password, (uint32_t)strlen(password), salt, 16, iters, got);
    return ct_memcmp(got, want, 32) == 0;
}
