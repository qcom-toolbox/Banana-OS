/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS SDK: shared libraries (<dlfcn.h>, <link.h>) - an ELF loader in
 * the app. A library is mapped (mmap), relocated at once and its
 * constructors run; its undefined symbols come from the app's export
 * table (made by banana.mk from the app's and the SDK's objects), the
 * libraries opened RTLD_GLOBAL, then its own DT_NEEDED libraries. */
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* the app's symbols (banana.mk: build/<cpu>/exports.c), sorted by name */
struct __dl_export { const char* name; void* addr; };
extern const struct __dl_export __dl_exports[] __attribute__((weak));
extern const unsigned char __ehdr_start[] __attribute__((weak, visibility("hidden")));

#ifdef __x86_64__
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
#define ELF_MACHINE 62
#define ELF_CLASS   2
#else
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct { int32_t tag; uint32_t val; } Dyn;
typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Sym;
#define ELF_MACHINE 3
#define ELF_CLASS   1
#endif
typedef ElfW_Phdr Phdr;

enum {
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
    DT_RELASZ = 8, DT_INIT = 12, DT_FINI = 13, DT_REL = 17, DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23,
    DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
};
#define DT_GNU_HASH 0x6ffffef5
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_TLS     7

typedef struct lib {
    struct lib*  next;
    char         path[256];
    const char*  name;                 /* its file name (no folder) */
    int          refs, global, nodelete;
    uintptr_t    base;
    void*        map;
    size_t       map_len;
    Phdr*        phdr;                 /* (a copy: the file is gone) */
    int          phnum;
    const Sym*   symtab;
    const char*  strtab;
    const uint32_t* hash;              /* DT_HASH (sysv) */
    const uint32_t* gnu;               /* DT_GNU_HASH */
    uint32_t     nsyms;
    struct lib*  deps[16];
    int          ndeps;
    void       (**fini_array)(void);
    size_t       nfini;
    void       (*fini)(void);
    size_t       tls_mod;              /* its thread-local block's module number (0: none) */
} lib_t;

static lib_t g_self = { .name = "app", .refs = 1, .global = 1, .nodelete = 1 };
static lib_t* g_libs;                 /* the libraries loaded, newest first */
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static char g_err[300];
static int g_has_err;

static void set_err(const char* fmt, const char* a, const char* b) {
    snprintf(g_err, sizeof(g_err), fmt, a ? a : "", b ? b : "");
    g_has_err = 1;
}

char* dlerror(void) {
    if (!g_has_err) return NULL;
    g_has_err = 0;
    return g_err;
}

/* ── symbols ──────────────────────────────────────────────────────── */

static uint32_t sysv_hash(const char* s) {
    uint32_t h = 0;
    for (; *s; s++) {
        h = (h << 4) + (uint8_t)*s;
        uint32_t g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char* s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

static int sym_ok(const lib_t* l, const Sym* s, const char* name) {
    int bind = s->info >> 4, type = s->info & 15;
    return s->shndx != 0 && (bind == 1 || bind == 2) && type != 3 /* section */ && !strcmp(l->strtab + s->name, name);
}

/* name defined in library l: its address, or 0 */
static void* lib_sym(const lib_t* l, const char* name) {
    if (l == &g_self || !l->symtab) return NULL;
    if (l->gnu) {
        const uint32_t* g = l->gnu;
        uint32_t nb = g[0], symoff = g[1], bloom = g[2];
        const uint32_t* buckets = g + 4 + bloom * (sizeof(uintptr_t) / 4);
        const uint32_t* chains = buckets + nb;
        uint32_t h = gnu_hash(name);
        uint32_t i = nb ? buckets[h % nb] : 0;
        if (!i) return NULL;
        for (;; i++) {
            uint32_t ch = chains[i - symoff];
            if ((ch | 1) == (h | 1) && sym_ok(l, &l->symtab[i], name)) return (void*)(l->base + l->symtab[i].value);
            if (ch & 1) return NULL;
        }
    }
    if (l->hash) {
        uint32_t nb = l->hash[0];
        const uint32_t* bucket = l->hash + 2;
        const uint32_t* chain = bucket + nb;
        for (uint32_t i = nb ? bucket[sysv_hash(name) % nb] : 0; i; i = chain[i])
            if (sym_ok(l, &l->symtab[i], name)) return (void*)(l->base + l->symtab[i].value);
        return NULL;
    }
    for (uint32_t i = 1; i < l->nsyms; i++)
        if (sym_ok(l, &l->symtab[i], name)) return (void*)(l->base + l->symtab[i].value);
    return NULL;
}

/* the app's own (its exports table, sorted) */
static void* app_sym(const char* name) {
    if (!__dl_exports) return NULL;
    int n = 0;
    while (__dl_exports[n].name) n++;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, __dl_exports[mid].name);
        if (!c) return __dl_exports[mid].addr;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return NULL;
}

/* the global scope: the app, then the libraries opened RTLD_GLOBAL (oldest first) */
static void* global_sym(const char* name) {
    void* a = app_sym(name);
    if (a) return a;
    lib_t* order[64];
    int n = 0;
    for (lib_t* l = g_libs; l && n < 64; l = l->next) order[n++] = l;
    for (int i = n - 1; i >= 0; i--)
        if (order[i]->global && (a = lib_sym(order[i], name))) return a;
    return NULL;
}

/* l and the libraries it needs, breadth first */
static void* tree_sym(lib_t* l, const char* name) {
    lib_t* q[64];
    int head = 0, tail = 0;
    q[tail++] = l;
    while (head < tail) {
        lib_t* x = q[head++];
        void* a = lib_sym(x, name);
        if (a) return a;
        for (int i = 0; i < x->ndeps && tail < 64; i++) q[tail++] = x->deps[i];
    }
    return NULL;
}

/* ── loading ──────────────────────────────────────────────────────── */

static lib_t* load(const char* name, const char* from_dir, int flags);
static int is_system_libc(const char* name);

static int read_file(const char* path, unsigned char** out, size_t* len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    unsigned char* b = size > 0 ? malloc((size_t)size) : NULL;
    if (!b || read(fd, b, (size_t)size) != size) { free(b); close(fd); return -1; }
    close(fd);
    *out = b;
    *len = (size_t)size;
    return 0;
}

static int file_exists(const char* path);

/* where a library named without a folder is looked for */
static int find(const char* name, const char* from_dir, char* out, size_t cap) {
    if (strchr(name, '/')) { snprintf(out, cap, "%s", name); return file_exists(out); }
    const char* dirs[4] = { from_dir, ".", "/lib", "/usr/lib" };
    for (int i = 0; i < 4; i++) {
        if (!dirs[i] || !dirs[i][0]) continue;
        snprintf(out, cap, "%s/%s", dirs[i], name);
        if (file_exists(out)) return 1;
    }
    return 0;
}



static void run_init(lib_t* l, const Dyn* dyn) {
    void (*init)(void) = NULL;
    void (**arr)(void) = NULL;
    size_t n = 0;
    for (const Dyn* d = dyn; d->tag != DT_NULL; d++) {
        if (d->tag == DT_INIT) init = (void (*)(void))(l->base + d->val);
        if (d->tag == DT_INIT_ARRAY) arr = (void (**)(void))(l->base + d->val);
        if (d->tag == DT_INIT_ARRAYSZ) n = (size_t)d->val / sizeof(void*);
        if (d->tag == DT_FINI) l->fini = (void (*)(void))(l->base + d->val);
        if (d->tag == DT_FINI_ARRAY) l->fini_array = (void (**)(void))(l->base + d->val);
        if (d->tag == DT_FINI_ARRAYSZ) l->nfini = (size_t)d->val / sizeof(void*);
    }
    if (init) init();
    for (size_t i = 0; i < n; i++) if (arr[i] && arr[i] != (void (*)(void))-1) arr[i]();
}

/* ── thread-local variables (PT_TLS: the general and local dynamic models)
 * Each library with a PT_TLS segment is a module; a thread gets its copy
 * of the module's block at its first access (__tls_get_addr), kept with a
 * pthread key and freed when the thread ends. */

typedef struct {
    const void*   image;               /* the initial values (filesz bytes, then zeros) */
    size_t        filesz, memsz, align;
    pthread_key_t key;
} tls_mod_t;
static tls_mod_t g_tls[32];
static size_t g_ntls;

static size_t tls_add(const void* image, size_t filesz, size_t memsz, size_t align) {
    if (g_ntls >= 32) return 0;
    tls_mod_t* m = &g_tls[g_ntls];
    if (pthread_key_create(&m->key, free) != 0) return 0;
    m->image = image;
    m->filesz = filesz;
    m->memsz = memsz;
    m->align = align < sizeof(void*) ? sizeof(void*) : align;
    return ++g_ntls;
}

typedef struct { size_t mod, off; } tls_index;

void* __tls_get_addr(tls_index* ti) {
    if (!ti->mod || ti->mod > g_ntls) abort();
    tls_mod_t* m = &g_tls[ti->mod - 1];
    char* b = pthread_getspecific(m->key);
    if (!b) {
        if (posix_memalign((void**)&b, m->align, m->memsz) != 0) abort();
        memcpy(b, m->image, m->filesz);
        memset(b + m->filesz, 0, m->memsz - m->filesz);
        pthread_setspecific(m->key, b);
    }
    return b + ti->off;
}

/* one table of relocations (rela: with addends) */
static int relocate(lib_t* l, uintptr_t at, size_t size, int rela) {
    size_t ent = rela ? 3 * sizeof(uintptr_t) : 2 * sizeof(uintptr_t);
    for (size_t o = 0; o + ent <= size; o += ent) {
        const uintptr_t* r = (const uintptr_t*)(l->base + at + o);
#ifdef __x86_64__
        uint32_t type = (uint32_t)r[1], si = (uint32_t)(r[1] >> 32);
#else
        uint32_t type = r[1] & 0xFF, si = r[1] >> 8;
#endif
        if (!type) continue;
        uintptr_t* P = (uintptr_t*)(l->base + r[0]);
        intptr_t A = rela ? (intptr_t)r[2] : (intptr_t)*P;
        if (type == 8) { *P = l->base + (uintptr_t)A; continue; }              /* RELATIVE */
#ifdef __x86_64__
        if (type == 37) { *P = ((uintptr_t (*)(void))(l->base + (uintptr_t)A))(); continue; }   /* IRELATIVE */
#else
        if (type == 42) { *P = ((uintptr_t (*)(void))(l->base + (uintptr_t)A))(); continue; }
#endif
#ifdef __x86_64__
        if (type == 16 && !si) { *P = l->tls_mod; continue; }                  /* DTPMOD64 (local dynamic) */
#endif
        uintptr_t S = 0;
        if (si) {
            const Sym* s = &l->symtab[si];
            const char* name = l->strtab + s->name;
            void* a = NULL;
            if ((s->info >> 4) == 0 && s->shndx) a = (void*)(l->base + s->value);          /* a local one */
            else if (!(a = global_sym(name)) && !(a = tree_sym(l, name)) && s->shndx) a = (void*)(l->base + s->value);
            if (!a && (s->info >> 4) != 2) { set_err("%s: undefined symbol: %s", l->path, name); return -1; }
            S = (uintptr_t)a;
            if ((s->info & 15) == 6) {                                         /* STT_TLS: an offset in its block */
                if (!s->shndx) { set_err("%s: thread-local %s from another library", l->path, name); return -1; }
                S = (uintptr_t)s->value;
            }
        }
        switch (type) {
#ifdef __x86_64__
        case 1:  *P = S + (uintptr_t)A; break;                                     /* 64 */
        case 2:  *(uint32_t*)P = (uint32_t)(S + (uintptr_t)A - (uintptr_t)P); break; /* PC32 */
        case 6: case 7: *P = S + (uintptr_t)A; break;                              /* GLOB_DAT, JUMP_SLOT */
        case 10: case 11: *(uint32_t*)P = (uint32_t)(S + (uintptr_t)A); break;     /* 32, 32S */
        case 16: *P = l->tls_mod; break;                                           /* DTPMOD64 */
        case 17: *P = S + (uintptr_t)A; break;                                     /* DTPOFF64 */
#else
        case 1:  *P = S + (uintptr_t)A; break;                                     /* 32 */
        case 2:  *P = S + (uintptr_t)A - (uintptr_t)P; break;                      /* PC32 */
        case 6: case 7: *P = S; break;                                             /* GLOB_DAT, JMP_SLOT */
#endif
        default: {
            char t[16];
            snprintf(t, sizeof(t), "%u", type);
            set_err("%s: relocation type %s is not supported (thread-local variables?)", l->path, t);
            return -1;
        }
        }
    }
    return 0;
}

static void unload(lib_t* l);

static lib_t* load_file(const char* path, int flags) {
    unsigned char* f;
    size_t size;
    if (read_file(path, &f, &size) != 0) { set_err("%s: cannot read it%s", path, ""); return NULL; }
    const Ehdr* eh = (const Ehdr*)f;
    if (size < sizeof(Ehdr) || memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != ELF_CLASS || eh->machine != ELF_MACHINE ||
        eh->type != 3 || eh->phentsize != sizeof(Phdr) || eh->phoff + (size_t)eh->phnum * sizeof(Phdr) > size) {
        free(f);
        set_err("%s: not a shared library for this CPU%s", path, "");
        return NULL;
    }
    const Phdr* ph = (const Phdr*)(f + eh->phoff);
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size || ph[i].p_filesz > ph[i].p_memsz) { free(f); set_err("%s: damaged%s", path, ""); return NULL; }
        if (ph[i].p_vaddr < lo) lo = (uintptr_t)ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = (uintptr_t)(ph[i].p_vaddr + ph[i].p_memsz);
    }
    lo &= ~(uintptr_t)4095;
    hi = (hi + 4095) & ~(uintptr_t)4095;
    lib_t* l = calloc(1, sizeof(lib_t));
    void* m = l && hi > lo ? mmap(NULL, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (m == MAP_FAILED) { free(l); free(f); set_err("%s: out of memory%s", path, ""); return NULL; }
    snprintf(l->path, sizeof(l->path), "%s", path);
    const char* slash = strrchr(l->path, '/');
    l->name = slash ? slash + 1 : l->path;
    l->map = m;
    l->map_len = hi - lo;
    l->base = (uintptr_t)m - lo;
    l->phdr = malloc(eh->phnum * sizeof(Phdr));
    if (l->phdr) { memcpy(l->phdr, ph, eh->phnum * sizeof(Phdr)); l->phnum = eh->phnum; }
    const Dyn* dyn = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].p_type == PT_LOAD) memcpy((void*)(l->base + ph[i].p_vaddr), f + ph[i].p_offset, (size_t)ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const Dyn*)(l->base + ph[i].p_vaddr);
        if (ph[i].p_type == PT_TLS && ph[i].p_memsz)
            l->tls_mod = tls_add((const void*)(l->base + ph[i].p_vaddr), (size_t)ph[i].p_filesz, (size_t)ph[i].p_memsz, (size_t)ph[i].p_align);
    }
    free(f);
    l->refs = 1;
    l->global = (flags & RTLD_GLOBAL) != 0;
    l->nodelete = (flags & RTLD_NODELETE) != 0;
    if (!dyn) { set_err("%s: no dynamic section%s", path, ""); munmap(m, l->map_len); free(l->phdr); free(l); return NULL; }
    uintptr_t rel = 0, relsz = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, pltrel = 0;
    for (const Dyn* d = dyn; d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_STRTAB:   l->strtab = (const char*)(l->base + d->val); break;
        case DT_SYMTAB:   l->symtab = (const Sym*)(l->base + d->val); break;
        case DT_HASH:     l->hash = (const uint32_t*)(l->base + d->val); break;
        case DT_GNU_HASH: l->gnu = (const uint32_t*)(l->base + d->val); break;
        case DT_REL:      rel = (uintptr_t)d->val; break;
        case DT_RELSZ:    relsz = (uintptr_t)d->val; break;
        case DT_RELA:     rela = (uintptr_t)d->val; break;
        case DT_RELASZ:   relasz = (uintptr_t)d->val; break;
        case DT_JMPREL:   jmprel = (uintptr_t)d->val; break;
        case DT_PLTRELSZ: pltrelsz = (uintptr_t)d->val; break;
        case DT_PLTREL:   pltrel = (uintptr_t)d->val; break;
        }
    }
    if (l->hash) l->nsyms = l->hash[1];
    /* in the list before its libraries load: one needing it back finds it */
    l->next = g_libs;
    g_libs = l;
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", l->path);
    char* ds = strrchr(dir, '/');
    if (ds) *ds = 0; else strcpy(dir, ".");
    for (const Dyn* d = dyn; d->tag != DT_NULL; d++) {
        if (d->tag != DT_NEEDED) continue;
        const char* need = l->strtab + d->val;
        if (is_system_libc(need)) continue;              /* (the app's: found in the global scope) */
        lib_t* dep = load(need, dir, RTLD_LOCAL);
        if (!dep) { unload(l); return NULL; }
        if (l->ndeps < 16) l->deps[l->ndeps++] = dep;
    }
    if ((rela && relocate(l, rela, relasz, 1)) || (rel && relocate(l, rel, relsz, 0)) ||
        (jmprel && relocate(l, jmprel, pltrelsz, pltrel == DT_RELA))) {
        unload(l);
        return NULL;
    }
    run_init(l, dyn);
    return l;
}

/* already loaded: the same library again */
static lib_t* loaded(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (lib_t* l = g_libs; l; l = l->next)
        if (!strcmp(l->path, path) || (!strchr(path, '/') && !strcmp(l->name, base))) return l;
    return NULL;
}

/* the system's C library under Linux's names (libc.so.6, libdl, libm,
 * libpthread, librt...): the app's own - every SDK app carries it. A
 * library built on Linux that needs only those loads (JNA's
 * libjnidispatch.so, from its jar); dlopen("libc.so.6") is the app. */
static int is_system_libc(const char* name) {
    const char* b = strrchr(name, '/');
    b = b ? b + 1 : name;
    static const char* const libs[] = { "libc.so", "libdl.so", "libm.so", "libpthread.so", "librt.so", "libutil.so", "ld-linux-x86-64.so", NULL };
    for (int i = 0; libs[i]; i++) {
        size_t n = strlen(libs[i]);
        if (!strncmp(b, libs[i], n) && (b[n] == 0 || b[n] == '.')) return 1;
    }
    return 0;
}

static lib_t* load(const char* name, const char* from_dir, int flags) {
    lib_t* l = loaded(name);
    char path[256];
    if (!l && find(name, from_dir, path, sizeof(path))) l = loaded(path);
    if (l) {
        l->refs++;
        if (flags & RTLD_GLOBAL) l->global = 1;
        if (flags & RTLD_NODELETE) l->nodelete = 1;
        return l;
    }
    if (flags & RTLD_NOLOAD) return NULL;
    if (!find(name, from_dir, path, sizeof(path))) { set_err("%s: cannot open shared object file: No such file%s", name, ""); return NULL; }
    return load_file(path, flags);
}

static void unload(lib_t* l) {
    if (--l->refs > 0 || l->nodelete) return;
    for (size_t i = l->nfini; i > 0; i--) if (l->fini_array[i - 1]) l->fini_array[i - 1]();
    if (l->fini) l->fini();
    for (lib_t** pp = &g_libs; *pp; pp = &(*pp)->next) if (*pp == l) { *pp = l->next; break; }
    for (int i = 0; i < l->ndeps; i++) unload(l->deps[i]);
    munmap(l->map, l->map_len);
    free(l->phdr);
    free(l);
}

/* ── the calls ────────────────────────────────────────────────────── */

static int file_exists(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void* dlopen(const char* path, int flags) {
    if (!path || is_system_libc(path)) return &g_self;
    pthread_mutex_lock(&g_lock);
    lib_t* l = load(path, NULL, flags);
    pthread_mutex_unlock(&g_lock);
    return l;
}

void* dlsym(void* handle, const char* name) {
    pthread_mutex_lock(&g_lock);
    void* a = handle == RTLD_DEFAULT || handle == RTLD_NEXT || handle == &g_self ? global_sym(name) : tree_sym((lib_t*)handle, name);
    pthread_mutex_unlock(&g_lock);
    if (!a) set_err("undefined symbol: %s%s", name, "");
    return a;
}

int dlclose(void* handle) {
    if (!handle || handle == &g_self) return 0;
    pthread_mutex_lock(&g_lock);
    unload((lib_t*)handle);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int dladdr(const void* addr, Dl_info* info) {
    uintptr_t a = (uintptr_t)addr;
    memset(info, 0, sizeof(*info));
    pthread_mutex_lock(&g_lock);
    for (lib_t* l = g_libs; l; l = l->next) {
        if (a < (uintptr_t)l->map || a >= (uintptr_t)l->map + l->map_len) continue;
        info->dli_fname = l->path;
        info->dli_fbase = (void*)l->base;
        uintptr_t best = 0;
        uint32_t n = l->nsyms;
        if (!n && l->symtab && l->strtab && (const char*)l->strtab > (const char*)l->symtab)
            n = (uint32_t)(((const char*)l->strtab - (const char*)l->symtab) / sizeof(Sym));   /* (symtab is just before strtab) */
        for (uint32_t i = 1; i < n; i++) {
            const Sym* s = &l->symtab[i];
            uintptr_t v = l->base + s->value;
            if (s->shndx && v <= a && v >= best) { best = v; info->dli_sname = l->strtab + s->name; info->dli_saddr = (void*)v; }
        }
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_lock);
    /* the app itself: the nearest export below */
    info->dli_fname = "app";
    info->dli_fbase = (void*)__ehdr_start;
    uintptr_t best = 0;
    for (int i = 0; __dl_exports && __dl_exports[i].name; i++) {
        uintptr_t v = (uintptr_t)__dl_exports[i].addr;
        if (v <= a && v >= best) { best = v; info->dli_sname = __dl_exports[i].name; info->dli_saddr = (void*)v; }
    }
    return best != 0;
}

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info*, size_t, void*), void* data) {
    struct dl_phdr_info i;
    if (__ehdr_start) {
        const Ehdr* eh = (const Ehdr*)__ehdr_start;
        i.dlpi_addr = (ElfW_Addr)(uintptr_t)__ehdr_start;
        i.dlpi_name = "";
        i.dlpi_phdr = (const Phdr*)(__ehdr_start + eh->phoff);
        i.dlpi_phnum = eh->phnum;
        int r = cb(&i, sizeof(i), data);
        if (r) return r;
    }
    pthread_mutex_lock(&g_lock);
    for (lib_t* l = g_libs; l; l = l->next) {
        i.dlpi_addr = (ElfW_Addr)l->base;
        i.dlpi_name = l->path;
        i.dlpi_phdr = l->phdr;
        i.dlpi_phnum = (uint16_t)l->phnum;
        int r = cb(&i, sizeof(i), data);
        if (r) { pthread_mutex_unlock(&g_lock); return r; }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* dlinfo(RTLD_DI_LINKMAP): where the library is, its file */
int dlinfo(void* handle, int request, void* out) {
    static struct link_map maps[64];
    static int next;
    if (request != RTLD_DI_LINKMAP || !out) { set_err("dlinfo: request not supported%s%s", "", ""); return -1; }
    pthread_mutex_lock(&g_lock);
    struct link_map* m = &maps[next++ % 64];
    memset(m, 0, sizeof(*m));
    if (!handle || handle == &g_self) { m->l_addr = (ElfW_Addr)(uintptr_t)__ehdr_start; m->l_name = (char*)""; }
    else { lib_t* l = handle; m->l_addr = (ElfW_Addr)l->base; m->l_name = l->path; }
    pthread_mutex_unlock(&g_lock);
    *(struct link_map**)out = m;
    return 0;
}

void* dlvsym(void* handle, const char* name, const char* version) { (void)version; return dlsym(handle, name); }
