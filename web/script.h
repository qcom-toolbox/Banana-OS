#ifndef SCRIPT_H
#define SCRIPT_H

#include "types.h"
#include "arena.h"

/*
 * BananaScript: one small tree-walking interpreter with two front ends -
 *   LANG_JS   a basic JavaScript (the web browser's <script>)
 *   LANG_PHP  a basic PHP (the web server's .php pages)
 * Values, objects/arrays, functions and closures are shared; the parser
 * and a few operators (+ vs ., ==, truthiness) follow each language.
 * Numbers are x87 long doubles (the kernel is built without SSE).
 * Everything lives in an arena; nothing is freed until the arena is.
 */

enum { LANG_JS = 1, LANG_PHP = 2 };

typedef long double num_t;

typedef enum { V_UNDEF = 0, V_NULL, V_BOOL, V_NUM, V_STR, V_OBJ, V_FUNC } vtype_t;

typedef struct str {
    uint32_t len;
    char     s[];            /* always NUL-terminated */
} str_t;

typedef struct obj obj_t;
typedef struct func func_t;
typedef struct interp interp_t;

typedef struct value {
    uint8_t t;
    union {
        int    b;
        num_t  n;
        str_t* s;
        obj_t* o;
        func_t* f;
    };
} value_t;

typedef struct prop {
    str_t*  key;
    value_t v;
} prop_t;

/* hooks for objects implemented by the host (DOM elements, ...) */
typedef struct host_class {
    const char* name;
    /* *found = 1 if the host knows the property */
    value_t (*get)(interp_t* I, obj_t* self, const char* key, int* found);
    /* 1 if handled (else the property is stored on the object) */
    int (*set)(interp_t* I, obj_t* self, const char* key, value_t v);
} host_class_t;

enum { OBJ_PLAIN = 0, OBJ_ARRAY, OBJ_PHPARRAY, OBJ_HOST, OBJ_ACCESSOR /* a getter/setter pair: props "get", "set" */ };

struct obj {
    uint8_t  kind;
    uint32_t n, cap;         /* properties (ordered) */
    prop_t*  props;
    uint32_t len, acap;      /* OBJ_ARRAY elements */
    value_t* items;
    int64_t  next_index;     /* OBJ_PHPARRAY: next automatic key */
    const host_class_t* hc;  /* OBJ_HOST */
    void*    host;
    obj_t*   proto;          /* method lookup (Date objects, ...) */
};

typedef value_t (*native_fn)(interp_t* I, value_t self, int argc, value_t* argv);

/* output sink (PHP echo, document.write) and logs (console.log) */
typedef void (*script_out_fn)(void* ctx, const char* s, uint32_t n);

/* extra PHP functions from the host: *found = 1 if name is one of them */
typedef value_t (*script_ext_fn)(interp_t* I, const char* name, int argc, value_t* argv, int* found);

interp_t* script_new(arena_t* A, int lang);
void      script_set_output(interp_t* I, script_out_fn out, void* ctx);
void      script_set_log(interp_t* I, script_out_fn log, void* ctx);
void      script_set_host(interp_t* I, void* host);
void*     script_host(interp_t* I);
void      script_set_limits(interp_t* I, uint32_t steps, uint32_t depth);
/* fn() runs every 64K steps of a long script (lets the desktop run) */
void      script_set_yield(interp_t* I, void (*fn)(void));
void      script_set_php_ext(interp_t* I, script_ext_fn fn);

/* parses and runs a program; 0 = ok, -1 = error (script_error()) */
int         script_run(interp_t* I, const char* src, uint32_t len, const char* name);
const char* script_error(interp_t* I);
/* ES modules: runs src in a scope of its own, its exports going into ns; the
 * host loads what it imports (returning the namespace object, or throwing) */
int         script_run_module(interp_t* I, const char* src, uint32_t len, const char* name, const char* url, obj_t* ns);
typedef value_t (*script_import_fn)(interp_t* I, void* ctx, const char* spec, const char* base_url);
void        script_set_import(interp_t* I, script_import_fn fn, void* ctx);
/* calls a function value; 0 = ok, -1 = it threw (script_error()) */
int         script_call(interp_t* I, value_t fn, value_t self, int argc, value_t* argv, value_t* ret);

/* values for hosts */
value_t v_undef(void);
value_t v_null(void);
value_t v_bool(int b);
value_t v_num(num_t n);
value_t v_str(interp_t* I, const char* s);
value_t v_strn(interp_t* I, const char* s, uint32_t n);
value_t v_obj(obj_t* o);
value_t v_native(interp_t* I, const char* name, native_fn f);

obj_t*  obj_new(interp_t* I, int kind);
void    obj_set(interp_t* I, obj_t* o, const char* key, value_t v);
value_t obj_get(interp_t* I, obj_t* o, const char* key);
void    arr_push(interp_t* I, obj_t* a, value_t v);
void    script_def_global(interp_t* I, const char* name, value_t v);
value_t script_get_global(interp_t* I, const char* name);

const char* v_cstr(interp_t* I, value_t v);   /* string conversion (arena) */
num_t       v_tonum(interp_t* I, value_t v);
int         v_truthy(interp_t* I, value_t v);
int         v_isfunc(value_t v);
void        script_throw(interp_t* I, const char* msg);   /* from natives */
void        num_format(num_t x, char* out, int cap);

/* the current date and time, supplied by the kernel (Date, PHP date()) */
typedef struct {
    int year, month, day, hour, minute, second, wday;   /* month 1-12, wday 0 = Sunday */
} script_tm_t;
extern void (*script_clock)(script_tm_t* tm);

#endif
