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
    N_EMPTY
};

/* binary/unary operator codes (op field) */
enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_CONCAT,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NULLISH,
    OP_NOT, OP_NEG, OP_PLUS, OP_BITNOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_SPACESHIP, OP_IN, OP_INSTANCEOF
};

typedef struct node {
    uint8_t  k;
    uint8_t  op;
    int      line;
    struct node *a, *b, *c, *d;
    struct node* next;       /* lists */
    str_t*   s;
    num_t    n;
} node_t;

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
};

enum { CTL_NONE = 0, CTL_BREAK, CTL_CONTINUE, CTL_RETURN, CTL_THROW };

struct interp {
    arena_t*  A;
    int       lang;
    env_t*    global;
    env_t*    fn_env;        /* scope of the running function */
    env_t*    cur;           /* current lexical scope */
    value_t   this_v;
    int       ctl;
    value_t   ret;           /* return value / thrown value */
    int       line;          /* line being run (errors) */
    uint32_t  steps, step_limit;
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

#endif
