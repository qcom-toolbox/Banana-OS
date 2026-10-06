#include "webview.h"
#include "browser.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "../net/http.h"
#include "../web/page.h"
#include "../web/render.h"
#include "../web/script.h"

#define WV_MAX    8
#define WV_CMDS   32
#define WV_MSGS   32
#define WV_HIST   16
#define WV_URL    HTTP_URL_MAX

enum { WC_NONE = 0, WC_LOAD, WC_HTML, WC_CLICK, WC_KEY, WC_GO, WC_EVAL, WC_POST };

typedef struct {
    int    kind, x, y;
    char*  str;                 /* kmalloc'd: url, html, script, message */
    char*  base;                /* WC_HTML: its address */
    /* WC_EVAL: the caller waits on done */
    volatile int* done;
    char*  out;
    int    cap;
    int*   rc;
} wcmd_t;

typedef struct {
    int       used, owner, closing;
    page_t*   page;
    page_env_t env;             /* the browser's, with this view as ctx (messages) */
    int       w, h;             /* the size the app asked for */
    uint32_t* px;               /* the rendered picture, pw x ph */
    int       pw, ph;
    int       scroll, page_h;
    int       loading, render_req;
    int       flags;            /* BANANA_WEB_* waiting for web_poll() */
    int       press, px0, py0;  /* mouse down at (view coordinates) */
    char      title[128];
    char      url[WV_URL];
    char      hist[WV_HIST][WV_URL];
    int       hist_n, hist_pos;
    wcmd_t    cmds[WV_CMDS];
    int       chead, ctail;
    char*     msgs[WV_MSGS];    /* from the page: banana.postMessage() */
    int       mhead, mtail;
} wv_t;

static wv_t* g_wv[WV_MAX];
static int   g_task = -1;

static wv_t* get(int owner, int id) {
    if (id < 0 || id >= WV_MAX || !g_wv[id] || !g_wv[id]->used || g_wv[id]->closing || g_wv[id]->owner != owner) return NULL;
    return g_wv[id];
}

static char* dup_str(const char* s) {
    if (!s) s = "";
    uint32_t n = (uint32_t)strlen(s);
    char* d = (char*)kmalloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static int push(wv_t* v, wcmd_t c) {
    int next = (v->chead + 1) % WV_CMDS;
    if (next == v->ctail) { if (c.str) kfree(c.str); if (c.base) kfree(c.base); return -1; }
    v->cmds[v->chead] = c;
    v->chead = next;
    return 0;
}

/* banana.postMessage(text) in the page */
static void on_message(void* ctx, const char* msg) {
    wv_t* v = (wv_t*)ctx;
    int next = (v->mhead + 1) % WV_MSGS;
    if (next == v->mtail) return;                    /* the app is not reading them: dropped */
    char* m = dup_str(msg);
    if (!m) return;
    v->msgs[v->mhead] = m;
    v->mhead = next;
    v->flags |= BANANA_WEB_MESSAGE;
}

/* ── the task's side ──────────────────────────────────────────────── */

static void ensure_picture(wv_t* v) {
    if (v->px && v->pw == v->w && v->ph == v->h) return;
    uint32_t* np = (uint32_t*)kmalloc((uint32_t)v->w * (uint32_t)v->h * 4);
    if (!np) return;
    memset32(np, 0xFFFFFF, (size_t)v->w * (size_t)v->h);
    uint32_t* old = v->px;
    v->px = np;
    v->pw = v->w;
    v->ph = v->h;
    if (old) kfree(old);
    v->render_req = 1;
}

static void render(wv_t* v) {
    ensure_picture(v);
    if (!v->px) return;
    page_t* p = v->page;
    layout_t* L = NULL;
    if (p && !v->loading) {
        p->view_h = v->ph;
        page_update(p, v->pw);
        L = p->layout;
    }
    v->page_h = L ? L->height : 0;
    int max = v->page_h - v->ph;
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
    if (L) L->sel_on = 0;
    render_page(L, v->px, v->pw, v->pw, v->ph, 0, 0, v->pw, v->ph, v->scroll);
    v->render_req = 0;
    v->flags |= BANANA_WEB_DIRTY;
}

static void load_doc(wv_t* v, const char* url, const char* html, const char* base, const char* post, uint32_t post_len,
                     int add_history) {
    char* data = NULL;
    uint32_t len = 0;
    char final_url[WV_URL];
    v->loading = 1;
    v->flags |= BANANA_WEB_LOADING;
    if (html) {
        len = (uint32_t)strlen(html);
        data = (char*)kmalloc(len + 1);
        if (data) memcpy(data, html, len + 1);
        kstrlcpy(final_url, base && *base ? base : "about:blank", sizeof(final_url));
    } else {
        browser_fetch_document(url, post, post_len, &data, &len, final_url, sizeof(final_url));
    }
    if (!data) { v->loading = 0; return; }
    if (v->page) { page_free(v->page); v->page = NULL; }
    page_t* p = page_new(&v->env, browser_page_mem() / 2);
    if (!p) { kfree(data); v->loading = 0; return; }
    p->view_h = v->h;
    page_load(p, final_url, data, len, v->w);
    kfree(data);
    v->page = p;
    v->scroll = p->scroll_req > 0 ? p->scroll_req : 0;
    p->scroll_req = -1;
    kstrlcpy(v->url, final_url, sizeof(v->url));
    kstrlcpy(v->title, p->title[0] ? p->title : final_url, sizeof(v->title));
    if (add_history && !html) {
        if (v->hist_pos < v->hist_n - 1) v->hist_n = v->hist_pos + 1;
        if (v->hist_n == WV_HIST) {
            for (int i = 1; i < WV_HIST; i++) kstrlcpy(v->hist[i - 1], v->hist[i], sizeof(v->hist[0]));
            v->hist_n--;
        }
        kstrlcpy(v->hist[v->hist_n], final_url, sizeof(v->hist[0]));
        v->hist_pos = v->hist_n++;
    }
    p->status[0] = 0;
    v->loading = 0;
    v->flags |= BANANA_WEB_TITLE;
    render(v);
}

/* after scripts, a click or a key: navigation, title, relayout */
static void after_event(wv_t* v) {
    page_t* p = v->page;
    if (!p) return;
    p->alert_pending = 0;                            /* (no dialogs in a view) */
    p->status[0] = 0;
    if (p->title[0] && strcmp(p->title, v->title) != 0) {
        kstrlcpy(v->title, p->title, sizeof(v->title));
        v->flags |= BANANA_WEB_TITLE;
    }
    if (p->nav_pending) {
        p->nav_pending = 0;
        p->nav_newtab = 0;                           /* target=_blank: here too */
        char* nav = dup_str(p->nav);
        if (!nav) return;
        if (p->nav_post) {
            uint32_t n = p->nav_post_len;
            char* body = (char*)kmalloc(n + 1);
            if (body) {
                memcpy(body, p->nav_post, n);
                body[n] = 0;
                load_doc(v, nav, NULL, NULL, body, n, 1);
                kfree(body);
            }
        } else {
            load_doc(v, nav, NULL, NULL, NULL, 0, 1);
        }
        kfree(nav);
        return;
    }
    int changed = p->dirty;
    page_update(p, v->pw ? v->pw : v->w);
    if (p->scroll_req >= 0) { v->scroll = p->scroll_req; p->scroll_req = -1; changed = 1; }
    if (changed) v->render_req = 1;
}

/* a value as text for web_eval: strings as they are, the rest as JSON */
static void value_text(interp_t* I, value_t r, char* out, int cap) {
    if (r.t == V_OBJ || r.t == V_FUNC) {
        value_t json = script_get_global(I, "JSON");
        if (json.t == V_OBJ) {
            value_t st = obj_get(I, json.o, "stringify");
            value_t s;
            if (v_isfunc(st) && script_call(I, st, json, 1, &r, &s) == 0 && s.t == V_STR) { kstrlcpy(out, v_cstr(I, s), (size_t)cap); return; }
        }
    }
    kstrlcpy(out, r.t == V_UNDEF ? "" : v_cstr(I, r), (size_t)cap);
}

static void run_cmd(wv_t* v, wcmd_t* c) {
    page_t* p = v->page;
    switch (c->kind) {
    case WC_LOAD: load_doc(v, c->str, NULL, NULL, NULL, 0, 1); break;
    case WC_HTML: load_doc(v, NULL, c->str, c->base, NULL, 0, 0); break;
    case WC_GO:
        if (c->x == 0) {
            if (v->hist_pos >= 0) { char* u = dup_str(v->hist[v->hist_pos]); if (u) { load_doc(v, u, NULL, NULL, NULL, 0, 0); kfree(u); } }
        } else {
            int np = v->hist_pos + c->x;
            if (np >= 0 && np < v->hist_n) {
                v->hist_pos = np;
                char* u = dup_str(v->hist[np]);
                if (u) { load_doc(v, u, NULL, NULL, NULL, 0, 0); kfree(u); }
            }
        }
        break;
    case WC_CLICK:
        if (p && !v->loading) { page_click(p, c->x, c->y + v->scroll); after_event(v); v->render_req = 1; }
        break;
    case WC_KEY:
        if (p && !v->loading && c->x < 0x100 && page_key(p, (char)c->x)) { after_event(v); break; }
        switch (c->x) {
        case BANANA_KEY_UP:   v->scroll -= 40; break;
        case BANANA_KEY_DOWN: v->scroll += 40; break;
        case BANANA_KEY_PGUP: v->scroll -= v->h - 40; break;
        case BANANA_KEY_PGDN: case ' ': v->scroll += v->h - 40; break;
        case BANANA_KEY_HOME: v->scroll = 0; break;
        case BANANA_KEY_END:  v->scroll = 1 << 28; break;
        default: break;
        }
        v->render_req = 1;
        break;
    case WC_EVAL: {
        int rc = -1;
        if (p && p->js && !v->loading) {
            interp_t* I = p->js;
            value_t ev = script_get_global(I, "eval"), arg = v_str(I, c->str), r;
            if (v_isfunc(ev) && script_call(I, ev, v_undef(), 1, &arg, &r) == 0) {
                if (c->out && c->cap > 0) value_text(I, r, c->out, c->cap);
                rc = 0;
            } else if (c->out && c->cap > 0) {
                kstrlcpy(c->out, script_error(I), (size_t)c->cap);
            }
            after_event(v);
        }
        if (c->rc) *c->rc = rc;
        if (c->done) *c->done = 1;
        break;
    }
    case WC_POST:
        if (p && p->js && !v->loading) {
            /* window.dispatchEvent(new MessageEvent("message", {data})) */
            interp_t* I = p->js;
            value_t me = script_get_global(I, "__banana_deliver");
            value_t arg = v_str(I, c->str), r;
            if (v_isfunc(me)) script_call(I, me, v_undef(), 1, &arg, &r);
            after_event(v);
        }
        break;
    }
}

static void free_view(wv_t* v) {
    while (v->ctail != v->chead) {
        wcmd_t* c = &v->cmds[v->ctail];
        if (c->kind == WC_EVAL) { if (c->rc) *c->rc = -1; if (c->done) *c->done = 1; }
        if (c->str) kfree(c->str);
        if (c->base) kfree(c->base);
        v->ctail = (v->ctail + 1) % WV_CMDS;
    }
    while (v->mtail != v->mhead) { kfree(v->msgs[v->mtail]); v->mtail = (v->mtail + 1) % WV_MSGS; }
    if (v->page) page_free(v->page);
    if (v->px) kfree(v->px);
    kfree(v);
}

static void webview_task(void) {
    for (;;) {
        int any = 0;
        for (int i = 0; i < WV_MAX; i++) {
            wv_t* v = g_wv[i];
            if (!v) continue;
            if (v->closing) { g_wv[i] = NULL; free_view(v); continue; }
            any = 1;
            while (v->ctail != v->chead && !v->closing) {
                wcmd_t c = v->cmds[v->ctail];
                v->ctail = (v->ctail + 1) % WV_CMDS;
                run_cmd(v, &c);
                if (c.str) kfree(c.str);
                if (c.base) kfree(c.base);
            }
            if (v->closing) continue;
            page_t* p = v->page;
            if (p && !v->loading) {
                if (page_tick(p)) p->dirty = 1;
                if (p->dirty || p->nav_pending || p->status[0] || p->alert_pending) after_event(v);
            }
            if (v->render_req || v->pw != v->w || v->ph != v->h) render(v);
        }
        browser_cookies_save();
        task_sleep_ms(any ? 10 : 100);
    }
}

/* ── the apps' side ───────────────────────────────────────────────── */

int webview_open(int owner, int w, int h) {
    if (w < 16 || h < 16 || w > 4096 || h > 4096) return -1;
    int id = -1;
    for (int i = 0; i < WV_MAX; i++) if (!g_wv[i]) { id = i; break; }
    if (id < 0) return -1;
    wv_t* v = (wv_t*)kzalloc(sizeof(wv_t));
    if (!v) return -1;
    v->used = 1;
    v->owner = owner;
    v->w = w;
    v->h = h;
    v->hist_pos = -1;
    v->env = *browser_env();
    v->env.ctx = v;
    v->env.message = on_message;
    kstrlcpy(v->url, "about:blank", sizeof(v->url));
    g_wv[id] = v;
    if (g_task < 0) g_task = task_create_stack("webview", webview_task, 2u << 20);
    v->render_req = 1;
    return id;
}

void webview_close(int owner, int id) {
    wv_t* v = get(owner, id);
    if (v) v->closing = 1;                           /* the task frees it */
}

void webview_close_owner(int owner) {
    for (int i = 0; i < WV_MAX; i++)
        if (g_wv[i] && g_wv[i]->owner == owner) g_wv[i]->closing = 1;
}

int webview_load(int owner, int id, const char* url) {
    wv_t* v = get(owner, id);
    if (!v || !url) return -1;
    v->loading = 1;
    return push(v, (wcmd_t){ .kind = WC_LOAD, .str = dup_str(url) });
}

int webview_load_html(int owner, int id, const char* html, const char* base_url) {
    wv_t* v = get(owner, id);
    if (!v || !html) return -1;
    v->loading = 1;
    return push(v, (wcmd_t){ .kind = WC_HTML, .str = dup_str(html), .base = dup_str(base_url ? base_url : "about:blank") });
}

void webview_resize(int owner, int id, int w, int h) {
    wv_t* v = get(owner, id);
    if (!v || w < 16 || h < 16 || w > 4096 || h > 4096) return;
    v->w = w;
    v->h = h;
    if (v->page) v->page->dirty = 1;
}

int webview_poll(int owner, int id) {
    wv_t* v = get(owner, id);
    if (!v) return 0;
    int f = v->flags & ~BANANA_WEB_LOADING;
    v->flags = 0;
    if (v->loading) f |= BANANA_WEB_LOADING;
    if (v->mtail != v->mhead) f |= BANANA_WEB_MESSAGE;
    return f;
}

/* the picture at (x, y) of a w x h buffer (clipped) */
void webview_draw(int owner, int id, unsigned int* px, int stride, int x, int y, int w, int h) {
    wv_t* v = get(owner, id);
    if (!v || !v->px || !px) return;
    for (int r = 0; r < v->ph; r++) {
        int dy = y + r;
        if (dy < 0 || dy >= h) continue;
        int x0 = x < 0 ? -x : 0, x1 = v->pw;
        if (x + x1 > w) x1 = w - x;
        if (x1 <= x0) continue;
        memcpy(px + (size_t)dy * (size_t)stride + x + x0, v->px + (size_t)r * (size_t)v->pw + x0, (size_t)(x1 - x0) * 4);
    }
}

void webview_event(int owner, int id, const banana_event_t* ev) {
    wv_t* v = get(owner, id);
    if (!v || !ev) return;
    switch (ev->type) {
    case BANANA_EV_MOUSE_DOWN:
        if (ev->button == 1) { v->press = 1; v->px0 = ev->x; v->py0 = ev->y; }
        break;
    case BANANA_EV_MOUSE_UP:
        if (ev->button == 1 && v->press) {
            v->press = 0;
            int dx = ev->x - v->px0, dy = ev->y - v->py0;
            if (dx * dx + dy * dy <= 25) push(v, (wcmd_t){ .kind = WC_CLICK, .x = ev->x, .y = ev->y });
        }
        break;
    case BANANA_EV_KEY:
        push(v, (wcmd_t){ .kind = WC_KEY, .x = ev->key });
        break;
    case BANANA_EV_RESIZE:
        break;                                       /* the app decides the view's size */
    }
}

void webview_scroll(int owner, int id, int dy) {
    wv_t* v = get(owner, id);
    if (!v) return;
    v->scroll += dy;
    v->render_req = 1;
}

void webview_go(int owner, int id, int delta) {
    wv_t* v = get(owner, id);
    if (v) push(v, (wcmd_t){ .kind = WC_GO, .x = delta });
}

int webview_info(int owner, int id, char* title, int tcap, char* url, int ucap) {
    wv_t* v = get(owner, id);
    if (!v) return -1;
    if (title && tcap > 0) kstrlcpy(title, v->title, (size_t)tcap);
    if (url && ucap > 0) kstrlcpy(url, v->url, (size_t)ucap);
    return v->loading;
}

int webview_eval(int owner, int id, const char* js, char* out, int cap) {
    wv_t* v = get(owner, id);
    if (!v || !js) return -1;
    volatile int done = 0;
    int rc = -1;
    if (out && cap > 0) out[0] = 0;
    if (push(v, (wcmd_t){ .kind = WC_EVAL, .str = dup_str(js), .done = &done, .out = out, .cap = cap, .rc = &rc }) != 0) return -1;
    /* the task runs it between pages; the command points at done, out and rc
     * here, so no giving up early (closing the view releases us too) */
    while (!done) task_sleep_ms(2);
    return rc;
}

int webview_message(int owner, int id, char* out, int cap) {
    wv_t* v = get(owner, id);
    if (!v || v->mtail == v->mhead) return -1;
    char* m = v->msgs[v->mtail];
    v->mtail = (v->mtail + 1) % WV_MSGS;
    int n = (int)strlen(m);
    if (out && cap > 0) kstrlcpy(out, m, (size_t)cap);
    kfree(m);
    return n;
}

int webview_post(int owner, int id, const char* msg) {
    wv_t* v = get(owner, id);
    if (!v || !msg) return -1;
    return push(v, (wcmd_t){ .kind = WC_POST, .str = dup_str(msg) });
}
