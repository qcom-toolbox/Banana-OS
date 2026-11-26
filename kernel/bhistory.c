/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bhistory.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"

#define BH_MAX  120
#define BH_PATH fs_home_join("/.config/browser-history")

typedef struct {
    char     url[BH_URL];
    char     title[BH_TITLE];
    uint32_t hits, seq;             /* how often, and when last (a counter) */
} visit_t;

static visit_t g_v[BH_MAX];
static int g_n, g_loaded;
static uint32_t g_seq;

static void save(void) {
    uint32_t cap = (uint32_t)g_n * (BH_URL + BH_TITLE + 16) + 1, n = 0;
    char* b = (char*)kmalloc(cap);
    if (!b) return;
    for (int i = 0; i < g_n; i++)
        n += (uint32_t)ksnprintf(b + n, cap - n, "%u\t%s\t%s\n", g_v[i].hits, g_v[i].url, g_v[i].title);
    fs_mkdir_p(fs_home_join("/.config"));
    fs_write_path(BH_PATH, b, n);
    kfree(b);
}

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    int idx = fs_find_file(BH_PATH);
    fs_file_t* f = idx >= 0 ? fs_file_info(idx) : NULL;
    if (!f || !f->size || f->size > (1u << 20)) return;
    char* b = (char*)kmalloc(f->size + 1);
    if (!b) return;
    int n = fs_read(idx, 0, b, f->size);
    b[n > 0 ? n : 0] = 0;
    for (char* l = b; *l && g_n < BH_MAX; ) {
        char* e = strchr(l, '\n');
        if (e) *e = 0;
        char* t1 = strchr(l, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t2) {
            *t1 = *t2 = 0;
            visit_t* v = &g_v[g_n++];
            k_parse_u32(l, &v->hits);
            kstrlcpy(v->url, t1 + 1, sizeof(v->url));
            kstrlcpy(v->title, t2 + 1, sizeof(v->title));
            v->seq = ++g_seq;
        }
        if (!e) break;
        l = e + 1;
    }
    kfree(b);
}

void bh_visit(const char* url, const char* title) {
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8) && strncasecmp(url, "file://", 7)) return;
    if (strlen(url) >= BH_URL) return;
    load();
    int i = 0;
    for (; i < g_n; i++) if (!strcmp(g_v[i].url, url)) break;
    if (i == g_n) {
        if (g_n == BH_MAX) {                      /* full: the one least used, longest ago, goes */
            int worst = 0;
            for (int k = 1; k < g_n; k++)
                if (g_v[k].hits < g_v[worst].hits || (g_v[k].hits == g_v[worst].hits && g_v[k].seq < g_v[worst].seq)) worst = k;
            i = worst;
        } else g_n++;
        memset(&g_v[i], 0, sizeof(g_v[i]));
        kstrlcpy(g_v[i].url, url, sizeof(g_v[i].url));
    }
    g_v[i].hits++;
    g_v[i].seq = ++g_seq;
    for (const char* p = title; p && *p; p++)        /* (one line in the file) */
        if (*p == '\t' || *p == '\n') { title = NULL; break; }
    if (title && title[0]) kstrlcpy(g_v[i].title, title, sizeof(g_v[i].title));
    save();
}

static int has(const char* s, const char* q, int qn) {
    for (; *s; s++) if (!strncasecmp(s, q, (size_t)qn)) return 1;
    return 0;
}

int bh_match(const char* q, int* out, int max) {
    load();
    int qn = (int)strlen(q), n = 0;
    if (!qn) return 0;
    for (int i = 0; i < g_n; i++) {
        const char* u = g_v[i].url;
        const char* bare = strstr(u, "://") ? strstr(u, "://") + 3 : u;
        if (strncasecmp(bare, "www.", 4) == 0) bare += 4;
        int score;
        if (!strncasecmp(bare, q, (size_t)qn) || !strncasecmp(u, q, (size_t)qn)) score = 3;     /* starts with it */
        else if (has(u, q, qn)) score = 2;
        else if (has(g_v[i].title, q, qn)) score = 1;
        else continue;
        /* in order: score, then visits, then how recent */
        int at = n;
        while (at > 0) {
            visit_t* o = &g_v[out[at - 1] & 0xFFFF];
            int os = out[at - 1] >> 16;
            if (os > score || (os == score && (o->hits > g_v[i].hits || (o->hits == g_v[i].hits && o->seq > g_v[i].seq)))) break;
            at--;
        }
        if (at >= max) continue;
        if (n < max) n++;
        for (int k = n - 1; k > at; k--) out[k] = out[k - 1];
        out[at] = score << 16 | i;
    }
    for (int k = 0; k < n; k++) out[k] &= 0xFFFF;
    return n;
}

const char* bh_url(int i)   { return i >= 0 && i < g_n ? g_v[i].url : ""; }
const char* bh_title(int i) { return i >= 0 && i < g_n ? g_v[i].title : ""; }
