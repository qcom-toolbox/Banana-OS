/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clipboard.h"
#include "kheap.h"
#include "kstring.h"
#include "spinlock.h"
#include "fs.h"

static char*    g_text;
static uint32_t g_len;
static uint32_t g_gen;

/* the history: g_hist[0] is the newest */
typedef struct { char* text; uint32_t len; int pinned; } clip_item_t;
static clip_item_t g_hist[CLIP_HIST_MAX];
static int         g_nhist;
static uint32_t    g_hgen;
static spinlock_t  g_lock = SPINLOCK_INIT;     /* (apps copy from other cores) */

#define PINNED_FILE "/etc/clipboard-pinned"

/* puts text (kmalloc'd, owned from now on) at the top: a copy already
 * there moves up instead; the oldest entry not pinned goes when it is full.
 * Called with the lock held; what it drops, it returns (kfree it after) */
static char* hist_push(char* text, uint32_t len, int pinned) {
    for (int i = 0; i < g_nhist; i++) {
        if (g_hist[i].len != len || memcmp(g_hist[i].text, text, len) != 0) continue;
        clip_item_t it = g_hist[i];
        memmove(&g_hist[1], &g_hist[0], (size_t)i * sizeof(clip_item_t));
        it.pinned |= pinned;
        g_hist[0] = it;
        g_hgen++;
        return text;                                     /* (the new copy is not needed) */
    }
    char* drop = NULL;
    if (g_nhist == CLIP_HIST_MAX) {
        int last = g_nhist - 1;
        while (last >= 0 && g_hist[last].pinned) last--;
        if (last < 0 && !pinned) return text;           /* keep every pinned entry */
        if (last < 0) last = g_nhist - 1;                /* (all pinned: the oldest goes) */
        drop = g_hist[last].text;
        memmove(&g_hist[last], &g_hist[last + 1], (size_t)(g_nhist - last - 1) * sizeof(clip_item_t));
        g_nhist--;
    }
    memmove(&g_hist[1], &g_hist[0], (size_t)g_nhist * sizeof(clip_item_t));
    g_hist[0] = (clip_item_t){ text, len, pinned };
    g_nhist++;
    g_hgen++;
    return drop;
}

void clipboard_set(const char* text, uint32_t len) {
    if (len > CLIPBOARD_MAX) len = CLIPBOARD_MAX;
    char* t = (char*)kmalloc(len + 1);
    char* h = len ? (char*)kmalloc(len + 1) : NULL;
    if (!t) { kfree(h); return; }
    memcpy(t, text, len);
    t[len] = 0;
    if (h) { memcpy(h, text, len); h[len] = 0; }
    uintptr_t f = spin_lock_irq(&g_lock);
    char* old = g_text;
    g_text = t;
    g_len = len;
    g_gen++;
    char* drop = h ? hist_push(h, len, 0) : NULL;
    spin_unlock_irq(&g_lock, f);
    kfree(old);
    kfree(drop);
}

const char* clipboard_get(uint32_t* len) {
    if (len) *len = g_text ? g_len : 0;
    return g_text ? g_text : "";
}

uint32_t clipboard_generation(void) { return g_gen; }

/* ── the history ───────────────────────────────────────────────────── */

int clipboard_hist_count(void) { return g_nhist; }
uint32_t clipboard_hist_generation(void) { return g_hgen; }

int clipboard_hist_peek(int i, char* out, uint32_t cap, uint32_t* len, int* pinned) {
    int rc = -1;
    uintptr_t f = spin_lock_irq(&g_lock);
    if (i >= 0 && i < g_nhist) {
        if (out && cap) {
            uint32_t n = g_hist[i].len < cap - 1 ? g_hist[i].len : cap - 1;
            memcpy(out, g_hist[i].text, n);
            out[n] = 0;
        }
        if (len) *len = g_hist[i].len;
        if (pinned) *pinned = g_hist[i].pinned;
        rc = 0;
    }
    spin_unlock_irq(&g_lock, f);
    return rc;
}

static void save_pinned(void);

int clipboard_hist_use(int i) {
    uint32_t len = 0;
    if (clipboard_hist_peek(i, NULL, 0, &len, NULL) < 0) return -1;
    uint32_t cap = len + 1;
    char* t = (char*)kmalloc(cap);
    if (!t) return -1;
    /* Another core may replace entry i while the allocation yields. peek
     * reports the full new length even when its copied text was truncated. */
    if (clipboard_hist_peek(i, t, cap, &len, NULL) < 0 || len >= cap) {
        kfree(t);
        return -1;
    }
    clipboard_set(t, len);                               /* (it moves to the top) */
    kfree(t);
    return 0;
}

int clipboard_hist_delete(int i) {
    char* drop = NULL;
    int was_pinned = 0;
    uintptr_t f = spin_lock_irq(&g_lock);
    if (i >= 0 && i < g_nhist) {
        drop = g_hist[i].text;
        was_pinned = g_hist[i].pinned;
        memmove(&g_hist[i], &g_hist[i + 1], (size_t)(g_nhist - i - 1) * sizeof(clip_item_t));
        g_nhist--;
        g_hgen++;
    }
    spin_unlock_irq(&g_lock, f);
    if (!drop) return -1;
    kfree(drop);
    if (was_pinned) save_pinned();
    return 0;
}

int clipboard_hist_pin(int i, int on) {
    int rc = -1;
    uintptr_t f = spin_lock_irq(&g_lock);
    if (i >= 0 && i < g_nhist) { g_hist[i].pinned = on != 0; g_hgen++; rc = 0; }
    spin_unlock_irq(&g_lock, f);
    if (rc == 0) save_pinned();
    return rc;
}

int clipboard_hist_clear(void) {
    char* drop[CLIP_HIST_MAX];
    int nd = 0, k = 0;
    uintptr_t f = spin_lock_irq(&g_lock);
    for (int i = 0; i < g_nhist; i++) {
        if (g_hist[i].pinned) g_hist[k++] = g_hist[i];
        else drop[nd++] = g_hist[i].text;
    }
    g_nhist = k;
    if (nd) g_hgen++;
    spin_unlock_irq(&g_lock, f);
    for (int i = 0; i < nd; i++) kfree(drop[i]);
    return nd;
}

/* ── pinned entries across restarts ───────────────────────────────────
 * /etc/clipboard-pinned: for each, its length in decimal, a newline, the
 * bytes, a newline - the oldest first */

static void save_pinned(void) {
    uint32_t total = 0;
    int n = 0;
    uintptr_t f = spin_lock_irq(&g_lock);
    for (int i = 0; i < g_nhist; i++) if (g_hist[i].pinned) { total += g_hist[i].len + 12; n++; }
    spin_unlock_irq(&g_lock, f);
    if (!n) { if (fs_find_file(PINNED_FILE) >= 0) fs_delete(PINNED_FILE, 0); return; }
    char* buf = (char*)kmalloc(total + 1);
    if (!buf) return;
    uint32_t off = 0;
    f = spin_lock_irq(&g_lock);
    for (int i = g_nhist - 1; i >= 0; i--) {
        if (!g_hist[i].pinned) continue;
        if (off + g_hist[i].len + 12 > total) break;     /* (changed meanwhile: what fits) */
        off += (uint32_t)ksnprintf(buf + off, 12, "%u\n", g_hist[i].len);
        memcpy(buf + off, g_hist[i].text, g_hist[i].len);
        off += g_hist[i].len;
        buf[off++] = '\n';
    }
    spin_unlock_irq(&g_lock, f);
    fs_mkdir_p("/etc");
    fs_write_path(PINNED_FILE, buf, off);
    kfree(buf);
}

void clipboard_hist_load(void) {
    static int done;
    if (done) return;
    done = 1;
    int fi = fs_find_file(PINNED_FILE);
    if (fi < 0) return;
    fs_pin(fi);
    fs_file_t* fl = fs_get_file(fi);
    if (!fl || !fl->loaded || !fl->content) { fs_unpin(fi); return; }
    const char* p = fl->content, *end = fl->content + fl->size;
    while (p < end) {
        uint32_t len = 0;
        if (*p < '0' || *p > '9') break;
        while (p < end && *p >= '0' && *p <= '9') {
            uint32_t digit = (uint32_t)(*p++ - '0');
            if (len > (CLIPBOARD_MAX - digit) / 10u) goto finished;
            len = len * 10u + digit;
        }
        if (p >= end || *p != '\n') break;
        p++;
        if ((uint32_t)(end - p) <= len || p[len] != '\n') break;
        char* t = (char*)kmalloc(len + 1);
        if (!t) break;
        memcpy(t, p, len);
        t[len] = 0;
        p += len + 1;
        uintptr_t f = spin_lock_irq(&g_lock);
        char* drop = hist_push(t, len, 1);
        spin_unlock_irq(&g_lock, f);
        kfree(drop);
    }
finished:
    fs_unpin(fi);
}
