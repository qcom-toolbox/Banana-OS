/* SPDX-License-Identifier: BSD-2-Clause */
/* posixtest - checks the POSIX layer apps get from the SDK, for programs
 * ported from Linux (on the way to OpenJDK): one ok / FAIL line per check,
 * the exit code is how many failed. `selftest posix` runs it.
 *
 *   posixtest          every part
 *   posixtest mmap     mmap / munmap / mprotect
 *   posixtest threads  pthreads, __thread, semaphores, clocks
 *   posixtest sockets  TCP and UDP on the loopback, poll / select, names
 *   posixtest epoll    epoll, eventfd, pipes, socket pairs, dup2
 *   posixtest signals  handlers, faults as signals, masks, pthread_kill
 *   posixtest dlopen   shared libraries (libdemo, libdemo2 next to it)
 *   posixtest files    mkdir's errors (EEXIST, ENOENT, ENOTDIR)
 *   posixtest memory   posix_memalign with malloc / free, from several threads
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <link.h>
#include <banana.h>

static int g_ok, g_fail;

static void check(const char* what, int ok, const char* note) {
    printf("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, note && note[0] ? " - " : "", note ? note : "");
    if (ok) g_ok++; else g_fail++;
}

/* ── mmap ─────────────────────────────────────────────────────────── */

static int all_zero(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

#define TOUCH (4u << 20)
static char* g_area;

/* each thread fills its own 4 MiB of a fresh mapping, page by page */
static int toucher(void* arg) {
    long k = (long)arg;
    unsigned* p = (unsigned*)(g_area + (size_t)k * TOUCH);
    for (size_t i = 0; i < TOUCH / 4; i += 1024) p[i] = (unsigned)(k * 1000003u + i);
    for (size_t i = 0; i < TOUCH / 4; i += 1024) if (p[i] != (unsigned)(k * 1000003u + i)) return 1;
    return 0;
}

/* MAP_FIXED over a range of this program's while another thread maps: the
 * range must stay this program's all along. The kernel used to take it out
 * of the address space while it released the old pages, and an mmap in
 * another thread could be given it meanwhile (the heap gives its empty spans
 * back this way; with LLVMpipe's JIT mappings, Minecraft's memory got shared
 * and unmapped under it) */
#define RACE_LEN (16 * 4096)
static uint8_t* volatile g_race_base;
static volatile int g_race_stop;

static int race_remapper(void* arg) {
    (void)arg;
    for (int i = 0; i < 3000 && !g_race_stop; i++) {
        void* q = mmap(g_race_base, RACE_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (q != g_race_base) { g_race_stop = 2; break; }
        g_race_base[i % RACE_LEN] = 1;                   /* (a fresh page each time) */
    }
    if (!g_race_stop) g_race_stop = 1;
    return 0;
}

static void t_mmap_fixed_race(void) {
    g_race_stop = 0;
    g_race_base = mmap(NULL, RACE_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_race_base == MAP_FAILED) { check("MAP_FIXED while another thread maps", 0, "no memory"); return; }
    int id = banana_thread(race_remapper, NULL);
    long asked = 0, stolen = 0;
    while (id >= 0 && !g_race_stop) {
        /* the range as a hint: granted only if it is free - it never is */
        for (int k = 0; k < 16; k++) {
            uint8_t* want = g_race_base + (size_t)k * 4096;
            uint8_t* q = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            asked++;
            if (q == MAP_FAILED) continue;
            if (q >= g_race_base && q < g_race_base + RACE_LEN) stolen++;
            else munmap(q, 4096);
        }
    }
    if (id >= 0) banana_join(id);
    char note[64];
    snprintf(note, sizeof(note), "%ld mappings asked for meanwhile, %ld inside it", asked, stolen);
    check("MAP_FIXED while another thread maps: the range is never given away", id >= 0 && g_race_stop == 1 && stolen == 0, note);
    munmap(g_race_base, RACE_LEN);
}

static void t_mmap(void) {
    printf("mmap / munmap / mprotect\n");
    int big = sizeof(void*) == 8;
    size_t reserve = big ? (1ul << 30) : (16ul << 20);
    char note[96];
    char* r = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    snprintf(note, sizeof(note), "%lu MiB at %p", (unsigned long)(reserve >> 20), (void*)r);
    check("reserve addresses (PROT_NONE)", r != MAP_FAILED, note);
    if (r == MAP_FAILED) return;

    size_t use = 8u << 20;
    check("mprotect 8 MiB of it read / write", mprotect(r, use, PROT_READ | PROT_WRITE) == 0, "");
    check("its pages read as zeros", all_zero(r, use), "");
    for (size_t i = 0; i < use; i += 4096) *(unsigned*)(r + i) = (unsigned)i * 2654435761u;
    int same = 1;
    for (size_t i = 0; i < use; i += 4096) if (*(unsigned*)(r + i) != (unsigned)i * 2654435761u) same = 0;
    check("written and read back, every page", same, "");

    /* a hole punched and mapped again (MAP_FIXED): fresh zero pages */
    char* mid = r + (2u << 20);
    memset(mid, 0xAB, 1u << 20);
    check("munmap 1 MiB in the middle", munmap(mid, 1u << 20) == 0, "");
    char* again = mmap(mid, 1u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    check("MAP_FIXED maps it again, zeroed", again == mid && all_zero(mid, 1u << 20), "");
    check("the pages around it kept their data", *(unsigned*)(r + (1u << 20)) == (1u << 20) * 2654435761u &&
          *(unsigned*)(r + (3u << 20)) == (3u << 20) * 2654435761u, "");

    if (big) {
        /* the far end of the reservation: only what is touched gets memory */
        char* end = r + reserve - (1u << 20);
        int ok = mprotect(end, 1u << 20, PROT_READ | PROT_WRITE) == 0;
        if (ok) { end[(1u << 20) - 1] = 42; ok = end[(1u << 20) - 1] == 42; }
        check("the last page of 1 GiB, touched alone", ok, "");
    }

    /* system calls given mapped memory: a file written from it, read back into it */
    int fd = open("/tmp/posixtest.dat", O_CREAT | O_TRUNC | O_RDWR);
    int io = fd >= 0 && write(fd, r, 100000) == 100000 && lseek(fd, 0, SEEK_SET) == 0;
    char* back = r + (5u << 20);
    io = io && read(fd, back, 100000) == 100000 && memcmp(back, r, 100000) == 0;
    check("write() and read() with a mapped buffer", io, "");

    /* a file mapped: its bytes, then zeros past its end */
    char* fm = fd >= 0 ? mmap(NULL, 200000, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    check("mmap a file", fm != MAP_FAILED && memcmp(fm, r, 100000) == 0 && all_zero(fm + 100000, 100000), "");
    if (fm != MAP_FAILED) munmap(fm, 200000);
    if (fd >= 0) close(fd);
    unlink("/tmp/posixtest.dat");

    /* several threads faulting pages in at once (on every core) */
    int n = banana_cpus() < 2 ? 2 : banana_cpus() > 8 ? 8 : banana_cpus();
    g_area = mmap(NULL, (size_t)n * TOUCH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ids[8], bad = g_area == MAP_FAILED;
    for (int k = 0; k < n && !bad; k++) ids[k] = banana_thread(toucher, (void*)(long)k);
    for (int k = 0; k < n && !bad; k++) if (ids[k] < 0 || banana_join(ids[k]) != 0) bad = 1;
    snprintf(note, sizeof(note), "%d threads, %d MiB", n, n * (int)(TOUCH >> 20));
    check("threads touching new pages at once", !bad, note);
    if (g_area != MAP_FAILED) munmap(g_area, (size_t)n * TOUCH);

    check("munmap the whole reservation", munmap(r, reserve) == 0, "");

    t_mmap_fixed_race();
}

/* ── threads ──────────────────────────────────────────────────────── */

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void* ret_arg(void* a) { return (void*)((long)a * 3); }

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static long g_count;
#define ADDS 20000

static void* adder(void* a) {
    (void)a;
    for (int i = 0; i < ADDS; i++) {
        pthread_mutex_lock(&g_m);
        g_count++;
        pthread_mutex_unlock(&g_m);
    }
    return NULL;
}

/* a bounded queue: a producer, a consumer, two condition variables */
#define QN 8
static int g_q[QN], g_qh, g_qt, g_qn;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_notfull = PTHREAD_COND_INITIALIZER, g_notempty = PTHREAD_COND_INITIALIZER;
#define ITEMS 2000

static void* producer(void* a) {
    (void)a;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&g_qm);
        while (g_qn == QN) pthread_cond_wait(&g_notfull, &g_qm);
        g_q[g_qt] = i; g_qt = (g_qt + 1) % QN; g_qn++;
        pthread_cond_signal(&g_notempty);
        pthread_mutex_unlock(&g_qm);
    }
    return NULL;
}

static void* consumer(void* a) {
    (void)a;
    long sum = 0;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&g_qm);
        while (!g_qn) pthread_cond_wait(&g_notempty, &g_qm);
        sum += g_q[g_qh]; g_qh = (g_qh + 1) % QN; g_qn--;
        pthread_cond_signal(&g_notfull);
        pthread_mutex_unlock(&g_qm);
    }
    return (void*)sum;
}

#ifdef __x86_64__
static __thread int t_var = 42;          /* (an initial value: the TLS image) */
static __thread char t_big[3000];        /* (zeroed: .tbss) */
static void* tls_user(void* a) {
    long k = (long)a;
    if (t_var != 42 || t_big[2999]) return (void*)1;
    t_var = (int)k * 7;
    t_big[2999] = (char)k;
    for (int i = 0; i < 2000; i++) { if (i % 100 == 0) sched_yield(); if (t_var != (int)k * 7) return (void*)2; }
    return t_big[2999] == (char)k ? NULL : (void*)3;
}
#endif

static pthread_key_t g_key;
static volatile int g_dtors;
static void key_dtor(void* v) { if (v == (void*)0x1234) __sync_fetch_and_add(&g_dtors, 1); }
static void* key_user(void* a) {
    (void)a;
    if (pthread_getspecific(g_key)) return (void*)1;
    pthread_setspecific(g_key, (void*)0x1234);
    return pthread_getspecific(g_key) == (void*)0x1234 ? NULL : (void*)2;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int g_once_runs;
static void once_fn(void) { __sync_fetch_and_add(&g_once_runs, 1); banana_sleep(20); }
static void* once_user(void* a) { (void)a; pthread_once(&g_once, once_fn); return g_once_runs == 1 ? NULL : (void*)1; }

static volatile int g_detached_done;
static void* detached_fn(void* a) { (void)a; __sync_fetch_and_add(&g_detached_done, 1); return NULL; }

static void* deep_stack(void* a) {
    (void)a;
    volatile char big[3u << 20];                 /* 3 MiB of locals: needs the 4 MiB stack */
    big[0] = 1; big[sizeof(big) - 1] = 2;
    for (size_t i = 0; i < sizeof(big); i += 4096) big[i] = (char)i;
    pthread_attr_t at;
    size_t sz = 0;
    void* lo = NULL;
    if (pthread_getattr_np(pthread_self(), &at) == 0) pthread_attr_getstack(&at, &lo, &sz);
    char* here = (char*)&at;
    return sz >= (4u << 20) && here >= (char*)lo && here < (char*)lo + sz ? NULL : (void*)1;
}

static sem_t g_sem;
static void* sem_poster(void* a) { (void)a; for (int i = 0; i < 50; i++) { banana_sleep(1); sem_post(&g_sem); } return NULL; }

static pthread_barrier_t g_bar;
static volatile int g_bar_phase[4];
static void* bar_user(void* a) {
    long k = (long)a;
    for (int r = 0; r < 3; r++) {
        g_bar_phase[k] = r;
        pthread_barrier_wait(&g_bar);
        for (int j = 0; j < 4; j++) if (g_bar_phase[j] < r) return (void*)1;
        pthread_barrier_wait(&g_bar);
    }
    return NULL;
}

static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile int g_readers_in, g_rw_bad;
static int g_shared[2];
static void* rw_reader(void* a) {
    (void)a;
    for (int i = 0; i < 300; i++) {
        pthread_rwlock_rdlock(&g_rw);
        __sync_fetch_and_add(&g_readers_in, 1);
        if (g_shared[0] != g_shared[1]) g_rw_bad = 1;
        __sync_fetch_and_add(&g_readers_in, -1);
        pthread_rwlock_unlock(&g_rw);
    }
    return NULL;
}
static void* rw_writer(void* a) {
    (void)a;
    for (int i = 0; i < 300; i++) {
        pthread_rwlock_wrlock(&g_rw);
        if (g_readers_in) g_rw_bad = 1;
        g_shared[0]++; sched_yield(); g_shared[1]++;
        pthread_rwlock_unlock(&g_rw);
    }
    return NULL;
}

static void* errno_user(void* a) { (void)a; errno = EIO; sched_yield(); return errno == EIO ? NULL : (void*)1; }

static void t_threads(void) {
    printf("pthreads, __thread, semaphores, clocks\n");
    char note[96];
    pthread_t th[8];
    void* r;
    int ok = 1;
    for (long i = 0; i < 8; i++) if (pthread_create(&th[i], NULL, ret_arg, (void*)i)) ok = 0;
    for (long i = 0; i < 8 && ok; i++) if (pthread_join(th[i], &r) || r != (void*)(i * 3)) ok = 0;
    check("pthread_create / pthread_join, 8 threads and their results", ok, "");

    int n = banana_cpus() < 2 ? 4 : banana_cpus() * 2 > 8 ? 8 : banana_cpus() * 2;
    g_count = 0;
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, adder, NULL);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    snprintf(note, sizeof(note), "%d threads x %d: %ld", n, ADDS, g_count);
    check("a mutex: no increment lost", g_count == (long)n * ADDS, note);

    pthread_mutexattr_t ma;
    pthread_mutex_t rm, em;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rm, &ma);
    ok = pthread_mutex_lock(&rm) == 0 && pthread_mutex_lock(&rm) == 0 && pthread_mutex_unlock(&rm) == 0 &&
         pthread_mutex_unlock(&rm) == 0 && pthread_mutex_trylock(&rm) == 0 && pthread_mutex_unlock(&rm) == 0;
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&em, &ma);
    ok = ok && pthread_mutex_lock(&em) == 0 && pthread_mutex_lock(&em) == EDEADLK && pthread_mutex_unlock(&em) == 0 &&
         pthread_mutex_unlock(&em) == EPERM;
    check("recursive and error-checking mutexes", ok, "");

    pthread_create(&th[0], NULL, producer, NULL);
    pthread_create(&th[1], NULL, consumer, NULL);
    pthread_join(th[0], NULL);
    pthread_join(th[1], &r);
    snprintf(note, sizeof(note), "%d items through 8 slots", ITEMS);
    check("condition variables: producer and consumer", (long)r == (long)ITEMS * (ITEMS + 1) / 2, note);

    pthread_condattr_t ca;
    pthread_cond_t cv;
    pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += 150 * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    long t0 = ms_now();
    pthread_mutex_lock(&cm);
    int rc = pthread_cond_timedwait(&cv, &cm, &until);
    pthread_mutex_unlock(&cm);
    long took = ms_now() - t0;
    snprintf(note, sizeof(note), "%ld ms for 150", took);
    check("pthread_cond_timedwait times out (CLOCK_MONOTONIC)", rc == ETIMEDOUT && took >= 140 && took < 1000, note);

#ifdef __x86_64__
    ok = t_var == 42 && !t_big[2999];
    for (long i = 0; i < 6; i++) pthread_create(&th[i], NULL, tls_user, (void*)(i + 1));
    for (int i = 0; i < 6; i++) { pthread_join(th[i], &r); if (r) ok = 0; }
    check("__thread variables: each thread its own, initial values", ok && t_var == 42, "");
#else
    printf("  skip  __thread variables - the 32-bit system has no thread pointer\n");
#endif

    pthread_key_create(&g_key, key_dtor);
    g_dtors = 0;
    ok = 1;
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, key_user, NULL);
    for (int i = 0; i < 4; i++) { pthread_join(th[i], &r); if (r) ok = 0; }
    check("keys: a value per thread, destructors at exit", ok && g_dtors == 4 && !pthread_getspecific(g_key), "");

    ok = 1;
    for (int i = 0; i < 6; i++) pthread_create(&th[i], NULL, once_user, NULL);
    for (int i = 0; i < 6; i++) { pthread_join(th[i], &r); if (r) ok = 0; }
    check("pthread_once runs once for 6 threads", ok && g_once_runs == 1, "");

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_detached_done = 0;
    for (int made = 0, tries = 0; made < 150 && tries < 1000; tries++) {
        pthread_t d;
        if (pthread_create(&d, &at, detached_fn, NULL) == 0) made++;
        else banana_sleep(5);                       /* (every slot busy: wait for some to end) */
    }
    long until_ms = ms_now() + 10000;
    while (g_detached_done < 150 && ms_now() < until_ms) banana_sleep(5);
    snprintf(note, sizeof(note), "%d of 150 ran (more than the 64 threads at once)", g_detached_done);
    check("detached threads: their slots come back", g_detached_done == 150, note);

    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    r = (void*)1;
    if (pthread_create(&th[0], &at, deep_stack, NULL) == 0) pthread_join(th[0], &r);
    check("a 4 MiB stack (pthread_attr_setstacksize), 3 MiB of locals", r == NULL, "");

    sem_init(&g_sem, 0, 0);
    pthread_create(&th[0], NULL, sem_poster, NULL);
    ok = 1;
    for (int i = 0; i < 50; i++) if (sem_wait(&g_sem)) ok = 0;
    pthread_join(th[0], NULL);
    int v = -1;
    sem_getvalue(&g_sem, &v);
    check("semaphores: 50 posts, 50 waits", ok && v == 0 && sem_trywait(&g_sem) == -1 && errno == EAGAIN, "");

    pthread_barrier_init(&g_bar, NULL, 4);
    ok = 1;
    for (long i = 0; i < 4; i++) pthread_create(&th[i], NULL, bar_user, (void*)i);
    for (int i = 0; i < 4; i++) { pthread_join(th[i], &r); if (r) ok = 0; }
    check("a barrier: 4 threads, 3 rounds", ok, "");

    g_rw_bad = 0;
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, rw_reader, NULL);
    pthread_create(&th[3], NULL, rw_writer, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    check("a read-write lock: readers never see a half write", !g_rw_bad && g_shared[0] == 300, "");

    errno = 0;
    pthread_create(&th[0], NULL, errno_user, NULL);
    pthread_join(th[0], &r);
    check("errno is per thread", r == NULL && errno == 0, "");

    t0 = ms_now();
    struct timespec nap = { 0, 60 * 1000000L };
    nanosleep(&nap, NULL);
    took = ms_now() - t0;
    snprintf(note, sizeof(note), "%ld ms for 60", took);
    check("nanosleep and CLOCK_MONOTONIC", took >= 55 && took < 1000, note);

    long cpus = sysconf(_SC_NPROCESSORS_ONLN), pages = sysconf(_SC_PHYS_PAGES);
    cpu_set_t set;
    sched_getaffinity(0, sizeof(set), &set);
    snprintf(note, sizeof(note), "%ld cores, %ld MiB, pid %d", cpus, pages / 256, (int)getpid());
    check("sysconf, sched_getaffinity, getpid", cpus >= 1 && pages > 1000 && CPU_COUNT(&set) == cpus && getpid() > 0, note);
}

/* ── sockets ──────────────────────────────────────────────────────── */

#define BULK 100000
static int g_lfd;
static volatile int g_echoed;

/* the server: accepts one connection and sends back all it gets */
static void* echo_server(void* a) {
    (void)a;
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    int c = accept(g_lfd, (struct sockaddr*)&peer, &pl);
    if (c < 0) return (void*)1;
    if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { close(c); return (void*)2; }
    char buf[4096];
    for (;;) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t o = 0; o < n; ) { ssize_t w = send(c, buf + o, (size_t)(n - o), 0); if (w <= 0) { close(c); return (void*)3; } o += w; }
        g_echoed += (int)n;
    }
    close(c);                                        /* the client sees the end */
    return NULL;
}

static void t_sockets(void) {
    printf("sockets: TCP and UDP on the loopback, poll / select, names\n");
    char note[96];
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;                                 /* any free port */

    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    int ok = g_lfd >= 0 && setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
             bind(g_lfd, (struct sockaddr*)&sa, sizeof(sa)) == 0 && listen(g_lfd, 8) == 0;
    socklen_t sl = sizeof(sa);
    ok = ok && getsockname(g_lfd, (struct sockaddr*)&sa, &sl) == 0 && ntohs(sa.sin_port) != 0;
    snprintf(note, sizeof(note), "port %u", ntohs(sa.sin_port));
    check("socket, bind (port 0), listen, getsockname", ok, note);
    if (!ok) return;

    pthread_t srv;
    pthread_create(&srv, NULL, echo_server, NULL);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = c >= 0 && connect(c, (struct sockaddr*)&sa, sizeof(sa)) == 0;
    check("connect to it on 127.0.0.1", ok, "");
    char* out = malloc(BULK);
    char* in = malloc(BULK);
    for (int i = 0; i < BULK; i++) out[i] = (char)(i * 7 + (i >> 9));
    /* sending and receiving at once (the echo would fill the buffers otherwise) */
    size_t sent = 0, got = 0;
    long until = ms_now() + 20000;
    while ((sent < BULK || got < BULK) && ms_now() < until && ok) {
        struct pollfd pf = { c, (short)(POLLIN | (sent < BULK ? POLLOUT : 0)), 0 };
        if (poll(&pf, 1, 1000) < 0) { ok = 0; break; }
        if ((pf.revents & POLLOUT) && sent < BULK) {
            ssize_t w = send(c, out + sent, BULK - sent > 8192 ? 8192 : BULK - sent, MSG_DONTWAIT);
            if (w > 0) sent += (size_t)w;
            else if (w < 0 && errno != EAGAIN) ok = 0;
        }
        if (pf.revents & POLLIN) {
            ssize_t r = recv(c, in + got, BULK - got, MSG_DONTWAIT);
            if (r > 0) got += (size_t)r;
            else if (r == 0 || errno != EAGAIN) ok = 0;
        }
    }
    snprintf(note, sizeof(note), "%lu bytes there and back", (unsigned long)got);
    check("send / recv / poll: an echo through the loopback", ok && got == BULK && !memcmp(in, out, BULK), note);

    char b[16];
    ok = write(c, "hello", 5) == 5 && read(c, b, 5) == 5 && !memcmp(b, "hello", 5);
    check("write() and read() on a socket", ok, "");

    ssize_t r = recv(c, b, sizeof(b), MSG_DONTWAIT);
    int e = errno;
    struct pollfd pf = { c, POLLIN, 0 };
    long t0 = ms_now();
    int pr = poll(&pf, 1, 120);
    long took = ms_now() - t0;
    snprintf(note, sizeof(note), "poll waited %ld ms for 120", took);
    check("nothing to read: EAGAIN, and poll times out", r == -1 && e == EAGAIN && pr == 0 && took >= 100, note);

    struct timeval tv = { 0, 200000 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    t0 = ms_now();
    r = recv(c, b, sizeof(b), 0);
    e = errno;
    took = ms_now() - t0;
    snprintf(note, sizeof(note), "%ld ms for 200", took);
    check("SO_RCVTIMEO: a blocking recv gives up", r == -1 && e == EAGAIN && took >= 180 && took < 2000, note);

    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(c, &rs);
    struct timeval zero = { 0, 0 };
    int sel = select(c + 1, &rs, NULL, NULL, &zero);
    write(c, "x", 1);
    banana_sleep(50);
    FD_ZERO(&rs);
    FD_SET(c, &rs);
    struct timeval wait1 = { 1, 0 };
    int sel2 = select(c + 1, &rs, NULL, NULL, &wait1);
    int avail = 0;
    ioctl(c, FIONREAD, &avail);
    check("select and FIONREAD", sel == 0 && sel2 == 1 && FD_ISSET(c, &rs) && avail == 1 && read(c, b, 1) == 1, "");

    shutdown(c, SHUT_WR);
    close(c);
    void* sr = (void*)1;
    pthread_join(srv, &sr);
    check("the server saw the end, and closed", sr == NULL, "");
    close(g_lfd);

    /* a closed port: refused */
    int c2 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in nowhere = sa;
    nowhere.sin_port = htons(1);
    r = connect(c2, (struct sockaddr*)&nowhere, sizeof(nowhere));
    e = errno;
    close(c2);
    check("connect to a closed port: ECONNREFUSED", r == -1 && e == ECONNREFUSED, "");

    /* UDP: two sockets on the loopback */
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a1 = { 0 }, a2;
    a1.sin_family = AF_INET;
    a1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sl = sizeof(a1);
    ok = u1 >= 0 && u2 >= 0 && bind(u1, (struct sockaddr*)&a1, sizeof(a1)) == 0 && getsockname(u1, (struct sockaddr*)&a1, &sl) == 0;
    a1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = ok && sendto(u2, "datagram!", 9, 0, (struct sockaddr*)&a1, sizeof(a1)) == 9;
    socklen_t l2 = sizeof(a2);
    char d[32] = { 0 };
    ok = ok && recvfrom(u1, d, sizeof(d), MSG_PEEK, NULL, NULL) == 9 &&
         recvfrom(u1, d, sizeof(d), 0, (struct sockaddr*)&a2, &l2) == 9 && !memcmp(d, "datagram!", 9);
    ok = ok && sendto(u1, "back", 4, 0, (struct sockaddr*)&a2, l2) == 4 && recv(u2, d, sizeof(d), 0) == 4 && !memcmp(d, "back", 4);
    check("UDP: sendto, recvfrom (MSG_PEEK too), a reply", ok, "");
    close(u1);
    close(u2);

    /* names and addresses */
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo("localhost", "80", &hints, &ai);
    ok = g == 0 && ai && ai->ai_family == AF_INET &&
         ((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
         ((struct sockaddr_in*)ai->ai_addr)->sin_port == htons(80);
    if (ai) freeaddrinfo(ai);
    /* this machine's own name (Java's InetAddress.getLocalHost) */
    char self[64];
    ai = NULL;
    ok = ok && gethostname(self, sizeof(self)) == 0 && getaddrinfo(self, NULL, &hints, &ai) == 0 && ai &&
         ((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    if (ai) freeaddrinfo(ai);
    unsigned char v6[16];
    char s6[64], s4[16];
    struct in_addr in4;
    ok = ok && inet_pton(AF_INET6, "fe80::1:2", v6) == 1 && inet_ntop(AF_INET6, v6, s6, sizeof(s6)) && !strcmp(s6, "fe80::1:2") &&
         inet_pton(AF_INET, "10.0.2.15", &in4) == 1 && !strcmp(inet_ntop(AF_INET, &in4, s4, sizeof(s4)), "10.0.2.15") &&
         inet_addr("1.2.3.4") == htonl(0x01020304);
    check("getaddrinfo (localhost, this host's name), inet_pton / inet_ntop (v4 and v6)", ok, "");
    /* out to the Internet, if there is a way: DNS, then HTTP over TCP */
    struct addrinfo* ex = NULL;
    if (getaddrinfo("example.com", "http", &hints, &ex) == 0 && ex) {
        int w = socket(AF_INET, SOCK_STREAM, 0);
        char resp[64] = { 0 };
        const char* req = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n";
        ok = w >= 0 && connect(w, ex->ai_addr, ex->ai_addrlen) == 0 && send(w, req, strlen(req), 0) == (ssize_t)strlen(req) &&
             recv(w, resp, sizeof(resp) - 1, MSG_WAITALL) > 8 && !strncmp(resp, "HTTP/1.", 7);
        char ipstr[16];
        inet_ntop(AF_INET, &((struct sockaddr_in*)ex->ai_addr)->sin_addr, ipstr, sizeof(ipstr));
        snprintf(note, sizeof(note), "example.com is %s: %.12s", ipstr, resp);
        check("DNS and an HTTP request over TCP to the Internet", ok, note);
        if (w >= 0) close(w);
        freeaddrinfo(ex);
    } else {
        printf("  skip  the Internet - example.com could not be looked up (no network?)\n");
    }
    int s6fd = socket(AF_INET6, SOCK_STREAM, 0);
    check("IPv6 sockets are refused (Java falls back to IPv4)", s6fd == -1 && errno == EAFNOSUPPORT, "");
    free(out);
    free(in);
}

/* ── epoll, eventfd, pipes, socket pairs ──────────────────────────── */

static int g_evfd;

/* wakes an epoll_wait from another thread, as Java's Selector.wakeup does */
static void* late_wake(void* a) {
    banana_sleep((int)(long)a);
    eventfd_write(g_evfd, 1);
    return NULL;
}

#define PIPE_BULK 200000
static char g_pipe_out[PIPE_BULK], g_pipe_in[PIPE_BULK];
static int g_pipe_wfd;

static void* pipe_writer(void* a) {
    (void)a;
    ssize_t w = write(g_pipe_wfd, g_pipe_out, PIPE_BULK);       /* (blocks while the pipe is full) */
    return (void*)(long)w;
}

static void t_epoll(void) {
    printf("epoll, eventfd, pipes, socket pairs\n");
    char note[96];
    eventfd_t v = 0;

    /* eventfd: a counter */
    int ev = eventfd(0, EFD_NONBLOCK);
    int ok = ev >= 0 && eventfd_write(ev, 3) == 0 && eventfd_write(ev, 4) == 0 && eventfd_read(ev, &v) == 0 && v == 7;
    ssize_t r = read(ev, &v, sizeof(v));
    int e = errno;
    struct pollfd pf = { ev, POLLIN | POLLOUT, 0 };
    int pr = poll(&pf, 1, 0);
    ok = ok && r == -1 && e == EAGAIN && pr == 1 && pf.revents == POLLOUT;
    int sem = eventfd(2, EFD_SEMAPHORE);
    ok = ok && eventfd_read(sem, &v) == 0 && v == 1 && eventfd_read(sem, &v) == 0 && v == 1;
    close(sem);
    snprintf(note, sizeof(note), "fd %d", ev);
    check("eventfd: write adds, read takes all (EFD_SEMAPHORE: one), EAGAIN when 0", ok, note);

    /* epoll over the eventfd */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ee = { 0 }, out[8];
    ee.events = EPOLLIN;
    ee.data.u64 = 0x1122334455667788ull;
    ok = ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, ev, &ee) == 0;
    int r0 = epoll_wait(ep, out, 8, 0);
    long t0 = ms_now();
    int r1 = epoll_wait(ep, out, 8, 100);
    long took = ms_now() - t0;
    eventfd_write(ev, 1);
    int r2 = epoll_wait(ep, out, 8, 1000);
    ok = ok && r0 == 0 && r1 == 0 && took >= 90 && r2 == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 0x1122334455667788ull;
    int again = epoll_wait(ep, out, 8, 0);              /* level-triggered: still there */
    eventfd_read(ev, &v);
    int gone = epoll_wait(ep, out, 8, 0);
    snprintf(note, sizeof(note), "waited %ld ms for 100", took);
    check("epoll_wait on an eventfd: times out, then sees it (level-triggered)", ok && again == 1 && gone == 0, note);

    ok = epoll_ctl(ep, EPOLL_CTL_ADD, ev, &ee) == -1 && errno == EEXIST &&
         epoll_ctl(ep, EPOLL_CTL_MOD, 1000, &ee) == -1 && errno == EBADF &&
         epoll_ctl(ep, EPOLL_CTL_DEL, 5, NULL) == -1 && (errno == ENOENT || errno == EBADF) &&
         epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ee) == -1 && errno == EINVAL &&
         epoll_ctl(12345, EPOLL_CTL_ADD, ev, &ee) == -1 && errno == EBADF;
    check("epoll_ctl errors: EEXIST, EBADF, ENOENT, EINVAL", ok, "");

    /* the TCP side: a listening socket and a connection, with the eventfd */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t sl = sizeof(sa);
    ok = l >= 0 && bind(l, (struct sockaddr*)&sa, sizeof(sa)) == 0 && listen(l, 4) == 0 &&
         getsockname(l, (struct sockaddr*)&sa, &sl) == 0;
    ee.events = EPOLLIN;
    ee.data.fd = l;
    int stage = 0;
    ok = ok && epoll_ctl(ep, EPOLL_CTL_ADD, l, &ee) == 0;
    if (ok) stage++;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);              /* (getsockname: 0.0.0.0) */
    ok = ok && connect(c, (struct sockaddr*)&sa, sizeof(sa)) == 0;
    if (ok) stage++;
    int n = epoll_wait(ep, out, 8, 2000);
    ok = ok && n == 1 && out[0].data.fd == l && (out[0].events & EPOLLIN);
    if (ok) stage++;
    int s = ok ? accept(l, NULL, NULL) : -1;
    ee.events = EPOLLIN | EPOLLONESHOT;
    ee.data.fd = s;
    ok = ok && s >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, s, &ee) == 0 && epoll_wait(ep, out, 8, 0) == 0;
    if (ok) stage++;
    ok = ok && send(c, "ping", 4, 0) == 4;
    if (ok) stage++;
    n = epoll_wait(ep, out, 8, 2000);
    ok = ok && n == 1 && out[0].data.fd == s && out[0].events == EPOLLIN;
    if (ok) stage++;
    int quiet = epoll_wait(ep, out, 8, 50);             /* one-shot: off until MOD */
    ee.events = EPOLLIN | EPOLLOUT;
    int back = epoll_ctl(ep, EPOLL_CTL_MOD, s, &ee) == 0 ? epoll_wait(ep, out, 8, 0) : -1;
    snprintf(note, sizeof(note), "stage %d, n %d, quiet %d, back %d, events %#x", stage, n, quiet, back, (unsigned)out[0].events);
    check("epoll over TCP: accept ready, data ready, EPOLLONESHOT, EPOLL_CTL_MOD", ok && quiet == 0 && back == 1 && out[0].events == (EPOLLIN | EPOLLOUT), note);

    /* a wakeup from another thread while it waits on sockets and the eventfd */
    char b[16];
    recv(s, b, sizeof(b), 0);
    epoll_ctl(ep, EPOLL_CTL_DEL, s, NULL);
    g_evfd = ev;
    pthread_t th;
    pthread_create(&th, NULL, late_wake, (void*)150L);
    t0 = ms_now();
    n = epoll_wait(ep, out, 8, 5000);
    took = ms_now() - t0;
    pthread_join(th, NULL);
    snprintf(note, sizeof(note), "woken after %ld ms (150)", took);
    check("epoll_wait woken by an eventfd write from another thread", n == 1 && out[0].data.u64 == 0x1122334455667788ull && took >= 140 && took < 1000, note);
    eventfd_read(ev, &v);

    /* the epoll fd itself in poll() */
    struct pollfd pe = { ep, POLLIN, 0 };
    int p0 = poll(&pe, 1, 0);
    eventfd_write(ev, 1);
    int p1 = poll(&pe, 1, 0);
    eventfd_read(ev, &v);
    check("poll() on an epoll fd: readable when one of its own is", p0 == 0 && p1 == 1 && (pe.revents & POLLIN), "");

    /* close takes a descriptor out of the set */
    close(ev);
    ok = epoll_wait(ep, out, 8, 0) == 0 && epoll_ctl(ep, EPOLL_CTL_DEL, ev, NULL) == -1;
    close(s);
    n = epoll_wait(ep, out, 8, 0);
    check("close() takes a descriptor out of the epoll set", ok && n == 0, "");

    /* pipes */
    int pp[2];
    struct stat st;
    ok = pipe(pp) == 0 && write(pp[1], "hello", 5) == 5 && fstat(pp[0], &st) == 0 && S_ISFIFO(st.st_mode);
    struct pollfd pq[2] = { { pp[0], POLLIN, 0 }, { pp[1], POLLOUT, 0 } };
    ok = ok && poll(pq, 2, 0) == 2 && read(pp[0], b, sizeof(b)) == 5 && !memcmp(b, "hello", 5);
    ok = ok && fcntl(pp[0], F_SETFL, fcntl(pp[0], F_GETFL) | O_NONBLOCK) == 0 && read(pp[0], b, 1) == -1 && errno == EAGAIN;
    check("pipe: write, poll, read, O_NONBLOCK", ok, "");

    /* 200 KB through a 64 KB pipe: the writer thread blocks, the reader drains */
    fcntl(pp[0], F_SETFL, 0);
    for (int i = 0; i < PIPE_BULK; i++) g_pipe_out[i] = (char)(i * 13 + (i >> 8));
    g_pipe_wfd = pp[1];
    pthread_t wt;
    pthread_create(&wt, NULL, pipe_writer, NULL);
    size_t got = 0;
    while (got < PIPE_BULK) {
        ssize_t k = read(pp[0], g_pipe_in + got, PIPE_BULK - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    void* wr = NULL;
    pthread_join(wt, &wr);
    snprintf(note, sizeof(note), "%lu bytes", (unsigned long)got);
    ok = got == PIPE_BULK && (long)wr == PIPE_BULK && !memcmp(g_pipe_in, g_pipe_out, PIPE_BULK);
    close(pp[1]);
    struct pollfd ph = { pp[0], POLLIN, 0 };
    ok = ok && poll(&ph, 1, 0) == 1 && (ph.revents & POLLHUP) && read(pp[0], b, 1) == 0;
    close(pp[0]);
    check("pipe: 200 KB from a blocked writer thread, then the end (POLLHUP, 0)", ok, note);

    /* a socket pair, and dup2 of one over a TCP socket (Java's preClose) */
    int sp[2];
    ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0 && send(sp[0], "ab", 2, 0) == 2 && write(sp[1], "xyz", 3) == 3 &&
         recv(sp[1], b, sizeof(b), 0) == 2 && !memcmp(b, "ab", 2) && read(sp[0], b, sizeof(b)) == 3 && !memcmp(b, "xyz", 3);
    ok = ok && shutdown(sp[1], SHUT_WR) == 0 && read(sp[0], b, 1) == 0;
    close(sp[1]);
    check("socketpair: both ways, shutdown gives the other end its end", ok, "");
    int d = dup(sp[0]);
    int d2 = dup2(sp[0], c);
    ok = d >= 0 && d2 == c && recv(c, b, sizeof(b), 0) == 0 && close(c) == 0 && close(d) == 0;
    check("dup, and dup2 over a TCP socket: shut down, then closed", ok, "");
    close(sp[0]);
    close(l);
    close(ep);
}

/* ── signals ──────────────────────────────────────────────────────── */

static volatile int g_sig_seen, g_sig_code;
static void* volatile g_sig_addr;
static sigjmp_buf g_jb;

static void on_usr1(int sig, siginfo_t* si, void* uc) { (void)uc; g_sig_seen = sig; g_sig_code = si->si_code; }
static void on_fault_jump(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    g_sig_seen = sig;
    g_sig_code = si->si_code;
    g_sig_addr = si->si_addr;
    siglongjmp(g_jb, 1);
}

#ifdef __x86_64__
/* a load the handler can skip: "mov (%rax), %eax" is 2 bytes (8b 00) */
static int load_via_rax(volatile int* p) {
    int v;
    __asm__ volatile("movl (%1), %0" : "=a"(v) : "a"(p) : "memory");
    return v;
}
static void on_fault_resume(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = ucv;
    g_sig_seen = sig;
    g_sig_addr = si->si_addr;
    uc->uc_mcontext.gregs[REG_RIP] += 2;            /* past the load */
    uc->uc_mcontext.gregs[REG_RAX] = 42;            /* what it "read" */
}

static volatile int g_kicked;
static void on_kick(int sig) { (void)sig; g_kicked++; }
static void* kick_target(void* a) {
    (void)a;
    long until = ms_now() + 5000;
    while (!g_kicked && ms_now() < until) sched_yield();      /* busy: the signal comes on the timer */
    int busy = g_kicked;
    struct timespec nap = { 5, 0 };
    long t0 = ms_now();
    nanosleep(&nap, NULL);                                    /* asleep: a signal wakes it */
    return (void*)(long)(busy ? (ms_now() - t0 < 3000 ? 0 : 2) : 1);
}

static char* g_alt;
static volatile int g_on_alt;
static void on_alt(int sig) {
    (void)sig;
    char here;
    g_on_alt = &here >= g_alt && &here < g_alt + 65536;
}

static int deep(int n) { volatile char pad[512]; pad[0] = (char)n; return n ? deep(n - 1) + pad[0] : 0; }
static void* overflow_thread(void* a) {
    (void)a;
    stack_t ss = { malloc(65536), 0, 65536 };
    sigaltstack(&ss, NULL);
    if (sigsetjmp(g_jb, 1)) return (void*)(long)(g_sig_seen == SIGSEGV ? 0 : 1);
    deep(1000000);                                            /* far past its 256 KiB stack */
    return (void*)2;
}
#endif

static void t_signals(void) {
    printf("signals: handlers, faults as signals, masks, pthread_kill\n");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    g_sig_seen = 0;
    int ok = sigaction(SIGUSR1, &sa, NULL) == 0 && raise(SIGUSR1) == 0 && g_sig_seen == SIGUSR1 && g_sig_code == SI_TKILL;
    check("sigaction + raise: the handler ran, with its siginfo", ok, "");

    signal(SIGUSR1, SIG_IGN);
    g_sig_seen = 0;
    check("SIG_IGN: raise does nothing", raise(SIGUSR1) == 0 && !g_sig_seen, "");

#ifdef __x86_64__
    sa.sa_sigaction = on_fault_jump;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    g_sig_seen = 0;
    volatile int* volatile bad = (volatile int*)16;     /* (a variable: the compiler must not see it is bad) */
    if (!sigsetjmp(g_jb, 1)) { *bad = 1; g_sig_seen = -1; }
    char note[96];
    snprintf(note, sizeof(note), "at %p", g_sig_addr);
    check("a bad write: SIGSEGV with si_addr, out with siglongjmp", g_sig_seen == SIGSEGV && g_sig_addr == (void*)16 && (g_sig_code == SEGV_ACCERR ||
          g_sig_code == SEGV_MAPERR), note);
    sigset_t cur;
    sigprocmask(SIG_SETMASK, NULL, &cur);
    check("siglongjmp put the signal mask back", !sigismember(&cur, SIGSEGV), "");

    sa.sa_sigaction = on_fault_resume;
    sigaction(SIGSEGV, &sa, NULL);
    g_sig_seen = 0;
    int v = load_via_rax((volatile int*)24);
    check("a handler changes RIP and RAX: the code goes on", v == 42 && g_sig_seen == SIGSEGV && g_sig_addr == (void*)24, "");

    sa.sa_sigaction = on_fault_jump;
    sigaction(SIGSEGV, &sa, NULL);
    char* ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_sig_seen = 0;
    volatile char rd = ro[0];
    (void)rd;
    if (!sigsetjmp(g_jb, 1)) { ro[10] = 1; g_sig_seen = -1; }
    check("writing a PROT_READ page: SIGSEGV (SEGV_ACCERR)", g_sig_seen == SIGSEGV && g_sig_code == SEGV_ACCERR &&
          g_sig_addr == ro + 10, "");
    munmap(ro, 4096);

    sigaction(SIGFPE, &sa, NULL);
    g_sig_seen = 0;
    volatile int zero = 0, num = 7;
    if (!sigsetjmp(g_jb, 1)) { num = num / zero; g_sig_seen = -1; }
    check("a division by zero: SIGFPE (FPE_INTDIV)", g_sig_seen == SIGFPE && g_sig_code == FPE_INTDIV, "");

    sigaction(SIGILL, &sa, NULL);
    g_sig_seen = 0;
    if (!sigsetjmp(g_jb, 1)) { __asm__ volatile("ud2"); g_sig_seen = -1; }
    check("an invalid instruction: SIGILL", g_sig_seen == SIGILL, "");

    /* blocked: it waits; unblocked: it comes */
    sa.sa_sigaction = on_usr1;
    sigaction(SIGUSR2, &sa, NULL);
    sigset_t s, pend;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    sigprocmask(SIG_BLOCK, &s, NULL);
    g_sig_seen = 0;
    raise(SIGUSR2);
    sigpending(&pend);
    int waited = !g_sig_seen && sigismember(&pend, SIGUSR2);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    banana_sleep(1);
    check("a blocked signal waits (sigpending), unblocked it comes", waited && g_sig_seen == SIGUSR2, "");

    /* to another thread: busy (the timer delivers it), then asleep (woken) */
    signal(SIGUSR1, on_kick);
    g_kicked = 0;
    pthread_t th;
    void* r = (void*)9;
    pthread_create(&th, NULL, kick_target, NULL);
    banana_sleep(100);
    pthread_kill(th, SIGUSR1);
    banana_sleep(300);
    pthread_kill(th, SIGUSR1);
    pthread_join(th, &r);
    snprintf(note, sizeof(note), "%d delivered", g_kicked);
    check("pthread_kill: a busy thread, then a sleeping one", r == NULL && g_kicked == 2, note);

    /* a handler on its own stack */
    g_alt = malloc(65536);
    stack_t ss = { g_alt, 0, 65536 };
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alt;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR2, &sa, NULL);
    g_on_alt = 0;
    raise(SIGUSR2);
    check("sigaltstack: the handler runs on the alternate stack", g_on_alt == 1, "");
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);

    /* a stack overflow (the guard page under a thread's stack) caught */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault_jump;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 << 10);
    g_sig_seen = 0;
    r = (void*)9;
    pthread_create(&th, &at, overflow_thread, NULL);
    pthread_join(th, &r);
    check("a stack overflow: SIGSEGV on the alternate stack, recovered", r == NULL, "");
    signal(SIGSEGV, SIG_DFL);
    signal(SIGFPE, SIG_DFL);
    signal(SIGILL, SIG_DFL);
#else
    printf("  skip  faults as signals, masks, pthread_kill - 64-bit only\n");
#endif
}

/* ── dlopen ───────────────────────────────────────────────────────── */

/* libdemo calls this back (it is in the app's export table) */
int posixtest_from_app(int x) { return x + 1; }

static int count_objs(struct dl_phdr_info* i, size_t size, void* data) { (void)i; (void)size; (*(int*)data)++; return 0; }

static void t_dlopen(void) {
    printf("dlopen: shared libraries\n");
    const char* bits = sizeof(void*) == 8 ? "64" : "32";
    char p2[96], p1[32], note[160];
    snprintf(p2, sizeof(p2), "/apps/posixtest/libdemo2_%s.so", bits);
    snprintf(p1, sizeof(p1), "libdemo%s.so", bits);
    void* h2 = dlopen(p2, RTLD_NOW);
    const char* e = h2 ? "" : dlerror();
    snprintf(note, sizeof(note), "%s", e ? e : "");
    check("dlopen a library that needs another (DT_NEEDED)", h2 != NULL, note);
    if (!h2) return;
    int (*twice)(int, int) = (int (*)(int, int))dlsym(h2, "demo2_twice_sum");
    check("dlsym, and a call across the two libraries", twice && twice(3, 4) == 14, "");

    void* h1 = dlopen(p1, RTLD_NOW);                  /* (loaded already: the same one) */
    int (*ran)(void) = (int (*)(void))dlsym(h1, "demo_ctor_ran");
    int* counter = (int*)dlsym(h1, "demo_counter");
    check("its constructor ran, its data is there", h1 && ran && ran() == 1 && counter && *counter == 105, "");

    int (*call_app)(int) = (int (*)(int))dlsym(h1, "demo_call_app");
    char* (*fmt)(int) = (char* (*)(int))dlsym(h1, "demo_format");
    char* s = fmt ? fmt(7) : NULL;
    snprintf(note, sizeof(note), "%s", s ? s : "(none)");
    check("the library calls the C library and the app", call_app && call_app(5) == 12 && s && !strcmp(s, "value=7 len=3"), note);
    free(s);

    Dl_info di;
    int (*add)(int, int) = (int (*)(int, int))dlsym(h1, "demo_add");
    int ok = add && dladdr((void*)add, &di) && di.dli_sname && !strcmp(di.dli_sname, "demo_add") && strstr(di.dli_fname, p1);
    int objs = 0;
    dl_iterate_phdr(count_objs, &objs);
    snprintf(note, sizeof(note), "%d objects loaded", objs);
    check("dladdr names the function, dl_iterate_phdr lists them", ok && objs >= 3, note);

    ok = dlsym(RTLD_DEFAULT, "malloc") == (void*)malloc && !dlsym(h1, "no_such_function") && dlerror() &&
         !dlopen("/no/such/library.so", RTLD_NOW) && dlerror();
    check("RTLD_DEFAULT finds the C library; missing things fail with dlerror", ok, "");
    check("dlclose", dlclose(h1) == 0 && dlclose(h2) == 0, "");
}

/* ── files ──────────────────────────────────────────────────────── */

/* mkdir's errno: Java's Files.createDirectories takes a folder that is
 * there by EEXIST, and gave up on Banana OS before it was set */
/* the time: UTC from time(), the zone's offset in localtime(), back with mktime() */
static void t_time(void) {
    time_t now = time(NULL);
    struct tm g, l;
    gmtime_r(&now, &g);
    localtime_r(&now, &l);
    long diff = (long)(timegm(&l) - now);             /* (the local fields read as UTC: now + offset) */
    char note[96];
    snprintf(note, sizeof(note), "offset %ld s, gmtoff %ld, %s", diff, l.tm_gmtoff, l.tm_zone ? l.tm_zone : "-");
    check("localtime() = UTC + tm_gmtoff", now > 1700000000 && diff == l.tm_gmtoff && diff % 900 == 0, note);
    struct tm c = l;
    check("mktime(localtime(t)) == t", mktime(&c) == now, "");
    char z[16];
    strftime(z, sizeof(z), "%z", &l);
    long a = l.tm_gmtoff < 0 ? -l.tm_gmtoff : l.tm_gmtoff;
    char want[16];
    snprintf(want, sizeof(want), "%c%02ld%02ld", l.tm_gmtoff < 0 ? '-' : '+', a / 3600, a / 60 % 60);
    check("strftime %z", !strcmp(z, want), z);
    tzset();
    check("tzset: timezone is minus the winter offset", timezone == -(l.tm_gmtoff - (l.tm_isdst ? 3600 : 0)), "");

    /* the wall clock never runs backwards (across the clock chip's seconds) */
    long long last_tv = 0, last_ts = 0;
    int back = 0;
    unsigned t0 = banana_ticks();
    while (banana_ticks() - t0 < 2500) {
        struct timeval tv;
        struct timespec ts;
        gettimeofday(&tv, NULL);
        clock_gettime(CLOCK_REALTIME, &ts);
        long long a1 = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000, a2 = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (a1 < last_tv || a2 < last_ts) back++;
        last_tv = a1;
        last_ts = a2;
    }
    snprintf(note, sizeof(note), "%d steps back in 2.5 s", back);
    check("gettimeofday / CLOCK_REALTIME never go back", back == 0, note);
}

static void t_files(void) {
    const char* home = getenv("HOME");
    char dir[256], sub[300], file[300];
    snprintf(dir, sizeof(dir), "%s/posixtest-dir", home && *home ? home : "/home/banana");
    snprintf(sub, sizeof(sub), "%s/missing/deeper", dir);
    snprintf(file, sizeof(file), "%s/a-file", dir);
    rmdir(dir);
    int made = mkdir(dir, 0755) == 0;
    errno = 0;
    int again = mkdir(dir, 0755);
    check("mkdir, and again: EEXIST", made && again == -1 && errno == EEXIST, "");
    errno = 0;
    int deep = mkdir(sub, 0755);
    int e1 = errno;
    FILE* f = fopen(file, "w");
    if (f) fclose(f);
    char under[340];
    snprintf(under, sizeof(under), "%s/x", file);
    errno = 0;
    int nd = mkdir(under, 0755);
    check("mkdir with no parent: ENOENT; under a file: ENOTDIR", deep == -1 && e1 == ENOENT && nd == -1 && errno == ENOTDIR, "");
    remove(file);
    rmdir(dir);
}

/* ── memory: posix_memalign next to malloc/free ───────────────────── */

#define MEM_THREADS 4
#define MEM_ROUNDS  100000

/* aligned and plain blocks mixed, kept a while and freed, from several threads at once */
static void* mem_worker(void* arg) {
    unsigned seed = (unsigned)(long)arg * 2654435761u + 1;
    void* keep[64] = { 0 };
    long bad = 0;
    for (int i = 0; i < MEM_ROUNDS; i++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)(seed >> 8) & 63;
        if (keep[k]) {
            if (*(unsigned char*)keep[k] != (unsigned char)k) bad++;
            free(keep[k]);
            keep[k] = NULL;
        }
        size_t align = (size_t)16 << ((seed >> 20) & 7);          /* 16 .. 2048 */
        void* p = NULL;
        if ((seed >> 4) & 1) { if (posix_memalign(&p, align, 24 + (seed & 255))) p = NULL; }
        else p = malloc(24 + (seed & 255));
        if (!p || ((seed >> 4) & 1 && ((uintptr_t)p & (align - 1)))) { bad++; continue; }
        *(unsigned char*)p = (unsigned char)k;
        keep[k] = p;
    }
    for (int k = 0; k < 64; k++) free(keep[k]);
    return (void*)bad;
}

static void t_memory(void) {
    printf("memory:\n");
    int ok = 1;
    for (size_t a = 32; a <= 4096; a *= 2) {
        void* p = NULL;
        if (posix_memalign(&p, a, 100) || ((uintptr_t)p & (a - 1))) { ok = 0; break; }
        memset(p, 0x5a, 100);
        free(p);
    }
    check("posix_memalign 32 .. 4096: aligned, usable, freed", ok, NULL);

    void* p = NULL;
    ok = posix_memalign(&p, 64, 50) == 0;
    if (ok) { memset(p, 7, 50); p = realloc(p, 5000); }
    ok = ok && p && ((unsigned char*)p)[0] == 7 && ((unsigned char*)p)[49] == 7;
    free(p);
    check("realloc of an aligned block keeps its bytes", ok, NULL);

    enum { N = 20000 };
    void** v = malloc(N * sizeof(void*));
    ok = v != NULL;
    for (int i = 0; ok && i < N; i++) {
        if (posix_memalign(&v[i], 32 << (i % 4), 40)) { ok = 0; break; }
        *(int*)v[i] = i;
    }
    for (int i = 0; ok && i < N; i++) {
        int j = (int)(((unsigned)i * 7919u) % N);            /* (another order than they came) */
        if (*(int*)v[j] != j) ok = 0;
        free(v[j]);
    }
    free(v);
    check("20000 aligned blocks at once, freed in another order", ok, NULL);

    pthread_t t[MEM_THREADS];
    unsigned t0 = banana_ticks();
    for (long i = 0; i < MEM_THREADS; i++) pthread_create(&t[i], NULL, mem_worker, (void*)i);
    long bad = 0;
    for (int i = 0; i < MEM_THREADS; i++) { void* r; pthread_join(t[i], &r); bad += (long)r; }
    char note[64];
    snprintf(note, sizeof(note), "%d threads x %d, %u ms", MEM_THREADS, MEM_ROUNDS, banana_ticks() - t0);
    check("aligned and plain blocks from several threads at once", bad == 0, note);
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp(only, "mmap")) t_mmap();
    if (!only || !strcmp(only, "threads")) t_threads();
    if (!only || !strcmp(only, "sockets")) t_sockets();
    if (!only || !strcmp(only, "epoll")) t_epoll();
    if (!only || !strcmp(only, "signals")) t_signals();
    if (!only || !strcmp(only, "dlopen")) t_dlopen();
    if (!only || !strcmp(only, "files")) t_files();
    if (!only || !strcmp(only, "time")) t_time();
    if (!only || !strcmp(only, "memory")) t_memory();
    printf("posixtest: %d passed, %d failed\n", g_ok, g_fail);
    return g_fail;
}
