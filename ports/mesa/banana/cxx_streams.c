/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS: what libstdc++.a (r600's iostreams, rebuild.py) still asks of
 * glibc beyond ports/llvm/compat.c - its 64-bit-offset and checked names. */
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <sys/types.h>

size_t mbsnrtowcs(wchar_t* d, const char** src, size_t bytes, size_t n, mbstate_t* state);   /* (compat.c) */
size_t __mbsnrtowcs_chk(wchar_t* d, const char** s, size_t bytes, size_t n, mbstate_t* st, size_t cap) {
    if (d && n > cap) abort();
    return mbsnrtowcs(d, s, bytes, n, st);
}
int fseeko64(FILE* f, long long off, int whence) { return fseeko(f, (off_t)off, whence); }
long long ftello64(FILE* f) { return ftello(f); }
