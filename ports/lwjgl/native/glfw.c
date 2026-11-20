/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * libglfw.so for Banana OS: the GLFW 3.4 API (what LWJGL's org.lwjgl.glfw
 * binds) over Banana OS windows (bwin) and OpenGL 3.3 (bgl3: Mesa's
 * libOSMesa.so). LWJGL loads it by name (libglfw.so) with the SDK's dlopen;
 * the window functions come from the app's export table (the java app),
 * bgl3 is compiled in (sdk/lib/gl3.c).
 *
 * What there is: windows with a 3.3 core context (asked for: 3.2 or 3.3
 * core) or else a compatibility one (Mesa's highest version, with the
 * fixed pipeline), swap, resize, close, focus,
 * keys (press from the window's events, release when banana_key_down() says
 * the key is up), characters, mouse buttons, motion, wheel, the captured
 * mouse (GLFW_CURSOR_DISABLED), dropped files, the clipboard, time, one
 * monitor. No joysticks, gamma, window position or fullscreen (a monitor
 * passed to glfwCreateWindow gives a window of that size).
 */
#include <GL/gl3.h>
#include <banana.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API __attribute__((visibility("default")))

#define GLFW_TRUE  1
#define GLFW_FALSE 0
#define GLFW_RELEASE 0
#define GLFW_PRESS   1
#define GLFW_REPEAT  2

#define GLFW_KEY_SPACE 32
#define GLFW_KEY_ESCAPE 256
#define GLFW_KEY_ENTER 257
#define GLFW_KEY_TAB 258
#define GLFW_KEY_BACKSPACE 259
#define GLFW_KEY_INSERT 260
#define GLFW_KEY_DELETE 261
#define GLFW_KEY_RIGHT 262
#define GLFW_KEY_LEFT 263
#define GLFW_KEY_DOWN 264
#define GLFW_KEY_UP 265
#define GLFW_KEY_PAGE_UP 266
#define GLFW_KEY_PAGE_DOWN 267
#define GLFW_KEY_HOME 268
#define GLFW_KEY_END 269
#define GLFW_KEY_F1 290
#define GLFW_KEY_LEFT_SHIFT 340
#define GLFW_KEY_LEFT_CONTROL 341
#define GLFW_KEY_LEFT_ALT 342
#define GLFW_KEY_LAST 348
#define GLFW_MOD_SHIFT 1
#define GLFW_MOD_CONTROL 2
#define GLFW_MOD_ALT 4

#define GLFW_NOT_INITIALIZED 0x10001
#define GLFW_NO_CURRENT_CONTEXT 0x10002
#define GLFW_INVALID_ENUM 0x10003
#define GLFW_INVALID_VALUE 0x10004
#define GLFW_OUT_OF_MEMORY 0x10005
#define GLFW_API_UNAVAILABLE 0x10006
#define GLFW_VERSION_UNAVAILABLE 0x10007
#define GLFW_PLATFORM_ERROR 0x10008
#define GLFW_NO_WINDOW_CONTEXT 0x1000A
#define GLFW_FEATURE_UNAVAILABLE 0x1000C

#define GLFW_FOCUSED 0x20001
#define GLFW_ICONIFIED 0x20002
#define GLFW_RESIZABLE 0x20003
#define GLFW_VISIBLE 0x20004
#define GLFW_DECORATED 0x20005
#define GLFW_MAXIMIZED 0x20008
#define GLFW_HOVERED 0x2000B
#define GLFW_RED_BITS 0x21001
#define GLFW_DOUBLEBUFFER 0x21010
#define GLFW_CLIENT_API 0x22001
#define GLFW_CONTEXT_VERSION_MAJOR 0x22002
#define GLFW_CONTEXT_VERSION_MINOR 0x22003
#define GLFW_CONTEXT_REVISION 0x22004
#define GLFW_CONTEXT_ROBUSTNESS 0x22005
#define GLFW_OPENGL_FORWARD_COMPAT 0x22006
#define GLFW_CONTEXT_DEBUG 0x22007
#define GLFW_OPENGL_PROFILE 0x22008
#define GLFW_CONTEXT_CREATION_API 0x2200B
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_CORE_PROFILE 0x32001
#define GLFW_OPENGL_COMPAT_PROFILE 0x32002
#define GLFW_OSMESA_CONTEXT_API 0x36003
#define GLFW_CURSOR 0x33001
#define GLFW_STICKY_KEYS 0x33002
#define GLFW_STICKY_MOUSE_BUTTONS 0x33003
#define GLFW_LOCK_KEY_MODS 0x33004
#define GLFW_RAW_MOUSE_MOTION 0x33005
#define GLFW_CURSOR_NORMAL 0x34001
#define GLFW_CURSOR_HIDDEN 0x34002
#define GLFW_CURSOR_DISABLED 0x34003
#define GLFW_CURSOR_CAPTURED 0x34004
#define GLFW_PLATFORM_NULL 0x60005

typedef struct GLFWwindow GLFWwindow;
typedef struct GLFWmonitor { const char* name; } GLFWmonitor;
typedef struct GLFWcursor { int shape; } GLFWcursor;
typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } GLFWvidmode;
typedef struct { unsigned short *red, *green, *blue; unsigned int size; } GLFWgammaramp;
typedef struct { int width, height; unsigned char* pixels; } GLFWimage;
typedef struct { unsigned char buttons[15]; float axes[6]; } GLFWgamepadstate;
typedef void (*GLFWglproc)(void);

typedef void (*errorfun)(int, const char*);
typedef void (*windowposfun)(GLFWwindow*, int, int);
typedef void (*windowsizefun)(GLFWwindow*, int, int);
typedef void (*windowclosefun)(GLFWwindow*);
typedef void (*windowrefreshfun)(GLFWwindow*);
typedef void (*windowfocusfun)(GLFWwindow*, int);
typedef void (*windowiconifyfun)(GLFWwindow*, int);
typedef void (*windowmaximizefun)(GLFWwindow*, int);
typedef void (*framebuffersizefun)(GLFWwindow*, int, int);
typedef void (*windowcontentscalefun)(GLFWwindow*, float, float);
typedef void (*keyfun)(GLFWwindow*, int, int, int, int);
typedef void (*charfun)(GLFWwindow*, unsigned int);
typedef void (*charmodsfun)(GLFWwindow*, unsigned int, int);
typedef void (*mousebuttonfun)(GLFWwindow*, int, int, int);
typedef void (*cursorposfun)(GLFWwindow*, double, double);
typedef void (*cursorenterfun)(GLFWwindow*, int);
typedef void (*scrollfun)(GLFWwindow*, double, double);
typedef void (*dropfun)(GLFWwindow*, int, const char**);
typedef void (*monitorfun)(GLFWmonitor*, int);
typedef void (*joystickfun)(int, int);

struct GLFWwindow {
    GLFWwindow* next;
    bwin_t win;
    bgl3_ctx_t* gl;
    int should_close, focused, visible, resizable, hovered;
    int fwd_compat, debug;
    int compat, gl_major, gl_minor;      /* the context: a compatibility one? its version */
    void* user;
    char title[256];
    int cursor_mode, sticky_keys, sticky_buttons, raw_motion;
    double cx, cy;                       /* the cursor (virtual when disabled) */
    unsigned char keys[GLFW_KEY_LAST + 1];
    int bkey[GLFW_KEY_LAST + 1];         /* the Banana OS key that pressed it (for banana_key_down) */
    unsigned char buttons[8];
    windowposfun pos_cb;
    windowsizefun size_cb;
    windowclosefun close_cb;
    windowrefreshfun refresh_cb;
    windowfocusfun focus_cb;
    windowiconifyfun iconify_cb;
    windowmaximizefun maximize_cb;
    framebuffersizefun fbsize_cb;
    windowcontentscalefun scale_cb;
    keyfun key_cb;
    charfun char_cb;
    charmodsfun charmods_cb;
    mousebuttonfun button_cb;
    cursorposfun cursor_cb;
    cursorenterfun enter_cb;
    scrollfun scroll_cb;
    dropfun drop_cb;
};

static int g_init;
static GLFWwindow* g_windows;
static __thread GLFWwindow* t_current;
static int g_error;
static char g_error_desc[512];
static errorfun g_error_cb;
static GLFWmonitor g_monitor = { "Banana OS display" };
static GLFWmonitor* g_monitors[1] = { &g_monitor };
static void* g_monitor_user;
static GLFWvidmode g_mode = { 1024, 768, 8, 8, 8, 60 };
static double g_time_base;
static volatile int g_empty_event;

/* the window hints */
static int h_client_api, h_major, h_minor, h_profile, h_fwd, h_debug, h_visible, h_resizable, h_focused;

static void error(int code, const char* fmt, const char* arg) {
    g_error = code;
    snprintf(g_error_desc, sizeof(g_error_desc), fmt, arg ? arg : "");
    if (g_error_cb) g_error_cb(code, g_error_desc);
}
#define REQUIRE_INIT(ret) do { if (!g_init) { error(GLFW_NOT_INITIALIZED, "The GLFW library is not initialized%s", NULL); return ret; } } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── init, errors, version ─────────────────────────────────────────── */

API void glfwDefaultWindowHints(void) {
    h_client_api = GLFW_OPENGL_API;
    h_major = 1;
    h_minor = 0;
    h_profile = 0;
    h_fwd = h_debug = 0;
    h_visible = h_resizable = h_focused = 1;
}

API int glfwInit(void) {
    if (g_init) return GLFW_TRUE;
    g_init = 1;
    g_time_base = now();
    glfwDefaultWindowHints();
    return GLFW_TRUE;
}

API void glfwDestroyWindow(GLFWwindow* w);

API void glfwTerminate(void) {
    if (!g_init) return;
    while (g_windows) glfwDestroyWindow(g_windows);
    g_init = 0;
}

API void glfwInitHint(int hint, int value) { (void)hint; (void)value; }
API void glfwInitAllocator(const void* allocator) { (void)allocator; }

API void glfwGetVersion(int* major, int* minor, int* rev) {
    if (major) *major = 3;
    if (minor) *minor = 4;
    if (rev) *rev = 0;
}

API const char* glfwGetVersionString(void) { return "3.4.0 Banana OS bwin bgl3 (OSMesa)"; }

API int glfwGetError(const char** desc) {
    int e = g_error;
    if (desc) *desc = e ? g_error_desc : NULL;
    g_error = 0;
    return e;
}

API errorfun glfwSetErrorCallback(errorfun cb) {
    errorfun old = g_error_cb;
    g_error_cb = cb;
    return old;
}

API int glfwGetPlatform(void) { REQUIRE_INIT(0); return GLFW_PLATFORM_NULL; }
API int glfwPlatformSupported(int p) { return p == GLFW_PLATFORM_NULL; }

/* ── monitors ──────────────────────────────────────────────────────── */

API GLFWmonitor** glfwGetMonitors(int* count) {
    if (count) *count = g_init ? 1 : 0;
    REQUIRE_INIT(NULL);
    return g_monitors;
}
API GLFWmonitor* glfwGetPrimaryMonitor(void) { REQUIRE_INIT(NULL); return &g_monitor; }
API void glfwGetMonitorPos(GLFWmonitor* m, int* x, int* y) { (void)m; if (x) *x = 0; if (y) *y = 0; }
API void glfwGetMonitorWorkarea(GLFWmonitor* m, int* x, int* y, int* w, int* h) {
    (void)m;
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = g_mode.width;
    if (h) *h = g_mode.height;
}
API void glfwGetMonitorPhysicalSize(GLFWmonitor* m, int* w, int* h) {
    (void)m;                                     /* (96 dpi) */
    if (w) *w = g_mode.width * 254 / 960;
    if (h) *h = g_mode.height * 254 / 960;
}
API void glfwGetMonitorContentScale(GLFWmonitor* m, float* x, float* y) { (void)m; if (x) *x = 1; if (y) *y = 1; }
API const char* glfwGetMonitorName(GLFWmonitor* m) { return m ? m->name : NULL; }
API void glfwSetMonitorUserPointer(GLFWmonitor* m, void* p) { (void)m; g_monitor_user = p; }
API void* glfwGetMonitorUserPointer(GLFWmonitor* m) { (void)m; return g_monitor_user; }
API monitorfun glfwSetMonitorCallback(monitorfun cb) { (void)cb; return NULL; }
API const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m, int* count) { (void)m; if (count) *count = 1; return &g_mode; }
API const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* m) { (void)m; return &g_mode; }
API void glfwSetGamma(GLFWmonitor* m, float g) { (void)m; (void)g; error(GLFW_FEATURE_UNAVAILABLE, "Banana OS: no gamma ramps%s", NULL); }
API const GLFWgammaramp* glfwGetGammaRamp(GLFWmonitor* m) { (void)m; error(GLFW_FEATURE_UNAVAILABLE, "Banana OS: no gamma ramps%s", NULL); return NULL; }
API void glfwSetGammaRamp(GLFWmonitor* m, const GLFWgammaramp* r) { (void)m; (void)r; error(GLFW_FEATURE_UNAVAILABLE, "Banana OS: no gamma ramps%s", NULL); }

/* ── windows ───────────────────────────────────────────────────────── */

API void glfwWindowHint(int hint, int value) {
    switch (hint) {
    case GLFW_CLIENT_API: h_client_api = value; break;
    case GLFW_CONTEXT_VERSION_MAJOR: h_major = value; break;
    case GLFW_CONTEXT_VERSION_MINOR: h_minor = value; break;
    case GLFW_OPENGL_PROFILE: h_profile = value; break;
    case GLFW_OPENGL_FORWARD_COMPAT: h_fwd = value; break;
    case GLFW_CONTEXT_DEBUG: h_debug = value; break;
    case GLFW_VISIBLE: h_visible = value; break;
    case GLFW_RESIZABLE: h_resizable = value; break;
    case GLFW_FOCUSED: h_focused = value; break;
    default: break;                               /* (framebuffer bits, samples...: what Mesa gives) */
    }
}

API void glfwWindowHintString(int hint, const char* value) { (void)hint; (void)value; }

API GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share) {
    (void)share;
    REQUIRE_INIT(NULL);
    if (width <= 0 || height <= 0) { error(GLFW_INVALID_VALUE, "Invalid window size%s", NULL); return NULL; }
    if (monitor) { width = g_mode.width; height = g_mode.height; }
    if (h_client_api != GLFW_NO_API && h_client_api != GLFW_OPENGL_API) {
        error(GLFW_API_UNAVAILABLE, "Banana OS: OpenGL only (no OpenGL ES)%s", NULL);
        return NULL;
    }
    if (h_client_api == GLFW_OPENGL_API && (h_major > 3 || (h_major == 3 && h_minor > 3))) {
        error(GLFW_VERSION_UNAVAILABLE, "Banana OS: OpenGL 3.3 at most (Mesa softpipe)%s", NULL);
        return NULL;
    }
    GLFWwindow* w = calloc(1, sizeof(*w));
    if (!w) { error(GLFW_OUT_OF_MEMORY, "Out of memory%s", NULL); return NULL; }
    snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
    if (bwin_open(&w->win, w->title, width, height) < 0) {
        error(GLFW_PLATFORM_ERROR, "Banana OS: cannot open a window (is the desktop running? startx)%s", NULL);
        free(w);
        return NULL;
    }
    w->resizable = h_resizable;
    if (h_resizable) bwin_resizable(&w->win, 64, 48);
    bwin_wheel(&w->win);
    if (h_client_api == GLFW_OPENGL_API) {
        bgl3_ctx_t* prev = bgl3_current();
        /* a core context when one is asked for (3.2 and up: Minecraft 1.17+);
         * else a compatibility one, as GLFW gives elsewhere - the fixed
         * pipeline older programs draw with (Minecraft 1.7.10 to 1.16) */
        w->compat = h_profile != GLFW_OPENGL_CORE_PROFILE && !h_fwd &&
                    (h_profile == GLFW_OPENGL_COMPAT_PROFILE || h_major < 3 || (h_major == 3 && h_minor < 2));
        w->gl = w->compat ? bgl3_create_compat(&w->win) : bgl3_create(&w->win);   /* (made current: put the previous one back) */
        if (!w->gl) {
            error(GLFW_API_UNAVAILABLE, "%s", bgl3_error());
            bwin_close(&w->win);
            free(w);
            bgl3_make_current(prev);
            return NULL;
        }
        w->gl_major = 3;
        w->gl_minor = 3;
        bgl3_glGetIntegerv(0x821B, &w->gl_major);                 /* GL_MAJOR_VERSION */
        bgl3_glGetIntegerv(0x821C, &w->gl_minor);
        bgl3_make_current(prev);
    }
    w->fwd_compat = h_fwd;
    w->debug = h_debug;
    w->visible = 1;
    w->focused = h_focused;
    w->cursor_mode = GLFW_CURSOR_NORMAL;
    w->next = g_windows;
    g_windows = w;
    return w;
}

API void glfwDestroyWindow(GLFWwindow* w) {
    if (!w) return;
    if (t_current == w) { bgl3_make_current(NULL); t_current = NULL; }
    for (GLFWwindow** p = &g_windows; *p; p = &(*p)->next)
        if (*p == w) { *p = w->next; break; }
    if (w->cursor_mode == GLFW_CURSOR_DISABLED) bwin_capture_mouse(&w->win, 0);
    if (w->gl) bgl3_destroy(w->gl);
    bwin_close(&w->win);
    free(w);
}

API int glfwWindowShouldClose(GLFWwindow* w) { return w ? w->should_close : 0; }
API void glfwSetWindowShouldClose(GLFWwindow* w, int v) { if (w) w->should_close = v; }
API void glfwSetWindowTitle(GLFWwindow* w, const char* t) {
    if (!w) return;
    snprintf(w->title, sizeof(w->title), "%s", t ? t : "");
    bwin_title(&w->win, w->title);
}
API void glfwSetWindowIcon(GLFWwindow* w, int n, const GLFWimage* images) { (void)w; (void)n; (void)images; }
API void glfwGetWindowPos(GLFWwindow* w, int* x, int* y) { (void)w; if (x) *x = 0; if (y) *y = 0; }
API void glfwSetWindowPos(GLFWwindow* w, int x, int y) { (void)w; (void)x; (void)y; }
API void glfwGetWindowSize(GLFWwindow* w, int* width, int* height) {
    if (width) *width = w ? w->win.w : 0;
    if (height) *height = w ? w->win.h : 0;
}
API void glfwSetWindowSizeLimits(GLFWwindow* w, int a, int b, int c, int d) {
    (void)c; (void)d;
    if (w && w->resizable) bwin_resizable(&w->win, a > 0 ? a : 64, b > 0 ? b : 48);
}
API void glfwSetWindowAspectRatio(GLFWwindow* w, int n, int d) { (void)w; (void)n; (void)d; }
API void glfwSetWindowSize(GLFWwindow* w, int width, int height) { (void)w; (void)width; (void)height; }
API void glfwGetFramebufferSize(GLFWwindow* w, int* width, int* height) { glfwGetWindowSize(w, width, height); }
API void glfwGetWindowFrameSize(GLFWwindow* w, int* l, int* t, int* r, int* b) {
    (void)w;
    if (l) *l = 0;
    if (t) *t = 0;
    if (r) *r = 0;
    if (b) *b = 0;
}
API void glfwGetWindowContentScale(GLFWwindow* w, float* x, float* y) { (void)w; if (x) *x = 1; if (y) *y = 1; }
API float glfwGetWindowOpacity(GLFWwindow* w) { (void)w; return 1; }
API void glfwSetWindowOpacity(GLFWwindow* w, float o) { (void)w; (void)o; }
API void glfwIconifyWindow(GLFWwindow* w) { (void)w; }
API void glfwRestoreWindow(GLFWwindow* w) { (void)w; }
API void glfwMaximizeWindow(GLFWwindow* w) { (void)w; }
API void glfwShowWindow(GLFWwindow* w) { if (w) w->visible = 1; }
API void glfwHideWindow(GLFWwindow* w) { (void)w; }
API void glfwFocusWindow(GLFWwindow* w) { (void)w; }
API void glfwRequestWindowAttention(GLFWwindow* w) { (void)w; }
API GLFWmonitor* glfwGetWindowMonitor(GLFWwindow* w) { (void)w; return NULL; }
API void glfwSetWindowMonitor(GLFWwindow* w, GLFWmonitor* m, int x, int y, int width, int height, int rate) {
    (void)w; (void)m; (void)x; (void)y; (void)width; (void)height; (void)rate;
}

API int glfwGetWindowAttrib(GLFWwindow* w, int a) {
    if (!w) return 0;
    switch (a) {
    case GLFW_FOCUSED: return w->focused;
    case GLFW_ICONIFIED: case GLFW_MAXIMIZED: return 0;
    case GLFW_RESIZABLE: return w->resizable;
    case GLFW_VISIBLE: return w->visible;
    case GLFW_DECORATED: return 1;
    case GLFW_HOVERED: return w->hovered;
    case GLFW_DOUBLEBUFFER: return 1;
    case GLFW_CLIENT_API: return w->gl ? GLFW_OPENGL_API : GLFW_NO_API;
    case GLFW_CONTEXT_CREATION_API: return GLFW_OSMESA_CONTEXT_API;
    case GLFW_CONTEXT_VERSION_MAJOR: return w->gl ? w->gl_major : 0;
    case GLFW_CONTEXT_VERSION_MINOR: return w->gl ? w->gl_minor : 0;
    case GLFW_CONTEXT_REVISION: return 0;
    case GLFW_OPENGL_PROFILE: return !w->gl ? 0 : w->compat ? GLFW_OPENGL_COMPAT_PROFILE : GLFW_OPENGL_CORE_PROFILE;
    case GLFW_OPENGL_FORWARD_COMPAT: return w->fwd_compat;
    case GLFW_CONTEXT_DEBUG: return w->debug;
    case GLFW_CONTEXT_ROBUSTNESS: return 0;
    default: return 0;
    }
}
API void glfwSetWindowAttrib(GLFWwindow* w, int a, int v) { if (w && a == GLFW_RESIZABLE) w->resizable = v; }
API void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { if (w) w->user = p; }
API void* glfwGetWindowUserPointer(GLFWwindow* w) { return w ? w->user : NULL; }

#define SETCB(fn, type, field) \
    API type fn(GLFWwindow* w, type cb) { if (!w) return NULL; type old = w->field; w->field = cb; return old; }
SETCB(glfwSetWindowPosCallback, windowposfun, pos_cb)
SETCB(glfwSetWindowSizeCallback, windowsizefun, size_cb)
SETCB(glfwSetWindowCloseCallback, windowclosefun, close_cb)
SETCB(glfwSetWindowRefreshCallback, windowrefreshfun, refresh_cb)
SETCB(glfwSetWindowFocusCallback, windowfocusfun, focus_cb)
SETCB(glfwSetWindowIconifyCallback, windowiconifyfun, iconify_cb)
SETCB(glfwSetWindowMaximizeCallback, windowmaximizefun, maximize_cb)
SETCB(glfwSetFramebufferSizeCallback, framebuffersizefun, fbsize_cb)
SETCB(glfwSetWindowContentScaleCallback, windowcontentscalefun, scale_cb)
SETCB(glfwSetKeyCallback, keyfun, key_cb)
SETCB(glfwSetCharCallback, charfun, char_cb)
SETCB(glfwSetCharModsCallback, charmodsfun, charmods_cb)
SETCB(glfwSetMouseButtonCallback, mousebuttonfun, button_cb)
SETCB(glfwSetCursorPosCallback, cursorposfun, cursor_cb)
SETCB(glfwSetCursorEnterCallback, cursorenterfun, enter_cb)
SETCB(glfwSetScrollCallback, scrollfun, scroll_cb)
SETCB(glfwSetDropCallback, dropfun, drop_cb)

/* ── input ─────────────────────────────────────────────────────────── */

/* a Banana OS key (ASCII or BANANA_KEY_*) as a GLFW key (the US layout) */
static int glfw_key(int k) {
    if (k >= 'a' && k <= 'z') return k - 32;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return k;
    switch (k) {
    case ' ': return GLFW_KEY_SPACE;
    case 27: return GLFW_KEY_ESCAPE;
    case '\r': case '\n': return GLFW_KEY_ENTER;
    case '\t': return GLFW_KEY_TAB;
    case 8: case 127: return GLFW_KEY_BACKSPACE;
    case '\'': case '"': return 39;
    case ',': case '<': return 44;
    case '-': case '_': return 45;
    case '.': case '>': return 46;
    case '/': case '?': return 47;
    case ';': case ':': return 59;
    case '=': case '+': return 61;
    case '[': case '{': return 91;
    case '\\': case '|': return 92;
    case ']': case '}': return 93;
    case '`': case '~': return 96;
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    case BANANA_KEY_UP: return GLFW_KEY_UP;
    case BANANA_KEY_DOWN: return GLFW_KEY_DOWN;
    case BANANA_KEY_LEFT: return GLFW_KEY_LEFT;
    case BANANA_KEY_RIGHT: return GLFW_KEY_RIGHT;
    case BANANA_KEY_HOME: return GLFW_KEY_HOME;
    case BANANA_KEY_END: return GLFW_KEY_END;
    case BANANA_KEY_PGUP: return GLFW_KEY_PAGE_UP;
    case BANANA_KEY_PGDN: return GLFW_KEY_PAGE_DOWN;
    case BANANA_KEY_DELETE: return GLFW_KEY_DELETE;
    default:
        if (k >= BANANA_KEY_F1 && k <= BANANA_KEY_F12) return GLFW_KEY_F1 + (k - BANANA_KEY_F1);
        return -1;
    }
}

/* the key banana_key_down() knows it by: lower-case letters, the unshifted symbol */
static int down_key(int gk, int k) {
    if (gk >= 'A' && gk <= 'Z') return gk + 32;
    if (gk >= '0' && gk <= '9') return gk;
    if (k >= 0x100) return k;
    switch (gk) {
    case GLFW_KEY_SPACE: return ' ';
    case GLFW_KEY_ESCAPE: return 27;
    case GLFW_KEY_ENTER: return '\n';
    case GLFW_KEY_TAB: return '\t';
    case GLFW_KEY_BACKSPACE: return 8;
    default: return gk < 128 ? gk : k;
    }
}

static int mods(void) {
    int m = banana_key_mods(), r = 0;
    if (m & BANANA_MOD_SHIFT) r |= GLFW_MOD_SHIFT;
    if (m & BANANA_MOD_CTRL) r |= GLFW_MOD_CONTROL;
    if (m & BANANA_MOD_ALT) r |= GLFW_MOD_ALT;
    return r;
}

static void key_event(GLFWwindow* w, int key, int action) {
    if (key < 0 || key > GLFW_KEY_LAST) return;
    int was = w->keys[key];
    if (action == GLFW_RELEASE && !was) return;
    if (action == GLFW_PRESS && was) action = GLFW_REPEAT;
    w->keys[key] = action != GLFW_RELEASE;
    if (w->key_cb) w->key_cb(w, key, key, action, mods());
}

/* releases: a key we saw pressed that is no longer down; the modifiers */
static void poll_keys(GLFWwindow* w) {
    static const int modkey[3] = { GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_LEFT_ALT };
    static const int modban[3] = { BANANA_KEY_SHIFT, BANANA_KEY_CTRL, BANANA_KEY_ALT };
    for (int i = 0; i < 3; i++) {
        int d = w->focused && banana_key_down(modban[i]);
        if (d != w->keys[modkey[i]]) key_event(w, modkey[i], d ? GLFW_PRESS : GLFW_RELEASE);
    }
    for (int k = 0; k < GLFW_KEY_LEFT_SHIFT; k++)
        if (w->keys[k] && !banana_key_down(w->bkey[k])) key_event(w, k, GLFW_RELEASE);
}

static void set_cursor(GLFWwindow* w, double x, double y) {
    if (x == w->cx && y == w->cy) return;
    w->cx = x;
    w->cy = y;
    if (w->cursor_cb) w->cursor_cb(w, x, y);
}

static void handle(GLFWwindow* w, const banana_event_t* ev) {
    switch (ev->type) {
    case BANANA_EV_KEY: {
        int gk = glfw_key(ev->key);
        if (gk >= 0) {
            w->bkey[gk] = down_key(gk, ev->key);
            key_event(w, gk, GLFW_PRESS);
        }
        if (ev->key >= 32 && ev->key < 127) {
            if (w->char_cb) w->char_cb(w, (unsigned)ev->key);
            if (w->charmods_cb) w->charmods_cb(w, (unsigned)ev->key, mods());
        }
        break;
    }
    case BANANA_EV_MOUSE_DOWN:
    case BANANA_EV_MOUSE_UP: {
        int b = ev->button == 1 ? 0 : ev->button == 2 ? 1 : ev->button == 3 ? 2 : -1;
        if (b < 0) break;
        if (w->cursor_mode != GLFW_CURSOR_DISABLED) set_cursor(w, ev->x, ev->y);
        else if (ev->type == BANANA_EV_MOUSE_DOWN && !bwin_mouse_captured(&w->win)) bwin_capture_mouse(&w->win, 1);
        int down = ev->type == BANANA_EV_MOUSE_DOWN;
        w->buttons[b] = down;
        if (w->button_cb) w->button_cb(w, b, down ? GLFW_PRESS : GLFW_RELEASE, mods());
        break;
    }
    case BANANA_EV_MOUSE_MOVE:
        if (!w->hovered) {
            w->hovered = 1;
            if (w->enter_cb) w->enter_cb(w, 1);
        }
        if (w->cursor_mode != GLFW_CURSOR_DISABLED) set_cursor(w, ev->x, ev->y);
        break;
    case BANANA_EV_MOUSE_REL:
        if (w->cursor_mode == GLFW_CURSOR_DISABLED) set_cursor(w, w->cx + ev->x, w->cy + ev->y);
        break;
    case BANANA_EV_WHEEL:
        if (w->scroll_cb) w->scroll_cb(w, 0, -(double)ev->y);
        break;
    case BANANA_EV_CLOSE:
        w->should_close = 1;
        if (w->close_cb) w->close_cb(w);
        break;
    case BANANA_EV_FOCUS:
        w->focused = ev->x != 0;
        if (w->focus_cb) w->focus_cb(w, w->focused);
        if (!w->focused)
            for (int k = 0; k <= GLFW_KEY_LAST; k++)
                if (w->keys[k]) key_event(w, k, GLFW_RELEASE);
        break;
    case BANANA_EV_RESIZE:
        if (w->gl) bgl3_resize(w->gl);
        if (w->size_cb) w->size_cb(w, w->win.w, w->win.h);
        if (w->fbsize_cb) w->fbsize_cb(w, w->win.w, w->win.h);
        if (w->refresh_cb) w->refresh_cb(w);
        break;
    case BANANA_EV_DROP: {
        int n = ev->key;
        if (n <= 0 || !w->drop_cb) break;
        char** paths = calloc((size_t)n, sizeof(char*));
        int got = 0;
        for (int i = 0; paths && i < n; i++) {
            char p[512];
            if (bwin_dropped(&w->win, i, p, sizeof(p)) == 0 && (paths[got] = strdup(p))) got++;
        }
        if (got) w->drop_cb(w, got, (const char**)paths);
        for (int i = 0; i < got; i++) free(paths[i]);
        free(paths);
        break;
    }
    default:
        break;
    }
}

/* every waiting event of every window; how many */
static int pump(void) {
    int n = 0;
    for (GLFWwindow* w = g_windows; w;) {
        GLFWwindow* next = w->next;           /* (a callback may destroy w) */
        banana_event_t ev;
        while (bwin_event(&w->win, &ev)) {
            handle(w, &ev);
            n++;
        }
        poll_keys(w);
        w = next;
    }
    return n;
}

API void glfwPollEvents(void) { REQUIRE_INIT(); pump(); }

API void glfwWaitEventsTimeout(double timeout) {
    REQUIRE_INIT();
    double end = now() + timeout;
    for (;;) {
        if (pump() || g_empty_event) break;
        double left = end - now();
        if (left <= 0) break;
        int ms = left > 0.05 ? 50 : (int)(left * 1000) + 1;
        if (g_windows && !g_windows->next) {
            banana_event_t ev;
            if (bwin_wait_event(&g_windows->win, &ev, ms)) {
                handle(g_windows, &ev);
                pump();
                break;
            }
        } else {
            banana_sleep((unsigned)ms);
        }
    }
    g_empty_event = 0;
}

API void glfwWaitEvents(void) { glfwWaitEventsTimeout(1e9); }
API void glfwPostEmptyEvent(void) { g_empty_event = 1; }

API int glfwGetInputMode(GLFWwindow* w, int mode) {
    if (!w) return 0;
    switch (mode) {
    case GLFW_CURSOR: return w->cursor_mode;
    case GLFW_STICKY_KEYS: return w->sticky_keys;
    case GLFW_STICKY_MOUSE_BUTTONS: return w->sticky_buttons;
    case GLFW_RAW_MOUSE_MOTION: return w->raw_motion;
    case GLFW_LOCK_KEY_MODS: return 0;
    default: error(GLFW_INVALID_ENUM, "Invalid input mode%s", NULL); return 0;
    }
}

API void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
    if (!w) return;
    switch (mode) {
    case GLFW_CURSOR:
        if (value == w->cursor_mode) break;
        if (value == GLFW_CURSOR_DISABLED) bwin_capture_mouse(&w->win, 1);
        else if (w->cursor_mode == GLFW_CURSOR_DISABLED) bwin_capture_mouse(&w->win, 0);
        w->cursor_mode = value;
        break;
    case GLFW_STICKY_KEYS: w->sticky_keys = value; break;
    case GLFW_STICKY_MOUSE_BUTTONS: w->sticky_buttons = value; break;
    case GLFW_RAW_MOUSE_MOTION: w->raw_motion = value; break;
    case GLFW_LOCK_KEY_MODS: break;
    default: error(GLFW_INVALID_ENUM, "Invalid input mode%s", NULL); break;
    }
}

API int glfwRawMouseMotionSupported(void) { return GLFW_TRUE; }

API const char* glfwGetKeyName(int key, int scancode) {
    static char names[128][2];
    if (key < 0) key = scancode;
    if (key < 33 || key > 96 || key == 64) return NULL;
    int c = key >= 'A' && key <= 'Z' ? key + 32 : key;
    names[c][0] = (char)c;
    names[c][1] = 0;
    return names[c];
}
API int glfwGetKeyScancode(int key) { return key; }
API int glfwGetKey(GLFWwindow* w, int key) { return w && key >= 0 && key <= GLFW_KEY_LAST ? w->keys[key] : 0; }
API int glfwGetMouseButton(GLFWwindow* w, int b) { return w && b >= 0 && b < 8 ? w->buttons[b] : 0; }
API void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (x) *x = w ? w->cx : 0;
    if (y) *y = w ? w->cy : 0;
}
API void glfwSetCursorPos(GLFWwindow* w, double x, double y) { if (w) { w->cx = x; w->cy = y; } }
API GLFWcursor* glfwCreateCursor(const GLFWimage* image, int xhot, int yhot) {
    (void)image; (void)xhot; (void)yhot;
    return calloc(1, sizeof(GLFWcursor));
}
API GLFWcursor* glfwCreateStandardCursor(int shape) {
    GLFWcursor* c = calloc(1, sizeof(GLFWcursor));
    if (c) c->shape = shape;
    return c;
}
API void glfwDestroyCursor(GLFWcursor* c) { free(c); }
API void glfwSetCursor(GLFWwindow* w, GLFWcursor* c) { (void)w; (void)c; }

/* joysticks: none */
API int glfwJoystickPresent(int jid) { (void)jid; return 0; }
API const float* glfwGetJoystickAxes(int jid, int* count) { (void)jid; if (count) *count = 0; return NULL; }
API const unsigned char* glfwGetJoystickButtons(int jid, int* count) { (void)jid; if (count) *count = 0; return NULL; }
API const unsigned char* glfwGetJoystickHats(int jid, int* count) { (void)jid; if (count) *count = 0; return NULL; }
API const char* glfwGetJoystickName(int jid) { (void)jid; return NULL; }
API const char* glfwGetJoystickGUID(int jid) { (void)jid; return NULL; }
API void glfwSetJoystickUserPointer(int jid, void* p) { (void)jid; (void)p; }
API void* glfwGetJoystickUserPointer(int jid) { (void)jid; return NULL; }
API int glfwJoystickIsGamepad(int jid) { (void)jid; return 0; }
API joystickfun glfwSetJoystickCallback(joystickfun cb) { (void)cb; return NULL; }
API int glfwUpdateGamepadMappings(const char* s) { (void)s; return GLFW_TRUE; }
API const char* glfwGetGamepadName(int jid) { (void)jid; return NULL; }
API int glfwGetGamepadState(int jid, GLFWgamepadstate* st) { (void)jid; (void)st; return GLFW_FALSE; }

API void glfwSetClipboardString(GLFWwindow* w, const char* s) { (void)w; banana_copy(s ? s : ""); }
API const char* glfwGetClipboardString(GLFWwindow* w) { (void)w; return banana_paste(); }

/* ── time ──────────────────────────────────────────────────────────── */

API double glfwGetTime(void) { return g_init ? now() - g_time_base : 0; }
API void glfwSetTime(double t) { g_time_base = now() - t; }
API uint64_t glfwGetTimerValue(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
API uint64_t glfwGetTimerFrequency(void) { return 1000000000u; }

/* ── contexts ──────────────────────────────────────────────────────── */

API void glfwMakeContextCurrent(GLFWwindow* w) {
    if (w && !w->gl) { error(GLFW_NO_WINDOW_CONTEXT, "The window has no OpenGL context%s", NULL); return; }
    if (bgl3_make_current(w ? w->gl : NULL) < 0) { error(GLFW_PLATFORM_ERROR, "Mesa could not make the context current%s", NULL); return; }
    t_current = w;
}
API GLFWwindow* glfwGetCurrentContext(void) { return t_current; }

/* glfwSwapInterval(n): a frame shown every n refreshes of the screen
 * (60 Hz). Banana OS gives apps no vertical blank to wait on, so the swap
 * keeps its own clock: after each frame it sleeps until the next one is due
 * (a deadline that advances by the period, so the rate stays even; a frame
 * late by more than a period starts the clock again). Without it a game
 * with VSync on - Minecraft's default - drew frames as fast as the CPU could
 * make them, in its menus too: every core busy for frames never shown. */
static int    g_swap_interval;
static double g_next_frame;

static void pace_frame(void) {
    if (g_swap_interval <= 0) return;
    double period = (double)g_swap_interval / 60.0, t = now();
    if (g_next_frame == 0 || t - g_next_frame > period) g_next_frame = t;
    g_next_frame += period;
    double left = g_next_frame - now();
    if (left >= 0.001) banana_sleep((unsigned)(left * 1000.0));
}

API void glfwSwapBuffers(GLFWwindow* w) {
    if (!w) return;
    if (!w->gl) { error(GLFW_NO_WINDOW_CONTEXT, "The window has no OpenGL context%s", NULL); return; }
    bgl3_ctx_t* cur = bgl3_current();
    if (cur != w->gl) bgl3_make_current(w->gl);
    bgl3_swap(w->gl);
    if (cur != w->gl) bgl3_make_current(cur);
    pace_frame();
}

/* (negative: adaptive sync - here the same as 1) */
API void glfwSwapInterval(int n) { g_swap_interval = n < 0 ? 1 : n; g_next_frame = 0; }

API int glfwExtensionSupported(const char* name) {
    if (!t_current) { error(GLFW_NO_CURRENT_CONTEXT, "No current context%s", NULL); return 0; }
    if (!name || !*name) return 0;
    GLint n = 0;
    bgl3_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char* e = (const char*)bgl3_glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return 1;
    }
    return 0;
}

API GLFWglproc glfwGetProcAddress(const char* name) {
    if (!t_current) { error(GLFW_NO_CURRENT_CONTEXT, "No current context%s", NULL); return NULL; }
    return (GLFWglproc)bgl3_proc(name);
}

/* LWJGL's GL.create() on Linux loads libGL.so.1 and asks it for glXGetProcAddress
 * (libGL.so.1 here: libgl.c, which needs this library) */
API void* glfwBananaGetProcAddress(const char* name) { return bgl3_proc(name); }
