#ifndef SCRIPT_INT_H
#define SCRIPT_INT_H

/* BananaScript internals, shared by script.c (core) and script_lib.c
 * (built-in functions). Not for other users - see script.h. */

#include "script.h"

/* ── syntax tree ─────────────────────────────────────────────────── */

enum {
    /* expressions */
    N_NUM = 1, N_STR, N_TRUE, N_FALSE, N_NULL, N_UNDEF, N_THIS,
    N_IDENT,        /* s */
    N_ARRAY,        /* a: list of elements (PHP: N_PAIR) */
    N_OBJECT,       /* a: list of N_PAIR */
    N_PAIR,         /* a: key expr (or NULL), b: value; s: plain key */
    N_FUNC,         /* s: name, a: params (N_PARAM), b: body, op: 1 = arrow with expression body */
    N_PARAM,        /* s: name, a: default value */
    N_MEMBER,       /* a: object, s: name, op: 1 = ?. */
    N_INDEX,        /* a: object, b: key (NULL: PHP $a[] append) */
    N_CALL,         /* a: callee, b: args */
    N_NEW,          /* a: callee, b: args */
    N_UNARY,        /* op, a */
    N_BINARY,       /* op, a, b */
    N_LOGIC,        /* op (&&, ||, ??), a, b */
    N_ASSIGN,       /* op ('=' or the compound operator), a: target, b: value */
    N_UPDATE,       /* op ('+' / '-'), a: target, n: 1 = prefix */
    N_COND,         /* a ? b : c */
    N_CAST,         /* PHP (int)$x: s = type, a */
    N_TYPEOF,       /* a */
    /* statements */
    N_VAR,          /* s: name, a: init, op: 'v' var / 'l' let / 'c' const */
    N_EXPR,         /* a */
    N_BLOCK,        /* a: list */
    N_IF,           /* a: cond, b: then, c: else */
    N_WHILE,        /* a: cond, b: body */
    N_DOWHILE,      /* a: cond, b: body */
    N_FOR,          /* a: init, b: cond, c: update, d: body */
    N_FOREACH,      /* a: iterable, b: value var, c: key var (or NULL), d: body; op: 'o' of / 'i' in / 'p' PHP */
    N_RETURN,       /* a */
    N_BREAK, N_CONTINUE,
    N_FUNCDECL,     /* a: N_FUNC */
    N_ECHO,         /* a: list of expressions */
    N_HTML,         /* s: inline HTML (PHP) */
    N_TRY,          /* a: block, s: catch name, b: catch block, c: finally */
    N_THROW,        /* a */
    N_SWITCH,       /* a: discriminant, b: list of N_CASE */
    N_CASE,         /* a: test (NULL = default), b: statement list */
    N_GLOBAL,       /* PHP global $x: s */
    N_EMPTY,
    /* later JavaScript */
    N_REGEX,        /* s: pattern, a: N_STR flags */
    N_SPREAD,       /* ...a (array/object literals, call arguments, rest elements) */
    N_SEQ,          /* a, b (comma operator): a = list */
    N_LABEL,        /* s: label, a: statement */
    N_CLASS,        /* s: name, a: superclass, b: members (N_PAIR: op 'm'/'g'/'s'/'f', n = 1 static); op 1: declaration */
    N_SUPERCALL,    /* super(b...) */
    N_SUPERMEMBER,  /* super.s */
    N_AWAIT,        /* await a */
    N_VARHOIST,     /* a = list of `var` names to declare (undefined) on entry */
    N_IMPORT,       /* import ... from s (see parse_import) */
    N_EXPORT,       /* export a / export { b } */
};

/* binary/unary operator codes (op field) */
enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_CONCAT,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NULLISH,
    OP_NOT, OP_NEG, OP_PLUS, OP_BITNOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_SPACESHIP, OP_IN, OP_INSTANCEOF, OP_USHR, OP_LOGAND_ASSIGN, OP_LOGOR_ASSIGN
};

typedef struct node {
    uint8_t  k;
    uint8_t  op;
    uint16_t col;            /* column, capped (errors in minified code) */
    int      line : 24;
    unsigned flags : 8;      /* N_FUNC: NF_* (worked out on first call) */
    struct node *a, *b, *c, *d;
    struct node* next;       /* lists */
    str_t*   s;
    num_t    n;
} node_t;

#define NF_CHECKED   1          /* N_FUNC flags */
#define NF_ARGUMENTS 2          /* its body may use `arguments` (or eval) */

/* ── run time ────────────────────────────────────────────────────── */

typedef struct var {
    str_t*      name;
    value_t     v;
    int         is_const;
    struct var* ref;         /* PHP `global $x`: the global variable this names */
    struct var* next;
} var_t;

typedef struct env {
    var_t*      vars;
    struct env* parent;
    int         is_func;     /* function (or global) scope: `var` lives here */
    int         captured;    /* a closure holds it: it must outlive its call */
} env_t;

struct func {
    int        native;
    native_fn  nf;
    node_t*    decl;         /* N_FUNC */
    env_t*     closure;
    const char* name;
    value_t    bound_this;   /* arrow functions keep the outer `this` */
    int        has_bound;
    obj_t*     statics;      /* properties of the function itself (String.fromCharCode, F.prototype) */
    value_t    data;         /* natives: a value they were made with (promise resolvers, bound arguments) */
    value_t    parent;       /* classes: the superclass constructor (undefined if none) */
    obj_t*     home;         /* class members: the object they belong to (super.x looks above it) */
    node_t*    fields;       /* class constructors: instance field initializers (N_PAIR list) */
    struct func* cls;        /* methods and arrows inside a class: that class's constructor */
    int        is_class;
    int        is_async;
};

enum { CTL_NONE = 0, CTL_BREAK, CTL_CONTINUE, CTL_RETURN, CTL_THROW };

struct interp {
    arena_t*  A;
    int       lang;
    env_t*    global;
    env_t*    fn_env;        /* scope of the running function */
    env_t*    cur;           /* current lexical scope */
    env_t*    free_envs;     /* scopes of finished calls nothing captured, for reuse */
    var_t*    free_vars;
    str_t*    s_arguments;   /* "arguments", made once */
    void*     free_items[24]; /* outgrown array / property buffers, by log2(capacity) */
    void*     free_props[24];
    value_t   this_v;
    int       ctl;
    value_t   ret;           /* return value / thrown value */
    int       line;          /* line being run (errors) */
    int       col;
    int       throw_line, throw_col;   /* where the exception was thrown */
    obj_t*    oom_err;       /* thrown when the arena is exhausted */
    uint32_t  steps, step_limit;
    void    (*yield_fn)(void);  /* called every 1K steps: other (cooperative) tasks keep running */
    uint32_t  depth, depth_limit;
    char      err[200];
    const char* src_name;
    script_out_fn out, log;
    void*     out_ctx;
    void*     log_ctx;
    void*     host;
    obj_t*    proto_string;  /* built-in method tables */
    obj_t*    proto_array;
    obj_t*    proto_number;
    obj_t*    proto_date;
    obj_t*    proto_func;
    obj_t*    proto_object;
    uint32_t  running;       /* script_run/script_call nesting (natives calling back) */
    script_ext_fn php_ext;   /* host-provided PHP functions (httpd: header, files) */
    func_t*   cur_fn;        /* the JS function running (super, class fields) */
    func_t*   cur_native;    /* the native function running (its data) */
    obj_t*    proto_regexp;
    obj_t*    proto_promise;
    obj_t*    proto_map;
    obj_t*    proto_set;
    obj_t*    jobs;          /* promise reactions waiting to run (an array) */
    uint32_t  job_head;
    str_t*    label;         /* break/continue with a label */
    int       draining;      /* es_run_jobs() is running */
    str_t**   itab;          /* interned identifier names (the lexer) */
    uint32_t  icap, icount;
    obj_t*    module_ns;     /* the module running: its exports */
    const char* module_url;  /* ...and its address (relative imports) */
    script_import_fn import_fn;
    void*     import_ctx;
};

/* helpers shared with script_lib.c */
str_t*  str_new(interp_t* I, const char* s, uint32_t n);
str_t*  str_cat(interp_t* I, str_t* a, str_t* b);
str_t*  v_tostr(interp_t* I, value_t v);
value_t v_strv(str_t* s);
int     v_loose_eq(interp_t* I, value_t a, value_t b);
int     v_strict_eq(value_t a, value_t b);
value_t obj_getv(interp_t* I, value_t ov, const char* key);
value_t prop_get_raw(obj_t* o, const char* key, int* found);
void    prop_set_raw(interp_t* I, obj_t* o, str_t* key, value_t v);
int     prop_del(obj_t* o, const char* key);
value_t arr_get(obj_t* a, uint32_t i);
void    arr_set(interp_t* I, obj_t* a, uint32_t i, value_t v);
value_t call_value(interp_t* I, value_t fn, value_t self, int argc, value_t* argv);
num_t   str_tonum(const char* s, int php);
int     num_isnan(num_t x);
/* script_es.c: RegExp, Promise, Map/Set, Symbol and the newer built-ins */
void    es_init(interp_t* I);
value_t es_regexp_new(interp_t* I, const char* pattern, uint32_t len, const char* flags);
value_t es_promise_new(interp_t* I);
void    es_promise_settle(interp_t* I, value_t p, int rejected, value_t v);
value_t es_promise_resolved(interp_t* I, value_t v);
int     es_promise_state(interp_t* I, value_t p, value_t* out);   /* 0 pending, 1 fulfilled, 2 rejected, -1 not a promise */
void    es_run_jobs(interp_t* I);
obj_t*  es_to_array(interp_t* I, value_t v);   /* an iterable/array-like as an array (NULL: not iterable) */
int     es_instanceof(interp_t* I, value_t v, value_t ctor);
value_t accessor_get(interp_t* I, value_t acc, value_t self);
num_t   num_floor(num_t x);
void    lib_init(interp_t* I);          /* script_lib.c: built-in globals */
value_t lib_member(interp_t* I, value_t ov, const char* key, int* found);   /* methods of strings, arrays, numbers */
value_t php_call_builtin(interp_t* I, const char* name, int argc, value_t* argv, int* found);
uint32_t php_array_count(obj_t* o);
value_t php_array_copy(interp_t* I, value_t v);
void    php_array_push(interp_t* I, obj_t* o, value_t v);
void    php_array_set(interp_t* I, obj_t* o, value_t key, value_t v);
value_t php_array_get(interp_t* I, obj_t* o, value_t key, int* found);

#define V_UNDEF_INIT { .t = V_UNDEF }

/* script_typed.c: ArrayBuffer, typed arrays, DataView */
void typed_init(interp_t* I);
int  typed_instanceof(value_t v, const char* ctor);   /* 1/0, or -1 if ctor is not one of them */

#endif
