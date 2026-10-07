#include "settings.h"
#include "gfx.h"
#include "display.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "fs.h"
#include "winframe.h"
#include "config.h"
#include "audio.h"
#include "keyboard.h"
#include "wallpaper.h"
#include "wallpaper_data.h"
#include "sysinfo.h"
#include "rtc.h"
#include "../net/net.h"
#include "../net/netconf.h"
#include "../net/httpd.h"
#include "../net/sshd.h"
#include "passwd.h"
#include "fsdisk.h"
#include "login.h"
#include "utf8.h"

#define TITLE_H  20
#define SIDE_W   132
#define BODY_Y   (TITLE_H + 8)
#define ITEM_H   26

#define C_PANEL  0x001D232Cu
#define C_TITLE  0x00384562u
#define C_SIDE   0x00161B22u
#define C_TEXT   0x00E8EEF6u
#define C_DIM    0x00AAB6C6u
#define C_HEAD   0x00FFFFFFu
#define C_ACCENT 0x003A7BD5u
#define C_SEL    0x002C3E5Cu

enum { PG_DISPLAY = 0, PG_SCREEN, PG_SOUND, PG_KEYBOARD, PG_NETWORK, PG_TIME, PG_STARTUP, PG_ABOUT, PG_COUNT };
static const char* const PAGE_NAMES[PG_COUNT] = { "Display", "Screen", "Sound", "Keyboard", "Network", "Date & time", "Startup", "About" };

static const char* const LAYOUTS[] = { "EN (Default)", "fr_CH", "FR", "DE", "de_CH", "BEPO" };
#define NLAYOUTS ((int)(sizeof(LAYOUTS) / sizeof(LAYOUTS[0])))

static int        g_open;
static win_geom_t g_win = { .x = 110, .y = 50, .w = 600, .h = 430, .min_w = 520, .min_h = 360 };
static int        g_page;
static uint32_t   g_gen;
static char       g_status[96];
static int        g_muted, g_vol_before_mute = 80;
static int        g_dragging_vol;
static uint32_t   g_tick_s;

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void button(int x, int y, int w, const char* label, int on) {
    uint32_t base = on ? C_ACCENT : 0x00303740u;
    bevel(x, y, w, 20, base, 0x00535D6Eu, 0x0015191Fu);
    int tw = (int)strlen(label) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 6, label, C_TEXT, base);
}

static void label(int x, int y, const char* s, uint32_t c) { gfx_draw_text(x, y, s, c, C_PANEL); }

static void save_setting(const char* key, const char* value) {
    if (cfg_set(CFG_SETTINGS, key, value, "# Banana OS settings (the Settings app writes this)\n") != 0) {
        kstrlcpy(g_status, "could not save the setting", sizeof(g_status));
        return;
    }
    cfg_persist();
}

void settings_set(const char* key, const char* value) { save_setting(key, value); }
int  settings_get(const char* key, char* out, int cap) { return cfg_get(CFG_SETTINGS, key, out, cap); }

/* ── layout of the content area ───────────────────────────────────── */

static int cx0(void) { return g_win.x + SIDE_W + 16; }
static int cy0(void) { return g_win.y + BODY_Y + 6; }
static int cw(void) { return g_win.w - SIDE_W - 28; }

/* Display: the wallpaper thumbnails */
#define THUMB_W 98
#define THUMB_H 60
static int thumbs_per_row(void) { int n = (cw() + 10) / (THUMB_W + 10); return n < 1 ? 1 : n; }
static void thumb_rect(int i, int* x, int* y) {
    int per = thumbs_per_row();
    *x = cx0() + (i % per) * (THUMB_W + 10);
    *y = cy0() + 42 + (i / per) * (THUMB_H + 24);
}

/* Sound: the volume slider */
static void slider_rect(int* x, int* y, int* w) { *x = cx0(); *y = cy0() + 64; *w = cw() - 60; }

static void set_volume_at(int mx) {
    int x, y, w;
    slider_rect(&x, &y, &w);
    int v = (mx - x) * 100 / (w > 0 ? w : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume(v);
    g_muted = 0;
}

static void save_volume(void) {
    char v[8];
    ksnprintf(v, sizeof(v), "%d", audio_get_volume());
    save_setting("volume", v);
}

/* ── pages ────────────────────────────────────────────────────────── */

/* the UI font switch, right of the "Wallpaper" heading */
#define FONT_BX(x) ((x) + 216)
static void draw_font_switch(int x, int y) {
    label(FONT_BX(x) - 48, y, "Font:", C_DIM);
    button(FONT_BX(x), y - 6, 64, "Smooth", gfx_smooth_text());
    button(FONT_BX(x) + 68, y - 6, 72, "Classic", !gfx_smooth_text());
}

static void draw_display(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Wallpaper", C_HEAD);
    draw_font_switch(x, y);
    const char* file = wallpaper_current_file();
    char line[96];
    if (file && *file) ksnprintf(line, sizeof(line), "Now: picture %s", file);
    else {
        const wallpaper_preset_t* p = wallpaper_preset(wallpaper_current_preset());
        ksnprintf(line, sizeof(line), "Now: %s", p ? p->name : "-");
    }
    label(x, y + 18, line, C_DIM);
    int n = wallpaper_preset_count();
    int cur = wallpaper_current_preset();
    for (int i = 0; i < n; i++) {
        int tx, ty;
        thumb_rect(i, &tx, &ty);
        if (ty + THUMB_H + 20 > g_win.y + g_win.h - 28) break;
        const wallpaper_preset_t* p = wallpaper_preset(i);
        gfx_fill_rect(tx - 2, ty - 2, THUMB_W + 4, THUMB_H + 4, i == cur ? C_ACCENT : 0x0010141Cu);
        if (p && p->pixels) {
            /* a scaled-down copy of the picture */
            for (int yy = 0; yy < THUMB_H; yy += 2)
                for (int xx = 0; xx < THUMB_W; xx += 2) {
                    int sx = xx * WALLPAPER_IMG_W / THUMB_W, sy = yy * WALLPAPER_IMG_H / THUMB_H;
                    const uint8_t* px = p->pixels + (sy * WALLPAPER_IMG_W + sx) * 3;
                    gfx_fill_rect(tx + xx, ty + yy, 2, 2, (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2]);
                }
        } else {
            gfx_fill_rect(tx, ty, THUMB_W, THUMB_H, p ? p->base : 0);
        }
        char nm[13];
        kstrlcpy(nm, p ? p->name : "?", sizeof(nm));
        label(tx, ty + THUMB_H + 6, nm, i == cur ? C_HEAD : C_DIM);
    }
    label(x, g_win.y + g_win.h - 40, "Own picture: right-click it in Files.", C_DIM);
}

static void draw_sound(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Sound", C_HEAD);
    char line[96];
    ksnprintf(line, sizeof(line), "Device: %s", audio_device_name());
    label(x, y + 18, line, C_DIM);
    int vol = audio_get_volume();
    ksnprintf(line, sizeof(line), "Volume: %d%%%s", vol, g_muted ? " (muted)" : "");
    label(x, y + 44, line, C_TEXT);
    int sx, sy, sw;
    slider_rect(&sx, &sy, &sw);
    bevel(sx, sy + 6, sw, 8, 0x00141920u, 0x0010141Cu, 0x00404B5Cu);
    gfx_fill_rect(sx + 1, sy + 7, (sw - 2) * vol / 100, 6, g_muted ? 0x00606A78u : C_ACCENT);
    int kx = sx + (sw - 10) * vol / 100;
    bevel(kx, sy, 10, 20, 0x00C8D2E0u, 0x00FFFFFFu, 0x00606A78u);
    button(sx + sw + 12, sy, 40, "Max", 0);
    button(x, sy + 36, 100, g_muted ? "Unmute" : "Mute", g_muted);
    button(x + 112, sy + 36, 120, "Test sound", 0);
}

static void draw_keyboard(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Keyboard layout", C_HEAD);
    label(x, y + 18, "Choose how the keys of your keyboard are read:", C_DIM);
    const char* cur = keyboard_layout_name();
    for (int i = 0; i < NLAYOUTS; i++) {
        int ry = y + 44 + i * ITEM_H;
        int on = cur && strcmp(cur, LAYOUTS[i]) == 0;
        bevel(x, ry, 14, 14, 0x00141920u, 0x0010141Cu, 0x00404B5Cu);
        if (on) gfx_fill_rect(x + 4, ry + 4, 6, 6, C_ACCENT);
        label(x + 24, ry + 3, LAYOUTS[i], on ? C_HEAD : C_TEXT);
    }
}

/* Screen: the resolution */
static void draw_screen(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Screen resolution", C_HEAD);
    const fb_info_t* fi = fb_info();
    char line[96];
    ksnprintf(line, sizeof(line), "Now: %u x %u", fi ? fi->width : 0, fi ? fi->height : 0);
    label(x, y + 18, line, C_DIM);
    display_mode_t modes[16];
    int n = display_modes(modes, 16);
    for (int i = 0; i < n; i++) {
        int ry = y + 44 + i * ITEM_H;
        int on = fi && modes[i].w == (int)fi->width && modes[i].h == (int)fi->height;
        char nm[32];
        ksnprintf(nm, sizeof(nm), "%d x %d", modes[i].w, modes[i].h);
        bevel(x, ry, 14, 14, 0x00141920u, 0x0010141Cu, 0x00404B5Cu);
        if (on) gfx_fill_rect(x + 4, ry + 4, 6, 6, C_ACCENT);
        label(x + 24, ry + 3, nm, on ? C_HEAD : C_TEXT);
    }
    if (!display_can_change())
        label(x, y + 44 + n * ITEM_H + 8, "This display keeps the mode set at boot (changing it needs QEMU, Bochs or VirtualBox graphics).", C_DIM);
}

/* Startup: what starts at boot (/etc/rc.conf), and the SSH password */
#define SU_ROW_H 34
static char g_pw[2][64];
static int  g_pw_focus = -1;        /* the password field being typed in, or -1 */

static int rc_on(const char* name) {
    char v[8];
    return cfg_get(CFG_SERVICES, name, v, sizeof(v)) && strcmp(v, "yes") == 0;
}
static uint16_t rc_port(const char* name, uint16_t def) {
    char key[32], v[8];
    uint32_t n;
    ksnprintf(key, sizeof(key), "%s_port", name);
    if (cfg_get(CFG_SERVICES, key, v, sizeof(v)) && k_parse_u32(v, &n) && n > 0 && n < 65536) return (uint16_t)n;
    return def;
}
/* the same keys `httpd boot on` / `sshd boot on` write */
static void rc_set(const char* name, int on, uint16_t port) {
    cfg_set(CFG_SERVICES, name, on ? "yes" : NULL, RC_CONF_HEADER);
    if (port) {
        char key[32], v[8];
        ksnprintf(key, sizeof(key), "%s_port", name);
        ksnprintf(v, sizeof(v), "%u", port);
        cfg_set(CFG_SERVICES, key, on ? v : NULL, RC_CONF_HEADER);
    }
    cfg_persist();
}

static void checkbox(int x, int y, int on) {
    bevel(x, y, 14, 14, 0x00141920u, 0x0010141Cu, 0x00404B5Cu);
    if (on) { gfx_fill_rect(x + 3, y + 3, 8, 8, C_ACCENT); }
}

static void su_rows(int y, int* ry) { for (int i = 0; i < 4; i++) ry[i] = y + 44 + i * SU_ROW_H; }
static int  su_pw_y(int y) { return y + 44 + 4 * SU_ROW_H + 14; }

static void draw_startup(void) {
    int x = cx0(), y = cy0(), w = cw();
    label(x, y, "Startup", C_HEAD);
    label(x, y + 18, "Started by themselves when Banana OS boots:", C_DIM);
    int ry[4];
    su_rows(y, ry);
    char line[96];

    checkbox(x, ry[3], login_enabled());
    label(x + 24, ry[3] + 3, passwd_is_set(PASSWD_USER) ? "Ask for the password at boot (login screen)"
                                                        : "Ask for the password at boot (set one below first)", C_TEXT);

    checkbox(x, ry[0], rc_on("desktop"));
    label(x + 24, ry[0] + 3, "Desktop (start the GUI instead of the text console)", C_TEXT);

    checkbox(x, ry[1], rc_on("httpd"));
    ksnprintf(line, sizeof(line), "Web server (httpd), port %u", rc_port("httpd", 80));
    label(x + 24, ry[1] + 3, line, C_TEXT);
    label(x + w - 170, ry[1] + 3, httpd_running() ? "running" : "stopped", httpd_running() ? 0x0080E080u : C_DIM);
    button(x + w - 90, ry[1] - 3, 86, httpd_running() ? "Stop" : "Start now", 0);

    checkbox(x, ry[2], rc_on("sshd"));
    ksnprintf(line, sizeof(line), "SSH server (sshd), port %u", rc_port("sshd", 22));
    label(x + 24, ry[2] + 3, line, C_TEXT);
    label(x + w - 170, ry[2] + 3, sshd_running() ? "running" : "stopped", sshd_running() ? 0x0080E080u : C_DIM);
    button(x + w - 90, ry[2] - 3, 86, sshd_running() ? "Stop" : "Start now", 0);

    /* the password */
    int py = su_pw_y(y);
    label(x, py, "Password (for SSH logins)", C_HEAD);
    int set = passwd_is_set(PASSWD_USER);
    label(x, py + 18, set ? "A password is set." : "No password yet: the SSH server will not start without one.",
          set ? C_DIM : 0x00FFC060u);
    static const char* const NAMES[2] = { "New:", "Again:" };
    for (int f = 0; f < 2; f++) {
        int fy = py + 42 + f * 26;
        label(x, fy + 4, NAMES[f], C_TEXT);
        bevel(x + 60, fy, 200, 20, 0x00141920u, 0x0010141Cu, g_pw_focus == f ? C_ACCENT : 0x00404B5Cu);
        int n = u8_cols(g_pw[f], (int)strlen(g_pw[f]));
        char stars[40];
        int k = 0;
        for (; k < n && k < 24; k++) stars[k] = '*';
        stars[k] = 0;
        gfx_draw_text(x + 66, fy + 6, stars, C_TEXT, 0x00141920u);
        if (g_pw_focus == f && (timer_ms() / 500) % 2 == 0) gfx_fill_rect(x + 66 + k * 8, fy + 4, 2, 12, C_TEXT);
    }
    button(x + 272, py + 42, 120, set ? "Change" : "Set password", 0);

    label(x, py + 106, fsdisk_is_installed()
          ? "Kept on the installed disk (saved automatically)."
          : "Live session: run `install` to keep these after a reboot.", C_DIM);
}

static void su_submit_password(void) {
    if ((int)strlen(g_pw[0]) < PASSWD_MIN) {
        ksnprintf(g_status, sizeof(g_status), "The password needs at least %d characters", PASSWD_MIN);
    } else if (strcmp(g_pw[0], g_pw[1]) != 0) {
        kstrlcpy(g_status, "The two passwords are not the same", sizeof(g_status));
    } else if (passwd_set(PASSWD_USER, g_pw[0]) != 0) {
        kstrlcpy(g_status, "Could not save the password", sizeof(g_status));
    } else {
        kstrlcpy(g_status, fsdisk_is_installed() ? "Password saved" : "Password saved (until reboot: not installed)",
                 sizeof(g_status));
    }
    memset(g_pw, 0, sizeof(g_pw));
    g_pw_focus = -1;
}

static void click_startup(int mx, int my) {
    int x = cx0(), y = cy0(), w = cw();
    int ry[4];
    su_rows(y, ry);
    char err[96];
    err[0] = 0;
    /* Start / Stop now */
    if (inside(mx, my, x + w - 90, ry[1] - 3, 86, 20)) {
        if (httpd_running()) { httpd_stop(); kstrlcpy(g_status, "Web server stopped", sizeof(g_status)); }
        else if (httpd_start(rc_port("httpd", 80), err, sizeof(err)) == 0) kstrlcpy(g_status, "Web server started", sizeof(g_status));
        else ksnprintf(g_status, sizeof(g_status), "httpd: %s", err);
        return;
    }
    if (inside(mx, my, x + w - 90, ry[2] - 3, 86, 20)) {
        if (sshd_running()) { sshd_stop(); kstrlcpy(g_status, "SSH server stopped", sizeof(g_status)); }
        else if (sshd_start(rc_port("sshd", 22), err, sizeof(err)) == 0) kstrlcpy(g_status, "SSH server started", sizeof(g_status));
        else ksnprintf(g_status, sizeof(g_status), "sshd: %s", err);
        return;
    }
    /* the boot switches */
    if (inside(mx, my, x, ry[0] - 4, w - 100, 24)) {
        int on = !rc_on("desktop");
        rc_set("desktop", on, 0);
        kstrlcpy(g_status, on ? "The desktop starts at boot" : "Boots to the text console", sizeof(g_status));
        return;
    }
    if (inside(mx, my, x, ry[1] - 4, w - 180, 24)) {
        int on = !rc_on("httpd");
        rc_set("httpd", on, rc_port("httpd", 80));
        kstrlcpy(g_status, on ? "The web server starts at boot" : "The web server no longer starts at boot", sizeof(g_status));
        return;
    }
    if (inside(mx, my, x, ry[2] - 4, w - 180, 24)) {
        int on = !rc_on("sshd");
        rc_set("sshd", on, rc_port("sshd", 22));
        kstrlcpy(g_status, !on ? "The SSH server no longer starts at boot"
                 : passwd_is_set(PASSWD_USER) ? "The SSH server starts at boot"
                 : "The SSH server starts at boot (set a password below)", sizeof(g_status));
        return;
    }
    if (inside(mx, my, x, ry[3] - 4, w - 100, 24)) {
        int on = !login_enabled();
        login_set_enabled(on);
        kstrlcpy(g_status, !on ? "No login screen at boot"
                 : passwd_is_set(PASSWD_USER) ? "The password is asked at boot"
                 : "The password will be asked at boot once one is set", sizeof(g_status));
        return;
    }
    /* the password fields and button */
    int py = su_pw_y(y);
    g_pw_focus = -1;
    for (int f = 0; f < 2; f++)
        if (inside(mx, my, x + 60, py + 42 + f * 26, 200, 20)) { g_pw_focus = f; return; }
    if (inside(mx, my, x + 272, py + 42, 120, 20)) su_submit_password();
}

/* typing into the password fields */
void settings_key(char c) {
    if (!g_open) return;
    if (g_page != PG_STARTUP || g_pw_focus < 0) {
        if (c == 27) settings_close();              /* Esc closes, as in the other windows */
        return;
    }
    char* s = g_pw[g_pw_focus];
    int n = (int)strlen(s);
    if (c == '\n') { if (g_pw_focus == 0) g_pw_focus = 1; else su_submit_password(); }
    else if (c == '\t') g_pw_focus ^= 1;
    else if (c == 27) { memset(g_pw, 0, sizeof(g_pw)); g_pw_focus = -1; }
    else if (c == '\b') { if (n) u8_backspace(s, n); }
    else if ((unsigned char)c >= 32 && n < (int)sizeof(g_pw[0]) - 1) { s[n] = c; s[n + 1] = 0; }
    g_gen++;
}

static void draw_network(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Network", C_HEAD);
    netif_t* nf = net_if();
    char a[16], line[96];
    if (!nf || !nf->dev) {
        label(x, y + 24, "No network card was found.", C_DIM);
        return;
    }
    ksnprintf(line, sizeof(line), "Card: %s", nf->dev->ifname);
    label(x, y + 24, line, C_TEXT);
    ksnprintf(line, sizeof(line), "Status: %s", nf->configured ? "connected" : "not connected");
    label(x, y + 42, line, nf->configured ? 0x0080E080u : 0x00E0A060u);
    ksnprintf(line, sizeof(line), "Mode: %s", nf->dhcp ? "automatic (DHCP)" : "manual (static address)");
    label(x, y + 60, line, C_TEXT);
    ip4_to_str(nf->ip, a);     ksnprintf(line, sizeof(line), "Address:  %s", nf->configured ? a : "-");  label(x, y + 84, line, C_TEXT);
    ip4_to_str(nf->netmask, a); ksnprintf(line, sizeof(line), "Netmask:  %s", nf->configured ? a : "-"); label(x, y + 100, line, C_TEXT);
    ip4_to_str(nf->gateway, a); ksnprintf(line, sizeof(line), "Gateway:  %s", nf->configured ? a : "-"); label(x, y + 116, line, C_TEXT);
    ip4_to_str(nf->dns, a);     ksnprintf(line, sizeof(line), "DNS:      %s", nf->configured ? a : "-"); label(x, y + 132, line, C_TEXT);
    button(x, y + 160, 200, nf->dhcp ? "Renew address (DHCP)" : "Switch to automatic", 0);
    label(x, y + 194, "A manual address: `ifconfig` in a terminal.", C_DIM);
}

static void draw_time(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Date & time", C_HEAD);
    rtc_datetime_t t;
    char line[96];
    static const char* const MON[12] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };
    if (rtc_read_datetime(&t) == 0 && t.month >= 1 && t.month <= 12) {
        ksnprintf(line, sizeof(line), "%02u:%02u:%02u", t.hour, t.minute, t.second);
        gfx_draw_text_scaled(x, y + 28, 3, line, C_HEAD, C_PANEL);
        ksnprintf(line, sizeof(line), "%u %s %u", t.day, MON[t.month - 1], t.year);
        label(x, y + 64, line, C_TEXT);
    } else {
        label(x, y + 28, "The clock could not be read.", C_DIM);
    }
    uint32_t up = timer_ms() / 1000;
    ksnprintf(line, sizeof(line), "Up for %uh %02um %02us", up / 3600, (up / 60) % 60, up % 60);
    label(x, y + 92, line, C_DIM);
    label(x, y + 120, "The time comes from the computer's hardware clock.", C_DIM);
}

static void draw_about(void) {
    int x = cx0(), y = cy0();
    gfx_draw_text_scaled(x, y, 2, "Banana OS 0.5", C_HEAD, C_PANEL);
    const sysinfo_t* si = sysinfo_get();
    char line[112];
    int ly = y + 32;
#ifdef __x86_64__
    label(x, ly, "64-bit (x86_64)", C_TEXT);
#else
    label(x, ly, "32-bit (i686)", C_TEXT);
#endif
    ly += 18;
    if (si) {
        ksnprintf(line, sizeof(line), "Firmware: %s", si->uefi ? "UEFI" : "BIOS");
        label(x, ly, line, C_TEXT); ly += 18;
        ksnprintf(line, sizeof(line), "Processor: %s", si->cpu_brand[0] ? si->cpu_brand : si->cpu_vendor);
        if ((int)strlen(line) * 8 > cw()) line[cw() / 8] = 0;
        label(x, ly, line, C_TEXT); ly += 18;
        ksnprintf(line, sizeof(line), "Memory: %u MB", si->mem_kb / 1024);
        label(x, ly, line, C_TEXT); ly += 18;
    }
    ksnprintf(line, sizeof(line), "Kernel heap: %u of %u MB used", kheap_used_bytes() >> 20, kheap_total_bytes() >> 20);
    label(x, ly, line, C_TEXT); ly += 18;
    ksnprintf(line, sizeof(line), "Files: %u of %u, folders %u of %u", fs_used_files(), fs_max_files(), fs_used_dirs(), fs_max_dirs());
    label(x, ly, line, C_TEXT); ly += 18;
    char res[32];
    const fb_info_t* fi = fb_info();
    ksnprintf(res, sizeof(res), "Screen: %ux%u", fi ? fi->width : 0, fi ? fi->height : 0);
    label(x, ly, res, C_TEXT); ly += 26;
    if (sysinfo_live_boot())
        label(x, ly, fsdisk_find_install(NULL)
              ? "Running: the live CD (Banana OS is also installed here: 'update' updates it)"
              : "Running: the live CD - changes are lost at shutdown ('install' to keep them)", 0x00FFB060u);
    else if (fsdisk_is_installed())
        label(x, ly, "Running: the installed system (changes are saved automatically)", 0x0080E080u);
    else
        label(x, ly, "Running: not installed - changes are lost at shutdown", 0x00FFB060u);
}

/* ── window ───────────────────────────────────────────────────────── */

void settings_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    bevel(x, y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    bevel(x + 3, y + 3, W - 6, TITLE_H - 1, C_TITLE, 0x00647692u, 0x00111923u);
    gfx_draw_text(x + 10, y + 7, "Settings", 0x00FFFFFFu, C_TITLE);
    win_draw_buttons(&g_win, 4, 12);

    /* the pages, on the left */
    gfx_fill_rect(x + 6, y + BODY_Y, SIDE_W, H - BODY_Y - 26, C_SIDE);
    for (int i = 0; i < PG_COUNT; i++) {
        int iy = y + BODY_Y + 4 + i * ITEM_H;
        uint32_t bg = i == g_page ? C_SEL : C_SIDE;
        gfx_fill_rect(x + 8, iy, SIDE_W - 4, ITEM_H - 4, bg);
        if (i == g_page) gfx_fill_rect(x + 8, iy, 3, ITEM_H - 4, C_ACCENT);
        gfx_draw_text(x + 18, iy + 7, PAGE_NAMES[i], i == g_page ? C_HEAD : C_DIM, bg);
    }

    switch (g_page) {
    case PG_DISPLAY:  draw_display(); break;
    case PG_SCREEN:   draw_screen(); break;
    case PG_SOUND:    draw_sound(); break;
    case PG_KEYBOARD: draw_keyboard(); break;
    case PG_NETWORK:  draw_network(); break;
    case PG_TIME:     draw_time(); break;
    case PG_STARTUP:  draw_startup(); break;
    default:          draw_about(); break;
    }
    if (g_status[0]) gfx_draw_text(x + 10, y + H - 18, g_status, C_DIM, C_PANEL);
    gfx_draw_grip(x + W, y + H);
}

void settings_click(int mx, int my) {
    if (!settings_contains(mx, my)) return;
    int lx = mx - g_win.x, ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { settings_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    g_status[0] = 0;

    /* sidebar */
    if (lx >= 8 && lx < 8 + SIDE_W - 4) {
        int i = (ly - BODY_Y - 4) / ITEM_H;
        if (ly >= BODY_Y + 4 && i >= 0 && i < PG_COUNT) g_page = i;
        return;
    }

    int x = cx0(), y = cy0();
    if (g_page == PG_DISPLAY) {
        if (inside(mx, my, FONT_BX(x), y - 6, 64, 20) || inside(mx, my, FONT_BX(x) + 68, y - 6, 72, 20)) {
            int smooth = mx < FONT_BX(x) + 66;
            gfx_set_smooth_text(smooth);
            save_setting("ui_font", smooth ? "smooth" : "classic");
            ksnprintf(g_status, sizeof(g_status), "Font: %s", smooth ? "smooth (DejaVu Sans Mono)" : "classic (8x8)");
            return;
        }
        for (int i = 0; i < wallpaper_preset_count(); i++) {
            int tx, ty;
            thumb_rect(i, &tx, &ty);
            if (inside(mx, my, tx, ty, THUMB_W, THUMB_H + 18)) {
                wallpaper_set_preset(i);
                const wallpaper_preset_t* p = wallpaper_preset(i);
                ksnprintf(g_status, sizeof(g_status), "Wallpaper: %s", p ? p->name : "");
                return;
            }
        }
    } else if (g_page == PG_SOUND) {
        int sx, sy, sw;
        slider_rect(&sx, &sy, &sw);
        if (inside(mx, my, sx - 6, sy, sw + 12, 20)) { set_volume_at(mx); g_dragging_vol = 1; return; }
        if (inside(mx, my, sx + sw + 12, sy, 40, 20)) { audio_set_volume(100); g_muted = 0; save_volume(); return; }
        if (inside(mx, my, x, sy + 36, 100, 20)) {
            if (!g_muted) { g_vol_before_mute = audio_get_volume(); audio_set_volume(0); g_muted = 1; }
            else { audio_set_volume(g_vol_before_mute ? g_vol_before_mute : 80); g_muted = 0; }
            save_volume();
            return;
        }
        if (inside(mx, my, x + 112, sy + 36, 120, 20)) { audio_beep(880, 150); return; }
    } else if (g_page == PG_SCREEN) {
        display_mode_t modes[16];
        int n = display_modes(modes, 16);
        for (int i = 0; i < n; i++) {
            int ry = y + 44 + i * ITEM_H;
            if (inside(mx, my, x, ry - 4, cw(), ITEM_H - 2)) {
                char v[32];
                ksnprintf(v, sizeof(v), "%dx%d", modes[i].w, modes[i].h);
                if (display_set_mode(modes[i].w, modes[i].h) == 0) {
                    save_setting("resolution", v);
                    win_clamp(&g_win);
                    ksnprintf(g_status, sizeof(g_status), "Resolution: %d x %d", modes[i].w, modes[i].h);
                } else {
                    ksnprintf(g_status, sizeof(g_status), "%d x %d: this display cannot show it", modes[i].w, modes[i].h);
                }
                return;
            }
        }
    } else if (g_page == PG_KEYBOARD) {
        for (int i = 0; i < NLAYOUTS; i++) {
            int ry = y + 44 + i * ITEM_H;
            if (inside(mx, my, x, ry - 4, cw(), ITEM_H - 2)) {
                if (keyboard_set_layout(LAYOUTS[i]) == 0) {
                    save_setting("keyboard", LAYOUTS[i]);
                    ksnprintf(g_status, sizeof(g_status), "Keyboard layout: %s", LAYOUTS[i]);
                }
                return;
            }
        }
    } else if (g_page == PG_STARTUP) {
        click_startup(mx, my);
    } else if (g_page == PG_NETWORK) {
        netif_t* nf = net_if();
        if (nf && nf->dev && inside(mx, my, x, y + 160, 200, 20)) {
            if (!nf->dhcp) netconf_save_dhcp(nf->dev);
            nf->configured = 0;
            dhcp_start();
            kstrlcpy(g_status, "Asking the network for an address...", sizeof(g_status));
            return;
        }
    }
}

void settings_mouse(int mx, int my, int left) {
    (void)my;
    if (g_dragging_vol) {
        if (left) { set_volume_at(mx); g_gen++; }
        else { g_dragging_vol = 0; save_volume(); g_gen++; }
        return;
    }
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

void settings_rclick(int mx, int my) { (void)mx; (void)my; }

void settings_show_status(const char* msg) { kstrlcpy(g_status, msg, sizeof(g_status)); g_gen++; }

void settings_open_page(int page) {
    settings_open();
    if (page >= 0 && page < PG_COUNT) g_page = page;
}

void settings_open(void) {
    g_open = 1;
    g_status[0] = 0;
    win_clamp(&g_win);
    g_gen++;
}

void settings_close(void) {
    g_open = 0;
    memset(g_pw, 0, sizeof(g_pw));
    g_pw_focus = -1;
    g_win.dragging = g_win.resizing = 0;
    g_dragging_vol = 0;
    g_gen++;
}

int settings_is_open(void) { return g_open; }
int settings_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }

uint32_t settings_signature(void) {
    if (!g_open) return 0;
    /* the clock, uptime and network status change by themselves */
    if (g_page == PG_TIME || g_page == PG_NETWORK || g_page == PG_ABOUT || g_page == PG_STARTUP) {
        uint32_t s = timer_ms() / (g_page == PG_STARTUP ? 500 : 1000);
        if (s != g_tick_s) { g_tick_s = s; g_gen++; }
    }
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^
           (uint32_t)g_page << 28 ^ (uint32_t)audio_get_volume() << 8 ^ wallpaper_generation() ^ (uint32_t)gfx_smooth_text() << 27;
}

void settings_boot(void) {
    char v[32];
    if (cfg_get(CFG_SETTINGS, "volume", v, sizeof(v))) {
        uint32_t n = 0;
        if (k_parse_u32(v, &n) && n <= 100) audio_set_volume((int)n);
    }
    if (cfg_get(CFG_SETTINGS, "keyboard", v, sizeof(v))) keyboard_set_layout(v);
    if (cfg_get(CFG_SETTINGS, "ui_font", v, sizeof(v))) gfx_set_smooth_text(strcmp(v, "classic") != 0);
    if (cfg_get(CFG_SETTINGS, "resolution", v, sizeof(v))) {       /* "1024x768" */
        uint32_t w = 0, h = 0;
        const char* xp = strchr(v, 'x');
        if (xp && k_parse_u32(v, &w) && k_parse_u32(xp + 1, &h)) display_set_mode((int)w, (int)h);
    }
}
