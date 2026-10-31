/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../net/firewall.h"
#include "i18n.h"
#include "theme.h"
#include "../net/ipv6.h"
#include "../net/wifi.h"
#include "settings.h"
#include "hostname.h"
#include "gfx.h"
#include "display.h"
#include "gpu.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "fs.h"
#include "winframe.h"
#include "config.h"
#include "audio.h"
#include "keyboard.h"
#include "wallpaper.h"
#include "wallpaper_gallery.h"
#include "wallpaper_data.h"
#include "sysinfo.h"
#include "rtc.h"
#include "../net/net.h"
#include "../net/netconf.h"
#include "../net/httpd.h"
#include "../net/sshd.h"
#include "../net/rdpd.h"
#include "passwd.h"
#include "fsdisk.h"
#include "login.h"
#include "utf8.h"
#include "usb.h"
#include "touchpad.h"
#include "font.h"
#include "fb.h"
#include "filechooser.h"
#include "kbnav.h"
#include "serial.h"
#include "gui.h"
#include "image.h"
#include "pkg.h"
#include "repo.h"
#include "app.h"
#include "task.h"
#include "tz.h"
#include "acpi_sleep.h"
#include "laptop.h"
#include "gpu.h"

void shell_power(int reboot);

#define TITLE_H  20
#define SIDE_W   132
#define BODY_Y   (TITLE_H + 8)
#define ITEM_H   26

#define C_PANEL  TC(0x1D232C)
#define C_TITLE  TC(0x384562)
#define C_SIDE   TC(0x161B22)
#define C_TEXT   TC(0xE8EEF6)
#define C_DIM    TC(0xAAB6C6)
#define C_HEAD   TC(0xFFFFFF)
#define C_ACCENT TC(0x3A7BD5)
#define C_SEL    TC(0x2C3E5C)

enum { PG_DISPLAY = 0, PG_THEMES, PG_FONTS, PG_SCREEN, PG_TASKBAR, PG_SOUND, PG_KEYBOARD, PG_LANGUAGE, PG_MOUSE, PG_NETWORK, PG_TIME, PG_STARTUP, PG_APPS, PG_ABOUT, PG_WIFI, PG_POWER, PG_COUNT };
static const char* const PAGE_NAMES[PG_COUNT] = { "Wallpaper", "Themes", "Fonts", "Screen", "Taskbar", "Sound", "Keyboard", "Language", "Mouse", "Network", "Date & time", "Startup", "Apps", "About", "Wi-Fi", "Power & USB-C" };

#define KB_ROW 20                           /* Keyboard: a row of the layout list (two columns) */

static int        g_open;
static kbnav_t    g_nav;              /* keyboard focus: what the page drew */
static win_geom_t g_win = { .x = 100, .y = 40, .w = 640, .h = 490, .min_w = 580, .min_h = 470 };
static int        g_page;
static uint32_t   g_gen;
static char       g_status[96];
static int g_sleep_available;
static char g_pc_name[HOSTNAME_MAX + 1];
static int g_pc_focus, g_pc_dirty;
static void pc_submit(void) {
    int rc = hostname_set(g_pc_name);
    if (rc == -1) kstrlcpy(g_status, "Use 1-63 letters, digits or hyphens; no hyphen at either end", sizeof(g_status));
    else if (rc) kstrlcpy(g_status, "Could not save the PC name", sizeof(g_status));
    else {
        kstrlcpy(g_pc_name, hostname_get(), sizeof(g_pc_name));
        kstrlcpy(g_status, fsdisk_is_installed() ? "PC name saved. New terminals use it." : "PC name changed for this live session.", sizeof(g_status));
        g_pc_focus = g_pc_dirty = 0;
    }
    g_gen++;
}
static int        g_muted, g_vol_before_mute = 80;
static int        g_dragging_vol, g_dragging_speed;
static uint32_t   g_tick_s;

static int sidebar_item_h(void) {
    int h=(g_win.h-BODY_Y-34)/PG_COUNT;
    return h<26?h:26;
}
static unsigned g_power_battery,g_power_port;
static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void button(int x, int y, int w, const char* label, int on) {
    kbnav_add(&g_nav, x, y, w, 20);
    uint32_t base = on ? C_ACCENT : TC(0x303740);
    bevel(x, y, w, 20, base, TC(0x535D6E), TC(0x15191F));
    int tw = (int)strlen(label) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 6, label, C_TEXT, base);
}

static void label(int x, int y, const char* s, uint32_t c) { gfx_draw_text(x, y, s, c, C_PANEL); }
static int  ipf_key(char c);

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

/* The gallery pages within the content area; preset IDs stay stable. */
#define THUMB_W WP_THUMB_W
#define THUMB_H WP_THUMB_H
#define THUMB_GAP WP_THUMB_GAP
static int g_wp_page;
static wp_gallery_t gallery(void) {
    wp_gallery_t l=wp_gallery_layout(cw(),g_win.y+g_win.h-28-cy0(),
                                    wallpaper_preset_count(),wallpaper_recent_count());
    if(g_wp_page>=l.pages)g_wp_page=l.pages-1;
    if(g_wp_page<0)g_wp_page=0;
    return l;
}
static int gallery_first(void) { wp_gallery_t l=gallery();return g_wp_page*l.capacity; }
static int gallery_end(void) {
    wp_gallery_t l=gallery();int end=(g_wp_page+1)*l.capacity;
    return end<wallpaper_preset_count()?end:wallpaper_preset_count();
}
static void thumb_rect(int slot,int* x,int* y) {
    wp_gallery_t l=gallery();int local=slot-g_wp_page*l.capacity;
    *x=cx0()+(local%l.per_row)*(THUMB_W+THUMB_GAP);
    *y=cy0()+38+(local/l.per_row)*WP_THUMB_ROW;
}
static int pager_y(void) { wp_gallery_t l=gallery();return cy0()+38+l.rows*WP_THUMB_ROW; }
static int pics_y(void) { wp_gallery_t l=gallery();return pager_y()+l.pager_h+4; }
static void pic_rect(int i,int* x,int* y) { *x=cx0()+i*(THUMB_W+THUMB_GAP);*y=pics_y()+18; }
static int wp_buttons_y(void) { return pics_y()+18+(wallpaper_recent_count()?THUMB_H:18)+24; }
static int wp_visible(int y,int h) { return y>=cy0()&&y+h<=g_win.y+g_win.h-28; }

/* a w x h picture at (x, y), clipped to the screen */
static void blit(int x, int y, int w, int h, const uint32_t* px) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t || !px) return;
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= th) continue;
        int x0 = x < 0 ? -x : 0, x1 = x + w > tw ? tw - x : w;
        if (x0 < x1) memcpy(t + (uint32_t)yy * (uint32_t)stride + x + x0, px + r * w + x0, (uint32_t)(x1 - x0) * 4u);
    }
}

static const char* const MODE_NAMES[4] = { "Fill", "Fit", "Stretch", "Center" };
static const image_mode_t MODES[4] = { IMAGE_FILL, IMAGE_FIT, IMAGE_STRETCH, IMAGE_CENTER };

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

static void draw_display(void) {
    int x = cx0(), y = cy0();
    char heading[64];ksnprintf(heading,sizeof(heading),"%s (%d)",_("Wallpaper"),wallpaper_preset_count());
    label(x,y,heading,C_HEAD);
    const char* file = wallpaper_current_file();
    char line[96];
    if (file && *file) {
        const char* b = strrchr(file, '/');
        ksnprintf(line, sizeof(line), "%s %s (%s)", _("Now: your picture"), b ? b + 1 : file, _(MODE_NAMES[wallpaper_current_mode() < 4 ? wallpaper_current_mode() : 0]));
        if ((int)strlen(line) * 8 > cw()) line[cw() / 8] = 0;
    }
    else {
        const wallpaper_preset_t* p = wallpaper_preset(wallpaper_current_preset());
        ksnprintf(line, sizeof(line), "%s %s", _("Now:"), p ? p->name : "-");
    }
    label(x, y + 18, line, C_DIM);
    int cur = wallpaper_current_preset();
    for(int slot=gallery_first();slot<gallery_end();slot++) {
        int i=wallpaper_gallery_preset(slot),tx,ty;
        thumb_rect(slot,&tx,&ty);
        if (ty + THUMB_H + 20 > g_win.y + g_win.h - 28) break;
        const wallpaper_preset_t* p = wallpaper_preset(i);
        gfx_fill_rect(tx - 2, ty - 2, THUMB_W + 4, THUMB_H + 4, i == cur ? C_ACCENT : TC(0x10141C));
        kbnav_add(&g_nav, tx, ty, THUMB_W, THUMB_H);
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
        char nm[11];
        kstrlcpy(nm, p ? p->name : "?", sizeof(nm));
        label(tx, ty + THUMB_H + 5, nm, i == cur ? C_HEAD : C_DIM);
    }

    wp_gallery_t layout=gallery();
    if(layout.pages>1&&wp_visible(pager_y(),20)){
        int py=pager_y();
        if(g_wp_page>0)button(x,py,80,_("Previous"),0);
        else label(x+8,py+6,_("Previous"),C_DIM);
        char page[40];ksnprintf(page,sizeof(page),"%d / %d",g_wp_page+1,layout.pages);
        label(x+cw()/2-20,py+6,page,C_DIM);
        if(g_wp_page+1<layout.pages)button(x+cw()-64,py,64,_("Next"),0);
        else label(x+cw()-56,py+6,_("Next"),C_DIM);
    }

    /* the user's pictures: those used lately */
    int py = pics_y();
    if(wp_visible(py,10))label(x,py,_("Your pictures"),C_HEAD);
    int nr=wallpaper_recent_count();
    if(!nr&&wp_visible(py+22,10))label(x,py+22,_("The pictures you choose show up here."),C_DIM);
    for (int i = 0; i < nr; i++) {
        int tx, ty;
        pic_rect(i, &tx, &ty);
        if(tx+THUMB_W>x+cw()||!wp_visible(ty,THUMB_H+18))break;
        int on = file && !strcmp(file, wallpaper_recent(i));
        gfx_fill_rect(tx - 2, ty - 2, THUMB_W + 4, THUMB_H + 4, on ? C_ACCENT : TC(0x10141C));
        kbnav_add(&g_nav, tx, ty + 14, THUMB_W, THUMB_H - 14);
        const uint32_t* th = wallpaper_recent_thumb(i, THUMB_W, THUMB_H);
        if (th) blit(tx, ty, THUMB_W, THUMB_H, th);
        else { gfx_fill_rect(tx, ty, THUMB_W, THUMB_H, TC(0x303740)); label(tx + 4, ty + 20, _("(missing)"), C_DIM); }
        /* remove it from the list: x in its corner */
        gfx_fill_rect(tx + THUMB_W - 12, ty, 12, 12, TC(0x202630));
        gfx_draw_text(tx + THUMB_W - 10, ty + 2, "x", C_TEXT, TC(0x202630));
        const char* b = strrchr(wallpaper_recent(i), '/');
        char nm[11];
        kstrlcpy(nm, b ? b + 1 : wallpaper_recent(i), sizeof(nm));
        label(tx, ty + THUMB_H + 5, nm, on ? C_HEAD : C_DIM);
    }

    /* Browse..., and how a picture covers the screen */
    int by=wp_buttons_y();
    if(!wp_visible(by,20))return;
    button(x, by, 96, _("Browse..."), 0);
    int pic = file && *file;
    by+=24;
    if(!wp_visible(by,20))return;
    label(x,by+6,_("Position:"),pic?C_TEXT:C_DIM);
    int step=(cw()-80)/4;
    for(int m=0;m<4;m++)
        button(x+80+m*step,by,64,_(MODE_NAMES[m]),pic&&wallpaper_current_mode()==MODES[m]);
}

/* ── Fonts: the text size, the installed fonts, which one the interface
 *    and the documents use ── */
#define FROW_H 34
static int g_font_top;                       /* the first row shown (the list scrolls) */
static int fonts_list_y(void) { return cy0() + 86; }
static int fonts_rows(void) { int r = (g_win.y + g_win.h - 64 - fonts_list_y()) / FROW_H; return r < 1 ? 1 : r; }
static int ui_col(void) { return cx0() + cw() - 190; }
static int doc_col(void) { return cx0() + cw() - 110; }

/* the rows: classic, DejaVu Sans Mono, DejaVu Sans, then the user's fonts */
static int font_rows_total(void) { return 3 + font_user_count(); }
static int row_face(int r) { return r == 0 ? -2 : r == 1 ? FONT_MONO : r == 2 ? FONT_SANS : font_user_face(r - 3); }

static void radio(int x, int y, int on) {
    kbnav_add(&g_nav, x, y, 14, 14);
    bevel(x, y, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    if (on) gfx_fill_rect(x + 4, y + 4, 6, 6, C_ACCENT);
}

static void draw_fonts(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Text size"), C_HEAD);
    label(x + 88, y, _("(terminal windows, Notepad, desktop icon names)"), C_DIM);
    for (int s = 0; s < GFX_TEXT_SIZES; s++) button(x + s * 92, y + 18, 86, gfx_text_size_name(s), gfx_text_size() == s);

    label(x, y + 56, _("Fonts"), C_HEAD);
    label(ui_col() - 20, y + 56, _("Interface"), C_DIM);
    label(doc_col() - 4, y + 56, _("Documents"), C_DIM);
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    int ly = fonts_list_y(), n = font_rows_total(), rows = fonts_rows();
    if (g_font_top > n - rows) g_font_top = n - rows;
    if (g_font_top < 0) g_font_top = 0;
    for (int i = 0; i < rows && g_font_top + i < n; i++) {
        int r = g_font_top + i, face = row_face(r);
        int ry = ly + i * FROW_H;
        gfx_fill_rect(x, ry, cw(), FROW_H - 2, (i & 1) ? TC(0x222932) : TC(0x1F252E));
        const char* name = r == 0 ? "Classic (8x8 pixels)" : r == 1 ? "DejaVu Sans Mono" : r == 2 ? "DejaVu Sans" : font_user_name(face);
        char nm[40];
        kstrlcpy(nm, name, sizeof(nm));
        if ((int)strlen(nm) > (ui_col() - x - 30) / 8) nm[(ui_col() - x - 30) / 8] = 0;
        gfx_draw_text(x + 6, ry + 4, nm, C_TEXT, (i & 1) ? TC(0x222932) : TC(0x1F252E));
        /* a sample in the font itself */
        if (face >= 0 && t) font_draw(t, stride, x, ry, ui_col() - 30, ry + FROW_H - 2, face, 15, x + 6, ry + 28,
                                      "The quick brown fox 0123", 24, TC(0xC8D4E4), 0);
        else if (face == -2) gfx_draw_text(x + 6, ry + 18, _("The quick brown fox 0123"), TC(0xC8D4E4), (i & 1) ? TC(0x222932) : TC(0x1F252E));
        int ui_on = face == -2 ? !gfx_smooth_text() : gfx_smooth_text() && font_ui() == face;
        if (face != FONT_SANS) radio(ui_col(), ry + 9, ui_on);
        if (face != -2 && face != FONT_MONO) radio(doc_col() + 20, ry + 9, face == FONT_SANS ? font_doc() < 0 : font_doc() == face);
        if (r >= 3) button(x + cw() - 30, ry + 6, 26, "X", 0);       /* uninstall */
    }
    if (n > rows) label(x + cw() - 120, ly + rows * FROW_H + 2, _("(wheel: more)"), C_DIM);
    int by = g_win.y + g_win.h - 50;
    button(x, by, 120, _("Install font..."), 0);
    label(x + 132, by + 6, _("a .ttf file (or right-click one in Files)"), C_DIM);
}

/* ── installing a font ── */
int settings_install_font(const char* path, char* msg, int cap) {
    const char* b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char dst[FS_PATH_LEN], err[96];
    ksnprintf(dst, sizeof(dst), "%s/%s", FONT_DIR, b);
    int copied = 0;
    if (strcmp(dst, path) != 0) {
        if (fs_find_file(dst) >= 0) {
            int have = font_user_find(dst);
            if (have >= 0) { ksnprintf(msg, (uint32_t)cap, "%s is installed already", font_user_name(have)); return -1; }
        }
        fs_mkdir_p(FONT_DIR);
        if (fs_copy(path, dst) < 0) { ksnprintf(msg, (uint32_t)cap, "could not copy it to %s", FONT_DIR); return -1; }
        copied = 1;
    }
    int face = font_user_load(dst, err, sizeof(err));
    if (face < 0) {
        if (copied) fs_delete(dst, 0);
        ksnprintf(msg, (uint32_t)cap, "%s: %s", b, err);
        return -1;
    }
    ksnprintf(msg, (uint32_t)cap, "Installed %s - choose where it is used in Settings > Fonts", font_user_name(face));
    g_gen++;
    return 0;
}

static int is_ttf(const char* n) {
    uint32_t l = (uint32_t)strlen(n);
    return l > 4 && (!strcmp(n + l - 4, ".ttf") || !strcmp(n + l - 4, ".TTF"));
}
static int is_picture(const char* n) { return wallpaper_is_image_name(n); }

static void font_chosen(const char* path) {
    settings_install_font(path, g_status, sizeof(g_status));
    g_gen++;
}

static void picture_chosen(const char* path) {
    char err[80];
    if (wallpaper_set_file(path, IMAGE_FILL, err, sizeof(err)) == 0) {
        const char* b = strrchr(path, '/');
        ksnprintf(g_status, sizeof(g_status), "Wallpaper: %s", b ? b + 1 : path);
    } else ksnprintf(g_status, sizeof(g_status), "Not a usable picture: %s", err);
    g_gen++;
}

static void apply_text_change(void) {
    gui_text_changed();
    g_gen++;
}

static void click_fonts(int mx, int my) {
    int x = cx0(), y = cy0();
    for (int s = 0; s < GFX_TEXT_SIZES; s++)
        if (inside(mx, my, x + s * 92, y + 18, 86, 20)) {
            gfx_set_text_size(s);
            char v[4];
            ksnprintf(v, sizeof(v), "%d", s);
            save_setting("text_size", v);
            ksnprintf(g_status, sizeof(g_status), "Text size: %s", gfx_text_size_name(s));
            apply_text_change();
            return;
        }
    int by = g_win.y + g_win.h - 50;
    if (inside(mx, my, x, by, 120, 20)) {
        fc_open(_("Choose a font to install (.ttf)"), "/home/banana/Downloads", is_ttf, font_chosen);
        return;
    }
    int ly = fonts_list_y(), n = font_rows_total(), rows = fonts_rows();
    for (int i = 0; i < rows && g_font_top + i < n; i++) {
        int r = g_font_top + i, face = row_face(r);
        int ry = ly + i * FROW_H;
        if (!inside(mx, my, x, ry, cw(), FROW_H)) continue;
        if (r >= 3 && inside(mx, my, x + cw() - 30, ry + 6, 26, 20)) {
            char path[FS_PATH_LEN], nm[48];
            kstrlcpy(path, font_user_path(face), sizeof(path));
            kstrlcpy(nm, font_user_name(face), sizeof(nm));
            int was_ui = font_ui() == face, was_doc = font_doc() == face;
            font_user_unload(face);
            fs_delete(path, 0);
            if (was_ui) save_setting("ui_face", "");
            if (was_doc) save_setting("doc_font", "");
            ksnprintf(g_status, sizeof(g_status), "Removed %s", nm);
            apply_text_change();
            return;
        }
        if (mx >= ui_col() - 6 && mx < ui_col() + 40 && face != FONT_SANS) {
            if (face == -2) {
                gfx_set_smooth_text(0);
                save_setting("ui_font", "classic");
            } else {
                gfx_set_smooth_text(1);
                font_set_ui(face);
                save_setting("ui_font", "smooth");
                save_setting("ui_face", face == FONT_MONO ? "" : font_user_path(face));
            }
            ksnprintf(g_status, sizeof(g_status), "Interface font: %s", face == -2 ? "classic" : face == FONT_MONO ? "DejaVu Sans Mono" : font_user_name(face));
            apply_text_change();
            return;
        }
        if (mx >= doc_col() && mx < doc_col() + 70 && face != -2 && face != FONT_MONO) {
            font_set_doc(face == FONT_SANS ? -1 : face);
            save_setting("doc_font", face == FONT_SANS ? "" : font_user_path(face));
            ksnprintf(g_status, sizeof(g_status), "Documents font: %s", face == FONT_SANS ? "DejaVu Sans" : font_user_name(face));
            apply_text_change();
            return;
        }
        return;
    }
}

static void apps_wheel(int dz);
void settings_wheel(int mx, int my, int dz) {
    (void)mx; (void)my;
    if (!g_open) return;
    if (fc_active()) { fc_wheel(dz); g_gen++; return; }
    if(g_page==PG_DISPLAY&&inside(mx,my,cx0(),cy0(),cw(),g_win.y+g_win.h-28-cy0())) {
        g_wp_page+=dz;gallery();g_gen++;
    }
    if (g_page == PG_FONTS) { g_font_top += dz; g_gen++; }
    if (g_page == PG_APPS) { apps_wheel(dz); g_gen++; }
}

static void draw_sound(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Sound"), C_HEAD);
    char line[96];
    ksnprintf(line, sizeof(line), "Device: %s", audio_device_name());
    label(x, y + 18, line, C_DIM);
    int vol = audio_get_volume();
    ksnprintf(line, sizeof(line), "Volume: %d%%%s", vol, g_muted ? " (muted)" : "");
    label(x, y + 44, line, C_TEXT);
    int sx, sy, sw;
    slider_rect(&sx, &sy, &sw);
    bevel(sx, sy + 6, sw, 8, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    gfx_fill_rect(sx + 1, sy + 7, (sw - 2) * vol / 100, 6, g_muted ? TC(0x606A78) : C_ACCENT);
    int kx = sx + (sw - 10) * vol / 100;
    bevel(kx, sy, 10, 20, TC(0xC8D2E0), TC(0xFFFFFF), TC(0x606A78));
    button(sx + sw + 12, sy, 40, _("Max"), 0);
    button(x, sy + 36, 100, g_muted ? "Unmute" : "Mute", g_muted);
    button(x + 112, sy + 36, 120, _("Test sound"), 0);
}

static void checkbox(int x, int y, int on);

/* Mouse: the pointer speed, 1..10 on the same kind of slider */
static void speed_rect(int* x, int* y, int* w) { *x = cx0(); *y = cy0() + 64; *w = cw() - 60; }

static void set_speed_at(int mx) {
    int x, y, w;
    speed_rect(&x, &y, &w);
    int v = 1 + ((mx - x) * 9 + (w > 0 ? w : 1) / 2) / (w > 0 ? w : 1);
    mouse_set_speed(v);
}

static void save_speed(void) {
    char v[8];
    ksnprintf(v, sizeof(v), "%d", mouse_get_speed());
    save_setting("mouse_speed", v);
}

static void draw_mouse(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Mouse"), C_HEAD);
    label(x, y + 18, _("How far the pointer moves when you move the mouse:"), C_DIM);
    int sp = mouse_get_speed();
    char line[64];
    ksnprintf(line, sizeof(line), "Pointer speed: %d%s", sp, sp == 5 ? " (normal)" : sp < 5 ? " (slower)" : " (faster)");
    label(x, y + 44, line, C_TEXT);
    int sx, sy, sw;
    speed_rect(&sx, &sy, &sw);
    bevel(sx, sy + 6, sw, 8, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    gfx_fill_rect(sx + 1, sy + 7, (sw - 2) * (sp - 1) / 9, 6, C_ACCENT);
    for (int i = 0; i < 10; i++) gfx_fill_rect(sx + 4 + (sw - 10) * i / 9, sy + 22, 2, 4, C_DIM);
    int kx = sx + (sw - 10) * (sp - 1) / 9;
    bevel(kx, sy, 10, 20, TC(0xC8D2E0), TC(0xFFFFFF), TC(0x606A78));
    label(sx, sy + 32, _("Slow"), C_DIM);
    label(sx + sw - 32, sy + 32, _("Fast"), C_DIM);
    button(x, sy + 56, 70, _("Slower"), 0);
    button(x + 80, sy + 56, 70, _("Faster"), 0);
    button(x + 160, sy + 56, 70, _("Normal"), sp == 5);

    label(x, sy + 100, _("Touchpad"), C_HEAD);
    checkbox(x, sy + 124, tp_tap_to_click);
    label(x + 24, sy + 127, _("Tap to click (two fingers: right click)"), C_TEXT);
    checkbox(x, sy + 150, tp_natural_scroll);
    label(x + 24, sy + 153, _("Natural scrolling (the page follows the fingers)"), C_TEXT);
    char tdesc[96];
    touchpad_describe(tdesc, sizeof(tdesc));
    label(x, sy + 182, tdesc + 3, C_DIM);       /* (without the " | ") */
}

static int kb_per_col(void) { return (keyboard_layout_count() + 1) / 2; }

/* Language: the desktop's (kernel/i18n.c) */
static void draw_language(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Language"), C_HEAD);
    label(x, y + 18, _("The language of the desktop - menus, windows, Settings:"), C_DIM);
    for (int i = 0; i < i18n_count(); i++) {
        int ry = y + 44 + i * 26, on = i == i18n_current();
        kbnav_add(&g_nav, x, ry - 2, 260, 20);
        bevel(x, ry, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
        if (on) gfx_fill_rect(x + 4, ry + 4, 6, 6, C_ACCENT);
        label(x + 24, ry + 3, i18n_name(i), on ? C_HEAD : C_TEXT);
    }
    int ny = y + 44 + i18n_count() * 26 + 14;
    label(x, ny, _("Apps follow it when they are translated; the others stay in English."), C_DIM);
    label(x, ny + 16, _("Commands in the terminal stay in English."), C_DIM);
}

static void click_language(int mx, int my) {
    int x = cx0(), y = cy0();
    for (int i = 0; i < i18n_count(); i++)
        if (inside(mx, my, x, y + 44 + i * 26 - 4, 300, 24)) {
            i18n_set(i18n_code(i));
            ksnprintf(g_status, sizeof(g_status), "%s: %s", _("Language"), i18n_name(i));
            return;
        }
}

static void draw_keyboard(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Keyboard layout"), C_HEAD);
    label(x, y + 18, _("Choose how the keys of your keyboard are read:"), C_DIM);
    const char* cur = keyboard_layout_name();
    int per = kb_per_col(), colw = cw() / 2;
    for (int i = 0; i < keyboard_layout_count(); i++) {
        int rx = x + (i / per) * colw, ry = y + 44 + (i % per) * KB_ROW;
        const char* what = NULL;
        const char* name = keyboard_layout_at(i, &what);
        int on = cur && strcmp(cur, name) == 0;
        kbnav_add(&g_nav, rx, ry - 2, colw - 8, 18);
        bevel(rx, ry, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
        if (on) gfx_fill_rect(rx + 4, ry + 4, 6, 6, C_ACCENT);
        char lb[34];
        kstrlcpy(lb, what, sizeof(lb));
        int maxc = (colw - 34) / 8;
        if (maxc > 0 && maxc < (int)sizeof(lb)) lb[maxc] = 0;
        label(rx + 22, ry + 3, lb, on ? C_HEAD : C_TEXT);
    }
    label(x, y + 50 + per * KB_ROW, _("AltGr: the right Alt key (or Ctrl+Alt)."), C_DIM);
    label(x, y + 66 + per * KB_ROW, _("Win+Space: English (US) and back to this layout."), C_DIM);
}

/* Screen, on any PC: the sizes the firmware offers, set by the boot loader
 * (from the next start on an installed system) */
#define BM_COL_W 150
static int  g_restart_pending;
static int bm_cols(void) { int c = cw() / BM_COL_W; return c < 1 ? 1 : c; }
static void bm_rect(int i, int* x, int* y) {     /* i = 0: automatic, then the modes */
    *x = cx0() + (i % bm_cols()) * BM_COL_W;
    *y = cy0() + 64 + (i / bm_cols()) * 22;
}

static void draw_screen_boot(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Screen resolution"), C_HEAD);
    const fb_info_t* fi = fb_info();
    char line[112], saved[24] = "";
    ksnprintf(line, sizeof(line), "Now: %u x %u", fi ? fi->width : 0, fi ? fi->height : 0);
    label(x, y + 18, line, C_DIM);
    display_mode_t modes[48];
    int n = display_boot_modes(modes, 48);
    if (!n) {
        label(x, y + 44, _("This display keeps the size it was started with: the boot loader"), C_DIM);
        label(x, y + 60, _("gave no list of sizes (started by another loader)."), C_DIM);
        return;
    }
    cfg_get(CFG_SETTINGS, "boot_resolution", saved, sizeof(saved));
    int installed = fsdisk_is_installed();
    label(x, y + 40, installed ? "Choose a size; it is used from the next start:"
                               : "The sizes this computer offers:", C_TEXT);
    for (int i = 0; i <= n; i++) {
        int bx, by;
        bm_rect(i, &bx, &by);
        if (by + 20 > g_win.y + g_win.h - 60) break;
        char nm[32], val[24];
        if (i == 0) { kstrlcpy(nm, "Automatic", sizeof(nm)); kstrlcpy(val, "auto", sizeof(val)); }
        else {
            ksnprintf(val, sizeof(val), "%dx%d", modes[i - 1].w, modes[i - 1].h);
            int now = fi && modes[i - 1].w == (int)fi->width && modes[i - 1].h == (int)fi->height;
            ksnprintf(nm, sizeof(nm), "%d x %d%s", modes[i - 1].w, modes[i - 1].h, now ? " *" : "");
        }
        int on = installed && (saved[0] ? !strcmp(saved, val) : i == 0);
        radio(bx, by, on);
        label(bx + 22, by + 3, nm, on ? C_HEAD : C_TEXT);
    }
    int ry = g_win.y + g_win.h - 54;
    label(x, ry - 16, _("* the size now"), C_DIM);
    if (g_restart_pending) button(x, ry, 130, _("Restart now"), 1);
    else if (!installed) label(x, ry + 4, _("Banana Boot menu: Left / Right changes the screen size."), C_DIM);
}

static void click_screen_boot(int mx, int my) {
    display_mode_t modes[48];
    int n = display_boot_modes(modes, 48);
    int ry = g_win.y + g_win.h - 54;
    if (g_restart_pending && inside(mx, my, cx0(), ry, 130, 20)) { shell_power(1); return; }
    for (int i = 0; i <= n; i++) {
        int bx, by;
        bm_rect(i, &bx, &by);
        if (!inside(mx, my, bx, by - 3, BM_COL_W - 4, 20)) continue;
        int w = i ? modes[i - 1].w : 0, h = i ? modes[i - 1].h : 0;
        kstrlcpy(g_status, "Saving the boot settings...", sizeof(g_status));
        int rc = display_set_boot_mode(w, h);
        if (rc == 0) {
            if (i) ksnprintf(g_status, sizeof(g_status), "%d x %d from the next start", w, h);
            else kstrlcpy(g_status, "Automatic size from the next start", sizeof(g_status));
            g_restart_pending = 1;
        } else if (rc == -2) {
            kstrlcpy(g_status, "Live CD: choose the size in the Banana Boot menu (Left / Right)", sizeof(g_status));
        } else {
            kstrlcpy(g_status, "Could not write the boot settings to the disk", sizeof(g_status));
        }
        return;
    }
}

/* ── Themes: the desktop's colours (kernel/theme.c) ── */
#define TCARD_W 132
#define TCARD_H 96
static int tcards_per_row(void) { int n = (cw() + 10) / (TCARD_W + 10); return n < 1 ? 1 : n; }
static void tcard_rect(int i, int* x, int* y) {
    int per = tcards_per_row();
    *x = cx0() + (i % per) * (TCARD_W + 10);
    *y = cy0() + 36 + (i / per) * (TCARD_H + 10);
}

/* a small window in the theme's own colours (not through TC: what it will look like) */
static void theme_preview(int x, int y, int w, int h, const theme_t* t) {
    const uint32_t* s = t->shade;
    gfx_fill_rect(x, y, w, h, s[TH_SHADOW]);
    gfx_fill_rect(x + 1, y + 1, w - 2, h - 2, s[TH_SURFACE]);
    gfx_fill_rect(x + 1, y + 1, w - 2, 12, s[TH_RAISED]);                 /* the title bar */
    gfx_fill_rect(x + 5, y + 5, 30, 3, s[TH_TEXT]);
    gfx_fill_rect(x + w - 10, y + 4, 6, 6, TC(0xC04040));
    gfx_fill_rect(x + 5, y + 17, w / 3, h - 30, s[TH_BASE]);             /* a list */
    for (int k = 0; k < 3; k++) gfx_fill_rect(x + 8, y + 21 + k * 7, w / 3 - 10, 2, k ? s[TH_DIM] : s[TH_TEXT]);
    gfx_fill_rect(x + w / 3 + 10, y + 19, w - w / 3 - 16, 2, s[TH_TEXT]);
    gfx_fill_rect(x + w / 3 + 10, y + 26, w - w / 3 - 30, 2, s[TH_DIM]);
    gfx_fill_rect(x + w / 3 + 10, y + h - 24, 34, 10, t->accent);         /* a button */
    gfx_fill_rect(x + 1, y + h - 9, w - 2, 8, s[TH_BASE]);                /* the taskbar */
    gfx_fill_rect(x + 4, y + h - 7, 4, 4, t->accent);
    gfx_fill_rect(x + 1, y + h - 10, w - 2, 1, s[TH_LINE]);
}

static const char* const TM_SHORT[TM_STYLES] = { "Banana", "Classic", "Win 10", "Win 11" };

static void draw_themes(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Themes"), C_HEAD);
    label(x + 64, y, _("(the colours of windows, the taskbar and menus)"), C_DIM);
    int n = theme_count(), cur = theme_current();
    for (int i = 0; i < n; i++) {
        int tx, ty;
        tcard_rect(i, &tx, &ty);
        if (ty + TCARD_H > g_win.y + g_win.h - 60) break;
        const theme_t* t = theme_get(i);
        kbnav_add(&g_nav, tx, ty, TCARD_W, TCARD_H);
        if (i == cur) gfx_fill_rect(tx - 2, ty - 2, TCARD_W + 4, TCARD_H - 14, C_ACCENT);
        theme_preview(tx, ty, TCARD_W, TCARD_H - 18, t);
        char nm[20];
        kstrlcpy(nm, t->name, sizeof(nm));
        label(tx + (TCARD_W - (int)strlen(nm) * 8) / 2, ty + TCARD_H - 12, nm, i == cur ? C_HEAD : C_TEXT);
    }
    /* Task Manager's look: the theme's, or one chosen */
    int ty = g_win.y + g_win.h - 80, ov = theme_taskmgr_override();
    label(x, ty + 6, _("Task Manager:"), C_TEXT);
    char auto_lbl[32];
    ksnprintf(auto_lbl, sizeof(auto_lbl), "Theme's: %s", _(TM_SHORT[theme_get(cur) ? theme_get(cur)->taskmgr : 0]));
    button(x + 112, ty, 8 + (int)strlen(auto_lbl) * 8, auto_lbl, ov < 0);
    int bx2 = x + 112 + 16 + (int)strlen(auto_lbl) * 8;
    for (int s = 0; s < TM_STYLES; s++) {
        static const char* const SHORT[TM_STYLES] = { "Banana", "Classic", "Win 10", "Win 11" };
        int bw = (int)strlen(_(SHORT[s])) * 8 + 12;
        button(bx2, ty, bw, _(SHORT[s]), ov == s);
        bx2 += bw + 6;
    }
    int by = g_win.y + g_win.h - 50;
    button(x, by, 128, _("Install theme..."), 0);
    button(x + 136, by, 128, _("Save a copy"), 0);
    label(x + 272, by + 6, _("make your own: sdk/themes"), C_DIM);
}

static int is_theme_file(const char* n) {
    int l = (int)strlen(n);
    return (l > 6 && !strcasecmp(n + l - 6, ".theme")) || (l > 7 && !strcasecmp(n + l - 7, ".btheme"));
}

static void theme_chosen(const char* path) {
    char err[96];
    int i = theme_install(path, err, sizeof(err));
    if (i < 0) { ksnprintf(g_status, sizeof(g_status), "Theme: %s", err); g_gen++; return; }
    theme_apply(i);
    ksnprintf(g_status, sizeof(g_status), "Installed and applied: %s", theme_get(i)->name);
    g_gen++;
}

static void click_themes(int mx, int my) {
    int n = theme_count();
    for (int i = 0; i < n; i++) {
        int tx, ty;
        tcard_rect(i, &tx, &ty);
        if (inside(mx, my, tx, ty, TCARD_W, TCARD_H)) {
            theme_apply(i);
            ksnprintf(g_status, sizeof(g_status), "Theme: %s", theme_get(i)->name);
            return;
        }
    }
    int x = cx0(), by = g_win.y + g_win.h - 50, ty = g_win.y + g_win.h - 80;
    if (my >= ty && my < ty + 20) {
        char auto_lbl[32];
        const theme_t* ct = theme_get(theme_current());
        ksnprintf(auto_lbl, sizeof(auto_lbl), "Theme's: %s", _(TM_SHORT[ct ? ct->taskmgr : 0]));
        int ax = x + 112, aw = 8 + (int)strlen(auto_lbl) * 8;
        if (mx >= ax && mx < ax + aw) { theme_set_taskmgr_override(-1); ksnprintf(g_status, sizeof(g_status), "Task Manager: the theme's look"); return; }
        int bx2 = ax + aw + 8;
        for (int s = 0; s < TM_STYLES; s++) {
            static const char* const SHORT[TM_STYLES] = { "Banana", "Classic", "Win 10", "Win 11" };
            int bw = (int)strlen(_(SHORT[s])) * 8 + 12;
            if (mx >= bx2 && mx < bx2 + bw) { theme_set_taskmgr_override(s); ksnprintf(g_status, sizeof(g_status), "Task Manager: %s", _(TM_STYLE_NAMES[s])); return; }
            bx2 += bw + 6;
        }
        return;
    }
    if (inside(mx, my, x, by, 128, 20)) {
        fc_open(_("Choose a theme to install (.theme, .btheme)"), "/home/banana/Downloads", is_theme_file, theme_chosen);
        return;
    }
    if (inside(mx, my, x + 136, by, 128, 20)) {
        char path[96];
        const theme_t* t = theme_get(theme_current());
        ksnprintf(path, sizeof(path), "/home/banana/Documents/%s.theme", t ? t->name : "my");
        for (char* p = path + 23; *p; p++) if (*p == ' ') *p = '-';
        fs_mkdir_p("/home/banana/Documents");
        if (theme_export(theme_current(), NULL, path) == 0) ksnprintf(g_status, sizeof(g_status), "Saved %s - edit it, then Install", path);
        else ksnprintf(g_status, sizeof(g_status), "Could not write %s", path);
    }
}

/* Taskbar: how the windows' buttons look */
static const char* const TB_STYLES[3] = { "Icons and titles", "Icons only", "Titles only" };
static void draw_taskbar_page(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Taskbar buttons"), C_HEAD);
    label(x, y + 18, _("How the open windows show on the taskbar:"), C_DIM);
    int cur = gui_taskbar_style();
    for (int i = 0; i < 3; i++) {
        int ry = y + 44 + i * ITEM_H;
        kbnav_add(&g_nav, x, ry - 2, 260, 18);
        bevel(x, ry, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
        if (i == cur) gfx_fill_rect(x + 4, ry + 4, 6, 6, C_ACCENT);
        label(x + 24, ry + 3, _(TB_STYLES[i]), i == cur ? C_HEAD : C_TEXT);
        /* a small preview */
        int px = x + 200, py = ry - 2;
        gfx_fill_rect(px, py, 110, 18, TC(0x192026));
        if (i != 2) gfx_fill_rect(px + (i == 1 ? 10 : 4), py + 4, 10, 10, TC(0xD0A030));
        if (i != 1) gfx_fill_rect(px + (i == 2 ? 6 : 18), py + 8, 70, 2, TC(0xE8EEF6));
    }
    /* the virtual desktops' buttons */
    int dy = y + 44 + 3 * ITEM_H + 16, on = gui_taskbar_desks();
    kbnav_add(&g_nav, x, dy - 2, 300, 18);
    bevel(x, dy, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    if (on) { gfx_fill_rect(x + 3, dy + 6, 3, 3, C_ACCENT); gfx_fill_rect(x + 5, dy + 8, 3, 3, C_ACCENT); gfx_fill_rect(x + 7, dy + 6, 2, 2, C_ACCENT); gfx_fill_rect(x + 9, dy + 4, 2, 2, C_ACCENT); }
    label(x + 24, dy + 3, _("Show the virtual desktop buttons (1 2 3 4)"), on ? C_HEAD : C_TEXT);
    label(x + 24, dy + 19, _("Hidden, Win+Ctrl+Left / Right still switch desktops"), C_DIM);
    int py = dy + 46;
    label(x, py, _("Pinned apps"), C_HEAD);
    label(x, py + 20, _("Right-click a window's taskbar button or an"), C_DIM);
    label(x, py + 36, _("app's desktop icon: Pin to taskbar."), C_DIM);
    label(x, py + 52, _("Right-click a pinned app to unpin it."), C_DIM);
}

/* Screen: the resolution */
static void draw_screen(void) {
    if (!display_can_change()) { draw_screen_boot(); return; }
    int x = cx0(), y = cy0();
    label(x, y, _("Screen resolution"), C_HEAD);
    const fb_info_t* fi = fb_info();
    char line[96];
    ksnprintf(line, sizeof(line), "Now: %u x %u", fi ? fi->width : 0, fi ? fi->height : 0);
    label(x, y + 18, line, C_DIM);
    display_mode_t modes[16];
    int n = display_modes(modes, 16);
    for (int i = 0; i < n; i++) {
        int ry = y + 44 + i * ITEM_H;
        int on = fi && modes[i].w == (int)fi->width && modes[i].h == (int)fi->height;
        kbnav_add(&g_nav, x, ry - 2, 200, 18);
        char nm[32];
        ksnprintf(nm, sizeof(nm), "%d x %d", modes[i].w, modes[i].h);
        bevel(x, ry, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
        if (on) gfx_fill_rect(x + 4, ry + 4, 6, 6, C_ACCENT);
        label(x + 24, ry + 3, nm, on ? C_HEAD : C_TEXT);
    }
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
    kbnav_add(&g_nav, x, y, 14, 14);
    bevel(x, y, 14, 14, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    if (on) { gfx_fill_rect(x + 3, y + 3, 8, 8, C_ACCENT); }
}

#define SU_ROWS 5
static void su_rows(int y, int* ry) { for (int i = 0; i < SU_ROWS; i++) ry[i] = y + 44 + i * SU_ROW_H; }
static int  su_pw_y(int y) { return y + 44 + SU_ROWS * SU_ROW_H + 14; }

static void draw_startup(void) {
    int x = cx0(), y = cy0(), w = cw();
    label(x, y, _("Startup"), C_HEAD);
    label(x, y + 18, _("Started by themselves when Banana OS boots:"), C_DIM);
    int ry[SU_ROWS];
    su_rows(y, ry);
    char line[96];

    /* auto-login: the login screen's switch, the other way round */
    checkbox(x, ry[4], !login_required());
    label(x + 24, ry[4] + 3, passwd_is_set(PASSWD_USER) ? "Log in automatically (no password asked at boot)"
                                                        : "Log in automatically (always: no password is set)", C_TEXT);

    checkbox(x, ry[0], rc_on("desktop"));
    label(x + 24, ry[0] + 3, _("Desktop (start the GUI instead of the text console)"), C_TEXT);

    checkbox(x, ry[1], rc_on("httpd"));
    ksnprintf(line, sizeof(line), "Web server (httpd), port %u", rc_port("httpd", 80));
    label(x + 24, ry[1] + 3, line, C_TEXT);
    label(x + w - 170, ry[1] + 3, httpd_running() ? "running" : "stopped", httpd_running() ? TC(0x80E080) : C_DIM);
    button(x + w - 90, ry[1] - 3, 86, httpd_running() ? "Stop" : "Start now", 0);

    checkbox(x, ry[2], rc_on("sshd"));
    ksnprintf(line, sizeof(line), "SSH server (sshd), port %u", rc_port("sshd", 22));
    label(x + 24, ry[2] + 3, line, C_TEXT);
    label(x + w - 170, ry[2] + 3, sshd_running() ? "running" : "stopped", sshd_running() ? TC(0x80E080) : C_DIM);
    button(x + w - 90, ry[2] - 3, 86, sshd_running() ? "Stop" : "Start now", 0);

    checkbox(x, ry[3], rc_on("rdpd"));
    ksnprintf(line, sizeof(line), "Remote Desktop (rdpd), port %u", rc_port("rdpd", 3389));
    label(x + 24, ry[3] + 3, line, C_TEXT);
    label(x + w - 170, ry[3] + 3, rdpd_running() ? "running" : "stopped", rdpd_running() ? TC(0x80E080) : C_DIM);
    button(x + w - 90, ry[3] - 3, 86, rdpd_running() ? "Stop" : "Start now", 0);

    /* the password */
    int py = su_pw_y(y);
    label(x, py, _("Password (for SSH and RDP logins)"), C_HEAD);
    int set = passwd_is_set(PASSWD_USER);
    label(x, py + 18, set ? "A password is set." : "No password yet: the SSH server will not start without one.",
          set ? C_DIM : TC(0xFFC060));
    static const char* const NAMES[2] = { "New:", "Again:" };
    for (int f = 0; f < 2; f++) {
        int fy = py + 42 + f * 26;
        label(x, fy + 4, _(NAMES[f]), C_TEXT);
        bevel(x + 60, fy, 200, 20, TC(0x141920), TC(0x10141C), g_pw_focus == f ? C_ACCENT : TC(0x404B5C));
        int n = u8_cols(g_pw[f], (int)strlen(g_pw[f]));
        char stars[40];
        int k = 0;
        for (; k < n && k < 24; k++) stars[k] = '*';
        stars[k] = 0;
        gfx_draw_text(x + 66, fy + 6, stars, C_TEXT, TC(0x141920));
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
    int ry[SU_ROWS];
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
    if (inside(mx, my, x + w - 90, ry[3] - 3, 86, 20)) {
        if (rdpd_running()) { rdpd_stop(); kstrlcpy(g_status, "Remote Desktop server stopped", sizeof(g_status)); }
        else {
            kstrlcpy(g_status, "Starting the Remote Desktop server (first start makes a key)...", sizeof(g_status));
            if (rdpd_start(rc_port("rdpd", 3389), err, sizeof(err)) == 0) kstrlcpy(g_status, "Remote Desktop server started", sizeof(g_status));
            else ksnprintf(g_status, sizeof(g_status), "rdpd: %s", err);
        }
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
    if (inside(mx, my, x, ry[3] - 4, w - 180, 24)) {
        int on = !rc_on("rdpd");
        rc_set("rdpd", on, rc_port("rdpd", 3389));
        kstrlcpy(g_status, !on ? "The Remote Desktop server no longer starts at boot"
                 : passwd_is_set(PASSWD_USER) ? "The Remote Desktop server starts at boot"
                 : "The Remote Desktop server starts at boot (set a password below)", sizeof(g_status));
        return;
    }
    if (inside(mx, my, x, ry[4] - 4, w - 100, 24)) {
        if (!passwd_is_set(PASSWD_USER)) {
            kstrlcpy(g_status, "No password is set: Banana OS always logs in by itself", sizeof(g_status));
            return;
        }
        int ask = !login_enabled();
        login_set_enabled(ask);
        kstrlcpy(g_status, ask ? "The password is asked at boot" : "Logs in automatically at boot (the lock screen still asks)",
                 sizeof(g_status));
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
static void draw_power(void) {
    int x=cx0(),y=cy0();char line[160];
    laptop_info_t p;laptop_get_info(&p);
    label(x,y,_("Power & USB-C"),C_HEAD);
    label(x,y+24,(p.runtime || p.battery_count || p.ac_present || p.brightness>=0 || p.lid>=0)?_("Firmware power services ready"):_("Firmware power information unavailable"),C_DIM);
    ksnprintf(line,sizeof(line),"AC power: %s   Lid: %s",!p.ac_present?"not reported":p.ac_online?"connected":"disconnected",
              p.lid<0?"not reported":p.lid?"open":"closed");
    label(x,y+44,line,C_TEXT);
    label(x,y+72,_("Battery"),C_HEAD);
    if(p.battery_count>1) button(x+cw()-110,y+68,110,_("Next battery"),0);
    if(!p.battery_count) label(x,y+96,_("No battery reported by the firmware."),C_DIM);
    else {
        laptop_battery_t* b=&p.batteries[g_power_battery%(unsigned)p.battery_count];
        if(!b->present) label(x,y+96,_("Battery absent"),C_DIM);
        else if(!b->valid) label(x,y+96,_("Battery telemetry unavailable"),C_DIM);
        else {
            ksnprintf(line,sizeof(line),"%d%% - %s%s",b->percent,b->state&2?"Charging":b->state&1?"On battery":"Idle",b->state&4?" (critical)":"");
            label(x,y+96,line,C_TEXT);
            ksnprintf(line,sizeof(line),"%s  |  %u / %u %s",b->model,b->remaining,b->full,b->unit?"mAh":"mWh");
            label(x,y+116,line,C_DIM);
            if(b->minutes>=0) {
                ksnprintf(line,sizeof(line),"Estimated time remaining: %d h %d min",b->minutes/60,b->minutes%60);
                label(x,y+136,line,C_DIM);
            }
        }
    }
    int current=gpu_backlight(-1);
    ksnprintf(line,sizeof(line),current>=0?"Screen brightness: %d%%":"Screen brightness: unavailable",current);
    label(x,y+172,line,C_HEAD);
    if(current>=0) {
        button(x,y+198,80,_("Dimmer"),0);button(x+90,y+198,80,_("Brighter"),0);
        if(p.brightness_pending) label(x+184,y+202,_("Applying..."),C_DIM);
    }
    label(x,y+246,_("USB-C Power Delivery"),C_HEAD);
    ucsi_t* u=&p.usbc;
    if(!u->ready) { label(x,y+272,_("No compatible USB-C firmware interface."),C_DIM);return; }
    if(u->num_ports>1) button(x+cw()-90,y+242,90,_("Next port"),0);
    unsigned port=g_power_port%u->num_ports;ucsi_port_t* c=&u->ports[port];
    if(!c->valid || u->failed) ksnprintf(line,sizeof(line),"Port %u: status unavailable",port+1);
    else if(!c->connected) ksnprintf(line,sizeof(line),"Port %u: disconnected",port+1);
    else ksnprintf(line,sizeof(line),"Port %u: %s",port+1,c->source?"Supplying power":"Receiving power");
    label(x,y+272,line,C_TEXT);
    if(c->valid && c->connected && !u->failed) {
        label(x,y+294,c->mode==3?_("USB Power Delivery active"):c->mode==4?_("Type-C current: 1.5 A"):c->mode==5?_("Type-C current: 3 A"):_("USB / Battery Charging power"),C_TEXT);
        if(c->contract_valid) {
            if(c->millivolts) ksnprintf(line,sizeof(line),"Contract: %u.%02u W (%u mV, %u mA)",c->milliwatts/1000,c->milliwatts%1000/10,c->millivolts,c->milliamps);
            else ksnprintf(line,sizeof(line),"Contract: %u mW",c->milliwatts);
            label(x,y+316,line,C_TEXT);
        } else if(c->mode==3) label(x,y+316,_("Contract details not reported"),C_DIM);
        label(x,y+338,c->charging==1?_("Battery charging normally"):c->charging==2?_("Slow charging"):c->charging==3?_("Trickle charging"):_("Battery not charging"),C_DIM);
    }
    if(u->last_error) {
        const char* reason=u->last_error==UCSI_ERROR_FIRMWARE?ucsi_ppm_error_text(u->ppm_error):ucsi_error_text(u->last_error);
        ksnprintf(line,sizeof(line),"Last USB-C error: %s",reason);
        label(x,y+364,line,C_DIM);
    } else label(x,y+364,_("Power negotiation is managed by the controller."),C_DIM);
}

/* Wi-Fi uses the same station core as the CLI; passwords are masked,
 * held only while editing and wiped on submit/page change/close. */
static int g_wifi_index,g_wifi_page,g_wifi_ap=-1,g_wifi_focus;
static char g_wifi_password[65];
static wifi_radio_t* g_wifi_last;
static wifi_ap_t g_wifi_selected;
static void wifi_clear_password(void) {
    volatile char* p=g_wifi_password;
    for(unsigned i=0;i<sizeof(g_wifi_password);i++) p[i]=0;
    g_wifi_focus=0;
}
static wifi_radio_t* settings_use_radio(wifi_radio_t* r) {
    if(r!=g_wifi_last) {
        wifi_clear_password();g_wifi_ap=-1;g_wifi_page=0;g_wifi_last=r;
    }
    return r;
}
static wifi_radio_t* settings_radio(void) {
    int at=0;
    for(int i=0;i<net_device_count();i++) {
        wifi_radio_t* r=wifi_from_device(net_device_at(i));
        if(r && at++==g_wifi_index) return settings_use_radio(r);
    }
    g_wifi_index=0;
    for(int i=0;i<net_device_count();i++) {
        wifi_radio_t* r=wifi_from_device(net_device_at(i));if(r) return settings_use_radio(r);
    }
    return settings_use_radio(NULL);
}
static void wifi_display_name(const wifi_ap_t* ap,char name[33]) {
    for(unsigned i=0;i<ap->ssid_len;i++) name[i]=ap->ssid[i]>=32 && ap->ssid[i]<127?(char)ap->ssid[i]:'?';
    name[ap->ssid_len]=0;
}
static void settings_wifi_connect(void) {
    wifi_radio_t* r=settings_radio();
    if(!r || g_wifi_ap<0 || g_wifi_ap>=r->nap) {
        kstrlcpy(g_status,"Select a scanned network first",sizeof(g_status));return;
    }
    wifi_ap_t ap=r->aps[g_wifi_ap];
    if(memcmp(ap.bssid,g_wifi_selected.bssid,6) || ap.ssid_len!=g_wifi_selected.ssid_len ||
       memcmp(ap.ssid,g_wifi_selected.ssid,ap.ssid_len)) {
        wifi_clear_password();g_wifi_ap=-1;
        kstrlcpy(g_status,"Network changed; select it again",sizeof(g_status));return;
    }
    int rc=wifi_connect(r,ap.ssid,ap.ssid_len,g_wifi_password,!ap.privacy);
    wifi_clear_password();
    kstrlcpy(g_status,rc?"Cannot join: disconnect first, check password/security, or scan again":
             "Joining the selected network...",sizeof(g_status));
}
static int settings_wifi_key(char c) {
    if(g_page!=PG_WIFI || !g_wifi_focus) return 0;
    unsigned n=(unsigned)strlen(g_wifi_password);
    if(c=='\n') settings_wifi_connect();
    else if(c==27) wifi_clear_password();
    else if(c=='\t') { g_wifi_focus=0;g_nav.shown=1; }
    else if(c=='\b') { if(n) g_wifi_password[n-1]=0; }
    else if((unsigned char)c>=32 && (unsigned char)c<127 && n<64) {
        g_wifi_password[n]=c;g_wifi_password[n+1]=0;
    }
    g_gen++;return 1;
}
static void draw_wifi(void) {
    int x=cx0(),y=cy0();char line[96];
    label(x,y,_("Wi-Fi"),C_HEAD);
    wifi_radio_t* r=settings_radio();
    if(!r) {
        label(x,y+30,_("No supported Wi-Fi radio detected."),C_TEXT);
        label(x,y+58,"Initial driver: MediaTek MT7601U (experimental)",C_DIM);
        label(x,y+82,"Other requested radio drivers are still pending.",C_DIM);
        label(x,y+110,"Use lsusb and dmesg to inspect a plugged-in card.",C_DIM);
        return;
    }
    ksnprintf(line,sizeof(line),"%s: %s",r->nd.ifname,wifi_state_name(r));label(x,y+26,line,C_TEXT);
    button(x,y+50,90,_("Scan"),0);
    button(x+96,y+50,110,_("Next radio"),0);
    button(x+212,y+50,110,_("Disconnect"),0);
    label(x,y+84,_("Network"),C_DIM);label(x+226,y+84,_("Security"),C_DIM);
    button(x+cw()-92,y+78,42,"<",0);button(x+cw()-46,y+78,42,">",0);
    int maxpage=r->nap?(r->nap-1)/7:0;if(g_wifi_page>maxpage) g_wifi_page=maxpage;
    for(int row=0;row<7;row++) {
        int index=g_wifi_page*7+row;if(index>=r->nap) break;
        wifi_ap_t* ap=&r->aps[index];int yy=y+108+row*24;
        uint32_t bg=index==g_wifi_ap?C_SEL:((row&1)?TC(0x242D3A):TC(0x1F252E));
        gfx_fill_rect(x,yy,cw()-4,22,bg);kbnav_add(&g_nav,x,yy,cw()-4,22);
        char name[33];wifi_display_name(ap,name);name[26]=0;
        gfx_draw_text(x+5,yy+7,name,C_TEXT,bg);
        gfx_draw_text(x+226,yy+7,ap->wpa2?"WPA2":ap->privacy?"Unsupported":"Open",C_DIM,bg);
        ksnprintf(line,sizeof(line),"%d dBm",ap->rssi);gfx_draw_text(x+cw()-76,yy+7,line,C_DIM,bg);
    }
    label(x,y+300,_("Password:"),C_TEXT);
    kbnav_add(&g_nav,x+100,y+296,300,20);
    bevel(x+100,y+296,300,20,TC(0x141920),TC(0x10141C),g_wifi_focus?C_ACCENT:TC(0x404B5C));
    char stars[37];unsigned n=(unsigned)strlen(g_wifi_password);if(n>36) n=36;
    memset(stars,'*',n);stars[n]=0;gfx_draw_text(x+106,y+302,stars,C_TEXT,TC(0x141920));
    if(g_wifi_focus && (timer_ms()/500)%2==0) gfx_fill_rect(x+106+n*8,y+300,2,12,C_TEXT);
    int open=g_wifi_ap>=0 && g_wifi_ap<r->nap && !r->aps[g_wifi_ap].privacy;
    button(x,y+330,180,open?_("Connect (open)"):_("Connect (WPA2)"),0);
    label(x,y+364,"2.4 GHz channels 1-11; WPA2-CCMP or explicit open.",C_DIM);
    label(x,y+386,"Connection priority: Settings > Network.",C_DIM);
}
static void click_wifi(int mx,int my) {
    int x=cx0(),y=cy0();wifi_radio_t* r=settings_radio();if(!r) return;
    g_wifi_focus=0;
    if(inside(mx,my,x,y+50,90,20)) {
        wifi_clear_password();g_wifi_ap=-1;g_wifi_page=0;
        kstrlcpy(g_status,wifi_scan(r)?"Disconnect before scanning, or wait for the current operation":"Scanning nearby networks...",sizeof(g_status));
    } else if(inside(mx,my,x+96,y+50,110,20)) {
        wifi_clear_password();g_wifi_ap=-1;g_wifi_page=0;g_wifi_index++;
        (void)settings_radio();
    } else if(inside(mx,my,x+212,y+50,110,20)) {
        wifi_clear_password();wifi_disconnect(r);
    } else if(inside(mx,my,x+cw()-92,y+78,42,20)) {
        if(g_wifi_page>0) g_wifi_page--;
    } else if(inside(mx,my,x+cw()-46,y+78,42,20)) {
        if((g_wifi_page+1)*7<r->nap) g_wifi_page++;
    } else if(inside(mx,my,x+100,y+296,300,20)) g_wifi_focus=1;
    else if(inside(mx,my,x,y+330,180,20)) settings_wifi_connect();
    else for(int row=0;row<7;row++) {
        int index=g_wifi_page*7+row;
        if(index<r->nap && inside(mx,my,x,y+108+row*24,cw()-4,22)) {
            wifi_clear_password();g_wifi_ap=index;g_wifi_selected=r->aps[index];g_wifi_focus=r->aps[index].privacy;break;
        }
    }
}

void settings_key(char c) {
    if (settings_wifi_key(c)) return;
    if (!g_open) return;
    if (fc_active()) { fc_key(c); g_gen++; return; }
    if (g_page == PG_ABOUT && g_pc_focus) {
        int n = (int)strlen(g_pc_name);
        if (c == '\n') pc_submit();
        else if (c == 27) { kstrlcpy(g_pc_name, hostname_get(), sizeof(g_pc_name)); g_pc_focus = g_pc_dirty = 0; }
        else if (c == '\t') { g_pc_focus = 0; g_nav.shown = 1; g_nav.focus = PG_COUNT + (g_sleep_available ? 2 : 1); }
        else if (c == '\b') { if (n) { g_pc_name[n - 1] = 0; g_pc_dirty = 1; } }
        else if ((unsigned char)c >= 32 && (unsigned char)c < 127 && n < HOSTNAME_MAX) { g_pc_name[n] = c; g_pc_name[n + 1] = 0; g_pc_dirty = 1; }
        g_gen++;
        return;
    }
    if (ipf_key(c)) return;                          /* the manual address fields (Network) */
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

/* Network: the card, how it gets its address - automatic (DHCP), or a
 * manual one typed here (IP address, subnet mask, gateway, DNS server: saved
 * in /etc/network.conf as `ifconfig` does) - the firewall and IPv6 */
static int  g_net_manual = -1;                   /* the form shown: -1 as the card is set up, 0 auto, 1 manual */
static int  g_ipf_focus = -1;                    /* the field being typed in */
static char g_ipf[4][16];                        /* IP address, subnet mask, gateway, DNS server */
static const char* const IPF_NAME[4] = { "IP address:", "Subnet mask:", "Gateway:", "DNS server:" };
#define NET_FX   140                             /* where the fields begin */
#define NET_LOW  (g_net_manual == 1 ? 250 : 220) /* the firewall's row */

/* the fields, filled from what the card has now */
static void ipf_fill(netif_t* nf) {
    ip4_t v[4] = { nf->ip, nf->netmask ? nf->netmask : IP4(255, 255, 255, 0), nf->gateway, nf->dns };
    for (int i = 0; i < 4; i++) {
        if (v[i] && (nf->configured || i == 1)) ip4_to_str(v[i], g_ipf[i]);
        else g_ipf[i][0] = 0;
    }
}

static void draw_network(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Network"), C_HEAD);
    label(x, y + 28, _("Priority:"), C_TEXT);
    int priority = netconf_priority();
    button(x + 110, y + 24, 80, _("Auto"), priority == NET_PRIORITY_AUTO);
    button(x + 196, y + 24, 110, _("Ethernet"), priority == NET_PRIORITY_ETHERNET);
    button(x + 312, y + 24, 100, _("Wi-Fi"), priority == NET_PRIORITY_WIFI);
    y += 32;
    netif_t* nf = net_if();
    char a[16], line[96];
    if (!nf || !nf->dev) {
        label(x, y + 24, _("No network card was found."), C_DIM);
        return;
    }
    if (g_net_manual < 0) { g_net_manual = !nf->dhcp; ipf_fill(nf); }
    ksnprintf(line, sizeof(line), "Card: %s (%s)", nf->dev->ifname, nf->dev->model ? nf->dev->model : nf->dev->name);
    label(x, y + 24, line, C_TEXT);
    /* A static address or an unexpired lease says nothing about the cable.
     * Drivers without a carrier callback cannot establish physical link. */
    int link = nf->dev->link_up ? nf->dev->link_up(nf->dev) != 0 : -1;
    ksnprintf(line, sizeof(line), "Link: %s; IP: %s; cards: %d",
              link < 0 ? "unknown" : link ? "up" : "down",
              nf->configured ? "configured" : "not configured", net_device_count());
    label(x, y + 42, line, link == 1 && nf->configured ? TC(0x80E080) : TC(0xE0A060));
    if (nf->dhcp) ksnprintf(line, sizeof(line), "Mode: DHCP (%s)", dhcp_state_str());
    else kstrlcpy(line, "Mode: manual (static address)", sizeof(line));
    label(x, y + 56, line, C_DIM);
    /* the address: automatic or manual */
    label(x, y + 70, _("Get an address:"), C_TEXT);
    button(x + NET_FX, y + 66, 150, _("Automatic (DHCP)"), g_net_manual == 0);
    button(x + NET_FX + 156, y + 66, 90, _("Manual"), g_net_manual == 1);
    if (g_net_manual == 0) {
        ip4_to_str(nf->ip, a);     ksnprintf(line, sizeof(line), "IP address:   %s", nf->configured ? a : "-"); label(x, y + 100, line, C_TEXT);
        ip4_to_str(nf->netmask, a); ksnprintf(line, sizeof(line), "Subnet mask:  %s", nf->configured ? a : "-"); label(x, y + 116, line, C_TEXT);
        ip4_to_str(nf->gateway, a); ksnprintf(line, sizeof(line), "Gateway:      %s", nf->configured ? a : "-"); label(x, y + 132, line, C_TEXT);
        ip4_to_str(nf->dns, a);     ksnprintf(line, sizeof(line), "DNS server:   %s", nf->configured ? a : "-"); label(x, y + 148, line, C_TEXT);
        button(x, y + 176, 200, nf->dhcp ? "Renew address (DHCP)" : "Use automatic (DHCP)", 0);
    } else {
        for (int f = 0; f < 4; f++) {
            int fy = y + 98 + f * 26;
            label(x, fy + 4, _(IPF_NAME[f]), C_TEXT);
            kbnav_add(&g_nav, x + NET_FX, fy, 160, 20);
            bevel(x + NET_FX, fy, 160, 20, TC(0x141920), TC(0x10141C), g_ipf_focus == f ? C_ACCENT : TC(0x404B5C));
            gfx_draw_text(x + NET_FX + 6, fy + 6, g_ipf[f], C_TEXT, TC(0x141920));
            if (g_ipf_focus == f && (timer_ms() / 500) % 2 == 0)
                gfx_fill_rect(x + NET_FX + 6 + (int)strlen(g_ipf[f]) * 8, fy + 4, 2, 12, C_TEXT);
        }
        label(x + NET_FX + 170, y + 128, _("(gateway and DNS"), C_DIM);
        label(x + NET_FX + 170, y + 144, _(" may stay empty)"), C_DIM);
        button(x + NET_FX, y + 204, 100, _("Apply"), 1);
    }
    /* the firewall and IPv6: both off unless turned on here */
    int ly = y + NET_LOW;
    label(x, ly + 4, _("Firewall: refuses connections from outside"), C_TEXT);
    button(x + 360, ly, 64, fw_enabled() ? "On" : "Off", fw_enabled());
    label(x, ly + 26, _("(ports to open: `firewall allow tcp PORT`)"), C_DIM);
    label(x, ly + 54, _("IPv6"), C_TEXT);
    button(x + 360, ly + 50, 64, ipv6_enabled() ? "On" : "Off", ipv6_enabled());
}

/* Apply: the manual address, at once and for the next boots */
static void ipf_apply(void) {
    netif_t* nf = net_if();
    ip4_t v[4] = { 0, 0, 0, 0 };
    for (int f = 0; f < 4; f++) {
        if (!g_ipf[f][0]) {
            if (f < 2) { ksnprintf(g_status, sizeof(g_status), "Type the %s", f ? "subnet mask" : "IP address"); g_ipf_focus = f; return; }
            continue;
        }
        if (!str_to_ip4(g_ipf[f], &v[f])) {
            ksnprintf(g_status, sizeof(g_status), "\"%s\" is not an address like 192.168.1.20", g_ipf[f]);
            g_ipf_focus = f;
            return;
        }
    }
    if (!nf || !nf->dev) return;
    net_set_static(v[0], v[1], v[2], v[3]);
    int disk = netconf_save_static(nf->dev, v[0], v[1], v[2], v[3]);
    g_ipf_focus = -1;
    kstrlcpy(g_status, disk ? "Address set, and kept for the next boots" : "Address set (kept until the computer restarts)", sizeof(g_status));
}

/* typing in the address fields: digits and dots, Tab / Enter to the next, Enter on the last applies */
static int ipf_key(char c) {
    if (g_page != PG_NETWORK || g_net_manual != 1 || g_ipf_focus < 0) return 0;
    char* s = g_ipf[g_ipf_focus];
    int n = (int)strlen(s);
    if (c == '\t') g_ipf_focus = (g_ipf_focus + 1) % 4;
    else if (c == '\n') { if (g_ipf_focus < 3) g_ipf_focus++; else ipf_apply(); }
    else if (c == 27) g_ipf_focus = -1;
    else if (c == '\b') { if (n) s[n - 1] = 0; }
    else if (((c >= '0' && c <= '9') || c == '.') && n < 15) { s[n] = c; s[n + 1] = 0; }
    g_gen++;
    return 1;
}

/* Date & time: the clock set by hand (- / + on each part), what the
 * computer's clock keeps, and the time zone (a list that scrolls) */
static const char* const MON[12] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };
#define TZ_ROW 18
static int g_tz_top = -1;                        /* the first zone shown (-1: the current one's page) */

static void step_rect(int row, int col, int* bx, int* by) { *bx = cx0() + 52 + col * 132; *by = cy0() + 96 + row * 26; }
static int  tz_list_y(void) { return cy0() + 200; }
static int  tz_rows(void) { int n = (g_win.y + g_win.h - 34 - tz_list_y()) / TZ_ROW; return n < 1 ? 1 : n; }

static void draw_time(void) {
    int x = cx0(), y = cy0();
    label(x, y, _("Date & time"), C_HEAD);
    rtc_datetime_t t;
    char line[128];
    uint32_t utc = rtc_now_utc();
    int ok = rtc_read_datetime(&t) == 0 && t.month >= 1 && t.month <= 12;
    if (ok) {
        static const char* const WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        ksnprintf(line, sizeof(line), "%02u:%02u:%02u", t.hour, t.minute, t.second);
        gfx_draw_text_scaled(x, y + 22, 3, line, C_HEAD, C_PANEL);
        ksnprintf(line, sizeof(line), "%s %u %s %u", _(WD[(rtc_now() / 86400u + 4u) % 7u]), t.day, _(MON[t.month - 1]), t.year);
        label(x, y + 52, line, C_TEXT);
    } else {
        label(x, y + 28, _("The clock could not be read."), C_DIM);
    }
    const tz_zone_t* z = tz_get(tz_current());
    char off[16];
    tz_format_offset(tz_offset_at(tz_current(), utc), off, sizeof(off));
    ksnprintf(line, sizeof(line), "%s - %s%s", z ? z->label : "UTC", off, tz_is_summer(tz_current(), utc) ? ", summer time" : "");
    label(x, y + 70, line, C_DIM);

    /* set by hand: each part - and + */
    if (ok) {
        char v[5][8];
        ksnprintf(v[0], 8, "%u", t.day);
        kstrlcpy(v[1], _(MON[t.month - 1]), 4);
        ksnprintf(v[2], 8, "%u", t.year);
        ksnprintf(v[3], 8, "%02u h", t.hour);
        ksnprintf(v[4], 8, "%02u min", t.minute);
        label(x, y + 102, _("Date"), C_TEXT);
        label(x, y + 128, _("Time"), C_TEXT);
        for (int k = 0; k < 5; k++) {
            int bx, by;
            step_rect(k / 3, k % 3, &bx, &by);
            button(bx, by, 22, "-", 0);
            label(bx + 30, by + 6, v[k], C_HEAD);
            button(bx + 92, by, 22, "+", 0);
        }
    }
    label(x, y + 158, _("The computer's clock keeps"), C_TEXT);
    button(x + 224, y + 154, 100, _("Local time"), !rtc_keeps_utc());
    button(x + 330, y + 154, 60, _("UTC"), rtc_keeps_utc());

    /* the time zone */
    label(x, y + 182, _("Time zone"), C_HEAD);
    int rows = tz_rows(), n = tz_count();
    if (g_tz_top < 0) g_tz_top = tz_current() - rows / 2;
    if (g_tz_top > n - rows) g_tz_top = n - rows;
    if (g_tz_top < 0) g_tz_top = 0;
    button(x + cw() - 52, y + 178, 24, "^", 0);
    button(x + cw() - 24, y + 178, 24, "v", 0);
    for (int r = 0; r < rows && g_tz_top + r < n; r++) {
        int i = g_tz_top + r, ry = tz_list_y() + r * TZ_ROW;
        const tz_zone_t* zz = tz_get(i);
        int on = i == tz_current();
        radio(x, ry, on);
        tz_format_offset(tz_offset_at(i, utc), off, sizeof(off));
        label(x + 22, ry + 3, off, on ? C_HEAD : C_DIM);
        kstrlcpy(line, zz->label, sizeof(line));
        int room = (cw() - 112) / 8;
        if (room > 0 && (int)strlen(line) > room) line[room] = 0;
        label(x + 112, ry + 3, line, on ? C_HEAD : C_TEXT);
    }
}

/* a part of the local time - or +: 0 day, 1 month, 2 year, 3 hour, 4 minute */
static void time_step(int part, int d) {
    uint32_t now = rtc_now();
    if (!now) return;
    rtc_datetime_t t;
    rtc_split_time(now, &t);
    int y = t.year, m = t.month, day = t.day;
    uint32_t nt = now;
    if (part == 0) nt = now + (uint32_t)(d * 86400);
    else if (part == 3) nt = now + (uint32_t)(d * 3600);
    else if (part == 4) nt = now + (uint32_t)(d * 60);
    else {
        if (part == 1) { m += d; if (m < 1) { m = 12; y--; } if (m > 12) { m = 1; y++; } }
        else y += d;
        static const uint8_t D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int md = D[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
        if (day > md) day = md;
        nt = rtc_make_time(y, m, day, t.hour, t.minute, t.second);
    }
    if (rtc_set_time(nt) != 0) { kstrlcpy(g_status, "The clock holds 2000-2099 only", sizeof(g_status)); return; }
    char s[40];
    rtc_format(nt, s, sizeof(s));
    ksnprintf(g_status, sizeof(g_status), "Clock set: %s", s);
}

static void click_time(int mx, int my) {
    int x = cx0(), y = cy0();
    for (int k = 0; k < 5; k++) {
        int bx, by;
        step_rect(k / 3, k % 3, &bx, &by);
        if (inside(mx, my, bx, by, 22, 20)) { time_step(k, -1); return; }
        if (inside(mx, my, bx + 92, by, 22, 20)) { time_step(k, 1); return; }
    }
    if (inside(mx, my, x + 224, y + 154, 100, 20) || inside(mx, my, x + 330, y + 154, 60, 20)) {
        int utc = mx >= x + 330;
        if (utc != rtc_keeps_utc()) rtc_set_keeps_utc(utc);
        kstrlcpy(g_status, utc ? "The computer's clock keeps UTC (as Linux does)"
                               : "The computer's clock keeps the local time (as Windows does)", sizeof(g_status));
        return;
    }
    int rows = tz_rows();
    if (inside(mx, my, x + cw() - 52, y + 178, 24, 20)) { g_tz_top -= rows - 1; if (g_tz_top < 0) g_tz_top = 0; return; }
    if (inside(mx, my, x + cw() - 24, y + 178, 24, 20)) { g_tz_top += rows - 1; return; }   /* (kept in range when drawn) */
    for (int r = 0; r < rows && g_tz_top + r < tz_count(); r++) {
        int ry = tz_list_y() + r * TZ_ROW;
        if (inside(mx, my, x, ry - 2, cw(), TZ_ROW)) {
            int i = g_tz_top + r;
            tz_set(i);
            ksnprintf(g_status, sizeof(g_status), "Time zone: %s", tz_get(i)->label);
            return;
        }
    }
}

static void draw_about(void) {
    int x = cx0(), y = cy0();
    gfx_draw_text_scaled(x, y, 2, _("Banana OS 0.6"), C_HEAD, C_PANEL);
    if(g_sleep_available)button(x+cw()-90,y+28,90,_("Sleep"),0);
    const sysinfo_t* si = sysinfo_get();
    char line[112];
    label(x, y + 32, _("PC name"), C_HEAD);
    int fw = cw() - 100;
    kbnav_add(&g_nav, x, y + 52, fw, 22);
    bevel(x, y + 52, fw, 22, TC(0x141920), TC(0x10141C), g_pc_focus ? C_ACCENT : TC(0x404B5C));
    const char* value = g_pc_focus || g_pc_dirty ? g_pc_name : hostname_get();
    int n = (int)strlen(value), cols = (fw - 16) / 8;
    int first = n > cols ? n - cols : 0;
    gfx_draw_text(x + 6, y + 59, value + first, C_TEXT, TC(0x141920));
    if (g_pc_focus && (timer_ms() / 500) % 2 == 0) gfx_fill_rect(x + 6 + (n - first) * 8, y + 57, 2, 12, C_TEXT);
    button(x + fw + 10, y + 53, 90, _("Apply"), 1);
    label(x, y + 82, _("Letters, digits or hyphens. New terminals use the name."), C_DIM);
    int ly = y + 112;
#ifdef __x86_64__
    label(x, ly, _("64-bit (x86_64)"), C_TEXT);
#else
    label(x, ly, _("32-bit (i686)"), C_TEXT);
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
    label(x, ly, res, C_TEXT); ly += 18;
    gpu_t* gp = gpu_active();
    if (gp) {
        ksnprintf(line, sizeof(line), "Graphics: %s (%s)", gp->name, strcmp(gp->driver, "firmware") ? gp->driver : "firmware screen");
        if ((int)strlen(line) * 8 > cw()) line[cw() / 8] = 0;
        label(x, ly, line, C_TEXT);
    }
    ly += 26;
    if (sysinfo_live_boot())
        label(x, ly, fsdisk_find_install(NULL)
              ? "Running: the live CD (Banana OS is also installed here: 'update' updates it)"
              : "Running: the live CD - changes are lost at shutdown ('install' to keep them)", TC(0xFFB060));
    else if (fsdisk_is_installed()) {
        label(x, ly, _("Running: the installed system (changes are saved automatically)"), TC(0x80E080));
        uint64_t used, cap;
        fsdisk_space(&used, &cap);
        char d[96];
        ksnprintf(d, sizeof(d), "Saved files: %u MB of %u MB", (uint32_t)((used + 1048575u) >> 20), (uint32_t)(cap >> 20));
        label(x, ly + 18, d, C_TEXT);
    }
    else
        label(x, ly, _("Running: not installed - changes are lost at shutdown"), TC(0xFFB060));
}

/* ── Apps: the installed apps, with Open, Stop, Update and Uninstall ─ */

#define APPS_MAX   64
#define APP_ROW_H  34
#define APP_INFO_H 104                           /* the selected app's details, under the list */

static pkg_info_t g_al[APPS_MAX];
static int        g_nal, g_al_top, g_al_confirm;
static char       g_al_newer[APPS_MAX][24];      /* a newer version the sources have, or "" */
static char       g_al_sel[PKG_NAME_MAX];        /* the selected app, by name (survives rescans) */
static uint32_t   g_al_scanned;
static char       g_al_job[PKG_NAME_MAX];        /* the app being updated (by the worker task) */
static volatile int g_al_busy;
static int        g_al_worker = -1;

/* what an installed app takes: its folder's files, counted down to 4 levels */
static void dir_usage(const char* path, int depth, uint32_t* files, uint32_t* bytes) {
    static int fi[256];
    int n = fs_list_files(path, fi, 256);
    for (int i = 0; i < n && i < 256; i++) {
        fs_file_t* f = fs_file_info(fi[i]);
        if (f) { (*files)++; *bytes += f->size; }
    }
    if (depth >= 4) return;
    int di[32];
    int nd = fs_list_dirs(path, di, 32);
    for (int i = 0; i < nd && i < 32; i++) {
        const fs_dir_t* d = fs_get_dir(di[i]);
        char p[FS_PATH_LEN];
        if (!d) continue;
        ksnprintf(p, sizeof(p), "%s/%s", path, d->name);
        dir_usage(p, depth + 1, files, bytes);
    }
}

static void apps_scan(void) {
    g_al_scanned = timer_ms() | 1;
    if (g_al_busy) return;                       /* (the package lists are being rewritten) */
    int n = pkg_list(g_al, APPS_MAX);
    g_nal = 0;
    for (int i = 0; i < n && i < APPS_MAX; i++)
        if (!banana_system_component(g_al[i].name)) g_al[g_nal++] = g_al[i];
    for (int i = 0; i < g_nal; i++) {
        char p[FS_PATH_LEN];
        ksnprintf(p, sizeof(p), "%s/%s", PKG_DIR, g_al[i].name);
        g_al[i].files = g_al[i].bytes = 0;
        dir_usage(p, 0, &g_al[i].files, &g_al[i].bytes);
        const repo_pkg_t* c = repo_candidate(g_al[i].name);
        g_al_newer[i][0] = 0;
        if (c && repo_vercmp(c->version, g_al[i].version) > 0) kstrlcpy(g_al_newer[i], c->version, sizeof(g_al_newer[i]));
    }
    g_gen++;
}

static int apps_sel(void) {
    for (int i = 0; i < g_nal; i++) if (!strcmp(g_al[i].name, g_al_sel)) return i;
    return -1;
}

/* the running copies of an app (pkg_run names them after it) */
static int apps_running(const char* name, int* ids, int max) {
    static app_info_t a[APP_MAX];
    int n = app_snapshot(a, APP_MAX), k = 0;
    for (int i = 0; i < n && i < APP_MAX; i++)
        if (!strcmp(a[i].name, name)) { if (ids && k < max) ids[k] = a[i].id; k++; }
    return k;
}

static void fmt_size(uint32_t b, char* out, int cap) {
    if (b < 1024) ksnprintf(out, (size_t)cap, "%u B", b);
    else if (b < 1024u * 1024) ksnprintf(out, (size_t)cap, "%u KB", (b + 1023) / 1024);
    else ksnprintf(out, (size_t)cap, "%u.%u MB", b >> 20, ((b & 0xFFFFF) * 10) >> 20);
}

static int al_list_y(void) { return cy0() + 40; }
static int al_info_y(void) { return g_win.y + g_win.h - 26 - APP_INFO_H; }
static int al_rows(void) { int r = (al_info_y() - 8 - al_list_y()) / APP_ROW_H; return r < 1 ? 1 : r; }

static void al_clamp(void) {
    int max = g_nal - al_rows();
    if (g_al_top > max) g_al_top = max;
    if (g_al_top < 0) g_al_top = 0;
}
static void apps_wheel(int dz) { g_al_top += dz; al_clamp(); }

/* the worker: updates run here, the window keeps drawing meanwhile */
static void apps_worker(void) {
    for (;;) {
        if (!g_al_job[0]) { task_sleep_ms(100); continue; }
        char m[96], st[96];
        if (repo_install(g_al_job, 0, NULL, NULL, m, sizeof(m)) == 0) ksnprintf(st, sizeof(st), "%s updated", g_al_job);
        else ksnprintf(st, sizeof(st), "%s: %s", g_al_job, m);
        kstrlcpy(g_status, st, sizeof(g_status));
        g_al_job[0] = 0;
        g_al_busy = 0;
        g_al_scanned = 0;                        /* rescan, with the new version */
        g_gen++;
    }
}

static void draw_apps(void) {
    int x = cx0(), y = cy0(), w = cw();
    if (!g_al_scanned) apps_scan();
    al_clamp();
    label(x, y, _("Apps"), C_HEAD);
    uint32_t total = 0;
    int updates = 0;
    for (int i = 0; i < g_nal; i++) { total += g_al[i].bytes; if (g_al_newer[i][0]) updates++; }
    char line[128], sz[24];
    fmt_size(total, sz, sizeof(sz));
    if (updates) ksnprintf(line, sizeof(line), "%d installed, %s - %d update%s", g_nal, sz, updates, updates == 1 ? "" : "s");
    else ksnprintf(line, sizeof(line), "%d installed, %s", g_nal, sz);
    label(x, y + 18, g_nal ? line : "No apps are installed: get some in the App Store.", C_DIM);
    button(x + w - 110, y + 10, 110, _("App Store"), 0);

    int ly = al_list_y(), rows = al_rows(), sel = apps_sel();
    bevel(x, ly - 2, w, rows * APP_ROW_H + 4, TC(0x141920), TC(0x10141C), TC(0x404B5C));
    for (int r = 0; r < rows && g_al_top + r < g_nal; r++) {
        int i = g_al_top + r, ry = ly + r * APP_ROW_H;
        const pkg_info_t* p = &g_al[i];
        uint32_t bg = i == sel ? C_SEL : TC(0x141920);
        gfx_fill_rect(x + 1, ry, w - 2, APP_ROW_H - 1, bg);
        kbnav_add(&g_nav, x + 1, ry, w - 2, APP_ROW_H - 1);
        if (i == sel) gfx_fill_rect(x + 1, ry, 3, APP_ROW_H - 1, C_ACCENT);
        gfx_draw_text(x + 10, ry + 5, p->title[0] ? p->title : p->name, i == sel ? C_HEAD : C_TEXT, bg);
        ksnprintf(line, sizeof(line), "%s %s%s%s%s", p->name, p->version, p->category[0] ? " - " : "", p->category,
                  repo_is_auto(p->name) ? " - needed by another app" : "");
        int maxc = (w - 150) / 8;
        if (maxc > 0 && (int)strlen(line) > maxc) line[maxc] = 0;
        gfx_draw_text(x + 10, ry + 19, line, C_DIM, bg);
        fmt_size(p->bytes, sz, sizeof(sz));
        gfx_draw_text(x + w - 10 - (int)strlen(sz) * 8, ry + 5, sz, C_DIM, bg);
        const char* tag = g_al_busy && !strcmp(g_al_job, p->name) ? "updating..."
                        : apps_running(p->name, NULL, 0) ? "running" : g_al_newer[i][0] ? "update" : "";
        uint32_t tc = !strcmp(tag, "running") ? TC(0x80E080) : TC(0xFFC060);
        if (tag[0]) gfx_draw_text(x + w - 10 - (int)strlen(tag) * 8, ry + 19, tag, tc, bg);
    }
    if (g_nal > rows) {                          /* where the list is scrolled to */
        int th = rows * APP_ROW_H, kh = th * rows / g_nal, ky = ly + (th - kh) * g_al_top / (g_nal - rows);
        gfx_fill_rect(x + w - 4, ky, 3, kh < 8 ? 8 : kh, TC(0x505D72));
    }

    /* the selected app */
    int iy = al_info_y();
    if (sel < 0) { if (g_nal) label(x, iy + 8, _("Select an app to open, update or uninstall it."), C_DIM); return; }
    const pkg_info_t* p = &g_al[sel];
    ksnprintf(line, sizeof(line), "%s %s", p->title[0] ? p->title : p->name, p->version);
    label(x, iy, line, C_HEAD);
    kstrlcpy(line, p->description[0] ? p->description : "No description.", sizeof(line));
    if ((int)strlen(line) * 8 > w) line[w / 8] = 0;
    label(x, iy + 18, line, C_TEXT);
    fmt_size(p->bytes, sz, sizeof(sz));
    ksnprintf(line, sizeof(line), "%s app, %u files, %s%s%s", !strcmp(p->type, "gui") ? "Desktop" : "Terminal", p->files, sz,
              p->author[0] ? " - by " : "", p->author);
    if ((int)strlen(line) * 8 > w) line[w / 8] = 0;
    label(x, iy + 34, line, C_DIM);
    int by = iy + 58, bx = x, running = apps_running(p->name, NULL, 0);
    button(bx, by, 80, _("Open"), 0); bx += 90;
    if (running) { button(bx, by, 80, _("Stop"), 0); bx += 90; }
    if (g_al_newer[sel][0] && !g_al_busy) {
        ksnprintf(line, sizeof(line), "Update to %s", g_al_newer[sel]);
        int bw = (int)strlen(line) * 8 + 20;
        button(bx, by, bw, line, 1);
    }
    button(x + w - 170, by, 170, g_al_confirm ? "Really uninstall?" : "Uninstall", g_al_confirm);
}

static void click_apps(int mx, int my) {
    int x = cx0(), y = cy0(), w = cw();
    int confirm = g_al_confirm;
    g_al_confirm = 0;
    char err[96];
    if (inside(mx, my, x + w - 110, y + 10, 110, 20)) {
        if (pkg_run("store", 0, NULL, 1, err, sizeof(err)) < 0) ksnprintf(g_status, sizeof(g_status), "App Store: %s", err);
        else kstrlcpy(g_status, "Opening the App Store...", sizeof(g_status));
        return;
    }
    int ly = al_list_y(), rows = al_rows();
    if (inside(mx, my, x, ly, w, rows * APP_ROW_H)) {
        int i = g_al_top + (my - ly) / APP_ROW_H;
        if (i < g_nal) kstrlcpy(g_al_sel, g_al[i].name, sizeof(g_al_sel));
        return;
    }
    int sel = apps_sel();
    if (sel < 0) return;
    const pkg_info_t* p = &g_al[sel];
    char title[48];
    kstrlcpy(title, p->title[0] ? p->title : p->name, sizeof(title));
    int by = al_info_y() + 58, bx = x;
    int ids[8], running = apps_running(p->name, ids, 8);
    if (inside(mx, my, bx, by, 80, 20)) {
        if (pkg_run(p->name, 0, NULL, 1, err, sizeof(err)) < 0) ksnprintf(g_status, sizeof(g_status), "%s: %s", title, err);
        else ksnprintf(g_status, sizeof(g_status), "Opening %s...", title);
        return;
    }
    bx += 90;
    if (running) {
        if (inside(mx, my, bx, by, 80, 20)) {
            for (int k = 0; k < running && k < 8; k++) app_kill(ids[k]);
            ksnprintf(g_status, sizeof(g_status), "%s stopped", title);
            return;
        }
        bx += 90;
    }
    if (g_al_newer[sel][0] && !g_al_busy) {
        char l[48];
        ksnprintf(l, sizeof(l), "Update to %s", g_al_newer[sel]);
        if (inside(mx, my, bx, by, (int)strlen(l) * 8 + 20, 20)) {
            if (g_al_worker < 0) g_al_worker = task_create_stack("apps-update", apps_worker, 256u << 10);
            if (g_al_worker < 0) { kstrlcpy(g_status, "Could not start the update (too many tasks)", sizeof(g_status)); return; }
            ksnprintf(g_status, sizeof(g_status), "Updating %s to %s...", title, g_al_newer[sel]);
            g_al_busy = 1;
            kstrlcpy(g_al_job, p->name, sizeof(g_al_job));
            return;
        }
    }
    if (inside(mx, my, x + w - 170, by, 170, 20)) {
        if (g_al_busy) { kstrlcpy(g_status, "Wait for the update to finish", sizeof(g_status)); return; }
        if (!confirm) { g_al_confirm = 1; ksnprintf(g_status, sizeof(g_status), "Click again to uninstall %s", title); return; }
        for (int k = 0; k < running && k < 8; k++) app_kill(ids[k]);
        char m[96], name[PKG_NAME_MAX];
        kstrlcpy(name, p->name, sizeof(name));
        if (repo_remove(name, m, sizeof(m)) == 0) {
            ksnprintf(g_status, sizeof(g_status), "%s uninstalled%s", title,
                      fsdisk_is_installed() ? "" : " (until reboot: live session)");
            g_al_sel[0] = 0;
        } else kstrlcpy(g_status, m, sizeof(g_status));
        apps_scan();
    }
}

/* Settings > Apps with this app selected (the launcher's Details) */
void settings_show_app(const char* name) {
    settings_open_page(PG_APPS);
    kstrlcpy(g_al_sel, name ? name : "", sizeof(g_al_sel));
    apps_scan();
    int i = apps_sel();
    if (i >= 0 && (i < g_al_top || i >= g_al_top + al_rows())) g_al_top = i - al_rows() / 2;
}

/* ── window ───────────────────────────────────────────────────────── */

void settings_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    kbnav_begin(&g_nav);
    bevel(x, y, W, H, C_PANEL, TC(0x505D72), TC(0x10141C));
    bevel(x + 3, y + 3, W - 6, TITLE_H - 1, C_TITLE, TC(0x647692), TC(0x111923));
    gfx_draw_text(x + 10, y + 7, _("Settings"), TC(0xFFFFFF), C_TITLE);
    win_draw_buttons(&g_win, 4, 12);

    /* the pages, on the left */
    gfx_fill_rect(x + 6, y + BODY_Y, SIDE_W, H - BODY_Y - 26, C_SIDE);
    for (int i = 0; i < PG_COUNT; i++) {
        int row=sidebar_item_h();
        int iy = y + BODY_Y + 4 + i * row;
        uint32_t bg = i == g_page ? C_SEL : C_SIDE;
        gfx_fill_rect(x + 8, iy, SIDE_W - 4, row - 4, bg);
        kbnav_add(&g_nav, x + 8, iy, SIDE_W - 4, row - 4);
        if (i == g_page) gfx_fill_rect(x + 8, iy, 3, row - 4, C_ACCENT);
        gfx_draw_text(x + 18, iy + 7, _(PAGE_NAMES[i]), i == g_page ? C_HEAD : C_DIM, bg);
    }

    if (fc_active()) {
        fc_draw(cx0(), cy0(), cw(), g_win.y + g_win.h - 26 - cy0());
        if (g_status[0]) gfx_draw_text(x + 10, y + H - 18, g_status, C_DIM, C_PANEL);
        gfx_draw_grip(x + W, y + H);
        return;
    }
    switch (g_page) {
    case PG_DISPLAY:  draw_display(); break;
    case PG_THEMES:   draw_themes(); break;
    case PG_FONTS:    draw_fonts(); break;
    case PG_SCREEN:   draw_screen(); break;
    case PG_TASKBAR:  draw_taskbar_page(); break;
    case PG_SOUND:    draw_sound(); break;
    case PG_KEYBOARD: draw_keyboard(); break;
    case PG_LANGUAGE: draw_language(); break;
    case PG_MOUSE:    draw_mouse(); break;
    case PG_NETWORK:  draw_network(); break;
    case PG_WIFI: draw_wifi(); break;
    case PG_POWER: draw_power(); break;
    case PG_TIME:     draw_time(); break;
    case PG_STARTUP:  draw_startup(); break;
    case PG_APPS:     draw_apps(); break;
    default:         draw_about(); break;
    }
    if (g_status[0]) gfx_draw_text(x + 10, y + H - 18, g_status, C_DIM, C_PANEL);
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, TC(0xFFD34E));
}

/* the keyboard (kbnav.h codes): Tab / arrows move between the controls,
 * Enter / Space click; 0 if it is not for the focus (typing, Esc) */
int settings_navkey(int code) {
    if (g_page == PG_WIFI && g_wifi_focus) return 0;
    if (!g_open) return 0;
    g_gen++;
    if (fc_active()) return fc_nav(code);
    if (g_page == PG_ABOUT && g_pc_focus) return 0;
    if (g_pw_focus >= 0) return 0;                 /* typing a password: Tab and Enter are its own */
    if (g_page == PG_NETWORK && g_net_manual == 1 && g_ipf_focus >= 0) return 0;   /* (the address fields: theirs too) */
    int cx, cy;
    int r = kbnav_key(&g_nav, code, &cx, &cy);
    if (r == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        settings_click(cx, cy);
        g_nav.shown = shown;
        g_nav.focus = focus;
    }
    return r != 0;
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
    kbnav_mouse(&g_nav);

    /* sidebar */
    if (lx >= 8 && lx < 8 + SIDE_W - 4) {
        int i = (ly - BODY_Y - 4) / sidebar_item_h();
        if (ly >= BODY_Y + 4 && i >= 0 && i < PG_COUNT) { wifi_clear_password(); g_page = i; g_pc_focus = 0; fc_close(); }
        return;
    }
    if (fc_active()) { fc_click(mx, my); return; }

    int x = cx0(), y = cy0();
    if(g_page==PG_POWER) {
        laptop_info_t p;laptop_get_info(&p);
        if(inside(mx,my,x+cw()-110,y+68,110,20) && p.battery_count>1) g_power_battery++;
        if(inside(mx,my,x+cw()-90,y+242,90,20) && p.usbc.num_ports>1) g_power_port++;
        int current=gpu_backlight(-1);
        if(current>=0 && inside(mx,my,x,y+198,80,20)) gpu_backlight_step(-10);
        if(current>=0 && inside(mx,my,x+90,y+198,80,20)) gpu_backlight_step(10);
        g_gen++;return;
    }
    if (g_page == PG_ABOUT) {
        int fw = cw() - 100;
        if (inside(mx, my, x, y + 52, fw, 22)) {
            if (!g_pc_dirty) kstrlcpy(g_pc_name, hostname_get(), sizeof(g_pc_name));
            g_pc_focus = 1;
            return;
        }
        if (inside(mx, my, x + fw + 10, y + 53, 90, 20)) {
            if (!g_pc_dirty) kstrlcpy(g_pc_name, hostname_get(), sizeof(g_pc_name));
            pc_submit();
            return;
        }
        g_pc_focus = 0;
    }
    if(g_page==PG_ABOUT && g_sleep_available && inside(mx,my,x+cw()-90,y+28,90,20)) {
        acpi_s3_suspend(0,g_status,sizeof(g_status));g_gen++;return;
    }
    if (g_page == PG_THEMES) { click_themes(mx, my); g_gen++; return; }
    if (g_page == PG_FONTS) { click_fonts(mx, my); return; }
    if(g_page==PG_DISPLAY) {
        wp_gallery_t l=gallery();
        if(l.pages>1&&wp_visible(pager_y(),20)){
            if(g_wp_page>0&&inside(mx,my,x,pager_y(),80,20)){g_wp_page--;g_gen++;return;}
            if(g_wp_page+1<l.pages&&inside(mx,my,x+cw()-64,pager_y(),64,20)){g_wp_page++;g_gen++;return;}
        }
        for (int i = 0; i < wallpaper_recent_count(); i++) {
            int tx, ty;
            pic_rect(i, &tx, &ty);
            if(tx+THUMB_W>x+cw()||!wp_visible(ty,THUMB_H+18))break;
            if (inside(mx, my, tx + THUMB_W - 12, ty, 12, 12)) { wallpaper_recent_remove(i); return; }
            if (inside(mx, my, tx, ty, THUMB_W, THUMB_H + 18)) { picture_chosen(wallpaper_recent(i)); return; }
        }
        int by = wp_buttons_y();
        if(wp_visible(by,20)&&inside(mx,my,x,by,96,20)) {
            fc_open(_("Choose a picture for the wallpaper"), WALLPAPER_DIR, is_picture, picture_chosen);
            return;
        }
        for (int m = 0; m < 4; m++)
            if(wp_visible(by+24,20)&&inside(mx,my,x+80+m*((cw()-80)/4),by+24,64,20)) {
                char err[80];
                if (wallpaper_set_mode(MODES[m], err, sizeof(err)) == 0)
                    ksnprintf(g_status, sizeof(g_status), "Position: %s", _(MODE_NAMES[m]));
                else ksnprintf(g_status, sizeof(g_status), "Choose a picture first (Browse...)");
                return;
            }
        for(int slot=gallery_first();slot<gallery_end();slot++) {
            int i=wallpaper_gallery_preset(slot),tx,ty;
            thumb_rect(slot,&tx,&ty);
            if(wp_visible(ty,THUMB_H+20)&&inside(mx,my,tx,ty,THUMB_W,THUMB_H+18)) {
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
    } else if (g_page == PG_SCREEN && !display_can_change()) {
        click_screen_boot(mx, my);
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
    } else if (g_page == PG_TASKBAR) {
        for (int i = 0; i < 3; i++)
            if (inside(mx, my, x, y + 44 + i * ITEM_H - 4, 280, ITEM_H - 2)) {
                gui_set_taskbar_style(i);
                ksnprintf(g_status, sizeof(g_status), "Taskbar: %s", _(TB_STYLES[i]));
                return;
            }
        if (inside(mx, my, x, y + 44 + 3 * ITEM_H + 12, 320, ITEM_H)) {
            gui_set_taskbar_desks(!gui_taskbar_desks());
            ksnprintf(g_status, sizeof(g_status), "Desktop buttons: %s", gui_taskbar_desks() ? "shown" : "hidden");
            return;
        }
    } else if (g_page == PG_LANGUAGE) {
        click_language(mx, my);
    } else if (g_page == PG_KEYBOARD) {
        int per = kb_per_col(), colw = cw() / 2;
        for (int i = 0; i < keyboard_layout_count(); i++) {
            int rx = x + (i / per) * colw, ry = y + 44 + (i % per) * KB_ROW;
            if (inside(mx, my, rx, ry - 3, colw, KB_ROW)) {
                const char* what = NULL;
                const char* name = keyboard_layout_at(i, &what);
                if (keyboard_set_layout(name) == 0) {
                    save_setting("keyboard", name);
                    ksnprintf(g_status, sizeof(g_status), "Keyboard layout: %s", what);
                }
                return;
            }
        }
    } else if (g_page == PG_MOUSE) {
        int sx, sy, sw;
        speed_rect(&sx, &sy, &sw);
        int sp = mouse_get_speed();
        if (inside(mx, my, sx - 6, sy, sw + 12, 28)) { set_speed_at(mx); g_dragging_speed = 1; return; }
        if (inside(mx, my, x, sy + 56, 70, 20)) mouse_set_speed(sp - 1);
        else if (inside(mx, my, x + 80, sy + 56, 70, 20)) mouse_set_speed(sp + 1);
        else if (inside(mx, my, x + 160, sy + 56, 70, 20)) mouse_set_speed(5);
        else if (inside(mx, my, x, sy + 120, cw(), 22)) {
            tp_tap_to_click = !tp_tap_to_click;
            save_setting("tp_tap", tp_tap_to_click ? "1" : "0");
            ksnprintf(g_status, sizeof(g_status), "Tap to click: %s", tp_tap_to_click ? "on" : "off");
            return;
        } else if (inside(mx, my, x, sy + 146, cw(), 22)) {
            tp_natural_scroll = !tp_natural_scroll;
            save_setting("tp_natural", tp_natural_scroll ? "1" : "0");
            ksnprintf(g_status, sizeof(g_status), "Natural scrolling: %s", tp_natural_scroll ? "on" : "off");
            return;
        }
        else return;
        save_speed();
        ksnprintf(g_status, sizeof(g_status), "Pointer speed: %d", mouse_get_speed());
    } else if (g_page == PG_TIME) {
        click_time(mx, my);
    } else if (g_page == PG_APPS) {
        click_apps(mx, my);
    } else if (g_page == PG_STARTUP) {
        click_startup(mx, my);
    } else if (g_page == PG_WIFI) {
        click_wifi(mx,my);
    } else if (g_page == PG_NETWORK) {
        for (int priority = NET_PRIORITY_AUTO; priority <= NET_PRIORITY_WIFI; priority++) {
            const int bx[] = {110, 196, 312}, bw[] = {80, 110, 100};
            if (inside(mx, my, x + bx[priority], y + 24, bw[priority], 20)) {
                int disk = netconf_set_priority(priority);
                g_net_manual = 0; g_ipf_focus = -1;
                kstrlcpy(g_status, disk < 0 ? "Priority applied; settings could not be saved" :
                         "Priority applied (DHCP); disconnected links use a fallback", sizeof(g_status));
                return;
            }
        }
        y += 32;
        netif_t* nf = net_if();
        if (!nf || !nf->dev) return;
        g_ipf_focus = -1;
        if (inside(mx, my, x + NET_FX, y + 66, 150, 20)) { g_net_manual = 0; return; }              /* Automatic */
        if (inside(mx, my, x + NET_FX + 156, y + 66, 90, 20)) {                                    /* Manual */
            if (g_net_manual != 1) { g_net_manual = 1; ipf_fill(nf); }
            g_ipf_focus = 0;
            return;
        }
        if (g_net_manual == 1) {
            for (int f = 0; f < 4; f++)
                if (inside(mx, my, x + NET_FX, y + 98 + f * 26, 160, 20)) { g_ipf_focus = f; return; }
            if (inside(mx, my, x + NET_FX, y + 204, 100, 20)) { ipf_apply(); return; }
        } else if (inside(mx, my, x, y + 176, 200, 20)) {                                         /* renew / back to DHCP */
            if (!nf->dhcp) netconf_use_dhcp();
            nf->configured = 0;
            dhcp_start();
            kstrlcpy(g_status, "Asking the network for an address...", sizeof(g_status));
            return;
        }
        if (inside(mx, my, x + 360, y + NET_LOW, 64, 20)) {
            fw_enable(!fw_enabled());
            kstrlcpy(g_status, fw_enabled() ? "Firewall on" : "Firewall off", sizeof(g_status));
            return;
        }
        if (inside(mx, my, x + 360, y + NET_LOW + 50, 64, 20)) {
            ipv6_enable(!ipv6_enabled());
            kstrlcpy(g_status, ipv6_enabled() ? "IPv6 on" : "IPv6 off", sizeof(g_status));
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
    if (g_dragging_speed) {
        if (left) { set_speed_at(mx); g_gen++; }
        else {
            g_dragging_speed = 0;
            save_speed();
            ksnprintf(g_status, sizeof(g_status), "Pointer speed: %d", mouse_get_speed());
            g_gen++;
        }
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
    wifi_clear_password();
    char reason[160];g_sleep_available=!acpi_s3_status(reason,sizeof(reason));
    g_open = 1;
    g_wp_page=0;
    g_pc_focus = g_pc_dirty = 0;
    kstrlcpy(g_pc_name, hostname_get(), sizeof(g_pc_name));
    g_status[0] = 0;
    win_clamp(&g_win);
    g_gen++;
}

void settings_close(void) {
    wifi_clear_password();
    g_open = 0;
    g_pc_focus = 0;
    fc_close();
    memset(g_pw, 0, sizeof(g_pw));
    g_pw_focus = -1;
    g_win.dragging = g_win.resizing = 0;
    g_dragging_vol = g_dragging_speed = 0;
    g_gen++;
}

int settings_is_open(void) { return g_open; }
int settings_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }

uint32_t settings_signature(void) {
    if (!g_open) return 0;
    /* the clock, uptime and network status change by themselves */
    if (g_page == PG_APPS && timer_ms() - g_al_scanned > 2000) apps_scan();     /* installed or removed meanwhile */
    if (g_page == PG_POWER || g_page == PG_WIFI || g_page == PG_TIME || g_page == PG_NETWORK || g_page == PG_ABOUT || g_page == PG_STARTUP || g_page == PG_APPS) {
        uint32_t s = timer_ms() / (g_page == PG_STARTUP || g_page == PG_ABOUT ? 500 : 1000);
        if (s != g_tick_s) { g_tick_s = s; g_gen++; }
    }
    return g_gen * 2654435761u ^ (uint32_t)((uint32_t)g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^
           (uint32_t)g_page << 28 ^ (uint32_t)audio_get_volume() << 8 ^ (uint32_t)mouse_get_speed() << 15 ^ wallpaper_generation() ^ (uint32_t)gfx_smooth_text() << 27 ^
           (fc_active() ? fc_generation() * 40503u : 0);
}

void settings_boot(void) {
    char v[32];
    i18n_boot();                        /* the language (English unless chosen) */
    gpu_radeon_boot();                  /* the experimental Radeon driver, when asked for */
    if (cfg_get(CFG_SETTINGS, "volume", v, sizeof(v))) {
        uint32_t n = 0;
        if (k_parse_u32(v, &n) && n <= 100) audio_set_volume((int)n);
    }
    if (cfg_get(CFG_SETTINGS, "keyboard", v, sizeof(v))) keyboard_set_layout(v);
    rtc_boot();                         /* whether the clock chip keeps UTC */
    tz_boot();                          /* the time zone */
    if (cfg_get(CFG_SETTINGS, "tp_tap", v, sizeof(v))) tp_tap_to_click = strcmp(v, "0") != 0;
    if (cfg_get(CFG_SETTINGS, "tp_natural", v, sizeof(v))) tp_natural_scroll = strcmp(v, "0") != 0;
    if (cfg_get(CFG_SETTINGS, "mouse_speed", v, sizeof(v))) {
        uint32_t n = 0;
        if (k_parse_u32(v, &n)) mouse_set_speed((int)n);
    }
    if (cfg_get(CFG_SETTINGS, "ui_font", v, sizeof(v))) gfx_set_smooth_text(strcmp(v, "classic") != 0);
    /* the installed fonts, and which ones are used */
    {
        static int idx[64];
        int n = fs_list_files(FONT_DIR, idx, 64);
        for (int i = 0; i < n && i < 64; i++) {
            fs_file_t* f = fs_file_info(idx[i]);
            char p[FS_PATH_LEN], err[96];
            if (!f || !is_ttf(f->name)) continue;
            ksnprintf(p, sizeof(p), "%s/%s", FONT_DIR, f->name);
            if (font_user_load(p, err, sizeof(err)) < 0) klog("fonts: %s: %s\n", p, err);
        }
        char p[FS_PATH_LEN];
        if (cfg_get(CFG_SETTINGS, "ui_face", p, sizeof(p)) && p[0]) font_set_ui(font_user_find(p));
        if (cfg_get(CFG_SETTINGS, "doc_font", p, sizeof(p)) && p[0]) font_set_doc(font_user_find(p));
    }
    if (cfg_get(CFG_SETTINGS, "text_size", v, sizeof(v))) {
        uint32_t n = 0;
        if (k_parse_u32(v, &n)) gfx_set_text_size((int)n);
    }
    if (cfg_get(CFG_SETTINGS, "resolution", v, sizeof(v))) {       /* "1024x768" */
        uint32_t w = 0, h = 0;
        const char* xp = strchr(v, 'x');
        if (xp && k_parse_u32(v, &w) && k_parse_u32(xp + 1, &h)) display_set_mode((int)w, (int)h);
    }
}

/* the window manager (kernel/gui.c) moves, snaps and maximizes it through its geometry */
win_geom_t* settings_geom(void) { return &g_win; }
