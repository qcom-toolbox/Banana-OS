#include "appwin.h"
#include "keyboard.h"
#include "winframe.h"
#include "gfx.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"

#define TITLE_H  20
#define BORDER   3
#define EVQ      64

#define C_PANEL  0x001D232Cu
#define C_TITLE  0x00384562u

typedef struct {
    int       used;
    int       owner;
    char      title[64];
    int       x, y;                /* outer top-left */
    int       cw, ch;              /* client (pixel buffer) size */
    uint32_t* px;
    banana_event_t q[EVQ];
    int       qh, qt;
    int       dragging, ddx, ddy;
    int       close_clicks;
    int       min;                 /* minimized (taskbar) */
    int       last_mx, last_my, last_left, inside_down;
    uint32_t  gen;
    /* resizing (only for apps that asked: win_set_resizable) */
    int       resizable, min_w, min_h;
    int       resizing, rdx, rdy;
    int       pending, pw, ph;     /* a new client size, applied when the app reads BANANA_EV_RESIZE */
    int       maxed, sx, sy, sw, sh;
    uint32_t  title_ms;
} awin_t;

#define GRIP 14

static awin_t   g_w[APPWIN_MAX];
static int      g_order[APPWIN_MAX];  /* back to front: window ids */
static int      g_norder;
static int      g_focused;            /* the app windows are the desktop's front window */
static uint32_t g_gen;
static int      g_killed[64];         /* owners whose app must stop (index: owner % 64) */
static int      g_esc;                /* ESC [ sequence state for the keyboard */
static int      g_new_window;         /* a window opened since appwin_take_new() */

static int scr_w(void) { const fb_info_t* fi = fb_info(); return fi && fi->width ? (int)fi->width : 800; }
static int scr_h(void) { const fb_info_t* fi = fb_info(); return fi && fi->height ? (int)fi->height : 600; }
static int outer_w(const awin_t* w) { return w->cw + 2 * BORDER; }
static int outer_h(const awin_t* w) { return w->ch + TITLE_H + 2 * BORDER; }

static awin_t* get(int id, int owner) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used || g_w[id].owner != owner) return NULL;
    return &g_w[id];
}

static void push(awin_t* w, const banana_event_t* ev) {
    /* mouse moves coalesce: only the latest position matters */
    if (ev->type == BANANA_EV_MOUSE_MOVE && w->qh != w->qt) {
        int last = (w->qh + EVQ - 1) % EVQ;
        if (w->q[last].type == BANANA_EV_MOUSE_MOVE) { w->q[last] = *ev; return; }
    }
    int next = (w->qh + 1) % EVQ;
    if (next == w->qt) return;            /* full: dropped */
    w->q[w->qh] = *ev;
    w->qh = next;
}

static void push_simple(awin_t* w, int type, int x, int y, int button, int key) {
    banana_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.button = button;
    ev.buttons = button;
    ev.key = key;
    push(w, &ev);
}

static void raise(int id) {
    int pos = -1;
    for (int i = 0; i < g_norder; i++) if (g_order[i] == id) pos = i;
    if (pos < 0) return;
    for (int i = pos; i < g_norder - 1; i++) g_order[i] = g_order[i + 1];
    g_order[g_norder - 1] = id;
    g_gen++;
}

static void unlink_order(int id) {
    int n = 0;
    for (int i = 0; i < g_norder; i++) if (g_order[i] != id) g_order[n++] = g_order[i];
    g_norder = n;
}

/* the topmost window that is not minimized */
static awin_t* front(void) {
    for (int i = g_norder - 1; i >= 0; i--) if (!g_w[g_order[i]].min) return &g_w[g_order[i]];
    return NULL;
}

/* ── app side ─────────────────────────────────────────────────────── */

/* the biggest client area that fits on the screen above the taskbar */
static int max_cw(void) { return scr_w() - 2 * BORDER; }
static int max_ch(void) { return scr_h() - 28 - TITLE_H - 2 * BORDER; }

int appwin_open(int owner, const char* title, int w, int h) {
    if (w < 32) w = 32;
    if (h < 16) h = 16;
    if (w > max_cw()) w = max_cw();
    if (h > max_ch()) h = max_ch();
    for (int i = 0; i < APPWIN_MAX; i++) {
        awin_t* a = &g_w[i];
        if (a->used) continue;
        uint32_t* px = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4);
        if (!px) return -1;
        memset(a, 0, sizeof(*a));
        memset32(px, 0x00FFFFFFu, (size_t)w * (size_t)h);
        a->used = 1;
        a->owner = owner;
        a->px = px;
        a->cw = w;
        a->ch = h;
        kstrlcpy(a->title, title && *title ? title : "App", sizeof(a->title));
        /* cascade new windows */
        a->x = 120 + (i % 4) * 28;
        a->y = 40 + (i % 4) * 24;
        if (a->x + outer_w(a) > scr_w()) a->x = scr_w() - outer_w(a);
        if (a->y + outer_h(a) > scr_h() - 28) a->y = scr_h() - 28 - outer_h(a);
        if (a->x < 0) a->x = 0;
        if (a->y < 0) a->y = 0;
        g_order[g_norder++] = i;
        g_new_window = 1;
        g_killed[owner & 63] = 0;
        g_gen++;
        return i;
    }
    return -1;
}

uint32_t* appwin_pixels(int id, int owner) {
    awin_t* w = get(id, owner);
    return w ? w->px : NULL;
}

void appwin_update(int id, int owner) {
    awin_t* w = get(id, owner);
    if (w) { w->gen++; g_gen++; }
}

/* the pending size takes effect: a new pixel buffer, the old picture
 * copied into its top-left corner (the app redraws on BANANA_EV_RESIZE) */
static void apply_resize(awin_t* w) {
    if (!w->pending) return;
    w->pending = 0;
    if (w->pw == w->cw && w->ph == w->ch) return;
    uint32_t* np = (uint32_t*)kmalloc((uint32_t)w->pw * (uint32_t)w->ph * 4);
    if (!np) return;                         /* no memory: stays as it is */
    memset32(np, 0x00FFFFFFu, (size_t)w->pw * (size_t)w->ph);
    int cw = w->cw < w->pw ? w->cw : w->pw, ch = w->ch < w->ph ? w->ch : w->ph;
    for (int y = 0; y < ch; y++) memcpy(np + (size_t)y * (size_t)w->pw, w->px + (size_t)y * (size_t)w->cw, (size_t)cw * 4);
    kfree(w->px);
    w->px = np;
    w->cw = w->pw;
    w->ch = w->ph;
    w->gen++;
    g_gen++;
}

int appwin_event(int id, int owner, banana_event_t* ev) {
    awin_t* w = get(id, owner);
    if (!w || w->qh == w->qt) return 0;
    *ev = w->q[w->qt];
    w->qt = (w->qt + 1) % EVQ;
    /* only now, with the app inside this call, can its buffer change */
    if (ev->type == BANANA_EV_RESIZE) {
        apply_resize(w);
        ev->x = w->cw;
        ev->y = w->ch;
    }
    return 1;
}

/* asks the app to take a new client size */
static void request_resize(awin_t* w, int cw, int ch) {
    if (cw < w->min_w) cw = w->min_w;
    if (ch < w->min_h) ch = w->min_h;
    if (cw > max_cw()) cw = max_cw();
    if (ch > max_ch()) ch = max_ch();
    if (cw == w->cw && ch == w->ch && !w->pending) return;
    int had = w->pending;
    w->pending = 1;
    w->pw = cw;
    w->ph = ch;
    if (!had) push_simple(w, BANANA_EV_RESIZE, cw, ch, 0, 0);   /* one event; the latest size wins */
    g_gen++;
}

void appwin_set_resizable(int id, int owner, int min_w, int min_h) {
    awin_t* w = get(id, owner);
    if (!w) return;
    w->resizable = 1;
    w->min_w = min_w < 32 ? 32 : min_w;
    w->min_h = min_h < 16 ? 16 : min_h;
    g_gen++;
}

static void destroy(int id) {
    awin_t* w = &g_w[id];
    if (!w->used) return;
    kfree(w->px);
    w->px = NULL;
    w->used = 0;
    unlink_order(id);
    g_gen++;
}

void appwin_close(int id, int owner) {
    if (get(id, owner)) destroy(id);
}

void appwin_close_owner(int owner) {
    for (int i = 0; i < APPWIN_MAX; i++) if (g_w[i].used && g_w[i].owner == owner) destroy(i);
    g_killed[owner & 63] = 0;
}

void appwin_set_title(int id, int owner, const char* title) {
    awin_t* w = get(id, owner);
    if (w && title) { kstrlcpy(w->title, title, sizeof(w->title)); g_gen++; }
}

void appwin_size(int id, int owner, int* wp, int* hp) {
    awin_t* w = get(id, owner);
    if (wp) *wp = w ? w->cw : 0;
    if (hp) *hp = w ? w->ch : 0;
}

int appwin_kill_requested(int owner) {
    return g_killed[owner & 63];
}

/* ── desktop side ─────────────────────────────────────────────────── */

int appwin_is_open(void) { return g_norder > 0; }

static int win_at(int mx, int my) {
    for (int i = g_norder - 1; i >= 0; i--) {
        awin_t* w = &g_w[g_order[i]];
        if (!w->min && mx >= w->x && mx < w->x + outer_w(w) && my >= w->y && my < w->y + outer_h(w)) return g_order[i];
    }
    return -1;
}

int appwin_contains(int mx, int my) { return win_at(mx, my) >= 0; }

/* over the resize grip of the app window under the mouse, or resizing one */
int appwin_resize_cursor(int mx, int my) {
    for (int i = 0; i < g_norder; i++) if (g_w[g_order[i]].resizing) return 1;
    int id = win_at(mx, my);
    if (id < 0) return 0;
    awin_t* w = &g_w[id];
    return w->resizable && mx >= w->x + outer_w(w) - GRIP && my >= w->y + outer_h(w) - GRIP;
}

uint32_t appwin_signature(void) {
    uint32_t s = g_gen * 2654435761u ^ (uint32_t)g_focused * 97u;
    for (int i = 0; i < APPWIN_MAX; i++)
        if (g_w[i].used) s ^= (g_w[i].gen + 1) * 40503u * (uint32_t)(i + 1) ^ (uint32_t)(g_w[i].x << 16 | g_w[i].y);
    return s;
}

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void blit(const awin_t* w, int x0, int y0) {
    int stride, tw, th;
    uint32_t* dst = fb_target(&stride, &tw, &th);
    if (!dst) return;
    for (int y = 0; y < w->ch; y++) {
        int ty = y0 + y;
        if (ty < 0 || ty >= th) continue;
        int xs = x0 < 0 ? -x0 : 0;
        int xe = x0 + w->cw > tw ? tw - x0 : w->cw;
        if (xe > xs) memcpy(dst + (size_t)ty * (size_t)stride + x0 + xs, w->px + (size_t)y * (size_t)w->cw + xs,
                            (size_t)(xe - xs) * 4);
    }
}

void appwin_draw(const fb_info_t* fi) {
    (void)fi;
    for (int i = 0; i < g_norder; i++) {
        awin_t* w = &g_w[g_order[i]];
        if (w->min) continue;
        int active = g_focused && w == front();
        int W = outer_w(w), H = outer_h(w);
        bevel(w->x, w->y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
        uint32_t tbg = active ? C_TITLE : 0x002A3240u;
        bevel(w->x + 3, w->y + 3, W - 6, TITLE_H - 2, tbg, 0x00647692u, 0x00111923u);
        char t[64];
        kstrlcpy(t, w->title, sizeof(t));
        int maxc = (W - 50) / 8;
        if (maxc < 1) maxc = 1;
        if ((int)strlen(t) > maxc) t[maxc] = 0;
        gfx_draw_text(w->x + 10, w->y + 8, t, 0x00FFFFFFu, tbg);
        win_draw_button_row(w->x + W, w->y + 5, 14, w->resizable, w->maxed);
        blit(w, w->x + BORDER, w->y + TITLE_H + BORDER);
        if (w->resizable) gfx_draw_grip(w->x + W, w->y + H);
        if (w->resizing || w->pending) {
            /* the size it is about to get */
            int ow = w->pw + 2 * BORDER, oh = w->ph + TITLE_H + 2 * BORDER;
            gfx_fill_rect(w->x, w->y, ow, 2, 0x00F4D35Eu);
            gfx_fill_rect(w->x, w->y + oh - 2, ow, 2, 0x00F4D35Eu);
            gfx_fill_rect(w->x, w->y, 2, oh, 0x00F4D35Eu);
            gfx_fill_rect(w->x + ow - 2, w->y, 2, oh, 0x00F4D35Eu);
        }
    }
}

static void toggle_maximize(awin_t* w) {
    if (!w->resizable) return;
    if (!w->maxed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->cw; w->sh = w->ch;
        w->x = 0; w->y = 0;
        w->maxed = 1;
        request_resize(w, max_cw(), max_ch());
    } else {
        w->x = w->sx; w->y = w->sy;
        w->maxed = 0;
        request_resize(w, w->sw, w->sh);
    }
}

void appwin_click(int mx, int my) {
    int id = win_at(mx, my);
    if (id < 0) return;
    awin_t* w = &g_w[id];
    raise(id);
    int W = outer_w(w);
    if (my < w->y + TITLE_H + 2) {
        int tbtn = win_button_hit(w->x + W, w->y + 5, 14, w->resizable, mx, my);
        if (tbtn == WIN_BTN_MAX) { toggle_maximize(w); return; }
        if (tbtn == WIN_BTN_CLOSE) {
            /* first click: the app is asked to close; a second one stops it */
            if (++w->close_clicks >= 2) g_killed[w->owner & 63] = 1;
            push_simple(w, BANANA_EV_CLOSE, 0, 0, 0, 0);
            return;
        }
        if (tbtn == WIN_BTN_MIN) {                         /* minimize to the taskbar */
            w->min = 1;
            g_gen++;
            return;
        }
        uint32_t now = timer_ms();
        if (w->resizable && now - w->title_ms < 400) {     /* double-click: maximize / restore */
            w->title_ms = 0;
            toggle_maximize(w);
            return;
        }
        w->title_ms = now;
        w->dragging = 1;
        w->ddx = mx - w->x;
        w->ddy = my - w->y;
        return;
    }
    if (w->resizable && mx >= w->x + W - GRIP && my >= w->y + outer_h(w) - GRIP) {
        w->resizing = 1;
        w->maxed = 0;
        w->pw = w->cw;
        w->ph = w->ch;
        w->rdx = w->x + W - mx;
        w->rdy = w->y + outer_h(w) - my;
        return;
    }
    int cx = mx - w->x - BORDER, cy = my - w->y - TITLE_H - BORDER;
    if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch) {
        push_simple(w, BANANA_EV_MOUSE_DOWN, cx, cy, 1, 0);
        w->inside_down = 1;
    }
}

void appwin_rclick(int mx, int my) {
    int id = win_at(mx, my);
    if (id < 0) return;
    awin_t* w = &g_w[id];
    raise(id);
    int cx = mx - w->x - BORDER, cy = my - w->y - TITLE_H - BORDER;
    if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch) {
        push_simple(w, BANANA_EV_MOUSE_DOWN, cx, cy, 2, 0);
        push_simple(w, BANANA_EV_MOUSE_UP, cx, cy, 2, 0);
    }
}

void appwin_mouse(int mx, int my, int left) {
    for (int i = 0; i < APPWIN_MAX; i++) {
        awin_t* w = &g_w[i];
        if (!w->used) continue;
        if (w->dragging) {
            if (!left) w->dragging = 0;
            else {
                int nx = mx - w->ddx, ny = my - w->ddy;
                if (nx < -outer_w(w) + 40) nx = -outer_w(w) + 40;
                if (nx > scr_w() - 40) nx = scr_w() - 40;
                if (ny < 0) ny = 0;
                if (ny > scr_h() - 50) ny = scr_h() - 50;
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; g_gen++; }
            }
        }
        if (w->resizing) {
            int ncw = mx + w->rdx - w->x - 2 * BORDER, nch = my + w->rdy - w->y - TITLE_H - 2 * BORDER;
            if (ncw < w->min_w) ncw = w->min_w;
            if (nch < w->min_h) nch = w->min_h;
            if (ncw > max_cw()) ncw = max_cw();
            if (nch > max_ch()) nch = max_ch();
            if (ncw != w->pw || nch != w->ph) { w->pw = ncw; w->ph = nch; g_gen++; }
            if (!left) {                     /* let go: the app gets the new size */
                w->resizing = 0;
                request_resize(w, w->pw, w->ph);
            }
        }
        int cx = mx - w->x - BORDER, cy = my - w->y - TITLE_H - BORDER;
        int in = cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch;
        if (!left && w->last_left && w->inside_down) {
            push_simple(w, BANANA_EV_MOUSE_UP, cx, cy, 1, 0);
            w->inside_down = 0;
        }
        /* moves inside the window, or anywhere while a button is held in it */
        if ((mx != w->last_mx || my != w->last_my) && (in || w->inside_down) && !w->dragging &&
            front() == w) {
            banana_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = BANANA_EV_MOUSE_MOVE;
            ev.x = cx;
            ev.y = cy;
            ev.buttons = left ? 1 : 0;
            push(w, &ev);
        }
        w->last_mx = mx;
        w->last_my = my;
        w->last_left = left;
    }
}

void appwin_close_all(void) {
    for (int i = 0; i < APPWIN_MAX; i++)
        if (g_w[i].used) push_simple(&g_w[i], BANANA_EV_CLOSE, 0, 0, 0, 0);
}

void appwin_focus(int focused) {
    if (focused == g_focused) return;
    g_focused = focused;
    awin_t* w = front();
    if (w) push_simple(w, BANANA_EV_FOCUS, focused, 0, 0, 0);
    g_gen++;
}

/* the wheel over an app's window: arrow keys, three per notch */
void appwin_wheel(int mx, int my, int dz) {
    int id = win_at(mx, my);
    if (id < 0 || !dz) return;
    int n = (dz < 0 ? -dz : dz) * 3;
    if (n > 30) n = 30;
    for (int i = 0; i < n; i++) push_simple(&g_w[id], BANANA_EV_KEY, 0, 0, 0, dz > 0 ? BANANA_KEY_DOWN : BANANA_KEY_UP);
}

void appwin_key(char c) {
    awin_t* w = front();
    if (!w) return;
    int k = (unsigned char)c;
    /* arrow keys come as ESC [ A..D (Home/End/PgUp/PgDn: H F 5~ 6~) */
    if (g_esc == 1) {
        if (c == '[') { g_esc = 2; return; }
        g_esc = 0;
        push_simple(w, BANANA_EV_KEY, 0, 0, 0, 27);
    } else if (g_esc == 2) {
        g_esc = 0;
        switch (c) {
        case 'A': k = BANANA_KEY_UP; break;
        case 'B': k = BANANA_KEY_DOWN; break;
        case 'C': k = BANANA_KEY_RIGHT; break;
        case 'D': k = BANANA_KEY_LEFT; break;
        case 'H': k = BANANA_KEY_HOME; break;
        case 'F': k = BANANA_KEY_END; break;
        case '5': k = BANANA_KEY_PGUP; g_esc = 3; break;
        case '6': k = BANANA_KEY_PGDN; g_esc = 3; break;
        case '3': k = BANANA_KEY_DELETE; g_esc = 3; break;
        case 'P': k = BANANA_KEY_DELETE; break;
        default: return;
        }
        push_simple(w, BANANA_EV_KEY, 0, 0, 0, k);
        return;
    } else if (g_esc == 3) {           /* the '~' after 5 / 6 / 3 */
        g_esc = 0;
        if (c == '~') return;
    }
    if (c == 27) { g_esc = 1; return; }
    push_simple(w, BANANA_EV_KEY, 0, 0, 0, k);
}

/* ── taskbar / Task Manager ───────────────────────────────────────── */

int appwin_info(int id, char* title, int cap, int* minimized) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return 0;
    if (title) kstrlcpy(title, g_w[id].title, (size_t)cap);
    if (minimized) *minimized = g_w[id].min;
    return 1;
}

int appwin_front_id(void) {
    awin_t* w = front();
    return w ? (int)(w - g_w) : -1;
}

void appwin_minimize(int id, int min) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    g_w[id].min = min ? 1 : 0;
    g_gen++;
}

void appwin_activate(int id) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    g_w[id].min = 0;
    raise(id);
}

void appwin_request_close(int id) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    awin_t* w = &g_w[id];
    if (++w->close_clicks >= 2) g_killed[w->owner & 63] = 1;
    push_simple(w, BANANA_EV_CLOSE, 0, 0, 0, 0);
}

int appwin_any_visible(void) {
    return front() != NULL;
}

int appwin_take_new(void) {
    int n = g_new_window;
    g_new_window = 0;
    return n;
}

void appwin_fkey(int k) {
    awin_t* w = front();
    if (!w) return;
    int c = KEYF_CODE(k), key = 0;
    if (c >= KEYF_F1 && c < KEYF_F1 + 12) key = BANANA_KEY_F1 + (c - KEYF_F1);
    else if (c == KEYF_PLAY) key = BANANA_KEY_PLAY;
    else if (c == KEYF_STOP) key = BANANA_KEY_STOP;
    else if (c == KEYF_NEXT) key = BANANA_KEY_NEXT;
    else if (c == KEYF_PREV) key = BANANA_KEY_PREV;
    if (key) push_simple(w, BANANA_EV_KEY, 0, 0, 0, key);
}
