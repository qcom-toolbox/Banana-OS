#include "selftest.h"
#include "sha256.h"
#include "chacha20.h"
#include "x25519.h"
#include "aead.h"
#include "kstring.h"
#include "kheap.h"

/* Known-answer tests from the standards documents, so a broken
 * primitive shows up as a named FAIL instead of a mysterious TLS
 * handshake error. */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t unhex(const char* s, uint8_t* out, uint32_t cap) {
    uint32_t n = 0;
    while (s[0] && s[1] && n < cap) {
        out[n++] = (uint8_t)(hexval(s[0]) * 16 + hexval(s[1]));
        s += 2;
    }
    return n;
}

static int check(const char* name, const uint8_t* got, const char* want_hex,
                 void (*report)(const char*, int)) {
    uint8_t want[128];
    uint32_t n = unhex(want_hex, want, sizeof(want));
    int ok = memcmp(got, want, n) == 0;
    report(name, ok);
    return ok;
}

int crypto_selftest(void (*report)(const char* name, int ok)) {
    int all = 1;
    uint8_t out[128];

    sha256("abc", 3, out);
    all &= check("SHA-256 (FIPS 180-4)", out,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", report);

    hmac_sha256((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, out);
    all &= check("HMAC-SHA256 (RFC 4231 #2)", out,
                 "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", report);

    {
        uint8_t ikm[22], salt[13], info[10], prk[32];
        memset(ikm, 0x0b, sizeof(ikm));
        for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
        for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
        hkdf_extract(salt, 13, ikm, 22, prk);
        hkdf_expand(prk, info, 10, out, 42);
        all &= check("HKDF-SHA256 (RFC 5869 #1)", out,
                     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
                     report);
    }

    {
        uint8_t k[32], u[32];
        unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
        unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
        x25519(out, k, u);
        all &= check("X25519 (RFC 7748 5.2)", out,
                     "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", report);
        unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", k, 32);
        x25519_base(out, k);
        all &= check("X25519 base point (RFC 7748 6.1)", out,
                     "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", report);
    }

    {
        aes128_t a;
        uint8_t key[16], pt[16];
        unhex("000102030405060708090a0b0c0d0e0f", key, 16);
        unhex("00112233445566778899aabbccddeeff", pt, 16);
        aes128_init(&a, key);
        aes128_encrypt(&a, pt, out);
        all &= check("AES-128 (FIPS 197 C.1)", out, "69c4e0d86a7b0430d8cdb78070b4c55a", report);
    }

    {
        aes_gcm_t* g = (aes_gcm_t*)kmalloc(sizeof(aes_gcm_t));
        uint8_t key[16], iv[12], aad[20], buf[60], tag[16];
        unhex("feffe9928665731c6d6a8f9467308308", key, 16);
        unhex("cafebabefacedbaddecaf888", iv, 12);
        unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
        unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
              "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", buf, 60);
        aes_gcm_init(g, key);
        aes_gcm_seal(g, iv, aad, 20, buf, 60, tag);
        all &= check("AES-128-GCM ciphertext (GCM spec #4)", buf,
                     "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e", report);
        all &= check("AES-128-GCM tag (GCM spec #4)", tag, "5bc94fbc3221a5db94fae95ae7121a47", report);
        int opened = aes_gcm_open(g, iv, aad, 20, buf, 60, tag) == 0 && buf[0] == 0xd9;
        report("AES-128-GCM decrypt + verify", opened);
        all &= opened;
        kfree(g);
    }

    {
        static const char pt[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                 "only one tip for the future, sunscreen would be it.";
        uint8_t key[32], nonce[12], aad[12], buf[114], tag[16];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
        unhex("070000004041424344454647", nonce, 12);
        unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
        memcpy(buf, pt, 114);
        chacha20poly1305_seal(key, nonce, aad, 12, buf, 114, tag);
        all &= check("ChaCha20 ciphertext (RFC 8439 2.8.2)", buf,
                     "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6", report);
        all &= check("Poly1305 tag (RFC 8439 2.8.2)", tag, "1ae10b594f09e26a7e902ecbd0600691", report);
        int opened = chacha20poly1305_open(key, nonce, aad, 12, buf, 114, tag) == 0 &&
                     memcmp(buf, pt, 114) == 0;
        report("ChaCha20-Poly1305 decrypt + verify", opened);
        all &= opened;
    }
    return all;
}
