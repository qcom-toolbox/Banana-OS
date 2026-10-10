/* Banana Code: projects. A folder with a banana.json is a Banana OS app:
 *
 *   { "name": "hello", "title": "Hello", "version": "1.0", "type": "console",
 *     "description": "...", "author": "...",
 *     "sources": ["main.c"], "cflags": "-O2", "data": ["levels.txt"] }
 *
 * Build compiles the sources with TinyCC (libtcc, built into this app)
 * against the SDK's C library, links them into this CPU's program and
 * packs build/<name>.bpk; Run installs that package and starts the app. */
#include "code.h"
#include <stdarg.h>
#include "libtcc.h"

char      g_folder[PATH_MAX_];
sbuf_t    g_output;
problem_t g_problems[MAX_PROBLEMS];
int       g_nproblems;

#ifdef __x86_64__
#define CPU "x86_64"
#define TCCLIB "/apps/code/tcc/x86_64"
#else
#define CPU "i686"
#define TCCLIB "/apps/code/tcc/i386"
#endif

void out_clear(void) { sb_free(&g_output); g_nproblems = 0; }

void out_printf(const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
    sb_addn(&g_output, tmp, n);
    g_ui_dirty = 1;
}

static void project_path(char* out, int cap) { join_path(out, cap, g_folder, "banana.json"); }

int project_exists(void) {
    char p[PATH_MAX_];
    if (!g_folder[0]) return 0;
    project_path(p, sizeof(p));
    banana_stat_t st;
    return __banana->stat(p, &st) == 0 && !st.is_dir;
}

/* "file:line: error: message" (TinyCC's form) into the Problems list */
static void add_problem(const char* msg) {
    if (g_nproblems >= MAX_PROBLEMS) return;
    problem_t* p = &g_problems[g_nproblems];
    memset(p, 0, sizeof(*p));
    const char* c1 = strchr(msg, ':');
    while (c1 && !(c1[1] >= '0' && c1[1] <= '9')) c1 = strchr(c1 + 1, ':');
    if (c1) {
        int fl = (int)(c1 - msg);
        if (fl >= (int)sizeof(p->file)) fl = sizeof(p->file) - 1;
        memcpy(p->file, msg, (size_t)fl);
        p->file[fl] = 0;
        p->line = atoi(c1 + 1);
        const char* r = strchr(c1 + 1, ':');
        r = r ? r + 1 : c1 + 1;
        while (*r == ' ') r++;
        p->is_error = strncmp(r, "warning", 7) != 0;
        if (!strncmp(r, "error: ", 7)) r += 7;
        else if (!strncmp(r, "warning: ", 9)) r += 9;
        snprintf(p->msg, sizeof(p->msg), "%s", r);
        /* a relative name: in the project */
        if (p->file[0] && p->file[0] != '/') {
            char full[PATH_MAX_];
            join_path(full, sizeof(full), g_folder, p->file);
            snprintf(p->file, sizeof(p->file), "%s", full);
        }
    }
    if (!c1 || p->line <= 0) {                /* not about a line of a file */
        p->file[0] = 0;
        p->line = 0;
        p->is_error = strstr(msg, "warning") == NULL;
        snprintf(p->msg, sizeof(p->msg), "%s", strncmp(msg, "tcc: ", 5) ? msg : msg + 5);
    }
    g_nproblems++;
}

static int g_errors;
static void tcc_msg(void* opaque, const char* msg) {
    (void)opaque;
    out_printf("%s\n", msg);
    add_problem(msg);
    if (!strstr(msg, "warning")) g_errors++;
}

/* a .bpk: "BPK1", count, { name[56], offset, size } each, then the data */
typedef struct { char name[56]; const char* data; int size; } pent_t;

static int write_bpk(const char* path, pent_t* e, int n) {
    int off = 8 + n * 64, total = off;
    for (int i = 0; i < n; i++) total += e[i].size;
    char* b = calloc(1, (size_t)total);
    if (!b) return -1;
    memcpy(b, "BPK1", 4);
    memcpy(b + 4, &n, 4);
    for (int i = 0; i < n; i++) {
        char* t = b + 8 + i * 64;
        memcpy(t, e[i].name, 55);
        memcpy(t + 56, &off, 4);
        memcpy(t + 60, &e[i].size, 4);
        memcpy(b + off, e[i].data, (size_t)e[i].size);
        off += e[i].size;
    }
    int rc = write_file(path, b, total);
    free(b);
    return rc;
}

static int valid_name(const char* s) {
    int n = 0;
    for (; *s; s++, n++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return n > 0 && n <= 24;
}

static char g_last_app[32];

int build_project(void) {
    out_clear();
    if (!g_folder[0]) { out_printf("Open a folder first (File > Open Folder).\n"); return -1; }
    char pj[PATH_MAX_];
    project_path(pj, sizeof(pj));
    char* t = read_file(pj, NULL);
    if (!t) {
        out_printf("This folder is not a Banana OS app yet: it has no banana.json.\n"
                   "Run \"Port a program to Banana OS\" (Ctrl+Shift+P) to make one for its C files.\n");
        return -1;
    }
    json_t* j = json_parse(t);
    free(t);
    if (!j || j->type != J_OBJ) { out_printf("banana.json: not valid JSON\n"); json_free(j); return -1; }
    const char* name = json_str(j, "name", "");
    if (!valid_name(name)) { out_printf("banana.json: \"name\" must be a-z, 0-9, - or _ (24 at most)\n"); json_free(j); return -1; }
    snprintf(g_last_app, sizeof(g_last_app), "%s", name);
    unsigned t0 = banana_ticks();
    out_printf("Building %s for %s with TinyCC...\n", name, CPU);

    char bdir[PATH_MAX_], prog[PATH_MAX_];
    join_path(bdir, sizeof(bdir), g_folder, "build");
    mkdir_p(bdir);
    join_path(prog, sizeof(prog), bdir, "app-" CPU);

    TCCState* s = tcc_new();
    if (!s) { out_printf("out of memory\n"); json_free(j); return -1; }
    g_errors = 0;
    tcc_set_error_func(s, NULL, tcc_msg);
    /* the options first: the output type adds the start files unless -nostdlib */
    tcc_set_options(s, "-nostdlib -Wall -Wl,-e,_banana_start");
    const char* cf = json_str(j, "cflags", "");
    if (cf[0]) tcc_set_options(s, cf);
    tcc_set_output_type(s, TCC_OUTPUT_DLL);
    tcc_define_symbol(s, "__BANANA_OS__", "1");
    tcc_add_include_path(s, g_folder);
    json_t* srcs = json_get(j, "sources");
    int nsrc = 0;
    for (json_t* f = srcs && srcs->type == J_ARR ? srcs->child : NULL; f; f = f->next) {
        if (f->type != J_STR) continue;
        char p[PATH_MAX_];
        join_path(p, sizeof(p), g_folder, f->str);
        out_printf("  %s\n", f->str);
        g_ui_dirty = 1;
        if (tcc_add_file(s, p) < 0) g_errors++;
        nsrc++;
    }
    if (!nsrc) out_printf("banana.json: no \"sources\"\n"), g_errors++;
    int rc = -1;
    if (!g_errors) {
        tcc_add_file(s, TCCLIB "/libbanana.a");
        tcc_add_file(s, TCCLIB "/libtcc1.a");
        rc = tcc_output_file(s, prog);
        if (rc < 0 || g_errors) rc = -1;
    }
    tcc_delete(s);
    if (rc < 0) {
        out_printf("\nBuild failed (%d problem%s).\n", g_nproblems, g_nproblems == 1 ? "" : "s");
        json_free(j);
        return -1;
    }

    /* the package: manifest, the program, data files */
    sbuf_t mf;
    sb_init(&mf);
    sb_printf(&mf, "name=%s\ntitle=%s\nversion=%s\ntype=%s\n", name, json_str(j, "title", name),
              json_str(j, "version", "1.0"), json_str(j, "type", "console"));
    if (json_str(j, "description", "")[0]) sb_printf(&mf, "description=%s\n", json_str(j, "description", ""));
    if (json_str(j, "author", "")[0]) sb_printf(&mf, "author=%s\n", json_str(j, "author", ""));
    pent_t e[34];
    int ne = 0;
    memset(e, 0, sizeof(e));
    snprintf(e[ne].name, 56, "manifest"); e[ne].data = mf.s; e[ne].size = mf.len; ne++;
    int plen = 0;
    char* pdata = read_file(prog, &plen);
    snprintf(e[ne].name, 56, "app-%s", CPU); e[ne].data = pdata; e[ne].size = plen; ne++;
    char* datas[32];
    int nd = 0;
    json_t* data = json_get(j, "data");
    for (json_t* f = data && data->type == J_ARR ? data->child : NULL; f && ne < 34 && nd < 32; f = f->next) {
        if (f->type != J_STR || strstr(f->str, "..") || f->str[0] == '/') continue;
        char p[PATH_MAX_];
        join_path(p, sizeof(p), g_folder, f->str);
        int l = 0;
        datas[nd] = read_file(p, &l);
        if (!datas[nd]) { out_printf("data file %s is missing\n", f->str); continue; }
        snprintf(e[ne].name, 56, "%s", f->str);
        e[ne].data = datas[nd++];
        e[ne].size = l;
        ne++;
    }
    char bpk[PATH_MAX_];
    char bn[64];
    snprintf(bn, sizeof(bn), "%s.bpk", name);
    join_path(bpk, sizeof(bpk), bdir, bn);
    rc = pdata ? write_bpk(bpk, e, ne) : -1;
    free(pdata);
    for (int i = 0; i < nd; i++) free(datas[i]);
    sb_free(&mf);
    json_free(j);
    if (rc < 0) { out_printf("could not write %s\n", bpk); return -1; }
    out_printf("\nBuilt build/%s (%u KB, %u ms)%s.\n", bn, (unsigned)(plen / 1024 + 1), banana_ticks() - t0,
               g_nproblems ? " with warnings" : "");
    out_printf("It runs on %s Banana OS (the SDK on Linux builds packages for both CPUs).\n", CPU);
    return 0;
}

int run_project(void) {
    if (build_project() != 0) return -1;
    char bpk[PATH_MAX_], bn[64], msg[200];
    snprintf(bn, sizeof(bn), "build/%s.bpk", g_last_app);
    join_path(bpk, sizeof(bpk), g_folder, bn);
    if (banana_pkg_install(bpk, msg, sizeof(msg)) != 0) { out_printf("Install: %s\n", msg); return -1; }
    out_printf("Installed: %s\n", msg);
    char* argv[1] = { g_last_app };
    if (banana_app_run(g_last_app, 1, argv, msg, sizeof(msg)) != 0) { out_printf("Run: %s\n", msg); return -1; }
    out_printf("Started %s.\n", g_last_app);
    return 0;
}

/* ── new projects ── */
static const char* CONSOLE_MAIN =
    "/* %s - a Banana OS console app (it runs in a terminal) */\n"
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "\n"
    "int main(int argc, char** argv) {\n"
    "    printf(\"Hello from %s!\\n\");\n"
    "    char line[128];\n"
    "    printf(\"What is your name? \");\n"
    "    if (fgets(line, sizeof(line), stdin)) {\n"
    "        line[strcspn(line, \"\\n\")] = 0;\n"
    "        printf(\"Nice to meet you, %%s.\\n\", line);\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

static const char* GUI_MAIN =
    "/* %s - a Banana OS desktop app: a window, the mouse and the keyboard */\n"
    "#include <banana.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "int main(int argc, char** argv) {\n"
    "    bwin_t win;\n"
    "    if (bwin_open(&win, \"%s\", 480, 320) != 0) return 1;\n"
    "    int clicks = 0;\n"
    "    for (;;) {\n"
    "        bwin_clear(&win, BANANA_DARK);\n"
    "        bwin_font(&win, 20, 20, BANANA_FONT_SANS_BOLD, 22, \"Hello from %s!\", BANANA_YELLOW);\n"
    "        char line[64];\n"
    "        snprintf(line, sizeof(line), \"Clicks: %%d\", clicks);\n"
    "        bwin_font(&win, 20, 60, BANANA_FONT_SANS, 16, line, BANANA_WHITE);\n"
    "        bwin_button(&win, 20, 100, 120, 28, \"Click me\", 0);\n"
    "        bwin_update(&win);\n"
    "        banana_event_t ev;\n"
    "        if (!bwin_wait_event(&win, &ev, -1)) continue;\n"
    "        if (ev.type == BANANA_EV_CLOSE) break;\n"
    "        if (ev.type == BANANA_EV_MOUSE_DOWN && ev.x >= 20 && ev.x < 140 && ev.y >= 100 && ev.y < 128) clicks++;\n"
    "    }\n"
    "    bwin_close(&win);\n"
    "    return 0;\n"
    "}\n";

int new_project(const char* parent, const char* name, int gui, char* out_dir, int cap) {
    if (!valid_name(name)) return -1;
    char dir[PATH_MAX_], p[PATH_MAX_];
    join_path(dir, sizeof(dir), parent, name);
    if (mkdir_p(dir) != 0) return -1;
    sbuf_t b;
    sb_init(&b);
    sb_printf(&b, "{\n  \"name\": \"%s\",\n  \"title\": \"%s\",\n  \"version\": \"1.0\",\n  \"type\": \"%s\",\n"
                  "  \"description\": \"My Banana OS app\",\n  \"sources\": [\"main.c\"]\n}\n", name, name, gui ? "gui" : "console");
    join_path(p, sizeof(p), dir, "banana.json");
    write_file(p, b.s, b.len);
    sb_free(&b);
    char src[2048];
    if (gui) snprintf(src, sizeof(src), GUI_MAIN, name, name, name);
    else snprintf(src, sizeof(src), CONSOLE_MAIN, name, name);
    join_path(p, sizeof(p), dir, "main.c");
    write_file(p, src, (int)strlen(src));
    snprintf(out_dir, (size_t)cap, "%s", dir);
    return 0;
}

/* "Port a program": every .c file of the folder becomes a source */
int port_project(void) {
    if (!g_folder[0]) return -1;
    const char* base = strrchr(g_folder, '/');
    base = base ? base + 1 : g_folder;
    char name[32];
    int k = 0;
    for (const char* c = base; *c && k < 24; c++) {
        char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') name[k++] = ch;
    }
    name[k] = 0;
    if (!k) snprintf(name, sizeof(name), "app");
    sbuf_t b;
    sb_init(&b);
    sb_printf(&b, "{\n  \"name\": \"%s\",\n  \"title\": \"%s\",\n  \"version\": \"1.0\",\n  \"type\": \"console\",\n"
                  "  \"description\": \"Ported to Banana OS\",\n  \"sources\": [", name, base);
    banana_dirent_t e;
    int n = 0, has_main = 0;
    for (int i = 0; __banana->readdir(g_folder, i, &e) == 0; i++) {
        int l = (int)strlen(e.name);
        if (e.is_dir || l < 3 || strcmp(e.name + l - 2, ".c")) continue;
        sb_printf(&b, "%s\"%s\"", n ? ", " : "", e.name);
        n++;
        char p[PATH_MAX_];
        join_path(p, sizeof(p), g_folder, e.name);
        char* t = read_file(p, NULL);
        if (t && (strstr(t, "int main(") || strstr(t, "int main (") || strstr(t, "void main("))) has_main = 1;
        if (t && strstr(t, "bwin_open")) { /* a window: a desktop app */ }
        free(t);
    }
    sb_add(&b, "],\n  \"cflags\": \"\"\n}\n");
    char pj[PATH_MAX_];
    project_path(pj, sizeof(pj));
    int rc = n ? write_file(pj, b.s, b.len) : -1;
    sb_free(&b);
    out_clear();
    if (!n) { out_printf("No .c files in %s.\n", g_folder); return -1; }
    out_printf("Made banana.json for %d C file%s.%s\n", n, n == 1 ? "" : "s",
               has_main ? "" : "\nNo main() was found: the app needs one.");
    out_printf("Banana OS has a C library with stdio, stdlib, string, math, time, setjmp and POSIX files\n"
               "(open/read/write/stat/mkdir). There is no fork/exec, signals, sockets or threads API from\n"
               "POSIX: use banana.h for windows, threads (banana_thread), the network (banana_http_*).\n"
               "Press F7 to build; the Problems tab lists what does not compile yet.\n");
    return rc;
}
