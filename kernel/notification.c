/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "notification.h"
#include "kstring.h"
#include "timer.h"
#include "spinlock.h"
#include "fb.h"
#include "gfx.h"
#include "theme.h"
#include "winframe.h"

/* Bounded ring buffer for history */
static notify_item_t g_history[NOTIFY_HIST_MAX];
static int           g_hist_count = 0;
static int           g_hist_head  = 0; /* index of newest item */
static uint32_t      g_next_id    = 1;
static spinlock_t    g_notify_lock = SPINLOCK_INIT;

/* Rate limiting tracking per application */
#define NOTIFY_RATE_TRACK_MAX 8
typedef struct {
    char     app[NOTIFY_APP_MAX];
    uint32_t timestamps[NOTIFY_RATE_MAX_BURST];
    int      count;
} app_rate_t;

static app_rate_t g_rates[NOTIFY_RATE_TRACK_MAX];
static int        g_rate_count = 0;

/* Active toast */
static notify_item_t g_active_toast;
static uint32_t      g_toast_expires_ms = 0;
static int           g_has_toast = 0;

/* Action callback */
static notify_action_fn g_action_cb = NULL;
static void*            g_action_data = NULL;

void notify_init(void) {
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    memset(g_history, 0, sizeof(g_history));
    memset(g_rates, 0, sizeof(g_rates));
    g_hist_count = 0;
    g_hist_head = 0;
    g_next_id = 1;
    g_rate_count = 0;
    g_has_toast = 0;
    g_toast_expires_ms = 0;
    g_action_cb = NULL;
    g_action_data = NULL;
    spin_unlock_irq(&g_notify_lock, f);
}

void notify_set_action_callback(notify_action_fn cb, void* user_data) {
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    g_action_cb = cb;
    g_action_data = user_data;
    spin_unlock_irq(&g_notify_lock, f);
}

/* Rate limiter helper: returns 1 if allowed, 0 if rate-limited */
static int check_rate_limit(const char* app, uint32_t now) {
    if (!app || !app[0]) return 1; /* anonymous allowed */
    int slot = -1;
    for (int i = 0; i < g_rate_count; i++) {
        if (strcmp(g_rates[i].app, app) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_rate_count < NOTIFY_RATE_TRACK_MAX) {
            slot = g_rate_count++;
            kstrlcpy(g_rates[slot].app, app, NOTIFY_APP_MAX);
            g_rates[slot].count = 0;
        } else {
            /* Replace oldest entry */
            slot = 0;
            kstrlcpy(g_rates[slot].app, app, NOTIFY_APP_MAX);
            g_rates[slot].count = 0;
        }
    }

    app_rate_t* r = &g_rates[slot];
    /* Prune timestamps older than window */
    int valid = 0;
    for (int i = 0; i < r->count; i++) {
        if ((uint32_t)(now - r->timestamps[i]) < NOTIFY_RATE_WINDOW_MS) {
            r->timestamps[valid++] = r->timestamps[i];
        }
    }
    r->count = valid;

    if (r->count >= NOTIFY_RATE_MAX_BURST) {
        return 0; /* Rate-limited! */
    }

    r->timestamps[r->count++] = now;
    return 1;
}

int notify_post(const char* app, const char* title, const char* body, int action_id) {
    if (!title && !body) return -1;
    uint32_t now = timer_ms();
    char source[NOTIFY_APP_MAX];
    kstrlcpy(source, app && *app ? app : "System", sizeof(source));

    uintptr_t f = spin_lock_irq(&g_notify_lock);

    if (!check_rate_limit(source, now)) {
        spin_unlock_irq(&g_notify_lock, f);
        return 0; /* Rate-limited */
    }

    if (!g_next_id || g_next_id > 0x7fffffffu) g_next_id = 1;
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;

    notify_item_t item;
    memset(&item, 0, sizeof(item));
    item.id = id;
    kstrlcpy(item.app, source, NOTIFY_APP_MAX);
    kstrlcpy(item.title, title ? title : "", NOTIFY_TITLE_MAX);
    kstrlcpy(item.body, body ? body : "", NOTIFY_BODY_MAX);
    item.action_id = action_id;
    item.timestamp_ms = now;
    item.read = 0;

    /* Add to ring buffer */
    if (g_hist_count == 0) {
        g_hist_head = 0;
        g_history[0] = item;
        g_hist_count = 1;
    } else {
        g_hist_head = (g_hist_head + 1) % NOTIFY_HIST_MAX;
        g_history[g_hist_head] = item;
        if (g_hist_count < NOTIFY_HIST_MAX) {
            g_hist_count++;
        }
    }

    /* Set as active toast */
    g_active_toast = item;
    g_toast_expires_ms = now + NOTIFY_TOAST_DURATION_MS;
    g_has_toast = 1;

    spin_unlock_irq(&g_notify_lock, f);
    return (int)id;
}

int notify_trigger_action(uint32_t id) {
    int act = -1;
    notify_action_fn cb = NULL;
    void* udata = NULL;

    uintptr_t f = spin_lock_irq(&g_notify_lock);
    for (int i = 0; i < g_hist_count; i++) {
        int idx = (g_hist_head - i + NOTIFY_HIST_MAX) % NOTIFY_HIST_MAX;
        if (g_history[idx].id == id) {
            act = g_history[idx].action_id;
            g_history[idx].read = 1;
            break;
        }
    }
    if (g_has_toast && g_active_toast.id == id) {
        g_has_toast = 0;
    }
    cb = g_action_cb;
    udata = g_action_data;
    spin_unlock_irq(&g_notify_lock, f);

    if (act >= 0 && cb) {
        cb(act, udata);
        return 0;
    }
    return act >= 0 ? 0 : -1;
}

int notify_count(void) {
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    int c = g_hist_count;
    spin_unlock_irq(&g_notify_lock, f);
    return c;
}

int notify_peek(int index, notify_item_t* out) {
    if (index < 0 || !out) return -1;
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    if (index >= g_hist_count) {
        spin_unlock_irq(&g_notify_lock, f);
        return -1;
    }
    /* index 0 is newest */
    int idx = (g_hist_head - index + NOTIFY_HIST_MAX) % NOTIFY_HIST_MAX;
    *out = g_history[idx];
    spin_unlock_irq(&g_notify_lock, f);
    return 0;
}

int notify_clear_history(void) {
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    int cleared = g_hist_count;
    g_hist_count = 0;
    g_hist_head = 0;
    g_has_toast = 0;
    spin_unlock_irq(&g_notify_lock, f);
    return cleared;
}

int notify_has_active_toast(void) {
    uint32_t now = timer_ms();
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    if (g_has_toast && (int32_t)(now - g_toast_expires_ms) >= 0) {
        g_has_toast = 0;
    }
    int active = g_has_toast;
    spin_unlock_irq(&g_notify_lock, f);
    return active;
}

int notify_get_active_toast(notify_item_t* out, uint32_t* remaining_ms) {
    uint32_t now = timer_ms();
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    if (!g_has_toast || (int32_t)(now - g_toast_expires_ms) >= 0) {
        g_has_toast = 0;
        spin_unlock_irq(&g_notify_lock, f);
        return 0;
    }
    if (out) *out = g_active_toast;
    if (remaining_ms) {
        *remaining_ms = g_toast_expires_ms - now;
    }
    spin_unlock_irq(&g_notify_lock, f);
    return 1;
}

void notify_dismiss_toast(void) {
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    g_has_toast = 0;
    spin_unlock_irq(&g_notify_lock, f);
}

void notify_poll(void) {
    uint32_t now = timer_ms();
    uintptr_t f = spin_lock_irq(&g_notify_lock);
    if (g_has_toast && (int32_t)(now - g_toast_expires_ms) >= 0) {
        g_has_toast = 0;
    }
    spin_unlock_irq(&g_notify_lock, f);
}

/* Toast geometry: bottom-right, above the taskbar. Shared by drawing/hit testing. */
#define TOAST_W 260
#define TOAST_H 64
#define TOAST_MARGIN 12

static void draw_bevel_box(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    if (w <= 2 || h <= 2) return;
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static int get_toast_rect(const fb_info_t* fi, int* x, int* y, int* w, int* h) {
    if (fi->width < TOAST_W || fi->height < WIN_TASKBAR + TOAST_H) return 0;
    *w = TOAST_W;
    *h = TOAST_H;
    *x = (int)fi->width - TOAST_W - TOAST_MARGIN;
    *y = (int)fi->height - WIN_TASKBAR - TOAST_H - TOAST_MARGIN;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    return 1;
}

void notify_draw_toast(const fb_info_t* fi) {
    if (!fi || !notify_has_active_toast()) return;
    notify_item_t toast;
    uint32_t rem = 0;
    if (!notify_get_active_toast(&toast, &rem)) return;

    int tx, ty, tw, th;
    if (!get_toast_rect(fi, &tx, &ty, &tw, &th)) return;

    /* Card background with bevel */
    draw_bevel_box(tx, ty, tw, th, TC(0x00222A36u), TC(0x005E708Au), TC(0x0010141Bu));

    /* Progress/timer bar at top */
    int prog_w = (int)((uint64_t)(tw - 4) * rem / NOTIFY_TOAST_DURATION_MS);
    if (prog_w > tw - 4) prog_w = tw - 4;
    if (prog_w < 0) prog_w = 0;
    gfx_fill_rect(tx + 2, ty + 2, prog_w, 2, TC(0x0068A8F0u));

    /* Close 'x' icon at top right */
    gfx_draw_text(tx + tw - 16, ty + 6, "x", TC(0x009AA6B6u), TC(0x00222A36u));

    /* App header & title */
    char hdr[64];
    ksnprintf(hdr, sizeof(hdr), "%s: %s", toast.app, toast.title);
    if (strlen(hdr) > 28) {
        hdr[25] = '.'; hdr[26] = '.'; hdr[27] = '.'; hdr[28] = 0;
    }
    gfx_draw_text(tx + 10, ty + 8, hdr, TC(0x00FFFFFFu), TC(0x00222A36u));

    /* Body text */
    char body[64];
    kstrlcpy(body, toast.body, sizeof(body));
    if (strlen(body) > 30) {
        body[27] = '.'; body[28] = '.'; body[29] = '.'; body[30] = 0;
    }
    gfx_draw_text(tx + 10, ty + 26, body, TC(0x00C2CCD8u), TC(0x00222A36u));

    /* Action prompt if action_id >= 0 */
    if (toast.action_id >= 0) {
        gfx_draw_text(tx + 10, ty + 44, "[Click to open]", TC(0x0068A8F0u), TC(0x00222A36u));
    }
}

int notify_click(const fb_info_t* fi, int mx, int my) {
    if (!fi || !notify_has_active_toast()) return 0;
    int tx, ty, tw, th;
    if (!get_toast_rect(fi, &tx, &ty, &tw, &th)) return 0;

    if (mx >= tx && mx < tx + tw && my >= ty && my < ty + th) {
        /* Check if clicked on 'x' */
        if (mx >= tx + tw - 24 && my <= ty + 20) {
            notify_dismiss_toast();
            return 1;
        }
        /* Clicked on toast body */
        notify_item_t toast;
        if (notify_get_active_toast(&toast, NULL)) {
            uint32_t tid = toast.id;
            notify_dismiss_toast();
            notify_trigger_action(tid);
            return 1;
        }
    }
    return 0;
}
