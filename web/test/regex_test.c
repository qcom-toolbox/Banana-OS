/* Host test for web/regex.c: pattern, flags, subject -> expected match (or "-" for none) and group 1 */
#include <stdio.h>
#include <string.h>
#include "regex.h"

static const struct { const char *pat, *flags, *subj, *m0, *g1; } T[] = {
    { "abc", "", "xxabcxx", "abc", NULL },
    { "a.c", "", "a-c", "a-c", NULL },
    { "^ab", "", "xab", "-", NULL },
    { "b$", "", "ab", "b", NULL },
    { "\\d+", "", "abc 12345 x", "12345", NULL },
    { "[a-c]+", "", "xxabcabcz", "abcabc", NULL },
    { "[^a-c]+", "", "abcxyzabc", "xyz", NULL },
    { "(\\w+)@(\\w+)\\.com", "", "mail bob@example.com now", "bob@example.com", "bob" },
    { "a|bc|d", "", "xxbcx", "bc", NULL },
    { "(a|ab)(c|bcd)(d*)", "", "abcd", "abcd", "a" },
    { "a*?b", "", "aaab", "aaab", NULL },
    { "<.+?>", "", "<a><b>", "<a>", NULL },
    { "<.+>", "", "<a><b>", "<a><b>", NULL },
    { "x{2,3}", "", "xxxxx", "xxx", NULL },
    { "x{2,}", "", "xxxxx", "xxxxx", NULL },
    { "(\\w)\\1", "", "abccd", "cc", "c" },
    { "hello", "i", "Say HELLO", "HELLO", NULL },
    { "\\bcat\\b", "", "concat cat", "cat", NULL },
    { "foo(?=bar)", "", "foobaz foobar", "foo", NULL },
    { "foo(?!bar)", "", "foobar foobaz", "foo", NULL },
    { "^\\s+|\\s+$", "", "   trim", "   ", NULL },
    { "(?<year>\\d{4})-(\\d\\d)", "", "on 2026-10 ok", "2026-10", "2026" },
    { "^line2$", "m", "line1\nline2\nline3", "line2", NULL },
    { "(a+)+b", "", "aaaaaaaaaaaaaaaaaaaaaaac", "-", NULL },
    { "[\\w.-]+", "", "  my.file-name.txt ", "my.file-name.txt", NULL },
    { "([0-9a-f]{2})", "i", "#FFAA00", "FF", "FF" },
    { "(?:ab)+", "", "ababab!", "ababab", NULL },
    { "a{", "", "a{b", "a{", NULL },
    { "\\/", "", "a/b", "/", NULL },
    { "[\\]]", "", "x]y", "]", NULL },
    { "(\\d+)(?:px)?", "", "width: 12px", "12px", "12" },
};

int main(void) {
    static char mem[1 << 20];
    (void)mem;
    arena_t A;
    arena_init(&A, 0);
    int fails = 0;
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        const char* err = NULL;
        regex_t* re = rx_compile(&A, T[i].pat, (uint32_t)strlen(T[i].pat), T[i].flags, &err);
        if (!re) { printf("FAIL %u /%s/: compile error %s\n", i, T[i].pat, err); fails++; continue; }
        int caps[2 * (RX_MAX_GROUPS + 1)];
        int ok = rx_exec(re, T[i].subj, (uint32_t)strlen(T[i].subj), 0, 0, caps);
        char got[128] = "-", g1[128] = "";
        if (ok) {
            snprintf(got, sizeof(got), "%.*s", caps[1] - caps[0], T[i].subj + caps[0]);
            if (caps[2] >= 0) snprintf(g1, sizeof(g1), "%.*s", caps[3] - caps[2], T[i].subj + caps[2]);
        }
        int pass = strcmp(got, T[i].m0) == 0 && (!T[i].g1 || strcmp(g1, T[i].g1) == 0);
        if (!pass) { printf("FAIL %u /%s/%s on \"%s\": got \"%s\" g1 \"%s\", want \"%s\" g1 \"%s\"\n",
                            i, T[i].pat, T[i].flags, T[i].subj, got, g1, T[i].m0, T[i].g1 ? T[i].g1 : ""); fails++; }
    }
    printf("%d failures of %u\n", fails, (unsigned)(sizeof(T) / sizeof(T[0])));
    return fails != 0;
}
