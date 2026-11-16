/* SPDX-License-Identifier: GPL-3.0-or-later */
/* WebGL in the page's worker: __gl(element, op, ...) records each call of the
 * WebGLRenderingContext (web/prelude.js) into the context's command stream
 * (webgl_ops.h); a call that needs an answer, and each frame shown, sends
 * the stream through the kernel (WW_GL) to the context's GPU worker
 * (apps/webgl), which plays it with OpenGL ES 2.0 on the GPU. A shown
 * frame comes back as pixels and becomes the <canvas>'s image (canvas.c).
 * Workers only. */
#ifdef BANANA_USER_WORKER
#include "canvas.h"
#include "page.h"
#include "kheap.h"
#include "kstring.h"
#include "script_int.h"
#include "webgl_ops.h"
#include "../sdk/include/banana_web_worker.h"

int typed_bytes(value_t v, const uint8_t** d, uint32_t* n);   /* script_typed.c */

typedef struct webgl {
    struct webgl* next;
    dom_node_t* node;
    int      id;                                 /* the kernel's, -1: lost */
    uint32_t* w;
    uint32_t n, cap;
    uint32_t next_id[GLK_KINDS];
    int      bw, bh;                             /* the drawing buffer */
    int      flip_y, premul;                     /* UNPACK_FLIP_Y_WEBGL, UNPACK_PREMULTIPLY_ALPHA_WEBGL */
    int      premultiplied;                      /* the context's premultipliedAlpha */
    int      version;                            /* 2: WebGL 1 on OpenGL ES 2.0; 3: WebGL 2 on ES 3.0 */
} webgl_t;

enum { R_NONE = 0, R_INT, R_STR, R_FLOATS, R_ACTIVE, R_BYTES };
/* each command's arguments: i int, f float, s string, b bytes, w words (rest),
 * * the rest of the arguments like the next letter; and what it answers */
static const struct { const char* sig; int res; } SIG[GLC_COUNT] = {
    [GLC_RESIZE] = { "ii", 0 }, [GLC_DELETE] = { "ii", 0 }, [GLC_SHADER_SOURCE] = { "is", 0 },
    [GLC_COMPILE] = { "i", 0 }, [GLC_ATTACH] = { "ii", 0 }, [GLC_DETACH] = { "ii", 0 }, [GLC_LINK] = { "i", 0 },
    [GLC_USE] = { "i", 0 }, [GLC_VALIDATE] = { "i", 0 }, [GLC_SHADER_PARAM] = { "ii", R_INT },
    [GLC_PROGRAM_PARAM] = { "ii", R_INT }, [GLC_SHADER_LOG] = { "i", R_STR }, [GLC_PROGRAM_LOG] = { "i", R_STR },
    [GLC_ATTRIB_LOC] = { "is", R_INT }, [GLC_BIND_ATTRIB_LOC] = { "iis", 0 }, [GLC_UNIFORM_LOC] = { "is", R_INT },
    [GLC_ACTIVE] = { "iii", R_ACTIVE }, [GLC_UNIFORM_F] = { "ii*f", 0 }, [GLC_UNIFORM_I] = { "ii*i", 0 },
    [GLC_UNIFORM_FV] = { "iiiw", 0 }, [GLC_UNIFORM_IV] = { "iiiw", 0 }, [GLC_UNIFORM_MAT] = { "iiiw", 0 },
    [GLC_ATTRIB_PTR] = { "iiiiii", 0 }, [GLC_ENABLE_ATTRIB] = { "i", 0 }, [GLC_DISABLE_ATTRIB] = { "i", 0 },
    [GLC_ATTRIB_F] = { "ii*f", 0 }, [GLC_BIND_BUFFER] = { "ii", 0 }, [GLC_BUFFER_DATA] = { "iiib", 0 },
    [GLC_BUFFER_SUB] = { "iib", 0 }, [GLC_DRAW_ARRAYS] = { "iii", 0 }, [GLC_DRAW_ELEMENTS] = { "iiii", 0 },
    [GLC_VIEWPORT] = { "iiii", 0 }, [GLC_SCISSOR] = { "iiii", 0 }, [GLC_CLEAR_COLOR] = { "ffff", 0 },
    [GLC_CLEAR_DEPTH] = { "f", 0 }, [GLC_CLEAR_STENCIL] = { "i", 0 }, [GLC_CLEAR] = { "i", 0 },
    [GLC_ENABLE] = { "i", 0 }, [GLC_DISABLE] = { "i", 0 }, [GLC_BLEND_FUNC] = { "iiii", 0 },
    [GLC_BLEND_EQ] = { "ii", 0 }, [GLC_BLEND_COLOR] = { "ffff", 0 }, [GLC_DEPTH_FUNC] = { "i", 0 },
    [GLC_DEPTH_MASK] = { "i", 0 }, [GLC_DEPTH_RANGE] = { "ff", 0 }, [GLC_COLOR_MASK] = { "iiii", 0 },
    [GLC_CULL_FACE] = { "i", 0 }, [GLC_FRONT_FACE] = { "i", 0 }, [GLC_LINE_WIDTH] = { "f", 0 },
    [GLC_POLYGON_OFFSET] = { "ff", 0 }, [GLC_STENCIL_FUNC] = { "iiii", 0 }, [GLC_STENCIL_OP] = { "iiii", 0 },
    [GLC_STENCIL_MASK] = { "ii", 0 }, [GLC_HINT] = { "ii", 0 }, [GLC_PIXEL_STORE] = { "ii", 0 },
    [GLC_ACTIVE_TEXTURE] = { "i", 0 }, [GLC_BIND_TEXTURE] = { "ii", 0 }, [GLC_TEX_PARAM_I] = { "iii", 0 },
    [GLC_TEX_PARAM_F] = { "iif", 0 }, [GLC_TEX_IMAGE] = { "iiiiiiib", 0 }, [GLC_TEX_SUB_IMAGE] = { "iiiiiiiib", 0 },
    [GLC_GEN_MIPMAP] = { "i", 0 }, [GLC_BIND_FRAMEBUFFER] = { "ii", 0 }, [GLC_FB_TEXTURE] = { "iiiii", 0 },
    [GLC_FB_RENDERBUFFER] = { "iiii", 0 }, [GLC_CHECK_FB] = { "i", R_INT }, [GLC_BIND_RENDERBUFFER] = { "ii", 0 },
    [GLC_RB_STORAGE] = { "iiii", 0 }, [GLC_READ_PIXELS] = { "iiiiii", R_BYTES }, [GLC_GET_ERROR] = { "", R_INT },
    [GLC_GET_PARAM] = { "i", R_FLOATS }, [GLC_IS_ENABLED] = { "i", R_INT }, [GLC_GET_STRING] = { "i", R_STR },
    [GLC_FINISH] = { "", 0 }, [GLC_COPY_TEX_IMAGE] = { "iiiiiii", 0 }, [GLC_COPY_TEX_SUB_IMAGE] = { "iiiiiiii", 0 },
    [GLC_SAMPLE_COVERAGE] = { "fi", 0 },
    [GLC_DRAW_ARRAYS_INST] = { "iiii", 0 }, [GLC_DRAW_ELEMENTS_INST] = { "iiiii", 0 }, [GLC_ATTRIB_DIVISOR] = { "ii", 0 },
    /* WebGL 2 */
    [GLC_BIND_VAO] = { "i", 0 }, [GLC_TEX_IMAGE3D] = { "iiiiiiiib", 0 }, [GLC_TEX_SUB_IMAGE3D] = { "iiiiiiiiiib", 0 },
    [GLC_TEX_STORAGE2D] = { "iiiii", 0 }, [GLC_TEX_STORAGE3D] = { "iiiiii", 0 }, [GLC_DRAW_BUFFERS] = { "*i", 0 },
    [GLC_READ_BUFFER] = { "i", 0 }, [GLC_BLIT_FB] = { "iiiiiiiiii", 0 }, [GLC_RB_STORAGE_MS] = { "iiiii", 0 },
    [GLC_FB_TEX_LAYER] = { "iiiii", 0 }, [GLC_BIND_BUFFER_BASE] = { "iii", 0 }, [GLC_BIND_BUFFER_RANGE] = { "iiiii", 0 },
    [GLC_UNIFORM_BLOCK_INDEX] = { "is", R_INT }, [GLC_UNIFORM_BLOCK_BINDING] = { "iii", 0 },
    [GLC_UNIFORM_BLOCK_PARAM] = { "iii", R_INT }, [GLC_UNIFORM_UI] = { "ii*i", 0 }, [GLC_UNIFORM_UIV] = { "iiiw", 0 },
    [GLC_UNIFORM_MAT_NS] = { "iiiiw", 0 }, [GLC_ATTRIB_IPTR] = { "iiiii", 0 }, [GLC_ATTRIB_I] = { "iiiiii", 0 },
    [GLC_CLEAR_BUFFER_FV] = { "ii*f", 0 }, [GLC_CLEAR_BUFFER_IV] = { "iii*i", 0 }, [GLC_CLEAR_BUFFER_FI] = { "iifi", 0 },
    [GLC_DRAW_RANGE_ELEMENTS] = { "iiiiii", 0 }, [GLC_BIND_SAMPLER] = { "ii", 0 }, [GLC_SAMPLER_PARAM_I] = { "iii", 0 },
    [GLC_SAMPLER_PARAM_F] = { "iif", 0 }, [GLC_FRAG_DATA_LOC] = { "is", R_INT }, [GLC_COPY_BUFFER_SUB] = { "iiiii", 0 },
    [GLC_GET_BUFFER_SUB] = { "iii", R_BYTES }, [GLC_BEGIN_QUERY] = { "ii", 0 }, [GLC_END_QUERY] = { "i", 0 },
    [GLC_QUERY_RESULT] = { "ii", R_INT }, [GLC_BIND_TF] = { "ii", 0 }, [GLC_BEGIN_TF] = { "i", 0 }, [GLC_END_TF] = { "", 0 },
    [GLC_TF_VARYINGS] = { "ii*s", 0 }, [GLC_PAUSE_TF] = { "", 0 }, [GLC_RESUME_TF] = { "", 0 },
};

/* the calls that are not commands */
enum { GLX_OPEN = 200, GLX_CREATE, GLX_PRESENT, GLX_TEX_ELEMENT, GLX_TEX_SUB_ELEMENT, GLX_UNPACK, GLX_TYPED_OK, GLX_LOST };

static page_t* host_page(interp_t* I) { return (page_t*)script_host(I); }
static long str_to_int_(const char* v) {
    long x = 0;
    while (*v == ' ') v++;
    for (; *v >= '0' && *v <= '9' && x < 100000; v++) x = x * 10 + (*v - '0');
    return x;
}

static webgl_t* gl_of(page_t* p, dom_node_t* n) {
    for (webgl_t* g = p->webgls; g; g = g->next) if (g->node == n) return g;
    return NULL;
}

void webgl_free_all(page_t* p) {
    for (webgl_t* g = p->webgls; g;) {
        webgl_t* next = g->next;
        if (g->id >= 0 && p->env && p->env->gl_close) p->env->gl_close(p->env->ctx, g->id);
        if (g->w) kfree(g->w);
        kfree(g);
        g = next;
    }
    p->webgls = NULL;
}

static int room(webgl_t* g, uint32_t words) {
    if (g->n + words > g->cap) {
        uint32_t nc = g->cap ? g->cap * 2 : 4096;
        while (nc < g->n + words) nc *= 2;
        if ((uint64_t)nc * 4 > WW_GL_COMMANDS) return 0;
        uint32_t* nw = kmalloc((size_t)nc * 4);
        if (!nw) return 0;
        if (g->n) memcpy(nw, g->w, (size_t)g->n * 4);
        if (g->w) kfree(g->w);
        g->w = nw;
        g->cap = nc;
    }
    return 1;
}
static void put(webgl_t* g, uint32_t v) { if (room(g, 1)) g->w[g->n++] = v; }
static void put_f(webgl_t* g, double d) { float f = (float)d; uint32_t v; memcpy(&v, &f, 4); put(g, v); }
static void put_bytes(webgl_t* g, const uint8_t* d, uint32_t n) {
    uint32_t words = (n + 3) / 4;
    if (!room(g, words + 1)) return;
    g->w[g->n++] = n;
    if (n) { g->w[g->n + words - 1] = 0; memcpy(g->w + g->n, d, n); }
    g->n += words;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* data: a typed array (read directly) or its bytes as hex (from LibJS); kmalloc'd if copied */
static const uint8_t* data_of(interp_t* I, value_t v, uint32_t* n, uint8_t** owned) {
    *owned = NULL;
    *n = 0;
    const uint8_t* d;
    if (typed_bytes(v, &d, n)) return d;
    if (v.t != V_STR) return NULL;
    const char* s = v_cstr(I, v);
    size_t len = strlen(s);
    uint8_t* o = kmalloc(len / 2 + 1);
    if (!o) return NULL;
    uint32_t k = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int hi = hexv(s[i]), lo = hexv(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        o[k++] = (uint8_t)(hi << 4 | lo);
    }
    *owned = o;
    *n = k;
    return o;
}

static double num(interp_t* I, int argc, value_t* argv, int i) {
    if (i >= argc) return 0;
    num_t v = v_tonum(I, argv[i]);
    return v != v ? 0 : (double)v;
}

/* the drawing buffer: the canvas's width / height, made smaller (the same
 * shape) past 4096 a side or WW_GL_PIXELS in all, as WebGL allows */
static void buffer_size(dom_node_t* n, int* bw, int* bh) {
    const char* aw = dom_attr(n, "width");
    const char* ah = dom_attr(n, "height");
    int w = aw ? (int)str_to_int_(aw) : 300, h = ah ? (int)str_to_int_(ah) : 150;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) { h = (int)((int64_t)h * 4096 / w); w = 4096; }
    if (h > 4096) { w = (int)((int64_t)w * 4096 / h); h = 4096; }
    while ((uint64_t)w * (uint64_t)h > WW_GL_PIXELS) { w = w * 7 / 8; h = h * 7 / 8; }
    *bw = w < 1 ? 1 : w;
    *bh = h < 1 ? 1 : h;
}

static void follow_size(webgl_t* g) {
    int w, h;
    buffer_size(g->node, &w, &h);
    if (w != g->bw || h != g->bh) {
        g->bw = w; g->bh = h;
        put(g, GLC_RESIZE); put(g, 2); put(g, (uint32_t)w); put(g, (uint32_t)h);
    }
}

/* the queued commands, run: the reply (kmalloc'd) or NULL */
static uint8_t* flush(page_t* p, webgl_t* g, int read, uint32_t* len) {
    if (g->id < 0 || !p->env || !p->env->gl_exec) { g->n = 0; return NULL; }
    uint8_t* reply = NULL;
    if (p->env->gl_exec(p->env->ctx, g->id, (const uint8_t*)g->w, g->n * 4, read, g->bw, g->bh, &reply, len) != 0) {
        g->id = -1;                                  /* the context is lost */
        g->n = 0;
        return NULL;
    }
    g->n = 0;
    return reply;
}

static size_t bpp(int fmt, int type) {
    int comps = fmt == 0x1908 ? 4 : fmt == 0x1907 ? 3 : fmt == 0x190A ? 2 : 1;   /* RGBA RGB LUMINANCE_ALPHA, else 1 */
    if (type == 0x8033 || type == 0x8034 || type == 0x8363) return 2;              /* packed 16-bit */
    if (type == 0x1406) return (size_t)comps * 4;                                  /* FLOAT */
    if (type == 0x8D61 || type == 0x140B) return (size_t)comps * 2;                /* HALF_FLOAT (OES) */
    return (size_t)comps;
}

/* typed-array pixels: flipped / premultiplied as the unpack flags say */
static uint8_t* unpack(webgl_t* g, const uint8_t* d, uint32_t n, int w, int h, int fmt, int type, int* changed) {
    *changed = 0;
    if ((!g->flip_y && !g->premul) || w <= 0 || h <= 0) return (uint8_t*)d;
    size_t row = bpp(fmt, type) * (size_t)w;
    row = (row + 3) & ~(size_t)3;                     /* (UNPACK_ALIGNMENT 4) */
    if (row * (size_t)h > n) return (uint8_t*)d;
    uint8_t* o = kmalloc(row * (size_t)h);
    if (!o) return (uint8_t*)d;
    for (int y = 0; y < h; y++) memcpy(o + row * y, d + row * (size_t)(g->flip_y ? h - 1 - y : y), row);
    if (g->premul && fmt == 0x1908 && type == 0x1401)
        for (size_t i = 0; i + 3 < row * (size_t)h; i += 4) { int a = o[i + 3]; o[i] = (uint8_t)(o[i] * a / 255); o[i + 1] = (uint8_t)(o[i + 1] * a / 255); o[i + 2] = (uint8_t)(o[i + 2] * a / 255); }
    *changed = 1;
    return o;
}

static value_t answer(interp_t* I, int res, const uint8_t* r, uint32_t n) {
    switch (res) {
    case R_INT: { int v = 0; if (n >= 4) memcpy(&v, r, 4); return v_num(v); }
    case R_STR: {
        int len = 0;
        if (n >= 4) memcpy(&len, r, 4);
        if (len < 0 || (uint32_t)len > n - 4) len = 0;
        return v_strn(I, (const char*)r + 4, (uint32_t)len);
    }
    case R_FLOATS: {
        int cnt = 0;
        if (n >= 4) memcpy(&cnt, r, 4);
        if (cnt < 0 || cnt > 16 || (uint32_t)cnt * 4 + 4 > n) cnt = 0;
        obj_t* a = obj_new(I, OBJ_ARRAY);
        for (int i = 0; i < cnt; i++) { float f; memcpy(&f, r + 4 + i * 4, 4); arr_push(I, a, v_num(f)); }
        return v_obj(a);
    }
    case R_ACTIVE: {
        int size = 0, type = 0, len = 0;
        if (n >= 12) { memcpy(&size, r, 4); memcpy(&type, r + 4, 4); memcpy(&len, r + 8, 4); }
        if (size <= 0) return v_null();
        if (len < 0 || (uint32_t)len > n - 12) len = 0;
        obj_t* o = obj_new(I, OBJ_PLAIN);
        obj_set(I, o, "size", v_num(size));
        obj_set(I, o, "type", v_num(type));
        obj_set(I, o, "name", v_strn(I, (const char*)r + 12, (uint32_t)len));
        return v_obj(o);
    }
    case R_BYTES: {                                  /* base64 (ASCII: the same in both engines) */
        int len = 0;
        if (n >= 4) memcpy(&len, r, 4);
        if (len <= 0 || (uint32_t)len > n - 4) return v_str(I, "");
        static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t m = ((size_t)len + 2) / 3 * 4;
        char* o = kmalloc(m + 1);
        if (!o) return v_str(I, "");
        const uint8_t* d = r + 4;
        size_t k = 0;
        for (int i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < len ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < len ? d[i + 2] : 0);
            o[k++] = B[v >> 18 & 63]; o[k++] = B[v >> 12 & 63];
            o[k++] = i + 1 < len ? B[v >> 6 & 63] : '='; o[k++] = i + 2 < len ? B[v & 63] : '=';
        }
        value_t s = v_strn(I, o, (uint32_t)k);
        kfree(o);
        return s;
    }
    default: return v_undef();
    }
}

static value_t gl_call(interp_t* I, value_t self, int argc, value_t* argv) {
    (void)self;
    page_t* p = host_page(I);
    if (argc < 2) return v_undef();
    dom_node_t* n = jsdom_node_of(argv[0]);
    int op = (int)num(I, argc, argv, 1);
    if (!n || n->type != DOM_ELEM || strcmp(n->tag, "canvas")) return v_undef();
    webgl_t* g = gl_of(p, n);
    if (op == GLX_TYPED_OK) { const uint8_t* d; uint32_t len; return v_bool(argc > 2 && typed_bytes(argv[2], &d, &len)); }
    if (op == GLX_OPEN) {                            /* (premultiplied, version wanted) -> version made, 0 */
        if (g) return v_num(g->id >= 0 ? g->version : 0);
        if (!p->env || !p->env->gl_open) return v_num(0);
        g = kzalloc(sizeof(*g));
        if (!g) return v_num(0);
        g->node = n;
        g->premultiplied = (int)num(I, argc, argv, 2);
        g->version = (int)num(I, argc, argv, 3) == 3 ? 3 : 2;
        buffer_size(n, &g->bw, &g->bh);
        g->id = p->env->gl_open(p->env->ctx, g->bw, g->bh, &g->version);
        if (g->id < 0) { kfree(g); return v_num(0); }
        g->next = p->webgls;
        p->webgls = g;
        return v_num(g->version);
    }
    if (!g || g->id < 0) return op == GLX_LOST ? v_bool(1) : v_undef();
    if (op == GLX_LOST) return v_bool(0);
    follow_size(g);
    switch (op) {
    case GLX_CREATE: {                               /* (kind, shader type) -> number */
        int kind = (int)num(I, argc, argv, 2);
        if (kind < 0 || kind >= GLK_KINDS || g->next_id[kind] + 1 >= GLC_MAX_IDS) return v_num(0);
        uint32_t id = ++g->next_id[kind];
        put(g, GLC_CREATE); put(g, 3); put(g, (uint32_t)kind); put(g, id); put(g, (uint32_t)num(I, argc, argv, 3));
        return v_num(id);
    }
    case GLX_UNPACK: g->flip_y = (int)num(I, argc, argv, 2); g->premul = (int)num(I, argc, argv, 3); return v_undef();
    case GLX_PRESENT: {
        uint32_t len = 0;
        uint8_t* r = flush(p, g, 1, &len);
        if (!r) return v_bool(0);
        ww_gl_reply_t h;
        memcpy(&h, r, sizeof(h));
        size_t need = sizeof(h) + (size_t)(h.result_bytes > 0 ? h.result_bytes : 0) + (size_t)h.width * h.height * 4;
        if (h.width > 0 && h.height > 0 && need <= len)
            canvas_put_rgba(p, n, h.width, h.height, r + sizeof(h) + (h.result_bytes > 0 ? h.result_bytes : 0), g->premultiplied);
        kfree(r);
        return v_bool(1);
    }
    case GLX_TEX_ELEMENT: case GLX_TEX_SUB_ELEMENT: {
        /* (target, level, internal format | x, [y,] format, type, element): its pixels */
        int sub = op == GLX_TEX_SUB_ELEMENT;
        int target = (int)num(I, argc, argv, 2), level = (int)num(I, argc, argv, 3);
        int a = (int)num(I, argc, argv, 4), b = sub ? (int)num(I, argc, argv, 5) : 0;
        int fmt = (int)num(I, argc, argv, sub ? 6 : 5), type = (int)num(I, argc, argv, sub ? 7 : 6);
        dom_node_t* src = jsdom_node_of(argc > (sub ? 8 : 7) ? argv[sub ? 8 : 7] : v_undef());
        int w = 0, h = 0;
        uint8_t* px = canvas_element_rgba(p, src, &w, &h);
        if (!px) return v_bool(0);
        if (g->flip_y) {
            size_t row = (size_t)w * 4;
            uint8_t* tmp = kmalloc(row);
            if (tmp) {
                for (int y = 0; y < h / 2; y++) { memcpy(tmp, px + row * y, row); memcpy(px + row * y, px + row * (size_t)(h - 1 - y), row); memcpy(px + row * (size_t)(h - 1 - y), tmp, row); }
                kfree(tmp);
            }
        }
        if (g->premul)
            for (size_t i = 0; i < (size_t)w * h * 4; i += 4) { int al = px[i + 3]; px[i] = (uint8_t)(px[i] * al / 255); px[i + 1] = (uint8_t)(px[i + 1] * al / 255); px[i + 2] = (uint8_t)(px[i + 2] * al / 255); }
        /* (the image's RGBA, whatever format was asked: GL converts) */
        uint32_t words = ((uint32_t)w * h * 4 + 3) / 4;
        if (!sub) {
            put(g, GLC_TEX_IMAGE); put(g, 7 + 1 + words);
            put(g, (uint32_t)target); put(g, (uint32_t)level); put(g, (uint32_t)a); put(g, (uint32_t)w); put(g, (uint32_t)h);
            put(g, 0x1908); put(g, 0x1401);
        } else {
            put(g, GLC_TEX_SUB_IMAGE); put(g, 8 + 1 + words);
            put(g, (uint32_t)target); put(g, (uint32_t)level); put(g, (uint32_t)a); put(g, (uint32_t)b); put(g, (uint32_t)w); put(g, (uint32_t)h);
            put(g, 0x1908); put(g, 0x1401);
        }
        (void)fmt; (void)type;
        put_bytes(g, px, (uint32_t)w * h * 4);
        kfree(px);
        return v_bool(1);
    }
    default: break;
    }
    if (op <= 0 || op >= GLC_COUNT || !SIG[op].sig) return v_undef();
    /* a command: its arguments as its signature says */
    const char* sig = SIG[op].sig;
    uint32_t at = g->n;
    put(g, (uint32_t)op);
    put(g, 0);
    int ai = 2;
    uint8_t* owned = NULL;
    uint8_t* unpacked = NULL;
    for (const char* c = sig; *c; c++) {
        char k = *c;
        if (k == '*') {                              /* the rest, like the next letter */
            k = c[1];
            for (; ai < argc; ai++) {
                if (k == 'f') put_f(g, num(I, argc, argv, ai));
                else if (k == 's') { const char* s = v_cstr(I, argv[ai]); put_bytes(g, (const uint8_t*)s, (uint32_t)strlen(s)); }
                else put(g, (uint32_t)(int64_t)num(I, argc, argv, ai));
            }
            break;
        }
        if (k == 'i') put(g, (uint32_t)(int64_t)num(I, argc, argv, ai++));
        else if (k == 'f') put_f(g, num(I, argc, argv, ai++));
        else if (k == 's') { const char* s = ai < argc ? v_cstr(I, argv[ai]) : ""; ai++; put_bytes(g, (const uint8_t*)s, (uint32_t)strlen(s)); }
        else if (k == 'b' || k == 'w') {
            uint32_t len = 0;
            const uint8_t* d = ai < argc && argv[ai].t != V_UNDEF && argv[ai].t != V_NULL ? data_of(I, argv[ai], &len, &owned) : NULL;
            ai++;
            if (k == 'b' && (op == GLC_TEX_IMAGE || op == GLC_TEX_SUB_IMAGE) && d) {
                int sub = op == GLC_TEX_SUB_IMAGE;
                int w = (int)num(I, argc, argv, sub ? 6 : 5), h = (int)num(I, argc, argv, sub ? 7 : 6);
                int fmt = (int)num(I, argc, argv, sub ? 8 : 7), type = (int)num(I, argc, argv, sub ? 9 : 8);
                int changed;
                uint8_t* u = unpack(g, d, len, w, h, fmt, type, &changed);
                if (changed) { unpacked = u; d = u; }
            }
            if (k == 'b') put_bytes(g, d, d ? len : 0);
            else if (d && len) {                     /* raw words */
                uint32_t words = len / 4;
                if (room(g, words)) { memcpy(g->w + g->n, d, (size_t)words * 4); g->n += words; }
            }
            if (owned) { kfree(owned); owned = NULL; }
            if (unpacked) { kfree(unpacked); unpacked = NULL; }
        }
    }
    if (at + 2 <= g->n) g->w[at + 1] = g->n - at - 2;           /* the words that follow */
    if (!SIG[op].res) return v_undef();
    uint32_t len = 0;
    uint8_t* r = flush(p, g, 0, &len);
    if (!r) return SIG[op].res == R_INT ? v_num(op == GLC_GET_ERROR ? 0x9242 : 0) : v_null();   /* CONTEXT_LOST_WEBGL */
    ww_gl_reply_t h;
    memcpy(&h, r, sizeof(h));
    uint32_t rn = h.result_bytes > 0 && (uint32_t)h.result_bytes <= len - sizeof(h) ? (uint32_t)h.result_bytes : 0;
    value_t v = answer(I, SIG[op].res, r + sizeof(h), rn);
    kfree(r);
    return v;
}

void webgl_install(interp_t* I) {
    script_def_global(I, "__gl", v_native(I, "__gl", gl_call));
}
#endif
