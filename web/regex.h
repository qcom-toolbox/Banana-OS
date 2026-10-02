#ifndef REGEX_H
#define REGEX_H

#include "types.h"
#include "arena.h"

/*
 * Regular expressions for BananaScript (JavaScript syntax): literals and
 * escapes, . [] [^] \d \w \s \b (and their negations), ^ $, groups
 * (...) (?:...) (?<name>...), lookaheads (?=...) (?!...), alternation,
 * * + ? {n,m} (greedy and lazy), backreferences \1 \k<name>.
 * Flags: i (case-insensitive, ASCII), g/y (callers), m (multiline), s (dotAll).
 * Works on bytes (UTF-8 text passes through literally). A backtracking
 * matcher with a step budget, so a pathological pattern fails instead of
 * hanging the page.
 */

#define RX_MAX_GROUPS 20

typedef struct regex regex_t;

/* compiles in the arena; NULL with *err set on a syntax error */
regex_t* rx_compile(arena_t* A, const char* pattern, uint32_t len, const char* flags, const char** err);

/* searches s[start..n); on a match fills caps[2*i], caps[2*i+1] (start, end; -1 unset)
 * for groups 0..ngroups and returns 1 */
int rx_exec(regex_t* re, const char* s, uint32_t n, uint32_t start, int sticky, int* caps);

int rx_groups(const regex_t* re);                  /* capture groups (not counting 0) */
const char* rx_group_name(const regex_t* re, int i); /* (?<name>) of group i, or NULL */

#endif
