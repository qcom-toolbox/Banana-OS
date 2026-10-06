/* threads - counts the prime numbers below a limit, first in one thread,
 * then split over several threads that add their results to a shared
 * total under a mutex; then a producer/consumer queue (semaphores) and
 * threads waiting for a signal (condition variable).
 * Usage: threads [threads] [limit] */
#include <stdio.h>
#include <stdlib.h>
#include <banana.h>

typedef struct {
    unsigned from, to;      /* [from, to) */
    unsigned found;
} job_t;

static banana_mutex_t g_lock = BANANA_MUTEX_INIT;
static unsigned g_total;

static int is_prime(unsigned n) {
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (unsigned d = 3; d * d <= n; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

static int worker(void* arg) {
    job_t* j = (job_t*)arg;
    unsigned n = 0;
    for (unsigned i = j->from; i < j->to; i++) n += (unsigned)is_prime(i);
    j->found = n;
    banana_lock(&g_lock);
    g_total += n;
    banana_unlock(&g_lock);
    return (int)n;
}

/* ── producer / consumer: a ring of 8 slots, semaphores count free/full ── */
#define QN 8
static int g_q[QN];
static int g_qh, g_qt;
static banana_sem_t g_free = BANANA_SEM_INIT(QN), g_full = BANANA_SEM_INIT(0);
static banana_mutex_t g_qlock = BANANA_MUTEX_INIT;

static int producer(void* arg) {
    int n = *(int*)arg;
    for (int i = 1; i <= n; i++) {
        banana_sem_wait(&g_free);
        banana_lock(&g_qlock);
        g_q[g_qh] = i;
        g_qh = (g_qh + 1) % QN;
        banana_unlock(&g_qlock);
        banana_sem_post(&g_full);
    }
    return 0;
}

static int consumer(void* arg) {
    int n = *(int*)arg;
    int sum = 0;
    for (int i = 0; i < n; i++) {
        banana_sem_wait(&g_full);
        banana_lock(&g_qlock);
        int v = g_q[g_qt];
        g_qt = (g_qt + 1) % QN;
        banana_unlock(&g_qlock);
        banana_sem_post(&g_free);
        sum += v;
    }
    return sum;
}

/* ── a condition variable: three threads wait until "go" ── */
static banana_mutex_t g_glock = BANANA_MUTEX_INIT;
static banana_cond_t g_go_cond = BANANA_COND_INIT;
static int g_go, g_woke;

static int waiter(void* arg) {
    (void)arg;
    banana_lock(&g_glock);
    while (!g_go) banana_cond_wait(&g_go_cond, &g_glock);
    g_woke++;
    banana_unlock(&g_glock);
    return 0;
}

int main(int argc, char** argv) {
    int nthreads = argc > 1 ? atoi(argv[1]) : 4;
    unsigned limit = argc > 2 ? (unsigned)atoi(argv[2]) : 400000;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 12) nthreads = 12;

    printf("Counting primes below %u (%d processor core%s)\n", limit, banana_cpus(), banana_cpus() == 1 ? "" : "s");

    unsigned t0 = banana_ticks();
    job_t one = { 0, limit, 0 };
    worker(&one);
    unsigned t1 = banana_ticks();
    printf("  1 thread:  %u primes in %u ms\n", one.found, t1 - t0);

    g_total = 0;
    job_t jobs[12];
    int ids[12];
    unsigned step = limit / (unsigned)nthreads;
    t0 = banana_ticks();
    for (int i = 0; i < nthreads; i++) {
        jobs[i].from = (unsigned)i * step;
        jobs[i].to = i == nthreads - 1 ? limit : (unsigned)(i + 1) * step;
        jobs[i].found = 0;
        ids[i] = banana_thread(worker, &jobs[i]);
        if (ids[i] < 0) worker(&jobs[i]);       /* an older Banana OS: do it here */
    }
    for (int i = 0; i < nthreads; i++)
        if (ids[i] > 0) banana_join(ids[i]);
    t1 = banana_ticks();
    printf("  %d threads: %u primes in %u ms\n", nthreads, g_total, t1 - t0);
    for (int i = 0; i < nthreads; i++)
        printf("    thread %d: [%u, %u) -> %u\n", i + 1, jobs[i].from, jobs[i].to, jobs[i].found);
    int ok = g_total == one.found;
    printf(ok ? "Results match.\n" : "Results DIFFER!\n");

    int items = 2000;
    int pid = banana_thread(producer, &items), cid = banana_thread(consumer, &items);
    if (pid > 0 && cid > 0) {
        banana_join(pid);
        int sum = banana_join(cid);
        int want = items * (items + 1) / 2;
        printf("Producer/consumer: %d items through a %d-slot queue, sum %d (%s)\n",
               items, QN, sum, sum == want ? "right" : "WRONG");
        ok = ok && sum == want;
    }

    int w[3];
    for (int i = 0; i < 3; i++) w[i] = banana_thread(waiter, NULL);
    banana_sleep(50);
    banana_lock(&g_glock);
    g_go = 1;
    banana_cond_broadcast(&g_go_cond);
    banana_unlock(&g_glock);
    for (int i = 0; i < 3; i++) if (w[i] > 0) banana_join(w[i]);
    printf("Condition variable: %d of 3 waiting threads woke up\n", g_woke);
    ok = ok && g_woke == 3;
    return ok ? 0 : 1;
}
