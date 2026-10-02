#include "page.h"
#include "kstring.h"
#include "kheap.h"

#define MAX_IMAGES   48
#define MAX_IMG_SIZE (6u << 20)
#define LAYOUT_MEM   (48u << 20)

/* ══ URLs ═════════════════════════════════════════════════════════════ */

static int has_scheme(const char* s) {
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
    if (has_scheme(rel)) { kstrlcpy(out, rel, (size_t)cap); return; }
    char b[1024];
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

static int fetch(page_t* p, const char* url, char** data, uint32_t* len, char* ctype, int ccap) {
    char fin[1024], err[160];
    if (!p->env || !p->env->fetch) return -1;
    int r = p->env->fetch(p->env->ctx, url, data, len, ctype, ccap, fin, sizeof(fin), err, sizeof(err));
    if (r != 0) plog(p, "browser: cannot load %s", url);
    return r;
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

static void collect_sheets(page_t* p, dom_node_t* n) {
    for (dom_node_t* c = n->first; c && p->nsheets < 24; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "style") == 0) {
            if (c->first && c->first->type == DOM_TEXT)
                p->sheets[p->nsheets++] = css_parse(&p->A, c->first->text, c->first->text_len, 1);
            continue;
        }
        if (strcmp(c->tag, "link") == 0) {
            const char* rel = dom_attr(c, "rel");
            const char* href = dom_attr(c, "href");
            if (rel && href && strcasecmp(rel, "stylesheet") == 0) {
                char url[1024], ct[96];
                url_resolve(p->url, href, url, sizeof(url));
                char* data;
                uint32_t len;
                if (fetch(p, url, &data, &len, ct, sizeof(ct)) == 0) {
                    char* copy = arena_strdup(&p->A, data, len);
                    kfree(data);
                    p->sheets[p->nsheets++] = css_parse(&p->A, copy, len, 1);
                }
            }
            continue;
        }
        collect_sheets(p, c);
    }
}

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
    if (!t || !*t) return !dom_attr(s, "nomodule");   /* modules work here: skip their fallbacks */
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
    while (holder->first) {
        dom_node_t* c = holder->first;
        dom_remove(c);
        if (parent) dom_insert_before(parent, c, ref);
    }
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
        char url[1024], ct[96];
        url_resolve(p->url, src_attr, url, sizeof(url));
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
        kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        plog(p, "js: %s", p->status);
    }
    if (fetched) kfree(fetched);                    /* the syntax tree does not point into it */
    flush_writes(p);
    p->cur_script = NULL;
}

static void run_scripts(page_t* p) {
    dom_node_t** list = (dom_node_t**)arena_alloc(&p->A, 128 * (uint32_t)sizeof(dom_node_t*));
    int n = collect_tags(p->doc, "script", list, 128, 0);
    for (int i = 0; i < n; i++) run_script(p, list[i]);
    /* document.write may have added more scripts */
    n = collect_tags(p->doc, "script", list, 128, 0);
    for (int i = 0; i < n; i++) run_script(p, list[i]);
}

/* ══ images ═══════════════════════════════════════════════════════════ */

static void load_images(page_t* p, dom_node_t* n, int* count) {
    for (dom_node_t* c = n->first; c && *count < MAX_IMAGES; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        if (strcmp(c->tag, "img") == 0 && !c->img) {
            const char* src = dom_attr(c, "src");
            img_data_t* img = (img_data_t*)arena_alloc(&p->A, sizeof(img_data_t));
            img->failed = 1;
            c->img = img;
            if (src && *src && p->env && p->env->decode_image) {
                char url[1024], ct[96];
                url_resolve(p->url, src, url, sizeof(url));
                char* data;
                uint32_t len;
                (*count)++;
                if (fetch(p, url, &data, &len, ct, sizeof(ct)) == 0) {
                    if (len <= MAX_IMG_SIZE && p->env->decode_image(p->env->ctx, (const uint8_t*)data, len, img, &p->A) == 0)
                        img->failed = 0;
                    kfree(data);
                }
            }
            continue;
        }
        load_images(p, c, count);
    }
}

/* ══ lifecycle ════════════════════════════════════════════════════════ */

page_t* page_new(page_env_t* env, uint32_t mem_limit) {
    page_t* p = (page_t*)kzalloc(sizeof(page_t));
    if (!p) return NULL;
    arena_init(&p->A, mem_limit);
    arena_init(&p->LA, LAYOUT_MEM);
    p->env = env;
    p->scroll_req = -1;
    p->next_timer = 1;
    return p;
}

void page_free(page_t* p) {
    if (!p) return;
    arena_free_all(&p->LA);
    arena_free_all(&p->A);
    kfree(p);
}

void page_update(page_t* p, int width) {
    if (!p->doc) return;
    if (!p->dirty && p->layout && width == p->width) return;
    init_forms(p->doc);
    init_forms_text(p, p->doc);
    int count = 0;
    load_images(p, p->doc, &count);          /* added by scripts */
    /* styles and the layout are rebuilt often (timers, typing): they live
     * in their own arena, emptied first */
    arena_free_all(&p->LA);
    arena_init(&p->LA, LAYOUT_MEM);
    css_viewport_w = width;                 /* @media (min-width / max-width) */
    css_style_tree(&p->LA, p->doc, p->sheets, p->nsheets);
    p->layout = layout_build(&p->LA, p->doc, width, p->focus);
    p->width = width;
    p->dirty = 0;
}

void page_load(page_t* p, const char* url, const char* html, uint32_t len, int width) {
    kstrlcpy(p->url, url, sizeof(p->url));
    p->width = width;
    p->doc = html_parse(&p->A, html, len);
    init_forms(p->doc);
    init_forms_text(p, p->doc);
    if (!p->js_disabled) {
        p->js = script_new(&p->A, LANG_JS);
        script_set_limits(p->js, 5000000, 120);
        jsdom_install(p);
        run_scripts(p);
    }
    p->sheets[0] = css_default_sheet(&p->A);
    p->nsheets = 1;
    collect_sheets(p, p->doc);
    set_title(p);
    int count = 0;
    load_images(p, p->doc, &count);
    /* onload handlers */
    if (p->js) {
        dom_node_t* body = dom_find_tag(p->doc, "body");
        const char* ol = body ? dom_attr(body, "onload") : NULL;
        if (ol && script_run(p->js, ol, (uint32_t)strlen(ol), "onload") != 0)
            kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        for (int i = 0; i < p->nonload; i++) {
            if (script_call(p->js, p->onload[i], v_undef(), 0, NULL, NULL) != 0)
                kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        }
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
            kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
        p->dirty = 1;
        return;
    }
    url_resolve(p->url, href, p->nav, sizeof(p->nav));
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

static void submit_form(page_t* p, dom_node_t* form, dom_node_t* submitter) {
    if (!form) return;
    if (jsdom_dispatch(p, form, "submit")) return;          /* preventDefault() / return false */
    const int qcap = 16384;
    char* q = (char*)arena_alloc(&p->A, qcap);
    form_fields(form, submitter, q, qcap);
    const char* action = dom_attr(form, "action");
    char base[1024];
    if (action && *action) url_resolve(p->url, action, base, sizeof(base));
    else kstrlcpy(base, p->url, sizeof(base));
    char* qm = strchr(base, '?');
    if (qm) *qm = 0;
    char* hm = strchr(base, '#');
    if (hm) *hm = 0;
    const char* method = dom_attr(form, "method");
    if (method && strcasecmp(method, "post") == 0) {
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

int page_link_at(page_t* p, int x, int y, char* out, int cap) {
    if (!p->layout) return 0;
    for (dom_node_t* e = layout_hit(p->layout, x, y); e && e->type == DOM_ELEM; e = e->parent) {
        if (strcmp(e->tag, "a") == 0 && dom_attr(e, "href")) {
            const char* href = dom_attr(e, "href");
            if (strncasecmp(href, "javascript:", 11) == 0) return 0;
            url_resolve(p->url, href, out, cap);
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
    dom_node_t* n = layout_hit(p->layout, x, y);
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
        if (strcmp(tag, "input") == 0) {
            const char* t = dom_attr(e, "type");
            if (!t) t = "text";
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
        if (!p->js) return 0;
        int prevented = jsdom_dispatch_key(p, dom_body(p->doc) ? dom_body(p->doc) : p->doc, "keydown", kn);
        jsdom_dispatch_key(p, dom_body(p->doc) ? dom_body(p->doc) : p->doc, "keyup", kn);
        return prevented;
    }
    if (p->js && jsdom_dispatch_key(p, f, "keydown", kn)) return 1;
    if (p->focus != f) return 1;           /* the handler moved the focus */
    const char* v = f->value ? f->value : "";
    uint32_t len = (uint32_t)strlen(v);
    if (c == '\n' && strcmp(f->tag, "textarea") != 0) {
        jsdom_dispatch(p, f, "change");
        submit_form(p, ancestor(f, "form"), NULL);
        return 1;
    }
    char* nv;
    if (c == '\b') {
        if (!len) return 1;
        nv = arena_strdup(&p->A, v, len - 1);
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

int page_tick(page_t* p) {
    if (!p->js) return 0;
    uint32_t t = now(p);
    int any = 0;
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
            kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
            tm->active = 0;
        }
        any = 1;
    }
    return any;
}
