/* Host test: a page with scripts, driven by actions, rendered to PPM.
 *   page_test file.html out.ppm [click:#id] [type:text] [key:enter] [tick:ms] [text:#id] ...
 * Files referenced by the page are read relative to the current directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "page.h"
#include "render.h"

static uint32_t g_now = 1000;
static uint32_t now_ms(void) { return g_now; }

static void logf_(void* ctx, const char* s) { (void)ctx; fprintf(stderr, "[log] %s\n", s); }

static int fetch(void* ctx, const char* url, char** data, uint32_t* len, char* ct, int ccap,
                 char* fin, int fcap, char* err, int ecap) {
    (void)ctx; (void)ct; (void)ccap; (void)fin; (void)fcap; (void)err; (void)ecap;
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        /* real pages: through curl */
        char cmd[2300];
        snprintf(cmd, sizeof(cmd), "curl -sL --max-time 20 -A 'Mozilla/5.0' '%s'", url);
        FILE* f = popen(cmd, "r");
        if (!f) return -1;
        size_t cap = 1 << 20, n = 0;
        char* buf = malloc(cap);
        size_t r;
        while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
            n += r;
            if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        }
        pclose(f);
        buf[n] = 0;
        fprintf(stderr, "[fetch] %s -> %zu bytes\n", url, n);
        *data = buf;
        *len = (uint32_t)n;
        return 0;
    }
    const char* path = url;
    if (strncmp(path, "file://", 7) == 0) path += 7;
    char clean[1024];
    snprintf(clean, sizeof(clean), "%s", path);
    char* q = strpbrk(clean, "?#");
    if (q) *q = 0;
    if (clean[0] == '/') memmove(clean, clean + 1, strlen(clean));
    FILE* f = fopen(clean, "rb");
    fprintf(stderr, "[fetch] %s -> %s\n", url, f ? "ok" : "missing");
    if (!f) return -1;
    char* buf = malloc(8 << 20);
    size_t n = fread(buf, 1, (8 << 20) - 1, f);
    fclose(f);
    buf[n] = 0;
    *data = buf;
    *len = (uint32_t)n;
    return 0;
}

static dom_node_t* by_id(page_t* p, const char* id) { return dom_find_id(p->doc, id); }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: page_test file.html out.ppm [actions]\n"); return 2; }
    int width = 640;
    setvbuf(stdout, NULL, _IONBF, 0);
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    static char src[8 << 20];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    page_env_t env = { .fetch = fetch, .now_ms = now_ms, .log = logf_ };
    page_t* p = page_new(&env, 64u << 20);
    char url[600];
    if (getenv("PAGE_URL")) snprintf(url, sizeof(url), "%s", getenv("PAGE_URL"));   /* the page's real address */
    else snprintf(url, sizeof(url), "file:///%s", argv[1]);
    page_load(p, url, src, (uint32_t)n, width);
    fprintf(stderr, "title: '%s' status: '%s'\n", p->title, p->status);
    for (int i = 3; i < argc; i++) {
        const char* a = argv[i];
        page_update(p, width);
        if (strncmp(a, "click:#", 7) == 0) {
            dom_node_t* e = by_id(p, a + 7);
            if (!e) { fprintf(stderr, "no #%s\n", a + 7); continue; }
            page_click(p, e->box_x + e->box_w / 2, e->box_y + e->box_h / 2);
        } else if (strncmp(a, "type:", 5) == 0) {
            for (const char* s = a + 5; *s; s++) page_key(p, *s);
        } else if (strcmp(a, "key:enter") == 0) {
            page_key(p, '\n');
        } else if (strcmp(a, "key:bs") == 0) {
            page_key(p, '\b');
        } else if (strncmp(a, "tick:", 5) == 0) {
            int ms = atoi(a + 5);
            for (int t = 0; t < ms; t += 10) { g_now += 10; page_tick(p); }
        } else if (strncmp(a, "text:#", 6) == 0) {
            dom_node_t* e = by_id(p, a + 6);
            printf("#%s = '%s'\n", a + 6, e ? dom_text(&p->A, e) : "(none)");
        } else if (strncmp(a, "html:#", 6) == 0) {
            dom_node_t* e = by_id(p, a + 6);
            printf("#%s html = '%s'\n", a + 6, e ? dom_html(&p->A, e, 0) : "(none)");
        }
        if (p->alert_pending) { printf("ALERT: %s\n", p->alert); p->alert_pending = 0; }
        if (p->nav_pending) { printf("NAV: %s\n", p->nav); p->nav_pending = 0; }
        if (p->status[0]) { printf("STATUS: %s\n", p->status); p->status[0] = 0; }
    }
    page_update(p, width);
    layout_t* L = p->layout;
    int h = L->height < 100 ? 100 : L->height;
    if (h > 4000) h = 4000;
    uint32_t* buf = calloc((size_t)width * h, 4);
    render_page(L, buf, width, width, h, 0, 0, width, h, 0);
    FILE* o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", width, h);
    for (int i = 0; i < width * h; i++) { unsigned char px[3] = { buf[i] >> 16, buf[i] >> 8, buf[i] }; fwrite(px, 1, 3, o); }
    fclose(o);
    fprintf(stderr, "layout: %u items, height %d, arena %u KB\n", L->n, L->height, p->A.total / 1024);
    page_free(p);
    return 0;
}
