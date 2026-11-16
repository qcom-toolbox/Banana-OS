/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "page.h"
#include "canvas.h"
#include "kstring.h"
#include "../kernel/utf8.h"
#include "kheap.h"
#include "svg.h"

#ifndef PAGE_JS_STEPS
#define PAGE_JS_STEPS 400000000u     /* per script or event handler */
#endif

#define MAX_IMAGES   48
#define MAX_IMG_SIZE (6u << 20)
#define LAYOUT_MEM   (48u << 20)

/* ══ URLs ═════════════════════════════════════════════════════════════ */

int url_has_scheme(const char* s) {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return 0;
    for (const char* p = s; *p; p++) {
        if (*p == ':') return p > s;
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.'))
            return 0;
    }
    return 0;
}

/* removes "." and ".." segments from the path part of a URL in place */
static void normalize(char* url) {
    char* path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : strchr(url, '/');
    if (!path) return;
    char* q = path;
    while (*q && *q != '?' && *q != '#') q++;
    char tail[1024];
    kstrlcpy(tail, q, sizeof(tail));
    *q = 0;
    char out[1024];
    int n = 0;
    const char* p = path;
    while (*p) {
        /* p at a '/' */
        const char* seg = p + 1;
        const char* e = strchr(seg, '/');
        uint32_t len = e ? (uint32_t)(e - seg) : (uint32_t)strlen(seg);
        if (len == 1 && seg[0] == '.') {
            if (!e) { out[n++] = '/'; }
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
            while (n > 0 && out[n - 1] != '/') n--;
            if (n == 0) out[n++] = '/';
            if (!e && (n == 0 || out[n - 1] != '/')) out[n++] = '/';
        } else {
            if (n == 0 || out[n - 1] != '/') out[n++] = '/';
            if (n + len < sizeof(out) - 2) { memcpy(out + n, seg, len); n += (int)len; }
            if (e && !e[1]) out[n++] = '/';
        }
        if (!e) break;
        p = e;
    }
    if (n == 0) out[n++] = '/';
    out[n] = 0;
    /* collapse a doubled slash the steps above may leave */
    char* d = out;
    char* r = out;
    while (*r) { *d++ = *r; if (*r == '/') while (r[1] == '/') r++; r++; }
    *d = 0;
    kstrlcpy(path, out, 1024 - (size_t)(path - url));
    kstrlcat(url, tail, 1024);
}

void url_resolve(const char* base, const char* rel, char* out, int cap) {
    while (*rel == ' ' || *rel == '\n' || *rel == '\t') rel++;
    if (url_has_scheme(rel)) { kstrlcpy(out, rel, (size_t)cap); return; }
    static char b[PAGE_URL_MAX];         /* never yields: one buffer is enough, off the stack */
    kstrlcpy(b, base, sizeof(b));
    if (rel[0] == '#') {
        char* h = strchr(b, '#');
        if (h) *h = 0;
        ksnprintf(out, (size_t)cap, "%s%s", b, rel);
        return;
    }
    char* h = strchr(b, '#');
    if (h) *h = 0;
    if (rel[0] == '?') {
        char* q = strchr(b, '?');
        if (q) *q = 0;
        ksnprintf(out, (size_t)cap, "%s%s", b, rel);
        return;
    }
    char* q = strchr(b, '?');
    if (q) *q = 0;
    char* sch = strstr(b, "://");
    char* host_end = sch ? strchr(sch + 3, '/') : NULL;
    if (rel[0] == '/' && rel[1] == '/') {
        char s[16];
        uint32_t sl = sch ? (uint32_t)(sch - b) : 4;
        if (sl > 15) sl = 15;
        memcpy(s, sch ? b : "http", sl);
        s[sl] = 0;
        ksnprintf(out, (size_t)cap, "%s:%s", s, rel);
        return;
    }
    if (rel[0] == '/') {
        if (host_end) *host_end = 0;
        ksnprintf(out, (size_t)cap, "%s%s", b, rel);
    } else {
        /* directory of the base */
        char* last = strrchr(b, '/');
        if (sch && (!host_end || last < host_end)) ksnprintf(out, (size_t)cap, "%s/%s", b, rel);
        else {
            if (last) last[1] = 0;
            ksnprintf(out, (size_t)cap, "%s%s", b, rel);
        }
    }
    normalize(out);
}

/* ══ helpers ══════════════════════════════════════════════════════════ */

static void plog(page_t* p, const char* fmt, const char* a) {
    char buf[256];
    ksnprintf(buf, sizeof(buf), fmt, a);
    if (p->env && p->env->log) p->env->log(p->env->ctx, buf);
}

static uint32_t now(page_t* p) { return p->env && p->env->now_ms ? p->env->now_ms() : 0; }

static uint32_t strtoul_hex(const char* h) {
    uint32_t v = 0;
    for (; *h; h++) {
        int c = *h;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return v >> 4;
    }
    return v;
}

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int page_data_url(const char* url, char** data, uint32_t* len, char* ctype, int ccap) {
    if (strncasecmp(url, "data:", 5) != 0) return -2;
    const char* comma = strchr(url + 5, ',');
    if (!comma) return -1;
    /* the type: "image/png;base64" */
    int b64 = 0;
    const char* semi = url + 5;
    int tl = 0;
    while (semi + tl < comma && semi[tl] != ';') tl++;
    if (ctype && ccap > 0) {
        int n = tl < ccap - 1 ? tl : ccap - 1;
        memcpy(ctype, semi, (size_t)n);
        ctype[n] = 0;
        if (!n) kstrlcpy(ctype, "text/plain", (size_t)ccap);
    }
    for (const char* q = semi; q < comma; q++) if (!strncasecmp(q, ";base64", 7)) b64 = 1;
    const char* s = comma + 1;
    uint32_t n = (uint32_t)strlen(s);
    char* out = (char*)kmalloc(n + 1);
    if (!out) return -1;
    uint32_t o = 0;
    if (b64) {
        uint32_t acc = 0;
        int bits = 0;
        for (uint32_t i = 0; i < n; i++) {
            int c = (unsigned char)s[i];
            if (c == '%' && i + 2 < n) {                 /* (an escaped character inside base64) */
                char h[3] = { s[i + 1], s[i + 2], 0 };
                c = (int)strtoul_hex(h);
                i += 2;
            }
            int v = b64v(c);
            if (v < 0) continue;                         /* '=', spaces */
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out[o++] = (char)(acc >> bits & 0xFF); }
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (s[i] == '%' && i + 2 < n) {
                char h[3] = { s[i + 1], s[i + 2], 0 };
                out[o++] = (char)strtoul_hex(h);
                i += 2;
            } else out[o++] = s[i];
        }
    }
    out[o] = 0;
    *data = out;
    *len = o;
    return 0;
}

static int fetch_final(page_t* p,const char* url,char** data,uint32_t* len,
                       char* ctype,int ccap,char* final,int fcap,char* err,int ecap) {
    if(fcap)final[0]=0;if(ecap)err[0]=0;
    int d=page_data_url(url,data,len,ctype,ccap);
    if(d!=-2){if(!d)kstrlcpy(final,url,fcap);return d;}
    if(!p->env||!p->env->fetch)return -1;
    int r=p->env->fetch(p->env->ctx,url,data,len,ctype,ccap,final,fcap,err,ecap);
    if(r)plog(p,"browser: cannot load %s",url);
    return r;
}
static int fetch(page_t* p,const char* url,char** data,uint32_t* len,char* ctype,int ccap) {
    char final[1024],err[160];
    return fetch_final(p,url,data,len,ctype,ccap,final,sizeof(final),err,sizeof(err));
}

static void init_forms(dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (!c->form_init) {
            if (strcmp(c->tag, "input") == 0) {
                const char* v = dom_attr(c, "value");
                c->value = (char*)(v ? v : "");
                c->checked = dom_attr(c, "checked") != NULL;
                c->form_init = 1;
            } else if (strcmp(c->tag, "textarea") == 0) {
                c->value = NULL;            /* filled lazily below */
                c->form_init = 2;
            } else if (strcmp(c->tag, "select") == 0) {
                c->form_init = 3;
            }
        }
        init_forms(c);
    }
}

/* textarea/select need the arena for their initial text */
static void init_forms_text(page_t* p, dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (c->form_init == 2) { c->value = dom_text(&p->A, c); c->form_init = 1; }
        else if (c->form_init == 3) {
            const char* v = NULL;
            for (dom_node_t* o = c->first; o; o = o->next)
                if (o->type == DOM_ELEM && strcmp(o->tag, "option") == 0 && (!v || dom_attr(o, "selected"))) {
                    const char* ov = dom_attr(o, "value");
                    v = ov ? ov : dom_text(&p->A, o);
                }
            c->value = (char*)(v ? v : "");
            c->form_init = 1;
        }
        init_forms_text(p, c);
    }
}

#include "page_styles.h"

static int collect_tags(dom_node_t* n, const char* tag, dom_node_t** out, int max, int k) {
    for (dom_node_t* c = n->first; c && k < max; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, tag) == 0) out[k++] = c;
        k = collect_tags(c, tag, out, max, k);
    }
    return k;
}

static void set_title(page_t* p) {
    dom_node_t* t = dom_find_tag(p->doc, "title");
    p->title[0] = 0;
    if (!t) return;
    char* s = dom_text(&p->A, t);
    char* a = (char*)arena_alloc(&p->A, (uint32_t)strlen(s) * 3 + 1);
    uint32_t n = text_to_ascii(s, (uint32_t)strlen(s), a);
    /* collapse whitespace */
    uint32_t o = 0;
    int sp = 0;
    for (uint32_t i = 0; i < n && o < sizeof(p->title) - 1; i++) {
        char c = a[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { sp = o > 0; continue; }
        if (sp && o < sizeof(p->title) - 2) p->title[o++] = ' ';
        sp = 0;
        p->title[o++] = c;
    }
    p->title[o] = 0;
}

/* ══ scripts ══════════════════════════════════════════════════════════ */

static int is_js(dom_node_t* s) {
    const char* t = dom_attr(s, "type");
#ifdef BANANA_LIBJS_WORKER
    /* The engine understands modules, but this DOM adapter has no module
     * loader yet. Honor classic nomodule fallbacks instead of claiming it. */
    if(t&&!strcasecmp(t,"module"))return 0;
    if(!t||!*t)return 1;
#else
    if (!t || !*t) return !dom_attr(s, "nomodule");
#endif
    return strcasecmp(t, "text/javascript") == 0 || strcasecmp(t, "application/javascript") == 0 ||
           strcasecmp(t, "module") == 0 || strcasecmp(t, "text/ecmascript") == 0;
}

static void flush_writes(page_t* p) {
    if (!p->write_len || !p->cur_script) { p->write_len = 0; return; }
    dom_node_t* holder = dom_new_element(&p->A, "div");
    html_parse_into(&p->A, holder, p->write_buf, p->write_len);
    dom_node_t* after = p->cur_script;
    dom_node_t* parent = after->parent;
    dom_node_t* ref = after->next;
    int count = 0;
    for (dom_node_t* c = holder->first; c; c = c->next) count++;
    dom_node_t** added = count ? arena_alloc(&p->A, count * sizeof(*added)) : NULL;
    int i = 0;
    while (holder->first) {
        dom_node_t* c = holder->first;
        if (added) added[i++] = c;
        dom_remove(c);
        if (parent) dom_insert_before(parent, c, ref);
    }
    if (parent && i) jsdom_mutation_children(p, parent, added, i, NULL, 0, after, ref);
    p->write_len = 0;
    p->dirty = 1;
}

static void run_script(page_t* p, dom_node_t* s) {
    if (s->script_done) return;
    const char* ty = dom_attr(s, "type");
    if (ty && strcasecmp(ty, "importmap") == 0) {
        s->script_done = 1;
        if (s->first && s->first->type == DOM_TEXT) jsdom_importmap(p, s->first->text, s->first->text_len);
        return;
    }
    if (!is_js(s)) return;
    s->script_done = 1;
    int module = ty && strcasecmp(ty, "module") == 0;
    const char* src_attr = dom_attr(s, "src");
    if (p->A.oom) return;                           /* out of page memory: no more scripts */
    const char* code;
    uint32_t len;
    char* fetched = NULL;
    const char* name;
    if (src_attr) {
        char url[PAGE_URL_MAX], ct[96];
        url_resolve(p->base, src_attr, url, sizeof(url));
        if (module) {                               /* fetched (once) and run by jsdom */
            p->cur_script = s;
            jsdom_module(p, url, NULL, 0);
            flush_writes(p);
            p->cur_script = NULL;
            return;
        }
        if (fetch(p, url, &fetched, &len, ct, sizeof(ct)) != 0) return;
        code = fetched;                             /* parsed straight from the download */
        const char* slash = strrchr(url, '/');
        const char* nm = slash ? slash + 1 : url;
        name = arena_strdup(&p->A, nm, (uint32_t)(strlen(nm) > 80 ? 80 : strlen(nm)));
    } else {
        if (!s->first || s->first->type != DOM_TEXT) return;
        code = s->first->text;
        len = s->first->text_len;
        name = "inline script";
    }
    p->cur_script = s;
    if (module) jsdom_module(p, p->url, code, len);
    else if (script_run(p->js, code, len, name) != 0) {
        if(!p->status[0])kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        plog(p, "js: %s", p->status);
    }
    if (fetched) kfree(fetched);                    /* the syntax tree does not point into it */
    flush_writes(p);
    script_checkpoint(p->js);                     /* document.write mutation records */
    p->cur_script = NULL;
}

static void run_scripts(page_t* p) {
    dom_node_t** list = (dom_node_t**)arena_alloc(&p->A, 128 * (uint32_t)sizeof(dom_node_t*));
    int n = collect_tags(p->doc, "script", list, 128, 0);
    for (int i = 0; i < n; i++) {
        dom_node_t* s=list[i];const char* type=dom_attr(s,"type");
        int deferred=(dom_attr(s,"src") && dom_attr(s,"defer") && !dom_attr(s,"async")) ||
                     (type && !strcasecmp(type,"module"));
        if(!deferred)run_script(p,s);
    }
    /* document.write may have added more scripts */
    n = collect_tags(p->doc, "script", list, 128, 0);
    for (int i = 0; i < n; i++) {
        dom_node_t* s=list[i];const char* type=dom_attr(s,"type");
        int deferred=(dom_attr(s,"src") && dom_attr(s,"defer") && !dom_attr(s,"async")) ||
                     (type && !strcasecmp(type,"module"));
        if(!deferred)run_script(p,s);
    }
    /* External defer scripts and modules run in document order after the
     * parser's ordinary scripts, before DOMContentLoaded. Inline defer
     * has no effect. Resources are fetched synchronously by this engine. */
    for (int i = 0; i < n; i++) run_script(p,list[i]);
}

/* ══ images ═══════════════════════════════════════════════════════════ */

static void load_images(page_t* p, dom_node_t* n, int* count) {
    for (dom_node_t* c = n->first; c && *count < MAX_IMAGES; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "svg") == 0) continue;   /* inline SVG: load_inline_svgs, once styled */
        if (strcmp(c->tag, "img") == 0 && !c->img) {
            const char* src = dom_attr(c, "src");
            img_data_t* img = (img_data_t*)arena_alloc(&p->A, sizeof(img_data_t));
            img->failed = 1;
            c->img = img;
            if (src && *src && p->env) {                 /* SVG needs no decoder; other formats do */
                char url[PAGE_URL_MAX], ct[96];
                url_resolve(p->base, src, url, sizeof(url));
                char* data;
                uint32_t len;
                (*count)++;
                if (fetch(p, url, &data, &len, ct, sizeof(ct)) == 0) {
                    if (len <= MAX_IMG_SIZE && svg_sniff((const uint8_t*)data, len)) {
                        if (svg_render(data, len, 0, 0, img, &p->A) == 0) img->failed = 0;
                    } else if (len <= MAX_IMG_SIZE && p->env->decode_image &&
                               p->env->decode_image(p->env->ctx, (const uint8_t*)data, len, img, &p->A) == 0)
                        img->failed = 0;
                    kfree(data);
                }
            }
            continue;
        }
        load_images(p, c, count);
    }
}

/* ── <iframe>: each is a page of its own ──────────────────────────── */

#define FRAME_MEM       (16u << 20)
#define MAX_FRAME_DEPTH 4

/* an iframe's inner (content) width, the box minus its 2px border each side */
static int frame_inner_w(page_t* p, dom_node_t* e) {
    int w, h;
    iframe_dims(e, p->width, &w, &h);
    int iw = w - 4;
    return iw < 16 ? 16 : iw;
}

/* loads the document of every not-yet-loaded <iframe> (once) */
static void load_frames(page_t* p, dom_node_t* n) {
    if (p->frame_depth >= MAX_FRAME_DEPTH) return;
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "iframe") == 0) {
            if (!c->frame && p->nframes < PAGE_MAX_FRAMES && p->env && p->env->fetch) {
                const char* srcdoc = dom_attr(c, "srcdoc");
                const char* src = dom_attr(c, "src");
                char* html = NULL;
                uint32_t len = 0;
                char furl[PAGE_URL_MAX],frame_error[160]={0};
                int frame_failed=0;
                if (srcdoc && *srcdoc) {
                    len = (uint32_t)strlen(srcdoc);
                    html = (char*)kmalloc(len + 1);
                    if (html) { memcpy(html, srcdoc, len); html[len] = 0; len = html_decode_entities(html, len); }
                    kstrlcpy(furl, p->base, sizeof(furl));
                } else if (src && *src && strcmp(src, "about:blank") != 0) {
                    char ct[96],final[PAGE_URL_MAX];
                    url_resolve(p->base,src,furl,sizeof(furl));
                    if(fetch_final(p,furl,&html,&len,ct,sizeof(ct),final,sizeof(final),frame_error,sizeof(frame_error))){
                        html=NULL;frame_failed=1;
                    }else if(final[0])kstrlcpy(furl,final,sizeof(furl));
                } else {
                    kstrlcpy(furl, "about:blank", sizeof(furl));
                }
                page_t* child = page_new(p->env, FRAME_MEM);
                if (child) {
                    child->frame_depth = p->frame_depth + 1;
                    child->js_disabled = p->js_disabled;
                    child->view_h = 150;
                    page_load(child, furl, html ? html : "", html ? len : 0, frame_inner_w(p, c));
                    if(frame_failed)kstrlcpy(child->status,frame_error[0]?frame_error:"Frame resource could not be fetched",sizeof(child->status));
                    p->frames[p->nframes++] = child;
                    c->frame = child;
                    c->frame_layout = child->layout;
                }
                if (html) kfree(html);
            }
            continue;                               /* its own markup is the child document */
        }
        load_frames(p, c);
    }
}

/* keeps each frame laid out at its box width, and the node's layout
 * pointer current (the child re-lays-out on its own) */
static void sync_frames(page_t* p, dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "iframe") == 0) {
            page_t* child = (page_t*)c->frame;
            if (child) {
                page_update(child, frame_inner_w(p, c));   /* no-op when clean + same width */
                c->frame_layout = child->layout;
            }
            continue;
        }
        sync_frames(p, c);
    }
}

/* CSS background-image: once styles are known, each element's image is
 * fetched (once per address: icons repeat) and decoded into the page arena */
static img_data_t* bg_image_for(page_t* p, const char* url) {
    for (int i = 0; i < p->nbg; i++)
        if (strcmp(p->bgcache[i].url, url) == 0) return p->bgcache[i].img;
    if (p->nbg >= 40) return NULL;
    img_data_t* img = (img_data_t*)arena_alloc(&p->A, sizeof(img_data_t));
    img->failed = 1;
    p->bgcache[p->nbg].url = arena_strdup(&p->A, url, (uint32_t)strlen(url));
    p->bgcache[p->nbg].img = img;
    p->nbg++;
    if (strncmp(url, "data:", 5) == 0) return img;          /* inline data: not decoded here */
    char* data;
    uint32_t len;
    char ct[96];
    if (fetch(p, url, &data, &len, ct, sizeof(ct)) == 0) {
        if (len <= MAX_IMG_SIZE && svg_sniff((const uint8_t*)data, len)) {
            if (svg_render(data, len, 0, 0, img, &p->A) == 0) img->failed = 0;
        } else if (len <= MAX_IMG_SIZE && p->env->decode_image &&
                   p->env->decode_image(p->env->ctx, (const uint8_t*)data, len, img, &p->A) == 0) {
            img->failed = 0;
        }
        kfree(data);
    }
    return img;
}

/* inline SVG, drawn as an image of its own markup in its text colour
 * (currentColor): after styling, and again if that colour changes */
#define MAX_INLINE_SVG 512
static void load_inline_svgs(page_t* p, dom_node_t* n, int* count) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM || !c->style || c->style->display == DISP_NONE) continue;
        if (strcmp(c->tag, "svg") == 0) {
            uint32_t col = c->style->color & 0xFFFFFFu;
            if ((c->img && c->svg_color == col) || *count >= MAX_INLINE_SVG) continue;
            (*count)++;
            img_data_t* img = (img_data_t*)arena_alloc(&p->A, sizeof(img_data_t));
            img->failed = 1;
            char* src = dom_html(&p->A, c, 1);
            if (src && svg_render_color(src, (uint32_t)strlen(src), 0, 0, img, &p->A, col) == 0) img->failed = 0;
            c->img = img;
            c->svg_color = col;
            continue;
        }
        load_inline_svgs(p, c, count);
    }
}

static void load_bg_images(page_t* p, dom_node_t* n) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM || !c->style) continue;
        const style_t* st = c->style;
        if (st->bg_url && st->bg_url_len && st->display != DISP_NONE) {
            char rel[512], url[1024];
            uint32_t l = st->bg_url_len < sizeof(rel) - 1 ? st->bg_url_len : sizeof(rel) - 1;
            memcpy(rel, st->bg_url, l);
            rel[l] = 0;
            url_resolve(p->base, rel, url, sizeof(url));
            if (!c->bg_img || !c->bg_img_url || strcmp(c->bg_img_url, url) != 0) {
                c->bg_img = bg_image_for(p, url);
                c->bg_img_url = c->bg_img ? p->bgcache[p->nbg - 1].url : NULL;
                for (int i = 0; i < p->nbg; i++) if (p->bgcache[i].img == c->bg_img) c->bg_img_url = p->bgcache[i].url;
            }
        } else {
            c->bg_img = NULL;
        }
        load_bg_images(p, c);
    }
}

/* ══ lifecycle ════════════════════════════════════════════════════════ */

page_t* page_new(page_env_t* env, uint32_t mem_limit) {
    page_t* p = (page_t*)kzalloc(sizeof(page_t));
    if (!p) return NULL;
    arena_init(&p->A, mem_limit);
    arena_init(&p->LA, LAYOUT_MEM);
    arena_init(&p->SA, LAYOUT_MEM);
    p->env = env;
    p->scroll_req = -1;
    p->next_timer = 1;
    return p;
}

void page_free(page_t* p) {
    if (p && p->env && p->env->media_cmd)            /* its sounds stop with it */
        for (int i = 0; i < p->nmedia; i++) p->env->media_cmd(p->env->ctx, p->media[i], PAGE_MEDIA_CLOSE, 0, 0, 0);
    if (p)                                           /* <video> pictures live in the heap */
        for (int i = 0; i < p->nmedia; i++)
            if (p->media_node[i] && p->media_node[i]->img && p->media_node[i]->img->px) {
                kfree(p->media_node[i]->img->px);
                p->media_node[i]->img->px = NULL;
            }
    if (!p) return;
#ifdef BANANA_USER_WORKER
    if (webgl_free_all) webgl_free_all(p);           /* (weak: tools that link no webgl.c) */
    if (canvas_free_all) canvas_free_all(p);         /* (weak: tools that link no canvas.c) */
#endif
    for (int i = 0; i < p->nframes; i++) page_free(p->frames[i]);   /* <iframe> children */
    script_destroy_backend(p->js);
    arena_free_all(&p->LA);
    arena_free_all(&p->SA);
    arena_free_all(&p->A);
    kfree(p);
}

void page_update(page_t* p, int width) {
    if (!p->doc) return;
    int styles_changed = refresh_sheets(p);
    if (!p->dirty && !styles_changed && p->layout && width == p->width) return;
    init_forms(p->doc);
    init_forms_text(p, p->doc);
    int count = 0;
    load_images(p, p->doc, &count);          /* added by scripts */
    /* styles and the layout are rebuilt often (timers, typing): they live
     * in their own arena, emptied first */
    arena_free_all(&p->LA);
    arena_init(&p->LA, LAYOUT_MEM);
    css_viewport_w = width;                 /* @media (min-width / max-width) */
    css_viewport_h = p->view_h > 0 ? p->view_h : 600;
    css_anim_now = now(p);                  /* CSS animations: their values now */
    css_anim_running = 0;
    css_style_tree(&p->LA, p->doc, p->sheets, p->nsheets);
    p->anim_running = css_anim_running;
    if (p->env) load_bg_images(p, p->doc);
    int svgs = 0;
    load_inline_svgs(p, p->doc, &svgs);
    if (p->env) { load_frames(p, p->doc); sync_frames(p, p->doc); }   /* <iframe> children */
    p->layout = layout_build(&p->LA, p->doc, width, p->focus);
    p->width = width;
    p->dirty = 0;
}

void page_load(page_t* p, const char* url, const char* html, uint32_t len, int width) {
    script_destroy_backend(p->js);
    p->js=NULL;
    kstrlcpy(p->url, url, sizeof(p->url));
    kstrlcpy(p->base, url, sizeof(p->base));
    p->width = width;
    p->doc = html_parse(&p->A, html, len);
    {   /* <base href="..."> */
        dom_node_t* b = dom_find_tag(p->doc, "base");
        const char* h = b ? dom_attr(b, "href") : NULL;
        if (h && *h) { char b2[PAGE_URL_MAX]; url_resolve(p->url, h, b2, sizeof(b2)); kstrlcpy(p->base, b2, sizeof(p->base)); }
    }
    init_forms(p->doc);
    init_forms_text(p, p->doc);
    p->ready = 0;
    if (!p->js_disabled) {
        p->js = script_new(&p->A, LANG_JS);
        /* long enough for big apps and challenge scripts; the env's yield
         * keeps the desktop responsive meanwhile */
        script_set_limits(p->js, PAGE_JS_STEPS, 120);
        if (p->env && p->env->yield) script_set_yield(p->js, p->env->yield);
        jsdom_install(p);
#ifdef BANANA_LIBJS_WORKER
        extern int banana_libjs_install_dom(page_t*);
        if(banana_libjs_install_dom(p)<0){
            if(!p->status[0])kstrlcpy(p->status,"LibJS DOM initialization failed",sizeof(p->status));
            return;
        }
#endif
        run_scripts(p);
    }
    p->style_snapshot_valid = 0;
    refresh_sheets(p);
    set_title(p);
    int count = 0;
    load_images(p, p->doc, &count);
    /* onload handlers */
    if (p->js) {
        dom_node_t* body = dom_find_tag(p->doc, "body");
        const char* ol = body ? dom_attr(body, "onload") : NULL;
        jsdom_loaded(p, 0);                      /* DOMContentLoaded */
        if (ol && script_run(p->js, ol, (uint32_t)strlen(ol), "onload") != 0)
            if(!p->status[0])kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        jsdom_loaded(p, 1);                      /* load */
        init_forms(p->doc);
        init_forms_text(p, p->doc);
        /* images added by scripts */
        load_images(p, p->doc, &count);
    }
    p->dirty = 1;
    page_update(p, width);
    /* #fragment */
    const char* hash = strchr(p->url, '#');
    if (hash && hash[1]) p->scroll_req = page_anchor_y(p, hash + 1);
}

int page_anchor_y(page_t* p, const char* name) {
    if (!p->doc) return -1;
    dom_node_t* e = dom_find_id(p->doc, name);
    if (!e) {
        dom_node_t* list[256];
        int n = collect_tags(p->doc, "a", list, 256, 0);
        for (int i = 0; i < n; i++) {
            const char* nm = dom_attr(list[i], "name");
            if (nm && strcmp(nm, name) == 0) { e = list[i]; break; }
        }
    }
    return e ? e->box_y : -1;
}

/* ══ interaction ══════════════════════════════════════════════════════ */

static dom_node_t* ancestor(dom_node_t* n, const char* tag) {
    for (; n && n->type == DOM_ELEM; n = n->parent)
        if (strcmp(n->tag, tag) == 0) return n;
    return NULL;
}

static void navigate(page_t* p, const char* href) {
    if (strncasecmp(href, "javascript:", 11) == 0) {
        if (p->js && script_run(p->js, href + 11, (uint32_t)strlen(href + 11), "link") != 0)
            if(!p->status[0])kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        p->dirty = 1;
        return;
    }
    url_resolve(p->base, href, p->nav, sizeof(p->nav));
    p->nav_post = NULL;
    p->nav_newtab = 0;
    p->nav_pending = 1;
}

static void url_append_enc(char* out, int cap, const char* s) {
    static const char hx[] = "0123456789ABCDEF";
    int n = (int)strlen(out);
    for (; *s && n < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else { out[n++] = '%'; out[n++] = hx[c >> 4]; out[n++] = hx[c & 15]; }
    }
    out[n] = 0;
}

static void add_field(char* q, int cap, const char* name, const char* value) {
    if (q[0]) kstrlcat(q, "&", (size_t)cap);
    url_append_enc(q, cap, name);
    kstrlcat(q, "=", (size_t)cap);
    url_append_enc(q, cap, value ? value : "");
}

/* ── uploads ── */

const char* page_mime_of(const char* name) {
    static const char* const M[][2] = {
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "svg", "image/svg+xml" }, { "ico", "image/x-icon" },
        { "mp4", "video/mp4" }, { "webm", "video/webm" }, { "mkv", "video/x-matroska" }, { "mov", "video/quicktime" },
        { "avi", "video/x-msvideo" }, { "mp3", "audio/mpeg" }, { "wav", "audio/wav" }, { "ogg", "audio/ogg" },
        { "flac", "audio/flac" }, { "txt", "text/plain" }, { "html", "text/html" }, { "htm", "text/html" },
        { "css", "text/css" }, { "js", "text/javascript" }, { "json", "application/json" }, { "pdf", "application/pdf" },
        { "zip", "application/zip" }, { "gz", "application/gzip" }, { "tar", "application/x-tar" },
        { "c", "text/plain" }, { "h", "text/plain" }, { "md", "text/markdown" }, { "csv", "text/csv" },
        { "ttf", "font/ttf" }, { "bpk", "application/octet-stream" },
    };
    const char* dot = strrchr(name, '.');
    if (dot) for (uint32_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) if (!strcasecmp(dot + 1, M[i][0])) return M[i][1];
    return "application/octet-stream";
}

static void mp_put(mpart_t* m, const void* d, uint32_t n) {
    if (m->oom) return;
    if (m->len + n + 1 > m->cap) {
        uint32_t cap = (m->len + n + 1) * 2;
        char* nb = (char*)krealloc(m->buf, cap);
        if (!nb) { m->oom = 1; return; }
        m->buf = nb;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    m->buf[m->len] = 0;
}
static void mp_str(mpart_t* m, const char* s) { mp_put(m, s, (uint32_t)strlen(s)); }

/* a name in a header: no quotes or line breaks */
static void mp_name(mpart_t* m, const char* s) {
    for (; *s; s++) {
        if (*s == '"') mp_str(m, "%22");
        else if (*s == '\r') mp_str(m, "%0D");
        else if (*s == '\n') mp_str(m, "%0A");
        else mp_put(m, s, 1);
    }
}

void mpart_init(mpart_t* m, uint32_t seed) {
    memset(m, 0, sizeof(*m));
    static const char H[] = "0123456789abcdef";
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    char r[17];
    for (int i = 0; i < 16; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; r[i] = H[x & 15]; }
    r[16] = 0;
    ksnprintf(m->boundary, sizeof(m->boundary), "----BananaFormBoundary%s", r);
}

static void mp_head(mpart_t* m, const char* name) {
    mp_str(m, "--");
    mp_str(m, m->boundary);
    mp_str(m, "\r\nContent-Disposition: form-data; name=\"");
    mp_name(m, name);
    mp_str(m, "\"");
}

void mpart_field(mpart_t* m, const char* name, const char* value, uint32_t vlen) {
    mp_head(m, name);
    mp_str(m, "\r\n\r\n");
    mp_put(m, value, vlen);
    mp_str(m, "\r\n");
}

void mpart_file(mpart_t* m, const char* name, const char* filename, const char* type, const uint8_t* data, uint32_t len) {
    mp_head(m, name);
    mp_str(m, "; filename=\"");
    mp_name(m, filename);
    mp_str(m, "\"\r\nContent-Type: ");
    mp_str(m, type && *type ? type : "application/octet-stream");
    mp_str(m, "\r\n\r\n");
    if (len) mp_put(m, data, len);
    mp_str(m, "\r\n");
}

void mpart_end(mpart_t* m) {
    mp_str(m, "--");
    mp_str(m, m->boundary);
    mp_str(m, "--\r\n");
}

/* a form's fields as multipart/form-data, files read from the disk */
static void form_multipart(page_t* p, dom_node_t* n, dom_node_t* submitter, mpart_t* m) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        const char* name = dom_attr(c, "name");
        if (name && !dom_attr(c, "disabled")) {
            const char* t = dom_attr(c, "type");
            if (!t) t = "text";
            if (strcmp(c->tag, "input") == 0) {
                if (strcasecmp(t, "checkbox") == 0 || strcasecmp(t, "radio") == 0) {
                    if (c->checked) { const char* v = dom_attr(c, "value") ? dom_attr(c, "value") : "on"; mpart_field(m, name, v, (uint32_t)strlen(v)); }
                } else if (strcasecmp(t, "submit") == 0 || strcasecmp(t, "button") == 0 || strcasecmp(t, "image") == 0) {
                    if (c == submitter) { const char* v = dom_attr(c, "value") ? dom_attr(c, "value") : ""; mpart_field(m, name, v, (uint32_t)strlen(v)); }
                } else if (strcasecmp(t, "file") == 0) {
                    /* the chosen file (its path is the node's value) */
                    const char* path = c->form_init && c->value ? c->value : "";
                    char* d = NULL;
                    uint32_t len = 0;
                    if (page_file_chosen(p, path) && p->env && p->env->read_file && p->env->read_file(p->env->ctx, path, &d, &len) == 0) {
                        const char* base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
                        mpart_file(m, name, base, page_mime_of(base), (const uint8_t*)d, len);
                        kfree(d);
                    } else mpart_file(m, name, "", "application/octet-stream", NULL, 0);
                } else if (strcasecmp(t, "reset") != 0) {
                    const char* v = c->form_init ? (c->value ? c->value : "") : (dom_attr(c, "value") ? dom_attr(c, "value") : "");
                    mpart_field(m, name, v, (uint32_t)strlen(v));
                }
            } else if (strcmp(c->tag, "textarea") == 0 || strcmp(c->tag, "select") == 0) {
                const char* v = c->value ? c->value : "";
                mpart_field(m, name, v, (uint32_t)strlen(v));
            } else if (strcmp(c->tag, "button") == 0 && c == submitter) {
                const char* v = dom_attr(c, "value") ? dom_attr(c, "value") : "";
                mpart_field(m, name, v, (uint32_t)strlen(v));
            }
        }
        form_multipart(p, c, submitter, m);
    }
}

int page_file_chosen(page_t* p, const char* path) {
    if (!path || !*path) return 0;
    for (int i = 0; i < p->nchosen; i++) if (!strcmp(p->chosen[i], path)) return 1;
    return 0;
}

void page_set_files(page_t* p, dom_node_t* in, const char* path) {
    if (!in) return;
    if (!path) { jsdom_dispatch(p, in, "cancel"); return; }
    in->value = arena_strdup(&p->A, path, (uint32_t)strlen(path));
    if (!page_file_chosen(p, path)) {
        if (p->nchosen == 16) { memmove(p->chosen, p->chosen + 1, sizeof(p->chosen[0]) * 15); p->nchosen--; }
        p->chosen[p->nchosen++] = in->value;
    }
    in->form_init = 1;
    uint32_t size = 0;
    char* d = NULL;
    uint32_t len = 0;
    if (p->env && p->env->read_file && p->env->read_file(p->env->ctx, path, &d, &len) == 0) { size = len; kfree(d); }
    const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (p->js) jsdom_set_files(p, in, path, name, size, page_mime_of(name));
    p->dirty = 1;
}

static void form_fields(dom_node_t* n, dom_node_t* submitter, char* q, int cap) {
    for (dom_node_t* c = n->first; c; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        const char* name = dom_attr(c, "name");
        if (name && !dom_attr(c, "disabled")) {
            if (strcmp(c->tag, "input") == 0) {
                const char* t = dom_attr(c, "type");
                if (!t) t = "text";
                if (strcasecmp(t, "checkbox") == 0 || strcasecmp(t, "radio") == 0) {
                    if (c->checked) add_field(q, cap, name, dom_attr(c, "value") ? dom_attr(c, "value") : "on");
                } else if (strcasecmp(t, "submit") == 0 || strcasecmp(t, "button") == 0 || strcasecmp(t, "image") == 0) {
                    if (c == submitter) add_field(q, cap, name, dom_attr(c, "value") ? dom_attr(c, "value") : "");
                } else if (strcasecmp(t, "reset") != 0 && strcasecmp(t, "file") != 0) {
                    add_field(q, cap, name, c->value);
                }
            } else if (strcmp(c->tag, "textarea") == 0 || strcmp(c->tag, "select") == 0) {
                add_field(q, cap, name, c->value);
            } else if (strcmp(c->tag, "button") == 0 && c == submitter) {
                add_field(q, cap, name, dom_attr(c, "value") ? dom_attr(c, "value") : "");
            }
        }
        form_fields(c, submitter, q, cap);
    }
}

void page_submit_form(page_t* p, dom_node_t* form, dom_node_t* submitter, int dispatch_event) {
    if (!form) return;
    if (dispatch_event && jsdom_dispatch(p, form, "submit")) return;          /* preventDefault() / return false */
    const int qcap = 16384;
    char* q = (char*)arena_alloc(&p->A, qcap);
    form_fields(form, submitter, q, qcap);
    const char* action = dom_attr(form, "action");
    char base[PAGE_URL_MAX];
    if (action && *action) url_resolve(p->base, action, base, sizeof(base));
    else kstrlcpy(base, p->url, sizeof(base));
    char* qm = strchr(base, '?');
    if (qm) *qm = 0;
    char* hm = strchr(base, '#');
    if (hm) *hm = 0;
    const char* method = dom_attr(form, "method");
    const char* enctype = dom_attr(form, "enctype");
    p->nav_post_type[0] = 0;
    if (method && strcasecmp(method, "post") == 0 && enctype && strncasecmp(enctype, "multipart/form-data", 19) == 0) {
        /* uploads: the files go along */
        mpart_t m;
        mpart_init(&m, p->env && p->env->now_ms ? p->env->now_ms() : 1);
        form_multipart(p, form, submitter, &m);
        mpart_end(&m);
        if (!m.oom && m.buf) {
            char* body = (char*)arena_alloc(&p->A, m.len + 1);
            if (!p->A.oom) {
                memcpy(body, m.buf, m.len + 1);
                kstrlcpy(p->nav, base, sizeof(p->nav));
                p->nav_post = body;
                p->nav_post_len = m.len;
                ksnprintf(p->nav_post_type, sizeof(p->nav_post_type), "multipart/form-data; boundary=%s", m.boundary);
            }
        }
        if (m.buf) kfree(m.buf);
    } else if (method && strcasecmp(method, "post") == 0) {
        kstrlcpy(p->nav, base, sizeof(p->nav));
        p->nav_post = q;
        p->nav_post_len = (uint32_t)strlen(q);
    } else {
        ksnprintf(p->nav, sizeof(p->nav), "%s?%s", base, q);
        p->nav_post = NULL;
    }
    const char* target = dom_attr(form, "target");
    p->nav_newtab = target && strcasecmp(target, "_blank") == 0;
    p->nav_pending = 1;
}

static void submit_form(page_t* p, dom_node_t* form, dom_node_t* submitter) {
    page_submit_form(p,form,submitter,1);
}

int page_link_at(page_t* p, int x, int y, char* out, int cap) {
    if (!p->layout) return 0;
    for (dom_node_t* e = layout_hit(p->layout, x, y); e && e->type == DOM_ELEM; e = e->parent) {
        if (strcmp(e->tag, "a") == 0 && dom_attr(e, "href")) {
            const char* href = dom_attr(e, "href");
            if (strncasecmp(href, "javascript:", 11) == 0) return 0;
            url_resolve(p->base, href, out, cap);
            return 1;
        }
    }
    return 0;
}

static int is_text_field(dom_node_t* e) {
    if (strcmp(e->tag, "textarea") == 0) return 1;
    if (strcmp(e->tag, "input") != 0) return 0;
    const char* t = dom_attr(e, "type");
    if (!t) return 1;
    static const char* const text_types[] = { "text", "password", "search", "email", "url", "number", "tel" };
    for (uint32_t i = 0; i < sizeof(text_types) / sizeof(text_types[0]); i++)
        if (strcasecmp(t, text_types[i]) == 0) return 1;
    return 0;
}

void page_click(page_t* p, int x, int y) {
    if (!p->layout) return;
    /* The topmost visible display item decides both ordinary and frame
     * activation. A clipped or covered iframe must not steal another click. */
    const dl_item_t* it=layout_hit_item(p->layout,x,y);
    if(it&&it->kind==DL_IFRAME){
        dom_node_t* e=it->node;
        page_t* child=e?(page_t*)e->frame:NULL;
        if(child){
            page_click(child,x-(it->x+2),y-(it->y+2)+e->frame_scroll);
            child->nav_pending=0;
            p->dirty=1;
        }
        return;
    }
    page_activate(p,it?it->node:NULL);
}

/* Shared HTML activation behavior for physical and scripted clicks. */
void page_activate(page_t* p, dom_node_t* n) {
    if(n&&dom_attr(n,"disabled"))return;
    dom_node_t* old_focus = p->focus;
    p->focus = NULL;
    if (!n) { if (old_focus) p->dirty = 1; return; }
    /* a checkbox flips before its onclick runs (and back if prevented) */
    dom_node_t* box = NULL;
    int box_was = 0;
    if (strcmp(n->tag, "input") == 0 && dom_attr(n, "type")) {
        const char* t = dom_attr(n, "type");
        if (strcasecmp(t, "checkbox") == 0 || strcasecmp(t, "radio") == 0) {
            box = n;
            box_was = n->checked;
            if (strcasecmp(t, "checkbox") == 0) n->checked = !n->checked;
            else n->checked = 1;
        }
    }
    int prevented = jsdom_dispatch(p, n, "click");
    p->dirty = 1;
    if (prevented) { if (box) box->checked = box_was; return; }
    if (box) {
        const char* t = dom_attr(box, "type");
        if (strcasecmp(t, "radio") == 0) {
            dom_node_t* form = ancestor(box, "form");
            const char* nm = dom_attr(box, "name");
            if (nm) {
                dom_node_t* list[128];
                int cnt = collect_tags(form ? form : p->doc, "input", list, 128, 0);
                for (int i = 0; i < cnt; i++) {
                    const char* on = dom_attr(list[i], "name");
                    if (list[i] != box && on && strcmp(on, nm) == 0) list[i]->checked = 0;
                }
            }
        }
        if (box->checked != box_was) jsdom_dispatch(p, box, "change");
        return;
    }
    for (dom_node_t* e = n; e && e->type == DOM_ELEM; e = e->parent) {
        const char* tag = e->tag;
        if (strcmp(tag, "a") == 0 && dom_attr(e, "href")) {
            const char* href = dom_attr(e, "href");
            if (href[0] == '#') {
                p->scroll_req = page_anchor_y(p, href + 1);
                if (!href[1]) p->scroll_req = 0;
                return;
            }
            navigate(p, href);
            const char* target = dom_attr(e, "target");
            p->nav_newtab = target && strcasecmp(target, "_blank") == 0;
            return;
        }
        if (is_text_field(e)) { p->focus = e; return; }
        if (strcmp(tag, "label") == 0) {
            /* a label stands for its control: <label for=id>, or the one inside it */
            dom_node_t* c = NULL;
            const char* f = dom_attr(e, "for");
            if (f && *f) c = dom_find_id(p->doc, f);
            if (!c) { dom_node_t* l[1]; if (collect_tags(e, "input", l, 1, 0) == 1) c = l[0]; }
            if (c && c != n && strcmp(c->tag, "input") == 0 && dom_attr(c, "type") && strcasecmp(dom_attr(c, "type"), "file") == 0 &&
                !dom_attr(c, "disabled") && !jsdom_dispatch(p, c, "click")) {
                p->file_pick = c;
                p->file_pick_pending = 1;
                return;
            }
            if (c && c != n && is_text_field(c)) { p->focus = c; return; }
        }
        if (strcmp(tag, "input") == 0) {
            const char* t = dom_attr(e, "type");
            if (!t) t = "text";
            if (strcasecmp(t, "file") == 0) {
                if (!dom_attr(e, "disabled")) { p->file_pick = e; p->file_pick_pending = 1; }
                return;
            }
            if (strcasecmp(t, "submit") == 0 || strcasecmp(t, "image") == 0) { submit_form(p, ancestor(e, "form"), e); return; }
            if (strcasecmp(t, "reset") == 0) return;
            return;
        }
        if (strcmp(tag, "button") == 0) {
            const char* t = dom_attr(e, "type");
            if (!t || strcasecmp(t, "submit") == 0) submit_form(p, ancestor(e, "form"), e);
            return;
        }
        if (strcmp(tag, "select") == 0) {
            /* no drop-down: each click selects the next option */
            dom_node_t* opts[64];
            int cnt = collect_tags(e, "option", opts, 64, 0);
            if (!cnt) return;
            int cur = 0;
            for (int i = 0; i < cnt; i++) {
                const char* ov = dom_attr(opts[i], "value");
                const char* txt = ov ? ov : dom_text(&p->A, opts[i]);
                if (e->value && strcmp(txt, e->value) == 0) cur = i;
            }
            int nxt = (cur + 1) % cnt;
            const char* ov = dom_attr(opts[nxt], "value");
            e->value = (char*)(ov ? ov : dom_text(&p->A, opts[nxt]));
            for (int i = 0; i < cnt; i++) dom_remove_attr(opts[i], "selected");
            dom_set_attr(&p->A, opts[nxt], "selected", "");
            jsdom_dispatch(p, e, "change");
            return;
        }
        if (strcmp(tag, "label") == 0) {
            const char* f = dom_attr(e, "for");
            dom_node_t* target = f ? dom_find_id(p->doc, f) : NULL;
            if (target && is_text_field(target)) p->focus = target;
            else if (target && strcmp(target->tag, "input") == 0) {
                const char* t = dom_attr(target, "type");
                if (t && strcasecmp(t, "checkbox") == 0) target->checked = !target->checked;
            }
            return;
        }
    }
}

static void key_name(char c, char* out) {
    if (c == '\n') kstrlcpy(out, "Enter", 16);
    else if (c == '\b') kstrlcpy(out, "Backspace", 16);
    else if (c == 27) kstrlcpy(out, "Escape", 16);
    else if (c == '\t') kstrlcpy(out, "Tab", 16);
    else { out[0] = c; out[1] = 0; }
}

int page_key(page_t* p, char c) {
    dom_node_t* f = p->focus;
    char kn[16];
    key_name(c, kn);
    if (!f) {
        /* no field: games and shortcuts listen on the document */
        if (!p->js || p->live_keys) return 0;     /* (they come from page_key_live) */
        int prevented = jsdom_dispatch_key(p, dom_body(p->doc) ? dom_body(p->doc) : p->doc, "keydown", kn);
        jsdom_dispatch_key(p, dom_body(p->doc) ? dom_body(p->doc) : p->doc, "keyup", kn);
        return prevented;
    }
    if (p->js && jsdom_dispatch_key(p, f, "keydown", kn)) return 1;
    if (p->focus != f) return 1;           /* the handler moved the focus */
    const char* v = f->value ? f->value : "";
    uint32_t len = (uint32_t)strlen(v);
    /* a one-line search box made of a textarea (Bing, Google): their
     * scripts submit it on Enter - so does the browser when they did not */
    int search_area = 0;
    if (c == '\n' && strcmp(f->tag, "textarea") == 0 && ancestor(f, "form")) {
        const char* rows = dom_attr(f, "rows");
        const char* ty = dom_attr(f, "type");
        const char* role = dom_attr(f, "role");
        search_area = (rows && strcmp(rows, "1") == 0) &&
                      ((ty && strcasecmp(ty, "search") == 0) || (role && strcasecmp(role, "combobox") == 0));
    }
    if (c == '\n' && (strcmp(f->tag, "textarea") != 0 || search_area)) {
        jsdom_dispatch(p, f, "change");
        submit_form(p, ancestor(f, "form"), NULL);
        return 1;
    }
    char* nv;
    if (c == '\b') {
        if (!len) return 1;
        nv = arena_strdup(&p->A, v, (uint32_t)u8_prev(v, (int)len));   /* a whole character */
    } else if ((unsigned char)c >= 32 || c == '\n') {
        const char* ml = dom_attr(f, "maxlength");
        if (ml && (uint32_t)(ml[0] - '0') < 10) {
            int max = 0;
            for (const char* q = ml; *q >= '0' && *q <= '9'; q++) max = max * 10 + (*q - '0');
            if ((int)len >= max) return 1;
        }
        nv = (char*)arena_alloc(&p->A, len + 2);
        memcpy(nv, v, len);
        nv[len] = c;
        nv[len + 1] = 0;
    } else {
        return 1;
    }
    f->value = nv;
    f->form_init = 1;
    jsdom_dispatch(p, f, "input");
    jsdom_dispatch_key(p, f, "keyup", kn);
    p->dirty = 1;
    return 1;
}

int page_key_live(page_t* p, int down, const char* key, const char* code, int keycode, int mods) {
    if (!p->js) return 0;
    p->live_keys = 1;
    dom_node_t* t = dom_body(p->doc) ? dom_body(p->doc) : p->doc;
    int prevented = jsdom_dispatch_keyev(p, t, down ? "keydown" : "keyup", key, code, keycode, mods);
    return down && prevented;
}

/* <video>: the newest picture of each playing stream becomes its element's
 * image (pixels kept in the kernel heap, reused while the size stays) */
static int media_pictures(page_t* p) {
    if (!p->env || !p->env->media_frame || !p->nmedia || (int32_t)(now(p) - p->media_tick) < 30) return 0;
    p->media_tick = now(p);
    int any = 0;
    for (int i = 0; i < p->nmedia; i++) {
        dom_node_t* e = p->media_node[i];
        if (!e) continue;
        uint32_t* px = NULL;
        int w = 0, h = 0;
        uint32_t seq = p->media_seq[i];
        if (p->env->media_frame(p->env->ctx, p->media[i], &seq, &px, &w, &h) != 0 || !px) continue;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { kfree(px); continue; }
        img_data_t* im = e->img;
        if (!im || im->w != w || im->h != h || !im->px) {
            if (!im) {
                im = (img_data_t*)arena_alloc(&p->A, sizeof(img_data_t));
                if (!im || p->A.oom) { kfree(px); continue; }
                memset(im, 0, sizeof(*im));
                e->img = im;
            }
            if (im->w != w || im->h != h) p->dirty = 1;   /* its natural size changed: lay out again */
            if (im->px) kfree(im->px);
            im->px = px;
            im->w = w;
            im->h = h;
            im->alpha = NULL;
            im->failed = 0;
        } else {
            memcpy(im->px, px, (size_t)w * (size_t)h * 4);
            kfree(px);
        }
        p->media_seq[i] = seq;
        any = 1;
    }
    return any;
}

int page_tick(page_t* p) {
    int any = media_pictures(p);
    if (p->canvas_dirty) { p->canvas_dirty = 0; any = 1; }   /* a <canvas> was drawn on: show it */
    /* a CSS animation runs: restyle and redraw, about 20 times a second */
    if (p->anim_running && (int32_t)(now(p) - p->anim_frame) >= 50) {
        p->anim_frame = now(p);
        p->dirty = 1;
        any = 1;
    }
    /* run the timers of the <iframe> children too, then keep their layouts current */
    for (int i = 0; i < p->nframes; i++)
        if (page_tick(p->frames[i])) any = 1;
    if (any) sync_frames(p, p->doc);
    if (!p->js) return any;
    if (jsdom_messages(p)) any = 1;
    uint32_t t = now(p);
    for (int i = 0; i < PAGE_TIMERS; i++) {
        page_timer_t* tm = &p->timers[i];
        if (!tm->active || (int32_t)(t - tm->due) < 0) continue;
        if (tm->interval) tm->due = t + tm->interval;
        else tm->active = 0;
        int r;
        if (tm->fn.t == V_FUNC) r = script_call(p->js, tm->fn, v_undef(), 0, NULL, NULL);
        else {
            const char* code = v_cstr(p->js, tm->fn);
            r = script_run(p->js, code, (uint32_t)strlen(code), "timer");
        }
        if (r != 0) {
            if(!p->status[0])kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
            tm->active = 0;
        }
        any = 1;
    }
    return any;
}
