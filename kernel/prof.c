/* SPDX-License-Identifier: GPL-3.0-or-later */
/* prof: a sampling profiler - where every processor core spends its time.
 *
 * While it runs, each core is looked at once a millisecond: the boot core
 * from its timer interrupt, the others from an interrupt sent to them on
 * that tick (the scheduler's "kick", which does nothing else unless their
 * task's turn is over). A look records where the core was - halted (idle),
 * in an app's code, or in the kernel (and then in which system call the
 * task is) - and which task it ran. The report gives, over all of it: how
 * busy each core was, the tasks, the system calls the kernel's time went to,
 * and the hottest places in app code (library + function, from its exported
 * symbols) and in the kernel (function names: the kernel carries its symbol
 * table, tools/mkksyms.py). For a game at 100% CPU this says what it is. */
#include "prof.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "task.h"
#include "smp.h"
#include "fs.h"
#include "serial.h"

typedef struct { uintptr_t ip; uint16_t pid; uint8_t cpu, mode; uint16_t sys, pad; } psample_t;
enum { M_IDLE = 0, M_KERNEL = 1, M_USER = 2 };

#define PROF_MAX (4u * 1000u * 64u)        /* 64 s of 4 cores, or 16 s of 16 */
static psample_t* g_s;
static volatile uint32_t g_n, g_cap;
static volatile int g_on;
static volatile uint32_t g_until;

/* the kernel's symbol table (tools/mkksyms.py, linked in a second pass):
 * count, addresses (sorted), name offsets, names. Empty in the first pass
 * and in the 32-bit kernel. */
__attribute__((weak)) const uint32_t g_ksyms[1] = { 0 };

static const char* ksym(uintptr_t ip, uint32_t* off) {
    const uint32_t* K = g_ksyms;
    __asm__("" : "+r"(K));                       /* (its real size is the second pass's) */
    uint32_t n = K[0];
    if (!n || (uint64_t)ip >> 32) return NULL;
    const uint32_t* addr = K + 1;
    const uint32_t* noff = addr + n;
    const char* names = (const char*)(noff + n);
    uint32_t lo = 0, hi = n;
    if ((uint32_t)ip < addr[0]) return NULL;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (addr[mid] <= (uint32_t)ip) lo = mid; else hi = mid;
    }
    *off = (uint32_t)ip - addr[lo];
    return names + noff[lo];
}

int app_prof_insys(int pid);
const char* app_prof_sysname(int code);

void prof_hit(uintptr_t ip, int user) {
    if (!g_on) return;
    uint32_t i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i >= g_cap) return;
    psample_t* s = &g_s[i];
    int pid = task_current_pid();
    s->ip = ip;
    s->pid = (uint16_t)(pid < 0 ? 0xFFFF : pid);
    s->cpu = (uint8_t)cpu_id();
    s->sys = (uint16_t)app_prof_insys(pid);               /* (the system call it is in, now) */
    /* halted: the instruction before the one it returns to is hlt (the idle loops) */
    s->mode = user ? M_USER : (ip > 0x100000 && *(const volatile uint8_t*)(ip - 1) == 0xF4) ? M_IDLE : M_KERNEL;
}

void prof_tick(uint32_t ms) {
    if (!g_on) return;
    if ((int32_t)(ms - g_until) >= 0) { g_on = 0; return; }
    for (int c = 1; c < cpu_count(); c++) smp_send_ipi(c, SMP_VEC_KICK);
}

int prof_running(void) { return g_on; }

/* ── the report ── */
typedef struct { uint64_t key; uint32_t n, user, kern; char what[72]; } bucket_t;
#define NB 2048
static bucket_t* g_b;

static bucket_t* bucket(uint64_t key, const char* what) {
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) % NB;
    for (uint32_t k = 0; k < NB; k++, h = (h + 1) % NB) {
        bucket_t* b = &g_b[h];
        if (b->n && b->key == key) return b;
        if (!b->n) { b->key = key; if (what) kstrlcpy(b->what, what, sizeof(b->what)); return b; }
    }
    return NULL;
}

static void sort_desc(bucket_t** v, int n) {
    for (int i = 1; i < n; i++) {
        bucket_t* x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j]->n < x->n) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

static void pct(char* out, int cap, uint32_t a, uint32_t total) {
    uint32_t t = total ? (uint32_t)((uint64_t)a * 1000u / total) : 0;
    ksnprintf(out, (size_t)cap, "%3u.%u%%", t / 10, t % 10);
}

/* the buckets of one kind (keys tagged in their top byte), the top max */
static void top(prof_out_t out, void* ctx, uint8_t kind, int max, uint32_t total, const char* title) {
    static bucket_t* v[NB];
    int n = 0;
    for (int i = 0; i < NB; i++) if (g_b[i].n && (uint8_t)(g_b[i].key >> 56) == kind) v[n++] = &g_b[i];
    if (!n) return;
    sort_desc(v, n);
    out(ctx, title);
    char line[160], p[16];
    for (int i = 0; i < n && i < max; i++) {
        pct(p, sizeof(p), v[i]->n, total);
        if (kind == 1) {
            char u[16], k[16];
            pct(u, sizeof(u), v[i]->user, total);
            pct(k, sizeof(k), v[i]->kern, total);
            ksnprintf(line, sizeof(line), "  %s  (app code %s, kernel %s)  %s", p, u, k, v[i]->what);
        } else {
            ksnprintf(line, sizeof(line), "  %s  %s", p, v[i]->what);
        }
        out(ctx, line);
    }
}

const char* app_prof_syscall(int pid);
const char* app_prof_name(int pid);
uint64_t    app_prof_where(int pid, uintptr_t ip, char* out, int cap);
int         app_prof_lib(int pid, uintptr_t ip, char* name, int cap, uint64_t* off);
void        app_prof_reset(void);

/* ── a library's full symbol table, from its file (.symtab: every
 * function, the internal ones too - Mesa's driver, the JVM) ── */
typedef struct { uint64_t addr, size; uint32_t name; } fsym_t;
typedef struct { char so[128]; int tried; fsym_t* sym; uint32_t n; char* str; } libsyms_t;
#define NLIB 24
static libsyms_t g_lib[NLIB];

/* the file called name somewhere under dir (depth levels down) */
static int find_file(const char* dir, const char* name, int depth, char* out, int cap) {
    static int idx[256];
    char path[256];
    int n = fs_list_files(dir, idx, 256);
    for (int i = 0; i < n && i < 256; i++) {
        fs_file_t* f = fs_file_info(idx[i]);
        if (f && !strcmp(f->name, name)) { ksnprintf(out, (size_t)cap, "%s/%s", strcmp(dir, "/") ? dir : "", name); return 1; }
    }
    if (depth <= 0) return 0;
    int dirs[64];
    n = fs_list_dirs(dir, dirs, 64);
    for (int i = 0; i < n && i < 64; i++) {
        const fs_dir_t* d = fs_get_dir(dirs[i]);
        if (!d) continue;
        ksnprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", d->name);
        if (find_file(path, name, depth - 1, out, cap)) return 1;
    }
    return 0;
}

static int rd(int fi, uint64_t off, void* buf, uint32_t len) {
    uint32_t got = 0;
    while (got < len) {
        int r = fs_read(fi, (uint32_t)off + got, (uint8_t*)buf + got, len - got);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

static int sym_cmp_le(const fsym_t* a, const fsym_t* b) { return a->addr <= b->addr; }

static void load_syms(libsyms_t* L) {
    L->tried = 1;
    char path[256];
    if (L->so[0] == '/') kstrlcpy(path, L->so, sizeof(path));               /* (an app's own file) */
    else if (!find_file("/apps", L->so, 4, path, sizeof(path)) && !find_file("/lib", L->so, 2, path, sizeof(path)) &&
        !find_file("/usr/lib", L->so, 2, path, sizeof(path)) && !find_file("/bin", L->so, 0, path, sizeof(path))) return;
    int fi = fs_find_file(path);
    if (fi < 0) return;
    uint8_t eh[64];
    if (rd(fi, 0, eh, 64) || memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 2) return;
    uint64_t shoff; uint16_t shentsize, shnum;
    memcpy(&shoff, eh + 40, 8); memcpy(&shentsize, eh + 58, 2); memcpy(&shnum, eh + 60, 2);
    if (shentsize != 64 || !shnum || shnum > 4096) return;
    uint8_t* sh = (uint8_t*)kmalloc((uint32_t)shnum * 64u);
    if (!sh || rd(fi, shoff, sh, (uint32_t)shnum * 64u)) { kfree(sh); return; }
    for (int i = 0; i < shnum; i++) {
        uint32_t type, link; uint64_t off, size;
        memcpy(&type, sh + i * 64 + 4, 4); memcpy(&off, sh + i * 64 + 24, 8);
        memcpy(&size, sh + i * 64 + 32, 8); memcpy(&link, sh + i * 64 + 40, 4);
        if (type != 2 || link >= shnum || size > (64u << 20)) continue;            /* SHT_SYMTAB */
        uint64_t soff, ssize;
        memcpy(&soff, sh + link * 64 + 24, 8); memcpy(&ssize, sh + link * 64 + 32, 8);
        if (ssize > (64u << 20)) break;
        uint8_t* raw = (uint8_t*)kmalloc((uint32_t)size);
        char* str = (char*)kmalloc((uint32_t)ssize + 1);
        uint32_t cnt = (uint32_t)(size / 24), n = 0;
        fsym_t* v = (fsym_t*)kmalloc(cnt * sizeof(fsym_t) + 1);
        if (!raw || !str || !v || rd(fi, off, raw, (uint32_t)size) || rd(fi, soff, str, (uint32_t)ssize)) {
            kfree(raw); kfree(str); kfree(v); break;
        }
        str[ssize] = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t* e = raw + k * 24;
            uint32_t nm; uint64_t val, sz;
            memcpy(&nm, e, 4); memcpy(&val, e + 8, 8); memcpy(&sz, e + 16, 8);
            if ((e[4] & 0xF) != 2 || !val || nm >= ssize) continue;                 /* functions */
            v[n++] = (fsym_t){ val, sz, nm };
        }
        kfree(raw);
        static const uint32_t gaps[] = { 13801, 4057, 1750, 701, 301, 132, 57, 23, 10, 4, 1 };   /* (Shell sort) */
        for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
            for (uint32_t a = gaps[g]; a < n; a++) {
                fsym_t x = v[a];
                uint32_t b = a;
                while (b >= gaps[g] && !sym_cmp_le(&v[b - gaps[g]], &x)) { v[b] = v[b - gaps[g]]; b -= gaps[g]; }
                v[b] = x;
            }
        L->sym = v; L->n = n; L->str = str;
        klog("prof: %u functions of %s (%s)\n", n, L->so, path);
        break;
    }
    kfree(sh);
}

/* fn+off of offset off in library so (from its file), 0 if not known */
static int lib_sym(const char* so, uint64_t off, char* out, int cap, uint64_t* key) {
    libsyms_t* L = NULL;
    for (int i = 0; i < NLIB; i++) {
        if (g_lib[i].so[0] && !strcmp(g_lib[i].so, so)) { L = &g_lib[i]; break; }
        if (!g_lib[i].so[0]) { L = &g_lib[i]; kstrlcpy(L->so, so, sizeof(L->so)); break; }
    }
    if (!L) return 0;
    if (!L->tried) load_syms(L);
    if (!L->n || off < L->sym[0].addr) return 0;
    uint32_t lo = 0, hi = L->n;
    while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (L->sym[m].addr <= off) lo = m; else hi = m; }
    const fsym_t* f = &L->sym[lo];
    const char* shown = strrchr(so, '/');
    ksnprintf(out, (size_t)cap, "%s (%s%s+0x%x)", shown ? shown + 1 : so, off < f->addr + f->size ? "" : "~", L->str + f->name, (uint32_t)(off - f->addr));
    *key = f->addr;
    return 1;
}
static void lib_free(void) {
    for (int i = 0; i < NLIB; i++) { kfree(g_lib[i].sym); kfree(g_lib[i].str); }
    memset(g_lib, 0, sizeof(g_lib));
}

static struct { uint32_t per_cpu[64], busy_cpu[64], idle, user, kern; } g_acc;
static task_info_t g_ti[TASK_MAX];
static int g_nt;
static void snap_tasks(void) {
    int nt = task_count();
    g_nt = nt > TASK_MAX ? TASK_MAX : nt;
    task_snapshot(g_ti, g_nt);
}
static void account(const psample_t* s) {
    char where[128];
    int c = s->cpu < 64 ? s->cpu : 63;
    g_acc.per_cpu[c]++;
    if (s->mode == M_IDLE) { g_acc.idle++; return; }
    g_acc.busy_cpu[c]++;
    int pid = s->pid == 0xFFFF ? -1 : s->pid;
    /* the task */
    const char* tname = "?";
    for (int k = 0; k < g_nt; k++) if ((int)g_ti[k].pid == pid) { tname = g_ti[k].name; break; }
    const char* app = app_prof_name(pid);
    ksnprintf(where, sizeof(where), "%s%s%s (task %d)", tname, app && strcmp(app, tname) ? " - app " : "", app && strcmp(app, tname) ? app : "", pid);
    bucket_t* b = bucket(1ull << 56 | (uint64_t)(uint16_t)pid, where);
    if (b) { b->n++; if (s->mode == M_USER) b->user++; else b->kern++; }
    if (s->mode == M_USER) {
        g_acc.user++;
        char lib[128];
        uint64_t off = 0, key = 0;
        int kind = app_prof_lib(pid, s->ip, lib, sizeof(lib), &off);
        if ((kind == 1 || kind == 2) && lib_sym(lib, off, where, sizeof(where), &key)) {
            uint64_t h = 5381;
            for (const char* q = lib; *q; q++) h = h * 33 + (uint8_t)*q;
            key ^= h << 36;                                  /* (one bucket a function of a library) */
        } else if (kind == 3) {
            kstrlcpy(where, "JIT / generated code (no library: Java's compiled code)", sizeof(where));
            key = 2;
        } else if (kind == 1 || kind == 2) {                 /* (no symbols in its file: 4 KB pieces) */
            const char* shown = strrchr(lib, '/');
            ksnprintf(where, sizeof(where), "%s+0x%lx (no symbols)", shown ? shown + 1 : lib, (unsigned long)(off & ~0xFFFull));
            uint64_t h = 5381;
            for (const char* q = lib; *q; q++) h = h * 33 + (uint8_t)*q;
            key = (h << 32) ^ (off & ~0xFFFull) ^ 0x5000000000000ull;
        } else {
            kstrlcpy(where, "(an app that ended, or code not found)", sizeof(where));
            key = 3;
        }
        b = bucket(3ull << 56 | (key & 0x00FFFFFFFFFFFFFFull), where);
        if (b) b->n++;
    } else {
        g_acc.kern++;
        const char* sc = s->sys ? app_prof_sysname(s->sys) : NULL;
        ksnprintf(where, sizeof(where), "%s", sc ? sc : "(not in a system call: interrupts, kernel tasks, the desktop)");
        uint64_t h = 5381;
        for (const char* q = where; *q; q++) h = h * 33 + (uint8_t)*q;
        b = bucket(2ull << 56 | (h & 0x00FFFFFFFFFFFFFFull), where);
        if (b) b->n++;
        uint32_t off = 0;
        const char* fn = ksym(s->ip, &off);
        if (fn) ksnprintf(where, sizeof(where), "%s+0x%x", fn, off);
        else ksnprintf(where, sizeof(where), "kernel 0x%lx", (unsigned long)s->ip);
        b = bucket(4ull << 56 | (fn ? (uint64_t)(uintptr_t)fn : (uint64_t)(s->ip & ~(uintptr_t)0xFF)), fn ? fn : where);
        if (b) b->n++;
    }
}

int prof_run(uint32_t seconds, prof_out_t out, void* ctx) {
    if (g_on) { out(ctx, "prof: already running"); return -1; }
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    int cores = cpu_count() < 1 ? 1 : cpu_count();
    uint32_t cap = seconds * 1000u * (uint32_t)cores + 4000u;
    if (cap > PROF_MAX) cap = PROF_MAX;
    g_s = (psample_t*)kmalloc(cap * sizeof(psample_t));
    g_b = (bucket_t*)kzalloc(NB * sizeof(bucket_t));
    if (!g_s || !g_b) { kfree(g_s); kfree(g_b); g_s = NULL; g_b = NULL; out(ctx, "prof: out of memory"); return -1; }
    g_cap = cap;
    g_n = 0;
    uint32_t t0 = timer_ms();
    g_until = t0 + seconds * 1000u;
    memset(&g_acc, 0, sizeof(g_acc));
    app_prof_reset();
    g_nt = 0;
    __sync_synchronize();
    g_on = 1;
    /* each 100 ms, the looks of the round before are looked up while their
     * apps still run (a program that ends takes its libraries with it) */
    uint32_t done = 0, seen = 0;
    while (g_on && (int32_t)(timer_ms() - g_until) < 200) {
        task_sleep_ms(100);
        uint32_t now = g_n < g_cap ? g_n : g_cap;
        snap_tasks();
        for (; done < seen; done++) account(&g_s[done]);
        seen = now;
    }
    g_on = 0;
    __sync_synchronize();
    task_sleep_ms(5);                         /* (a look being written on another core) */
    uint32_t n = g_n < g_cap ? g_n : g_cap;
    snap_tasks();
    for (; done < n; done++) account(&g_s[done]);
    uint32_t idle = g_acc.idle, user = g_acc.user, kern = g_acc.kern;
    uint32_t* per_cpu = g_acc.per_cpu; uint32_t* busy_cpu = g_acc.busy_cpu;
    char line[200];
    uint32_t ms = timer_ms() - t0;
    ksnprintf(line, sizeof(line), "prof: %u.%u s, %d cores, %u looks (each core once a millisecond)%s",
              ms / 1000, (ms % 1000) / 100, cores, n, g_n > g_cap ? " - buffer full, the rest not counted" : "");
    out(ctx, line);
    char a[16], b2[16], c3[16];
    pct(a, sizeof(a), n - idle, n); pct(b2, sizeof(b2), user, n); pct(c3, sizeof(c3), kern, n);
    ksnprintf(line, sizeof(line), "all cores: %s busy - app code %s, kernel %s", a, b2, c3);
    out(ctx, line);
    for (int c = 0; c < cores && c < 64; c++) {
        pct(a, sizeof(a), busy_cpu[c], per_cpu[c]);
        ksnprintf(line, sizeof(line), "  cpu %d: %s busy (%u looks)", c, a, per_cpu[c]);
        out(ctx, line);
    }
    top(out, ctx, 1, 15, n, "tasks (share of all the looks):");
    top(out, ctx, 2, 12, n, "kernel time by system call:");
    top(out, ctx, 3, 30, n, "app code, hottest first (library (function+offset); ~ = nearest exported function):");
    top(out, ctx, 4, 25, n, "kernel code, hottest first:");
    kfree(g_s); g_s = NULL;
    kfree(g_b); g_b = NULL;
    lib_free();
    return 0;
}
