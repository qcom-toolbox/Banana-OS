#include "page.h"
#include "script_int.h"
#include "kstring.h"

/*
 * The DOM as JavaScript sees it: document, window, location, elements
 * (attributes, innerHTML, style, classList, events), timers and alert().
 * Element wrappers are host objects cached in dom_node_t.js; methods live
 * in shared prototype tables.
 */

#define ARG(i) ((i) < argc ? argv[i] : v_undef())
#define K(s)   (strcmp(key, (s)) == 0)
#define MAX_LIST 1024
#define MAX_ONLOAD 16

static const host_class_t elem_class, style_class, class_class, win_class, loc_class;

static page_t* P(interp_t* I) { return (page_t*)script_host(I); }

static value_t wrap(page_t* p, dom_node_t* n);

static dom_node_t* node_of(value_t v) {
    if (v.t != V_OBJ || v.o->kind != OBJ_HOST || v.o->hc != &elem_class) return NULL;
    return (dom_node_t*)v.o->host;
}

static dom_node_t* self_node(interp_t* I, value_t self) {
    dom_node_t* n = node_of(self);
    if (!n) script_throw(I, "TypeError: not a DOM node");
    return n;
}

static const char* arg_str(interp_t* I, int argc, value_t* argv, int i) {
    return i < argc ? v_cstr(I, argv[i]) : "";
}

static char* dupz(page_t* p, const char* s) { return arena_strdup(&p->A, s, (uint32_t)strlen(s)); }

static void changed(page_t* p) { p->dirty = 1; }

static int is_elem(dom_node_t* n, const char* tag) {
    return n && n->type == DOM_ELEM && strcmp(n->tag, tag) == 0;
}

/* ══ lists ════════════════════════════════════════════════════════════ */

static value_t list_of(page_t* p, dom_node_t** nodes, int n) {
    obj_t* a = obj_new(p->js, OBJ_ARRAY);
    for (int i = 0; i < n; i++) arr_push(p->js, a, wrap(p, nodes[i]));
    return v_obj(a);
}

static int has_classes(dom_node_t* n, const char* classes) {
    char word[64];
    const char* s = classes;
    int any = 0;
    while (*s) {
        while (*s == ' ') s++;
        int l = 0;
        while (s[l] && s[l] != ' ' && l < 63) { word[l] = s[l]; l++; }
        word[l] = 0;
        s += l;
        if (!l) break;
        if (!dom_has_class(n, word)) return 0;
        any = 1;
    }
    return any;
}

/* mode 0: tag ("*" = all), 1: class names */
static int collect(dom_node_t* root, int mode, const char* what, dom_node_t** out, int k) {
    for (dom_node_t* c = root->first; c && k < MAX_LIST; c = c->next) {
        if (c->type != DOM_ELEM) continue;
        int m = mode == 0 ? (what[0] == '*' || strcasecmp(c->tag, what) == 0) : has_classes(c, what);
        if (m) out[k++] = c;
        k = collect(c, mode, what, out, k);
    }
    return k;
}

static value_t find_list(interp_t* I, dom_node_t* root, int mode, const char* what) {
    page_t* p = P(I);
    dom_node_t** buf = (dom_node_t**)arena_alloc(&p->A, MAX_LIST * (uint32_t)sizeof(dom_node_t*));
    return list_of(p, buf, collect(root, mode, what, buf, 0));
}

/* ══ form values ══════════════════════════════════════════════════════ */

static const char* node_value(page_t* p, dom_node_t* n) {
    if (n->form_init) return n->value ? n->value : "";
    if (strcmp(n->tag, "textarea") == 0) return dom_text(&p->A, n);
    if (strcmp(n->tag, "option") == 0) {
        const char* v = dom_attr(n, "value");
        return v ? v : dom_text(&p->A, n);
    }
    const char* v = dom_attr(n, "value");
    return v ? v : "";
}

static void select_sync(page_t* p, dom_node_t* sel, const char* value) {
    dom_node_t* opts[128];
    int cnt = collect(sel, 0, "option", opts, 0);
    for (int i = 0; i < cnt && i < 128; i++) {
        if (strcmp(node_value(p, opts[i]), value) == 0) dom_set_attr(&p->A, opts[i], "selected", "");
        else dom_remove_attr(opts[i], "selected");
    }
}

static void set_value(page_t* p, dom_node_t* n, const char* v) {
    n->value = dupz(p, v);
    if (strcmp(n->tag, "input") == 0 || strcmp(n->tag, "textarea") == 0 || strcmp(n->tag, "select") == 0) {
        if (!n->form_init) n->checked = dom_attr(n, "checked") != NULL;
        n->form_init = 1;
        if (strcmp(n->tag, "select") == 0) select_sync(p, n, v);
    } else {
        dom_set_attr(&p->A, n, "value", v);
    }
    changed(p);
}

/* ══ handlers ═════════════════════════════════════════════════════════ */

static void add_handler(page_t* p, dom_node_t* n, const char* event, value_t fn) {
    dom_handler_t* h = (dom_handler_t*)arena_alloc(&p->A, sizeof(dom_handler_t));
    h->event = dupz(p, event);
    value_t* f = (value_t*)arena_alloc(&p->A, sizeof(value_t));
    *f = fn;
    h->fn = f;
    dom_handler_t** pp = &n->handlers;
    while (*pp) pp = &(*pp)->next;
    *pp = h;
}

static void remove_handler(dom_node_t* n, const char* event, value_t* fn) {
    for (dom_handler_t** pp = &n->handlers; *pp; pp = &(*pp)->next) {
        dom_handler_t* h = *pp;
        if (strcmp(h->event, event) == 0 && (!fn || v_strict_eq(*(value_t*)h->fn, *fn))) {
            *pp = h->next;
            return;
        }
    }
}

/* the on<event> property handler ("@click"); attribute text is compiled on first use */
static value_t* prop_handler(page_t* p, dom_node_t* n, const char* type) {
    char ev[40];
    ksnprintf(ev, sizeof(ev), "@%s", type);
    for (dom_handler_t* h = n->handlers; h; h = h->next)
        if (strcmp(h->event, ev) == 0) return (value_t*)h->fn;
    if (n->type != DOM_ELEM) return NULL;
    char attr[40];
    ksnprintf(attr, sizeof(attr), "on%s", type);
    const char* code = dom_attr(n, attr);
    if (!code || !p->js) return NULL;
    uint32_t cl = (uint32_t)strlen(code);
    char* src = (char*)arena_alloc(&p->A, cl + 64);
    ksnprintf(src, cl + 64, "__jsdom_h = function (event) {\n%s\n};", code);
    value_t fn = v_null();
    if (script_run(p->js, src, (uint32_t)strlen(src), attr) == 0) fn = script_get_global(p->js, "__jsdom_h");
    else kstrlcpy(p->status, script_error(p->js), sizeof(p->status));
    add_handler(p, n, ev, fn);
    return prop_handler(p, n, type);
}

static void set_prop_handler(page_t* p, dom_node_t* n, const char* type, value_t fn) {
    char ev[40];
    ksnprintf(ev, sizeof(ev), "@%s", type);
    remove_handler(n, ev, NULL);
    add_handler(p, n, ev, v_isfunc(fn) ? fn : v_null());
}


static value_t ev_preventDefault(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    if (self.t == V_OBJ) obj_set(I, self.o, "defaultPrevented", v_bool(1));
    return v_undef();
}

static value_t ev_stopPropagation(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    if (self.t == V_OBJ) obj_set(I, self.o, "cancelBubble", v_bool(1));
    return v_undef();
}

static obj_t* event_proto(page_t* p) {
    static const char* const key = "__event_proto";
    value_t v = script_get_global(p->js, key);
    if (v.t == V_OBJ) return v.o;
    obj_t* o = obj_new(p->js, OBJ_PLAIN);
    obj_set(p->js, o, "preventDefault", v_native(p->js, "preventDefault", ev_preventDefault));
    obj_set(p->js, o, "stopPropagation", v_native(p->js, "stopPropagation", ev_stopPropagation));
    obj_set(p->js, o, "stopImmediatePropagation", v_native(p->js, "stopImmediatePropagation", ev_stopPropagation));
    script_def_global(p->js, key, v_obj(o));
    return o;
}

static int dispatch(page_t* p, dom_node_t* target, const char* type, const char* keyname) {
    if (!p->js || !target) return 0;
    interp_t* I = p->js;
    obj_t* ev = obj_new(I, OBJ_PLAIN);
    ev->proto = event_proto(p);
    obj_set(I, ev, "type", v_str(I, type));
    obj_set(I, ev, "target", wrap(p, target));
    obj_set(I, ev, "srcElement", wrap(p, target));
    obj_set(I, ev, "defaultPrevented", v_bool(0));
    obj_set(I, ev, "cancelBubble", v_bool(0));
    obj_set(I, ev, "bubbles", v_bool(1));
    if (keyname) {
        obj_set(I, ev, "key", v_str(I, keyname));
        int code = keyname[1] ? 0 : (unsigned char)keyname[0];
        if (strcmp(keyname, "Enter") == 0) code = 13;
        else if (strcmp(keyname, "Backspace") == 0) code = 8;
        else if (strcmp(keyname, "ArrowLeft") == 0) code = 37;
        else if (strcmp(keyname, "ArrowUp") == 0) code = 38;
        else if (strcmp(keyname, "ArrowRight") == 0) code = 39;
        else if (strcmp(keyname, "ArrowDown") == 0) code = 40;
        else if (strcmp(keyname, "Escape") == 0) code = 27;
        else if (code >= 'a' && code <= 'z') code -= 32;
        obj_set(I, ev, "keyCode", v_num(code));
        obj_set(I, ev, "which", v_num(code));
    }
    value_t evv = v_obj(ev);
    int prevented = 0;
    p->dispatch_depth++;
    for (dom_node_t* n = target; n; n = n->parent) {
        prop_handler(p, n, type);          /* compiles on<type>="" if present */
        obj_set(I, ev, "currentTarget", wrap(p, n));
        char at[40];
        ksnprintf(at, sizeof(at), "@%s", type);
        for (dom_handler_t* h = n->handlers; h; h = h->next) {
            int isprop = strcmp(h->event, at) == 0;
            if (!isprop && strcmp(h->event, type) != 0) continue;
            value_t fn = *(value_t*)h->fn;
            if (!v_isfunc(fn)) continue;
            value_t ret = v_undef();
            if (script_call(I, fn, wrap(p, n), 1, &evv, &ret) != 0) {
                kstrlcpy(p->status, script_error(I), sizeof(p->status));
                if (p->env && p->env->log) p->env->log(p->env->ctx, p->status);
            } else if (isprop && ret.t == V_BOOL && !ret.b) {
                prevented = 1;
            }
        }
        if (v_truthy(I, obj_get(I, ev, "cancelBubble"))) break;
    }
    p->dispatch_depth--;
    if (v_truthy(I, obj_get(I, ev, "defaultPrevented"))) prevented = 1;
    changed(p);
    return prevented;
}

int jsdom_dispatch(page_t* p, dom_node_t* target, const char* type) { return dispatch(p, target, type, NULL); }
int jsdom_dispatch_key(page_t* p, dom_node_t* target, const char* type, const char* key) { return dispatch(p, target, type, key); }

void jsdom_run_handlers_from_attrs(page_t* p) { (void)p; }

/* ══ style ════════════════════════════════════════════════════════════ */

/* backgroundColor -> background-color */
static void kebab(const char* key, char* out, int cap) {
    int n = 0;
    if (strcmp(key, "cssFloat") == 0) key = "float";
    for (const char* s = key; *s && n < cap - 2; s++) {
        if (*s >= 'A' && *s <= 'Z') { out[n++] = '-'; out[n++] = (char)(*s + 32); }
        else out[n++] = *s;
    }
    out[n] = 0;
}

/* finds "prop: value" in a style attribute; returns the value span */
static int style_find(const char* st, const char* prop, int* decl_s, int* decl_e, int* vs, int* ve) {
    int i = 0, plen = (int)strlen(prop);
    int n = (int)strlen(st);
    while (i < n) {
        int s = i;
        while (i < n && st[i] != ';') i++;
        int e = i;
        int a = s;
        while (a < e && (st[a] == ' ' || st[a] == '\n' || st[a] == '\t')) a++;
        int colon = a;
        while (colon < e && st[colon] != ':') colon++;
        int b = colon;
        while (b > a && st[b - 1] == ' ') b--;
        if (colon < e && b - a == plen && strncasecmp(st + a, prop, (size_t)plen) == 0) {
            int v0 = colon + 1, v1 = e;
            while (v0 < v1 && st[v0] == ' ') v0++;
            while (v1 > v0 && (st[v1 - 1] == ' ' || st[v1 - 1] == '\n')) v1--;
            *decl_s = s; *decl_e = e < n ? e + 1 : e; *vs = v0; *ve = v1;
            return 1;
        }
        i++;
    }
    return 0;
}

static value_t style_get(interp_t* I, obj_t* self, const char* key, int* found) {
    dom_node_t* n = (dom_node_t*)self->host;
    const char* st = dom_attr(n, "style");
    if (!st) st = "";
    if (K("cssText")) { *found = 1; return v_str(I, st); }
    if (K("setProperty") || K("getPropertyValue") || K("removeProperty")) return v_undef();
    char prop[64];
    kebab(key, prop, sizeof(prop));
    int ds, de, vs, ve;
    *found = 1;
    if (style_find(st, prop, &ds, &de, &vs, &ve)) return v_strn(I, st + vs, (uint32_t)(ve - vs));
    return v_str(I, "");
}

static void style_put(page_t* p, dom_node_t* n, const char* prop, const char* val) {
    const char* st = dom_attr(n, "style");
    if (!st) st = "";
    uint32_t sl = (uint32_t)strlen(st), vl = (uint32_t)strlen(val), pl = (uint32_t)strlen(prop);
    char* out = (char*)arena_alloc(&p->A, sl + vl + pl + 8);
    int ds, de, vs, ve;
    uint32_t o = 0;
    if (style_find(st, prop, &ds, &de, &vs, &ve)) {
        memcpy(out, st, (size_t)ds);
        o = (uint32_t)ds;
        memcpy(out + o, st + de, sl - (uint32_t)de);
        o += sl - (uint32_t)de;
    } else {
        memcpy(out, st, sl);
        o = sl;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == ';')) o--;
    if (vl) {
        if (o) { out[o++] = ';'; out[o++] = ' '; }
        memcpy(out + o, prop, pl); o += pl;
        out[o++] = ':'; out[o++] = ' ';
        memcpy(out + o, val, vl); o += vl;
    }
    out[o] = 0;
    dom_set_attr(&p->A, n, "style", out);
    changed(p);
}

static int style_set(interp_t* I, obj_t* self, const char* key, value_t v) {
    page_t* p = P(I);
    dom_node_t* n = (dom_node_t*)self->host;
    if (K("cssText")) { dom_set_attr(&p->A, n, "style", v_cstr(I, v)); changed(p); return 1; }
    char prop[64];
    kebab(key, prop, sizeof(prop));
    const char* val = (v.t == V_NULL || v.t == V_UNDEF) ? "" : v_cstr(I, v);
    style_put(p, n, prop, val);
    return 1;
}

static value_t st_setProperty(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ || self.o->hc != &style_class) return v_undef();
    style_put(P(I), (dom_node_t*)self.o->host, arg_str(I, argc, argv, 0), arg_str(I, argc, argv, 1));
    return v_undef();
}

static value_t st_removeProperty(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ || self.o->hc != &style_class) return v_undef();
    style_put(P(I), (dom_node_t*)self.o->host, arg_str(I, argc, argv, 0), "");
    return v_undef();
}

static value_t st_getPropertyValue(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ || self.o->hc != &style_class) return v_str(I, "");
    int f;
    return style_get(I, self.o, arg_str(I, argc, argv, 0), &f);
}

static const host_class_t style_class = { "CSSStyleDeclaration", style_get, style_set };

/* ══ classList ════════════════════════════════════════════════════════ */

static void class_edit(page_t* p, dom_node_t* n, const char* cls, int add) {
    const char* cur = dom_attr(n, "class");
    if (!cur) cur = "";
    uint32_t cl = (uint32_t)strlen(cur), nl = (uint32_t)strlen(cls);
    char* out = (char*)arena_alloc(&p->A, cl + nl + 2);
    uint32_t o = 0;
    const char* s = cur;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        uint32_t l = 0;
        while (s[l] && s[l] != ' ' && s[l] != '\t' && s[l] != '\n') l++;
        if (!l) break;
        if (!(l == nl && memcmp(s, cls, l) == 0)) {
            if (o) out[o++] = ' ';
            memcpy(out + o, s, l);
            o += l;
        }
        s += l;
    }
    if (add && nl) {
        if (o) out[o++] = ' ';
        memcpy(out + o, cls, nl);
        o += nl;
    }
    out[o] = 0;
    dom_set_attr(&p->A, n, "class", out);
    changed(p);
}

static dom_node_t* cl_node(value_t self) {
    return (self.t == V_OBJ && self.o->hc == &class_class) ? (dom_node_t*)self.o->host : NULL;
}

static value_t cl_add(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = cl_node(self);
    if (n) for (int i = 0; i < argc; i++) class_edit(P(I), n, v_cstr(I, argv[i]), 1);
    return v_undef();
}

static value_t cl_remove(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = cl_node(self);
    if (n) for (int i = 0; i < argc; i++) class_edit(P(I), n, v_cstr(I, argv[i]), 0);
    return v_undef();
}

static value_t cl_contains(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = cl_node(self);
    return v_bool(n && dom_has_class(n, arg_str(I, argc, argv, 0)));
}

static value_t cl_toggle(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = cl_node(self);
    if (!n) return v_bool(0);
    const char* c = arg_str(I, argc, argv, 0);
    int on = argc > 1 && argv[1].t != V_UNDEF ? v_truthy(I, argv[1]) : !dom_has_class(n, c);
    class_edit(P(I), n, c, on);
    return v_bool(on);
}

static value_t class_get(interp_t* I, obj_t* self, const char* key, int* found) {
    dom_node_t* n = (dom_node_t*)self->host;
    const char* cur = dom_attr(n, "class");
    if (!cur) cur = "";
    if (K("value")) { *found = 1; return v_str(I, cur); }
    if (K("length")) {
        int cnt = 0, in = 0;
        for (const char* s = cur; *s; s++) {
            int ws = *s == ' ' || *s == '\t' || *s == '\n';
            if (!ws && !in) cnt++;
            in = !ws;
        }
        *found = 1;
        return v_num(cnt);
    }
    return v_undef();
}

static const host_class_t class_class = { "DOMTokenList", class_get, NULL };

/* ══ elements ═════════════════════════════════════════════════════════ */

static value_t wrap(page_t* p, dom_node_t* n) {
    if (!n) return v_null();
    if (n->js) return v_obj((obj_t*)n->js);
    obj_t* o = obj_new(p->js, OBJ_HOST);
    o->hc = &elem_class;
    o->host = n;
    o->proto = n->type == DOM_DOC ? p->doc_obj : p->elem_proto;
    n->js = o;
    return v_obj(o);
}

static void upper_tag(const char* t, char* out, int cap) {
    int i = 0;
    for (; t[i] && i < cap - 1; i++) out[i] = (t[i] >= 'a' && t[i] <= 'z') ? (char)(t[i] - 32) : t[i];
    out[i] = 0;
}

static dom_node_t* first_elem(dom_node_t* n, int forward) {
    for (; n; n = forward ? n->next : n->prev)
        if (n->type == DOM_ELEM) return n;
    return NULL;
}

static value_t attr_or_empty(interp_t* I, dom_node_t* n, const char* name) {
    const char* v = dom_attr(n, name);
    return v_str(I, v ? v : "");
}

static void set_text(page_t* p, dom_node_t* n, const char* s) {
    dom_remove_children(n);
    if (*s) dom_append(n, dom_new_text(&p->A, s, (uint32_t)strlen(s)));
    if (strcmp(n->tag ? n->tag : "", "textarea") == 0) { n->value = dupz(p, s); n->form_init = 1; }
    changed(p);
}

static void set_html(page_t* p, dom_node_t* n, const char* s) {
    dom_remove_children(n);
    html_parse_into(&p->A, n, s, (uint32_t)strlen(s));
    changed(p);
}

/* the document node */
static value_t doc_get(interp_t* I, page_t* p, dom_node_t* d, const char* key, int* found) {
    *found = 1;
    if (K("body")) return wrap(p, dom_body(d));
    if (K("head")) return wrap(p, dom_find_tag(d, "head"));
    if (K("documentElement")) return wrap(p, dom_find_tag(d, "html"));
    if (K("title")) return v_str(I, p->title);
    if (K("URL") || K("documentURI")) return v_str(I, p->url);
    if (K("location")) return v_obj(p->loc_obj);
    if (K("readyState")) return v_str(I, "complete");
    if (K("defaultView")) return v_obj(p->win_obj);
    if (K("nodeType")) return v_num(9);
    if (K("nodeName")) return v_str(I, "#document");
    if (K("forms")) return find_list(I, d, 0, "form");
    if (K("images")) return find_list(I, d, 0, "img");
    if (K("links")) return find_list(I, d, 0, "a");
    if (K("activeElement")) return p->focus ? wrap(p, p->focus) : wrap(p, dom_body(d));
    *found = 0;
    return v_undef();
}

static value_t elem_get(interp_t* I, obj_t* self, const char* key, int* found) {
    page_t* p = P(I);
    dom_node_t* n = (dom_node_t*)self->host;
    if (n->type == DOM_DOC) {
        value_t v = doc_get(I, p, n, key, found);
        if (*found) return v;
    }
    *found = 1;
    /* tree */
    if (K("parentNode")) return n->parent ? wrap(p, n->parent) : v_null();
    if (K("parentElement")) return n->parent && n->parent->type == DOM_ELEM ? wrap(p, n->parent) : v_null();
    if (K("firstChild")) return wrap(p, n->first);
    if (K("lastChild")) return wrap(p, n->last);
    if (K("nextSibling")) return wrap(p, n->next);
    if (K("previousSibling")) return wrap(p, n->prev);
    if (K("firstElementChild")) return wrap(p, first_elem(n->first, 1));
    if (K("lastElementChild")) return wrap(p, first_elem(n->last, 0));
    if (K("nextElementSibling")) return wrap(p, first_elem(n->next, 1));
    if (K("previousElementSibling")) return wrap(p, first_elem(n->prev, 0));
    if (K("children") || K("childNodes")) {
        int elems_only = K("children");
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (dom_node_t* c = n->first; c; c = c->next)
            if (!elems_only || c->type == DOM_ELEM) arr_push(I, a, wrap(p, c));
        return v_obj(a);
    }
    if (K("childElementCount")) {
        int c = 0;
        for (dom_node_t* k = n->first; k; k = k->next) c += k->type == DOM_ELEM;
        return v_num(c);
    }
    if (K("ownerDocument")) return wrap(p, p->doc);
    if (K("nodeType")) return v_num(n->type == DOM_ELEM ? 1 : n->type == DOM_TEXT ? 3 : n->type == DOM_COMMENT ? 8 : 9);
    if (K("isConnected")) {
        dom_node_t* r = n;
        while (r->parent) r = r->parent;
        return v_bool(r == p->doc);
    }
    /* text nodes */
    if (n->type != DOM_ELEM) {
        if (K("textContent") || K("nodeValue") || K("data") || K("wholeText")) return v_strn(I, n->text ? n->text : "", n->text_len);
        if (K("nodeName")) return v_str(I, n->type == DOM_TEXT ? "#text" : "#comment");
        if (K("length")) return v_num(n->text_len);
        *found = 0;
        return v_undef();
    }
    char up[32];
    if (K("tagName") || K("nodeName") || K("localName")) {
        if (K("localName")) return v_str(I, n->tag);
        upper_tag(n->tag, up, sizeof(up));
        return v_str(I, up);
    }
    if (K("id")) return attr_or_empty(I, n, "id");
    if (K("className")) return attr_or_empty(I, n, "class");
    if (K("innerHTML")) return v_str(I, dom_html(&p->A, n, 0));
    if (K("outerHTML")) return v_str(I, dom_html(&p->A, n, 1));
    if (K("textContent") || K("innerText") || K("text")) return v_str(I, dom_text(&p->A, n));
    if (K("value")) return v_str(I, node_value(p, n));
    if (K("checked")) return v_bool(n->form_init ? n->checked : dom_attr(n, "checked") != NULL);
    if (K("disabled") || K("hidden") || K("selected") || K("readOnly") || K("required") || K("multiple")) {
        const char* a = K("readOnly") ? "readonly" : key;
        return v_bool(dom_attr(n, a) != NULL);
    }
    if (K("href") || K("src") || K("action")) {
        const char* v = dom_attr(n, key);
        if (!v) return v_str(I, "");
        char out[1024];
        url_resolve(p->url, v, out, sizeof(out));
        return v_str(I, out);
    }
    if (K("type")) {
        const char* t = dom_attr(n, "type");
        if (t) return v_str(I, t);
        if (strcmp(n->tag, "input") == 0) return v_str(I, "text");
        if (strcmp(n->tag, "button") == 0) return v_str(I, "submit");
        if (strcmp(n->tag, "select") == 0) return v_str(I, "select-one");
        return v_str(I, "");
    }
    if (K("htmlFor")) return attr_or_empty(I, n, "for");
    if (K("title") || K("alt") || K("name") || K("placeholder") || K("rel") || K("method") || K("target") || K("lang")) return attr_or_empty(I, n, key);
    if (K("style")) {
        obj_t* o = obj_new(I, OBJ_HOST);
        o->hc = &style_class;
        o->host = n;
        o->proto = p->style_proto;
        return v_obj(o);
    }
    if (K("classList")) {
        obj_t* o = obj_new(I, OBJ_HOST);
        o->hc = &class_class;
        o->host = n;
        o->proto = p->class_proto;
        return v_obj(o);
    }
    if (K("dataset")) {
        obj_t* o = obj_new(I, OBJ_PLAIN);
        for (dom_attr_t* a = n->attrs; a; a = a->next) {
            if (strncmp(a->name, "data-", 5) != 0) continue;
            char camel[64];
            int k = 0;
            for (const char* s = a->name + 5; *s && k < 63; s++) {
                if (*s == '-' && s[1]) { s++; camel[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; }
                else camel[k++] = *s;
            }
            camel[k] = 0;
            obj_set(I, o, camel, v_str(I, a->value));
        }
        return v_obj(o);
    }
    if (K("offsetWidth") || K("clientWidth") || K("scrollWidth")) return v_num(n->box_w);
    if (K("offsetHeight") || K("clientHeight") || K("scrollHeight")) return v_num(n->box_h);
    if (K("offsetLeft")) return v_num(n->box_x);
    if (K("offsetTop")) return v_num(n->box_y);
    if (K("scrollTop") || K("scrollLeft")) return v_num(0);
    if (K("tabIndex")) return v_num(0);
    if (strcmp(n->tag, "select") == 0) {
        if (K("options")) return find_list(I, n, 0, "option");
        if (K("selectedIndex")) {
            dom_node_t* opts[128];
            int cnt = collect(n, 0, "option", opts, 0);
            const char* v = node_value(p, n);
            for (int i = 0; i < cnt && i < 128; i++)
                if (strcmp(node_value(p, opts[i]), v) == 0) return v_num(i);
            return v_num(cnt ? 0 : -1);
        }
        if (K("length")) {
            dom_node_t* opts[128];
            return v_num(collect(n, 0, "option", opts, 0));
        }
    }
    if (strcmp(n->tag, "form") == 0 && K("elements")) {
        obj_t* a = obj_new(I, OBJ_ARRAY);
        dom_node_t* all[MAX_LIST];
        int cnt = collect(n, 0, "*", all, 0);
        for (int i = 0; i < cnt; i++)
            if (is_elem(all[i], "input") || is_elem(all[i], "select") || is_elem(all[i], "textarea") || is_elem(all[i], "button"))
                arr_push(I, a, wrap(p, all[i]));
        return v_obj(a);
    }
    if (key[0] == 'o' && key[1] == 'n' && key[2]) {
        value_t* h = prop_handler(p, n, key + 2);
        return h ? *h : v_null();
    }
    if (K("__tostring")) {
        char buf[48];
        upper_tag(n->tag, up, sizeof(up));
        ksnprintf(buf, sizeof(buf), "[object HTML%sElement]", up);
        return v_str(I, buf);
    }
    *found = 0;
    return v_undef();
}

static int elem_set(interp_t* I, obj_t* self, const char* key, value_t v) {
    page_t* p = P(I);
    dom_node_t* n = (dom_node_t*)self->host;
    if (n->type == DOM_DOC) {
        if (K("title")) {
            kstrlcpy(p->title, v_cstr(I, v), sizeof(p->title));
            dom_node_t* t = dom_find_tag(n, "title");
            if (t) set_text(p, t, p->title);
            return 1;
        }
        if (K("location")) { url_resolve(p->url, v_cstr(I, v), p->nav, sizeof(p->nav)); p->nav_pending = 1; return 1; }
        return 0;
    }
    if (n->type != DOM_ELEM) {
        if (K("textContent") || K("nodeValue") || K("data")) {
            const char* s = v_cstr(I, v);
            n->text_len = (uint32_t)strlen(s);
            n->text = arena_strdup(&p->A, s, n->text_len);
            changed(p);
            return 1;
        }
        return 0;
    }
    const char* s = (v.t == V_NULL || v.t == V_UNDEF) ? "" : NULL;
    if (K("innerHTML")) { set_html(p, n, s ? s : v_cstr(I, v)); return 1; }
    if (K("textContent") || K("innerText") || K("text")) { set_text(p, n, s ? s : v_cstr(I, v)); return 1; }
    if (K("outerHTML")) {
        dom_node_t* holder = dom_new_element(&p->A, "div");
        html_parse_into(&p->A, holder, v_cstr(I, v), (uint32_t)strlen(v_cstr(I, v)));
        dom_node_t* parent = n->parent;
        if (parent) {
            while (holder->first) { dom_node_t* c = holder->first; dom_remove(c); dom_insert_before(parent, c, n); }
            dom_remove(n);
        }
        changed(p);
        return 1;
    }
    if (K("value")) { set_value(p, n, s ? s : v_cstr(I, v)); return 1; }
    if (K("checked")) {
        if (!n->form_init) { n->value = (char*)node_value(p, n); n->form_init = 1; }
        n->checked = v_truthy(I, v);
        if (n->checked) {
            const char* t = dom_attr(n, "type");
            const char* nm = dom_attr(n, "name");
            if (t && nm && strcasecmp(t, "radio") == 0) {
                dom_node_t* list[MAX_LIST];
                int cnt = collect(p->doc, 0, "input", list, 0);
                for (int i = 0; i < cnt; i++) {
                    const char* on = dom_attr(list[i], "name");
                    if (list[i] != n && on && strcmp(on, nm) == 0) {
                        if (!list[i]->form_init) { list[i]->value = (char*)node_value(p, list[i]); list[i]->form_init = 1; }
                        list[i]->checked = 0;
                    }
                }
            }
        }
        changed(p);
        return 1;
    }
    if (K("disabled") || K("hidden") || K("selected") || K("readOnly") || K("required")) {
        const char* a = K("readOnly") ? "readonly" : key;
        if (v_truthy(I, v)) dom_set_attr(&p->A, n, a, "");
        else dom_remove_attr(n, a);
        changed(p);
        return 1;
    }
    if (K("id")) { dom_set_attr(&p->A, n, "id", v_cstr(I, v)); changed(p); return 1; }
    if (K("className")) { dom_set_attr(&p->A, n, "class", v_cstr(I, v)); changed(p); return 1; }
    if (K("htmlFor")) { dom_set_attr(&p->A, n, "for", v_cstr(I, v)); return 1; }
    if (K("href") || K("src") || K("title") || K("alt") || K("name") || K("placeholder") || K("type") ||
        K("rel") || K("action") || K("method") || K("target") || K("lang")) {
        dom_set_attr(&p->A, n, key, v_cstr(I, v));
        if (K("src") && n->img) n->img = NULL;          /* reload */
        changed(p);
        return 1;
    }
    if (K("style")) { dom_set_attr(&p->A, n, "style", v_cstr(I, v)); changed(p); return 1; }
    if (K("selectedIndex") && strcmp(n->tag, "select") == 0) {
        dom_node_t* opts[128];
        int cnt = collect(n, 0, "option", opts, 0);
        int i = (int)v_tonum(I, v);
        if (i >= 0 && i < cnt && i < 128) set_value(p, n, node_value(p, opts[i]));
        return 1;
    }
    if (key[0] == 'o' && key[1] == 'n' && key[2]) { set_prop_handler(p, n, key + 2, v); return 1; }
    if (K("scrollTop")) return 1;
    return 0;
}

static const host_class_t elem_class = { "Element", elem_get, elem_set };

/* ── element methods ──────────────────────────────────────────────── */

static value_t m_getAttribute(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    if (!n || n->type != DOM_ELEM) return v_null();
    char name[64];
    kstrlcpy(name, arg_str(I, argc, argv, 0), sizeof(name));
    for (char* c = name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    if (strcmp(name, "value") == 0 && n->form_init && strcmp(n->tag, "input") != 0) return v_str(I, node_value(P(I), n));
    const char* v = dom_attr(n, name);
    return v ? v_str(I, v) : v_null();
}

static value_t m_setAttribute(interp_t* I, value_t self, int argc, value_t* argv) {
    page_t* p = P(I);
    dom_node_t* n = self_node(I, self);
    if (!n || n->type != DOM_ELEM) return v_undef();
    char name[64];
    kstrlcpy(name, arg_str(I, argc, argv, 0), sizeof(name));
    for (char* c = name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    const char* val = arg_str(I, argc, argv, 1);
    dom_set_attr(&p->A, n, name, val);
    if (name[0] == 'o' && name[1] == 'n') {                /* recompile on next event */
        char ev[40];
        ksnprintf(ev, sizeof(ev), "@%s", name + 2);
        remove_handler(n, ev, NULL);
    } else if (strcmp(name, "value") == 0) {
        n->value = dupz(p, val);
    } else if (strcmp(name, "checked") == 0) {
        n->checked = 1;
    } else if (strcmp(name, "src") == 0) {
        n->img = NULL;
    }
    changed(p);
    return v_undef();
}

static value_t m_removeAttribute(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    if (!n || n->type != DOM_ELEM) return v_undef();
    const char* name = arg_str(I, argc, argv, 0);
    dom_remove_attr(n, name);
    if (strcmp(name, "checked") == 0) n->checked = 0;
    changed(P(I));
    return v_undef();
}

static value_t m_hasAttribute(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    return v_bool(n && n->type == DOM_ELEM && dom_attr(n, arg_str(I, argc, argv, 0)) != NULL);
}

static int is_fragment(dom_node_t* n) { return n && n->type == DOM_ELEM && strcmp(n->tag, "#fragment") == 0; }

static int contains(dom_node_t* a, dom_node_t* b) {
    for (; b; b = b->parent) if (a == b) return 1;
    return 0;
}

/* inserts child (or a fragment's children) into parent before ref */
static int insert(interp_t* I, dom_node_t* parent, dom_node_t* child, dom_node_t* ref) {
    if (!parent || !child) { script_throw(I, "TypeError: parameter is not of type 'Node'"); return 0; }
    if (contains(child, parent)) { script_throw(I, "HierarchyRequestError: the new child contains the parent"); return 0; }
    if (is_fragment(child)) {
        while (child->first) {
            dom_node_t* c = child->first;
            dom_remove(c);
            dom_insert_before(parent, c, ref);
        }
    } else {
        if (child == ref) return 1;
        if (child->parent) dom_remove(child);
        if (ref && ref->parent == parent) dom_insert_before(parent, child, ref);
        else dom_append(parent, child);
    }
    changed(P(I));
    return 1;
}

/* a string argument (append("text")) becomes a text node */
static dom_node_t* node_or_text(interp_t* I, value_t v) {
    dom_node_t* n = node_of(v);
    if (n) return n;
    const char* s = v_cstr(I, v);
    return dom_new_text(&P(I)->A, s, (uint32_t)strlen(s));
}

static value_t m_appendChild(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    insert(I, n, node_of(ARG(0)), NULL);
    return ARG(0);
}

static value_t m_append(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    for (int i = 0; i < argc && n; i++) insert(I, n, node_or_text(I, argv[i]), NULL);
    return v_undef();
}

static value_t m_prepend(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    dom_node_t* first = n ? n->first : NULL;
    for (int i = 0; i < argc && n; i++) insert(I, n, node_or_text(I, argv[i]), first);
    return v_undef();
}

static value_t m_insertBefore(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    insert(I, n, node_of(ARG(0)), node_of(ARG(1)));
    return ARG(0);
}

static value_t m_removeChild(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    dom_node_t* c = node_of(ARG(0));
    if (!n || !c || c->parent != n) { script_throw(I, "NotFoundError: the node is not a child of this node"); return v_undef(); }
    if (P(I)->focus && contains(c, P(I)->focus)) P(I)->focus = NULL;
    dom_remove(c);
    changed(P(I));
    return ARG(0);
}

static value_t m_replaceChild(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    dom_node_t* nw = node_of(ARG(0));
    dom_node_t* old = node_of(ARG(1));
    if (!n || !nw || !old || old->parent != n) { script_throw(I, "NotFoundError: the node is not a child of this node"); return v_undef(); }
    if (insert(I, n, nw, old)) dom_remove(old);
    return ARG(1);
}

static value_t m_remove(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    if (n && n->parent) {
        if (P(I)->focus && contains(n, P(I)->focus)) P(I)->focus = NULL;
        dom_remove(n);
        changed(P(I));
    }
    return v_undef();
}

static value_t m_replaceWith(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    if (!n || !n->parent) return v_undef();
    dom_node_t* parent = n->parent;
    for (int i = 0; i < argc; i++) insert(I, parent, node_or_text(I, argv[i]), n);
    dom_remove(n);
    changed(P(I));
    return v_undef();
}

static value_t m_hasChildNodes(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    return v_bool(n && n->first);
}

static value_t m_contains(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    dom_node_t* o = node_of(ARG(0));
    return v_bool(n && o && contains(n, o));
}

static dom_node_t* clone(page_t* p, dom_node_t* n, int deep) {
    dom_node_t* c;
    if (n->type == DOM_TEXT) return dom_new_text(&p->A, n->text, n->text_len);
    if (n->type != DOM_ELEM) return dom_new_text(&p->A, "", 0);
    c = dom_new_element(&p->A, n->tag);
    for (dom_attr_t* a = n->attrs; a; a = a->next) dom_set_attr(&p->A, c, a->name, a->value);
    c->value = n->value;
    c->checked = n->checked;
    c->form_init = n->form_init;
    if (deep)
        for (dom_node_t* k = n->first; k; k = k->next) dom_append(c, clone(p, k, 1));
    return c;
}

static value_t m_cloneNode(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    if (!n || n->type == DOM_DOC) return v_null();
    return wrap(P(I), clone(P(I), n, v_truthy(I, ARG(0))));
}

static value_t m_querySelector(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    if (!n) return v_null();
    return wrap(P(I), css_query(&P(I)->A, n, arg_str(I, argc, argv, 0)));
}

static value_t m_querySelectorAll(interp_t* I, value_t self, int argc, value_t* argv) {
    page_t* p = P(I);
    dom_node_t* n = self_node(I, self);
    if (!n) return v_undef();
    dom_node_t** buf = (dom_node_t**)arena_alloc(&p->A, MAX_LIST * (uint32_t)sizeof(dom_node_t*));
    int k = css_query_all(&p->A, n, arg_str(I, argc, argv, 0), buf, MAX_LIST);
    return list_of(p, buf, k);
}

static value_t m_getElementsByTagName(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    return n ? find_list(I, n, 0, arg_str(I, argc, argv, 0)) : v_undef();
}

static value_t m_getElementsByClassName(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    return n ? find_list(I, n, 1, arg_str(I, argc, argv, 0)) : v_undef();
}

static value_t m_closest(interp_t* I, value_t self, int argc, value_t* argv) {
    const char* sel = arg_str(I, argc, argv, 0);
    for (dom_node_t* n = self_node(I, self); n && n->type == DOM_ELEM; n = n->parent)
        if (css_matches(&P(I)->A, n, sel)) return wrap(P(I), n);
    return v_null();
}

static value_t m_matches(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    return v_bool(n && n->type == DOM_ELEM && css_matches(&P(I)->A, n, arg_str(I, argc, argv, 0)));
}

static value_t m_addEventListener(interp_t* I, value_t self, int argc, value_t* argv) {
    page_t* p = P(I);
    dom_node_t* n = self_node(I, self);
    const char* type = arg_str(I, argc, argv, 0);
    if (!n || !v_isfunc(ARG(1))) return v_undef();
    if (n->type == DOM_DOC && (strcmp(type, "DOMContentLoaded") == 0 || strcmp(type, "load") == 0)) {
        if (p->nonload < MAX_ONLOAD) p->onload[p->nonload++] = ARG(1);
        return v_undef();
    }
    add_handler(p, n, type, ARG(1));
    return v_undef();
}

static value_t m_removeEventListener(interp_t* I, value_t self, int argc, value_t* argv) {
    dom_node_t* n = self_node(I, self);
    value_t fn = ARG(1);
    if (n) remove_handler(n, arg_str(I, argc, argv, 0), &fn);
    return v_undef();
}

static value_t m_click(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    if (!n) return v_undef();
    if (is_elem(n, "input")) {
        const char* t = dom_attr(n, "type");
        if (t && (strcasecmp(t, "checkbox") == 0)) {
            if (!n->form_init) { n->value = (char*)node_value(P(I), n); n->checked = dom_attr(n, "checked") != NULL; n->form_init = 1; }
            n->checked = !n->checked;
        }
    }
    dispatch(P(I), n, "click", NULL);
    return v_undef();
}

static value_t m_focus(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    if (n && (is_elem(n, "input") || is_elem(n, "textarea"))) { P(I)->focus = n; changed(P(I)); }
    return v_undef();
}

static value_t m_blur(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    if (P(I)->focus == self_node(I, self)) { P(I)->focus = NULL; changed(P(I)); }
    return v_undef();
}

static value_t m_insertAdjacentHTML(interp_t* I, value_t self, int argc, value_t* argv) {
    page_t* p = P(I);
    dom_node_t* n = self_node(I, self);
    if (!n) return v_undef();
    const char* where = arg_str(I, argc, argv, 0);
    const char* html = arg_str(I, argc, argv, 1);
    dom_node_t* holder = dom_new_element(&p->A, "div");
    html_parse_into(&p->A, holder, html, (uint32_t)strlen(html));
    dom_node_t *parent, *ref;
    if (strcasecmp(where, "beforebegin") == 0) { parent = n->parent; ref = n; }
    else if (strcasecmp(where, "afterend") == 0) { parent = n->parent; ref = n->next; }
    else if (strcasecmp(where, "afterbegin") == 0) { parent = n; ref = n->first; }
    else { parent = n; ref = NULL; }
    if (!parent) return v_undef();
    while (holder->first) {
        dom_node_t* c = holder->first;
        dom_remove(c);
        if (ref) dom_insert_before(parent, c, ref);
        else dom_append(parent, c);
    }
    changed(p);
    return v_undef();
}

static value_t m_getBoundingClientRect(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    obj_t* r = obj_new(I, OBJ_PLAIN);
    if (!n) return v_obj(r);
    obj_set(I, r, "x", v_num(n->box_x));
    obj_set(I, r, "y", v_num(n->box_y));
    obj_set(I, r, "left", v_num(n->box_x));
    obj_set(I, r, "top", v_num(n->box_y));
    obj_set(I, r, "width", v_num(n->box_w));
    obj_set(I, r, "height", v_num(n->box_h));
    obj_set(I, r, "right", v_num(n->box_x + n->box_w));
    obj_set(I, r, "bottom", v_num(n->box_y + n->box_h));
    return v_obj(r);
}

static value_t m_getAttributeNames(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    obj_t* a = obj_new(I, OBJ_ARRAY);
    if (n) for (dom_attr_t* at = n->attrs; at; at = at->next) arr_push(I, a, v_str(I, at->name));
    return v_obj(a);
}

static value_t m_submit(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    if (!n) return v_undef();
    /* a synthetic click on a hidden submit button would be simpler, but
     * form.submit() must not fire onsubmit: build the GET url here */
    page_t* p = P(I);
    char base[1024];
    const char* action = dom_attr(n, "action");
    if (action && *action) url_resolve(p->url, action, base, sizeof(base));
    else kstrlcpy(base, p->url, sizeof(base));
    char* q = strchr(base, '?');
    if (q) *q = 0;
    kstrlcpy(p->nav, base, sizeof(p->nav));
    kstrlcat(p->nav, "?", sizeof(p->nav));
    dom_node_t* all[MAX_LIST];
    int cnt = collect(n, 0, "*", all, 0);
    int first = 1;
    for (int i = 0; i < cnt; i++) {
        dom_node_t* e = all[i];
        const char* name = dom_attr(e, "name");
        if (!name || !(is_elem(e, "input") || is_elem(e, "select") || is_elem(e, "textarea"))) continue;
        const char* t = dom_attr(e, "type");
        if (t && (strcasecmp(t, "submit") == 0 || strcasecmp(t, "button") == 0)) continue;
        if (t && (strcasecmp(t, "checkbox") == 0 || strcasecmp(t, "radio") == 0) && !e->checked) continue;
        if (!first) kstrlcat(p->nav, "&", sizeof(p->nav));
        first = 0;
        kstrlcat(p->nav, name, sizeof(p->nav));
        kstrlcat(p->nav, "=", sizeof(p->nav));
        static const char hx[] = "0123456789ABCDEF";
        for (const char* s = node_value(p, e); *s; s++) {
            unsigned char c = (unsigned char)*s;
            char enc[4] = { (char)c, 0, 0, 0 };
            if (c == ' ') enc[0] = '+';
            else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
                enc[0] = '%'; enc[1] = hx[c >> 4]; enc[2] = hx[c & 15];
            }
            kstrlcat(p->nav, enc, sizeof(p->nav));
        }
    }
    p->nav_pending = 1;
    return v_undef();
}

static value_t m_reset(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    dom_node_t* n = self_node(I, self);
    if (!n) return v_undef();
    dom_node_t* all[MAX_LIST];
    int cnt = collect(n, 0, "*", all, 0);
    for (int i = 0; i < cnt; i++) all[i]->form_init = 0;
    changed(P(I));
    return v_undef();
}

/* ── document methods ─────────────────────────────────────────────── */

static value_t d_getElementById(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    return wrap(P(I), dom_find_id(P(I)->doc, arg_str(I, argc, argv, 0)));
}

static value_t d_createElement(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    char tag[32];
    kstrlcpy(tag, arg_str(I, argc, argv, 0), sizeof(tag));
    for (char* c = tag; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    return wrap(P(I), dom_new_element(&P(I)->A, tag));
}

static value_t d_createTextNode(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    const char* s = arg_str(I, argc, argv, 0);
    return wrap(P(I), dom_new_text(&P(I)->A, s, (uint32_t)strlen(s)));
}

static value_t d_createDocumentFragment(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self; (void)argc; (void)argv;
    return wrap(P(I), dom_new_element(&P(I)->A, "#fragment"));
}

static void write_text(page_t* p, const char* s, uint32_t n) {
    if (!p->cur_script) {                 /* after loading: append to the body */
        dom_node_t* body = dom_body(p->doc);
        if (body) html_parse_into(&p->A, body, s, n);
        changed(p);
        return;
    }
    if (p->write_len + n + 1 > p->write_cap) {
        uint32_t cap = (p->write_len + n + 1) * 2 + 256;
        char* nb = (char*)arena_alloc(&p->A, cap);
        if (p->write_len) memcpy(nb, p->write_buf, p->write_len);
        p->write_buf = nb;
        p->write_cap = cap;
    }
    memcpy(p->write_buf + p->write_len, s, n);
    p->write_len += n;
    p->write_buf[p->write_len] = 0;
}

static value_t d_write(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    for (int i = 0; i < argc; i++) {
        const char* s = v_cstr(I, argv[i]);
        write_text(P(I), s, (uint32_t)strlen(s));
    }
    return v_undef();
}

static value_t d_writeln(interp_t* I, value_t self, int argc, value_t* argv) {
    d_write(I, self, argc, argv);
    write_text(P(I), "\n", 1);
    return v_undef();
}

/* ══ window ═══════════════════════════════════════════════════════════ */

static value_t w_alert(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = P(I);
    const char* s = argc ? v_cstr(I, argv[0]) : "";
    if (p->alert_pending) {               /* several alerts: show them together */
        kstrlcat(p->alert, "\n", sizeof(p->alert));
        kstrlcat(p->alert, s, sizeof(p->alert));
    } else {
        kstrlcpy(p->alert, s, sizeof(p->alert));
    }
    p->alert_pending = 1;
    return v_undef();
}

static value_t w_confirm(interp_t* I, value_t self, int argc, value_t* argv) {
    w_alert(I, self, argc, argv);
    return v_bool(1);
}

static value_t w_prompt(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    if (argc > 1) return v_str(I, v_cstr(I, argv[1]));
    return v_null();
}

static value_t add_timer(interp_t* I, int argc, value_t* argv, int repeat) {
    page_t* p = P(I);
    if (!argc) return v_num(0);
    num_t ms = argc > 1 ? v_tonum(I, argv[1]) : 0;
    if (!(ms >= 0)) ms = 0;
    if (ms > 86400000) ms = 86400000;
    if (repeat && ms < 10) ms = 10;
    for (int i = 0; i < PAGE_TIMERS; i++) {
        page_timer_t* t = &p->timers[i];
        if (t->active) continue;
        t->active = 1;
        t->id = p->next_timer++;
        t->fn = argv[0].t == V_FUNC ? argv[0] : v_str(I, v_cstr(I, argv[0]));
        t->interval = repeat ? (uint32_t)ms : 0;
        t->due = (p->env && p->env->now_ms ? p->env->now_ms() : 0) + (uint32_t)ms;
        return v_num(t->id);
    }
    script_throw(I, "Error: too many timers");
    return v_num(0);
}

static value_t w_setTimeout(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return add_timer(I, argc, argv, 0); }
static value_t w_setInterval(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return add_timer(I, argc, argv, 1); }

static value_t w_requestAnimationFrame(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t a[2] = { ARG(0), v_num(16) };
    return add_timer(I, 2, a, 0);
}

static value_t w_clearTimer(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = P(I);
    int id = (int)v_tonum(I, ARG(0));
    for (int i = 0; i < PAGE_TIMERS; i++)
        if (p->timers[i].active && p->timers[i].id == id) p->timers[i].active = 0;
    return v_undef();
}

static value_t w_scrollTo(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    num_t y = argc > 1 ? v_tonum(I, argv[1]) : 0;
    if (argc == 1 && argv[0].t == V_OBJ) y = v_tonum(I, obj_get(I, argv[0].o, "top"));
    P(I)->scroll_req = y > 0 ? (int)y : 0;
    return v_undef();
}

static value_t w_getComputedStyle(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    /* inline styles only */
    dom_node_t* n = node_of(ARG(0));
    if (!n) return v_undef();
    int f;
    return elem_get(I, (obj_t*)n->js, "style", &f);
}

static value_t win_get(interp_t* I, obj_t* self, const char* key, int* found) {
    (void)self;
    page_t* p = P(I);
    *found = 1;
    if (K("window") || K("self") || K("top") || K("parent") || K("globalThis")) return v_obj(p->win_obj);
    if (K("document")) return wrap(p, p->doc);
    if (K("location")) return v_obj(p->loc_obj);
    if (K("innerWidth") || K("outerWidth")) return v_num(p->width);
    if (K("innerHeight") || K("outerHeight")) return v_num(p->view_h ? p->view_h : 480);
    if (K("scrollX") || K("scrollY") || K("pageXOffset") || K("pageYOffset")) return v_num(0);
    if (K("onload")) return p->nonload ? p->onload[0] : v_null();
    value_t v = script_get_global(I, key);
    if (v.t == V_UNDEF) *found = 0;
    return v;
}

static int win_set(interp_t* I, obj_t* self, const char* key, value_t v) {
    (void)self;
    page_t* p = P(I);
    if (K("onload")) {
        if (v_isfunc(v) && p->nonload < MAX_ONLOAD) p->onload[p->nonload++] = v;
        return 1;
    }
    if (K("location")) { url_resolve(p->url, v_cstr(I, v), p->nav, sizeof(p->nav)); p->nav_pending = 1; return 1; }
    script_def_global(I, key, v);
    return 1;
}

static value_t w_addEventListener(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = P(I);
    const char* type = arg_str(I, argc, argv, 0);
    if (!v_isfunc(ARG(1))) return v_undef();
    if (strcmp(type, "load") == 0 || strcmp(type, "DOMContentLoaded") == 0) {
        if (p->nonload < MAX_ONLOAD) p->onload[p->nonload++] = ARG(1);
    } else {
        add_handler(p, p->doc, type, ARG(1));       /* keys etc. bubble up to the document */
    }
    return v_undef();
}

static const host_class_t win_class = { "Window", win_get, win_set };

/* ══ location ═════════════════════════════════════════════════════════ */

static void url_parts(const char* url, const char** host, int* hl, const char** path, int* pl,
                      const char** search, int* sl, const char** hash, int* hsl) {
    const char* s = strstr(url, "://");
    const char* h = s ? s + 3 : url;
    const char* e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    *host = h; *hl = s ? (int)(e - h) : 0;
    if (!s) e = url;
    const char* q = e;
    while (*q && *q != '?' && *q != '#') q++;
    *path = e; *pl = (int)(q - e);
    const char* hs = q;
    while (*hs && *hs != '#') hs++;
    *search = q; *sl = (int)(hs - q);
    *hash = hs; *hsl = (int)strlen(hs);
}

static value_t loc_get(interp_t* I, obj_t* self, const char* key, int* found) {
    (void)self;
    page_t* p = P(I);
    const char *host, *path, *search, *hash;
    int hl, pl, sl, hsl;
    url_parts(p->url, &host, &hl, &path, &pl, &search, &sl, &hash, &hsl);
    *found = 1;
    if (K("href") || K("__tostring")) return v_str(I, p->url);
    if (K("protocol")) {
        const char* c = strchr(p->url, ':');
        return v_strn(I, p->url, c ? (uint32_t)(c - p->url + 1) : 0);
    }
    if (K("host")) return v_strn(I, host, (uint32_t)hl);
    if (K("hostname")) {
        int k = 0;
        while (k < hl && host[k] != ':') k++;
        return v_strn(I, host, (uint32_t)k);
    }
    if (K("port")) {
        int k = 0;
        while (k < hl && host[k] != ':') k++;
        return k < hl ? v_strn(I, host + k + 1, (uint32_t)(hl - k - 1)) : v_str(I, "");
    }
    if (K("origin")) return v_strn(I, p->url, (uint32_t)(host + hl - p->url));
    if (K("pathname")) return pl ? v_strn(I, path, (uint32_t)pl) : v_str(I, "/");
    if (K("search")) return sl > 1 ? v_strn(I, search, (uint32_t)sl) : v_str(I, "");
    if (K("hash")) return hsl > 1 ? v_strn(I, hash, (uint32_t)hsl) : v_str(I, "");
    *found = 0;
    return v_undef();
}

static int loc_set(interp_t* I, obj_t* self, const char* key, value_t v) {
    (void)self;
    page_t* p = P(I);
    const char* s = v_cstr(I, v);
    if (K("href")) { url_resolve(p->url, s, p->nav, sizeof(p->nav)); p->nav_pending = 1; return 1; }
    if (K("hash")) {
        char* h = strchr(p->url, '#');
        if (h) *h = 0;
        if (*s == '#') s++;
        kstrlcat(p->url, "#", sizeof(p->url));
        kstrlcat(p->url, s, sizeof(p->url));
        p->scroll_req = page_anchor_y(p, s);
        return 1;
    }
    if (K("search")) {
        char rel[1024];
        ksnprintf(rel, sizeof(rel), "%s%s", *s == '?' ? "" : "?", s);
        url_resolve(p->url, rel, p->nav, sizeof(p->nav));
        p->nav_pending = 1;
        return 1;
    }
    return 1;
}

static value_t l_reload(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self; (void)argc; (void)argv;
    page_t* p = P(I);
    kstrlcpy(p->nav, p->url, sizeof(p->nav));
    p->nav_pending = 1;
    return v_undef();
}

static value_t l_assign(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = P(I);
    url_resolve(p->url, arg_str(I, argc, argv, 0), p->nav, sizeof(p->nav));
    p->nav_pending = 1;
    return v_undef();
}

static value_t l_toString(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self; (void)argc; (void)argv;
    return v_str(I, P(I)->url);
}

static const host_class_t loc_class = { "Location", loc_get, loc_set };

/* ══ storage (per page, not persisted) ════════════════════════════════ */

static value_t s_getItem(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ) return v_null();
    char k[128];
    ksnprintf(k, sizeof(k), "\x01%s", arg_str(I, argc, argv, 0));
    int f = 0;
    value_t v = prop_get_raw(self.o, k, &f);
    return f && v.t == V_STR ? v : v_null();
}

static value_t s_setItem(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ) return v_undef();
    char k[128];
    ksnprintf(k, sizeof(k), "\x01%s", arg_str(I, argc, argv, 0));
    obj_set(I, self.o, k, v_str(I, arg_str(I, argc, argv, 1)));
    return v_undef();
}

static value_t s_removeItem(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_OBJ) return v_undef();
    char k[128];
    ksnprintf(k, sizeof(k), "\x01%s", arg_str(I, argc, argv, 0));
    obj_set(I, self.o, k, v_null());
    return v_undef();
}

/* ══ install ══════════════════════════════════════════════════════════ */

typedef struct { const char* name; native_fn f; } method_t;

static obj_t* table(interp_t* I, const method_t* m, int n, obj_t* proto) {
    obj_t* o = obj_new(I, OBJ_PLAIN);
    for (int i = 0; i < n; i++) obj_set(I, o, m[i].name, v_native(I, m[i].name, m[i].f));
    o->proto = proto;
    return o;
}

#define TABLE(I, arr, proto) table(I, arr, (int)(sizeof(arr) / sizeof(arr[0])), proto)

static const method_t ELEM_METHODS[] = {
    { "getAttribute", m_getAttribute }, { "setAttribute", m_setAttribute }, { "removeAttribute", m_removeAttribute },
    { "hasAttribute", m_hasAttribute }, { "getAttributeNames", m_getAttributeNames },
    { "appendChild", m_appendChild }, { "append", m_append }, { "prepend", m_prepend },
    { "insertBefore", m_insertBefore }, { "removeChild", m_removeChild }, { "replaceChild", m_replaceChild },
    { "remove", m_remove }, { "replaceWith", m_replaceWith }, { "hasChildNodes", m_hasChildNodes }, { "contains", m_contains },
    { "cloneNode", m_cloneNode }, { "querySelector", m_querySelector }, { "querySelectorAll", m_querySelectorAll },
    { "getElementsByTagName", m_getElementsByTagName }, { "getElementsByClassName", m_getElementsByClassName },
    { "closest", m_closest }, { "matches", m_matches },
    { "addEventListener", m_addEventListener }, { "removeEventListener", m_removeEventListener },
    { "click", m_click }, { "focus", m_focus }, { "blur", m_blur }, { "insertAdjacentHTML", m_insertAdjacentHTML },
    { "getBoundingClientRect", m_getBoundingClientRect }, { "submit", m_submit }, { "reset", m_reset },
};

static const method_t DOC_METHODS[] = {
    { "getElementById", d_getElementById }, { "createElement", d_createElement },
    { "createTextNode", d_createTextNode }, { "createDocumentFragment", d_createDocumentFragment },
    { "write", d_write }, { "writeln", d_writeln },
};

static const method_t STYLE_METHODS[] = {
    { "setProperty", st_setProperty }, { "removeProperty", st_removeProperty }, { "getPropertyValue", st_getPropertyValue },
};

static const method_t CLASS_METHODS[] = {
    { "add", cl_add }, { "remove", cl_remove }, { "contains", cl_contains }, { "toggle", cl_toggle },
};

static const method_t LOC_METHODS[] = {
    { "reload", l_reload }, { "assign", l_assign }, { "replace", l_assign }, { "toString", l_toString },
};

static const method_t WIN_FUNCS[] = {
    { "alert", w_alert }, { "confirm", w_confirm }, { "prompt", w_prompt },
    { "setTimeout", w_setTimeout }, { "setInterval", w_setInterval },
    { "clearTimeout", w_clearTimer }, { "clearInterval", w_clearTimer },
    { "requestAnimationFrame", w_requestAnimationFrame }, { "cancelAnimationFrame", w_clearTimer },
    { "scrollTo", w_scrollTo }, { "scroll", w_scrollTo }, { "getComputedStyle", w_getComputedStyle },
    { "addEventListener", w_addEventListener },
};

static const method_t STORAGE_METHODS[] = {
    { "getItem", s_getItem }, { "setItem", s_setItem }, { "removeItem", s_removeItem },
};

static void js_log(void* ctx, const char* s, uint32_t n) {
    page_t* p = (page_t*)ctx;
    if (!p->env || !p->env->log) return;
    char buf[240];
    if (n > sizeof(buf) - 5) n = sizeof(buf) - 5;
    memcpy(buf, "js> ", 4);
    memcpy(buf + 4, s, n);
    buf[4 + n] = 0;
    if (n && buf[3 + n] == '\n') buf[3 + n] = 0;
    p->env->log(p->env->ctx, buf);
}

static void js_out(void* ctx, const char* s, uint32_t n) { write_text((page_t*)ctx, s, n); }

void jsdom_install(page_t* p) {
    interp_t* I = p->js;
    script_set_host(I, p);
    script_set_log(I, js_log, p);
    script_set_output(I, js_out, p);
    p->onload = (value_t*)arena_alloc(&p->A, MAX_ONLOAD * (uint32_t)sizeof(value_t));
    p->nonload = 0;

    p->elem_proto = TABLE(I, ELEM_METHODS, NULL);
    p->doc_obj = TABLE(I, DOC_METHODS, p->elem_proto);         /* the document's prototype */
    p->style_proto = TABLE(I, STYLE_METHODS, NULL);
    p->class_proto = TABLE(I, CLASS_METHODS, NULL);

    p->loc_obj = TABLE(I, LOC_METHODS, NULL);
    p->loc_obj->kind = OBJ_HOST;
    p->loc_obj->hc = &loc_class;

    value_t doc = wrap(p, p->doc);
    obj_set(I, doc.o, "cookie", v_str(I, ""));

    obj_t* win = obj_new(I, OBJ_HOST);
    win->hc = &win_class;
    p->win_obj = win;
    for (uint32_t i = 0; i < sizeof(WIN_FUNCS) / sizeof(WIN_FUNCS[0]); i++)
        script_def_global(I, WIN_FUNCS[i].name, v_native(I, WIN_FUNCS[i].name, WIN_FUNCS[i].f));

    script_def_global(I, "window", v_obj(win));
    script_def_global(I, "self", v_obj(win));
    script_def_global(I, "document", doc);
    script_def_global(I, "location", v_obj(p->loc_obj));

    obj_t* nav = obj_new(I, OBJ_PLAIN);
    obj_set(I, nav, "userAgent", v_str(I, "Mozilla/5.0 (Banana OS) BananaBrowser/1.0"));
    obj_set(I, nav, "appName", v_str(I, "BananaBrowser"));
    obj_set(I, nav, "platform", v_str(I, "Banana OS"));
    obj_set(I, nav, "language", v_str(I, "en-US"));
    obj_set(I, nav, "onLine", v_bool(1));
    obj_set(I, nav, "cookieEnabled", v_bool(0));
    script_def_global(I, "navigator", v_obj(nav));

    obj_t* scr = obj_new(I, OBJ_PLAIN);
    obj_set(I, scr, "width", v_num(p->width ? p->width : 800));
    obj_set(I, scr, "height", v_num(600));
    script_def_global(I, "screen", v_obj(scr));

    script_def_global(I, "localStorage", v_obj(TABLE(I, STORAGE_METHODS, NULL)));
    script_def_global(I, "sessionStorage", v_obj(TABLE(I, STORAGE_METHODS, NULL)));
}
