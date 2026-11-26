/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dlmgr.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "rtc.h"
#include "serial.h"
#include "../net/http.h"

#define DLM_MAX      40               /* entries kept in the list */
#define DLM_WORKERS  2                /* downloads at once (more wait their turn) */
#define LIST_PATH    fs_home_join("/.config/downloads")

typedef struct {
    int      used;
    dlm_info_t i;
    char*    full_url;                /* the whole address (kmalloc'd) */
    volatile int cancel;
    int      fidx, open, http_ok;
    uint32_t rate_t0, rate_b0;
} entry_t;

static entry_t g_e[DLM_MAX];
static int g_next_id = 1, g_loaded, g_workers, g_session_base = 1;
static volatile uint32_t g_gen;
static dlm_cookies_fn g_cookies;

void dlm_set_cookies(dlm_cookies_fn f) { g_cookies = f; }
uint32_t dlm_gen(void) { return g_gen; }

/* ── the list on the disk: finished downloads, one a line ──────────── */

static void save_list(void) {
    uint32_t cap = 512, n = 0;
    char* b = (char*)kmalloc(cap);
    if (!b) return;
    for (int k = 0; k < DLM_MAX; k++) {
        entry_t* e = &g_e[k];
        if (!e->used || e->i.state == DLM_QUEUED || e->i.state == DLM_RUNNING) continue;
        uint32_t need = n + 64 + (uint32_t)strlen(e->i.path) + (uint32_t)strlen(e->full_url ? e->full_url : "") + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char* nb = (char*)kmalloc(cap);
            if (!nb) { kfree(b); return; }
            memcpy(nb, b, n);
            kfree(b);
            b = nb;
        }
        n += (uint32_t)ksnprintf(b + n, cap - n, "%d\t%u\t%u\t%u\t%s\t%s\n", e->i.state, e->i.got, e->i.total,
                                 e->i.when, e->i.path, e->full_url ? e->full_url : "");
    }
    fs_mkdir_p(fs_home_join("/.config"));
    fs_write_path(LIST_PATH, b, n);
    kfree(b);
}

static entry_t* new_entry(void) {
    entry_t* free_e = NULL;
    for (int k = 0; k < DLM_MAX; k++) if (!g_e[k].used) { free_e = &g_e[k]; break; }
    if (!free_e) {                            /* full: the oldest finished one goes */
        uint32_t oldest = 0xFFFFFFFFu;
        for (int k = 0; k < DLM_MAX; k++) {
            entry_t* e = &g_e[k];
            if (e->i.state != DLM_QUEUED && e->i.state != DLM_RUNNING && (uint32_t)e->i.id < oldest) { oldest = (uint32_t)e->i.id; free_e = e; }
        }
        if (!free_e) return NULL;
        kfree(free_e->full_url);
    }
    memset(free_e, 0, sizeof(*free_e));
    free_e->used = 1;
    free_e->i.id = g_next_id++;
    free_e->fidx = -1;
    return free_e;
}

static void set_url(entry_t* e, const char* url) {
    uint32_t n = (uint32_t)strlen(url);
    e->full_url = (char*)kmalloc(n + 1);
    if (e->full_url) memcpy(e->full_url, url, n + 1);
    kstrlcpy(e->i.url, url, sizeof(e->i.url));
}

static void set_path(entry_t* e, const char* path) {
    kstrlcpy(e->i.path, path, sizeof(e->i.path));
    const char* s = strrchr(path, '/');
    kstrlcpy(e->i.name, s ? s + 1 : path, sizeof(e->i.name));
}

static void load_list(void) {
    if (g_loaded) return;
    g_loaded = 1;
    int idx = fs_find_file(LIST_PATH);
    fs_file_t* f = idx >= 0 ? fs_file_info(idx) : NULL;
    if (!f || !f->size || f->size > (1u << 20)) { g_session_base = g_next_id; return; }
    char* b = (char*)kmalloc(f->size + 1);
    if (!b) { g_session_base = g_next_id; return; }
    int n = fs_read(idx, 0, b, f->size);
    if (n < 0) { kfree(b); g_session_base = g_next_id; return; }
    b[n] = 0;
    /* (oldest first in the file: each new entry is newer) */
    for (char* l = b; *l; ) {
        char* e = strchr(l, '\n');
        if (e) *e = 0;
        char* fld[6];
        int k = 0;
        for (char* p = l; k < 6; ) {
            fld[k++] = p;
            char* t = k < 6 ? strchr(p, '\t') : NULL;
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (k == 6) {
            entry_t* en = new_entry();
            if (en) {
                uint32_t v;
                k_parse_u32(fld[0], &v); en->i.state = (int)v;
                if (en->i.state != DLM_DONE && en->i.state != DLM_FAILED) en->i.state = DLM_CANCELLED;
                k_parse_u32(fld[1], &en->i.got);
                k_parse_u32(fld[2], &en->i.total);
                k_parse_u32(fld[3], &en->i.when);
                set_path(en, fld[4]);
                set_url(en, fld[5]);
                if (en->i.state == DLM_FAILED) kstrlcpy(en->i.err, "Failed", sizeof(en->i.err));
            }
        }
        if (!e) break;
        l = e + 1;
    }
    kfree(b);
    g_session_base = g_next_id;               /* (what comes after: this start's) */
}

int dlm_session_base(void) {
    load_list();
    return g_session_base;
}

/* ── a download, in a worker task ──────────────────────────────────── */

static char g_hdr[DLM_WORKERS][8192];      /* each worker's cookie header */

typedef struct { entry_t* e; int w; } run_t;

static const char* headers_for(void* ctx, const char* url) {
    run_t* r = (run_t*)ctx;
    if (!g_cookies) return NULL;
    g_cookies(url, g_hdr[r->w], sizeof(g_hdr[r->w]));
    return g_hdr[r->w][0] ? g_hdr[r->w] : NULL;
}

static void on_headers(void* ctx, const http_response_t* resp, const char* raw) {
    (void)raw;
    entry_t* e = ((run_t*)ctx)->e;
    if (resp->status >= 300 && resp->status < 400) return;      /* (a redirect: the next answer counts) */
    e->http_ok = resp->status >= 200 && resp->status < 300;
    if (!e->http_ok) ksnprintf(e->i.err, sizeof(e->i.err), "HTTP %d %s", resp->status, resp->reason);
    e->i.total = resp->content_length > 0 ? (uint32_t)resp->content_length : 0;
    g_gen++;
}

static int on_body(void* ctx, const uint8_t* d, uint32_t n) {
    entry_t* e = ((run_t*)ctx)->e;
    if (e->cancel || !e->http_ok) return -1;
    if (!e->open) {
        e->fidx = fs_create(e->i.path);
        if (e->fidx < 0 || fs_write(e->fidx, "", 0) != 0) { kstrlcpy(e->i.err, "Cannot create the file", sizeof(e->i.err)); return -1; }
        e->open = 1;
    }
    if ((uint64_t)e->i.got + n > FS_MAX_FILE_SIZE) { kstrlcpy(e->i.err, "Too big (2 GiB at most)", sizeof(e->i.err)); return -1; }
    if (fs_append(e->fidx, d, n) != 0) { kstrlcpy(e->i.err, "Disk full", sizeof(e->i.err)); return -1; }
    e->i.got += n;
    uint32_t now = timer_ms();
    if (now - e->rate_t0 >= 1000) {           /* the speed: over the last second or so */
        uint32_t r = (uint32_t)((uint64_t)(e->i.got - e->rate_b0) * 1000u / (now - e->rate_t0));
        e->i.rate = e->i.rate ? (e->i.rate + r) / 2 : r;
        e->rate_t0 = now;
        e->rate_b0 = e->i.got;
        g_gen++;
    }
    return 0;
}

static void run(entry_t* e, int w) {
    run_t r = { e, w };
    e->i.got = 0;
    e->i.total = 0;
    e->i.rate = 0;
    e->i.err[0] = 0;
    e->open = 0;
    e->http_ok = 0;
    e->rate_t0 = timer_ms();
    e->rate_b0 = 0;
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 30000;
    req.user_agent = "Mozilla/5.0 (Banana OS) BananaBrowser/1.0";
    req.headers_for = headers_for;
    req.ctx = &r;
    req.on_headers = on_headers;
    req.on_body = on_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    char err[160] = "";
    int rc = resp ? http_fetch(e->full_url ? e->full_url : e->i.url, &req, resp, err, sizeof(err)) : NET_ERR_NOMEM;
    kfree(resp);
    if (e->cancel) e->i.state = DLM_CANCELLED;
    else if (rc != NET_OK || !e->http_ok || e->i.err[0]) {
        e->i.state = DLM_FAILED;
        if (!e->i.err[0]) kstrlcpy(e->i.err, err[0] ? err : "Network error", sizeof(e->i.err));
    } else {
        if (!e->open) {                          /* (an empty file) */
            int fi = fs_create(e->i.path);
            if (fi >= 0) fs_write(fi, "", 0);
        }
        e->i.state = DLM_DONE;
        e->i.total = e->i.got;
    }
    if (e->i.state != DLM_DONE && e->open) fs_delete(e->i.path, 0);   /* no half file left */
    e->open = 0;
    e->i.rate = 0;
    klog("download: %s %s (%u bytes)\n", e->i.name, e->i.state == DLM_DONE ? "done" : e->i.state == DLM_CANCELLED ? "cancelled" : e->i.err, e->i.got);
    save_list();
    g_gen++;
}

static int g_worker_no;

static void worker(void) {
    int w = g_worker_no++;
    task_set_background();
    for (;;) {
        entry_t* job = NULL;
        for (int k = 0; k < DLM_MAX; k++)               /* the oldest waiting one (cooperative: no race) */
            if (g_e[k].used && g_e[k].i.state == DLM_QUEUED && (!job || g_e[k].i.id < job->i.id)) job = &g_e[k];
        if (!job) { task_sleep_ms(200); continue; }
        job->i.state = DLM_RUNNING;
        g_gen++;
        run(job, w < DLM_WORKERS ? w : 0);
    }
}

static void ensure_workers(void) {
    while (g_workers < DLM_WORKERS) {
        if (task_create_stack("download", worker, 64u << 10) < 0) break;
        g_workers++;
    }
}

/* ── the API ────────────────────────────────────────────────────────── */

int dlm_start(const char* url, const char* path) {
    load_list();
    entry_t* e = new_entry();
    if (!e) return -1;
    set_url(e, url);
    set_path(e, path);
    e->i.when = rtc_now();
    e->i.state = DLM_QUEUED;
    ensure_workers();
    g_gen++;
    return e->i.id;
}

static entry_t* by_id(int id) {
    for (int k = 0; k < DLM_MAX; k++) if (g_e[k].used && g_e[k].i.id == id) return &g_e[k];
    return NULL;
}

int dlm_get(int i, dlm_info_t* out) {
    load_list();
    /* newest first: the i-th highest id */
    int last = 0x7FFFFFFF;
    entry_t* e = NULL;
    for (int n = 0; n <= i; n++) {
        e = NULL;
        for (int k = 0; k < DLM_MAX; k++)
            if (g_e[k].used && g_e[k].i.id < last && (!e || g_e[k].i.id > e->i.id)) e = &g_e[k];
        if (!e) return 0;
        last = e->i.id;
    }
    *out = e->i;
    return 1;
}

int dlm_find(int id, dlm_info_t* out) {
    entry_t* e = by_id(id);
    if (!e) return 0;
    *out = e->i;
    return 1;
}

void dlm_cancel(int id) {
    entry_t* e = by_id(id);
    if (!e) return;
    if (e->i.state == DLM_QUEUED) { e->i.state = DLM_CANCELLED; save_list(); }
    else if (e->i.state == DLM_RUNNING) e->cancel = 1;        /* (its task stops at the next piece) */
    g_gen++;
}

void dlm_retry(int id) {
    entry_t* e = by_id(id);
    if (!e || e->i.state == DLM_QUEUED || e->i.state == DLM_RUNNING) return;
    e->cancel = 0;
    e->i.state = DLM_QUEUED;
    e->i.err[0] = 0;
    e->i.when = rtc_now();
    ensure_workers();
    g_gen++;
}

void dlm_remove(int id) {
    entry_t* e = by_id(id);
    if (!e || e->i.state == DLM_QUEUED || e->i.state == DLM_RUNNING) return;
    kfree(e->full_url);
    memset(e, 0, sizeof(*e));
    save_list();
    g_gen++;
}

void dlm_clear(void) {
    load_list();
    for (int k = 0; k < DLM_MAX; k++) {
        entry_t* e = &g_e[k];
        if (!e->used || e->i.state == DLM_QUEUED || e->i.state == DLM_RUNNING) continue;
        kfree(e->full_url);
        memset(e, 0, sizeof(*e));
    }
    save_list();
    g_gen++;
}

int dlm_running(uint32_t* got, uint32_t* total) {
    int n = 0;
    uint32_t g = 0, t = 0;
    int unknown = 0;
    for (int k = 0; k < DLM_MAX; k++) {
        entry_t* e = &g_e[k];
        if (!e->used || (e->i.state != DLM_RUNNING && e->i.state != DLM_QUEUED)) continue;
        n++;
        g += e->i.got;
        if (e->i.total) t += e->i.total; else unknown = 1;
    }
    if (got) *got = g;
    if (total) *total = unknown ? 0 : t;
    return n;
}
