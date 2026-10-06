#ifndef PAGE_H
#define PAGE_H

#include "types.h"
#include "arena.h"
#include "html.h"
#include "css.h"
#include "layout.h"
#include "script.h"

/*
 * A loaded web page: its DOM, style sheets, layout, JavaScript state and
 * timers. Resources come from the host through page_env_t, so the same
 * code runs in the kernel (network, image decoder) and in host tests.
 */

/* <audio>: what a media stream is doing (page_env_t media_status) */
typedef struct {
    int      state;                 /* 0 none, 1 loading, 2 ready, 3 error */
    int      playing, ended;
    uint32_t pos_ms, dur_ms, seq;   /* seq: bumps when a seek lands or it loops */
    char     error[96];
} page_media_status_t;
enum { PAGE_MEDIA_PLAY = 1, PAGE_MEDIA_PAUSE, PAGE_MEDIA_SEEK, PAGE_MEDIA_SET, PAGE_MEDIA_CLOSE };

typedef struct page_env {
    /* GET url into a kmalloc'd buffer (caller kfree()s); 0 = ok */
    int  (*fetch)(void* ctx, const char* url, char** data, uint32_t* len,
                  char* ctype, int ctype_cap, char* final_url, int final_cap, char* err, int err_cap);
    /* optional: a request with a method and body (fetch(), XMLHttpRequest); 0 = ok */
    int  (*request)(void* ctx, const char* url, const char* method, const char* body, uint32_t blen,
                    const char* ctype, char** data, uint32_t* len, char* rtype, int rcap, char* err, int ecap);
    /* decode an image into out (pixels in the arena); 0 = ok */
    int  (*decode_image)(void* ctx, const uint8_t* data, uint32_t len, img_data_t* out, arena_t* A);
    uint32_t (*now_ms)(void);
    void (*yield)(void);             /* optional: let other tasks run during a long script */
    void (*log)(void* ctx, const char* line);
    /* optional: document.cookie - the cookies a script may see for url ("a=1; b=2"), and one it sets */
    void (*cookie_get)(void* ctx, const char* url, char* out, int cap);
    void (*cookie_set)(void* ctx, const char* url, const char* line);
    /* optional: banana.postMessage(text) - the page talking to the app showing it (web views) */
    void (*message)(void* ctx, const char* text);
    /* optional: sound for <audio> / new Audio(): open (owner: the page) -> id; commands; status */
    int  (*media_open)(void* ctx, void* owner, const char* url);
    void (*media_cmd)(void* ctx, int id, int cmd, int a, int b, int c);   /* SET: volume %, muted, loop */
    int  (*media_status)(void* ctx, int id, page_media_status_t* out);
    void* ctx;
} page_env_t;

#define PAGE_TIMERS 32
#define PAGE_URL_MAX 8192           /* challenge pages navigate with kilobytes of token */

typedef struct page_timer {
    int      id;
    uint32_t due, interval;     /* interval 0 = setTimeout */
    value_t  fn;
    int      active;
} page_timer_t;

typedef struct page {
    arena_t      A;
    page_env_t*  env;
    char         url[PAGE_URL_MAX];
    dom_node_t*  doc;
#define PAGE_MAX_SHEETS 64
    css_sheet_t* sheets[PAGE_MAX_SHEETS];
    uint32_t     sheet_hash[PAGE_MAX_SHEETS];   /* address of each linked sheet (0: inline): loaded once */
    int          nsheets;
    layout_t*    layout;
    int          width;
    interp_t*    js;
    char         title[128];
    int          dirty;               /* DOM/styles changed: restyle + relayout */
    dom_node_t*  focus;               /* focused text field */
    page_timer_t timers[PAGE_TIMERS];
    int          next_timer;
    char         status[200];         /* last script error, for the status bar */
    char         alert[256];          /* alert() text waiting to be shown */
    int          alert_pending;
    char         nav[PAGE_URL_MAX];   /* navigation requested (link, location.href, form) */
    int          nav_pending;
    int          nav_newtab;          /* ...in a new tab (target=_blank, window.open) */
    char*        nav_post;            /* ...as a POST with this form body (NULL: GET) */
    uint32_t     nav_post_len;
    int          scroll_req;          /* scroll position requested (#fragment), -1 none */
    char*        write_buf;           /* document.write() collected during a script */
    uint32_t     write_len, write_cap;
    dom_node_t*  cur_script;
    struct page_module* mods;         /* ES modules loaded, by address */
    int          nmods, mods_cap;
    obj_t*       importmap;           /* <script type=importmap>: its "imports" */
    value_t*     onload;              /* window.onload / DOMContentLoaded handlers */
    int          nonload;
    int          js_disabled;
    arena_t      LA;                  /* styles + layout, emptied on each relayout */
    int          dispatch_depth;      /* jsdom: handlers running (nested dispatch) */
    obj_t*       elem_proto;          /* jsdom: element methods */
    obj_t*       ce_reg;              /* custom elements: tag -> class */
    obj_t*       ce_wait;             /* ...whenDefined() promises still waiting: tag -> array */
    obj_t*       style_proto;
    obj_t*       class_proto;
    obj_t*       doc_obj;
    obj_t*       win_obj;
    obj_t*       loc_obj;
    int          view_h;              /* visible height (window.innerHeight) */
    int          media[16];           /* its <audio> streams (closed with the page) */
    int          nmedia;
    /* CSS background images, fetched once per address */
    struct { const char* url; struct img_data* img; } bgcache[40];
    int          nbg;
} page_t;

page_t* page_new(page_env_t* env, uint32_t mem_limit);
void    page_free(page_t* p);

/* parses html as the document at url, loads style sheets, runs scripts,
 * lays it out at width and loads images */
void    page_load(page_t* p, const char* url, const char* html, uint32_t len, int width);
/* re-styles and re-lays out if needed (or if the width changed) */
void    page_update(page_t* p, int width);

/* mouse click at page coordinates: links, buttons, form fields, onclick */
void    page_click(page_t* p, int x, int y);
/* a key for the focused field: printable, '\b', '\n' (submit) */
int     page_key(page_t* p, char c);
/* runs due timers; 1 if anything happened */
int     page_tick(page_t* p);

/* y of the element with id/name (for #fragments), -1 if none */
int     page_anchor_y(page_t* p, const char* name);

/* the link (absolute URL) at page coordinates; 0 if there is none */
int     page_link_at(page_t* p, int x, int y, char* out, int cap);

/* resolves a link against a base URL */
void    url_resolve(const char* base, const char* rel, char* out, int cap);

/* jsdom.c */
void    jsdom_install(page_t* p);
int     jsdom_dispatch(page_t* p, dom_node_t* target, const char* type);   /* 1 = default prevented */
int     jsdom_dispatch_key(page_t* p, dom_node_t* target, const char* type, const char* key);
void    jsdom_run_handlers_from_attrs(page_t* p);
void    jsdom_module(page_t* p, const char* url, const char* code, uint32_t len);   /* code NULL: load url */
void    jsdom_importmap(page_t* p, const char* text, uint32_t len);

#endif
