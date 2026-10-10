/* Banana Code: a small JSON reader and writer (extension manifests,
 * banana.json, the messages extensions exchange with the editor). */
#include "code.h"
#include <stdarg.h>

typedef struct { const char* p; int depth; int bad; } jp_t;

static void ws(jp_t* j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }

static json_t* node(jtype_t t) {
    json_t* n = calloc(1, sizeof(json_t));
    if (n) n->type = t;
    return n;
}

static void put_utf8(sbuf_t* b, unsigned cp) {
    char u[4];
    int n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63)); u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_addn(b, u, n);
}

static int hex4(const char* p, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static char* parse_string(jp_t* j) {
    if (*j->p != '"') { j->bad = 1; return NULL; }
    j->p++;
    sbuf_t b;
    sb_init(&b);
    while (*j->p && *j->p != '"') {
        if (*j->p == '\\') {
            j->p++;
            char c = *j->p++;
            switch (c) {
            case 'n': sb_add(&b, "\n"); break;
            case 't': sb_add(&b, "\t"); break;
            case 'r': sb_add(&b, "\r"); break;
            case 'b': sb_add(&b, "\b"); break;
            case 'f': sb_add(&b, "\f"); break;
            case 'u': {
                unsigned cp;
                if (hex4(j->p, &cp)) { j->bad = 1; break; }
                j->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo;
                    if (!hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                }
                put_utf8(&b, cp);
                break;
            }
            case 0: j->bad = 1; j->p--; break;
            default: { char s[2] = { c, 0 }; sb_add(&b, s); }
            }
        } else {
            sb_addn(&b, j->p, 1);
            j->p++;
        }
    }
    if (*j->p == '"') j->p++;
    else j->bad = 1;
    if (!b.s) sb_add(&b, "");
    return b.s;
}

static json_t* parse_value(jp_t* j) {
    ws(j);
    if (++j->depth > 64) { j->bad = 1; return NULL; }
    json_t* n = NULL;
    char c = *j->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        n = node(obj ? J_OBJ : J_ARR);
        j->p++;
        json_t** tail = &n->child;
        ws(j);
        if (*j->p == (obj ? '}' : ']')) j->p++;
        else for (;;) {
            char* key = NULL;
            if (obj) {
                ws(j);
                key = parse_string(j);
                ws(j);
                if (*j->p == ':') j->p++; else j->bad = 1;
            }
            json_t* v = parse_value(j);
            if (!v) { free(key); j->bad = 1; break; }
            v->key = key;
            *tail = v;
            tail = &v->next;
            ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == (obj ? '}' : ']')) { j->p++; break; }
            j->bad = 1;
            break;
        }
    } else if (c == '"') {
        n = node(J_STR);
        n->str = parse_string(j);
    } else if (!strncmp(j->p, "true", 4)) { n = node(J_BOOL); n->num = 1; j->p += 4; }
    else if (!strncmp(j->p, "false", 5)) { n = node(J_BOOL); j->p += 5; }
    else if (!strncmp(j->p, "null", 4)) { n = node(J_NULL); j->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char* e;
        n = node(J_NUM);
        n->num = strtod(j->p, &e);
        if (e == j->p) j->bad = 1;
        j->p = e;
    } else {
        j->bad = 1;
    }
    j->depth--;
    return n;
}

json_t* json_parse(const char* text) {
    if (!text) return NULL;
    jp_t j = { text, 0, 0 };
    json_t* v = parse_value(&j);
    if (j.bad) { json_free(v); return NULL; }
    return v;
}

void json_free(json_t* j) {
    while (j) {
        json_t* nx = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = nx;
    }
}

json_t* json_get(const json_t* obj, const char* key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (json_t* c = obj->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char* json_str(const json_t* obj, const char* key, const char* def) {
    json_t* v = json_get(obj, key);
    return v && v->type == J_STR ? v->str : def;
}

double json_num(const json_t* obj, const char* key, double def) {
    json_t* v = json_get(obj, key);
    return v && (v->type == J_NUM || v->type == J_BOOL) ? v->num : def;
}

/* ── writing ── */
void sb_init(sbuf_t* b) { b->s = NULL; b->len = b->cap = 0; }
void sb_free(sbuf_t* b) { free(b->s); sb_init(b); }

void sb_addn(sbuf_t* b, const char* s, int n) {
    if (n <= 0) { if (!b->s) { b->s = malloc(16); if (b->s) { b->s[0] = 0; b->cap = 16; } } return; }
    if (b->len + n + 1 > b->cap) {
        int c = b->cap ? b->cap : 64;
        while (c < b->len + n + 1) c *= 2;
        char* ns = realloc(b->s, (size_t)c);
        if (!ns) return;
        b->s = ns;
        b->cap = c;
    }
    memcpy(b->s + b->len, s, (size_t)n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_add(sbuf_t* b, const char* s) { sb_addn(b, s, (int)strlen(s)); }

void sb_printf(sbuf_t* b, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
    sb_addn(b, tmp, n);
}

void sb_json_str(sbuf_t* b, const char* s, int n) {
    if (n < 0) n = s ? (int)strlen(s) : 0;
    sb_add(b, "\"");
    int start = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char* esc = NULL;
        char u[8];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) { snprintf(u, sizeof(u), "\\u%04x", c); esc = u; }
        else if (c == '<' && i + 1 < n && s[i + 1] == '/') esc = "<\\/";      /* never closes a <script> */
        if (esc) {
            sb_addn(b, s + start, i - start);
            sb_add(b, esc);
            if (c == '<') i++;
            start = i + 1;
        }
    }
    sb_addn(b, s + start, n - start);
    sb_add(b, "\"");
}
