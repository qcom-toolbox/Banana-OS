#include "script_int.h"
#include "regex.h"
#include "kstring.h"
#include "timer.h"

/*
 * The newer JavaScript built-ins: RegExp (and the string methods that take
 * one), Promise (with a job queue run after each script/event), Map/Set,
 * Symbol, Function.prototype.bind, and the Array/Object/String/Number/Math
 * additions modern pages expect.
 */

#define ARG(i) ((i) < argc ? argv[i] : v_undef())

static int is_new_call(value_t self) { return self.t == V_NULL && self.b == 0x4E57; }

static value_t hidden_get(obj_t* o, const char* k) {
    int f = 0;
    value_t v = prop_get_raw(o, k, &f);
    return f ? v : v_undef();
}

static void hidden_set(interp_t* I, obj_t* o, const char* k, value_t v) {
    prop_set_raw(I, o, str_new(I, k, (uint32_t)strlen(k)), v);
}

static value_t native(interp_t* I, const char* name, native_fn f, value_t data) {
    value_t v = v_native(I, name, f);
    v.f->data = data;
    return v;
}

static obj_t* statics_of(interp_t* I, value_t fn) {
    if (!fn.f->statics) fn.f->statics = obj_new(I, OBJ_PLAIN);
    return fn.f->statics;
}

static void method(interp_t* I, obj_t* o, const char* name, native_fn f) {
    obj_set(I, o, name, v_native(I, name, f));
}

static void getter(interp_t* I, obj_t* o, const char* name, native_fn f) {
    obj_t* acc = obj_new(I, OBJ_PLAIN);
    acc->kind = OBJ_ACCESSOR;
    obj_set(I, acc, "get", v_native(I, name, f));
    hidden_set(I, o, name, v_obj(acc));
}

/* ══ iteration ════════════════════════════════════════════════════════ */

static int has_proto(obj_t* o, obj_t* proto) {
    for (obj_t* p = o->proto; p; p = p->proto) if (p == proto) return 1;
    return 0;
}

/* ── generators. A tree walker has no continuations, so the body runs
 * to its end on the first next(), collecting what it yields, and next()
 * hands the values out. Right for the usual finite generator; `yield`
 * itself evaluates to undefined, and an endless generator is stopped
 * after 10000 values. ───────────────────────────────────────────────── */
typedef struct { value_t fn, self; obj_t* args; obj_t* vals; uint32_t pos; value_t ret; int state; } gen_t;

value_t es_generator_new(interp_t* I, value_t fn, value_t self, int argc, value_t* argv) {
    gen_t* g = (gen_t*)arena_alloc(I->A, sizeof(gen_t));
    g->fn = fn;
    g->self = self;
    g->args = obj_new(I, OBJ_ARRAY);
    for (int i = 0; i < argc; i++) arr_push(I, g->args, argv[i]);
    obj_t* o = obj_new(I, OBJ_PLAIN);
    o->proto = I->proto_gen;
    o->host = g;
    return v_obj(o);
}

static gen_t* gen_of(interp_t* I, value_t v) {
    if (v.t != V_OBJ || !I->proto_gen || v.o->proto != I->proto_gen) return NULL;
    return (gen_t*)v.o->host;
}

/* state: 0 not started, 1 values ready, 2 finished */
static void gen_run(interp_t* I, gen_t* g) {
    obj_t* saved = I->gen_out;
    g->vals = obj_new(I, OBJ_ARRAY);
    I->gen_out = g->vals;
    I->gen_force = 1;
    g->ret = call_value(I, g->fn, g->self, (int)g->args->len, g->args->items);
    I->gen_force = 0;
    I->gen_out = saved;
    if (g->fn.f->is_async) g->ret = v_undef();      /* (that was a promise of it) */
    g->state = 1;
}

static value_t iter_result(interp_t* I, value_t v, int done) {
    obj_t* r = obj_new(I, OBJ_PLAIN);
    obj_set(I, r, "value", v);
    obj_set(I, r, "done", v_bool(done));
    return v_obj(r);
}

/* async generators answer with promises */
static value_t gen_answer(interp_t* I, gen_t* g, value_t r) {
    if (!g->fn.f->is_async) return r;
    value_t p = es_promise_new(I);
    if (I->ctl == CTL_THROW) {
        value_t e = I->ret;
        I->ctl = CTL_NONE;
        es_promise_settle(I, p, 1, e);
    } else {
        es_promise_settle(I, p, 0, r);
    }
    return p;
}

static value_t gen_next(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    gen_t* g = gen_of(I, self);
    if (!g) { script_throw(I, "TypeError: next method called on a non-generator"); return v_undef(); }
    if (g->state == 0) gen_run(I, g);
    value_t r;
    if (I->ctl == CTL_THROW) { g->state = 2; r = v_undef(); }
    else if (g->state == 1 && g->pos < g->vals->len) r = iter_result(I, g->vals->items[g->pos++], 0);
    else if (g->state == 1) { g->state = 2; r = iter_result(I, g->ret, 1); }
    else r = iter_result(I, v_undef(), 1);
    return gen_answer(I, g, r);
}

static value_t gen_return(interp_t* I, value_t self, int argc, value_t* argv) {
    gen_t* g = gen_of(I, self);
    if (!g) { script_throw(I, "TypeError: return method called on a non-generator"); return v_undef(); }
    g->state = 2;
    return gen_answer(I, g, iter_result(I, argc ? argv[0] : v_undef(), 1));
}

static value_t gen_throw(interp_t* I, value_t self, int argc, value_t* argv) {
    gen_t* g = gen_of(I, self);
    if (!g) { script_throw(I, "TypeError: throw method called on a non-generator"); return v_undef(); }
    g->state = 2;
    I->ret = argc ? argv[0] : v_undef();
    I->ctl = CTL_THROW;
    I->throw_line = I->line; I->throw_col = I->col; I->throw_src = I->src_name;
    return gen_answer(I, g, v_undef());
}

static value_t gen_self(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)argc; (void)argv;
    return self;
}

obj_t* es_to_array(interp_t* I, value_t v) {
    gen_t* g = gen_of(I, v);
    if (g) {                                         /* for-of / spread over a generator: what is left of it */
        if (g->state == 0) gen_run(I, g);
        obj_t* a = obj_new(I, OBJ_ARRAY);
        if (g->state == 1 && !I->ctl)
            for (; g->pos < g->vals->len; g->pos++) arr_push(I, a, g->vals->items[g->pos]);
        g->state = 2;
        return a;
    }
    if (v.t == V_STR) {
        obj_t* a = obj_new(I, OBJ_ARRAY);
        const unsigned char* s = (const unsigned char*)v.s->s;
        for (uint32_t i = 0; i < v.s->len; ) {
            uint32_t l = s[i] < 0x80 ? 1 : s[i] < 0xE0 ? 2 : s[i] < 0xF0 ? 3 : 4;
            if (i + l > v.s->len) l = v.s->len - i;
            arr_push(I, a, v_strn(I, (const char*)s + i, l));
            i += l;
        }
        return a;
    }
    if (v.t != V_OBJ) return NULL;
    obj_t* o = v.o;
    if (o->kind == OBJ_ARRAY) return o;
    if (I->proto_map && has_proto(o, I->proto_map)) {
        obj_t* k = hidden_get(o, "\x01k").o, *vals = hidden_get(o, "\x01v").o;
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (uint32_t i = 0; k && i < k->len; i++) {
            obj_t* pair = obj_new(I, OBJ_ARRAY);
            arr_push(I, pair, k->items[i]);
            arr_push(I, pair, vals->items[i]);
            arr_push(I, a, v_obj(pair));
        }
        return a;
    }
    if (I->proto_set && has_proto(o, I->proto_set)) {
        obj_t* k = hidden_get(o, "\x01k").o;
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (uint32_t i = 0; k && i < k->len; i++) arr_push(I, a, k->items[i]);
        return a;
    }
    /* an iterator protocol object: [Symbol.iterator]() / next() */
    value_t itf = obj_get(I, o, "@@iterator");
    if (itf.t == V_FUNC) {
        value_t it = call_value(I, itf, v, 0, NULL);
        if (I->ctl || it.t != V_OBJ) return NULL;
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (int guard = 0; guard < 1000000; guard++) {
            value_t nf = obj_getv(I, it, "next");
            if (nf.t != V_FUNC) break;
            value_t r = call_value(I, nf, it, 0, NULL);
            if (I->ctl || r.t != V_OBJ) break;
            if (v_truthy(I, obj_get(I, r.o, "done"))) break;
            arr_push(I, a, obj_get(I, r.o, "value"));
        }
        return a;
    }
    /* array-likes: length + indexes (NodeLists, arguments, {length: n}) */
    value_t len = obj_get(I, o, "length");
    if (len.t == V_NUM && len.n >= 0 && len.n < 10000000) {
        obj_t* a = obj_new(I, OBJ_ARRAY);
        uint32_t n = (uint32_t)len.n;
        for (uint32_t i = 0; i < n; i++) {
            char b[16];
            ksnprintf(b, sizeof(b), "%u", i);
            arr_push(I, a, obj_get(I, o, b));
        }
        return a;
    }
    return NULL;
}

/* ── Proxy: a host object that forwards to its target, through the
 * handler's get / set / has traps when it has them ─────────────────── */
typedef struct { obj_t* target; obj_t* handler; } proxy_t;
static value_t proxy_hget(interp_t* I, obj_t* self, const char* key, int* found);
static int proxy_hset(interp_t* I, obj_t* self, const char* key, value_t v);
static const host_class_t proxy_class = { "Proxy", proxy_hget, proxy_hset };

static value_t proxy_trap(interp_t* I, proxy_t* px, const char* name) {
    value_t t = obj_get(I, px->handler, name);
    return t.t == V_FUNC ? t : v_undef();
}

static value_t proxy_hget(interp_t* I, obj_t* self, const char* key, int* found) {
    proxy_t* px = (proxy_t*)self->host;
    if (strcmp(key, "__tostring") == 0) return v_undef();     /* the engine's own hook, not a property */
    *found = 1;
    value_t tr = proxy_trap(I, px, "get");
    if (tr.t == V_FUNC) {
        value_t a[3] = { v_obj(px->target), v_str(I, key), v_obj(self) };
        return call_value(I, tr, v_obj(px->handler), 3, a);
    }
    return obj_get(I, px->target, key);
}

static int proxy_hset(interp_t* I, obj_t* self, const char* key, value_t v) {
    proxy_t* px = (proxy_t*)self->host;
    value_t tr = proxy_trap(I, px, "set");
    if (tr.t == V_FUNC) {
        value_t a[4] = { v_obj(px->target), v_str(I, key), v, v_obj(self) };
        call_value(I, tr, v_obj(px->handler), 4, a);
    } else {
        obj_set(I, px->target, key, v);
    }
    return 1;
}

/* the object a proxy stands for (NULL if o is not a proxy) */
obj_t* es_proxy_target(obj_t* o) {
    return (o->kind == OBJ_HOST && o->hc == &proxy_class) ? ((proxy_t*)o->host)->target : NULL;
}

/* key in proxy */
int es_proxy_has(interp_t* I, obj_t* o, const char* key) {
    proxy_t* px = (proxy_t*)o->host;
    value_t tr = proxy_trap(I, px, "has");
    if (tr.t == V_FUNC) {
        value_t a[2] = { v_obj(px->target), v_str(I, key) };
        return v_truthy(I, call_value(I, tr, v_obj(px->handler), 2, a));
    }
    if (obj_get(I, px->target, key).t != V_UNDEF) return 1;
    for (obj_t* p = px->target; p; p = p->proto) { int f = 0; prop_get_raw(p, key, &f); if (f) return 1; }
    return 0;
}

static value_t js_Proxy(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    if (argc < 2 || argv[1].t != V_OBJ || (argv[0].t != V_OBJ && argv[0].t != V_FUNC)) {
        script_throw(I, "TypeError: Cannot create proxy with a non-object as target or handler");
        return v_undef();
    }
    if (argv[0].t == V_FUNC) return argv[0];        /* function targets: no call traps, the function itself */
    proxy_t* px = (proxy_t*)arena_alloc(I->A, sizeof(proxy_t));
    px->target = argv[0].o;
    px->handler = argv[1].o;
    obj_t* o = obj_new(I, OBJ_HOST);
    o->hc = &proxy_class;
    o->host = px;
    return v_obj(o);
}

static value_t proxy_revoke(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)self; (void)argc; (void)argv;
    return v_undef();
}

static value_t js_Proxy_revocable(interp_t* I, value_t self, int argc, value_t* argv) {
    value_t p = js_Proxy(I, self, argc, argv);
    if (I->ctl) return v_undef();
    obj_t* r = obj_new(I, OBJ_PLAIN);
    obj_set(I, r, "proxy", p);
    obj_set(I, r, "revoke", v_native(I, "revoke", proxy_revoke));
    return v_obj(r);
}

int es_instanceof(interp_t* I, value_t v, value_t ctor) {
    if (ctor.t != V_FUNC) return 0;
    func_t* f = ctor.f;
    if (f->native) {
        const char* n = f->name ? f->name : "";
        if (strcmp(n, "Object") == 0) return v.t == V_OBJ || v.t == V_FUNC;
        if (strcmp(n, "Function") == 0) return v.t == V_FUNC;
        if (v.t != V_OBJ) return 0;
        int tr = typed_instanceof(v, n);             /* typed arrays, ArrayBuffer, DataView */
        if (tr >= 0) return tr;
        if (strcmp(n, "Array") == 0) return v.o->kind == OBJ_ARRAY;
        if (strcmp(n, "Date") == 0) return has_proto(v.o, I->proto_date);
        if (strcmp(n, "RegExp") == 0) return has_proto(v.o, I->proto_regexp);
        if (strcmp(n, "Promise") == 0) return has_proto(v.o, I->proto_promise);
        if (strcmp(n, "Map") == 0 || strcmp(n, "WeakMap") == 0) return has_proto(v.o, I->proto_map);
        if (strcmp(n, "Set") == 0 || strcmp(n, "WeakSet") == 0) return has_proto(v.o, I->proto_set);
        if (strstr(n, "Error")) {
            int f2 = 0;
            prop_get_raw(v.o, "message", &f2);
            if (!f2) return 0;
            if (strcmp(n, "Error") == 0) return 1;
            value_t nm = obj_get(I, v.o, "name");
            return nm.t == V_STR && strcmp(nm.s->s, n) == 0;
        }
        /* DOM classes (Element, Node, HTMLElement, ...): host objects */
        if (v.o->kind == OBJ_HOST && v.o->hc) return strstr(n, "Element") || strcmp(n, "Node") == 0 || strcmp(n, "EventTarget") == 0 ||
                                                       strcmp(v.o->hc->name, n) == 0;
        if (!ctor.f->statics) return 0;              /* else: by its prototype (class X extends HTMLElement) */
    }
    /* elements are event targets, whichever EventTarget the page has */
    if (v.t == V_OBJ && v.o->kind == OBJ_HOST && ctor.f->name && strcmp(ctor.f->name, "EventTarget") == 0) return 1;
    value_t proto = obj_getv(I, ctor, "prototype");
    if (proto.t != V_OBJ || v.t != V_OBJ) return 0;
    return has_proto(v.o, proto.o);
}

/* ══ RegExp ═══════════════════════════════════════════════════════════ */

static regex_t* rx_of(interp_t* I, value_t v) {
    if (v.t != V_OBJ || !I->proto_regexp || !has_proto(v.o, I->proto_regexp)) return NULL;
    return (regex_t*)v.o->host;
}

static int rx_flag(interp_t* I, value_t re, char f) {
    value_t fl = obj_get(I, re.o, "flags");
    return fl.t == V_STR && strchr(fl.s->s, f) != NULL;
}

value_t es_regexp_new(interp_t* I, const char* pattern, uint32_t len, const char* flags) {
    const char* err = NULL;
    regex_t* re = rx_compile(I->A, pattern, len, flags, &err);
    if (!re) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "SyntaxError: invalid regular expression /%.*s/: %s", (int)(len > 60 ? 60 : len), pattern, err);
        script_throw(I, msg);
        return v_undef();
    }
    obj_t* o = obj_new(I, OBJ_PLAIN);
    o->proto = I->proto_regexp;
    o->host = re;
    obj_set(I, o, "source", v_strn(I, pattern, len));
    obj_set(I, o, "flags", v_str(I, flags));
    obj_set(I, o, "global", v_bool(strchr(flags, 'g') != NULL));
    obj_set(I, o, "ignoreCase", v_bool(strchr(flags, 'i') != NULL));
    obj_set(I, o, "multiline", v_bool(strchr(flags, 'm') != NULL));
    obj_set(I, o, "sticky", v_bool(strchr(flags, 'y') != NULL));
    obj_set(I, o, "lastIndex", v_num(0));
    return v_obj(o);
}

static value_t js_RegExp(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t p = ARG(0);
    const char* flags = argc > 1 && argv[1].t != V_UNDEF ? v_cstr(I, argv[1]) : NULL;
    if (rx_of(I, p)) {
        value_t src = obj_get(I, p.o, "source");
        if (!flags) flags = v_cstr(I, obj_get(I, p.o, "flags"));
        return es_regexp_new(I, src.s->s, src.s->len, flags);
    }
    str_t* s = p.t == V_UNDEF ? str_new(I, "(?:)", 4) : v_tostr(I, p);
    return es_regexp_new(I, s->s, s->len, flags ? flags : "");
}

/* one match at or after `from`: the exec() array, or null */
static value_t match_obj(interp_t* I, regex_t* re, str_t* s, int* caps) {
    obj_t* a = obj_new(I, OBJ_ARRAY);
    int ng = rx_groups(re);
    obj_t* groups = NULL;
    for (int g = 0; g <= ng; g++) {
        value_t v = caps[2 * g] >= 0 ? v_strn(I, s->s + caps[2 * g], (uint32_t)(caps[2 * g + 1] - caps[2 * g])) : v_undef();
        arr_push(I, a, v);
        const char* nm = rx_group_name(re, g);
        if (g && nm) { if (!groups) groups = obj_new(I, OBJ_PLAIN); obj_set(I, groups, nm, v); }
    }
    obj_set(I, a, "index", v_num(caps[0]));
    obj_set(I, a, "input", v_strv(s));
    obj_set(I, a, "groups", groups ? v_obj(groups) : v_undef());
    return v_obj(a);
}

static value_t re_exec_at(interp_t* I, value_t rev, str_t* s, int* caps_out) {
    regex_t* re = rx_of(I, rev);
    int global = rx_flag(I, rev, 'g'), sticky = rx_flag(I, rev, 'y');
    uint32_t start = 0;
    if (global || sticky) {
        num_t li = v_tonum(I, obj_get(I, rev.o, "lastIndex"));
        if (li < 0 || li > s->len || li != li) { obj_set(I, rev.o, "lastIndex", v_num(0)); return v_null(); }
        start = (uint32_t)li;
    }
    int caps[2 * (RX_MAX_GROUPS + 1)];
    if (!rx_exec(re, s->s, s->len, start, sticky, caps)) {
        if (global || sticky) obj_set(I, rev.o, "lastIndex", v_num(0));
        return v_null();
    }
    if (global || sticky) obj_set(I, rev.o, "lastIndex", v_num(caps[1] == caps[0] && caps[1] == (int)start ? caps[1] : caps[1]));
    if (caps_out) memcpy(caps_out, caps, sizeof(caps));
    return match_obj(I, re, s, caps);
}

static value_t re_exec(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!rx_of(I, self)) { script_throw(I, "TypeError: exec on a non-RegExp"); return v_undef(); }
    return re_exec_at(I, self, v_tostr(I, ARG(0)), NULL);
}

static value_t re_test(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!rx_of(I, self)) return v_bool(0);
    return v_bool(re_exec_at(I, self, v_tostr(I, ARG(0)), NULL).t != V_NULL);
}

static value_t re_toString(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    if (self.t != V_OBJ) return v_str(I, "/(?:)/");
    char b[512];
    ksnprintf(b, sizeof(b), "/%s/%s", v_cstr(I, obj_get(I, self.o, "source")), v_cstr(I, obj_get(I, self.o, "flags")));
    return v_str(I, b);
}

/* ── string methods with a RegExp ── */

typedef struct { char* s; uint32_t n, cap; arena_t* A; } sb_t;
static void sb_add(sb_t* b, const char* s, uint32_t n) {
    if (b->n + n + 1 > b->cap) {
        uint32_t nc = (b->n + n + 1) * 2 + 64;
        char* ns = (char*)arena_alloc(b->A, nc);
        if (b->n) memcpy(ns, b->s, b->n);
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

/* the replacement for one match: a function's result, or the string with $& $1 $<n> $$ $` $' */
static void add_replacement(interp_t* I, sb_t* out, value_t rep, str_t* s, int* caps, int ng, regex_t* re) {
    if (rep.t == V_FUNC) {
        value_t args[RX_MAX_GROUPS + 4];
        int n = 0;
        for (int g = 0; g <= ng; g++)
            args[n++] = caps[2 * g] >= 0 ? v_strn(I, s->s + caps[2 * g], (uint32_t)(caps[2 * g + 1] - caps[2 * g])) : v_undef();
        args[n++] = v_num(caps[0]);
        args[n++] = v_strv(s);
        value_t r = call_value(I, rep, v_undef(), n, args);
        if (I->ctl) return;
        str_t* rs = v_tostr(I, r);
        sb_add(out, rs->s, rs->len);
        return;
    }
    str_t* t = v_tostr(I, rep);
    for (uint32_t i = 0; i < t->len; i++) {
        char c = t->s[i];
        if (c != '$' || i + 1 >= t->len) { sb_add(out, &c, 1); continue; }
        char d = t->s[i + 1];
        if (d == '$') { sb_add(out, "$", 1); i++; }
        else if (d == '&') { sb_add(out, s->s + caps[0], (uint32_t)(caps[1] - caps[0])); i++; }
        else if (d == '`') { sb_add(out, s->s, (uint32_t)caps[0]); i++; }
        else if (d == '\'') { sb_add(out, s->s + caps[1], s->len - (uint32_t)caps[1]); i++; }
        else if (d >= '0' && d <= '9') {
            int g = d - '0';
            uint32_t used = 1;
            if (i + 2 < t->len && t->s[i + 2] >= '0' && t->s[i + 2] <= '9' && g * 10 + (t->s[i + 2] - '0') <= ng) { g = g * 10 + (t->s[i + 2] - '0'); used = 2; }
            if (g >= 1 && g <= ng) {
                if (caps[2 * g] >= 0) sb_add(out, s->s + caps[2 * g], (uint32_t)(caps[2 * g + 1] - caps[2 * g]));
                i += used;
            } else sb_add(out, "$", 1);
        } else if (d == '<' && re) {
            const char* e = strchr(t->s + i + 2, '>');
            if (!e) { sb_add(out, "$", 1); continue; }
            uint32_t nl = (uint32_t)(e - (t->s + i + 2));
            for (int g = 1; g <= ng; g++) {
                const char* nm = rx_group_name(re, g);
                if (nm && strlen(nm) == nl && memcmp(nm, t->s + i + 2, nl) == 0 && caps[2 * g] >= 0)
                    sb_add(out, s->s + caps[2 * g], (uint32_t)(caps[2 * g + 1] - caps[2 * g]));
            }
            i = (uint32_t)(e - t->s);
        } else sb_add(out, "$", 1);
    }
}

static native_fn g_old_replace, g_old_replaceAll, g_old_split;

static value_t do_replace(interp_t* I, value_t self, int argc, value_t* argv, int all) {
    str_t* s = v_tostr(I, self);
    value_t pat = ARG(0), rep = ARG(1);
    sb_t out = { NULL, 0, 0, I->A };
    regex_t* re = rx_of(I, pat);
    if (!re) {
        /* a plain string pattern */
        if (rep.t != V_FUNC) return (all ? g_old_replaceAll : g_old_replace)(I, self, argc, argv);
        str_t* p = v_tostr(I, pat);
        uint32_t last = 0;
        for (uint32_t i = 0; i + p->len <= s->len; ) {
            if (memcmp(s->s + i, p->s, p->len) == 0) {
                int caps[2 * (RX_MAX_GROUPS + 1)];
                caps[0] = (int)i; caps[1] = (int)(i + p->len);
                sb_add(&out, s->s + last, i - last);
                add_replacement(I, &out, rep, s, caps, 0, NULL);
                if (I->ctl) return v_undef();
                last = i + p->len;
                i += p->len ? p->len : 1;
                if (!all) break;
            } else i++;
        }
        sb_add(&out, s->s + last, s->len - last);
        return v_strn(I, out.s ? out.s : "", out.n);
    }
    int global = rx_flag(I, pat, 'g') || all;
    int ng = rx_groups(re);
    uint32_t pos = 0, last = 0;
    int caps[2 * (RX_MAX_GROUPS + 1)];
    while (pos <= s->len && rx_exec(re, s->s, s->len, pos, 0, caps)) {
        sb_add(&out, s->s + last, (uint32_t)caps[0] - last);
        add_replacement(I, &out, rep, s, caps, ng, re);
        if (I->ctl) return v_undef();
        last = (uint32_t)caps[1];
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
        if (caps[1] == caps[0] && (uint32_t)caps[0] < s->len) {
            /* an empty match: keep the character it stood before */
        }
        if (!global) break;
    }
    if (last < s->len) sb_add(&out, s->s + last, s->len - last);
    if (rx_flag(I, pat, 'g')) obj_set(I, pat.o, "lastIndex", v_num(0));
    return v_strn(I, out.s ? out.s : "", out.n);
}

static value_t s_replace(interp_t* I, value_t self, int argc, value_t* argv) { return do_replace(I, self, argc, argv, 0); }
static value_t s_replaceAll(interp_t* I, value_t self, int argc, value_t* argv) { return do_replace(I, self, argc, argv, 1); }

/* a string argument becomes a RegExp (match, matchAll, search) */
static value_t as_regexp(interp_t* I, value_t p, const char* flags) {
    if (rx_of(I, p)) return p;
    str_t* s = p.t == V_UNDEF ? str_new(I, "(?:)", 4) : v_tostr(I, p);
    return es_regexp_new(I, s->s, s->len, flags);
}

static value_t s_match(interp_t* I, value_t self, int argc, value_t* argv) {
    str_t* s = v_tostr(I, self);
    value_t rev = as_regexp(I, ARG(0), "");
    if (I->ctl) return v_undef();
    if (!rx_flag(I, rev, 'g')) { obj_set(I, rev.o, "lastIndex", v_num(0)); return re_exec_at(I, rev, s, NULL); }
    regex_t* re = rx_of(I, rev);
    obj_t* a = obj_new(I, OBJ_ARRAY);
    int caps[2 * (RX_MAX_GROUPS + 1)];
    uint32_t pos = 0;
    while (pos <= s->len && rx_exec(re, s->s, s->len, pos, 0, caps)) {
        arr_push(I, a, v_strn(I, s->s + caps[0], (uint32_t)(caps[1] - caps[0])));
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
    }
    obj_set(I, rev.o, "lastIndex", v_num(0));
    return a->len ? v_obj(a) : v_null();
}

static value_t s_matchAll(interp_t* I, value_t self, int argc, value_t* argv) {
    str_t* s = v_tostr(I, self);
    value_t rev = as_regexp(I, ARG(0), "g");
    if (I->ctl) return v_undef();
    regex_t* re = rx_of(I, rev);
    obj_t* a = obj_new(I, OBJ_ARRAY);
    int caps[2 * (RX_MAX_GROUPS + 1)];
    uint32_t pos = 0;
    while (pos <= s->len && rx_exec(re, s->s, s->len, pos, 0, caps)) {
        arr_push(I, a, match_obj(I, re, s, caps));
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
    }
    return v_obj(a);
}

static value_t s_search(interp_t* I, value_t self, int argc, value_t* argv) {
    str_t* s = v_tostr(I, self);
    value_t rev = as_regexp(I, ARG(0), "");
    if (I->ctl) return v_undef();
    int caps[2 * (RX_MAX_GROUPS + 1)];
    return v_num(rx_exec(rx_of(I, rev), s->s, s->len, 0, 0, caps) ? caps[0] : -1);
}

static value_t s_split(interp_t* I, value_t self, int argc, value_t* argv) {
    regex_t* re = rx_of(I, ARG(0));
    if (!re) return g_old_split(I, self, argc, argv);
    str_t* s = v_tostr(I, self);
    uint32_t limit = argc > 1 && argv[1].t != V_UNDEF ? (uint32_t)v_tonum(I, argv[1]) : 0xFFFFFFFFu;
    obj_t* a = obj_new(I, OBJ_ARRAY);
    if (!s->len) { if (limit) arr_push(I, a, v_strv(s)); return v_obj(a); }
    int ng = rx_groups(re);
    int caps[2 * (RX_MAX_GROUPS + 1)];
    uint32_t last = 0, pos = 0;
    while (pos < s->len && a->len < limit && rx_exec(re, s->s, s->len, pos, 0, caps)) {
        if (caps[1] == caps[0]) {                      /* an empty match splits between characters */
            if ((uint32_t)caps[0] >= s->len) break;
            if ((uint32_t)caps[0] == last) { pos = (uint32_t)caps[0] + 1; if (pos > s->len) break; caps[0] = caps[1] = (int)pos; if (pos >= s->len) break; }
        }
        arr_push(I, a, v_strn(I, s->s + last, (uint32_t)caps[0] - last));
        for (int g = 1; g <= ng && a->len < limit; g++)
            arr_push(I, a, caps[2 * g] >= 0 ? v_strn(I, s->s + caps[2 * g], (uint32_t)(caps[2 * g + 1] - caps[2 * g])) : v_undef());
        last = (uint32_t)caps[1];
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
    }
    if (a->len < limit) arr_push(I, a, v_strn(I, s->s + last, s->len - last));
    return v_obj(a);
}

/* ══ Promise ══════════════════════════════════════════════════════════ */

#define P_STATE "\x01s"
#define P_VALUE "\x01v"
#define P_REACT "\x01r"

static int is_promise(interp_t* I, value_t v) {
    return v.t == V_OBJ && I->proto_promise && has_proto(v.o, I->proto_promise);
}

value_t es_promise_new(interp_t* I) {
    obj_t* o = obj_new(I, OBJ_PLAIN);
    o->proto = I->proto_promise;
    hidden_set(I, o, P_STATE, v_num(0));
    return v_obj(o);
}

int es_promise_state(interp_t* I, value_t p, value_t* out) {
    if (!is_promise(I, p)) return -1;
    int st = (int)v_tonum(I, hidden_get(p.o, P_STATE));
    if (out) *out = hidden_get(p.o, P_VALUE);
    return st;
}

/* a reaction job: {h: handler, c: child promise, k: 0 fulfil / 1 reject / 2 finally, v: value} */
static void queue_job(interp_t* I, value_t handler, value_t child, int kind, int rejected, value_t v) {
    if (!I->jobs) { I->jobs = obj_new(I, OBJ_ARRAY); I->job_head = 0; }
    obj_t* j = obj_new(I, OBJ_PLAIN);
    hidden_set(I, j, "h", handler);
    hidden_set(I, j, "c", child);
    hidden_set(I, j, "k", v_num(kind));
    hidden_set(I, j, "r", v_bool(rejected));
    hidden_set(I, j, "v", v);
    arr_push(I, I->jobs, v_obj(j));
}

static value_t resolver_fn(interp_t* I, value_t self, int argc, value_t* argv);

void es_promise_settle(interp_t* I, value_t p, int rejected, value_t v) {
    if (es_promise_state(I, p, NULL) != 0) return;
    if (!rejected && (v.t == V_OBJ || v.t == V_FUNC)) {
        if (v.t == V_OBJ && v.o == p.o) { rejected = 1; v = v_str(I, "TypeError: a promise resolved with itself"); }
        else {
            value_t then = obj_getv(I, v, "then");
            if (I->ctl == CTL_THROW) { v = I->ret; I->ctl = CTL_NONE; rejected = 1; }
            else if (then.t == V_FUNC) {
                /* adopt the thenable: it settles p through these two */
                obj_t* once = obj_new(I, OBJ_PLAIN);
                hidden_set(I, once, "p", p);
                hidden_set(I, once, "done", v_bool(0));
                value_t args[2] = { native(I, "resolve", resolver_fn, v_obj(once)), native(I, "reject", resolver_fn, v_obj(once)) };
                args[1].f->has_bound = 2;                     /* marks the rejecting one */
                call_value(I, then, v, 2, args);
                if (I->ctl == CTL_THROW) { value_t ex = I->ret; I->ctl = CTL_NONE; if (!v_truthy(I, hidden_get(once, "done"))) { hidden_set(I, once, "done", v_bool(1)); es_promise_settle(I, p, 1, ex); } }
                return;
            }
        }
    }
    hidden_set(I, p.o, P_STATE, v_num(rejected ? 2 : 1));
    hidden_set(I, p.o, P_VALUE, v);
    value_t rs = hidden_get(p.o, P_REACT);
    if (rs.t == V_OBJ) {
        for (uint32_t i = 0; i < rs.o->len; i++) {
            obj_t* r = rs.o->items[i].o;
            int kind = (int)v_tonum(I, hidden_get(r, "k"));
            value_t h = kind == 2 ? hidden_get(r, "f") : hidden_get(r, rejected ? "rej" : "ful");
            queue_job(I, h, hidden_get(r, "c"), kind, rejected, v);
        }
        hidden_set(I, p.o, P_REACT, v_undef());
    }
}

/* resolve/reject functions handed to executors and thenables */
static value_t resolver_fn(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    func_t* me = I->cur_native;
    value_t data = me ? me->data : v_undef();
    if (data.t != V_OBJ) return v_undef();
    obj_t* once = data.o;
    if (v_truthy(I, hidden_get(once, "done"))) return v_undef();
    hidden_set(I, once, "done", v_bool(1));
    es_promise_settle(I, hidden_get(once, "p"), me->has_bound == 2, ARG(0));
    return v_undef();
}

value_t es_promise_resolved(interp_t* I, value_t v) {
    if (is_promise(I, v)) return v;
    value_t p = es_promise_new(I);
    es_promise_settle(I, p, 0, v);
    return p;
}

static void add_reaction(interp_t* I, value_t p, value_t ful, value_t rej, value_t fin, value_t child, int kind) {
    value_t val;
    int st = es_promise_state(I, p, &val);
    if (st == 0) {
        value_t rs = hidden_get(p.o, P_REACT);
        if (rs.t != V_OBJ) { rs = v_obj(obj_new(I, OBJ_ARRAY)); hidden_set(I, p.o, P_REACT, rs); }
        obj_t* r = obj_new(I, OBJ_PLAIN);
        hidden_set(I, r, "ful", ful);
        hidden_set(I, r, "rej", rej);
        hidden_set(I, r, "f", fin);
        hidden_set(I, r, "c", child);
        hidden_set(I, r, "k", v_num(kind));
        arr_push(I, rs.o, v_obj(r));
    } else {
        queue_job(I, kind == 2 ? fin : (st == 2 ? rej : ful), child, kind, st == 2, val);
    }
}

static value_t pr_then(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!is_promise(I, self)) { script_throw(I, "TypeError: then on a non-promise"); return v_undef(); }
    value_t child = es_promise_new(I);
    add_reaction(I, self, ARG(0), ARG(1), v_undef(), child, 0);
    return child;
}

static value_t pr_catch(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!is_promise(I, self)) return v_undef();
    value_t child = es_promise_new(I);
    add_reaction(I, self, v_undef(), ARG(0), v_undef(), child, 0);
    return child;
}

static value_t pr_finally(interp_t* I, value_t self, int argc, value_t* argv) {
    if (!is_promise(I, self)) return v_undef();
    value_t child = es_promise_new(I);
    add_reaction(I, self, v_undef(), v_undef(), ARG(0), child, 2);
    return child;
}

void es_run_jobs(interp_t* I) {
    if (!I->jobs || I->draining) return;
    I->draining = 1;
    I->running++;                              /* handlers run nested: no recursive draining */
    int budget = 100000;
    while (I->job_head < I->jobs->len && budget-- > 0) {
        obj_t* j = I->jobs->items[I->job_head++].o;
        value_t h = hidden_get(j, "h"), child = hidden_get(j, "c"), v = hidden_get(j, "v");
        int kind = (int)v_tonum(I, hidden_get(j, "k"));
        int rejected = v_truthy(I, hidden_get(j, "r"));
        if (kind == 2) {                       /* finally: run, then pass the outcome on */
            if (h.t == V_FUNC) {
                value_t r;
                if (script_call(I, h, v_undef(), 0, NULL, &r) != 0) { es_promise_settle(I, child, 1, v_str(I, script_error(I))); continue; }
            }
            es_promise_settle(I, child, rejected, v);
            continue;
        }
        if (h.t != V_FUNC) { es_promise_settle(I, child, rejected, v); continue; }
        value_t r;
        if (script_call(I, h, v_undef(), 1, &v, &r) != 0) {
            /* the handler threw: its exception (as the error message object) rejects the child */
            obj_t* e = obj_new(I, OBJ_PLAIN);
            obj_set(I, e, "message", v_str(I, script_error(I)));
            obj_set(I, e, "name", v_str(I, "Error"));
            if (I->log) I->log(I->log_ctx, script_error(I), (uint32_t)strlen(script_error(I)));
            es_promise_settle(I, child, 1, v_obj(e));
        } else {
            es_promise_settle(I, child, 0, r);
        }
    }
    if (I->job_head >= I->jobs->len) { I->jobs->len = 0; I->job_head = 0; }
    I->running--;
    I->draining = 0;
}

static value_t js_Promise(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t p = es_promise_new(I);
    value_t ex = ARG(0);
    if (ex.t != V_FUNC) { script_throw(I, "TypeError: Promise needs an executor function"); return v_undef(); }
    obj_t* once = obj_new(I, OBJ_PLAIN);
    hidden_set(I, once, "p", p);
    hidden_set(I, once, "done", v_bool(0));
    value_t args[2] = { native(I, "resolve", resolver_fn, v_obj(once)), native(I, "reject", resolver_fn, v_obj(once)) };
    args[1].f->has_bound = 2;
    call_value(I, ex, v_undef(), 2, args);
    if (I->ctl == CTL_THROW) {
        value_t e = I->ret;
        I->ctl = CTL_NONE;
        if (!v_truthy(I, hidden_get(once, "done"))) { hidden_set(I, once, "done", v_bool(1)); es_promise_settle(I, p, 1, e); }
    }
    return p;
}

static value_t pr_resolve(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return es_promise_resolved(I, ARG(0)); }
static value_t pr_reject(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t p = es_promise_new(I);
    es_promise_settle(I, p, 1, ARG(0));
    return p;
}

/* Promise.all / allSettled / race / any - over promises that may already be
 * settled; pending ones are followed by reactions */
static value_t combinator_step(interp_t* I, value_t self, int argc, value_t* argv);

static value_t combine(interp_t* I, int argc, value_t* argv, int mode) {     /* 0 all, 1 allSettled, 2 race, 3 any */
    value_t result = es_promise_new(I);
    obj_t* items = es_to_array(I, ARG(0));
    if (!items) { es_promise_settle(I, result, 1, v_str(I, "TypeError: not iterable")); return result; }
    obj_t* st = obj_new(I, OBJ_PLAIN);                      /* shared state */
    obj_t* out = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < items->len; i++) arr_push(I, out, v_undef());
    hidden_set(I, st, "out", v_obj(out));
    hidden_set(I, st, "left", v_num(items->len));
    hidden_set(I, st, "res", result);
    hidden_set(I, st, "mode", v_num(mode));
    if (!items->len) {
        if (mode == 3) es_promise_settle(I, result, 1, v_str(I, "AggregateError: all promises were rejected"));
        else if (mode != 2) es_promise_settle(I, result, 0, v_obj(out));
        return result;
    }
    for (uint32_t i = 0; i < items->len; i++) {
        value_t p = es_promise_resolved(I, items->items[i]);
        obj_t* d1 = obj_new(I, OBJ_PLAIN);
        hidden_set(I, d1, "st", v_obj(st)); hidden_set(I, d1, "i", v_num(i)); hidden_set(I, d1, "rej", v_bool(0));
        obj_t* d2 = obj_new(I, OBJ_PLAIN);
        hidden_set(I, d2, "st", v_obj(st)); hidden_set(I, d2, "i", v_num(i)); hidden_set(I, d2, "rej", v_bool(1));
        add_reaction(I, p, native(I, "step", combinator_step, v_obj(d1)), native(I, "step", combinator_step, v_obj(d2)),
                     v_undef(), es_promise_new(I), 0);
    }
    return result;
}

static value_t combinator_step(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* d = I->cur_native->data.o;
    obj_t* st = hidden_get(d, "st").o;
    int i = (int)v_tonum(I, hidden_get(d, "i"));
    int rej = v_truthy(I, hidden_get(d, "rej"));
    int mode = (int)v_tonum(I, hidden_get(st, "mode"));
    value_t res = hidden_get(st, "res");
    obj_t* out = hidden_get(st, "out").o;
    value_t v = ARG(0);
    if (mode == 2) { es_promise_settle(I, res, rej, v); return v_undef(); }
    if (mode == 0 && rej) { es_promise_settle(I, res, 1, v); return v_undef(); }
    if (mode == 3 && !rej) { es_promise_settle(I, res, 0, v); return v_undef(); }
    if (mode == 1) {
        obj_t* r = obj_new(I, OBJ_PLAIN);
        obj_set(I, r, "status", v_str(I, rej ? "rejected" : "fulfilled"));
        obj_set(I, r, rej ? "reason" : "value", v);
        v = v_obj(r);
    }
    out->items[i] = v;
    int left = (int)v_tonum(I, hidden_get(st, "left")) - 1;
    hidden_set(I, st, "left", v_num(left));
    if (left == 0) {
        if (mode == 3) es_promise_settle(I, res, 1, v_str(I, "AggregateError: all promises were rejected"));
        else es_promise_settle(I, res, 0, v_obj(out));
    }
    return v_undef();
}

static value_t pr_all(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return combine(I, argc, argv, 0); }
static value_t pr_allSettled(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return combine(I, argc, argv, 1); }
static value_t pr_race(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return combine(I, argc, argv, 2); }
static value_t pr_any(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return combine(I, argc, argv, 3); }

static value_t js_queueMicrotask(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    if (ARG(0).t == V_FUNC) queue_job(I, ARG(0), es_promise_new(I), 0, 0, v_undef());
    return v_undef();
}

/* ══ Map / Set ════════════════════════════════════════════════════════ */

static obj_t* coll_keys(interp_t* I, value_t self) {
    if (self.t != V_OBJ) return NULL;
    value_t k = hidden_get(self.o, "\x01k");
    if (k.t != V_OBJ) { script_throw(I, "TypeError: not a Map/Set"); return NULL; }
    return k.o;
}
static obj_t* coll_vals(value_t self) { value_t v = hidden_get(self.o, "\x01v"); return v.t == V_OBJ ? v.o : NULL; }

static int same_value_zero(value_t a, value_t b) {
    if (a.t == V_NUM && b.t == V_NUM && a.n != a.n && b.n != b.n) return 1;   /* NaN */
    return v_strict_eq(a, b);
}

static int coll_find(obj_t* keys, value_t k) {
    for (uint32_t i = 0; i < keys->len; i++) if (same_value_zero(keys->items[i], k)) return (int)i;
    return -1;
}

static void coll_remove(obj_t* a, uint32_t i) {
    if (!a) return;
    memmove(&a->items[i], &a->items[i + 1], (a->len - i - 1) * sizeof(value_t));
    a->len--;
}

static value_t new_coll(interp_t* I, obj_t* proto, int is_map, value_t init) {
    obj_t* o = obj_new(I, OBJ_PLAIN);
    o->proto = proto;
    hidden_set(I, o, "\x01k", v_obj(obj_new(I, OBJ_ARRAY)));
    if (is_map) hidden_set(I, o, "\x01v", v_obj(obj_new(I, OBJ_ARRAY)));
    if (init.t != V_UNDEF && init.t != V_NULL) {
        obj_t* items = es_to_array(I, init);
        obj_t* keys = hidden_get(o, "\x01k").o;
        obj_t* vals = is_map ? hidden_get(o, "\x01v").o : NULL;
        for (uint32_t i = 0; items && i < items->len; i++) {
            value_t it = items->items[i];
            value_t k = it, v = v_undef();
            if (is_map) {
                if (it.t != V_OBJ) continue;
                k = obj_get(I, it.o, "0");
                v = obj_get(I, it.o, "1");
            }
            int at = coll_find(keys, k);
            if (at >= 0) { if (vals) vals->items[at] = v; continue; }
            arr_push(I, keys, k);
            if (vals) arr_push(I, vals, v);
        }
    }
    return v_obj(o);
}

static value_t js_Map(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return new_coll(I, I->proto_map, 1, ARG(0)); }
static value_t js_Set(interp_t* I, value_t self, int argc, value_t* argv) { (void)self; return new_coll(I, I->proto_set, 0, ARG(0)); }

static value_t m_get(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    if (!k) return v_undef();
    int i = coll_find(k, ARG(0));
    return i >= 0 ? coll_vals(self)->items[i] : v_undef();
}
static value_t m_set(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    if (!k) return v_undef();
    int i = coll_find(k, ARG(0));
    if (i >= 0) coll_vals(self)->items[i] = ARG(1);
    else { arr_push(I, k, ARG(0)); arr_push(I, coll_vals(self), ARG(1)); }
    return self;
}
static value_t c_has(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    return v_bool(k && coll_find(k, ARG(0)) >= 0);
}
static value_t c_delete(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    if (!k) return v_bool(0);
    int i = coll_find(k, ARG(0));
    if (i < 0) return v_bool(0);
    coll_remove(k, (uint32_t)i);
    coll_remove(coll_vals(self), (uint32_t)i);
    return v_bool(1);
}
static value_t c_clear(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* k = coll_keys(I, self);
    if (k) { k->len = 0; if (coll_vals(self)) coll_vals(self)->len = 0; }
    return v_undef();
}
static value_t c_size(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* k = coll_keys(I, self);
    return v_num(k ? k->len : 0);
}
static value_t s_add(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    if (k && coll_find(k, ARG(0)) < 0) arr_push(I, k, ARG(0));
    return self;
}
static value_t c_forEach(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* k = coll_keys(I, self);
    if (!k || ARG(0).t != V_FUNC) return v_undef();
    obj_t* vals = coll_vals(self);
    for (uint32_t i = 0; i < k->len && !I->ctl; i++) {
        value_t args[3] = { vals ? vals->items[i] : k->items[i], k->items[i], self };
        call_value(I, ARG(0), ARG(1), 3, args);
    }
    return v_undef();
}
static value_t c_keys(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* k = coll_keys(I, self);
    obj_t* a = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; k && i < k->len; i++) arr_push(I, a, k->items[i]);
    return v_obj(a);
}
static value_t c_values(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* vals = coll_keys(I, self) ? coll_vals(self) : NULL;
    if (!vals) return c_keys(I, self, argc, argv);           /* a Set's values are its keys */
    obj_t* a = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < vals->len; i++) arr_push(I, a, vals->items[i]);
    return v_obj(a);
}
static value_t c_entries(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* r = es_to_array(I, self);
    if (r && coll_keys(I, self) && !coll_vals(self)) {       /* Set: [v, v] */
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (uint32_t i = 0; i < r->len; i++) {
            obj_t* pair = obj_new(I, OBJ_ARRAY);
            arr_push(I, pair, r->items[i]);
            arr_push(I, pair, r->items[i]);
            arr_push(I, a, v_obj(pair));
        }
        return v_obj(a);
    }
    return r ? v_obj(r) : v_undef();
}

/* ══ Symbol ═══════════════════════════════════════════════════════════ */

static value_t js_Symbol(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    static uint32_t serial;
    char b[128];
    ksnprintf(b, sizeof(b), "@@sym:%s:%u", argc && argv[0].t != V_UNDEF ? v_cstr(I, argv[0]) : "", ++serial);
    return v_str(I, b);
}
static value_t sym_for(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    char b[128];
    ksnprintf(b, sizeof(b), "@@sym:%s", v_cstr(I, ARG(0)));
    return v_str(I, b);
}

/* ══ Function.prototype.bind ══════════════════════════════════════════ */

static value_t bound_call(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* d = I->cur_native->data.o;
    value_t fn = hidden_get(d, "f"), t = hidden_get(d, "t");
    obj_t* pre = hidden_get(d, "a").o;
    int n = (int)pre->len + argc;
    value_t* args = (value_t*)arena_alloc(I->A, (uint32_t)(n + 1) * (uint32_t)sizeof(value_t));
    for (uint32_t i = 0; i < pre->len; i++) args[i] = pre->items[i];
    for (int i = 0; i < argc; i++) args[pre->len + (uint32_t)i] = argv[i];
    if (is_new_call(self)) t = self;
    return call_value(I, fn, t, n, args);
}

static value_t f_bind(interp_t* I, value_t self, int argc, value_t* argv) {
    if (self.t != V_FUNC) { script_throw(I, "TypeError: bind on a non-function"); return v_undef(); }
    obj_t* d = obj_new(I, OBJ_PLAIN);
    hidden_set(I, d, "f", self);
    hidden_set(I, d, "t", ARG(0));
    obj_t* pre = obj_new(I, OBJ_ARRAY);
    for (int i = 1; i < argc; i++) arr_push(I, pre, argv[i]);
    hidden_set(I, d, "a", v_obj(pre));
    return native(I, self.f->name ? self.f->name : "bound", bound_call, v_obj(d));
}

/* ══ Object / Array / String / Number / Math additions ═══════════════ */

static value_t o_defineProperties(interp_t* I, value_t self, int argc, value_t* argv);
static value_t o_create(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* o = obj_new(I, OBJ_PLAIN);
    if (ARG(0).t == V_OBJ) o->proto = ARG(0).o;
    /* Object.create(proto, { name: descriptor, ... }) */
    if (ARG(1).t == V_OBJ) { value_t a[2] = { v_obj(o), ARG(1) }; o_defineProperties(I, self, 2, a); }
    return v_obj(o);
}
static value_t o_getPrototypeOf(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t v = ARG(0);
    if (v.t == V_OBJ && v.o->proto) return v_obj(v.o->proto);
    if (v.t == V_OBJ && v.o->kind == OBJ_ARRAY && I->proto_array) return v_obj(I->proto_array);
    return v_null();
}
static value_t o_setPrototypeOf(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self; (void)I;
    if (ARG(0).t == V_OBJ) ARG(0).o->proto = ARG(1).t == V_OBJ ? ARG(1).o : NULL;
    return ARG(0);
}
static void define_prop(interp_t* I, obj_t* o, const char* key, value_t desc) {
    if (desc.t != V_OBJ) return;
    value_t g = obj_get(I, desc.o, "get"), s = obj_get(I, desc.o, "set");
    if (g.t == V_FUNC || s.t == V_FUNC) {
        obj_t* acc = obj_new(I, OBJ_PLAIN);
        acc->kind = OBJ_ACCESSOR;
        if (g.t == V_FUNC) obj_set(I, acc, "get", g);
        if (s.t == V_FUNC) obj_set(I, acc, "set", s);
        prop_set_raw(I, o, str_new(I, key, (uint32_t)strlen(key)), v_obj(acc));
        return;
    }
    int f = 0;
    value_t v = prop_get_raw(desc.o, "value", &f);
    if (f) obj_set(I, o, key, v);
}
static value_t o_defineProperty(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t o = ARG(0);
    if (o.t == V_FUNC) { define_prop(I, statics_of(I, o), v_cstr(I, ARG(1)), ARG(2)); return o; }
    if (o.t != V_OBJ) { script_throw(I, "TypeError: defineProperty on a non-object"); return v_undef(); }
    define_prop(I, o.o, v_cstr(I, ARG(1)), ARG(2));
    return o;
}
static value_t o_defineProperties(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t o = ARG(0), props = ARG(1);
    if (o.t != V_OBJ || props.t != V_OBJ) return o;
    for (uint32_t i = 0; i < props.o->n; i++) define_prop(I, o.o, props.o->props[i].key->s, props.o->props[i].v);
    return o;
}
static value_t o_getOwnPropertyDescriptor(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    value_t o = ARG(0);
    if (o.t != V_OBJ) return v_undef();
    int f = 0;
    value_t v = prop_get_raw(o.o, v_cstr(I, ARG(1)), &f);
    if (!f) return v_undef();
    obj_t* d = obj_new(I, OBJ_PLAIN);
    if (v.t == V_OBJ && v.o->kind == OBJ_ACCESSOR) {
        obj_set(I, d, "get", obj_get(I, v.o, "get"));
        obj_set(I, d, "set", obj_get(I, v.o, "set"));
    } else {
        obj_set(I, d, "value", v);
        obj_set(I, d, "writable", v_bool(1));
    }
    obj_set(I, d, "enumerable", v_bool(1));
    obj_set(I, d, "configurable", v_bool(1));
    return v_obj(d);
}
static value_t o_identity(interp_t* I, value_t self, int argc, value_t* argv) { (void)I; (void)self; return ARG(0); }
static value_t o_false(interp_t* I, value_t self, int argc, value_t* argv) { (void)I; (void)self; (void)argc; (void)argv; return v_bool(0); }
static value_t o_fromEntries(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* o = obj_new(I, OBJ_PLAIN);
    obj_t* items = es_to_array(I, ARG(0));
    for (uint32_t i = 0; items && i < items->len; i++) {
        value_t it = items->items[i];
        if (it.t != V_OBJ) continue;
        obj_set(I, o, v_cstr(I, obj_get(I, it.o, "0")), obj_get(I, it.o, "1"));
    }
    return v_obj(o);
}
static value_t o_is(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)self;
    return v_bool(same_value_zero(ARG(0), ARG(1)));
}

static value_t a_from(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* src = es_to_array(I, ARG(0));
    obj_t* a = obj_new(I, OBJ_ARRAY);
    if (!src) return v_obj(a);
    value_t fn = ARG(1);
    for (uint32_t i = 0; i < src->len && !I->ctl; i++) {
        value_t v = src->items[i];
        if (fn.t == V_FUNC) { value_t args[2] = { v, v_num(i) }; v = call_value(I, fn, ARG(2), 2, args); }
        arr_push(I, a, v);
    }
    return v_obj(a);
}
static value_t a_of(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    obj_t* a = obj_new(I, OBJ_ARRAY);
    for (int i = 0; i < argc; i++) arr_push(I, a, argv[i]);
    return v_obj(a);
}

static obj_t* self_arr(interp_t* I, value_t self) {
    if (self.t != V_OBJ || self.o->kind != OBJ_ARRAY) { script_throw(I, "TypeError: not an array"); return NULL; }
    return self.o;
}

static void flatten(interp_t* I, obj_t* out, obj_t* in, int depth) {
    for (uint32_t i = 0; i < in->len; i++) {
        value_t v = in->items[i];
        if (depth > 0 && v.t == V_OBJ && v.o->kind == OBJ_ARRAY) flatten(I, out, v.o, depth - 1);
        else arr_push(I, out, v);
    }
}
static value_t a_flat(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a) return v_undef();
    int depth = argc && argv[0].t != V_UNDEF ? (int)v_tonum(I, argv[0]) : 1;
    obj_t* out = obj_new(I, OBJ_ARRAY);
    flatten(I, out, a, depth > 64 ? 64 : depth);
    return v_obj(out);
}
static value_t a_flatMap(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a || ARG(0).t != V_FUNC) return v_undef();
    obj_t* out = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; i < a->len && !I->ctl; i++) {
        value_t args[3] = { a->items[i], v_num(i), self };
        value_t r = call_value(I, ARG(0), ARG(1), 3, args);
        if (r.t == V_OBJ && r.o->kind == OBJ_ARRAY) for (uint32_t k = 0; k < r.o->len; k++) arr_push(I, out, r.o->items[k]);
        else arr_push(I, out, r);
    }
    return v_obj(out);
}
static value_t a_lastIndexOf(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a) return v_num(-1);
    for (uint32_t i = a->len; i > 0; i--) if (v_strict_eq(a->items[i - 1], ARG(0))) return v_num(i - 1);
    return v_num(-1);
}
static value_t a_findLast(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a || ARG(0).t != V_FUNC) return v_undef();
    for (uint32_t i = a->len; i > 0 && !I->ctl; i--) {
        value_t args[3] = { a->items[i - 1], v_num(i - 1), self };
        if (v_truthy(I, call_value(I, ARG(0), ARG(1), 3, args))) return a->items[i - 1];
    }
    return v_undef();
}
static value_t a_findLastIndex(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a || ARG(0).t != V_FUNC) return v_num(-1);
    for (uint32_t i = a->len; i > 0 && !I->ctl; i--) {
        value_t args[3] = { a->items[i - 1], v_num(i - 1), self };
        if (v_truthy(I, call_value(I, ARG(0), ARG(1), 3, args))) return v_num(i - 1);
    }
    return v_num(-1);
}
static value_t a_reduceRight(interp_t* I, value_t self, int argc, value_t* argv) {
    obj_t* a = self_arr(I, self);
    if (!a || ARG(0).t != V_FUNC) return v_undef();
    uint32_t i = a->len;
    value_t acc;
    if (argc > 1) acc = argv[1];
    else { if (!i) { script_throw(I, "TypeError: reduce of an empty array"); return v_undef(); } acc = a->items[--i]; }
    for (; i > 0 && !I->ctl; i--) {
        value_t args[4] = { acc, a->items[i - 1], v_num(i - 1), self };
        acc = call_value(I, ARG(0), v_undef(), 4, args);
    }
    return acc;
}
static value_t a_keys(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* a = self_arr(I, self);
    obj_t* out = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; a && i < a->len; i++) arr_push(I, out, v_num(i));
    return v_obj(out);
}
static value_t a_values(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; (void)I; return self; }
static value_t a_entries(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)argc; (void)argv;
    obj_t* a = self_arr(I, self);
    obj_t* out = obj_new(I, OBJ_ARRAY);
    for (uint32_t i = 0; a && i < a->len; i++) {
        obj_t* pair = obj_new(I, OBJ_ARRAY);
        arr_push(I, pair, v_num(i));
        arr_push(I, pair, a->items[i]);
        arr_push(I, out, v_obj(pair));
    }
    return v_obj(out);
}

static value_t s_codePointAt(interp_t* I, value_t self, int argc, value_t* argv) {
    str_t* s = v_tostr(I, self);
    num_t i = argc ? v_tonum(I, argv[0]) : 0;
    if (i < 0 || i >= s->len) return v_undef();
    const unsigned char* p = (const unsigned char*)s->s + (uint32_t)i;
    uint32_t c = p[0];
    if (c >= 0xF0 && i + 3 < s->len) c = (c & 7) << 18 | (p[1] & 63u) << 12 | (p[2] & 63u) << 6 | (p[3] & 63u);
    else if (c >= 0xE0 && i + 2 < s->len) c = (c & 15) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u);
    else if (c >= 0xC0 && i + 1 < s->len) c = (c & 31) << 6 | (p[1] & 63u);
    return v_num(c);
}
static value_t s_localeCompare(interp_t* I, value_t self, int argc, value_t* argv) {
    str_t* a = v_tostr(I, self);
    str_t* b = v_tostr(I, ARG(0));
    int c = strcasecmp(a->s, b->s);
    if (!c) c = strcmp(a->s, b->s);
    return v_num(c < 0 ? -1 : c > 0 ? 1 : 0);
}
static value_t s_normalize(interp_t* I, value_t self, int argc, value_t* argv) { (void)argc; (void)argv; return v_strv(v_tostr(I, self)); }

static value_t n_isInteger(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)self;
    value_t v = ARG(0);
    if (v.t != V_NUM || v.n != v.n) return v_bool(0);
    num_t x = v.n;
    if (x > 9e18L || x < -9e18L) return v_bool(0);
    return v_bool((num_t)(int64_t)x == x);
}
static value_t n_isFinite(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)I; (void)self;
    value_t v = ARG(0);
    return v_bool(v.t == V_NUM && v.n == v.n && v.n < 1e308L && v.n > -1e308L);
}

/* x87 helpers */
static num_t x87_atan2(num_t y, num_t x) { num_t r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static num_t x87_log2(num_t x) { num_t r; __asm__("fld1\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
static num_t x87_sqrt(num_t x) { num_t r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static num_t x87_sin(num_t x) { num_t r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static num_t x87_cos(num_t x) { num_t r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static num_t num_arg(interp_t* I, int argc, value_t* argv, int i) { return i < argc ? v_tonum(I, argv[i]) : (num_t)0 / (num_t)0; }

static value_t m_atan2(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_atan2(num_arg(I, argc, argv, 0), num_arg(I, argc, argv, 1))); }
static value_t m_atan(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_atan2(num_arg(I, argc, argv, 0), 1)); }
static value_t m_tan(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = num_arg(I, argc, argv, 0); return v_num(x87_sin(x) / x87_cos(x)); }
static value_t m_asin(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = num_arg(I, argc, argv, 0); return v_num(x87_atan2(x, x87_sqrt(1 - x * x))); }
static value_t m_acos(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; num_t x = num_arg(I, argc, argv, 0); return v_num(x87_atan2(x87_sqrt(1 - x * x), x)); }
static value_t m_log2(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_log2(num_arg(I, argc, argv, 0))); }
static value_t m_log10(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(x87_log2(num_arg(I, argc, argv, 0)) * 0.30102999566398119521L); }
static value_t m_hypot(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t t = 0;
    for (int i = 0; i < argc; i++) { num_t x = v_tonum(I, argv[i]); t += x * x; }
    return v_num(x87_sqrt(t));
}
static value_t m_cbrt(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    num_t x = num_arg(I, argc, argv, 0);
    if (x == 0 || x != x) return v_num(x);
    num_t a = x < 0 ? -x : x, r = a > 1 ? a / 3 : a;
    for (int k = 0; k < 80; k++) r = r - (r * r * r - a) / (3 * r * r);
    return v_num(x < 0 ? -r : r);
}
static value_t m_fround(interp_t* I, value_t s, int argc, value_t* argv) { (void)s; return v_num(num_arg(I, argc, argv, 0)); }
static value_t m_imul(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    int32_t a = (int32_t)(int64_t)num_arg(I, argc, argv, 0), b = (int32_t)(int64_t)num_arg(I, argc, argv, 1);
    return v_num((int32_t)((uint32_t)a * (uint32_t)b));
}
static value_t m_clz32(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    uint32_t x = (uint32_t)(int64_t)num_arg(I, argc, argv, 0);
    int n = 0;
    while (n < 32 && !(x & 0x80000000u)) { x <<= 1; n++; }
    return v_num(n);
}

/* ── globals ── */

static value_t js_performance_now(interp_t* I, value_t s, int argc, value_t* argv) { (void)I; (void)s; (void)argc; (void)argv; return v_num(timer_ms()); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static value_t js_btoa(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    str_t* in = v_tostr(I, ARG(0));
    uint32_t n = in->len, o = 0;
    char* out = (char*)arena_alloc(I->A, (n + 2) / 3 * 4 + 1);
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)in->s[i] << 16 | (i + 1 < n ? (uint32_t)(unsigned char)in->s[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in->s[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    return v_strn(I, out, o);
}
static value_t js_atob(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    str_t* in = v_tostr(I, ARG(0));
    char* out = (char*)arena_alloc(I->A, in->len + 1);
    uint32_t o = 0, acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < in->len; i++) {
        const char* p = strchr(B64, in->s[i]);
        if (!p || !in->s[i]) continue;
        acc = acc << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)(acc >> bits & 255); }
    }
    return v_strn(I, out, o);
}
static value_t js_encodeURI(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    static const char hx[] = "0123456789ABCDEF";
    str_t* in = v_tostr(I, ARG(0));
    char* out = (char*)arena_alloc(I->A, in->len * 3 + 1);
    uint32_t o = 0;
    for (uint32_t i = 0; i < in->len; i++) {
        unsigned char c = (unsigned char)in->s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'();/?:@&=+$,#", c)) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    return v_strn(I, out, o);
}
/* new Function("a", "b", "return a + b"): compiled at global scope */
static value_t run_for_value(interp_t* I, const char* src, uint32_t len, const char* name) {
    script_def_global(I, "__banana_eval_result", v_undef());
    if (script_run(I, src, len, name) != 0) {
        char msg[200];
        kstrlcpy(msg, I->err, sizeof(msg));
        script_throw(I, msg);
        return v_undef();
    }
    value_t r = script_get_global(I, "__banana_eval_result");
    script_def_global(I, "__banana_eval_result", v_undef());
    return r;
}
static value_t js_global_this(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s; (void)argc; (void)argv;
    return script_get_global(I, "globalThis");
}
static value_t js_Function(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    /* Function("return this")(): the usual way to find the global object, which
     * plain calls here do not pass as `this` */
    if (argc == 1 && argv[0].t == V_STR) {
        const char* b = argv[0].s->s;
        while (*b == ' ' || *b == '\n') b++;
        if (strncmp(b, "return this", 11) == 0) {
            const char* e = b + 11;
            while (*e == ' ' || *e == ';' || *e == '\n') e++;
            if (!*e) return v_native(I, "anonymous", js_global_this);
        }
    }
    sb_t b = { NULL, 0, 0, I->A };
    sb_add(&b, "__banana_eval_result = (function anonymous(", 43);
    for (int i = 0; i + 1 < argc; i++) {
        str_t* a = v_tostr(I, argv[i]);
        if (i) sb_add(&b, ",", 1);
        sb_add(&b, a->s, a->len);
    }
    sb_add(&b, "\n) {\n", 5);
    if (argc) { str_t* body = v_tostr(I, argv[argc - 1]); sb_add(&b, body->s, body->len); }
    sb_add(&b, "\n});", 4);
    return run_for_value(I, b.s, b.n, "Function");
}
/* eval(code): run at global scope; the value is that of a lone expression, else undefined */
static value_t js_eval(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    if (!argc || argv[0].t != V_STR) return argc ? argv[0] : v_undef();
    str_t* code = argv[0].s;
    sb_t b = { NULL, 0, 0, I->A };
    sb_add(&b, "__banana_eval_result = (", 24);
    sb_add(&b, code->s, code->len);
    sb_add(&b, "\n);", 3);
    /* try it as an expression first; statements are run as they are */
    char saved[200];
    kstrlcpy(saved, I->err, sizeof(saved));
    script_def_global(I, "__banana_eval_result", v_undef());
    if (script_run(I, b.s, b.n, "eval") == 0) {
        value_t r = script_get_global(I, "__banana_eval_result");
        script_def_global(I, "__banana_eval_result", v_undef());
        return r;
    }
    if (strncmp(I->err, "SyntaxError", 11) != 0) {     /* it ran and threw */
        char msg[200];
        kstrlcpy(msg, I->err, sizeof(msg));
        script_throw(I, msg);
        return v_undef();
    }
    kstrlcpy(I->err, saved, sizeof(I->err));
    run_for_value(I, code->s, code->len, "eval");
    return v_undef();
}

/* tagged templates: the strings array also carries .raw */
static value_t js_tplstrings(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    if (argc && argv[0].t == V_OBJ) obj_set(I, argv[0].o, "raw", argc > 1 ? argv[1] : argv[0]);
    return argc ? argv[0] : v_undef();
}

static value_t js_structuredClone(interp_t* I, value_t s, int argc, value_t* argv) {
    (void)s;
    value_t json = script_get_global(I, "JSON");
    if (json.t != V_OBJ) return ARG(0);
    value_t st = obj_get(I, json.o, "stringify"), pa = obj_get(I, json.o, "parse");
    value_t a0 = ARG(0);
    value_t t = call_value(I, st, json, 1, &a0);
    if (I->ctl || t.t != V_STR) return v_undef();
    return call_value(I, pa, json, 1, &t);
}

/* ══ init ═════════════════════════════════════════════════════════════ */

void es_init(interp_t* I) {
    typed_init(I);                                   /* script_typed.c */
    /* RegExp */
    I->proto_regexp = obj_new(I, OBJ_PLAIN);
    method(I, I->proto_regexp, "exec", re_exec);
    method(I, I->proto_regexp, "test", re_test);
    method(I, I->proto_regexp, "toString", re_toString);
    value_t rx = v_native(I, "RegExp", js_RegExp);
    obj_set(I, statics_of(I, rx), "prototype", v_obj(I->proto_regexp));
    script_def_global(I, "RegExp", rx);

    /* the string methods that also take a RegExp (the old ones handle strings) */
    int f = 0;
    value_t old = prop_get_raw(I->proto_string, "replace", &f);
    if (f && old.t == V_FUNC) g_old_replace = old.f->nf;
    old = prop_get_raw(I->proto_string, "replaceAll", &f);
    if (f && old.t == V_FUNC) g_old_replaceAll = old.f->nf;
    old = prop_get_raw(I->proto_string, "split", &f);
    if (f && old.t == V_FUNC) g_old_split = old.f->nf;
    method(I, I->proto_string, "replace", s_replace);
    method(I, I->proto_string, "replaceAll", s_replaceAll);
    method(I, I->proto_string, "split", s_split);
    method(I, I->proto_string, "match", s_match);
    method(I, I->proto_string, "matchAll", s_matchAll);
    method(I, I->proto_string, "search", s_search);
    method(I, I->proto_string, "codePointAt", s_codePointAt);
    method(I, I->proto_string, "localeCompare", s_localeCompare);
    method(I, I->proto_string, "normalize", s_normalize);

    /* Promise */
    I->proto_promise = obj_new(I, OBJ_PLAIN);
    method(I, I->proto_promise, "then", pr_then);
    method(I, I->proto_promise, "catch", pr_catch);
    method(I, I->proto_promise, "finally", pr_finally);
    value_t pr = v_native(I, "Promise", js_Promise);
    obj_t* ps = statics_of(I, pr);
    obj_set(I, ps, "prototype", v_obj(I->proto_promise));
    method(I, ps, "resolve", pr_resolve);
    method(I, ps, "reject", pr_reject);
    method(I, ps, "all", pr_all);
    method(I, ps, "allSettled", pr_allSettled);
    method(I, ps, "race", pr_race);
    method(I, ps, "any", pr_any);
    script_def_global(I, "Promise", pr);
    script_def_global(I, "queueMicrotask", v_native(I, "queueMicrotask", js_queueMicrotask));

    /* Map, Set (WeakMap/WeakSet: the same, nothing is ever collected here) */
    I->proto_map = obj_new(I, OBJ_PLAIN);
    method(I, I->proto_map, "get", m_get);
    method(I, I->proto_map, "set", m_set);
    method(I, I->proto_map, "has", c_has);
    method(I, I->proto_map, "delete", c_delete);
    method(I, I->proto_map, "clear", c_clear);
    method(I, I->proto_map, "forEach", c_forEach);
    method(I, I->proto_map, "keys", c_keys);
    method(I, I->proto_map, "values", c_values);
    method(I, I->proto_map, "entries", c_entries);
    getter(I, I->proto_map, "size", c_size);
    I->proto_set = obj_new(I, OBJ_PLAIN);
    method(I, I->proto_set, "add", s_add);
    method(I, I->proto_set, "has", c_has);
    method(I, I->proto_set, "delete", c_delete);
    method(I, I->proto_set, "clear", c_clear);
    method(I, I->proto_set, "forEach", c_forEach);
    method(I, I->proto_set, "keys", c_keys);
    method(I, I->proto_set, "values", c_values);
    method(I, I->proto_set, "entries", c_entries);
    getter(I, I->proto_set, "size", c_size);
    const char* const maps[] = { "Map", "WeakMap" }, *const sets[] = { "Set", "WeakSet" };
    for (int i = 0; i < 2; i++) {
        value_t m = v_native(I, maps[i], js_Map);
        obj_set(I, statics_of(I, m), "prototype", v_obj(I->proto_map));
        script_def_global(I, maps[i], m);
        value_t s = v_native(I, sets[i], js_Set);
        obj_set(I, statics_of(I, s), "prototype", v_obj(I->proto_set));
        script_def_global(I, sets[i], s);
    }

    /* Symbol */
    value_t sym = v_native(I, "Symbol", js_Symbol);
    obj_t* ss = statics_of(I, sym);
    obj_set(I, ss, "iterator", v_str(I, "@@iterator"));
    obj_set(I, ss, "asyncIterator", v_str(I, "@@asyncIterator"));
    obj_set(I, ss, "hasInstance", v_str(I, "@@hasInstance"));
    obj_set(I, ss, "toStringTag", v_str(I, "@@toStringTag"));
    obj_set(I, ss, "toPrimitive", v_str(I, "@@toPrimitive"));
    method(I, ss, "for", sym_for);
    script_def_global(I, "Symbol", sym);

    value_t prx = v_native(I, "Proxy", js_Proxy);
    method(I, statics_of(I, prx), "revocable", js_Proxy_revocable);
    script_def_global(I, "Proxy", prx);

    I->proto_gen = obj_new(I, OBJ_PLAIN);
    method(I, I->proto_gen, "next", gen_next);
    method(I, I->proto_gen, "return", gen_return);
    method(I, I->proto_gen, "throw", gen_throw);
    method(I, I->proto_gen, "@@iterator", gen_self);
    method(I, I->proto_gen, "@@asyncIterator", gen_self);

    /* Function.prototype.bind */
    if (I->proto_func) method(I, I->proto_func, "bind", f_bind);

    /* Object */
    value_t O = script_get_global(I, "Object");
    if (O.t == V_FUNC) {
        obj_t* os = statics_of(I, O);
        method(I, os, "create", o_create);
        method(I, os, "getPrototypeOf", o_getPrototypeOf);
        method(I, os, "setPrototypeOf", o_setPrototypeOf);
        method(I, os, "defineProperty", o_defineProperty);
        method(I, os, "defineProperties", o_defineProperties);
        method(I, os, "getOwnPropertyDescriptor", o_getOwnPropertyDescriptor);
        method(I, os, "freeze", o_identity);
        method(I, os, "seal", o_identity);
        method(I, os, "preventExtensions", o_identity);
        method(I, os, "isFrozen", o_false);
        method(I, os, "isSealed", o_false);
        method(I, os, "fromEntries", o_fromEntries);
        method(I, os, "is", o_is);
        int ff = 0;
        value_t keys = prop_get_raw(os, "keys", &ff);
        if (ff) obj_set(I, os, "getOwnPropertyNames", keys);
        obj_set(I, os, "prototype", v_obj(I->proto_object));
    }
    /* Array */
    value_t A = script_get_global(I, "Array");
    if (A.t == V_FUNC) {
        obj_t* as = statics_of(I, A);
        method(I, as, "from", a_from);
        method(I, as, "of", a_of);
        obj_set(I, as, "prototype", v_obj(I->proto_array));
    }
    method(I, I->proto_array, "flat", a_flat);
    method(I, I->proto_array, "flatMap", a_flatMap);
    method(I, I->proto_array, "lastIndexOf", a_lastIndexOf);
    method(I, I->proto_array, "findLast", a_findLast);
    method(I, I->proto_array, "findLastIndex", a_findLastIndex);
    method(I, I->proto_array, "reduceRight", a_reduceRight);
    method(I, I->proto_array, "keys", a_keys);
    method(I, I->proto_array, "values", a_values);
    method(I, I->proto_array, "entries", a_entries);
    /* String / Function prototypes for instanceof & friends */
    value_t S = script_get_global(I, "String");
    if (S.t == V_FUNC) obj_set(I, statics_of(I, S), "prototype", v_obj(I->proto_string));
    /* Number */
    value_t N = script_get_global(I, "Number");
    if (N.t == V_FUNC) {
        obj_t* ns = statics_of(I, N);
        method(I, ns, "isInteger", n_isInteger);
        method(I, ns, "isSafeInteger", n_isInteger);
        method(I, ns, "isFinite", n_isFinite);
        obj_set(I, ns, "EPSILON", v_num(2.220446049250313e-16L));
        obj_set(I, ns, "MAX_SAFE_INTEGER", v_num(9007199254740991.0L));
        obj_set(I, ns, "MIN_SAFE_INTEGER", v_num(-9007199254740991.0L));
        obj_set(I, ns, "MAX_VALUE", v_num(1.7976931348623157e308L));
        obj_set(I, ns, "MIN_VALUE", v_num(5e-324L));
        num_t z = 0;
        obj_set(I, ns, "POSITIVE_INFINITY", v_num(1 / z));
        obj_set(I, ns, "NEGATIVE_INFINITY", v_num(-1 / z));
        obj_set(I, ns, "NaN", v_num(z / z));
        obj_set(I, ns, "prototype", v_obj(I->proto_number));
    }
    /* Math */
    value_t M = script_get_global(I, "Math");
    if (M.t == V_OBJ) {
        obj_t* m = M.o;
        method(I, m, "atan2", m_atan2);
        method(I, m, "atan", m_atan);
        method(I, m, "tan", m_tan);
        method(I, m, "asin", m_asin);
        method(I, m, "acos", m_acos);
        method(I, m, "log2", m_log2);
        method(I, m, "log10", m_log10);
        method(I, m, "hypot", m_hypot);
        method(I, m, "cbrt", m_cbrt);
        method(I, m, "fround", m_fround);
        method(I, m, "imul", m_imul);
        method(I, m, "clz32", m_clz32);
        obj_set(I, m, "LN2", v_num(0.69314718055994530942L));
        obj_set(I, m, "LN10", v_num(2.30258509299404568402L));
        obj_set(I, m, "LOG2E", v_num(1.44269504088896340736L));
        obj_set(I, m, "LOG10E", v_num(0.43429448190325182765L));
        obj_set(I, m, "SQRT2", v_num(1.41421356237309504880L));
        obj_set(I, m, "SQRT1_2", v_num(0.70710678118654752440L));
    }
    /* console extras */
    value_t con = script_get_global(I, "console");
    if (con.t == V_OBJ) {
        int ff = 0;
        value_t log = prop_get_raw(con.o, "log", &ff);
        if (ff) { obj_set(I, con.o, "debug", log); obj_set(I, con.o, "table", log); obj_set(I, con.o, "trace", log); }
    }
    /* globals */
    obj_t* perf = obj_new(I, OBJ_PLAIN);
    method(I, perf, "now", js_performance_now);
    script_def_global(I, "performance", v_obj(perf));
    script_def_global(I, "btoa", v_native(I, "btoa", js_btoa));
    script_def_global(I, "atob", v_native(I, "atob", js_atob));
    script_def_global(I, "encodeURI", v_native(I, "encodeURI", js_encodeURI));
    value_t dec = script_get_global(I, "decodeURIComponent");
    if (dec.t == V_FUNC) script_def_global(I, "decodeURI", dec);
    script_def_global(I, "structuredClone", v_native(I, "structuredClone", js_structuredClone));
    script_def_global(I, "\x01tplstrings", v_native(I, "tplstrings", js_tplstrings));
    value_t fnc = v_native(I, "Function", js_Function);
    if (I->proto_func) obj_set(I, statics_of(I, fnc), "prototype", v_obj(I->proto_func));
    script_def_global(I, "Function", fnc);
    script_def_global(I, "eval", v_native(I, "eval", js_eval));
    /* Error subclasses that may be missing */
    value_t err = script_get_global(I, "Error");
    const char* const errs[] = { "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError", "AggregateError" };
    for (uint32_t i = 0; err.t == V_FUNC && i < sizeof(errs) / sizeof(errs[0]); i++)
        if (script_get_global(I, errs[i]).t == V_UNDEF) {
            value_t e = v_native(I, errs[i], err.f->nf);
            script_def_global(I, errs[i], e);
        }
}
