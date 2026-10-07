/* "Install Banana OS": the live CD's installer window. It finds the hard
 * disk, asks for the keyboard layout, a password and what starts at boot,
 * then installs (fsdisk_install) - or, when the disk already holds Banana
 * OS, updates it and keeps its files (fsdisk_update). The work runs in its
 * own task, so the window shows the progress. Only on the live CD. */
#include "installer.h"
#include "gfx.h"
#include "winframe.h"
#include "fsdisk.h"
#include "disk.h"
#include "kstring.h"
#include "keyboard.h"
#include "settings.h"
#include "passwd.h"
#include "login.h"
#include "config.h"
#include "task.h"
#include "timer.h"
#include "sysinfo.h"
#include "utf8.h"

#define TITLE_H  20
#define C_PANEL  0x001D232Cu
#define C_TITLE  0x00384562u
#define C_TEXT   0x00E8EEF6u
#define C_DIM    0x00AAB6C6u
#define C_HEAD   0x00FFFFFFu
#define C_ACCENT 0x003A7BD5u
#define C_WARN   0x00FFB060u
#define C_ERR    0x00FF8A80u
#define C_OK     0x0080E080u
#define C_FIELD  0x00141920u

enum { P_WELCOME, P_SETUP, P_CONFIRM, P_WORKING, P_DONE };
enum { M_INSTALL, M_UPDATE };

static const char* const LAYOUTS[] = { "EN (Default)", "fr_CH", "FR", "DE", "de_CH", "BEPO" };
#define NLAYOUTS 6

static int        g_open;
static win_geom_t g_win = { .x = 130, .y = 40, .w = 560, .h = 440, .min_w = 520, .min_h = 420 };
static uint32_t   g_gen, g_tick;
static int        g_page, g_mode;
static char       g_status[112];

/* the disk */
static int        g_disks;            /* hard disks found (exactly one is needed) */
static ata_disk_t g_disk;
static int        g_has_install;
static char       g_disk_desc[96];

/* the choices */
static int  g_layout;
static char g_pw[2][64];
static int  g_focus = -1;             /* password field being typed in */
static int  g_desktop = 1, g_login = 1, g_httpd = 0, g_sshd = 0;

/* the work (installer task) */
static volatile int g_running, g_result;
static volatile const char* g_phase;

/* ── small drawing helpers (as in Settings) ── */

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}
static void label(int x, int y, const char* s, uint32_t c) { gfx_draw_text(x, y, s, c, C_PANEL); }
static void button(int x, int y, int w, const char* text, int primary, int enabled) {
    uint32_t base = !enabled ? 0x00262B33u : primary ? C_ACCENT : 0x00303740u;
    bevel(x, y, w, 22, base, 0x00535D6Eu, 0x0015191Fu);
    int tw = u8_cols(text, (int)strlen(text)) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 7, text, enabled ? C_TEXT : 0x00687384u, base);
}
static void checkbox(int x, int y, int on, const char* text) {
    bevel(x, y, 14, 14, C_FIELD, 0x0010141Cu, 0x00404B5Cu);
    if (on) gfx_fill_rect(x + 3, y + 3, 8, 8, C_ACCENT);
    label(x + 24, y + 3, text, C_TEXT);
}
static void radio(int x, int y, int on, const char* text) {
    bevel(x, y, 14, 14, C_FIELD, 0x0010141Cu, 0x00404B5Cu);
    if (on) gfx_fill_rect(x + 4, y + 4, 6, 6, C_ACCENT);
    label(x + 24, y + 3, text, on ? C_HEAD : C_TEXT);
}

static int cx0(void) { return g_win.x + 20; }
static int cy0(void) { return g_win.y + TITLE_H + 16; }
static int cw(void)  { return g_win.w - 40; }
/* the button row at the bottom: right edge, y */
static int btn_y(void) { return g_win.y + g_win.h - 44; }
static int btn_r(void) { return g_win.x + g_win.w - 20; }

/* ── the disk ── */

static void look_for_disk(void) {
    ata_disk_t d;
    int r = fsdisk_find_target(&d);
    g_disks = r == 1 ? 1 : r < 0 ? 2 : 0;
    g_has_install = 0;
    g_disk_desc[0] = 0;
    if (g_disks == 1) {
        g_disk = d;
        char where[48];
        disk_describe(&d, where, sizeof(where));
        ksnprintf(g_disk_desc, sizeof(g_disk_desc), "%s - %s (%u MB)", where,
                  d.model[0] ? d.model : "hard disk", (uint32_t)(d.sectors / 2048u));
        g_has_install = fsdisk_find_install(0);
    }
    g_mode = g_has_install ? M_UPDATE : M_INSTALL;
}

static int disk_big_enough(void) { return g_disks == 1 && g_disk.sectors / 2048u >= 160u; }

/* ── the work ── */

static void installer_task(void) {
    int rc;
    if (g_mode == M_UPDATE) {
        g_phase = "Updating the system on the disk...";
        rc = fsdisk_update();
    } else {
        /* the choices go into this (live) system first: install writes it to the disk */
        g_phase = "Saving your settings...";
        settings_set("keyboard", LAYOUTS[g_layout]);
        keyboard_set_layout(LAYOUTS[g_layout]);
        if (g_pw[0][0]) passwd_set(PASSWD_USER, g_pw[0]);
        cfg_set(CFG_SERVICES, "desktop", g_desktop ? "yes" : 0, RC_CONF_HEADER);
        login_set_enabled(g_login);
        cfg_set(CFG_SERVICES, "httpd", g_httpd ? "yes" : 0, RC_CONF_HEADER);
        cfg_set(CFG_SERVICES, "httpd_port", g_httpd ? "80" : 0, RC_CONF_HEADER);
        cfg_set(CFG_SERVICES, "sshd", g_sshd ? "yes" : 0, RC_CONF_HEADER);
        cfg_set(CFG_SERVICES, "sshd_port", g_sshd ? "22" : 0, RC_CONF_HEADER);
        memset(g_pw, 0, sizeof(g_pw));
        g_phase = "Copying Banana OS onto the disk...";
        rc = fsdisk_install();
    }
    g_result = rc;
    g_running = 0;
    g_gen++;
}

static void start_work(void) {
    g_page = P_WORKING;
    g_running = 1;
    g_result = 0;
    g_status[0] = 0;
    if (task_create("installer", installer_task) < 0) {
        g_running = 0;
        g_result = FSDISK_ERR_IO;
        g_page = P_DONE;
    }
}

static const char* result_text(int rc) {
    switch (rc) {
    case FSDISK_OK:              return 0;
    case FSDISK_ERR_NO_TARGET:   return "No hard disk was found.";
    case FSDISK_ERR_AMBIGUOUS:   return "More than one hard disk: detach the others and try again.";
    case FSDISK_ERR_NO_SOURCE:   return "No Banana OS CD or USB stick found to copy the system from.";
    case FSDISK_ERR_TOO_SMALL:   return "The disk is too small (256 MB or more is best).";
    case FSDISK_ERR_ISO_TOO_BIG: return "This CD's system does not fit the disk's boot area.";
    case FSDISK_ERR_FULL:        return "The files are too big for this disk.";
    case FSDISK_ERR_NO_INSTALL:  return "There is no Banana OS on the disk to update.";
    default:                     return "A disk read or write failed.";
    }
}

/* ── pages ── */

static void draw_welcome(void) {
    int x = cx0(), y = cy0();
    gfx_draw_text_scaled(x, y, 2, "Install Banana OS", C_HEAD, C_PANEL);
    label(x, y + 30, "Puts Banana OS on this computer's hard disk, so it starts", C_DIM);
    label(x, y + 46, "without the CD and keeps your files and settings.", C_DIM);
    label(x, y + 80, "Disk", C_HEAD);
    if (g_disks == 0) {
        label(x, y + 100, "No hard disk was found.", C_ERR);
        label(x, y + 118, "Add a virtual hard disk (256 MB or more) to the VM, then look again.", C_DIM);
    } else if (g_disks > 1) {
        label(x, y + 100, "More than one hard disk was found.", C_ERR);
        label(x, y + 118, "Leave only the one to install on, then look again.", C_DIM);
    } else {
        label(x, y + 100, g_disk_desc, C_TEXT);
        if (!disk_big_enough())
            label(x, y + 118, "This disk is too small: Banana OS needs at least 160 MB.", C_ERR);
        else if (g_has_install) {
            label(x, y + 124, "Banana OS is already installed on this disk.", C_WARN);
            label(x, y + 142, "Update it with this CD and keep its files, settings and password,", C_DIM);
            label(x, y + 158, "or erase the disk and install from scratch.", C_DIM);
        } else {
            label(x, y + 124, "The disk will be erased and Banana OS installed on it.", C_DIM);
        }
    }
    int by = btn_y(), br = btn_r();
    button(g_win.x + 20, by, 110, "Look again", 0, 1);
    if (g_disks == 1 && disk_big_enough() && g_has_install) {
        button(br - 170, by, 170, "Update (keep files)", 1, 1);
        button(br - 350, by, 170, "Erase and reinstall", 0, 1);
    } else {
        button(br - 100, by, 100, "Next", 1, g_disks == 1 && disk_big_enough());
    }
}

static int setup_y_pw(void)    { return cy0() + 150; }
static int setup_y_boot(void)  { return cy0() + 236; }

static void draw_setup(void) {
    int x = cx0(), y = cy0();
    label(x, y, "Keyboard layout", C_HEAD);
    for (int i = 0; i < NLAYOUTS; i++)
        radio(x + (i % 3) * 170, y + 22 + (i / 3) * 24, i == g_layout, LAYOUTS[i]);

    int py = setup_y_pw();
    label(x, py - 60, "Password", C_HEAD);
    label(x, py - 42, "For the login screen and SSH. Leave it empty for none.", C_DIM);
    static const char* const NAMES[2] = { "Password:", "Again:" };
    for (int f = 0; f < 2; f++) {
        int fy = py - 18 + f * 26;
        label(x, fy + 4, NAMES[f], C_TEXT);
        bevel(x + 90, fy, 220, 20, C_FIELD, 0x0010141Cu, g_focus == f ? C_ACCENT : 0x00404B5Cu);
        int n = u8_cols(g_pw[f], (int)strlen(g_pw[f]));
        char stars[32];
        int k = 0;
        for (; k < n && k < 26; k++) stars[k] = '*';
        stars[k] = 0;
        gfx_draw_text(x + 96, fy + 6, stars, C_TEXT, C_FIELD);
        if (g_focus == f && (timer_ms() / 500) % 2 == 0) gfx_fill_rect(x + 96 + k * 8, fy + 4, 2, 12, C_TEXT);
    }

    int by = setup_y_boot();
    label(x, by, "When the computer starts", C_HEAD);
    checkbox(x, by + 22, g_desktop, "Start the desktop (not the text console)");
    checkbox(x, by + 46, g_login && g_pw[0][0], "Ask for the password (login screen)");
    checkbox(x, by + 70, g_httpd, "Start the web server (httpd, port 80)");
    checkbox(x, by + 94, g_sshd, "Start the SSH server (sshd, port 22 - needs a password)");

    button(g_win.x + 20, btn_y(), 100, "Back", 0, 1);
    button(btn_r() - 100, btn_y(), 100, "Next", 1, 1);
}

static void draw_confirm(void) {
    int x = cx0(), y = cy0();
    char line[128];
    if (g_mode == M_UPDATE) {
        label(x, y, "Ready to update", C_HEAD);
        label(x, y + 24, g_disk_desc, C_TEXT);
        label(x, y + 52, "The system on the disk is replaced with this CD's.", C_DIM);
        label(x, y + 70, "Your files, settings and password are kept.", C_OK);
    } else {
        label(x, y, "Ready to install", C_HEAD);
        label(x, y + 24, g_disk_desc, C_TEXT);
        ksnprintf(line, sizeof(line), "Keyboard: %s", LAYOUTS[g_layout]);
        label(x, y + 56, line, C_TEXT);
        label(x, y + 74, g_pw[0][0] ? "Password: set" : "Password: none", C_TEXT);
        ksnprintf(line, sizeof(line), "At startup: %s%s%s%s",
                  g_desktop ? "desktop" : "text console",
                  (g_login && g_pw[0][0]) ? ", login screen" : "",
                  g_httpd ? ", web server" : "", g_sshd ? ", SSH server" : "");
        label(x, y + 92, line, C_TEXT);
        label(x, y + 126, "Everything on this disk will be erased.", C_ERR);
    }
    button(g_win.x + 20, btn_y(), 100, "Back", 0, 1);
    button(btn_r() - 120, btn_y(), 120, g_mode == M_UPDATE ? "Update" : "Install", 1, 1);
}

static void draw_working(void) {
    int x = cx0(), y = cy0();
    label(x, y, g_mode == M_UPDATE ? "Updating Banana OS" : "Installing Banana OS", C_HEAD);
    label(x, y + 24, g_phase ? (const char*)g_phase : "", C_TEXT);
    uint32_t done = 0, total = 0;
    fsdisk_progress(&done, &total);
    int bw = cw();
    bevel(x, y + 56, bw, 18, C_FIELD, 0x0010141Cu, 0x00404B5Cu);
    int fill = total ? (int)((uint64_t)(bw - 2) * done / total) : 0;
    gfx_fill_rect(x + 1, y + 57, fill, 16, C_ACCENT);
    char pct[16];
    ksnprintf(pct, sizeof(pct), "%u%%", total ? (uint32_t)((uint64_t)done * 100u / total) : 0u);
    label(x, y + 84, pct, C_DIM);
    label(x, y + 110, "Please wait, and do not switch the computer off.", C_WARN);
}

static void draw_done(void) {
    int x = cx0(), y = cy0();
    const char* err = result_text(g_result);
    if (err) {
        label(x, y, g_mode == M_UPDATE ? "The update failed" : "The installation failed", C_ERR);
        label(x, y + 24, err, C_TEXT);
        label(x, y + 48, "Nothing else was changed; you can try again.", C_DIM);
        button(g_win.x + 20, btn_y(), 100, "Back", 0, 1);
        button(btn_r() - 100, btn_y(), 100, "Close", 0, 1);
        return;
    }
    label(x, y, g_mode == M_UPDATE ? "Banana OS is updated" : "Banana OS is installed", C_OK);
    label(x, y + 24, "Remove the CD (or choose the hard disk in the boot menu)", C_TEXT);
    label(x, y + 42, "and restart the computer.", C_TEXT);
    button(btn_r() - 130, btn_y(), 130, "Restart now", 1, 1);
    button(btn_r() - 130 - 10 - 100, btn_y(), 100, "Close", 0, 1);
}

void installer_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    bevel(x, y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    bevel(x + 3, y + 3, W - 6, TITLE_H - 1, C_TITLE, 0x00647692u, 0x00111923u);
    gfx_draw_text(x + 10, y + 7, "Install Banana OS", 0x00FFFFFFu, C_TITLE);
    win_draw_buttons(&g_win, 4, 12);
    if (g_running) g_page = P_WORKING;
    else if (g_page == P_WORKING) g_page = P_DONE;
    switch (g_page) {
    case P_WELCOME: draw_welcome(); break;
    case P_SETUP:   draw_setup(); break;
    case P_CONFIRM: draw_confirm(); break;
    case P_WORKING: draw_working(); break;
    default:        draw_done(); break;
    }
    if (g_status[0]) gfx_draw_text(x + 20, y + H - 66, g_status, C_ERR, C_PANEL);
    gfx_draw_grip(x + W, y + H);
}

/* the setup page's choices, checked before going on */
static int setup_ok(void) {
    if (g_pw[0][0] || g_pw[1][0]) {
        if ((int)strlen(g_pw[0]) < PASSWD_MIN) { ksnprintf(g_status, sizeof(g_status), "The password needs at least %d characters.", PASSWD_MIN); return 0; }
        if (strcmp(g_pw[0], g_pw[1]) != 0) { kstrlcpy(g_status, "The two passwords are not the same.", sizeof(g_status)); return 0; }
    }
    if (g_sshd && !g_pw[0][0]) { kstrlcpy(g_status, "The SSH server needs a password.", sizeof(g_status)); return 0; }
    return 1;
}

static void restart(void) {
    fsdisk_sync();                              /* (anything changed since the install) */
    __asm__ volatile("outb %0,%1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));   /* reset line */
    for (;;) __asm__ volatile("hlt");
}

void installer_click(int mx, int my) {
    if (!g_open) return;
    g_gen++;
    int ly = my - g_win.y;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { installer_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    if (g_running) return;
    g_status[0] = 0;

    int x = cx0(), y = cy0(), by = btn_y(), br = btn_r(), bl = g_win.x + 20;
    switch (g_page) {
    case P_WELCOME:
        if (inside(mx, my, bl, by, 110, 22)) { look_for_disk(); return; }
        if (g_disks == 1 && disk_big_enough() && g_has_install) {
            if (inside(mx, my, br - 170, by, 170, 22)) { g_mode = M_UPDATE; g_page = P_CONFIRM; return; }
            if (inside(mx, my, br - 350, by, 170, 22)) { g_mode = M_INSTALL; g_page = P_SETUP; return; }
        } else if (g_disks == 1 && disk_big_enough() && inside(mx, my, br - 100, by, 100, 22)) {
            g_mode = M_INSTALL;
            g_page = P_SETUP;
        }
        return;
    case P_SETUP: {
        for (int i = 0; i < NLAYOUTS; i++)
            if (inside(mx, my, x + (i % 3) * 170, y + 18 + (i / 3) * 24, 160, 22)) { g_layout = i; return; }
        int py = setup_y_pw();
        g_focus = -1;
        for (int f = 0; f < 2; f++)
            if (inside(mx, my, x + 90, py - 18 + f * 26, 220, 20)) { g_focus = f; return; }
        int bt = setup_y_boot();
        if (inside(mx, my, x, bt + 18, cw(), 22)) { g_desktop = !g_desktop; return; }
        if (inside(mx, my, x, bt + 42, cw(), 22)) {
            if (!g_pw[0][0]) kstrlcpy(g_status, "Type a password first.", sizeof(g_status));
            else g_login = !g_login;
            return;
        }
        if (inside(mx, my, x, bt + 66, cw(), 22)) { g_httpd = !g_httpd; return; }
        if (inside(mx, my, x, bt + 90, cw(), 22)) { g_sshd = !g_sshd; return; }
        if (inside(mx, my, bl, by, 100, 22)) { g_page = P_WELCOME; return; }
        if (inside(mx, my, br - 100, by, 100, 22) && setup_ok()) g_page = P_CONFIRM;
        return;
    }
    case P_CONFIRM:
        if (inside(mx, my, bl, by, 100, 22)) { g_page = g_mode == M_UPDATE ? P_WELCOME : P_SETUP; return; }
        if (inside(mx, my, br - 120, by, 120, 22)) start_work();
        return;
    case P_DONE:
        if (result_text(g_result)) {
            if (inside(mx, my, bl, by, 100, 22)) { look_for_disk(); g_page = P_WELCOME; return; }
            if (inside(mx, my, br - 100, by, 100, 22)) installer_close();
        } else {
            if (inside(mx, my, br - 130, by, 130, 22)) restart();
            if (inside(mx, my, br - 240, by, 100, 22)) installer_close();
        }
        return;
    }
}

void installer_key(char c) {
    if (!g_open || g_running) return;
    if (g_page != P_SETUP || g_focus < 0) {
        if (c == 27) installer_close();
        return;
    }
    char* s = g_pw[g_focus];
    int n = (int)strlen(s);
    if (c == '\n' || c == '\t') g_focus ^= 1;
    else if (c == 27) g_focus = -1;
    else if (c == '\b') { if (n) u8_backspace(s, n); }
    else if ((unsigned char)c >= 32 && n < (int)sizeof(g_pw[0]) - 1) { s[n] = c; s[n + 1] = 0; }
    g_gen++;
}

void installer_mouse(int mx, int my, int left) { if (win_mouse(&g_win, mx, my, left)) g_gen++; }
void installer_rclick(int mx, int my) { (void)mx; (void)my; }

int installer_available(void) { return sysinfo_live_boot(); }

void installer_open(void) {
    if (!installer_available()) return;
    if (!g_open && !g_running) {
        g_page = P_WELCOME;
        g_status[0] = 0;
        look_for_disk();
        /* the keyboard layout this live session uses, as the default */
        const char* cur = keyboard_layout_name();
        for (int i = 0; i < NLAYOUTS; i++) if (cur && strcmp(cur, LAYOUTS[i]) == 0) g_layout = i;
    }
    g_open = 1;
    win_clamp(&g_win);
    g_gen++;
}

void installer_close(void) {
    if (g_running) { kstrlcpy(g_status, "Please wait until it is finished.", sizeof(g_status)); g_gen++; return; }
    g_open = 0;
    memset(g_pw, 0, sizeof(g_pw));
    g_focus = -1;
    g_win.dragging = g_win.resizing = 0;
    g_gen++;
}

int installer_is_open(void) { return g_open; }
int installer_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }

uint32_t installer_signature(void) {
    if (!g_open) return 0;
    /* the progress bar and the caret move by themselves */
    uint32_t t = timer_ms() / (g_running ? 200 : 500);
    if (t != g_tick && (g_running || (g_page == P_SETUP && g_focus >= 0))) { g_tick = t; g_gen++; }
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^ (uint32_t)g_page << 28;
}
