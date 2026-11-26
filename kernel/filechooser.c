/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "Choose a file" (filechooser.h): a panel inside another window. */
#include "filechooser.h"
#include "i18n.h"
#include "theme.h"
#include "fs.h"
#include "gfx.h"
#include "kstring.h"
#include "timer.h"
#include "fileicons.h"
#include "kbnav.h"

#define C_BG     TC(0x1D232C)
#define C_LIST   TC(0x141920)
#define C_SIDE   TC(0x161B22)
#define C_TEXT   TC(0xE8EEF6)
#define C_DIM    TC(0xAAB6C6)
#define C_HEAD   TC(0xFFFFFF)
#define C_ACCENT TC(0x3A7BD5)
#define C_SEL    TC(0x2C3E5C)
#define C_BTN    TC(0x303740)

#define ROW_H    20
#define SIDE_W   104
#define MAX_ENT  400

typedef struct { char name[FS_NAME_LEN]; int is_dir; } ent_t;

static int         g_on;
/* save mode (fc_save): a file name to type, a type to show, Save; a name
 * that is taken asks first */
static int         g_save;
static char        g_fname[FS_NAME_LEN];
static int         g_type;                      /* 0: text documents (*.txt), 1: all files */
static int         g_confirm;                   /* "already exists - replace it?" */
static char        g_cpath[FS_PATH_LEN];
static char        g_title[48];
static char        g_dir[FS_PATH_LEN];
static fc_filter_t g_filter;
static fc_done_t   g_done;
static ent_t       g_ent[MAX_ENT];
static int         g_n, g_sel = -1, g_top;
static int         g_x, g_y, g_w, g_h;           /* where it was drawn */
static uint32_t    g_gen = 1;
static uint32_t    g_last_ms;
static int         g_last_row = -1;

static const struct { const char* name; const char* path; fileicon_t icon; } PLACES[] = {
    { "Home",      "~",           FI_HOME },
    { "Pictures",  "~/Pictures",  FI_PICTURES },
    { "Downloads", "~/Downloads", FI_DOWNLOADS },
    { "USB",       "/mnt",                   FI_USB },
    { "Computer",  "/",                      FI_COMPUTER },
};

/* a place's folder: the logged-in user's home for "~" */
static const char* place_path(int i) { return PLACES[i].path[0] == '~' ? fs_home_join(PLACES[i].path + 1) : PLACES[i].path; }

#define NPLACES ((int)(sizeof(PLACES) / sizeof(PLACES[0])))

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void button(int x, int y, int w, const char* label, int on) {
    uint32_t base = on ? C_ACCENT : C_BTN;
    gfx_fill_rect(x, y, w, 20, base);
    gfx_fill_rect(x, y, w, 1, TC(0x535D6E));
    gfx_fill_rect(x, y + 19, w, 1, TC(0x15191F));
    gfx_draw_text(x + (w - (int)strlen(label) * 8) / 2, y + 6, label, C_TEXT, base);
}

/* the folders first, then the files; each by name */
static int before(const ent_t* a, const ent_t* b) {
    if (a->is_dir != b->is_dir) return a->is_dir;
    return strcasecmp(a->name, b->name) < 0;
}

static void load(const char* dir) {
    if (fs_find_dir(dir) < 0) dir = "/";
    kstrlcpy(g_dir, dir, sizeof(g_dir));
    g_n = 0;
    g_sel = -1;
    g_top = 0;
    static int idx[MAX_ENT];
    int nd = fs_list_dirs(g_dir, idx, MAX_ENT);
    for (int i = 0; i < nd && i < MAX_ENT && g_n < MAX_ENT; i++) {
        const fs_dir_t* d = fs_get_dir(idx[i]);
        if (!d || d->name[0] == '.') continue;
        kstrlcpy(g_ent[g_n].name, d->name, FS_NAME_LEN);
        g_ent[g_n++].is_dir = 1;
    }
    int nf = fs_list_files(g_dir, idx, MAX_ENT);
    for (int i = 0; i < nf && i < MAX_ENT && g_n < MAX_ENT; i++) {
        fs_file_t* f = fs_file_info(idx[i]);
        if (!f || f->name[0] == '.' || (g_filter && !g_filter(f->name))) continue;
        if (g_save && g_type == 0) {                      /* Text documents: *.txt */
            size_t l = strlen(f->name);
            if (l < 4 || strcasecmp(f->name + l - 4, ".txt")) continue;
        }
        kstrlcpy(g_ent[g_n].name, f->name, FS_NAME_LEN);
        g_ent[g_n++].is_dir = 0;
    }
    for (int i = 1; i < g_n; i++)
        for (int j = i; j > 0 && before(&g_ent[j], &g_ent[j - 1]); j--) {
            ent_t t = g_ent[j]; g_ent[j] = g_ent[j - 1]; g_ent[j - 1] = t;
        }
    g_gen++;
}

static void child(const char* name, char* out, int cap) {
    if (!strcmp(g_dir, "/")) ksnprintf(out, (uint32_t)cap, "/%s", name);
    else ksnprintf(out, (uint32_t)cap, "%s/%s", g_dir, name);
}

static void up(void) {
    char p[FS_PATH_LEN];
    kstrlcpy(p, g_dir, sizeof(p));
    char* s = strrchr(p, '/');
    if (!s || s == p) kstrlcpy(p, "/", sizeof(p));
    else *s = 0;
    load(p);
}

static void finish(const char* path) {
    fc_done_t done = g_done;
    fc_close();
    if (done) done(path);
}

/* Save: the name typed (or chosen) in this folder; a folder's name opens
 * it; ".txt" added for a text document without one; a name taken asks */
static void save_attempt(void) {
    if (!g_fname[0]) {
        if (g_sel >= 0 && g_sel < g_n && g_ent[g_sel].is_dir) { char p[FS_PATH_LEN]; child(g_ent[g_sel].name, p, sizeof(p)); load(p); }
        return;
    }
    char p[FS_PATH_LEN];
    if (g_fname[0] == '/') kstrlcpy(p, g_fname, sizeof(p));
    else child(g_fname, p, sizeof(p));
    if (fs_find_dir(p) >= 0) { load(p); g_fname[0] = 0; return; }
    const char* base = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
    if (g_type == 0 && !strchr(base, '.') && strlen(p) + 4 < sizeof(p)) kstrlcat(p, ".txt", sizeof(p));
    if (fs_find_file(p) >= 0) { kstrlcpy(g_cpath, p, sizeof(g_cpath)); g_confirm = 1; g_gen++; return; }
    finish(p);
}

/* opens the selected folder, or chooses the selected file */
static void choose(void) {
    if (g_save) { save_attempt(); return; }
    if (g_sel < 0 || g_sel >= g_n) return;
    char p[FS_PATH_LEN];
    child(g_ent[g_sel].name, p, sizeof(p));
    if (g_ent[g_sel].is_dir) { load(p); return; }
    finish(p);
}

void fc_open(const char* title, const char* start_dir, fc_filter_t filter, fc_done_t done) {
    kstrlcpy(g_title, title, sizeof(g_title));
    g_filter = filter;
    g_done = done;
    g_save = 0;
    g_confirm = 0;
    g_on = 1;
    load(start_dir && *start_dir ? start_dir : fs_home_path());
}

void fc_save(const char* title, const char* start_dir, const char* name, fc_done_t done) {
    kstrlcpy(g_title, title, sizeof(g_title));
    g_filter = NULL;
    g_done = done;
    g_save = 1;
    g_confirm = 0;
    kstrlcpy(g_fname, name ? name : "", sizeof(g_fname));
    size_t l = strlen(g_fname);
    g_type = l == 0 || (l >= 4 && !strcasecmp(g_fname + l - 4, ".txt")) ? 0 : 1;   /* (another kind of file: all files) */
    g_on = 1;
    load(start_dir && *start_dir ? start_dir : fs_home_path());
}

/* a new folder here, chosen in the list ("New folder (2)"... if taken) */
static void new_folder(void) {
    char p[FS_PATH_LEN];
    child("New folder", p, sizeof(p));
    for (int n = 2; (fs_find_dir(p) >= 0 || fs_find_file(p) >= 0) && n < 100; n++) {
        char nm[32];
        ksnprintf(nm, sizeof(nm), "New folder (%d)", n);
        child(nm, p, sizeof(p));
    }
    if (fs_mkdir(p) < 0) return;
    const char* nm = strrchr(p, '/') + 1;
    char keep[FS_NAME_LEN];
    kstrlcpy(keep, nm, sizeof(keep));
    load(g_dir);
    for (int i = 0; i < g_n; i++) if (g_ent[i].is_dir && !strcmp(g_ent[i].name, keep)) g_sel = i;
}

int  fc_active(void) { return g_on; }
void fc_close(void) { g_on = 0; g_gen++; }
uint32_t fc_generation(void) { return g_gen ^ (uint32_t)g_sel << 20 ^ (uint32_t)g_top << 10; }

static int list_y(void) { return g_y + 50; }
static int list_h(void) { return g_h - 50 - 32 - (g_save ? 58 : 0); }   /* (save: the name and the type under it) */
static int rows(void) { int r = list_h() / ROW_H; return r < 1 ? 1 : r; }

#define NEWDIR_W 96                              /* the New folder button, right of Up */
#define TYPE_W   200                             /* the Save as type box */
static const char* type_name(void) { return g_type == 0 ? "Text Documents (*.txt)" : "All Files (*.*)"; }

/* save mode: where the name field and the type box are */
static int field_y(void) { return list_y() + list_h() + 6; }

void fc_draw(int x, int y, int w, int h) {
    if (!g_on) return;
    g_x = x; g_y = y; g_w = w; g_h = h;
    gfx_fill_rect(x, y, w, h, C_BG);
    gfx_draw_text(x, y + 2, g_title, C_HEAD, C_BG);
    /* Up, New folder, and the folder */
    button(x, y + 20, 40, _("Up"), 0);
    button(x + 44, y + 20, NEWDIR_W, _("New folder"), 0);
    int px = x + 44 + NEWDIR_W + 4;
    char path[FS_PATH_LEN];
    kstrlcpy(path, g_dir, sizeof(path));
    int maxc = (x + w - px - 8) / 8;
    const char* shown = path;
    if ((int)strlen(path) > maxc && maxc > 3) shown = path + strlen(path) - (maxc - 2);
    gfx_fill_rect(px, y + 20, x + w - px, 20, C_LIST);
    if (shown != path) { gfx_draw_text(px + 4, y + 26, "..", C_DIM, C_LIST); gfx_draw_text(px + 20, y + 26, shown, C_TEXT, C_LIST); }
    else gfx_draw_text(px + 4, y + 26, shown, C_TEXT, C_LIST);

    /* places */
    int ly = list_y(), lh = list_h();
    gfx_fill_rect(x, ly, SIDE_W, lh, C_SIDE);
    for (int i = 0; i < NPLACES; i++) {
        int py = ly + 4 + i * 24;
        if (py + 22 > ly + lh) break;
        int on = !strcmp(g_dir, place_path(i));
        uint32_t bg = on ? C_SEL : C_SIDE;
        gfx_fill_rect(x + 2, py, SIDE_W - 4, 22, bg);
        fileicon_draw(PLACES[i].icon, x + 6, py + 3, 16);
        gfx_draw_text(x + 26, py + 7, PLACES[i].name, on ? C_HEAD : C_DIM, bg);
    }

    /* the folder's contents */
    int lx = x + SIDE_W + 4, lw = w - SIDE_W - 4;
    gfx_fill_rect(lx, ly, lw, lh, C_LIST);
    int r = rows();
    if (g_n == 0) gfx_draw_text(lx + 8, ly + 8, g_filter || (g_save && g_type == 0) ? "No folders or matching files here." : "This folder is empty.", C_DIM, C_LIST);
    for (int i = 0; i < r && g_top + i < g_n; i++) {
        const ent_t* e = &g_ent[g_top + i];
        int ry = ly + i * ROW_H;
        int sel = g_top + i == g_sel;
        uint32_t bg = sel ? C_SEL : C_LIST;
        if (sel) gfx_fill_rect(lx, ry, lw - 6, ROW_H, bg);
        fileicon_draw(e->is_dir ? FI_FOLDER : fileicon_for_name(e->name), lx + 4, ry + 2, 16);
        char nm[FS_NAME_LEN];
        kstrlcpy(nm, e->name, sizeof(nm));
        int maxn = (lw - 36) / 8;
        if (maxn > 0 && maxn < (int)sizeof(nm) && (int)strlen(nm) > maxn) { nm[maxn - 1] = '~'; nm[maxn] = 0; }
        gfx_draw_text(lx + 26, ry + 6, nm, sel ? C_HEAD : C_TEXT, bg);
    }
    if (g_n > r) {                                       /* a scroll bar */
        int th = lh * r / g_n;
        if (th < 12) th = 12;
        int ty = ly + (lh - th) * g_top / (g_n - r);
        gfx_fill_rect(lx + lw - 5, ty, 4, th, TC(0x596678));
    }

    int by = y + h - 24;
    if (g_save) {
        /* File name: [...........]   Save as type: [Text Documents (*.txt) v] */
        int fy = field_y(), fx = x + 112;
        gfx_draw_text(x + 4, fy + 6, _("File name:"), C_TEXT, C_BG);
        gfx_fill_rect(fx, fy, x + w - fx, 22, TC(0xFFFFFF));
        gfx_fill_rect(fx, fy + 21, x + w - fx, 1, C_ACCENT);
        int fmax = (x + w - fx - 12) / 8, fl = (int)strlen(g_fname);
        const char* fs = g_fname + (fl > fmax - 1 ? fl - (fmax - 1) : 0);
        gfx_draw_text(fx + 4, fy + 7, fs, TC(0x101010), TC(0xFFFFFF));
        if (!g_confirm) gfx_fill_rect(fx + 4 + (int)strlen(fs) * 8, fy + 5, 1, 13, TC(0x202020));
        gfx_draw_text(x + 4, fy + 34, _("Save as type:"), C_TEXT, C_BG);
        gfx_fill_rect(fx, fy + 28, TYPE_W, 22, C_LIST);
        gfx_draw_text(fx + 6, fy + 35, type_name(), C_TEXT, C_LIST);
        gfx_draw_text(fx + TYPE_W - 14, fy + 35, "v", C_DIM, C_LIST);
        button(x + w - 172, by, 80, _("Cancel"), 0);
        button(x + w - 84, by, 84, _("Save"), g_fname[0] != 0);
    } else {
        int can_open = g_sel >= 0 && g_sel < g_n;
        button(x + w - 172, by, 80, _("Cancel"), 0);
        button(x + w - 84, by, 84, _("Open"), can_open);
        if (can_open && !g_ent[g_sel].is_dir) {
            char line[80];
            ksnprintf(line, sizeof(line), "%s", g_ent[g_sel].name);
            int maxl = (w - 180) / 8;
            if (maxl > 0 && maxl < (int)sizeof(line)) line[maxl] = 0;
            gfx_draw_text(x, by + 6, line, C_DIM, C_BG);
        }
    }

    /* "... already exists. Do you want to replace it?" */
    if (g_confirm) {
        int cw = w - 40 < 420 ? w - 40 : 420, ch = 96, cx = x + (w - cw) / 2, cy = y + (h - ch) / 2;
        gfx_fill_rect(cx + 4, cy + 4, cw, ch, TC(0x080A0E));
        gfx_fill_rect(cx, cy, cw, ch, TC(0x262C36));
        gfx_fill_rect(cx, cy, cw, 1, TC(0x707C90));
        gfx_fill_rect(cx, cy + ch - 1, cw, 1, TC(0x0C0F14));
        char line[96];
        const char* b = strrchr(g_cpath, '/');
        ksnprintf(line, sizeof(line), "%s already exists.", b ? b + 1 : g_cpath);
        int ml = (cw - 24) / 8;
        if (ml > 0 && ml < (int)sizeof(line)) line[ml] = 0;
        gfx_draw_text(cx + 12, cy + 14, line, C_HEAD, TC(0x262C36));
        gfx_draw_text(cx + 12, cy + 32, _("Do you want to replace it?"), C_TEXT, TC(0x262C36));
        button(cx + cw - 184, cy + ch - 32, 80, _("Yes"), 1);
        button(cx + cw - 96, cy + ch - 32, 80, _("No"), 0);
    }
}

/* the confirmation's buttons: 1 Yes, 0 No, -1 neither */
static int confirm_hit(int mx, int my) {
    int cw = g_w - 40 < 420 ? g_w - 40 : 420, ch = 96, cx = g_x + (g_w - cw) / 2, cy = g_y + (g_h - ch) / 2;
    if (inside(mx, my, cx + cw - 184, cy + ch - 32, 80, 20)) return 1;
    if (inside(mx, my, cx + cw - 96, cy + ch - 32, 80, 20)) return 0;
    return -1;
}

int fc_click(int mx, int my) {
    if (!g_on || !inside(mx, my, g_x, g_y, g_w, g_h)) return 0;
    g_gen++;
    if (g_confirm) {                                     /* (it takes every click) */
        int k = confirm_hit(mx, my);
        if (k == 1) { g_confirm = 0; finish(g_cpath); }
        else if (k == 0) g_confirm = 0;
        return 1;
    }
    if (inside(mx, my, g_x, g_y + 20, 40, 20)) { up(); return 1; }
    if (inside(mx, my, g_x + 44, g_y + 20, NEWDIR_W, 20)) { new_folder(); return 1; }
    int ly = list_y(), lh = list_h();
    for (int i = 0; i < NPLACES; i++)
        if (ly + 4 + i * 24 + 22 <= ly + lh && inside(mx, my, g_x, ly + 4 + i * 24, SIDE_W, 22)) { load(place_path(i)); return 1; }
    int lx = g_x + SIDE_W + 4, lw = g_w - SIDE_W - 4;
    if (inside(mx, my, lx, ly, lw, lh)) {
        int row = g_top + (my - ly) / ROW_H;
        if (row >= g_n) { g_sel = -1; return 1; }
        uint32_t now = timer_ms();
        int dbl = row == g_last_row && now - g_last_ms < 450;
        g_sel = row;
        g_last_row = row;
        g_last_ms = now;
        if (g_save && !g_ent[row].is_dir) kstrlcpy(g_fname, g_ent[row].name, sizeof(g_fname));   /* (its name to save over) */
        if (dbl) {
            g_last_row = -1;
            if (g_save && g_ent[row].is_dir) { char p[FS_PATH_LEN]; child(g_ent[row].name, p, sizeof(p)); load(p); }
            else choose();
        }
        return 1;
    }
    if (g_save && inside(mx, my, g_x + 112, field_y() + 28, TYPE_W, 22)) {   /* the type: the other one */
        g_type ^= 1;
        load(g_dir);
        return 1;
    }
    int by = g_y + g_h - 24;
    if (inside(mx, my, g_x + g_w - 172, by, 80, 20)) { fc_close(); return 1; }
    if (inside(mx, my, g_x + g_w - 84, by, 84, 20)) { choose(); return 1; }
    return 1;
}

void fc_wheel(int dz) {
    if (!g_on) return;
    int max = g_n - rows();
    g_top += dz * 3;
    if (g_top > max) g_top = max;
    if (g_top < 0) g_top = 0;
    g_gen++;
}

int fc_nav(int code) {
    if (!g_on) return 0;
    int r = rows();
    if (g_confirm) {                                     /* Yes (Enter) / No (Esc) */
        if (code == KB_ENTER) { g_confirm = 0; finish(g_cpath); }
        else if (code == KB_ESC) { g_confirm = 0; g_gen++; }
        return 1;
    }
    if (g_save && (code == KB_LEFT || code == KB_RIGHT)) return 1;   /* (the name field has the caret) */
    switch (code) {
    case KB_UP: if (g_sel > 0) g_sel--; else g_sel = 0; break;
    case KB_DOWN: if (g_sel < g_n - 1) g_sel++; break;
    case KB_PGUP: g_sel -= r; if (g_sel < 0) g_sel = 0; break;
    case KB_PGDN: g_sel += r; if (g_sel >= g_n) g_sel = g_n - 1; break;
    case KB_HOME: g_sel = 0; break;
    case KB_END: g_sel = g_n - 1; break;
    case KB_LEFT: up(); return 1;
    case KB_RIGHT: case KB_ENTER: choose(); return 1;
    case KB_ESC: fc_close(); return 1;
    case KB_TAB: case KB_BACKTAB: {                 /* the places, in turn */
        static int place;
        place = (place + (code == KB_TAB ? 1 : NPLACES - 1)) % NPLACES;
        load(place_path(place));
        return 1;
    }
    default: return 0;
    }
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + r) g_top = g_sel - r + 1;
    if (g_top < 0) g_top = 0;
    if (g_save && g_sel >= 0 && g_sel < g_n && !g_ent[g_sel].is_dir) kstrlcpy(g_fname, g_ent[g_sel].name, sizeof(g_fname));
    g_gen++;
    return 1;
}

void fc_key(char c) {
    if (!g_on) return;
    if (g_confirm) {                                     /* y / Enter: replace; n / Esc: not */
        if (c == 'y' || c == 'Y' || c == '\n') { g_confirm = 0; finish(g_cpath); }
        else if (c == 'n' || c == 'N' || c == 27) { g_confirm = 0; g_gen++; }
        return;
    }
    if (g_save) {                                        /* the keys type the name */
        int n = (int)strlen(g_fname);
        if (c == 27) fc_close();
        else if (c == '\n') save_attempt();
        else if (c == '\b') { while (n > 0 && ((unsigned char)g_fname[n - 1] & 0xC0) == 0x80) n--; if (n > 0) n--; g_fname[n] = 0; }
        else if ((unsigned char)c >= 32 && n < (int)sizeof(g_fname) - 1) { g_fname[n] = c; g_fname[n + 1] = 0; }
        g_gen++;
        return;
    }
    if (c == 27) { fc_close(); return; }
    if (c == '\n') { choose(); return; }
    if (c == '\b') { up(); return; }
    /* a letter: the next entry starting with it */
    if ((unsigned char)c > ' ') {
        for (int k = 1; k <= g_n; k++) {
            int i = (g_sel + k + g_n) % g_n;
            if (k_tolower((unsigned char)g_ent[i].name[0]) == k_tolower((unsigned char)c)) {
                g_sel = i;
                if (g_sel < g_top) g_top = g_sel;
                if (g_sel >= g_top + rows()) g_top = g_sel - rows() + 1;
                break;
            }
        }
    }
    g_gen++;
}
