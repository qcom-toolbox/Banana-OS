/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A page's WebGL context, played back with OpenGL ES 2.0 (EGL, Mesa: the GPU
 * where Mesa drives one - Intel crocus / iris, VirGL - else its software
 * renderer). The command stream (web/webgl_ops.h) comes from the page's
 * worker through the kernel; every argument is checked against the stream's
 * bounds and every object number against the table, so a hostile page can
 * only make GL errors. The answer to the last command and, when asked, the
 * drawing buffer (RGBA, top row first) go back. */
#include <banana.h>
#include <banana_web_worker.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "../../web/webgl_ops.h"

static EGLDisplay g_dpy;
static EGLConfig g_cfg;
static EGLContext g_ctx;
static EGLSurface g_surf;
static int g_w, g_h;
static GLuint* g_names[GLK_KINDS];             /* page number -> GL name */

/* the reply being built */
static unsigned char* g_res;
static unsigned int g_res_n, g_res_cap;

static void res_put(const void* d, unsigned int n) {
    if (g_res_n + n > g_res_cap) {
        unsigned int nc = (g_res_cap ? g_res_cap * 2 : 256) + n;
        unsigned char* r = realloc(g_res, nc);
        if (!r) return;
        g_res = r;
        g_res_cap = nc;
    }
    memcpy(g_res + g_res_n, d, n);
    g_res_n += n;
}
static void res_int(int v) { res_put(&v, 4); }
static void res_str(const char* s, int n) { if (n < 0) n = 0; res_int(n); res_put(s, (unsigned)n); }

static int surface(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    EGLint attrs[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(g_dpy, g_cfg, attrs);
    if (s == EGL_NO_SURFACE) return -1;
    if (!eglMakeCurrent(g_dpy, s, s, g_ctx)) { eglDestroySurface(g_dpy, s); return -1; }
    if (g_surf != EGL_NO_SURFACE && g_surf) eglDestroySurface(g_dpy, g_surf);
    g_surf = s;
    g_w = w;
    g_h = h;
    return 0;
}

static int init(int w, int h) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, NULL, NULL)) return -1;
    EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, want, &g_cfg, 1, &n) || n < 1) return -1;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ca);
    if (g_ctx == EGL_NO_CONTEXT) return -1;
    for (int k = 0; k < GLK_KINDS; k++) {
        g_names[k] = calloc(GLC_MAX_IDS, sizeof(GLuint));
        if (!g_names[k]) return -1;
    }
    if (surface(w, h)) return -1;
    glViewport(0, 0, g_w, g_h);
    return 0;
}

static GLuint name_of(int kind, unsigned int id) {
    if (kind < 0 || kind >= GLK_KINDS || id >= GLC_MAX_IDS) return 0;
    return g_names[kind][id];
}

static void create(int kind, unsigned int id, unsigned int type) {
    if (kind < 0 || kind >= GLK_KINDS || !id || id >= GLC_MAX_IDS || g_names[kind][id]) return;
    GLuint n = 0;
    switch (kind) {
    case GLK_BUFFER: glGenBuffers(1, &n); break;
    case GLK_TEXTURE: glGenTextures(1, &n); break;
    case GLK_FRAMEBUFFER: glGenFramebuffers(1, &n); break;
    case GLK_RENDERBUFFER: glGenRenderbuffers(1, &n); break;
    case GLK_PROGRAM: n = glCreateProgram(); break;
    case GLK_SHADER: if (type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER) n = glCreateShader(type); break;
    }
    g_names[kind][id] = n;
}

static void destroy(int kind, unsigned int id) {
    GLuint n = name_of(kind, id);
    if (!n) return;
    switch (kind) {
    case GLK_BUFFER: glDeleteBuffers(1, &n); break;
    case GLK_TEXTURE: glDeleteTextures(1, &n); break;
    case GLK_FRAMEBUFFER: glDeleteFramebuffers(1, &n); break;
    case GLK_RENDERBUFFER: glDeleteRenderbuffers(1, &n); break;
    case GLK_PROGRAM: glDeleteProgram(n); break;
    case GLK_SHADER: glDeleteShader(n); break;
    }
    g_names[kind][id] = 0;
}

/* the stream, read with bounds */
typedef struct { const uint32_t* w; unsigned int n, i; int bad; } rd_t;
static uint32_t u(rd_t* r) { if (r->i >= r->n) { r->bad = 1; return 0; } return r->w[r->i++]; }
static int si(rd_t* r) { return (int)u(r); }
static float f(rd_t* r) { uint32_t v = u(r); float x; memcpy(&x, &v, 4); return x; }
/* a byte string: its count, then the bytes (padded to 4) */
static const unsigned char* bytes(rd_t* r, unsigned int* len) {
    unsigned int n = u(r), words = (n + 3) / 4;
    if (r->bad || words > r->n - r->i) { r->bad = 1; *len = 0; return NULL; }
    const unsigned char* p = (const unsigned char*)(r->w + r->i);
    r->i += words;
    *len = n;
    return p;
}

static char* cstr(rd_t* r) {
    unsigned int n;
    const unsigned char* p = bytes(r, &n);
    char* s = malloc(n + 1);
    if (!s) return NULL;
    if (p) memcpy(s, p, n);
    s[p ? n : 0] = 0;
    return s;
}

static void run(rd_t* R) {
    while (R->i + 2 <= R->n && !R->bad) {
        unsigned int op = u(R), nw = u(R);
        if (nw > R->n - R->i) { R->bad = 1; break; }
        rd_t r = { R->w + R->i, nw, 0, 0 };
        R->i += nw;
        switch (op) {
        case GLC_RESIZE: { int w = si(&r), h = si(&r); if (w != g_w || h != g_h) surface(w, h); break; }
        case GLC_CREATE: { int k = si(&r); unsigned id = u(&r), t = r.i < r.n ? u(&r) : 0; create(k, id, t); break; }
        case GLC_DELETE: { int k = si(&r); destroy(k, u(&r)); break; }
        case GLC_SHADER_SOURCE: {
            GLuint s = name_of(GLK_SHADER, u(&r));
            char* src = cstr(&r);
            if (s && src) { const char* list[1] = { src }; glShaderSource(s, 1, list, NULL); }
            free(src);
            break;
        }
        case GLC_COMPILE: { GLuint s = name_of(GLK_SHADER, u(&r)); if (s) glCompileShader(s); break; }
        case GLC_ATTACH: case GLC_DETACH: {
            GLuint p = name_of(GLK_PROGRAM, u(&r)), s = name_of(GLK_SHADER, u(&r));
            if (p && s) { if (op == GLC_ATTACH) glAttachShader(p, s); else glDetachShader(p, s); }
            break;
        }
        case GLC_LINK: { GLuint p = name_of(GLK_PROGRAM, u(&r)); if (p) glLinkProgram(p); break; }
        case GLC_USE: { unsigned id = u(&r); glUseProgram(id ? name_of(GLK_PROGRAM, id) : 0); break; }
        case GLC_VALIDATE: { GLuint p = name_of(GLK_PROGRAM, u(&r)); if (p) glValidateProgram(p); break; }
        case GLC_SHADER_PARAM: case GLC_PROGRAM_PARAM: {
            GLuint o = name_of(op == GLC_SHADER_PARAM ? GLK_SHADER : GLK_PROGRAM, u(&r));
            GLenum pn = u(&r);
            GLint v = 0;
            if (o) { if (op == GLC_SHADER_PARAM) glGetShaderiv(o, pn, &v); else glGetProgramiv(o, pn, &v); }
            res_int(v);
            break;
        }
        case GLC_SHADER_LOG: case GLC_PROGRAM_LOG: {
            GLuint o = name_of(op == GLC_SHADER_LOG ? GLK_SHADER : GLK_PROGRAM, u(&r));
            char log[4096];
            GLsizei n = 0;
            log[0] = 0;
            if (o) { if (op == GLC_SHADER_LOG) glGetShaderInfoLog(o, sizeof(log), &n, log); else glGetProgramInfoLog(o, sizeof(log), &n, log); }
            res_str(log, n);
            break;
        }
        case GLC_ATTRIB_LOC: case GLC_UNIFORM_LOC: {
            GLuint p = name_of(GLK_PROGRAM, u(&r));
            char* nm = cstr(&r);
            int v = -1;
            if (p && nm) v = op == GLC_ATTRIB_LOC ? glGetAttribLocation(p, nm) : glGetUniformLocation(p, nm);
            free(nm);
            res_int(v);
            break;
        }
        case GLC_BIND_ATTRIB_LOC: {
            GLuint p = name_of(GLK_PROGRAM, u(&r));
            unsigned idx = u(&r);
            char* nm = cstr(&r);
            if (p && nm) glBindAttribLocation(p, idx, nm);
            free(nm);
            break;
        }
        case GLC_ACTIVE: {
            GLuint p = name_of(GLK_PROGRAM, u(&r));
            int uni = si(&r);
            unsigned idx = u(&r);
            char nm[256];
            GLsizei n = 0;
            GLint size = 0;
            GLenum type = 0;
            nm[0] = 0;
            if (p) { if (uni) glGetActiveUniform(p, idx, sizeof(nm), &n, &size, &type, nm); else glGetActiveAttrib(p, idx, sizeof(nm), &n, &size, &type, nm); }
            res_int(size);
            res_int((int)type);
            res_str(nm, n);
            break;
        }
        case GLC_UNIFORM_F: {
            int loc = si(&r), n = si(&r);
            float v[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n && i < 4; i++) v[i] = f(&r);
            if (n == 1) glUniform1f(loc, v[0]); else if (n == 2) glUniform2f(loc, v[0], v[1]);
            else if (n == 3) glUniform3f(loc, v[0], v[1], v[2]); else if (n == 4) glUniform4f(loc, v[0], v[1], v[2], v[3]);
            break;
        }
        case GLC_UNIFORM_I: {
            int loc = si(&r), n = si(&r);
            int v[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < n && i < 4; i++) v[i] = si(&r);
            if (n == 1) glUniform1i(loc, v[0]); else if (n == 2) glUniform2i(loc, v[0], v[1]);
            else if (n == 3) glUniform3i(loc, v[0], v[1], v[2]); else if (n == 4) glUniform4i(loc, v[0], v[1], v[2], v[3]);
            break;
        }
        case GLC_UNIFORM_FV: case GLC_UNIFORM_IV: case GLC_UNIFORM_MAT: {
            int loc = si(&r), comp = si(&r), count = si(&r);
            int per = op == GLC_UNIFORM_MAT ? comp * comp : comp;
            if (per < 1 || per > 16 || count < 0 || (unsigned)(per * count) > r.n - r.i) break;
            const void* data = r.w + r.i;
            if (op == GLC_UNIFORM_FV) {
                if (comp == 1) glUniform1fv(loc, count, data); else if (comp == 2) glUniform2fv(loc, count, data);
                else if (comp == 3) glUniform3fv(loc, count, data); else if (comp == 4) glUniform4fv(loc, count, data);
            } else if (op == GLC_UNIFORM_IV) {
                if (comp == 1) glUniform1iv(loc, count, data); else if (comp == 2) glUniform2iv(loc, count, data);
                else if (comp == 3) glUniform3iv(loc, count, data); else if (comp == 4) glUniform4iv(loc, count, data);
            } else {
                if (comp == 2) glUniformMatrix2fv(loc, count, GL_FALSE, data);
                else if (comp == 3) glUniformMatrix3fv(loc, count, GL_FALSE, data);
                else if (comp == 4) glUniformMatrix4fv(loc, count, GL_FALSE, data);
            }
            break;
        }
        case GLC_ATTRIB_PTR: {
            unsigned idx = u(&r);
            int size = si(&r);
            GLenum type = u(&r);
            int norm = si(&r), stride = si(&r);
            uintptr_t off = u(&r);
            glVertexAttribPointer(idx, size, type, norm ? GL_TRUE : GL_FALSE, stride, (const void*)off);
            break;
        }
        case GLC_ENABLE_ATTRIB: glEnableVertexAttribArray(u(&r)); break;
        case GLC_DISABLE_ATTRIB: glDisableVertexAttribArray(u(&r)); break;
        case GLC_ATTRIB_F: {
            unsigned idx = u(&r);
            int n = si(&r);
            float v[4] = { 0, 0, 0, 1 };
            for (int i = 0; i < n && i < 4; i++) v[i] = f(&r);
            glVertexAttrib4f(idx, v[0], v[1], v[2], v[3]);
            break;
        }
        case GLC_BIND_BUFFER: { GLenum t = u(&r); unsigned id = u(&r); glBindBuffer(t, id ? name_of(GLK_BUFFER, id) : 0); break; }
        case GLC_BUFFER_DATA: {
            GLenum t = u(&r), usage = u(&r);
            unsigned int size = u(&r), n;
            const unsigned char* d = bytes(&r, &n);
            if (r.bad || size > (256u << 20)) break;
            if (n) glBufferData(t, n, d, usage);
            else glBufferData(t, size, NULL, usage);
            break;
        }
        case GLC_BUFFER_SUB: {
            GLenum t = u(&r);
            unsigned int off = u(&r), n;
            const unsigned char* d = bytes(&r, &n);
            if (!r.bad && n) glBufferSubData(t, off, n, d);
            break;
        }
        case GLC_DRAW_ARRAYS: { GLenum m = u(&r); int a = si(&r), c = si(&r); glDrawArrays(m, a, c); break; }
        case GLC_DRAW_ELEMENTS: { GLenum m = u(&r); int c = si(&r); GLenum t = u(&r); uintptr_t off = u(&r); glDrawElements(m, c, t, (const void*)off); break; }
        case GLC_VIEWPORT: { int x = si(&r), y = si(&r), w = si(&r), h = si(&r); glViewport(x, y, w, h); break; }
        case GLC_SCISSOR: { int x = si(&r), y = si(&r), w = si(&r), h = si(&r); glScissor(x, y, w, h); break; }
        case GLC_CLEAR_COLOR: { float a = f(&r), b = f(&r), c = f(&r), d = f(&r); glClearColor(a, b, c, d); break; }
        case GLC_CLEAR_DEPTH: glClearDepthf(f(&r)); break;
        case GLC_CLEAR_STENCIL: glClearStencil(si(&r)); break;
        case GLC_CLEAR: glClear(u(&r) & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)); break;
        case GLC_ENABLE: glEnable(u(&r)); break;
        case GLC_DISABLE: glDisable(u(&r)); break;
        case GLC_BLEND_FUNC: { GLenum a = u(&r), b = u(&r), c = u(&r), d = u(&r); glBlendFuncSeparate(a, b, c, d); break; }
        case GLC_BLEND_EQ: { GLenum a = u(&r), b = u(&r); glBlendEquationSeparate(a, b); break; }
        case GLC_BLEND_COLOR: { float a = f(&r), b = f(&r), c = f(&r), d = f(&r); glBlendColor(a, b, c, d); break; }
        case GLC_DEPTH_FUNC: glDepthFunc(u(&r)); break;
        case GLC_DEPTH_MASK: glDepthMask(u(&r) ? GL_TRUE : GL_FALSE); break;
        case GLC_DEPTH_RANGE: { float a = f(&r), b = f(&r); glDepthRangef(a, b); break; }
        case GLC_COLOR_MASK: { int a = si(&r), b = si(&r), c = si(&r), d = si(&r); glColorMask(a != 0, b != 0, c != 0, d != 0); break; }
        case GLC_CULL_FACE: glCullFace(u(&r)); break;
        case GLC_FRONT_FACE: glFrontFace(u(&r)); break;
        case GLC_LINE_WIDTH: glLineWidth(f(&r)); break;
        case GLC_POLYGON_OFFSET: { float a = f(&r), b = f(&r); glPolygonOffset(a, b); break; }
        case GLC_STENCIL_FUNC: { GLenum face = u(&r), fn = u(&r); int ref = si(&r); unsigned m = u(&r); glStencilFuncSeparate(face, fn, ref, m); break; }
        case GLC_STENCIL_OP: { GLenum face = u(&r), a = u(&r), b = u(&r), c = u(&r); glStencilOpSeparate(face, a, b, c); break; }
        case GLC_STENCIL_MASK: { GLenum face = u(&r); unsigned m = u(&r); glStencilMaskSeparate(face, m); break; }
        case GLC_HINT: { GLenum a = u(&r), b = u(&r); glHint(a, b); break; }
        case GLC_PIXEL_STORE: { GLenum a = u(&r); int b = si(&r); if (a == GL_PACK_ALIGNMENT || a == GL_UNPACK_ALIGNMENT) glPixelStorei(a, b); break; }
        case GLC_ACTIVE_TEXTURE: glActiveTexture(u(&r)); break;
        case GLC_BIND_TEXTURE: { GLenum t = u(&r); unsigned id = u(&r); glBindTexture(t, id ? name_of(GLK_TEXTURE, id) : 0); break; }
        case GLC_TEX_PARAM_I: { GLenum t = u(&r), p = u(&r); int v = si(&r); glTexParameteri(t, p, v); break; }
        case GLC_TEX_PARAM_F: { GLenum t = u(&r), p = u(&r); float v = f(&r); glTexParameterf(t, p, v); break; }
        case GLC_TEX_IMAGE: {
            GLenum t = u(&r);
            int level = si(&r), ifmt = si(&r), w = si(&r), h = si(&r);
            GLenum fmt = u(&r), type = u(&r);
            unsigned int n;
            const unsigned char* d = bytes(&r, &n);
            if (r.bad || w < 0 || h < 0 || w > 8192 || h > 8192) break;
            glTexImage2D(t, level, ifmt, w, h, 0, fmt, type, n ? d : NULL);
            break;
        }
        case GLC_TEX_SUB_IMAGE: {
            GLenum t = u(&r);
            int level = si(&r), x = si(&r), y = si(&r), w = si(&r), h = si(&r);
            GLenum fmt = u(&r), type = u(&r);
            unsigned int n;
            const unsigned char* d = bytes(&r, &n);
            if (!r.bad && n && w > 0 && h > 0) glTexSubImage2D(t, level, x, y, w, h, fmt, type, d);
            break;
        }
        case GLC_GEN_MIPMAP: glGenerateMipmap(u(&r)); break;
        case GLC_BIND_FRAMEBUFFER: { GLenum t = u(&r); unsigned id = u(&r); glBindFramebuffer(t, id ? name_of(GLK_FRAMEBUFFER, id) : 0); break; }
        case GLC_FB_TEXTURE: { GLenum t = u(&r), a = u(&r), tt = u(&r); unsigned id = u(&r); int lv = si(&r); glFramebufferTexture2D(t, a, tt, id ? name_of(GLK_TEXTURE, id) : 0, lv); break; }
        case GLC_FB_RENDERBUFFER: { GLenum t = u(&r), a = u(&r), rt = u(&r); unsigned id = u(&r); glFramebufferRenderbuffer(t, a, rt, id ? name_of(GLK_RENDERBUFFER, id) : 0); break; }
        case GLC_CHECK_FB: res_int((int)glCheckFramebufferStatus(u(&r))); break;
        case GLC_BIND_RENDERBUFFER: { GLenum t = u(&r); unsigned id = u(&r); glBindRenderbuffer(t, id ? name_of(GLK_RENDERBUFFER, id) : 0); break; }
        case GLC_RB_STORAGE: { GLenum t = u(&r), fm = u(&r); int w = si(&r), h = si(&r); if (w >= 0 && h >= 0 && w <= 8192 && h <= 8192) glRenderbufferStorage(t, fm, w, h); break; }
        case GLC_READ_PIXELS: {
            int x = si(&r), y = si(&r), w = si(&r), h = si(&r);
            GLenum fmt = u(&r), type = u(&r);
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || fmt != GL_RGBA || type != GL_UNSIGNED_BYTE) { res_int(0); break; }
            unsigned char* px = malloc((size_t)w * h * 4);
            if (!px) { res_int(0); break; }
            glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            res_int(w * h * 4);
            res_put(px, (unsigned)(w * h * 4));
            free(px);
            break;
        }
        case GLC_GET_ERROR: res_int((int)glGetError()); break;
        case GLC_GET_PARAM: {
            GLenum pn = u(&r);
            GLfloat v[16];
            memset(v, 0, sizeof(v));
            glGetFloatv(pn, v);
            int count = 16;
            while (count > 1 && v[count - 1] == 0) count--;
            res_int(count);
            res_put(v, (unsigned)count * 4);
            break;
        }
        case GLC_IS_ENABLED: res_int(glIsEnabled(u(&r)) ? 1 : 0); break;
        case GLC_GET_STRING: {
            const char* s = (const char*)glGetString(u(&r));
            res_str(s ? s : "", s ? (int)strlen(s) : 0);
            break;
        }
        case GLC_FINISH: glFinish(); break;
        case GLC_COPY_TEX_IMAGE: {
            GLenum t = u(&r);
            int lv = si(&r);
            GLenum ifmt = u(&r);
            int x = si(&r), y = si(&r), w = si(&r), h = si(&r);
            glCopyTexImage2D(t, lv, ifmt, x, y, w, h, 0);
            break;
        }
        case GLC_COPY_TEX_SUB_IMAGE: {
            GLenum t = u(&r);
            int lv = si(&r), xo = si(&r), yo = si(&r), x = si(&r), y = si(&r), w = si(&r), h = si(&r);
            glCopyTexSubImage2D(t, lv, xo, yo, x, y, w, h);
            break;
        }
        case GLC_SAMPLE_COVERAGE: { float v = f(&r); int inv = si(&r); glSampleCoverage(v, inv ? GL_TRUE : GL_FALSE); break; }
        default: break;                                  /* (unknown: skipped, its words counted) */
        }
    }
}

int ww_gl_execute(unsigned int op, const int* a, const void* data, unsigned int n,
                  unsigned char** out, unsigned int* out_len) {
    if (op == WW_GL_INIT) return init(a[0], a[1]);
    if (op != WW_GL_EXEC || !g_ctx) return -1;
    g_res_n = 0;
    rd_t r = { data, n / 4, 0, 0 };
    if (data) run(&r);
    int read = a[0] == 1;
    ww_gl_reply_t h = { (int)g_res_n, read ? g_w : 0, read ? g_h : 0, 0 };
    size_t pix = read ? (size_t)g_w * g_h * 4 : 0;
    unsigned char* buf = malloc(sizeof(h) + g_res_n + pix);
    if (!buf) return -1;
    if (read) {
        /* the drawing buffer, top row first (GL's rows go up) */
        GLint fb = 0, pack = 4;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);    /* (the page's binding is kept) */
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        unsigned char* px = buf + sizeof(h) + g_res_n;
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        size_t row = (size_t)g_w * 4;
        unsigned char* tmp = malloc(row);
        if (tmp) {
            for (int y = 0; y < g_h / 2; y++) {
                memcpy(tmp, px + (size_t)y * row, row);
                memcpy(px + (size_t)y * row, px + (size_t)(g_h - 1 - y) * row, row);
                memcpy(px + (size_t)(g_h - 1 - y) * row, tmp, row);
            }
            free(tmp);
        }
    }
    memcpy(buf, &h, sizeof(h));
    if (g_res_n) memcpy(buf + sizeof(h), g_res, g_res_n);
    *out = buf;
    *out_len = (unsigned int)(sizeof(h) + g_res_n + pix);
    return 0;
}
