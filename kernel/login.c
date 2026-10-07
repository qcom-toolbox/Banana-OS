/* The login screen at boot: once a password is set (Settings > Startup or
 * `passwd`), Banana OS asks for it before the console or the desktop come
 * up. Drawn over the wallpaper on a framebuffer, a plain prompt otherwise. */
#include "login.h"
#include "passwd.h"
#include "config.h"
#include "fb.h"
#include "gfx.h"
#include "font.h"
#include "wallpaper.h"
#include "keyboard.h"
#include "terminal.h"
#include "timer.h"
#include "task.h"
#include "rtc.h"
#include "kheap.h"
#include "kstring.h"
#include "utf8.h"
#include "usb.h"

int login_enabled(void) {
    char v[8];
    if (cfg_get(CFG_SERVICES, "login", v, sizeof(v)) && strcmp(v, "no") == 0) return 0;
    return 1;                                    /* on unless switched off */
}

void login_set_enabled(int on) {
    cfg_set(CFG_SERVICES, "login", on ? "yes" : "no", RC_CONF_HEADER);
    cfg_persist();
}

int login_required(void) {
    return login_enabled() && passwd_is_set(PASSWD_USER);
}

/* ── drawing ── */

static int g_locked;           /* the desktop is locked: nothing else reads the keyboard */

int login_is_locked(void) { return g_locked; }

static uint32_t* g_target;
static int g_w, g_h, g_stride;

static uint32_t shade(uint32_t c, int num, int den) {
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0xFF) * (uint32_t)num / (uint32_t)den;
    return r << 16 | g << 8 | b;
}

/* darkens a rectangle (a translucent panel) */
static void dim_rect(int x, int y, int w, int h, int num, int den) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < g_h; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < g_w; xx++) {
            uint32_t* p = &g_target[yy * g_stride + xx];
            *p = shade(*p, num, den);
        }
}

/* the mouse: an arrow, and the button next to the field */
static const char* const ARROW[] = {
    "B...........", "BB..........", "BWB.........", "BWWB........", "BWWWB.......",
    "BWWWWB......", "BWWWWWB.....", "BWWWWWWB....", "BWWWWWWWB...", "BWWWWWWWWB..",
    "BWWWWWWWWWB.", "BWWWWWWBBBBB", "BWWWBWWB....", "BWWBBWWB....", "BWB..BWWB...",
    "BB...BWWB...", "B.....BWWB..", "......BWWB..", ".......BB...",
};

static void draw_arrow(int mx, int my) {
    for (int r = 0; r < (int)(sizeof(ARROW) / sizeof(ARROW[0])); r++)
        for (int c = 0; ARROW[r][c]; c++) {
            int x = mx + c, y = my + r;
            if (x < 0 || y < 0 || x >= g_w || y >= g_h || ARROW[r][c] == '.') continue;
            g_target[y * g_stride + x] = ARROW[r][c] == 'B' ? 0x00000000u : 0x00FFFFFFu;
        }
}

/* where the field and its button are (the panel is centred) */
static void field_rects(int* fx, int* fy, int* fw, int* fh, int* bx) {
    int pw = 340, px = g_w / 2 - pw / 2, py = g_h / 2 - 90;
    *fx = px + 40; *fy = py + 116; *fh = 30;
    *fw = pw - 80 - 36;
    *bx = *fx + *fw + 6;
}

static void text(int face, int px, int x, int y, const char* s, uint32_t color) {
    font_draw(g_target, g_stride, 0, 0, g_w, g_h, face, px, x, y, s, (uint32_t)strlen(s), color, 0);
}
static void text_center(int face, int px, int cx, int y, const char* s, uint32_t color) {
    text(face, px, cx - font_text_width(face, px, s, (uint32_t)strlen(s)) / 2, y, s, color);
}

static void filled_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                int x = cx + dx, y = cy + dy;
                if (x >= 0 && y >= 0 && x < g_w && y < g_h) g_target[y * g_stride + x] = color;
            }
}

typedef struct {
    int  locked;                /* unlocking the desktop (not the boot login) */
    char pw[64];
    const char* message;        /* under the field: an error, "Checking..." */
    uint32_t message_color;
    int caret;
    int mx, my;                 /* the mouse */
    int hover;                  /* over the button */
} login_view_t;

static void draw(const uint32_t* bg, const login_view_t* v) {
    memcpy(g_target, bg, (uint32_t)g_stride * (uint32_t)g_h * 4u);
    int cx = g_w / 2;

    /* the clock */
    rtc_datetime_t t;
    if (rtc_read_datetime(&t) == 0) {
        char clk[16], day[32];
        static const char* const MON[12] = { "January", "February", "March", "April", "May", "June", "July",
                                             "August", "September", "October", "November", "December" };
        ksnprintf(clk, sizeof(clk), "%02u:%02u", t.hour, t.minute);
        ksnprintf(day, sizeof(day), "%u %s %u", t.day, (t.month >= 1 && t.month <= 12) ? MON[t.month - 1] : "", t.year);
        text_center(FONT_SANS, 64, cx, g_h / 2 - 150, clk, 0x00FFFFFFu);
        text_center(FONT_SANS, 18, cx, g_h / 2 - 120, day, 0x00DDE6F2u);
    }

    /* the panel */
    int pw = 340, ph = 200, px = cx - pw / 2, py = g_h / 2 - 90;
    dim_rect(px, py, pw, ph, 2, 5);
    filled_circle(cx, py + 42, 26, 0x003A7BD5u);
    text_center(FONT_SANS_BOLD, 26, cx, py + 52, "B", 0x00FFFFFFu);
    text_center(FONT_SANS_BOLD, 18, cx, py + 98, "banana", 0x00FFFFFFu);

    /* the password field: one dot per character */
    int fx, fy, fw, fh, bx;
    field_rects(&fx, &fy, &fw, &fh, &bx);
    for (int yy = fy; yy < fy + fh; yy++)
        for (int xx = fx; xx < fx + fw; xx++) g_target[yy * g_stride + xx] = 0x00F4F6FAu;
    int n = u8_cols(v->pw, (int)strlen(v->pw));
    if (n == 0) {
        text(FONT_SANS, 14, fx + 10, fy + 20, "Password", 0x009098A8u);
    } else {
        char dots[3 * 32 + 1];
        int k = 0;
        for (int i = 0; i < n && i < 28; i++) k += u8_encode(0x25CF, dots + k);   /* ● */
        dots[k] = 0;
        text(FONT_SANS, 14, fx + 10, fy + 20, dots, 0x00202838u);
    }
    if (v->caret) {
        char dots[3 * 32 + 1];
        int k = 0;
        for (int i = 0; i < n && i < 28; i++) k += u8_encode(0x25CF, dots + k);
        dots[k] = 0;
        int cxp = fx + 10 + (n ? font_text_width(FONT_SANS, 14, dots, (uint32_t)k) : 0);
        for (int yy = fy + 7; yy < fy + fh - 7; yy++) { g_target[yy * g_stride + cxp] = 0x00202838u; g_target[yy * g_stride + cxp + 1] = 0x00202838u; }
    }

    /* the button: unlock / log in with the mouse */
    uint32_t bc = v->hover ? 0x005A9BF0u : 0x003A7BD5u;
    for (int yy = fy; yy < fy + fh; yy++)
        for (int xx = bx; xx < bx + 30; xx++) g_target[yy * g_stride + xx] = bc;
    char arrow[4];
    arrow[u8_encode(0x2192, arrow)] = 0;                 /* → */
    text_center(FONT_SANS_BOLD, 18, bx + 15, fy + 21, arrow, 0x00FFFFFFu);

    if (v->message) text_center(FONT_SANS, 14, cx, fy + fh + 24, v->message, v->message_color);
    else if (keyboard_caps_lock()) text_center(FONT_SANS, 14, cx, fy + fh + 24, "Caps Lock is on", 0x00FFD070u);
    else text_center(FONT_SANS, 13, cx, fy + fh + 24,
                     v->locked ? "Locked - type your password to unlock" : "Type your password and press Enter", 0x00C8D2E0u);

    text_center(FONT_SANS, 13, cx, g_h - 24, "Banana OS", 0x00C8D2E0u);
    draw_arrow(v->mx, v->my);
    fb_present();
}

/* reads keys into v->pw until Enter; 0 then, or keeps going */
static int take_keys(login_view_t* v, int* skip, int* changed) {
    for (;;) {
        char c = keyboard_try_getchar();
        if (!c) return 0;
        *changed = 1;
        if (*skip) { (*skip)--; continue; }
        if (c == 27) { *skip = 2; continue; }        /* arrow keys: ESC [ X */
        int n = (int)strlen(v->pw);
        v->message = 0;
        if (c == '\n') return 1;
        if (c == '\b') { if (n) u8_backspace(v->pw, n); continue; }
        if (c == 21) { v->pw[0] = 0; continue; }     /* Ctrl+U: clear */
        if ((unsigned char)c >= 32 && n < (int)sizeof(v->pw) - 1) { v->pw[n] = c; v->pw[n + 1] = 0; }
    }
}

static void login_graphical(int locked, int* mouse_x, int* mouse_y) {
    const fb_info_t* fi = fb_info();
    int w = (int)fi->width, h = (int)fi->height;
    uint32_t* bg = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    uint32_t* back = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!bg || !back) {
        if (bg) kfree(bg);
        if (back) kfree(back);
        return;
    }
    wallpaper_load_config();
    wallpaper_render(bg, w, h, w);
    for (int i = 0; i < w * h; i++) bg[i] = shade(bg[i], 3, 4);

    fb_set_backbuffer(back, (uint32_t)w, (uint32_t)h);
    g_target = fb_target(&g_stride, &g_w, &g_h);

    login_view_t v;
    memset(&v, 0, sizeof(v));
    v.locked = locked;
    keyboard_set_owner(task_current_pid());  /* no other task takes a key meanwhile */
    v.mx = mouse_x ? *mouse_x : g_w / 2;
    v.my = mouse_y ? *mouse_y : g_h / 2 + 120;
    int prev_left = 1;                       /* (the click that chose "Lock screen") */
    while (keyboard_try_getchar()) {}        /* keys typed before it came up */
    int skip = 0, failures = 0, changed = 1;
    uint32_t last_blink = 0, last_min = 0xFFFFFFFFu;
    for (;;) {
        uint32_t now = timer_ms();
        int caret = (now / 500) % 2 == 0;
        uint32_t min = now / 30000;              /* the clock: twice a minute */
        if (caret != (int)last_blink || min != last_min) { changed = 1; last_blink = (uint32_t)caret; last_min = min; }
        v.caret = caret;
        mouse_state_t ms = mouse_read();
        int clicked = 0;
        if (ms.dx || ms.dy) {
            v.mx += ms.dx;
            v.my -= ms.dy;
            if (v.mx < 0) v.mx = 0;
            if (v.my < 0) v.my = 0;
            if (v.mx > g_w - 1) v.mx = g_w - 1;
            if (v.my > g_h - 1) v.my = g_h - 1;
            changed = 1;
        }
        {
            int fx, fy, fw, fh, bx;
            field_rects(&fx, &fy, &fw, &fh, &bx);
            int hover = v.mx >= bx && v.mx < bx + 30 && v.my >= fy && v.my < fy + fh;
            if (hover != v.hover) { v.hover = hover; changed = 1; }
            if (ms.btn_left && !prev_left && hover) clicked = 1;
            prev_left = ms.btn_left;
        }
        if (take_keys(&v, &skip, &changed) || (clicked && v.pw[0])) {
            v.message = "Checking...";
            v.message_color = 0x00C8D2E0u;
            draw(bg, &v);
            if (passwd_check(PASSWD_USER, v.pw)) break;
            failures++;
            memset(v.pw, 0, sizeof(v.pw));
            v.message = "Wrong password - try again";
            v.message_color = 0x00FF8A80u;
            draw(bg, &v);
            /* a little longer after every miss (at most 5 s) */
            task_sleep_ms((uint32_t)(failures < 5 ? failures : 5) * 1000u);
            while (keyboard_try_getchar()) {}    /* what was typed meanwhile */
            changed = 1;
            continue;
        }
        if (changed) { draw(bg, &v); changed = 0; }
        task_sleep_ms(15);
    }
    if (mouse_x) *mouse_x = v.mx;
    if (mouse_y) *mouse_y = v.my;
    memset(&v, 0, sizeof(v));
    keyboard_set_owner(-1);

    fb_clear_backbuffer();
    kfree(back);
    kfree(bg);
    if (!locked) terminal_screen_changed();      /* the console comes back (the desktop redraws itself) */
}

static void login_text(void) {
    int failures = 0;
    for (;;) {
        terminal_write("\nbanana-os login: banana\nPassword: ");
        terminal_flush();
        char pw[64];
        int n = 0, skip = 0;
        for (;;) {
            char c = keyboard_try_getchar();
            if (!c) { task_sleep_ms(10); continue; }
            if (skip) { skip--; continue; }
            if (c == 27) { skip = 2; continue; }
            if (c == '\n') break;
            if (c == '\b') { if (n) n = u8_prev(pw, n); continue; }
            if ((unsigned char)c >= 32 && n < (int)sizeof(pw) - 1) pw[n++] = c;
        }
        pw[n] = 0;
        terminal_putchar('\n');
        int ok = passwd_check(PASSWD_USER, pw);
        memset(pw, 0, sizeof(pw));
        if (ok) { terminal_writeln(""); return; }
        failures++;
        terminal_writeln("Login incorrect");
        task_sleep_ms((uint32_t)(failures < 5 ? failures : 5) * 1000u);
    }
}

void login_screen(void) {
    if (!login_required()) return;
    const fb_info_t* fi = fb_info();
    if (fb_available() && fi && fi->bpp == 32 && fi->width >= 640 && fi->height >= 480 && font_available(FONT_SANS))
        login_graphical(0, 0, 0);
    else
        login_text();
}

int login_lock(int* mouse_x, int* mouse_y) {
    if (!passwd_is_set(PASSWD_USER) || g_locked) return -1;
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi || fi->bpp != 32 || !font_available(FONT_SANS)) return -1;
    g_locked = 1;
    login_graphical(1, mouse_x, mouse_y);
    g_locked = 0;
    return 0;
}
