/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UVM_H
#define UVM_H

#include "types.h"
#include "uspace.h"
#include "umem.h"

/*
 * Memory an app maps itself (mmap, 64-bit kernel): a region of virtual
 * addresses of its own, UVM_BASE .. UVM_BASE + UVM_SIZE, outside the
 * identity map of kernel/uspace.c. A mapping only reserves addresses; a
 * page gets memory (zeroed, from the app's own blocks: kernel/umem.c) the
 * first time it is touched, by the app or by a system call working for it.
 * So a program can reserve a big range (a Java heap) and use a little of
 * it. Each range has its protection - none, read, read + write (execute
 * goes with read: the kernel does not turn on NX) - and touching a page
 * against it faults like any bad access.
 *
 * The 32-bit kernel has no paging: kernel/app.c hands out ordinary
 * (zeroed) app memory there, without reservations or protection.
 */

#define UVM_BASE  0x0000640000000000ull     /* PML4 slots 200..203: 100 TiB */
#define UVM_SLOTS 4
#define UVM_SIZE  ((uint64_t)UVM_SLOTS << 39)  /* 2 TiB */

#define UVM_READ   1
#define UVM_WRITE  2
#define UVM_EXEC   4
#define UVM_FIXED  1                         /* at addr exactly, replacing what is there */
#define UVM_DEV    8                         /* (kernel only) a range of device pages: uvm_map_frames */

/* maps len bytes (rounded up to pages) at addr (a hint, or exactly with
 * UVM_FIXED): the address, or 0 */
int uvm_limit(uspace_t* us, umem_t* budget, uint64_t bytes, int ranges);
uintptr_t uvm_map(uspace_t* us, umem_t* m, uintptr_t addr, uint64_t len, int prot, int flags);
/* unmaps [addr, addr + len): its pages go back to the app's memory; 0, -1 */
int       uvm_unmap(uspace_t* us, umem_t* m, uintptr_t addr, uint64_t len);
/* new protection for [addr, addr + len), which must be mapped: 0, -1 */
int       uvm_protect(uspace_t* us, uintptr_t addr, uint64_t len, int prot);
/* 1 if [p, p + n) is mapped and readable (a system call may use it) */
int       uvm_owns(uspace_t* us, const void* p, uint64_t n);
int       uvm_owns_str(uspace_t* us, const char* s);
/* n bytes of its memory at va, read from another task (prof): 0, or -1 if a page is not there */
int       uvm_peek(uspace_t* us, uintptr_t va, void* out, uint32_t n);
/* a page fault at addr (err: the CPU's error code): 1 if a page was put
 * there (or it was a stale translation) and the access can go on; 2 if the
 * page is in the swap area on the disk (*on_disk: its kernel/zswap.c entry,
 * to read in where the fault may wait - then this again); 3 if there was no
 * memory for the page (on_disk given: make some, then this again);
 * 4: hard restricted memory quota, 0 otherwise */
int       uvm_fault(uspace_t* us, umem_t* m, uintptr_t addr, uint32_t err, uint32_t* on_disk);
/* maps n given pages - not the app's memory: a GPU buffer's (kernel/gpu_gem.c)
 * - at a free place of the region: the address, or 0. They are never
 * compressed or swapped, and an unmap (or the app's end) only takes them
 * out of its tables; wc: write-combining. uvm_protect refuses them. */
uintptr_t uvm_map_frames(uspace_t* us, const uint64_t* frames, uint32_t n, int prot, int wc);
/* unmaps such a range if addr still shows frame0 there (the app may have
 * unmapped it and put something else there): 0, -1 */
int       uvm_unmap_frames(uspace_t* us, uintptr_t addr, uint32_t n, uint64_t frame0);
/* bytes of memory its mapped pages use now */
uint64_t  uvm_resident(uspace_t* us);
/* bytes of its pages compressed away now (kernel/zswap.c) */
uint64_t  uvm_compressed(uspace_t* us);
/* compression (kernel/zswap.c): up to max pages of every app's mmap region
 * compressed and given back - those not used since the last pass, or all
 * of them (force); how many */
uint32_t  uvm_page_out_all(int force, uint32_t max);
/* 1 if the app may write every byte of [a, a + n) right now - its pages
 * present and writable in its tables (mapped ones are faulted in first):
 * what the kernel checks before it writes there from a fault handler,
 * where a fault of its own must not happen (a signal frame) */
int       uvm_user_writable(uspace_t* us, umem_t* m, uintptr_t a, uint64_t n);

/* fork: to (a new space, nothing mapped) gets from's ranges, every page of
 * them shared copy-on-write (read-only in both until written: uvm_fault
 * copies it then). 0; -1 out of memory (to emptied again); -2: the page at
 * *zsw is compressed away - bring it in (a fault), then call this again */
int       uvm_fork(uspace_t* from, uspace_t* to, uintptr_t* zsw);
/* copies n bytes to va in space us, which need not be the running one (a
 * program loaded into a new space): its pages are given now. 0, or -1 (not
 * mapped, no memory) */
int       uvm_write(uspace_t* us, uintptr_t va, const void* src, uint64_t n);

#endif
