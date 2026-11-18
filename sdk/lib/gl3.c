/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * OpenGL 3.3 for Banana OS apps (USE_GL3 = 1): a GL 3.3 core context from
 * Mesa's OSMesa (libOSMesa.so, softpipe: drawing on the processor), loaded
 * with the SDK's dlopen the first time a context is made. Mesa draws into a
 * buffer of ours (rows top first, 0xAARRGGBB); bgl3_swap copies it into
 * the window. On a GPU (r600, crocus, iris, VirGL) a window's frames are
 * read back a frame behind instead, through two pixel buffers the GPU
 * fills while the app draws the next frame: OSMesa's own copy at every
 * glFinish made the processor wait for the GPU and the GPU for the
 * processor (Minecraft on a Radeon HD 7450: the GPU 30 % busy). The GL functions are pointers (<GL/gl3procs.h>) filled from
 * OSMesaGetProcAddress.
 */
#include "GL/gl3.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(type, name) type bgl3_##name;
BGL3_PROCS(X)
#undef X

/* OSMesa (GL/osmesa.h) */
typedef struct osmesa_context* OSMesaContext;
#define OSMESA_BGRA                  0x1
#define OSMESA_Y_UP                  0x11
#define OSMESA_FORMAT                0x22
#define OSMESA_DEPTH_BITS            0x30
#define OSMESA_STENCIL_BITS          0x31
#define OSMESA_PROFILE               0x33
#define OSMESA_CORE_PROFILE          0x34
#define OSMESA_COMPAT_PROFILE        0x35
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
static OSMesaContext (*os_create)(const int* attribs, OSMesaContext share);
static void (*os_destroy)(OSMesaContext c);
static GLboolean (*os_make_current)(OSMesaContext c, void* buf, GLenum type, GLsizei w, GLsizei h);
static void (*os_pixel_store)(GLint pname, GLint value);
static void* (*os_proc)(const char* name);
static void (*os_snapshot)(GLboolean on);    /* (Banana's OSMesa: frames left to us, or copied at each flush) */

struct bgl3_ctx {
    OSMesaContext os;
    bwin_t* win;
    int w, h;
    unsigned int* px;               /* Mesa's color buffer: w x h, top row first */
    int async;                      /* a window on a GPU: frames read back a frame behind */
    GLuint pbo[2];                  /* ...into these, in turn */
    int pbo_w, pbo_h, next, pending;
};

static void* g_lib;
static char g_err[256];
static bgl3_ctx_t* g_cur;

const char* bgl3_error(void) { return g_err; }

static int load(void) {
    if (g_lib) return 0;
    const char* env = getenv("BGL3_LIBRARY");
    static const char* const where[] = {
        "libOSMesa.so", "/lib/libOSMesa.so", "/usr/lib/libOSMesa.so", "/usr/java/lib/libOSMesa.so",
        "/mnt/usb/lib/libOSMesa.so", "/mnt/virtio/lib/libOSMesa.so", "/mnt/sata/lib/libOSMesa.so", NULL,
    };
    void* lib = env ? dlopen(env, RTLD_NOW | RTLD_GLOBAL) : NULL;
    /* (a copy: dlerror's text is overwritten by the next failure) */
    char why[200] = "";
    if (env && !lib) snprintf(why, sizeof(why), "%s", dlerror());
    for (int i = 0; !lib && where[i]; i++) {
        lib = dlopen(where[i], RTLD_NOW | RTLD_GLOBAL);
        const char* e = lib ? NULL : dlerror();
        if (e && !strstr(e, "No such file")) snprintf(why, sizeof(why), "%s", e);   /* (found, but it did not load) */
    }
    if (!lib) {
        snprintf(g_err, sizeof(g_err), "OpenGL 3.3: %s", why[0] ? why : "libOSMesa.so not found (/lib, /usr/lib, /usr/java/lib, /mnt/*/lib)");
        return -1;
    }
    *(void**)&os_create = dlsym(lib, "OSMesaCreateContextAttribs");
    *(void**)&os_destroy = dlsym(lib, "OSMesaDestroyContext");
    *(void**)&os_make_current = dlsym(lib, "OSMesaMakeCurrent");
    *(void**)&os_pixel_store = dlsym(lib, "OSMesaPixelStore");
    *(void**)&os_proc = dlsym(lib, "OSMesaGetProcAddress");
    *(void**)&os_snapshot = os_proc ? os_proc("OSMesaBananaSnapshot") : NULL;   /* (not in older libraries) */
    if (!os_create || !os_destroy || !os_make_current || !os_pixel_store || !os_proc) {
        snprintf(g_err, sizeof(g_err), "OpenGL 3.3: libOSMesa.so lacks the OSMesa functions");
        dlclose(lib);
        return -1;
    }
    g_lib = lib;
    return 0;
}

/* the GL function pointers, from the current context's Mesa */
static int load_procs(void) {
    int missing = 0;
#define X(type, name) if (!(bgl3_##name = (type)os_proc(#name))) missing++;
    BGL3_PROCS(X)
#undef X
    if (missing) snprintf(g_err, sizeof(g_err), "OpenGL 3.3: %d functions missing in libOSMesa.so", missing);
    return missing ? -1 : 0;
}

void* bgl3_proc(const char* name) { return os_proc && name ? os_proc(name) : NULL; }

static int alloc_px(bgl3_ctx_t* c, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned int* p = calloc((size_t)w * h, 4);
    if (!p) return -1;
    free(c->px);
    c->px = p;
    c->w = w;
    c->h = h;
    return 0;
}

/* an OSMesa context: profile BGL3_CORE or BGL3_COMPAT; major 0 = no
 * version asked (the compatibility profile: Mesa's highest) or, for the
 * core profile, the highest from 4.6 down that Mesa makes */
static OSMesaContext os_context(int profile, int major, int minor, OSMesaContext share) {
    int at[16], n = 0;
    at[n++] = OSMESA_FORMAT; at[n++] = OSMESA_BGRA;
    at[n++] = OSMESA_DEPTH_BITS; at[n++] = 24;
    at[n++] = OSMESA_STENCIL_BITS; at[n++] = 8;
    at[n++] = OSMESA_PROFILE; at[n++] = profile == BGL3_COMPAT ? OSMESA_COMPAT_PROFILE : OSMESA_CORE_PROFILE;
    if (major > 0) {
        at[n++] = OSMESA_CONTEXT_MAJOR_VERSION; at[n++] = major;
        at[n++] = OSMESA_CONTEXT_MINOR_VERSION; at[n++] = minor;
        at[n] = 0;
        return os_create(at, share);
    }
    at[n] = 0;
    if (profile == BGL3_COMPAT) return os_create(at, share);
    static const int V[][2] = { { 4, 6 }, { 4, 5 }, { 4, 4 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 }, { 3, 3 }, { 3, 2 } };
    for (unsigned i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        at[n] = OSMESA_CONTEXT_MAJOR_VERSION; at[n + 1] = V[i][0];
        at[n + 2] = OSMESA_CONTEXT_MINOR_VERSION; at[n + 3] = V[i][1];
        at[n + 4] = 0;
        OSMesaContext c = os_create(at, share);
        if (c) return c;
    }
    return NULL;
}

static bgl3_ctx_t* create_v(bwin_t* win, int w, int h, int profile, int major, int minor, bgl3_ctx_t* share) {
    g_err[0] = 0;
    if (load()) return NULL;
    bgl3_ctx_t* c = calloc(1, sizeof(*c));
    if (!c || alloc_px(c, w, h)) { free(c); snprintf(g_err, sizeof(g_err), "OpenGL: out of memory"); return NULL; }
    c->win = win;
    c->os = os_context(profile, major, minor, share ? share->os : NULL);
    if (!c->os) {
        if (major > 0) snprintf(g_err, sizeof(g_err), "OpenGL: Mesa could not make a %d.%d %s context", major, minor, profile == BGL3_COMPAT ? "compatibility" : "core");
        else snprintf(g_err, sizeof(g_err), "OpenGL: Mesa could not make a %s context", profile == BGL3_COMPAT ? "compatibility" : "core");
        free(c->px);
        free(c);
        return NULL;
    }
    if (bgl3_make_current(c) || load_procs()) {
        bgl3_destroy(c);
        if (!g_err[0]) snprintf(g_err, sizeof(g_err), "OpenGL 3.3: the context could not be made current");
        return NULL;
    }
    /* a window drawn by a GPU: its frames read back a frame behind (BGL3_SYNC=1: as before;
     * BGL3_ASYNC=1: on the processor's renderers too - the tests) */
    const char* r = (const char*)bgl3_glGetString(GL_RENDERER);
    const char* sync = getenv("BGL3_SYNC");
    const char* force = getenv("BGL3_ASYNC");
    int gpu = r && !strstr(r, "softpipe") && !strstr(r, "llvmpipe");
    if (win && os_snapshot && (gpu || (force && *force == '1')) && !(sync && *sync == '1')) {
        c->async = 1;
        os_snapshot(0);
    }
    return c;
}

static bgl3_ctx_t* create(bwin_t* win, int w, int h, int compat) {
    return compat ? create_v(win, w, h, BGL3_COMPAT, 0, 0, NULL) : create_v(win, w, h, BGL3_CORE, 3, 3, NULL);
}

bgl3_ctx_t* bgl3_create_version(bwin_t* win, int w, int h, int profile, int major, int minor, bgl3_ctx_t* share) {
    if (win && win->id >= 0) { w = win->w; h = win->h; }
    else win = NULL;
    return create_v(win, w, h, profile, major, minor, share);
}

int bgl3_make_current_buffer(bgl3_ctx_t* c, unsigned int* px, int w, int h) {
    if (!c || !px) return bgl3_make_current(c);
    if (!os_make_current(c->os, px, GL_UNSIGNED_BYTE, w, h)) return -1;
    os_pixel_store(OSMESA_Y_UP, 0);
    if (os_snapshot) os_snapshot(1);                   /* (the app's own buffer: OSMesa fills it) */
    g_cur = c;
    return 0;
}

bgl3_ctx_t* bgl3_create(bwin_t* win) {
    if (!win || win->id < 0) { snprintf(g_err, sizeof(g_err), "OpenGL 3.3: no window"); return NULL; }
    return create(win, win->w, win->h, 0);
}

bgl3_ctx_t* bgl3_create_compat(bwin_t* win) {
    if (!win || win->id < 0) { snprintf(g_err, sizeof(g_err), "OpenGL: no window"); return NULL; }
    return create(win, win->w, win->h, 1);
}

bgl3_ctx_t* bgl3_create_offscreen(int w, int h) { return create(NULL, w, h, 0); }

int bgl3_make_current(bgl3_ctx_t* c) {
    if (!c) { if (os_make_current) os_make_current(NULL, NULL, 0, 0, 0); g_cur = NULL; return 0; }
    if (!os_make_current(c->os, c->px, GL_UNSIGNED_BYTE, c->w, c->h)) return -1;
    os_pixel_store(OSMESA_Y_UP, 0);                    /* row 0 at the top, as windows */
    if (os_snapshot) os_snapshot(!c->async);
    g_cur = c;
    return 0;
}

bgl3_ctx_t* bgl3_current(void) { return g_cur; }

void bgl3_resize(bgl3_ctx_t* c) {
    if (!c || !c->win || (c->win->w == c->w && c->win->h == c->h)) return;
    if (bgl3_glFinish) bgl3_glFinish();
    if (alloc_px(c, c->win->w, c->win->h) == 0 && g_cur == c) bgl3_make_current(c);
}

/* the frame read into one pixel buffer by the GPU, the one before copied
 * from the other into the window - the app's GL state kept as it was */
static void swap_async(bgl3_ctx_t* c) {
    GLint pack = 0, fb = 0, rb = 0, al = 4, rl = 0, sp = 0, sr = 0;
    bgl3_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    bgl3_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fb);
    bgl3_glGetIntegerv(GL_PACK_ALIGNMENT, &al);
    bgl3_glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    bgl3_glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
    bgl3_glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
    bgl3_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    bgl3_glGetIntegerv(GL_READ_BUFFER, &rb);
    bgl3_glReadBuffer(GL_FRONT);
    bgl3_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    bgl3_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    bgl3_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    bgl3_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    GLsizeiptr bytes = (GLsizeiptr)c->w * c->h * 4;
    if (!c->pbo[0] || c->pbo_w != c->w || c->pbo_h != c->h) {
        if (!c->pbo[0]) bgl3_glGenBuffers(2, c->pbo);
        for (int i = 0; i < 2; i++) {
            bgl3_glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[i]);
            bgl3_glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        }
        c->pbo_w = c->w; c->pbo_h = c->h; c->next = 0; c->pending = 0;
    }
    /* this frame: into the GPU's queue */
    bgl3_glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[c->next]);
    bgl3_glReadPixels(0, 0, c->w, c->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 0);
    bgl3_glFlush();
    /* the one before: done by now (or nearly), into the window */
    if (c->pending) {
        bgl3_glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[c->next ^ 1]);
        const unsigned int* p = (const unsigned int*)bgl3_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (p) {
            int w = c->w < c->win->w ? c->w : c->win->w, h = c->h < c->win->h ? c->h : c->win->h;
            for (int y = 0; y < h; y++) {
                const unsigned int* s = p + (long)(c->h - 1 - y) * c->w;     /* (GL's rows: bottom first) */
                unsigned int* d = c->win->px + (long)y * c->win->w;
                for (int x = 0; x < w; x++) d[x] = s[x] & 0x00FFFFFFu;
            }
            bgl3_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    c->pending = 1;
    c->next ^= 1;
    bgl3_glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    bgl3_glReadBuffer((GLenum)rb);
    bgl3_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)fb);
    bgl3_glPixelStorei(GL_PACK_ALIGNMENT, al);
    bgl3_glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    bgl3_glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
    bgl3_glPixelStorei(GL_PACK_SKIP_ROWS, sr);
    bwin_update(c->win);
}

void bgl3_swap(bgl3_ctx_t* c) {
    if (!c) return;
    if (c->async && c->win && c->win->px && bgl3_current() == c) { swap_async(c); return; }
    bgl3_glFinish();
    if (!c->win || !c->win->px) return;
    int w = c->w < c->win->w ? c->w : c->win->w, h = c->h < c->win->h ? c->h : c->win->h;
    for (int y = 0; y < h; y++) {
        const unsigned int* s = c->px + (long)y * c->w;
        unsigned int* d = c->win->px + (long)y * c->win->w;
        for (int x = 0; x < w; x++) d[x] = s[x] & 0x00FFFFFFu;
    }
    bwin_update(c->win);
}

const unsigned int* bgl3_pixels(bgl3_ctx_t* c, int* w, int* h) {
    if (!c) return NULL;
    if (c->async && os_snapshot && bgl3_current() == c) { os_snapshot(1); bgl3_glFinish(); os_snapshot(0); }   /* (this once: the frame as it is) */
    else bgl3_glFinish();
    if (w) *w = c->w;
    if (h) *h = c->h;
    return c->px;
}

void bgl3_destroy(bgl3_ctx_t* c) {
    if (!c) return;
    if (g_cur == c) bgl3_make_current(NULL);
    if (c->os) os_destroy(c->os);
    free(c->px);
    free(c);
}
