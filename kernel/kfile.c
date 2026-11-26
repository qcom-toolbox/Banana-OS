/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kfile.h"
#include "kheap.h"
#include "kstring.h"
#include "fs.h"
#include "usock.h"
#include "task.h"
#include "spinlock.h"
#include "pty.h"

/* one direction of a pipe or socket pair: a ring of KPIPE_SIZE bytes */
struct kpipe {
    uint8_t* buf;                  /* made on the first write */
    uint32_t head, len;
    int      readers, writers;     /* descriptions reading / writing it */
};

static spinlock_t g_lock;
static volatile uint32_t g_gen;    /* bumped by every change: waiters look again */
static volatile int g_open;

#define WAITERS 64
static volatile int g_waiter[WAITERS];   /* tasks in kpipe_wait (pid + 1) */

static void changed(void) {
    g_gen++;
    for (int i = 0; i < WAITERS; i++) {
        int w = g_waiter[i];
        if (w) task_wake(w - 1);
    }
}

uint32_t kpipe_generation(void) { return g_gen; }
void kfile_changed(void) { changed(); }

static void (*g_pty_hook)(struct pty* t);
void kfile_set_pty_hook(void (*fn)(struct pty* t)) { g_pty_hook = fn; }

kfile_t* kfile_new(int type, int flags) {
    kfile_t* f = (kfile_t*)kmalloc(sizeof(kfile_t));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->type = type;
    f->refs = 1;
    f->flags = flags;
    f->fidx = -1;
    __sync_fetch_and_add(&g_open, 1);
    return f;
}

kfile_t* kfile_ref(kfile_t* f) {
    if (f) __sync_fetch_and_add(&f->refs, 1);
    return f;
}

int kfile_count(void) { return g_open; }

static kpipe_t* pipe_new(void) {
    kpipe_t* p = (kpipe_t*)kmalloc(sizeof(kpipe_t));
    if (p) memset(p, 0, sizeof(*p));
    return p;
}

/* an end lets go of its directions (lock held); a direction nobody holds goes */
static void drop_in(kfile_t* f) {
    kpipe_t* p = f->in;
    if (!p) return;
    f->in = NULL;
    if (--p->readers <= 0 && p->writers <= 0) { kfree(p->buf); kfree(p); }
}
static void drop_out(kfile_t* f) {
    kpipe_t* p = f->out;
    if (!p) return;
    f->out = NULL;
    if (--p->writers <= 0 && p->readers <= 0) { kfree(p->buf); kfree(p); }
}

void kfile_put(kfile_t* f) {
    if (!f) return;
    if (__sync_sub_and_fetch(&f->refs, 1) > 0) return;
    switch (f->type) {
    case KF_FILE:
        if (f->fidx >= 0) fs_unpin(f->fidx);
        break;
    case KF_SOCK:
        if (f->sock) usock_close(f->sock);
        break;
    case KF_PTM:
        if (!pty_master_ref(f->pty, -1) && g_pty_hook) g_pty_hook(f->pty);   /* (the last one: a hangup) */
        break;
    case KF_PTS:
        pty_slave_ref(f->pty, -1);
        break;
    case KF_DIR:
        kfree(f->path);
        break;
    case KF_MEM:
        kfree(f->mem);
        break;
    case KF_PIPE: {
        uintptr_t fl = spin_lock_irq(&g_lock);
        drop_in(f);
        drop_out(f);
        spin_unlock_irq(&g_lock, fl);
        changed();
        break;
    }
    }
    __sync_fetch_and_sub(&g_open, 1);
    kfree(f);
}

static int make_pair(kfile_t** a, kfile_t** b, int sock, int flags) {
    int nb = flags & KF_NONBLOCK;
    kfile_t* x = kfile_new(KF_PIPE, (sock ? KF_READ | KF_WRITE : KF_READ) | nb);
    kfile_t* y = kfile_new(KF_PIPE, (sock ? KF_READ | KF_WRITE : KF_WRITE) | nb);
    kpipe_t* p = pipe_new();
    kpipe_t* q = sock ? pipe_new() : NULL;
    if (!x || !y || !p || (sock && !q)) {
        kfree(x); kfree(y); kfree(p); kfree(q);
        if (x) __sync_fetch_and_sub(&g_open, 1);
        if (y) __sync_fetch_and_sub(&g_open, 1);
        return -12;                                 /* ENOMEM */
    }
    x->is_sock = y->is_sock = sock;
    x->in = p;  y->out = p;                         /* pipe: y writes, x reads */
    p->readers = p->writers = 1;
    if (sock) { y->in = q; x->out = q; q->readers = q->writers = 1; }
    *a = x;
    *b = y;
    return 0;
}

int kfile_pipe(kfile_t** r, kfile_t** w, int flags) { return make_pair(r, w, 0, flags); }
int kfile_socketpair(kfile_t** a, kfile_t** b, int flags) { return make_pair(a, b, 1, flags); }

long kpipe_read(kfile_t* f, void* buf, unsigned long n) {
    if (!f || f->type != KF_PIPE) return -9;           /* EBADF */
    uintptr_t fl = spin_lock_irq(&g_lock);
    kpipe_t* p = f->in;
    if (!p) { spin_unlock_irq(&g_lock, fl); return f->is_sock ? 0 : -9; }   /* (shut down: the end) */
    if (!p->len) {
        int writers = p->writers;
        spin_unlock_irq(&g_lock, fl);
        return writers ? (n ? -11 : 0) : 0;
    }
    uint32_t take = p->len < n ? p->len : (uint32_t)n;
    uint32_t first = KPIPE_SIZE - p->head;
    if (first > take) first = take;
    memcpy(buf, p->buf + p->head, first);
    if (take > first) memcpy((uint8_t*)buf + first, p->buf, take - first);
    p->head = (p->head + take) % KPIPE_SIZE;
    p->len -= take;
    spin_unlock_irq(&g_lock, fl);
    changed();
    return (long)take;
}

long kpipe_write(kfile_t* f, const void* buf, unsigned long n) {
    if (!f || f->type != KF_PIPE) return -9;
    if (!n) return 0;
    uintptr_t fl = spin_lock_irq(&g_lock);
    kpipe_t* p = f->out;
    if (!p) { spin_unlock_irq(&g_lock, fl); return f->is_sock ? -32 : -9; }
    if (p->readers <= 0) { spin_unlock_irq(&g_lock, fl); return -32; }     /* EPIPE */
    if (!p->buf) {
        spin_unlock_irq(&g_lock, fl);
        uint8_t* b = (uint8_t*)kmalloc(KPIPE_SIZE);
        if (!b) return -12;
        fl = spin_lock_irq(&g_lock);
        if (f->out != p) { spin_unlock_irq(&g_lock, fl); kfree(b); return -32; }
        if (!p->buf) p->buf = b;
        else kfree(b);
    }
    uint32_t room = KPIPE_SIZE - p->len;
    if (!room || (n <= KPIPE_ATOMIC && room < n)) { spin_unlock_irq(&g_lock, fl); return -11; }
    uint32_t put = room < n ? room : (uint32_t)n;
    uint32_t at = (p->head + p->len) % KPIPE_SIZE;
    uint32_t first = KPIPE_SIZE - at;
    if (first > put) first = put;
    memcpy(p->buf + at, buf, first);
    if (put > first) memcpy(p->buf, (const uint8_t*)buf + first, put - first);
    p->len += put;
    spin_unlock_irq(&g_lock, fl);
    changed();
    return (long)put;
}

int kpipe_poll(kfile_t* f, int events) {
    if (!f || f->type != KF_PIPE) return KF_POLLNVAL;
    int r = 0;
    uintptr_t fl = spin_lock_irq(&g_lock);
    kpipe_t* in = f->in, * out = f->out;
    if (in) {
        if (in->len) r |= KF_POLLIN;
        else if (!in->writers) r |= KF_POLLIN | KF_POLLHUP;    /* (a read returns 0: the end) */
    } else if (f->is_sock) r |= KF_POLLIN | KF_POLLHUP;
    if (out) {
        if (out->readers <= 0) r |= KF_POLLERR | (f->is_sock ? KF_POLLHUP : 0);
        else if (KPIPE_SIZE - out->len >= KPIPE_ATOMIC) r |= KF_POLLOUT;
    } else if (f->is_sock) r |= KF_POLLOUT;      /* (shut down: a write fails at once) */
    spin_unlock_irq(&g_lock, fl);
    return r & (events | KF_POLLERR | KF_POLLHUP | KF_POLLNVAL);
}

int kpipe_nread(kfile_t* f) {
    if (!f || f->type != KF_PIPE) return -9;
    uintptr_t fl = spin_lock_irq(&g_lock);
    int n = f->in ? (int)f->in->len : 0;
    spin_unlock_irq(&g_lock, fl);
    return n;
}

int kpipe_shutdown(kfile_t* f, int how) {
    if (!f || f->type != KF_PIPE) return -9;
    if (!f->is_sock) return -88;                         /* ENOTSOCK */
    if (how < 0 || how > 2) return -22;
    uintptr_t fl = spin_lock_irq(&g_lock);
    if (how != 1) drop_in(f);
    if (how != 0) drop_out(f);
    spin_unlock_irq(&g_lock, fl);
    changed();
    return 0;
}

void kpipe_wait(kfile_t* f, int events, uint32_t ms) {
    uint32_t gen = g_gen;
    if (f && kpipe_poll(f, events)) return;
    kpipe_sleep(gen, ms);
}

void kpipe_sleep(uint32_t gen, uint32_t ms) {
    int me = task_current_pid() + 1, slot = -1;
    for (int i = 0; i < WAITERS && slot < 0; i++)
        if (__sync_bool_compare_and_swap(&g_waiter[i], 0, me)) slot = i;
    if (gen == g_gen) task_sleep_ms(slot >= 0 ? ms : (ms < 5 ? ms : 5));
    if (slot >= 0) g_waiter[slot] = 0;
}

/* ── selftest ──────────────────────────────────────────────────────── */

void kfile_tests(void (*report)(const char*, int, const char*)) {
#define CHECK(c) report("pipes: " #c, (c) != 0, NULL)
    int open0 = kfile_count();
    kfile_t* r, * w;
    CHECK(kfile_pipe(&r, &w, 0) == 0);
    char buf[64];
    CHECK(kpipe_read(r, buf, sizeof(buf)) == -11);          /* empty, a writer: wait */
    CHECK(kpipe_poll(w, KF_POLLOUT) == KF_POLLOUT);
    CHECK(kpipe_write(w, "hello", 5) == 5);
    CHECK(kpipe_poll(r, KF_POLLIN) == KF_POLLIN);
    CHECK(kpipe_nread(r) == 5);
    CHECK(kpipe_read(r, buf, 3) == 3 && !memcmp(buf, "hel", 3));
    CHECK(kpipe_write(r, "x", 1) == -9);                    /* the read end */
    /* fill it: then a small write waits, a big one moves what fits */
    static uint8_t big[KPIPE_SIZE];
    for (int i = 0; i < KPIPE_SIZE; i++) big[i] = (uint8_t)i;
    CHECK(kpipe_write(w, big, KPIPE_SIZE) == KPIPE_SIZE - 2);
    CHECK(kpipe_write(w, "y", 1) == -11);
    CHECK(kpipe_poll(w, KF_POLLOUT) == 0);
    CHECK(kpipe_read(r, buf, 2) == 2 && !memcmp(buf, "lo", 2));
    int ok = 1;
    for (uint32_t got = 0; got < KPIPE_SIZE - 2;) {
        long n = kpipe_read(r, buf, sizeof(buf));
        if (n <= 0) { ok = 0; break; }
        for (long i = 0; i < n; i++) if (buf[i] != (char)(uint8_t)(got + (uint32_t)i)) ok = 0;
        got += (uint32_t)n;
    }
    CHECK(ok);                                              /* across the ring's wrap */
    kfile_t* w2 = kfile_ref(w);                             /* (dup) */
    kfile_put(w);
    CHECK(kpipe_read(r, buf, 1) == -11);                    /* a writer is left */
    kfile_put(w2);
    CHECK(kpipe_read(r, buf, 1) == 0);                      /* none: the end */
    CHECK(kpipe_poll(r, KF_POLLIN) == (KF_POLLIN | KF_POLLHUP));
    kfile_put(r);
    /* no reader: EPIPE */
    CHECK(kfile_pipe(&r, &w, 0) == 0);
    kfile_put(r);
    CHECK(kpipe_write(w, "z", 1) == -32);
    CHECK(kpipe_poll(w, KF_POLLOUT) & KF_POLLERR);
    kfile_put(w);
    /* a socket pair: both ways, shutdown */
    kfile_t* a, * b;
    CHECK(kfile_socketpair(&a, &b, 0) == 0);
    CHECK(kpipe_write(a, "ab", 2) == 2 && kpipe_write(b, "ba", 2) == 2);
    CHECK(kpipe_read(b, buf, 8) == 2 && !memcmp(buf, "ab", 2));
    CHECK(kpipe_read(a, buf, 8) == 2 && !memcmp(buf, "ba", 2));
    CHECK(kpipe_shutdown(a, 1) == 0);
    CHECK(kpipe_read(b, buf, 8) == 0);                      /* a writes no more */
    CHECK(kpipe_write(a, "q", 1) == -32);
    CHECK(kpipe_write(b, "q", 1) == 1);                     /* the other way still works */
    kfile_put(a);
    kfile_put(b);
    CHECK(kfile_count() == open0);
#undef CHECK
}
