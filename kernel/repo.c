#include "repo.h"
#include "pkg.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "../net/http.h"
#include "../crypto/sha256.h"
#include "../crypto/ed25519.h"

#define MAX_SOURCES   16
#define MAX_PKGS      2048
#define MAX_FETCH     (96u << 20)          /* the biggest package downloaded */
#define MAX_PLAN      48                   /* packages one install may bring */

static const char* DEFAULT_SOURCES =
    "# Banana OS package sources - `apt update` after changing this file.\n"
    "#\n"
    "#   repo <url> key=<public key, 64 hex digits>   a signed repository\n"
    "#   repo <url> trusted                           no signature (local folders)\n"
    "#\n"
    "# Make your own repository on Linux with tools/repo/banana-repo (see the\n"
    "# README): `banana-repo key` prints the line to put here.\n"
    "\n"
    "# the examples that come with Banana OS\n"
    "repo file:///home/banana/Examples trusted\n";

/* ── small helpers ───────────────────────────────────────────────── */

static volatile int g_busy;               /* one update / install at a time */
static char         g_stat[160];
static volatile int g_pct = -1;

static int lock(char* msg, int mcap) {
    if (__sync_lock_test_and_set(&g_busy, 1)) {
        kstrlcpy(msg, "another install or update is running - try again when it is done", (size_t)mcap);
        return -1;
    }
    return 0;
}
static void unlock(void) { g_pct = -1; g_stat[0] = 0; __sync_lock_release(&g_busy); }

static void status(int pct, const char* s) { if (s) kstrlcpy(g_stat, s, sizeof(g_stat)); g_pct = pct; }

int repo_status(char* msg, int mcap) {
    if (msg && mcap > 0) kstrlcpy(msg, g_stat, (size_t)mcap);
    return g_busy ? (g_pct < 0 ? 0 : g_pct) : -1;
}

static void say(repo_log_t log, void* ctx, const char* fmt, ...) {
    char line[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    kstrlcpy(g_stat, line, sizeof(g_stat));
    if (log) log(ctx, line);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(const char* s, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}

static void tohex(const uint8_t* d, int n, char* out) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = H[d[i] >> 4]; out[2 * i + 1] = H[d[i] & 15]; }
    out[2 * n] = 0;
}

/* a file's bytes, kmalloc'd and NUL-terminated */
static int read_file(const char* path, char** data, uint32_t* len) {
    int fi = fs_find_file(path);
    if (fi < 0) return -1;
    fs_pin(fi);
    fs_file_t* f = fs_get_file(fi);
    if (!f || (!f->content && f->size)) { fs_unpin(fi); return -1; }
    char* d = (char*)kmalloc(f->size + 1);
    if (!d) { fs_unpin(fi); return -1; }
    if (f->size) memcpy(d, f->content, f->size);
    d[f->size] = 0;
    *data = d;
    *len = f->size;
    fs_unpin(fi);
    return 0;
}

/* ── downloads ───────────────────────────────────────────────────── */

typedef struct { char* buf; uint32_t n, cap; int oom; const char* what; } dl_t;

static int dl_body(void* ctx, const uint8_t* d, uint32_t n) {
    dl_t* b = (dl_t*)ctx;
    if (b->n + n + 1 > MAX_FETCH) { b->oom = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        if (cap > MAX_FETCH) cap = MAX_FETCH;
        char* nb = (char*)krealloc(b->buf, cap);
        if (!nb) { b->oom = 1; return -1; }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

static void dl_progress(void* ctx, uint32_t got, int32_t total) {
    dl_t* b = (dl_t*)ctx;
    char s[160];
    if (total > 0) {
        ksnprintf(s, sizeof(s), "Downloading %s - %u of %u KB", b->what, got / 1024, (uint32_t)total / 1024);
        status((int)((uint64_t)got * 100 / (uint32_t)total), s);
    } else {
        ksnprintf(s, sizeof(s), "Downloading %s - %u KB", b->what, got / 1024);
        status(g_pct < 0 ? 0 : g_pct, s);
    }
}

/* url: http://, https:// or file:///path; 0 with *data (kmalloc'd), or -1 */
static int fetch(const char* url, const char* what, char** data, uint32_t* len, char* err, int ecap) {
    if (!strncmp(url, "file://", 7)) {
        if (read_file(url + 7, data, len) != 0) { ksnprintf(err, (size_t)ecap, "%s: no such file", url + 7); return -1; }
        return 0;
    }
    dl_t b = { 0, 0, 0, 0, what };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 20000;
    req.user_agent = "BananaOS-apt/1.0";
    req.ctx = &b;
    req.on_body = dl_body;
    req.on_progress = dl_progress;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
    char e[128];
    int rc = http_fetch(url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom || resp->status >= 300) {
        if (b.oom) kstrlcpy(err, "too big, or out of memory", (size_t)ecap);
        else if (rc != NET_OK) kstrlcpy(err, e, (size_t)ecap);
        else ksnprintf(err, (size_t)ecap, "HTTP %d %s", resp->status, resp->reason);
        if (b.buf) kfree(b.buf);
        kfree(resp);
        return -1;
    }
    kfree(resp);
    if (!b.buf) { b.buf = (char*)kmalloc(1); if (!b.buf) return -1; b.buf[0] = 0; }
    *data = b.buf;
    *len = b.n;
    return 0;
}

/* base + "/" + rel (rel may be a full URL already) */
static void join_url(const char* base, const char* rel, char* out, int cap) {
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8) || !strncmp(rel, "file://", 7)) {
        kstrlcpy(out, rel, (size_t)cap);
        return;
    }
    kstrlcpy(out, base, (size_t)cap);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = 0;
    while (*rel == '/') rel++;
    kstrlcat(out, "/", (size_t)cap);
    kstrlcat(out, rel, (size_t)cap);
}

/* ── sources.list ────────────────────────────────────────────────── */

static void ensure_sources(void) {
    if (fs_find_file(REPO_SOURCES) >= 0) return;
    fs_mkdir_p("/etc/pkg");
    fs_write_path(REPO_SOURCES, DEFAULT_SOURCES, (uint32_t)strlen(DEFAULT_SOURCES));
}

static int next_word(const char** ps, char* out, int cap) {
    const char* s = *ps;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#' || *s == '\n' || *s == '\r') { *ps = s; return 0; }
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') { if (n < cap - 1) out[n++] = *s; s++; }
    out[n] = 0;
    *ps = s;
    return 1;
}

int repo_sources(repo_source_t* out, int max) {
    ensure_sources();
    char* d;
    uint32_t len;
    if (read_file(REPO_SOURCES, &d, &len) != 0) return 0;
    int n = 0;
    for (char* line = d; line && *line && n < max;) {
        char* e = strchr(line, '\n');
        if (e) *e = 0;
        const char* s = line;
        char w[200];
        if (next_word(&s, w, sizeof(w)) && (!strcmp(w, "repo") || !strcmp(w, "deb"))) {
            repo_source_t* r = &out[n];
            memset(r, 0, sizeof(*r));
            if (next_word(&s, r->url, sizeof(r->url))) {
                while (next_word(&s, w, sizeof(w))) {
                    if (!strncmp(w, "key=", 4) && strlen(w + 4) == 64 && unhex(w + 4, r->key, 32) == 0) r->has_key = 1;
                    else if (!strcmp(w, "trusted") || !strcmp(w, "[trusted=yes]")) r->trusted = 1;
                }
                n++;
            }
        }
        line = e ? e + 1 : NULL;
    }
    kfree(d);
    return n;
}

int repo_add_source(const char* url, const char* key_hex, int trusted, char* msg, int mcap) {
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8) && strncmp(url, "file://", 7)) {
        kstrlcpy(msg, "the address must start with https://, http:// or file://", (size_t)mcap);
        return -1;
    }
    uint8_t k[32];
    if (key_hex && (strlen(key_hex) != 64 || unhex(key_hex, k, 32) != 0)) {
        kstrlcpy(msg, "a key is 64 hex digits (banana-repo key prints it)", (size_t)mcap);
        return -1;
    }
    static repo_source_t src[MAX_SOURCES];
    int n = repo_sources(src, MAX_SOURCES);
    for (int i = 0; i < n; i++)
        if (!strcmp(src[i].url, url)) { ksnprintf(msg, (size_t)mcap, "%s is already a source", url); return -1; }
    if (n >= MAX_SOURCES) { kstrlcpy(msg, "too many sources", (size_t)mcap); return -1; }
    char line[320];
    ksnprintf(line, sizeof(line), "repo %s%s%s%s\n", url, key_hex ? " key=" : "", key_hex ? key_hex : "",
              trusted ? " trusted" : "");
    int fi = fs_find_file(REPO_SOURCES);
    if (fi < 0 || fs_append(fi, line, (uint32_t)strlen(line)) < 0) { kstrlcpy(msg, "cannot write " REPO_SOURCES, (size_t)mcap); return -1; }
    ksnprintf(msg, (size_t)mcap, "added %s - now: apt update", url);
    return 0;
}

int repo_remove_source(const char* which, char* msg, int mcap) {
    ensure_sources();
    char* d;
    uint32_t len;
    if (read_file(REPO_SOURCES, &d, &len) != 0) { kstrlcpy(msg, "cannot read " REPO_SOURCES, (size_t)mcap); return -1; }
    uint32_t num = 0;
    int by_num = k_parse_u32(which, &num);
    char* out = (char*)kmalloc(len + 1);
    if (!out) { kfree(d); return -1; }
    uint32_t o = 0;
    int idx = 0, removed = 0;
    for (char* line = d; line && *line;) {
        char* e = strchr(line, '\n');
        uint32_t ll = e ? (uint32_t)(e - line) + 1 : (uint32_t)strlen(line);
        char copy[320];
        uint32_t cl = ll < sizeof(copy) - 1 ? ll : sizeof(copy) - 1;
        memcpy(copy, line, cl);
        copy[cl] = 0;
        const char* s = copy;
        char w[200], url[200];
        int drop = 0;
        if (next_word(&s, w, sizeof(w)) && (!strcmp(w, "repo") || !strcmp(w, "deb")) && next_word(&s, url, sizeof(url))) {
            idx++;
            if ((by_num && (uint32_t)idx == num) || (!by_num && !strcmp(url, which))) drop = 1;
        }
        if (drop) removed++;
        else { memcpy(out + o, line, ll); o += ll; }
        line = e ? e + 1 : NULL;
    }
    if (removed) fs_write_path(REPO_SOURCES, out, o);
    kfree(out);
    kfree(d);
    if (!removed) { ksnprintf(msg, (size_t)mcap, "no source %s (apt sources lists them)", which); return -1; }
    kstrlcpy(msg, "source removed", (size_t)mcap);
    return 0;
}

/* ── the index ───────────────────────────────────────────────────── */

static repo_pkg_t* g_pkgs;
static int         g_npkgs;
static int         g_loaded;
static uint32_t    g_lists_gen;            /* fs generation when the lists were read */

static void list_path(int i, char* out, int cap) { ksnprintf(out, (size_t)cap, "%s/source%d", REPO_LISTS, i); }

static void field(repo_pkg_t* p, const char* k, const char* v) {
#define F(key, dst) if (!strcasecmp(k, key)) { kstrlcpy(p->dst, v, sizeof(p->dst)); return; }
    F("Package", name) F("Version", version) F("Title", title) F("Type", type) F("Category", category)
    F("Description", description) F("Author", author) F("Depends", depends) F("Arch", arch)
    F("Architecture", arch) F("Filename", filename) F("Icon", icon) F("SHA256", sha256)
#undef F
    if (!strcasecmp(k, "Size")) { uint32_t v32 = 0; k_parse_u32(v, &v32); p->size = v32; }
}

/* one index's stanzas into g_pkgs */
static void parse_index(char* text, int source) {
    repo_pkg_t cur;
    memset(&cur, 0, sizeof(cur));
    char* line = text;
    for (;;) {
        char* e = line ? strchr(line, '\n') : NULL;
        if (e) *e = 0;
        int blank = !line || !*line || !strcmp(line, "\r");
        if (blank) {
            if (cur.name[0] && cur.filename[0] && g_npkgs < MAX_PKGS) {
                if (!cur.title[0]) kstrlcpy(cur.title, cur.name, sizeof(cur.title));
                if (!cur.type[0]) kstrlcpy(cur.type, "console", sizeof(cur.type));
                cur.source = source;
                g_pkgs[g_npkgs++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            if (!line) break;
        } else if (line[0] != '#') {
            int l = (int)strlen(line);
            if (l && line[l - 1] == '\r') line[--l] = 0;
            if ((line[0] == ' ' || line[0] == '\t') && cur.name[0]) {     /* a continued description */
                kstrlcat(cur.description, " ", sizeof(cur.description));
                const char* s = line;
                while (*s == ' ' || *s == '\t') s++;
                kstrlcat(cur.description, strcmp(s, ".") ? s : "", sizeof(cur.description));
            } else {
                char* c = strchr(line, ':');
                if (c) {
                    *c = 0;
                    const char* v = c + 1;
                    while (*v == ' ') v++;
                    field(&cur, line, v);
                }
            }
        }
        line = e ? e + 1 : NULL;
    }
}

static void load_index(void) {
    if (g_loaded && g_lists_gen == fs_generation()) return;
    if (!g_pkgs) g_pkgs = (repo_pkg_t*)kmalloc(sizeof(repo_pkg_t) * MAX_PKGS);
    g_npkgs = 0;
    g_loaded = 1;
    if (!g_pkgs) return;
    static repo_source_t src[MAX_SOURCES];
    int n = repo_sources(src, MAX_SOURCES);
    for (int i = 0; i < n; i++) {
        char path[FS_PATH_LEN];
        list_path(i, path, sizeof(path));
        char* d;
        uint32_t len;
        if (read_file(path, &d, &len) != 0) continue;
        /* "#source <url>": the list belongs to the source it came from */
        char want[220];
        ksnprintf(want, sizeof(want), "#source %s\n", src[i].url);
        if (!strncmp(d, want, strlen(want))) parse_index(d, i);
        kfree(d);
    }
    g_lists_gen = fs_generation();
}

int repo_count(void) { load_index(); return g_npkgs; }
const repo_pkg_t* repo_at(int i) { load_index(); return i >= 0 && i < g_npkgs ? &g_pkgs[i] : NULL; }

int repo_never_updated(void) { return fs_find_dir(REPO_LISTS) < 0; }

static int arch_ok(const repo_pkg_t* p) {
    if (!p->arch[0] || strstr(p->arch, "all")) return 1;
    return strstr(p->arch, BANANA_ARCH) != NULL;
}

const repo_pkg_t* repo_candidate(const char* name) {
    load_index();
    const repo_pkg_t* best = NULL;
    for (int i = 0; i < g_npkgs; i++) {
        const repo_pkg_t* p = &g_pkgs[i];
        if (strcmp(p->name, name) || !arch_ok(p)) continue;
        if (!best || repo_vercmp(p->version, best->version) > 0) best = p;
    }
    return best;
}

/* ── versions ────────────────────────────────────────────────────── */

static int order(int c) {
    if (k_isdigit(c)) return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

/* dpkg's algorithm: non-digit runs compare character by character (letters
 * before other signs, ~ before everything, even the end), digit runs as numbers */
int repo_vercmp(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a || *b) {
        while ((*a && !k_isdigit(*a)) || (*b && !k_isdigit(*b))) {
            int ac = order(*a), bc = order(*b);
            if (ac != bc) return ac - bc;
            a++;
            b++;
        }
        while (*a == '0') a++;
        while (*b == '0') b++;
        int first = 0;
        while (k_isdigit(*a) && k_isdigit(*b)) {
            if (!first) first = *a - *b;
            a++;
            b++;
        }
        if (k_isdigit(*a)) return 1;
        if (k_isdigit(*b)) return -1;
        if (first) return first;
    }
    return 0;
}

/* "op version" satisfied by have? */
static int constraint_ok(const char* have, const char* op, const char* ver) {
    if (!op[0]) return 1;
    int c = repo_vercmp(have, ver);
    if (!strcmp(op, ">=")) return c >= 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">>") || !strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<<") || !strcmp(op, "<")) return c < 0;
    if (!strcmp(op, "=")) return c == 0;
    return 1;
}

/* ── installed packages' extras ──────────────────────────────────── */

static void app_file(const char* name, const char* file, char* out, int cap) {
    ksnprintf(out, (size_t)cap, "%s/%s/%s", PKG_DIR, name, file);
}

int repo_is_auto(const char* name) {
    char p[FS_PATH_LEN];
    app_file(name, ".auto", p, sizeof(p));
    return fs_find_file(p) >= 0;
}

/* an installed package's depends= (from its manifest) */
static void installed_depends(const char* name, char* out, int cap) {
    out[0] = 0;
    char p[FS_PATH_LEN];
    app_file(name, "manifest", p, sizeof(p));
    char* d;
    uint32_t len;
    if (read_file(p, &d, &len) != 0) return;
    for (char* line = d; line && *line;) {
        char* e = strchr(line, '\n');
        if (e) *e = 0;
        if (!strncmp(line, "depends=", 8)) {
            kstrlcpy(out, line + 8, (size_t)cap);
            int l = (int)strlen(out);
            while (l && (out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0;
        }
        line = e ? e + 1 : NULL;
    }
    kfree(d);
}

/* each "name (op version)" of a Depends line, alternatives split by | */
typedef struct { char name[PKG_NAME_MAX]; char op[3]; char ver[24]; } dep_t;

static int parse_alts(const char* s, dep_t* alts, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '|') s++;
        if (!*s) break;
        dep_t* d = &alts[n];
        memset(d, 0, sizeof(*d));
        int k = 0;
        while (*s && *s != ' ' && *s != '(' && *s != '|') { if (k < PKG_NAME_MAX - 1) d->name[k++] = *s; s++; }
        while (*s == ' ') s++;
        if (*s == '(') {
            s++;
            while (*s == ' ') s++;
            k = 0;
            while (*s == '<' || *s == '>' || *s == '=') { if (k < 2) d->op[k++] = *s; s++; }
            while (*s == ' ') s++;
            k = 0;
            while (*s && *s != ')' && *s != ' ') { if (k < 23) d->ver[k++] = *s; s++; }
            while (*s && *s != ')') s++;
            if (*s == ')') s++;
        }
        while (*s == ' ') s++;
        if (d->name[0]) n++;
        if (*s == '|') s++;
        else if (*s && *s != '|') break;
    }
    return n;
}

/* ── installing ──────────────────────────────────────────────────── */

typedef struct { repo_pkg_t p; int as_dep; } plan_t;   /* copies: the index is re-read as files change */

static int in_plan(plan_t* plan, int n, const char* name) {
    for (int i = 0; i < n; i++) if (!strcmp(plan[i].p.name, name)) return 1;
    return 0;
}

/* adds what p needs (first) and p to the plan; -1 with msg if it cannot */
static int plan_add(const repo_pkg_t* p, int as_dep, plan_t* plan, int* n, int depth, char* msg, int mcap) {
    if (depth > 16) { ksnprintf(msg, (size_t)mcap, "%s: dependencies go round in circles", p->name); return -1; }
    if (in_plan(plan, *n, p->name)) return 0;
    const char* s = p->depends;
    while (*s) {
        char part[128];
        int k = 0;
        while (*s && *s != ',') { if (k < (int)sizeof(part) - 1) part[k++] = *s; s++; }
        part[k] = 0;
        if (*s == ',') s++;
        dep_t alts[4];
        int na = parse_alts(part, alts, 4);
        if (!na) continue;
        int ok = 0;
        /* already installed in a good version, or on its way */
        for (int a = 0; a < na && !ok; a++) {
            pkg_info_t inst;
            if (pkg_get(alts[a].name, &inst) == 0 && constraint_ok(inst.version, alts[a].op, alts[a].ver)) ok = 1;
            else if (in_plan(plan, *n, alts[a].name)) ok = 1;
        }
        for (int a = 0; a < na && !ok; a++) {
            const repo_pkg_t* c = repo_candidate(alts[a].name);
            if (c && constraint_ok(c->version, alts[a].op, alts[a].ver)) {
                if (plan_add(c, 1, plan, n, depth + 1, msg, mcap) != 0) return -1;
                ok = 1;
            }
        }
        if (!ok) {
            ksnprintf(msg, (size_t)mcap, "%s needs %s%s%s%s%s, which no source has (apt update?)", p->name, alts[0].name,
                      alts[0].op[0] ? " (" : "", alts[0].op, alts[0].op[0] ? alts[0].ver : "", alts[0].op[0] ? ")" : "");
            return -1;
        }
    }
    if (*n >= MAX_PLAN) { kstrlcpy(msg, "too many packages at once", (size_t)mcap); return -1; }
    plan[*n].p = *p;
    plan[*n].as_dep = as_dep;
    (*n)++;
    return 0;
}

/* downloads, checks and installs one package */
static int install_one(const repo_pkg_t* p, int as_dep, repo_log_t log, void* ctx, char* msg, int mcap) {
    static repo_source_t src[MAX_SOURCES];
    int ns = repo_sources(src, MAX_SOURCES);
    if (p->source >= ns) { kstrlcpy(msg, "the sources changed - run apt update", (size_t)mcap); return -1; }
    const repo_source_t* s = &src[p->source];
    char url[512];
    join_url(s->url, p->filename, url, sizeof(url));
    say(log, ctx, "Get: %s %s (%u KB)", p->name, p->version, (p->size + 1023) / 1024);
    char* d;
    uint32_t len;
    char err[160];
    if (fetch(url, p->name, &d, &len, err, sizeof(err)) != 0) {
        ksnprintf(msg, (size_t)mcap, "%s: %s", url, err);
        return -1;
    }
    /* the signed index says what the file must be */
    if (p->size && len != p->size) {
        ksnprintf(msg, (size_t)mcap, "%s: %u bytes, the index says %u - not installed", p->name, len, p->size);
        kfree(d);
        return -1;
    }
    if (p->sha256[0] || s->has_key) {
        uint8_t h[32];
        char hx[65];
        sha256(d, len, h);
        tohex(h, 32, hx);
        if (strcasecmp(hx, p->sha256) != 0) {
            ksnprintf(msg, (size_t)mcap, "%s: the download does not match the index (SHA256) - not installed", p->name);
            kfree(d);
            return -1;
        }
    }
    int was_auto = repo_is_auto(p->name);
    pkg_info_t before;
    int had = pkg_get(p->name, &before) == 0;
    say(log, ctx, "%s %s (%s)...", had ? "Upgrading" : "Installing", p->name, p->version);
    int rc = pkg_install_mem((const uint8_t*)d, len, msg, mcap);
    kfree(d);
    if (rc != 0) return -1;
    char path[FS_PATH_LEN];
    /* installed only because something needs it: autoremove may take it */
    if ((as_dep && !had) || (had && was_auto && as_dep)) {
        app_file(p->name, ".auto", path, sizeof(path));
        fs_write_path(path, "", 0);
    }
    app_file(p->name, ".origin", path, sizeof(path));
    fs_write_path(path, s->url, (uint32_t)strlen(s->url));
    if (as_dep) say(log, ctx, "%s", msg);       /* (the last one is the caller's message) */
    return 0;
}

static int install_locked(const char* name, int reinstall, repo_log_t log, void* ctx, char* msg, int mcap) {
    const repo_pkg_t* c = repo_candidate(name);
    if (!c) {
        load_index();
        int other = 0;
        for (int i = 0; i < g_npkgs; i++) if (!strcmp(g_pkgs[i].name, name)) other = 1;
        if (other) ksnprintf(msg, (size_t)mcap, "%s is not made for this computer (%s)", name, BANANA_ARCH);
        else if (repo_never_updated()) ksnprintf(msg, (size_t)mcap, "unknown package %s - run apt update first", name);
        else ksnprintf(msg, (size_t)mcap, "unknown package %s (apt search finds packages)", name);
        return -1;
    }
    pkg_info_t inst;
    if (!reinstall && pkg_get(name, &inst) == 0 && repo_vercmp(inst.version, c->version) >= 0) {
        if (repo_is_auto(name)) {                        /* asked for by name now: keep it */
            char path[FS_PATH_LEN];
            app_file(name, ".auto", path, sizeof(path));
            fs_delete(path, 0);
        }
        ksnprintf(msg, (size_t)mcap, "%s is already the newest version (%s)", name, inst.version);
        return 0;
    }
    static plan_t plan[MAX_PLAN];
    int n = 0;
    if (plan_add(c, 0, plan, &n, 0, msg, mcap) != 0) return -1;
    if (n > 1) {
        char line[200] = "";
        for (int i = 0; i < n; i++) {
            if (i) kstrlcat(line, " ", sizeof(line));
            kstrlcat(line, plan[i].p.name, sizeof(line));
        }
        say(log, ctx, "The following packages will be installed: %s", line);
    }
    for (int i = 0; i < n; i++) {
        if (install_one(&plan[i].p, plan[i].as_dep, log, ctx, msg, mcap) != 0) return -1;
        status((i + 1) * 100 / n, NULL);
    }
    if (repo_is_auto(name)) {
        char path[FS_PATH_LEN];
        app_file(name, ".auto", path, sizeof(path));
        fs_delete(path, 0);
    }
    if (n > 1) ksnprintf(msg, (size_t)mcap, "installed %s %s and %d package%s it needs", c->title, c->version, n - 1, n == 2 ? "" : "s");
    return 0;
}

int repo_install(const char* name, int reinstall, repo_log_t log, void* ctx, char* msg, int mcap) {
    if (lock(msg, mcap)) return -1;
    status(0, "Preparing...");
    int rc = install_locked(name, reinstall, log, ctx, msg, mcap);
    unlock();
    return rc;
}

/* ── upgrades, removal ───────────────────────────────────────────── */

static pkg_info_t g_inst[64];

int repo_upgradable(void) {
    int n = pkg_list(g_inst, 64), up = 0;
    for (int i = 0; i < n && i < 64; i++) {
        const repo_pkg_t* c = repo_candidate(g_inst[i].name);
        if (c && repo_vercmp(c->version, g_inst[i].version) > 0) up++;
    }
    return up;
}

int repo_upgrade(repo_log_t log, void* ctx, char* msg, int mcap) {
    if (lock(msg, mcap)) return -1;
    static pkg_info_t inst[64];
    int n = pkg_list(inst, 64), done = 0, failed = 0;
    for (int i = 0; i < n && i < 64; i++) {
        const repo_pkg_t* c = repo_candidate(inst[i].name);
        if (!c || repo_vercmp(c->version, inst[i].version) <= 0) continue;
        say(log, ctx, "%s: %s -> %s", inst[i].name, inst[i].version, c->version);
        char m[160];
        if (install_locked(inst[i].name, 0, log, ctx, m, sizeof(m)) == 0) done++;
        else { failed++; say(log, ctx, "%s: %s", inst[i].name, m); }
    }
    if (!done && !failed) kstrlcpy(msg, "everything is up to date", (size_t)mcap);
    else ksnprintf(msg, (size_t)mcap, "%d upgraded%s", done, failed ? ", some failed (see above)" : "");
    unlock();
    return failed ? -1 : done;
}

int repo_remove(const char* name, char* msg, int mcap) {
    if (lock(msg, mcap)) return -1;
    int rc = pkg_remove(name, msg, mcap);
    unlock();
    return rc;
}

/* marks everything name needs (installed) as needed */
static void mark_needed(const char* name, char* needed, int n, int depth) {
    if (depth > 16) return;
    char deps[128];
    installed_depends(name, deps, sizeof(deps));
    const char* s = deps;
    while (*s) {
        char part[128];
        int k = 0;
        while (*s && *s != ',') { if (k < (int)sizeof(part) - 1) part[k++] = *s; s++; }
        part[k] = 0;
        if (*s == ',') s++;
        dep_t alts[4];
        int na = parse_alts(part, alts, 4);
        for (int a = 0; a < na; a++)
            for (int i = 0; i < n; i++)
                if (!needed[i] && !strcmp(g_inst[i].name, alts[a].name)) { needed[i] = 1; mark_needed(g_inst[i].name, needed, n, depth + 1); }
    }
}

int repo_autoremove(repo_log_t log, void* ctx, char* msg, int mcap) {
    if (lock(msg, mcap)) return -1;
    int n = pkg_list(g_inst, 64);
    if (n > 64) n = 64;
    char needed[64];
    for (int i = 0; i < n; i++) needed[i] = !repo_is_auto(g_inst[i].name);
    for (int i = 0; i < n; i++) if (needed[i]) mark_needed(g_inst[i].name, needed, n, 0);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (needed[i]) continue;
        char m[160];
        if (pkg_remove(g_inst[i].name, m, sizeof(m)) == 0) { removed++; say(log, ctx, "Removing %s", g_inst[i].name); }
    }
    if (removed) ksnprintf(msg, (size_t)mcap, "%d package%s removed", removed, removed == 1 ? "" : "s");
    else kstrlcpy(msg, "nothing to remove", (size_t)mcap);
    unlock();
    return removed;
}

/* ── apt update ──────────────────────────────────────────────────── */

/* a local folder without an index: its .bpk files are the packages */
static char* flat_index(const char* dir, int* count) {
    int idx[128];
    int n = fs_list_files(dir, idx, 128);
    if (n > 128) n = 128;
    uint32_t cap = 4096, len = 0;
    char* out = (char*)kmalloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    *count = 0;
    for (int i = 0; i < n; i++) {
        fs_file_t* fi = fs_file_info(idx[i]);
        if (!fi) continue;
        int nl = (int)strlen(fi->name);
        if (nl < 5 || strcasecmp(fi->name + nl - 4, ".bpk")) continue;
        char path[FS_PATH_LEN];
        ksnprintf(path, sizeof(path), "%s/%s", dir, fi->name);
        char* d;
        uint32_t dl;
        if (read_file(path, &d, &dl) != 0) continue;
        pkg_info_t info;
        char err[96];
        if (pkg_inspect_mem((const uint8_t*)d, dl, &info, err, sizeof(err)) == 0) {
            uint8_t h[32];
            char hx[65], st[1024];
            sha256(d, dl, h);
            tohex(h, 32, hx);
            ksnprintf(st, sizeof(st),
                      "Package: %s\nVersion: %s\nTitle: %s\nType: %s\nCategory: %s\nDescription: %s\nAuthor: %s\n"
                      "Depends: %s\nArch: %s%s%s\nFilename: %s\nSize: %u\nSHA256: %s\n\n",
                      info.name, info.version[0] ? info.version : "0", info.title, info.type, info.category,
                      info.description, info.author, info.depends, info.has_i686 ? "i686" : "",
                      info.has_i686 && info.has_x86_64 ? " " : "", info.has_x86_64 ? "x86_64" : "", fi->name, dl, hx);
            uint32_t sl = (uint32_t)strlen(st);
            if (len + sl + 1 > cap) {
                cap = (len + sl + 1) * 2;
                char* nb = (char*)krealloc(out, cap);
                if (!nb) { kfree(d); break; }
                out = nb;
            }
            memcpy(out + len, st, sl + 1);
            len += sl;
            (*count)++;
        }
        kfree(d);
    }
    return out;
}

int repo_update(repo_log_t log, void* ctx, char* msg, int mcap) {
    if (lock(msg, mcap)) return -1;
    static repo_source_t src[MAX_SOURCES];
    int n = repo_sources(src, MAX_SOURCES);
    fs_mkdir_p(REPO_LISTS);
    int ok = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        repo_source_t* s = &src[i];
        status(i * 100 / (n ? n : 1), NULL);
        char url[512], err[160];
        char* d = NULL;
        uint32_t len = 0;
        int flat = 0;
        join_url(s->url, "Packages", url, sizeof(url));
        say(log, ctx, "Get:%d %s", i + 1, s->url);
        if (fetch(url, "the package list", &d, &len, err, sizeof(err)) != 0) {
            /* a plain local folder of .bpk files */
            if (!strncmp(s->url, "file://", 7) && fs_find_dir(s->url + 7) >= 0) {
                int count = 0;
                d = flat_index(s->url + 7, &count);
                if (d) { len = (uint32_t)strlen(d); flat = 1; }
            }
            if (!d) { say(log, ctx, "Err:%d %s - %s", i + 1, s->url, err); bad++; continue; }
        }
        if (s->has_key && !flat) {
            char* sig = NULL;
            uint32_t sl = 0;
            uint8_t sb[64];
            join_url(s->url, "Packages.sig", url, sizeof(url));
            int good = fetch(url, "the signature", &sig, &sl, err, sizeof(err)) == 0 && sl >= 128 &&
                       unhex(sig, sb, 64) == 0 && ed25519_verify(sb, (const uint8_t*)d, len, s->key) == 0;
            if (sig) kfree(sig);
            if (!good) {
                say(log, ctx, "Err:%d %s - the signature does not match its key: ignored", i + 1, s->url);
                kfree(d);
                bad++;
                continue;
            }
        } else if (!s->trusted && !(s->has_key && flat)) {
            say(log, ctx, "Err:%d %s - not signed: add key=<its key> (or trusted) in %s", i + 1, s->url, REPO_SOURCES);
            kfree(d);
            bad++;
            continue;
        }
        /* saved with the source it came from on top */
        char head[240];
        ksnprintf(head, sizeof(head), "#source %s\n", s->url);
        uint32_t hl = (uint32_t)strlen(head);
        char* out = (char*)kmalloc(hl + len + 1);
        if (out) {
            memcpy(out, head, hl);
            memcpy(out + hl, d, len);
            char path[FS_PATH_LEN];
            list_path(i, path, sizeof(path));
            fs_write_path(path, out, hl + len);
            kfree(out);
            ok++;
        }
        kfree(d);
    }
    /* lists of sources that are gone */
    for (int i = n; i < MAX_SOURCES; i++) {
        char path[FS_PATH_LEN];
        list_path(i, path, sizeof(path));
        if (fs_find_file(path) >= 0) fs_delete(path, 0);
    }
    g_loaded = 0;
    load_index();
    int avail = 0;
    for (int i = 0; i < g_npkgs; i++) if (arch_ok(&g_pkgs[i]) && repo_candidate(g_pkgs[i].name) == &g_pkgs[i]) avail++;
    unlock();
    int up = repo_upgradable();
    ksnprintf(msg, (size_t)mcap, "%d package%s from %d source%s%s", avail, avail == 1 ? "" : "s", ok, ok == 1 ? "" : "s",
              bad ? " (some failed, see above)" : "");
    if (up) {
        char more[64];
        ksnprintf(more, sizeof(more), " - %d can be upgraded (apt upgrade)", up);
        kstrlcat(msg, more, (size_t)mcap);
    }
    return bad && !ok ? -1 : avail;
}

/* ── icons ───────────────────────────────────────────────────────── */

int repo_icon(const char* name, char* path, int cap) {
    char p[FS_PATH_LEN];
    app_file(name, "icon.png", p, sizeof(p));
    if (fs_find_file(p) >= 0) { kstrlcpy(path, p, (size_t)cap); return 0; }
    const repo_pkg_t* c = repo_candidate(name);
    if (!c || !c->icon[0]) return -1;
    ksnprintf(p, sizeof(p), "%s/%s-%s.png", REPO_ICONS, name, c->version);
    if (fs_find_file(p) >= 0) { kstrlcpy(path, p, (size_t)cap); return 0; }
    static repo_source_t src[MAX_SOURCES];
    int ns = repo_sources(src, MAX_SOURCES);
    if (c->source >= ns) return -1;
    char url[512], err[128];
    join_url(src[c->source].url, c->icon, url, sizeof(url));
    char* d;
    uint32_t len;
    if (fetch(url, "an icon", &d, &len, err, sizeof(err)) != 0) return -1;
    fs_mkdir_p(REPO_ICONS);
    int rc = len > 0 && len < (1u << 20) ? fs_write_path(p, d, len) : -1;
    kfree(d);
    if (rc < 0) return -1;
    kstrlcpy(path, p, (size_t)cap);
    return 0;
}
