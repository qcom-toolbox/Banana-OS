#ifndef UTF8_H
#define UTF8_H

#include "types.h"

/*
 * Typed text is UTF-8 (the keyboard sends é as two bytes). Line editors
 * keep bytes but move, erase and draw whole characters: one character is
 * one column.
 */

static inline int u8_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* columns (characters) in the first n bytes */
static inline int u8_cols(const char* s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (!u8_cont(s[i])) c++;
    return c;
}

/* the byte where column col starts (len when past the end) */
static inline int u8_byte_at(const char* s, int len, int col) {
    int i = 0;
    while (i < len && col > 0) {
        i++;
        while (i < len && u8_cont(s[i])) i++;
        col--;
    }
    return i;
}

/* the start of the character before / after byte i */
static inline int u8_prev(const char* s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && u8_cont(s[i])) i--;
    return i;
}
static inline int u8_next(const char* s, int len, int i) {
    if (i >= len) return len;
    i++;
    while (i < len && u8_cont(s[i])) i++;
    return i;
}

/* the character at byte *i (advancing it); 0xFFFD for a broken one */
static inline uint32_t u8_decode(const char* s, int len, int* i) {
    unsigned char b = (unsigned char)s[*i];
    (*i)++;
    if (b < 0x80) return b;
    int need = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (!need) return 0xFFFD;
    uint32_t cp = b & (0x3F >> need);
    for (int k = 0; k < need; k++) {
        if (*i >= len || !u8_cont(s[*i])) return 0xFFFD;
        cp = cp << 6 | ((unsigned char)s[(*i)++] & 0x3F);
    }
    return cp;
}

/* a character as UTF-8 (out: 4 bytes); the byte count */
static inline int u8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* the 8x8 cell for a character: Latin-1 as one byte, anything else '?' */
static inline char u8_cell(uint32_t cp) {
    if (cp < 0x80) return (char)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (char)cp;
    return '?';
}

/* drops the last character of a NUL-terminated string of n bytes; the new length */
static inline int u8_backspace(char* s, int n) {
    n = u8_prev(s, n);
    s[n] = 0;
    return n;
}

#endif
