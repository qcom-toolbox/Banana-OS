/* SPDX-License-Identifier: BSD-2-Clause */
/* What Rust's standard library (built for x86_64-unknown-linux-musl) needs
 * beyond the SDK's C library, for programs built by sdk/tools/banana-cargo.
 * They are built with panic=abort: a panic prints its message and aborts,
 * so the unwinder entry points below are never reached - they only satisfy
 * the precompiled std, which was built with unwinding tables. */
#include <stdlib.h>
#include <string.h>

int bcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }

/* the auxiliary vector: only the page size is known */
unsigned long getauxval(unsigned long type) { return type == 6 /* AT_PAGESZ */ ? 4096 : 0; }

void _Unwind_Resume(void* exception) { (void)exception; abort(); }
int _Unwind_Backtrace(void* fn, void* arg) { (void)fn; (void)arg; return 5; }   /* _URC_END_OF_STACK */
unsigned long _Unwind_GetIP(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetIPInfo(void* c, int* before) { (void)c; if (before) *before = 0; return 0; }
unsigned long _Unwind_GetCFA(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetTextRelBase(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetDataRelBase(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetRegionStart(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetLanguageSpecificData(void* c) { (void)c; return 0; }
unsigned long _Unwind_GetGR(void* c, int i) { (void)c; (void)i; return 0; }
void _Unwind_SetIP(void* c, unsigned long v) { (void)c; (void)v; }
void _Unwind_SetGR(void* c, int i, unsigned long v) { (void)c; (void)i; (void)v; }
void _Unwind_DeleteException(void* e) { (void)e; }
int _Unwind_RaiseException(void* e) { (void)e; abort(); }
void* _Unwind_FindEnclosingFunction(void* pc) { (void)pc; return 0; }
