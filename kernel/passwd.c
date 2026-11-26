/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "passwd.h"
#include "config.h"
#include "random.h"
#include "kstring.h"
#include "../crypto/sha256.h"
#include "../crypto/aead.h"
#include "users.h"
#include "kheap.h"
#include "fs.h"
#include "rtc.h"
#include "serial.h"

#define ITERATIONS 100000   /* (older hashes, 10000, are redone at the next good login) */

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

/* the hash field of user's line ("" no password, "!" locked), from the
 * standard format or the old "user=hash" one: 1 if there is a line */
static int hash_of(const char* user, char* out, int cap) {
    out[0] = 0;
    int fi = fs_find_file(PASSWD_FILE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return 0;
    size_t ul = strlen(user);
    const char* t = f->content;
    for (const char* p = t; p && *p;) {
        const char* nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l > ul && !strncmp(p, user, ul) && (p[ul] == ':' || p[ul] == '=')) {
            const char* h = p + ul + 1;
            size_t n = 0;
            while (h + n < p + l && h[n] != ':' && h[n] != '\r') n++;
            if (p[ul] == '=') {                          /* old: "user=pbkdf2-sha256$..." */
                if ((int)n + 2 > cap) return 1;
                out[0] = '$';
                memcpy(out + 1, h, n);
                out[n + 1] = 0;
            } else {
                if ((int)n + 1 > cap) return 1;
                memcpy(out, h, n);
                out[n] = 0;
            }
            return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

static int write_hash(const char* user, const char* hash) {
    char line[256];
    uint32_t days = (uint32_t)(rtc_now() / 86400);
    ksnprintf(line, sizeof(line), "%s:%s:%u:0:99999:7:::", user, hash, days);
    int r = users_file_line(PASSWD_FILE, user, line, 0600);
    cfg_persist();
    return r;
}

int passwd_is_set(const char* user) {
    char v[200];
    return hash_of(user, v, sizeof(v)) && !strncmp(v, "$pbkdf2-sha256$", 15);
}

int passwd_locked(const char* user) {
    char v[200];
    if (!hash_of(user, v, sizeof(v))) return !strcmp(user, "root");   /* (root: locked unless given one) */
    return v[0] == '!' || v[0] == '*';
}

int passwd_set(const char* user, const char* password) {
    uint8_t salt[16], hash[32];
    char salt_hex[33], hash_hex[65], v[180];
    random_bytes(salt, sizeof(salt));
    pbkdf2_sha256((const uint8_t*)password, (uint32_t)strlen(password), salt, sizeof(salt),
                  ITERATIONS, hash);
    to_hex(salt, 16, salt_hex);
    to_hex(hash, 32, hash_hex);
    ksnprintf(v, sizeof(v), "$pbkdf2-sha256$%u$%s$%s", ITERATIONS, salt_hex, hash_hex);
    return write_hash(user, v);
}

int passwd_clear(const char* user) { return write_hash(user, strcmp(user, "root") ? "" : "!"); }

int passwd_remove(const char* user) {
    int r = users_file_line(PASSWD_FILE, user, NULL, 0600);
    cfg_persist();
    return r;
}

int passwd_check(const char* user, const char* password) {
    char v[200];
    if (!hash_of(user, v, sizeof(v)) || strncmp(v, "$pbkdf2-sha256$", 15) != 0) return 0;
    const char* p = v + 15;
    uint32_t iters;
    int n = k_parse_u32(p, &iters);
    if (n == 0 || p[n] != '$' || iters == 0 || iters > 1000000) return 0;
    p += n + 1;
    uint8_t salt[16], want[32], got[32];
    if (!from_hex(p, salt, 16) || p[32] != '$') return 0;
    if (!from_hex(p + 33, want, 32)) return 0;
    pbkdf2_sha256((const uint8_t*)password, (uint32_t)strlen(password), salt, 16, iters, got);
    int ok = ct_memcmp(got, want, 32) == 0;
    if (ok && iters < ITERATIONS) passwd_set(user, password);   /* a stronger hash from now on */
    return ok;
}

/* /etc/shadow at every start: old "user=hash" lines (and the old header)
 * in the standard format; root there, locked; root's file, 0600 */
void passwd_boot(void) {
    int fi = fs_find_file(PASSWD_FILE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (f && f->content) {
        int old = 0;
        for (const char* p = f->content; p && *p;) {
            const char* nl = strchr(p, '\n');
            const char* eq = strchr(p, '=');
            const char* co = strchr(p, ':');
            if (p[0] != '#' && eq && (!nl || eq < nl) && (!co || eq < co)) old = 1;
            if (p[0] == '#') old = 1;
            p = nl ? nl + 1 : NULL;
        }
        if (old) {
            /* the names of the old lines, then each rewritten */
            char names[8][40];
            int nn = 0;
            for (const char* p = f->content; p && *p && nn < 8;) {
                const char* nl = strchr(p, '\n');
                const char* eq = strchr(p, '=');
                if (p[0] != '#' && eq && (!nl || eq < nl) && eq - p < 39) {
                    memcpy(names[nn], p, (size_t)(eq - p));
                    names[nn][eq - p] = 0;
                    nn++;
                }
                p = nl ? nl + 1 : NULL;
            }
            char hashes[8][200];
            for (int i = 0; i < nn; i++) hash_of(names[i], hashes[i], sizeof(hashes[i]));
            /* keep the standard lines there may be, drop the old ones and comments */
            char* keep = (char*)kmalloc(f->size + 1);
            uint32_t k = 0;
            if (keep) {
                for (const char* p = f->content; p && *p;) {
                    const char* nl = strchr(p, '\n');
                    size_t l = nl ? (size_t)(nl - p) + 1 : strlen(p);
                    const char* eq = strchr(p, '=');
                    const char* co = strchr(p, ':');
                    int std = p[0] != '#' && co && (!nl || co < nl) && (!eq || (nl && eq > nl) || co < eq);
                    if (std) { memcpy(keep + k, p, l); k += (uint32_t)l; }
                    p = nl ? nl + 1 : NULL;
                }
                fs_write_path(PASSWD_FILE, keep, k);
                kfree(keep);
            }
            for (int i = 0; i < nn; i++) write_hash(names[i], hashes[i]);
            klog("passwd: /etc/shadow in the standard format (%d password%s kept)\n", nn, nn == 1 ? "" : "s");
        }
    }
    char v[200];
    if (!hash_of("root", v, sizeof(v))) write_hash("root", "!");
    if (fs_find_file(PASSWD_FILE) >= 0) {
        fs_chown(PASSWD_FILE, 0, 0);
        fs_chmod(PASSWD_FILE, 0600);
    }
}
