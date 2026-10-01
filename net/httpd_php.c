#include "httpd_php.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "../web/script_int.h"

/*
 * .php pages for httpd: the file runs in BananaScript's PHP front end
 * with $_GET / $_REQUEST / $_SERVER set from the request; what it echoes
 * becomes the response body. header() can set the status, the content
 * type or a redirect; file_get_contents()/file_put_contents() and friends
 * work on the Banana OS file system (relative to the script's folder).
 */

#define PHP_MEM    (32u << 20)
#define PHP_OUTMAX (4u << 20)

typedef struct {
    php_result_t* res;
    char dir[FS_PATH_LEN];          /* folder of the script */
} php_ctx_t;

static void out_fn(void* ctx, const char* s, uint32_t n) {
    php_result_t* r = ((php_ctx_t*)ctx)->res;
    if (r->overflow) return;
    if (r->len + n + 1 > r->cap) {
        uint32_t cap = (r->len + n + 1) * 2 + 4096;
        if (cap > PHP_OUTMAX) cap = PHP_OUTMAX;
        if (r->len + n + 1 > cap) { r->overflow = 1; return; }
        char* nb = (char*)kmalloc(cap);
        if (!nb) { r->overflow = 1; return; }
        if (r->body) { memcpy(nb, r->body, r->len); kfree(r->body); }
        r->body = nb;
        r->cap = cap;
    }
    memcpy(r->body + r->len, s, n);
    r->len += n;
    r->body[r->len] = 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* url-decodes s[0..n) into the arena */
static char* url_decode(arena_t* A, const char* s, uint32_t n) {
    char* o = (char*)arena_alloc(A, n + 1);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') c = ' ';
        else if (c == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            c = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        }
        o[k++] = c;
    }
    o[k] = 0;
    return o;
}

/* a=1&b=two&list[]=x -> PHP array */
static value_t parse_query(interp_t* I, arena_t* A, const char* q) {
    obj_t* arr = obj_new(I, OBJ_PHPARRAY);
    while (q && *q) {
        const char* amp = strchr(q, '&');
        uint32_t len = amp ? (uint32_t)(amp - q) : (uint32_t)strlen(q);
        uint32_t kl = 0;
        while (kl < len && q[kl] != '=') kl++;
        const char* eq = kl < len ? q + kl : NULL;
        if (kl) {
            char* key = url_decode(A, q, kl);
            char* val = eq ? url_decode(A, eq + 1, len - kl - 1) : arena_strdup(A, "", 0);
            size_t ks = strlen(key);
            if (ks > 2 && key[ks - 2] == '[' && key[ks - 1] == ']') {
                key[ks - 2] = 0;
                int found = 0;
                value_t sub = php_array_get(I, arr, v_str(I, key), &found);
                if (!found || sub.t != V_OBJ) {
                    sub = v_obj(obj_new(I, OBJ_PHPARRAY));
                    php_array_set(I, arr, v_str(I, key), sub);
                }
                php_array_push(I, sub.o, v_str(I, val));
            } else {
                php_array_set(I, arr, v_str(I, key), v_str(I, val));
            }
        }
        q = amp ? amp + 1 : NULL;
    }
    return v_obj(arr);
}

static void set_kv(interp_t* I, obj_t* a, const char* k, const char* v) {
    php_array_set(I, a, v_str(I, k), v_str(I, v));
}

/* script-relative path -> absolute Banana OS path */
static void resolve(php_ctx_t* c, const char* p, char* out, int cap) {
    if (p[0] == '/') kstrlcpy(out, p, (size_t)cap);
    else ksnprintf(out, (size_t)cap, "%s/%s", c->dir, p[0] == '.' && p[1] == '/' ? p + 2 : p);
}

static value_t ext(interp_t* I, const char* nm, int argc, value_t* argv, int* found) {
    php_ctx_t* c = (php_ctx_t*)script_host(I);
    php_result_t* r = c->res;
    char path[FS_PATH_LEN];
    *found = 1;
    if (strcmp(nm, "header") == 0 && argc) {
        const char* h = v_cstr(I, argv[0]);
        if (strncasecmp(h, "Location:", 9) == 0) {
            const char* v = h + 9;
            while (*v == ' ') v++;
            kstrlcpy(r->location, v, sizeof(r->location));
            if (r->status == 200) r->status = 302;
        } else if (strncasecmp(h, "Content-Type:", 13) == 0) {
            const char* v = h + 13;
            while (*v == ' ') v++;
            kstrlcpy(r->content_type, v, sizeof(r->content_type));
        } else if (strncasecmp(h, "HTTP/", 5) == 0) {
            const char* sp = strchr(h, ' ');
            if (sp) r->status = (int)v_tonum(I, v_str(I, sp + 1));
        }
        if (argc > 2) r->status = (int)v_tonum(I, argv[2]);
        return v_null();
    }
    if (strcmp(nm, "http_response_code") == 0) {
        int old = r->status;
        if (argc) r->status = (int)v_tonum(I, argv[0]);
        return v_num(old);
    }
    if (strcmp(nm, "file_get_contents") == 0) {
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        int fi = fs_find_file(path);
        if (fi < 0) return v_bool(0);
        fs_file_t* f = fs_get_file(fi);
        return v_strn(I, f->content, f->size);
    }
    if (strcmp(nm, "file_put_contents") == 0 && argc >= 2) {
        resolve(c, v_cstr(I, argv[0]), path, sizeof(path));
        str_t* data = v_tostr(I, argv[1]);
        int append = argc > 2 && ((int)v_tonum(I, argv[2]) & 8);
        int fi = fs_find_file(path);
        if (fi < 0) fi = fs_create(path);
        if (fi < 0) return v_bool(0);
        int rc = append ? fs_append(fi, data->s, data->len) : fs_write(fi, data->s, data->len);
        return rc == 0 ? v_num(data->len) : v_bool(0);
    }
    if (strcmp(nm, "file_exists") == 0 || strcmp(nm, "is_file") == 0 || strcmp(nm, "is_dir") == 0) {
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        int f = fs_find_file(path) >= 0, d = fs_find_dir(path) >= 0;
        return v_bool(nm[3] == 'f' ? f : nm[3] == 'd' ? d : (f || d));
    }
    if (strcmp(nm, "filesize") == 0) {
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        int fi = fs_find_file(path);
        return fi < 0 ? v_bool(0) : v_num(fs_get_file(fi)->size);
    }
    if (strcmp(nm, "unlink") == 0) {
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        if (fs_find_file(path) < 0) return v_bool(0);
        fs_delete(path, 0);
        return v_bool(1);
    }
    if (strcmp(nm, "file") == 0) {              /* lines, with their "\n" */
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        int fi = fs_find_file(path);
        if (fi < 0) return v_bool(0);
        fs_file_t* f = fs_get_file(fi);
        obj_t* a = obj_new(I, OBJ_PHPARRAY);
        uint32_t s = 0;
        for (uint32_t i = 0; i < f->size; i++)
            if (f->content[i] == '\n') { php_array_push(I, a, v_strn(I, f->content + s, i + 1 - s)); s = i + 1; }
        if (s < f->size) php_array_push(I, a, v_strn(I, f->content + s, f->size - s));
        return v_obj(a);
    }
    if (strcmp(nm, "scandir") == 0) {
        resolve(c, v_cstr(I, argc ? argv[0] : v_undef()), path, sizeof(path));
        if (fs_find_dir(path) < 0) return v_bool(0);
        obj_t* a = obj_new(I, OBJ_PHPARRAY);
        php_array_push(I, a, v_str(I, "."));
        php_array_push(I, a, v_str(I, ".."));
        int idx[FS_MAX_FILES];
        int nd = fs_list_dirs(path, idx, FS_MAX_DIRS);
        for (int i = 0; i < nd; i++) php_array_push(I, a, v_str(I, fs_get_dir(idx[i])->name));
        int nf = fs_list_files(path, idx, FS_MAX_FILES);
        for (int i = 0; i < nf; i++) php_array_push(I, a, v_str(I, fs_get_file(idx[i])->name));
        return v_obj(a);
    }
    *found = 0;
    return v_undef();
}

int httpd_run_php(const char* fspath, const char* code, uint32_t code_len, const php_request_t* rq, php_result_t* res) {
    memset(res, 0, sizeof(*res));
    res->status = 200;
    kstrlcpy(res->content_type, "text/html; charset=utf-8", sizeof(res->content_type));

    arena_t A;
    arena_init(&A, PHP_MEM);
    php_ctx_t ctx;
    ctx.res = res;
    kstrlcpy(ctx.dir, fspath, sizeof(ctx.dir));
    char* slash = strrchr(ctx.dir, '/');
    if (slash && slash != ctx.dir) *slash = 0;
    else kstrlcpy(ctx.dir, "/", sizeof(ctx.dir));

    interp_t* I = script_new(&A, LANG_PHP);
    script_set_host(I, &ctx);
    script_set_output(I, out_fn, &ctx);
    script_set_limits(I, 20000000, 200);
    script_set_php_ext(I, ext);

    value_t get = parse_query(I, &A, rq->query);
    script_def_global(I, "_GET", get);
    script_def_global(I, "_REQUEST", get);
    script_def_global(I, "_POST", v_obj(obj_new(I, OBJ_PHPARRAY)));
    script_def_global(I, "_COOKIE", v_obj(obj_new(I, OBJ_PHPARRAY)));
    obj_t* srv = obj_new(I, OBJ_PHPARRAY);
    set_kv(I, srv, "REQUEST_METHOD", rq->method);
    set_kv(I, srv, "REQUEST_URI", rq->uri);
    set_kv(I, srv, "QUERY_STRING", rq->query ? rq->query : "");
    set_kv(I, srv, "SCRIPT_NAME", rq->path);
    set_kv(I, srv, "PHP_SELF", rq->path);
    set_kv(I, srv, "SCRIPT_FILENAME", fspath);
    set_kv(I, srv, "DOCUMENT_ROOT", rq->docroot);
    set_kv(I, srv, "REMOTE_ADDR", rq->remote_addr);
    set_kv(I, srv, "SERVER_SOFTWARE", "BananaOS-httpd/0.5 PHP/8.3.0-banana");
    set_kv(I, srv, "SERVER_NAME", rq->host[0] ? rq->host : "banana-os");
    set_kv(I, srv, "HTTP_HOST", rq->host);
    set_kv(I, srv, "HTTP_USER_AGENT", rq->user_agent);
    script_def_global(I, "_SERVER", v_obj(srv));

    const char* name = strrchr(fspath, '/');
    int rc = script_run(I, code, code_len, name ? name + 1 : fspath);
    if (rc != 0) {
        /* like PHP with display_errors on */
        char msg[260];
        ksnprintf(msg, sizeof(msg), "<br />\n<b>%s</b><br />\n", script_error(I));
        out_fn(&ctx, msg, (uint32_t)strlen(msg));
        if (!res->len) res->status = 500;
    }
    if (A.oom) {
        const char* m = "<br />\n<b>Fatal error</b>: allowed memory size exhausted<br />\n";
        out_fn(&ctx, m, (uint32_t)strlen(m));
    }
    arena_free_all(&A);
    return rc;
}
