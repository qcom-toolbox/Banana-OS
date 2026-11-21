/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "appwin.h"
#include "theme.h"
#include "keyboard.h"
#include "winframe.h"
#include "gfx.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "app.h"
#include "mouse_capture.h"

#define TITLE_H  20
#define BORDER   3
#define EVQ      64

#define C_PANEL  TC(0x1D232C)
#define C_TITLE  TC(0x384562)

typedef struct {
    int       used;
    int       owner;
    char      title[64];
    int       x, y;                /* outer top-left */
    int       cw, ch;              /* client (pixel buffer) size */
    uint32_t* px;                  /* what the app draws into */
    int       app_mem;             /* px is the app's own memory (kernel/app.c: an app in ring 3) */
    uint32_t* shown;               /* what the desktop shows: px as of the last win_update */
    banana_event_t q[EVQ];
    int       qh, qt;
    int       dragging, ddx, ddy, dpx, dpy;   /* (dpx, dpy: where the drag started) */
    int       close_clicks;
    int       min;                 /* minimized (taskbar) */
    int       last_mx, last_my, last_left, inside_down;
    uint32_t  gen;
    uint32_t  drawn_gen;           /* gen as the desktop last drew it (appwin_fast_frame) */
    /* resizing (only for apps that asked: win_set_resizable) */
    int       resizable, min_w, min_h;
    int       resizing, rdx, rdy;
    int       pending, pw, ph;     /* a new client size, applied when the app reads BANANA_EV_RESIZE */
    int       maxed, sx, sy, sw, sh;
    int       fullscreen, fx, fy, fw, fh;
    int       snapped;             /* in a half or quarter of the screen (sx.. restore it) */
    int       hidden;              /* on another virtual desktop (kernel/gui.c): not shown */
    uint32_t  title_ms;
    int       wheel;               /* wants BANANA_EV_WHEEL (win_wheel) */
    char    (*drop)[FS_PATH_LEN];  /* the last files dropped on it */
    int       ndrop;
} awin_t;

#define GRIP 14

static awin_t   g_w[APPWIN_MAX];
static int      g_order[APPWIN_MAX];  /* back to front: window ids */
static int      g_norder;
static int      g_focused;            /* the app windows are the desktop's front window */
static uint32_t g_gen;                  /* the windows' layout: added, moved, resized, titles, focus */
static uint32_t g_drawn_gen;            /* g_gen as the desktop last drew everything */
static int      g_drawn_focused;
static int      g_killed[64];         /* owners whose app must stop (index: owner % 64) */
static int      g_esc;                /* ESC [ sequence state for the keyboard */
static mouse_capture_t g_capture = {.win=-1,.owner=-1};
static uint32_t g_capture_escape;
static int      g_new_window;         /* a window opened since appwin_take_new() */

static int scr_w(void) { const fb_info_t* fi = fb_info(); return fi && fi->width ? (int)fi->width : 800; }
static int scr_h(void) { const fb_info_t* fi = fb_info(); return fi && fi->height ? (int)fi->height : 600; }
static int client_x(const awin_t* w){return w->fullscreen?(scr_w()-w->cw)/2:w->x+BORDER;}
static int client_y(const awin_t* w){return w->fullscreen?(scr_h()-w->ch)/2:w->y+TITLE_H+BORDER;}
static int outer_w(const awin_t* w) { return w->fullscreen ? scr_w() : w->cw + 2 * BORDER; }
static int outer_h(const awin_t* w) { return w->fullscreen ? scr_h() : w->ch + TITLE_H + 2 * BORDER; }

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
    if (ev->type == BANANA_EV_MOUSE_REL && w->qh != w->qt) {
        int last=(w->qh+EVQ-1)%EVQ;
        if(w->q[last].type==BANANA_EV_MOUSE_REL && w->q[last].buttons==ev->buttons) {
            w->q[last].x=mouse_capture_add(w->q[last].x,ev->x);
            w->q[last].y=mouse_capture_add(w->q[last].y,ev->y);return;
        }
    }
    int next = (w->qh + 1) % EVQ;
    if(next==w->qt && ev->type==BANANA_EV_CAPTURE)w->qt=(w->qt+1)%EVQ;
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
    if(g_capture.win>=0 && g_capture.win!=id)appwin_release_mouse();
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
    for (int i = g_norder - 1; i >= 0; i--) if (!g_w[g_order[i]].min && !g_w[g_order[i]].hidden) return &g_w[g_order[i]];
    return NULL;
}

/* Capture belongs to one foreground window, never just to a process. */
int appwin_capture_pending(void) { return g_capture.win>=0; }
void appwin_release_mouse(void) {
    if(g_capture.win<0)return;
    awin_t* w=get(g_capture.win,g_capture.owner);int buttons=g_capture.buttons;
    mouse_capture_reset(&g_capture);
    if(w) {
        for(int bit=0;bit<3;bit++)if(buttons&(1<<bit)) {
            banana_event_t ev={.type=BANANA_EV_MOUSE_UP,.button=bit+1};push(w,&ev);
        }
        w->inside_down=w->last_left=0;
        banana_event_t ev={.type=BANANA_EV_CAPTURE,.x=0};push(w,&ev);
    }
    g_gen++;
}
int appwin_capture_active(void) {
    if(g_capture.win<0)return 0;
    awin_t* w=get(g_capture.win,g_capture.owner);
    if(!w || !g_focused || w!=front() || w->min || w->hidden ||
       keyboard_escape_sequence()!=g_capture_escape) { appwin_release_mouse();return 0; }
    return 1;
}
int appwin_mouse_captured(int id,int owner) {
    if(!get(id,owner))return -1;
    return appwin_capture_active() && g_capture.win==id;
}
int appwin_capture_mouse(int id,int owner,int capture) {
    awin_t* w=get(id,owner);if(!w)return -1;
    if(!capture) {
        if(g_capture.win==id && g_capture.owner==owner)appwin_release_mouse();
        return 0;
    }
    if(!g_focused || w!=front() || w->min || w->hidden || w->dragging || w->resizing || keyboard_held_char(27))return -1;
    appwin_capture_active();
    if(g_capture.win==id && g_capture.owner==owner)return 0;
    if(mouse_capture_begin(&g_capture,id,owner,1))return -1;
    g_capture_escape=keyboard_escape_sequence();g_capture.buttons=w->last_left?1:0;
    w->inside_down=0;
    banana_event_t ev={.type=BANANA_EV_CAPTURE,.x=1,.buttons=g_capture.buttons};push(w,&ev);
    g_gen++;return 0;
}
int appwin_capture_input(const mouse_state_t* m,int width,int height) {
    if(!appwin_capture_active())return 0;
    awin_t* w=get(g_capture.win,g_capture.owner);int dx,dy;
    mouse_capture_motion(&g_capture,m,width,height,&dx,&dy);
    int buttons=(m->btn_left?1:0)|(m->btn_right?2:0)|(m->btn_middle?4:0);
    int changed=buttons^g_capture.buttons;
    for(int bit=0;bit<3;bit++)if(changed&(1<<bit)) {
        g_capture.buttons=(g_capture.buttons&~(1<<bit))|(buttons&(1<<bit));
        banana_event_t ev={.type=(buttons&(1<<bit))?BANANA_EV_MOUSE_DOWN:BANANA_EV_MOUSE_UP,
            .x=w->cw/2,.y=w->ch/2,.button=bit+1,.buttons=g_capture.buttons};push(w,&ev);
    }
    if(dx || dy) { banana_event_t ev={.type=BANANA_EV_MOUSE_REL,.x=dx,.y=dy,.buttons=buttons};push(w,&ev); }
    if(m->dz) { banana_event_t ev={.type=BANANA_EV_WHEEL,.y=m->dz,.buttons=buttons};push(w,&ev); }
    return 1;
}

/* ── app side ─────────────────────────────────────────────────────── */

/* the biggest client area that fits on the screen above the taskbar */
static int max_cw(void) { return scr_w() - 2 * BORDER; }
static int max_ch(void) { return scr_h() - 28 - TITLE_H - 2 * BORDER; }

/* the picture an app draws into is its own memory (it may not write the
 * kernel's); the kernel's windows (Remote Desktop) keep kernel memory */
static uint32_t* px_alloc(int app, int owner, size_t bytes) {
    return app ? (uint32_t*)app_mem_alloc(owner, bytes) : (uint32_t*)kmalloc(bytes);
}

static void px_free(int app, int owner, uint32_t* px) {
    if (app) app_mem_free(owner, px);
    else kfree(px);
}

static int open_win(int owner, const char* title, int w, int h, int app);

int appwin_open(int owner, const char* title, int w, int h) { return open_win(owner, title, w, h, 0); }
int appwin_open_app(int app, const char* title, int w, int h) { return open_win(app, title, w, h, 1); }

static int open_win(int owner, const char* title, int w, int h, int app) {
    if (w < 32) w = 32;
    if (h < 16) h = 16;
    if (w > max_cw()) w = max_cw();
    if (h > max_ch()) h = max_ch();
    for (int i = 0; i < APPWIN_MAX; i++) {
        awin_t* a = &g_w[i];
        if (a->used) continue;
        uint32_t* px = px_alloc(app, owner, (size_t)w * (size_t)h * 4);
        uint32_t* shown = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4);
        if (!px || !shown) { if (px) px_free(app, owner, px); kfree(shown); return -1; }
        memset(a, 0, sizeof(*a));
        a->app_mem = app;
        memset32(px, TC(0xFFFFFF), (size_t)w * (size_t)h);
        memset32(shown, TC(0xFFFFFF), (size_t)w * (size_t)h);
        a->used = 1;
        a->owner = owner;
        a->px = px;
        a->shown = shown;
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
        appwin_release_mouse();
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

/* the frame the app finished becomes the one shown: the desktop never
 * shows a picture the app is still drawing (no flashing while it redraws) */
void appwin_update(int id, int owner) {
    awin_t* w = get(id, owner);
    if (!w) return;
    if (w->shown) memcpy(w->shown, w->px, (size_t)w->cw * (size_t)w->ch * 4);
    w->gen++;                       /* (its pixels only: g_gen is the layout - appwin_fast_frame) */
}

/* the pending size takes effect: a new pixel buffer, the old picture
 * copied into its top-left corner (the app redraws on BANANA_EV_RESIZE) */
static void apply_resize(awin_t* w) {
    if (!w->pending) return;
    w->pending = 0;
    if (w->pw == w->cw && w->ph == w->ch) return;
    uint32_t* np = px_alloc(w->app_mem, w->owner, (size_t)w->pw * (size_t)w->ph * 4);
    uint32_t* ns = (uint32_t*)kmalloc((uint32_t)w->pw * (uint32_t)w->ph * 4);
    if (!np || !ns) { if (np) px_free(w->app_mem, w->owner, np); kfree(ns); return; }   /* no memory: stays as it is */
    memset32(np, TC(0xFFFFFF), (size_t)w->pw * (size_t)w->ph);
    int cw = w->cw < w->pw ? w->cw : w->pw, ch = w->ch < w->ph ? w->ch : w->ph;
    for (int y = 0; y < ch; y++) memcpy(np + (size_t)y * (size_t)w->pw, w->shown + (size_t)y * (size_t)w->cw, (size_t)cw * 4);
    memcpy(ns, np, (size_t)w->pw * (size_t)w->ph * 4);
    px_free(w->app_mem, w->owner, w->px);
    kfree(w->shown);
    w->px = np;
    w->shown = ns;
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
    int limit_w=w->fullscreen?scr_w():max_cw(),limit_h=w->fullscreen?scr_h():max_ch();
    if (cw > limit_w) cw = limit_w;
    if (ch > limit_h) ch = limit_h;
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
    if(g_capture.win==id)appwin_release_mouse();
    if (w->px) px_free(w->app_mem, w->owner, w->px);
    kfree(w->shown);
    w->px = w->shown = NULL;
    if (w->drop) { kfree(w->drop); w->drop = NULL; }
    w->ndrop = 0;
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
        if (!w->min && !w->hidden && mx >= w->x && mx < w->x + outer_w(w) && my >= w->y && my < w->y + outer_h(w)) return g_order[i];
    }
    return -1;
}

int appwin_contains(int mx, int my) { return win_at(mx, my) >= 0; }

#define DROP_MAX 64

int appwin_drop(int mx, int my, const char (*paths)[FS_PATH_LEN], int n) {
    int id = win_at(mx, my);
    if (id < 0 || n <= 0) return 0;
    awin_t* w = &g_w[id];
    if (n > DROP_MAX) n = DROP_MAX;
    char (*d)[FS_PATH_LEN] = (char (*)[FS_PATH_LEN])kmalloc((uint32_t)n * FS_PATH_LEN);
    if (!d) return 0;
    memcpy(d, paths, (uint32_t)n * FS_PATH_LEN);
    if (w->drop) kfree(w->drop);
    w->drop = d;
    w->ndrop = n;
    raise(id);
    int cx = mx - client_x(w), cy = my - client_y(w);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= w->cw) cx = w->cw - 1;
    if (cy >= w->ch) cy = w->ch - 1;
    push_simple(w, BANANA_EV_DROP, cx, cy, 0, n);
    return 1;
}

int appwin_dropped(int id, int owner, int index, char* path, int cap) {
    awin_t* w = get(id, owner);
    if (!w || !path || cap <= 0 || index < 0 || index >= w->ndrop) return -1;
    kstrlcpy(path, w->drop[index], (size_t)cap);
    return 0;
}

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
        if (xe > xs) memcpy(dst + (size_t)ty * (size_t)stride + x0 + xs, w->shown + (size_t)y * (size_t)w->cw + xs,
                            (size_t)(xe - xs) * 4);
    }
}

/* A game's frame on its own: when only the front window's pixels changed
 * since the desktop last drew, they go into the desktop's picture where its
 * client area is - nothing else is drawn again (kernel/gui.c then draws what
 * is over it and shows that rectangle). 1 and the rectangle, or 0: the
 * whole desktop has to be drawn. Before, every frame of a game redrew the
 * wallpaper, the icons, every window and the taskbar, and compared the
 * whole screen with the last one: the desktop took ~30% of the CPU. */
int appwin_fast_frame(int* x, int* y, int* w, int* h) {
    awin_t* f = front();
    if (!f || g_gen != g_drawn_gen || g_focused != g_drawn_focused || f->gen == f->drawn_gen || f->resizing || f->pending || f->dragging) return 0;
    for (int i = 0; i < APPWIN_MAX; i++)
        if (g_w[i].used && &g_w[i] != f && g_w[i].gen != g_w[i].drawn_gen) return 0;   /* (another one changed too) */
    blit(f, client_x(f), client_y(f));
    f->drawn_gen = f->gen;
    *x = client_x(f); *y = client_y(f); *w = f->cw; *h = f->ch;
    return 1;
}

void appwin_draw(const fb_info_t* fi) {
    (void)fi;
    g_drawn_gen = g_gen;
    g_drawn_focused = g_focused;
    for (int i = 0; i < APPWIN_MAX; i++) g_w[i].drawn_gen = g_w[i].gen;
    for (int i = 0; i < g_norder; i++) {
        awin_t* w = &g_w[g_order[i]];
        if (w->min || w->hidden) continue;
        if(w->fullscreen){gfx_fill_rect(0,0,scr_w(),scr_h(),0);blit(w,client_x(w),client_y(w));continue;}
        int active = g_focused && w == front();
        int W = outer_w(w), H = outer_h(w);
        bevel(w->x, w->y, W, H, C_PANEL, TC(0x505D72), TC(0x10141C));
        uint32_t tbg = active ? C_TITLE : TC(0x2A3240);
        bevel(w->x + 3, w->y + 3, W - 6, TITLE_H - 2, tbg, TC(0x647692), TC(0x111923));
        char t[64];
        kstrlcpy(t, w->title, sizeof(t));
        int maxc = (W - 50) / 8;
        if (maxc < 1) maxc = 1;
        if ((int)strlen(t) > maxc) t[maxc] = 0;
        gfx_draw_text(w->x + 10, w->y + 8, t, TC(0xFFFFFF), tbg);
        win_draw_button_row(w->x + W, w->y + 5, 14, w->resizable, w->maxed);
        blit(w, w->x + BORDER, w->y + TITLE_H + BORDER);
        if (w->resizable) gfx_draw_grip(w->x + W, w->y + H);
        if (w->resizing || w->pending) {
            /* the size it is about to get */
            int ow = w->pw + 2 * BORDER, oh = w->ph + TITLE_H + 2 * BORDER;
            gfx_fill_rect(w->x, w->y, ow, 2, TC(0xF4D35E));
            gfx_fill_rect(w->x, w->y + oh - 2, ow, 2, TC(0xF4D35E));
            gfx_fill_rect(w->x, w->y, 2, oh, TC(0xF4D35E));
            gfx_fill_rect(w->x + ow - 2, w->y, 2, oh, TC(0xF4D35E));
        }
    }
}

int appwin_fullscreen(int id) {
    return id>=0&&id<APPWIN_MAX&&g_w[id].used&&g_w[id].fullscreen;
}
void appwin_toggle_fullscreen(int id) {
    if(id<0||id>=APPWIN_MAX||!g_w[id].used)return;
    awin_t* w=&g_w[id];
    if(!w->fullscreen){
        w->fx=w->x;w->fy=w->y;w->fw=w->pending?w->pw:w->cw;w->fh=w->pending?w->ph:w->ch;
        w->fullscreen=1;w->x=w->y=0;
        if(w->resizable)request_resize(w,scr_w(),scr_h());
    }else{
        w->fullscreen=0;w->x=w->fx;w->y=w->fy;
        if(w->resizable)request_resize(w,w->fw,w->fh);
    }
    w->dragging=w->resizing=0;g_gen++;
}

static void toggle_maximize(awin_t* w) {
    if (!w->resizable) return;
    if (!w->maxed) {
        if (!w->snapped) { w->sx = w->x; w->sy = w->y; w->sw = w->cw; w->sh = w->ch; }
        w->x = 0; w->y = 0;
        w->maxed = 1;
        w->snapped = 0;
        request_resize(w, max_cw(), max_ch());
    } else {
        w->x = w->sx; w->y = w->sy;
        w->maxed = 0;
        request_resize(w, w->sw, w->sh);
    }
}

/* into a half / quarter (SNAP_HALF) or the whole screen (SNAP_MAX): the
 * outer rectangle r; the app is asked for the client size that fills it */
static void snap_to(awin_t* w, const win_rect_t* r, int where) {
    if (!w->resizable) return;
    if (!w->maxed && !w->snapped) { w->sx = w->x; w->sy = w->y; w->sw = w->cw; w->sh = w->ch; }
    w->x = r->x;
    w->y = r->y;
    w->maxed = where == SNAP_MAX;
    w->snapped = where == SNAP_HALF;
    request_resize(w, r->w - 2 * BORDER, r->h - TITLE_H - 2 * BORDER);
    g_gen++;
}

/* ── the window manager's hold on them (kernel/gui.c) ── */

void appwin_set_hidden(int id, int hidden) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used || g_w[id].hidden == (hidden ? 1 : 0)) return;
    if(hidden && g_capture.win==id)appwin_release_mouse();
    g_w[id].hidden = hidden ? 1 : 0;
    g_gen++;
}

int appwin_get_rect(int id, int* x, int* y, int* w, int* h) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return 0;
    awin_t* a = &g_w[id];
    *x = a->x; *y = a->y;
    *w = a->fullscreen ? scr_w() : a->pending ? a->pw + 2 * BORDER : outer_w(a);
    *h = a->fullscreen ? scr_h() : a->pending ? a->ph + TITLE_H + 2 * BORDER : outer_h(a);
    return 1;
}

int appwin_state(int id, int* maxed, int* snapped, int* resizable) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return 0;
    if (maxed) *maxed = g_w[id].maxed;
    if (snapped) *snapped = g_w[id].snapped;
    if (resizable) *resizable = g_w[id].resizable;
    return 1;
}

void appwin_snap(int id, const win_rect_t* r, int where) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    awin_t* w = &g_w[id];
    if(w->fullscreen)appwin_toggle_fullscreen(id);
    if (where == SNAP_NONE) {                         /* back to its own size */
        if (!w->maxed && !w->snapped) return;
        w->x = w->sx; w->y = w->sy;
        w->maxed = w->snapped = 0;
        request_resize(w, w->sw, w->sh);
        g_gen++;
        return;
    }
    snap_to(w, r, where);
}

void appwin_move(int id, int x, int y) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    if(g_w[id].fullscreen)return;
    g_w[id].x = x;
    g_w[id].y = y;
    g_gen++;
}

void appwin_start_drag(int id, int mx, int my) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    awin_t* w = &g_w[id];
    if(w->fullscreen)return;
    raise(id);
    w->dragging = 1;
    w->ddx = mx - w->x;
    w->ddy = my - w->y;
    w->dpx = mx;
    w->dpy = my;
}

/* the window under (mx, my), -1; *title: the point is on its title bar */
int appwin_at(int mx, int my, int* title) {
    int id = win_at(mx, my);
    if (title) *title = id >= 0 && !g_w[id].fullscreen && my < g_w[id].y + TITLE_H + 2;
    return id;
}

void appwin_click(int mx, int my) {
    int id = win_at(mx, my);
    if (id < 0) return;
    awin_t* w = &g_w[id];
    raise(id);
    int W = outer_w(w);
    if (!w->fullscreen && my < w->y + TITLE_H + 2) {
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
        w->dpx = mx;
        w->dpy = my;
        w->ddy = my - w->y;
        return;
    }
    if (!w->fullscreen && w->resizable && mx >= w->x + W - GRIP && my >= w->y + outer_h(w) - GRIP) {
        w->resizing = 1;
        w->maxed = 0;
        w->pw = w->cw;
        w->ph = w->ch;
        w->rdx = w->x + W - mx;
        w->rdy = w->y + outer_h(w) - my;
        return;
    }
    int cx = mx - client_x(w), cy = my - client_y(w);
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
    int cx = mx - client_x(w), cy = my - client_y(w);
    if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch) {
        push_simple(w, BANANA_EV_MOUSE_DOWN, cx, cy, 2, 0);
        push_simple(w, BANANA_EV_MOUSE_UP, cx, cy, 2, 0);
    }
}

void appwin_mouse(int mx, int my, int left) {
    if(appwin_capture_active())return;
    for (int i = 0; i < APPWIN_MAX; i++) {
        awin_t* w = &g_w[i];
        if (!w->used) continue;
        if (w->dragging) {
            win_rect_t r;
            int z = w->resizable ? winframe_snap_zone(mx, my, &r) : SNAP_NONE;
            if (!left) {
                w->dragging = 0;
                winframe_set_preview(NULL);
                if (z) snap_to(w, &r, z);            /* let go over a snap zone: it goes there */
            } else {
                /* snapped or maximized, dragged away: back to its own size */
                if ((w->maxed || w->snapped) && (mx - w->dpx > 6 || w->dpx - mx > 6 || my - w->dpy > 6 || w->dpy - my > 6)) {
                    int ow0 = outer_w(w) > 0 ? outer_w(w) : 1;
                    w->ddx = w->ddx * (w->sw + 2 * BORDER) / ow0;
                    w->maxed = w->snapped = 0;
                    request_resize(w, w->sw, w->sh);
                }
                winframe_set_preview(z ? &r : NULL);
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
        int cx = mx - client_x(w), cy = my - client_y(w);
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
    appwin_release_mouse();
    for (int i = 0; i < APPWIN_MAX; i++)
        if (g_w[i].used) push_simple(&g_w[i], BANANA_EV_CLOSE, 0, 0, 0, 0);
}

void appwin_focus(int focused) {
    if(!focused)appwin_release_mouse();
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
    if (g_w[id].wheel) { push_simple(&g_w[id], BANANA_EV_WHEEL, mx - g_w[id].x, dz, 0, 0); return; }
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
        case 'I': k = BANANA_KEY_PGUP; break;          /* (the keyboard's own: kernel/keyboard.c) */
        case 'G': k = BANANA_KEY_PGDN; break;
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

/* an Esc with nothing after it (appwin_key keeps one waiting for a '['): sent */
void appwin_key_esc(void) {
    awin_t* w = front();
    if (g_esc == 1 && w) push_simple(w, BANANA_EV_KEY, 0, 0, 0, 27);
    g_esc = 0;
}

/* ── taskbar / Task Manager ───────────────────────────────────────── */

int appwin_info(int id, char* title, int cap, int* minimized) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return 0;
    if (title) kstrlcpy(title, g_w[id].title, (size_t)cap);
    if (minimized) *minimized = g_w[id].min;
    return 1;
}

int appwin_is_terminal(int id) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return 0;
    app_info_t apps[APP_MAX];
    int n = app_snapshot(apps, APP_MAX);
    for (int i = 0; i < n && i < APP_MAX; i++)
        if (apps[i].id == g_w[id].owner) return !strcmp(apps[i].name, "terminal");
    return 0;
}

int appwin_front_id(void) {
    awin_t* w = front();
    return w ? (int)(w - g_w) : -1;
}

int appwin_focused_owner(void) {
    awin_t* w = g_focused ? front() : NULL;
    return w ? w->owner : -1;
}

void appwin_minimize(int id, int min) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    if(min && g_capture.win==id)appwin_release_mouse();
    g_w[id].min = min ? 1 : 0;
    g_gen++;
}

void appwin_activate(int id) {
    if (id < 0 || id >= APPWIN_MAX || !g_w[id].used) return;
    g_w[id].min = 0;
    raise(id);
}

void appwin_request_close(int id) {
    if(g_capture.win==id)appwin_release_mouse();
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

/* the window that gets the media keys (play / pause, next...) wherever
 * the focus is: the last one that asked (a music player) */
static int g_media_win = -1, g_media_owner = -1;

void appwin_set_wheel(int id, int owner) {
    awin_t* w = get(id, owner);
    if (w) w->wheel = 1;
}

void appwin_set_media(int id, int owner) {
    if (get(id, owner)) { g_media_win = id; g_media_owner = owner; }
}

int appwin_media_key(int k) {
    if (g_media_win < 0 || !get(g_media_win, g_media_owner)) { g_media_win = -1; return 0; }
    int key = k == KEYF_PLAY ? BANANA_KEY_PLAY : k == KEYF_STOP ? BANANA_KEY_STOP :
              k == KEYF_NEXT ? BANANA_KEY_NEXT : k == KEYF_PREV ? BANANA_KEY_PREV : 0;
    if (!key) return 0;
    push_simple(&g_w[g_media_win], BANANA_EV_KEY, 0, 0, 0, key);
    return 1;
}
