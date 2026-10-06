/* threads - counts the prime numbers below a limit, first in one thread,
 * then split over several threads that add their results to a shared
 * total under a mutex. Usage: threads [threads] [limit] */
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

int main(int argc, char** argv) {
    int nthreads = argc > 1 ? atoi(argv[1]) : 4;
    unsigned limit = argc > 2 ? (unsigned)atoi(argv[2]) : 400000;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 16) nthreads = 16;

    printf("Counting primes below %u (%d processor core%s)\n", limit, banana_cpus(), banana_cpus() == 1 ? "" : "s");

    unsigned t0 = banana_ticks();
    job_t one = { 0, limit, 0 };
    worker(&one);
    unsigned t1 = banana_ticks();
    printf("  1 thread:  %u primes in %u ms\n", one.found, t1 - t0);

    g_total = 0;
    job_t jobs[16];
    int ids[16];
    unsigned step = limit / (unsigned)nthreads;
    t0 = banana_ticks();
    for (int i = 0; i < nthreads; i++) {
        jobs[i].from = (unsigned)i * step;
        jobs[i].to = i == nthreads - 1 ? limit : (unsigned)(i + 1) * step;
        jobs[i].found = 0;
        ids[i] = banana_thread(worker, &jobs[i]);
        if (ids[i] < 0) {                       /* an older Banana OS: do it here */
            worker(&jobs[i]);
        }
    }
    for (int i = 0; i < nthreads; i++)
        if (ids[i] > 0) banana_join(ids[i]);
    t1 = banana_ticks();
    printf("  %d threads: %u primes in %u ms\n", nthreads, g_total, t1 - t0);
    for (int i = 0; i < nthreads; i++)
        printf("    thread %d: [%u, %u) -> %u\n", i + 1, jobs[i].from, jobs[i].to, jobs[i].found);
    printf(g_total == one.found ? "Results match.\n" : "Results DIFFER!\n");
    return g_total == one.found ? 0 : 1;
}
