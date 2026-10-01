/* Host test driver for BananaScript:  script_test js|php <file> */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "script.h"

static void out(void* ctx, const char* s, uint32_t n) { (void)ctx; fwrite(s, 1, n, stdout); }
static void logf_(void* ctx, const char* s, uint32_t n) { (void)ctx; fwrite(s, 1, n, stdout); fputc('\n', stdout); }

static void host_clock(script_tm_t* tm) {
    time_t t = time(NULL);
    struct tm* g = gmtime(&t);
    tm->year = g->tm_year + 1900; tm->month = g->tm_mon + 1; tm->day = g->tm_mday;
    tm->hour = g->tm_hour; tm->minute = g->tm_min; tm->second = g->tm_sec; tm->wday = g->tm_wday;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s js|php file\n", argv[0]); return 2; }
    FILE* f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    static char src[1 << 20];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    script_clock = host_clock;
    arena_t A;
    arena_init(&A, 64u << 20);
    interp_t* I = script_new(&A, argv[1][0] == 'p' ? LANG_PHP : LANG_JS);
    script_set_output(I, out, NULL);
    script_set_log(I, logf_, NULL);
    int r = script_run(I, src, (uint32_t)n, argv[2]);
    if (r) printf("\n[ERROR] %s\n", script_error(I));
    arena_free_all(&A);
    return r ? 1 : 0;
}
