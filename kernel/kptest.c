/*
 * kptest: a check of kernel preemption (kernel/task.h, task_kpreempt).
 * Worker tasks decode the same picture (PNG / JPEG / GIF / BMP through
 * stb_image) and draw the same SVG over and over at once, so the timer
 * preempts them in the middle of that code and switches between them; every
 * result must match the one computed first, alone.
 */
#include "kptest.h"
#include "task.h"
#include "timer.h"
#include "kheap.h"
#include "kstring.h"
#include "fs.h"
#include "image.h"
#include "svg.h"

#define WORKERS 3

static struct {
    uint8_t*  img; uint32_t img_len; uint32_t img_sum;
    char*     svg; uint32_t svg_len; uint32_t svg_sum;
    uint32_t  until;
    volatile int done;
    volatile uint32_t runs[WORKERS], bad[WORKERS];
    uint32_t  bad_img, failed_img, bad_svg, failed_svg;
    char      first_err[96];
} g_t;

static uint32_t sum_bytes(const uint8_t* p, uint32_t n) {
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < n; i++) s = (s ^ p[i]) * 16777619u;
    return s;
}

/* one decode of each: their checksums (0 = it failed) */
static uint32_t do_img(void) {
    if (!g_t.img) return 0;
    image_t im;
    char err[96];
    if (image_decode(g_t.img, g_t.img_len, &im, err, sizeof(err)) != 0) {
        if (!g_t.first_err[0]) kstrlcpy(g_t.first_err, err, sizeof(g_t.first_err));
        return 0;
    }
    uint32_t s = sum_bytes(im.rgb, (uint32_t)(im.w * im.h * 3)) ^ (uint32_t)im.w << 16 ^ (uint32_t)im.h;
    image_free(&im);
    return s ? s : 1;
}

static uint32_t do_svg(void) {
    if (!g_t.svg) return 0;
    arena_t A;
    arena_init(&A, 64u << 20);
    img_data_t out;
    memset(&out, 0, sizeof(out));
    uint32_t s = 0;
    if (svg_render_color(g_t.svg, g_t.svg_len, 0, 0, &out, &A, 0x336699) == 0 && out.px) {
        s = sum_bytes((const uint8_t*)out.px, (uint32_t)(out.w * out.h * 4)) ^
            sum_bytes(out.alpha, (uint32_t)(out.w * out.h));
        if (!s) s = 1;
    }
    arena_free_all(&A);
    return s;
}

static void worker(int k) {
    task_set_priority(TASK_PRIO_NORMAL);
    while ((int32_t)(timer_ms() - g_t.until) < 0) {
        uint32_t a = g_t.img ? do_img() : 0, b = g_t.svg ? do_svg() : 0;
        if (g_t.img && a != g_t.img_sum) { g_t.bad[k]++; g_t.bad_img++; if (!a) g_t.failed_img++; }
        if (g_t.svg && b != g_t.svg_sum) { g_t.bad[k]++; g_t.bad_svg++; if (!b) g_t.failed_svg++; }
        g_t.runs[k]++;
        task_sleep_ms(1);
    }
    __sync_fetch_and_add(&g_t.done, 1);      /* (then it ends: its slot is reused) */
}
/* the expected results, computed alone - in a task like the workers (a new
 * task starts with the FPU in its default state; SVG drawing uses it) */
static void reference(void) {
    g_t.img_sum = do_img();
    g_t.svg_sum = do_svg();
    __sync_fetch_and_add(&g_t.done, 1);
}
static void w0(void) { worker(0); }
static void w1(void) { worker(1); }
static void w2(void) { worker(2); }

static uint8_t* copy_file(const char* path, uint32_t* len) {
    int idx = fs_find_file(path);
    if (idx < 0) return NULL;
    fs_file_t* f = fs_get_file(idx);
    uint8_t* p = (uint8_t*)kmalloc(f->size + 1);
    if (!p) return NULL;
    memcpy(p, f->content, f->size);
    p[f->size] = 0;
    *len = f->size;
    return p;
}

static uint32_t kpreempts_total(void) {
    static task_info_t t[TASK_MAX];
    int n = task_count();
    if (n > TASK_MAX) n = TASK_MAX;
    task_snapshot(t, n);
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += t[i].kpreempts;
    return s;
}

int kptest_run(const char* img_path, const char* svg_path, int secs, char* out, int cap) {
    memset(&g_t, 0, sizeof(g_t));
    if (img_path && *img_path) g_t.img = copy_file(img_path, &g_t.img_len);
    if (svg_path && *svg_path) g_t.svg = (char*)copy_file(svg_path, &g_t.svg_len);
    if (!g_t.img && !g_t.svg) { ksnprintf(out, (size_t)cap, "kptest: no picture / SVG file to use"); return -1; }
    task_create_stack("kptest", reference, 256u << 10);
    while (!g_t.done) task_sleep_ms(20);
    g_t.done = 0;
    if ((g_t.img && !g_t.img_sum) || (g_t.svg && !g_t.svg_sum)) {
        ksnprintf(out, (size_t)cap, "kptest: the file%s could not be decoded", g_t.img && g_t.svg ? "s" : "");
        kfree(g_t.img); kfree(g_t.svg);
        return -1;
    }
    uint32_t kp0 = kpreempts_total();
    g_t.until = timer_ms() + (uint32_t)secs * 1000u;
    void (*fn[WORKERS])(void) = { w0, w1, w2 };
    for (int k = 0; k < WORKERS; k++) task_create_stack("kptest", fn[k], 256u << 10);
    while (g_t.done < WORKERS) task_sleep_ms(50);
    uint32_t runs = 0, bad = 0;
    for (int k = 0; k < WORKERS; k++) { runs += g_t.runs[k]; bad += g_t.bad[k]; }
    uint32_t kp = kpreempts_total() - kp0;
    ksnprintf(out, (size_t)cap, "kptest: %u rounds in %d tasks, %u preemptions inside the decoders, %u wrong result%s - %s",
              runs, WORKERS, kp, bad, bad == 1 ? "" : "s", bad ? "FAILED" : kp ? "ok" : "ok (but never preempted)");
    if (bad) {
        int l = (int)strlen(out);
        ksnprintf(out + l, (size_t)(cap - l), "\n  picture: %u wrong (%u failed%s%s), SVG: %u wrong (%u failed)",
                  g_t.bad_img, g_t.failed_img, g_t.first_err[0] ? ": " : "", g_t.first_err, g_t.bad_svg, g_t.failed_svg);
    }
    kfree(g_t.img); kfree(g_t.svg);
    return bad ? -1 : 0;
}
