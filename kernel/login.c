/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "users.h"

int login_enabled(void) {
    char v[8];
    if (cfg_get(CFG_SERVICES, "login", v, sizeof(v)) && strcmp(v, "no") == 0) return 0;
    return 1;                                    /* on unless switched off */
}

void login_set_enabled(int on) {
    cfg_set(CFG_SERVICES, "login", on ? "yes" : "no", RC_CONF_HEADER);
    cfg_persist();
}

/* the people who can log in; *def: the one picked first (the last to
 * log in - or banana) */
static int people(const user_t** out, int max, int* def) {
    int n = users_people(out, max);
    *def = 0;
    for (int i = 0; i < n; i++) if (out[i]->uid == session_uid()) *def = i;
    return n;
}

/* a login is asked: switched on, and someone to choose or a password to type */
int login_required(void) {
    if (!login_enabled()) return 0;
    const user_t* p[USERS_MAX];
    int def, n = people(p, USERS_MAX, &def);
    if (n > 1) return 1;
    return n == 1 && passwd_is_set(p[0]->name);
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
    const user_t* who[8];       /* the people to pick from (the lock screen: the one logged in) */
    int  nwho, sel;
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
    const user_t* u = v->nwho ? v->who[v->sel] : NULL;
    char initial[2] = { u && u->name[0] ? (char)(u->name[0] >= 'a' && u->name[0] <= 'z' ? u->name[0] - 32 : u->name[0]) : 'B', 0 };
    filled_circle(cx, py + 42, 26, 0x003A7BD5u);
    text_center(FONT_SANS_BOLD, 26, cx, py + 52, initial, 0x00FFFFFFu);
    text_center(FONT_SANS_BOLD, 18, cx, py + 98, u ? u->name : "banana", 0x00FFFFFFu);
    /* the others: under the panel, a click (or Tab, the arrows) picks one */
    if (v->nwho > 1) {
        int tw = 84, total = v->nwho * tw, x0 = cx - total / 2, ty = py + ph + 18;
        for (int i = 0; i < v->nwho; i++) {
            int tx = x0 + i * tw;
            dim_rect(tx + 4, ty, tw - 8, 58, i == v->sel ? 1 : 2, i == v->sel ? 2 : 5);
            char in[2] = { (char)(v->who[i]->name[0] >= 'a' && v->who[i]->name[0] <= 'z' ? v->who[i]->name[0] - 32 : v->who[i]->name[0]), 0 };
            filled_circle(tx + tw / 2, ty + 18, 12, i == v->sel ? 0x003A7BD5u : 0x00505E74u);
            text_center(FONT_SANS_BOLD, 13, tx + tw / 2, ty + 23, in, 0x00FFFFFFu);
            text_center(FONT_SANS, 12, tx + tw / 2, ty + 48, v->who[i]->name, 0x00FFFFFFu);
        }
    }

    /* the password field: one dot per character */
    int fx, fy, fw, fh, bx;
    field_rects(&fx, &fy, &fw, &fh, &bx);
    for (int yy = fy; yy < fy + fh; yy++)
        for (int xx = fx; xx < fx + fw; xx++) g_target[yy * g_stride + xx] = 0x00F4F6FAu;
    int n = u8_cols(v->pw, (int)strlen(v->pw));
    if (n == 0) {
        text(FONT_SANS, 14, fx + 10, fy + 20, u && !passwd_is_set(u->name) ? "No password - press Enter" : "Password", 0x009098A8u);
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
        if (*skip == 2 && c == '[') { (*skip)--; continue; }
        if (*skip == 1) {                            /* an arrow: left / right pick the user */
            *skip = 0;
            if (v->nwho > 1 && (c == 'C' || c == 'D')) {
                v->sel = (v->sel + (c == 'C' ? 1 : v->nwho - 1)) % v->nwho;
                memset(v->pw, 0, sizeof(v->pw));
                v->message = 0;
            }
            continue;
        }
        if (*skip) { (*skip)--; continue; }
        if (c == 27) { *skip = 2; continue; }        /* arrow keys: ESC [ X */
        if (c == '\t' && v->nwho > 1) { v->sel = (v->sel + 1) % v->nwho; memset(v->pw, 0, sizeof(v->pw)); v->message = 0; continue; }
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
    v.locked = locked == 1;                      /* (2: Switch user - the picker, over the desktop) */
    if (locked == 1) {                           /* (the lock screen: the one logged in) */
        v.who[0] = session_user();
        v.nwho = v.who[0] ? 1 : 0;
    } else {
        v.nwho = people(v.who, 8, &v.sel);
    }
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
            if (ms.btn_left && !prev_left && v.nwho > 1) {      /* a user's tile */
                int tw = 84, x0 = g_w / 2 - v.nwho * tw / 2, ty = g_h / 2 - 90 + 200 + 18;
                if (v.my >= ty && v.my < ty + 58 && v.mx >= x0 && v.mx < x0 + v.nwho * tw) {
                    v.sel = (v.mx - x0) / tw;
                    memset(v.pw, 0, sizeof(v.pw));
                    v.message = 0;
                    changed = 1;
                }
            }
            prev_left = ms.btn_left;
        }
        const user_t* who = v.nwho ? v.who[v.sel] : NULL;
        if (take_keys(&v, &skip, &changed) || (clicked && (v.pw[0] || (who && !passwd_is_set(who->name))))) {
            if (!who) break;
            if (!passwd_is_set(who->name)) { session_set(who->uid); break; }   /* (no password: in) */
            v.message = "Checking...";
            v.message_color = 0x00C8D2E0u;
            draw(bg, &v);
            if (passwd_check(who->name, v.pw)) { if (locked != 1) session_set(who->uid); break; }
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

/* a line typed at the console (echo: shown) into buf */
static void read_console(char* buf, int cap, int echo) {
    int n = 0, skip = 0;
    for (;;) {
        char c = keyboard_try_getchar();
        if (!c) { task_sleep_ms(10); continue; }
        if (skip) { skip--; continue; }
        if (c == 27) { skip = 2; continue; }
        if (c == '\n') break;
        if (c == '\b') { if (n) { n = u8_prev(buf, n); if (echo) terminal_write("\b \b"); } continue; }
        if ((unsigned char)c >= 32 && n < cap - 1) { buf[n++] = c; if (echo) terminal_putchar(c); }
    }
    buf[n] = 0;
    terminal_putchar('\n');
}

static void login_text(void) {
    int failures = 0;
    for (;;) {
        /* the user (Enter alone: the one picked first) */
        const user_t* p[USERS_MAX];
        int def, np = people(p, USERS_MAX, &def);
        char name[USER_NAME_MAX];
        terminal_write("\nbanana-os login: ");
        if (np > 1) {
            terminal_flush();
            read_console(name, sizeof(name), 1);
            if (!name[0] && np) kstrlcpy(name, p[def]->name, sizeof(name));
        } else {
            kstrlcpy(name, np ? p[0]->name : PASSWD_USER, sizeof(name));
            terminal_writeln(name);
        }
        const user_t* u = user_by_name(name);
        if (u && user_is_person(u) && !passwd_is_set(u->name)) { session_set(u->uid); terminal_writeln(""); return; }
        terminal_write("Password: ");
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
        int ok = u && user_is_person(u) && passwd_check(u->name, pw);
        memset(pw, 0, sizeof(pw));
        if (ok) { session_set(u->uid); terminal_writeln(""); return; }
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

void login_switch(int* mouse_x, int* mouse_y) {
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi || fi->bpp != 32 || !font_available(FONT_SANS)) return;
    g_locked = 1;
    login_graphical(2, mouse_x, mouse_y);
    g_locked = 0;
}

int login_lock(int* mouse_x, int* mouse_y) {
    const user_t* me = session_user();
    if (!me || !passwd_is_set(me->name) || g_locked) return -1;
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi || fi->bpp != 32 || !font_available(FONT_SANS)) return -1;
    g_locked = 1;
    login_graphical(1, mouse_x, mouse_y);
    g_locked = 0;
    return 0;
}
