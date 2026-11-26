/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KFILE_H
#define KFILE_H

#include "types.h"

/*
 * Open file descriptions (Unix's "open files"): what a process's file
 * descriptors point at. dup and fork make more descriptors for the same
 * description - they share its position and status flags - and it is
 * closed when the last one goes. kernel/app.c keeps each process's table
 * of descriptors (with their close-on-exec flag) and does the I/O on the
 * kinds below; this file has the descriptions and the kernel's pipes.
 *
 * Pipes and socket pairs live here, in the kernel, so the two ends can
 * be in different processes (a shell's pipelines): 64 KiB each way, a
 * write of up to PIPE_BUF (4096) bytes never mixed with another's.
 */

#define KF_FILE 1          /* a file of kernel/fs.c */
#define KF_SOCK 2          /* a socket (kernel/usock.c) */
#define KF_PIPE 3          /* an end of a pipe, or of a socket pair (both ways) */
#define KF_CONS 4          /* the text terminal the process was started on (kernel/app.c) */
#define KF_PTM  5          /* a pseudo-terminal's master (/dev/ptmx: kernel/pty.c) */
#define KF_PTS  6          /* ... its slave (/dev/pts/N, /dev/tty) */
#define KF_DEV  7          /* /dev/null, zero, full, random, urandom (dev: which) */
#define KF_DIR  8          /* a folder (open of one: its path - openat, fchdir, fdopendir) */
#define KF_MEM  9          /* a text made when opened: a /proc file (kernel/procfs.c) - mem, mem_len, pos */

#define KF_DEV_NULL   1
#define KF_DEV_ZERO   2
#define KF_DEV_FULL   3
#define KF_DEV_RANDOM 4

/* status flags (BANANA_O_READ / WRITE / APPEND are the same bits) */
#define KF_READ     0x01
#define KF_WRITE    0x02
#define KF_APPEND   0x10
#define KF_NONBLOCK 0x800  /* (Linux's O_NONBLOCK) */

/* poll events (Linux's numbers) */
#define KF_POLLIN   0x001
#define KF_POLLOUT  0x004
#define KF_POLLERR  0x008
#define KF_POLLHUP  0x010
#define KF_POLLNVAL 0x020

#define KPIPE_SIZE  65536
#define KPIPE_ATOMIC 4096

struct usock;
struct pty;
typedef struct kpipe kpipe_t;

typedef struct kfile {
    int           type;
    volatile int  refs;
    int           flags;           /* KF_READ ... KF_NONBLOCK */
    uint32_t      pos;             /* KF_FILE */
    int           fidx;            /* KF_FILE: kernel/fs.c's index, pinned while open */
    struct usock* sock;            /* KF_SOCK */
    kpipe_t*      in;              /* KF_PIPE: what reads take from */
    kpipe_t*      out;             /* ... and writes put in (NULL: a pipe's read end) */
    int           is_sock;         /* KF_PIPE: a socket pair's end (shutdown, S_IFSOCK) */
    struct pty*   pty;             /* KF_PTM, KF_PTS */
    int           dev;             /* KF_DEV: KF_DEV_* */
    char*         path;            /* KF_DIR: its absolute path (kmalloc'd) */
    char*         mem;             /* KF_MEM: its text (kmalloc'd) */
    uint32_t      mem_len;
} kfile_t;

kfile_t* kfile_new(int type, int flags);       /* one reference; NULL if out of memory */
kfile_t* kfile_ref(kfile_t* f);                /* another reference (f) */
void     kfile_put(kfile_t* f);                /* drops one; the last closes it */
/* who is told when a pty's master closed (kernel/app.c: SIGHUP to its session) */
void     kfile_set_pty_hook(void (*fn)(struct pty* t));
int      kfile_count(void);                    /* descriptions open (tests) */

/* a pipe: r reads what w writes; 0, or -errno */
int  kfile_pipe(kfile_t** r, kfile_t** w, int flags);
/* a connected pair of stream sockets (AF_UNIX socketpair); 0, or -errno */
int  kfile_socketpair(kfile_t** a, kfile_t** b, int flags);

/* I/O on a KF_PIPE. They never wait: bytes moved, 0 at the end (read: no
 * writer left and nothing there), -11 (EAGAIN) when it would have to wait,
 * -32 (EPIPE) writing with no reader left. A write either goes in whole
 * (n <= KPIPE_ATOMIC and room for it) or moves what fits. */
long kpipe_read(kfile_t* f, void* buf, unsigned long n);
long kpipe_write(kfile_t* f, const void* buf, unsigned long n);
int  kpipe_poll(kfile_t* f, int events);       /* KF_POLL* ready now */
int  kpipe_nread(kfile_t* f);                  /* FIONREAD */
int  kpipe_shutdown(kfile_t* f, int how);      /* socket pair: SHUT_RD 0, WR 1, RDWR 2 */
/* waits (up to ms) until f may be ready for events: the other end did
 * something, or the time is over. The caller checks again. */
void kpipe_wait(kfile_t* f, int events, uint32_t ms);
/* a pipe changed: whoever waits on it in kpipe_wait / poll wakes up */
uint32_t kpipe_generation(void);
/* something a waiter may want changed (a pty: kernel/pty.c) - wakes them all */
void kfile_changed(void);
/* sleeps up to ms unless a pipe changed since generation gen */
void kpipe_sleep(uint32_t gen, uint32_t ms);

void kfile_tests(void (*report)(const char*, int, const char*));

#endif
