/* "Choose a file" (filechooser.h): a panel inside another window. */
#include "filechooser.h"
#include "fs.h"
#include "gfx.h"
#include "kstring.h"
#include "timer.h"
#include "fileicons.h"

#define C_BG     0x001D232Cu
#define C_LIST   0x00141920u
#define C_SIDE   0x00161B22u
#define C_TEXT   0x00E8EEF6u
#define C_DIM    0x00AAB6C6u
#define C_HEAD   0x00FFFFFFu
#define C_ACCENT 0x003A7BD5u
#define C_SEL    0x002C3E5Cu
#define C_BTN    0x00303740u

#define ROW_H    20
#define SIDE_W   104
#define MAX_ENT  400

typedef struct { char name[FS_NAME_LEN]; int is_dir; } ent_t;

static int         g_on;
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
    { "Home",      "/home/banana",           FI_HOME },
    { "Pictures",  "/home/banana/Pictures",  FI_PICTURES },
    { "Downloads", "/home/banana/Downloads", FI_DOWNLOADS },
    { "USB",       "/mnt",                   FI_USB },
    { "Computer",  "/",                      FI_COMPUTER },
};
#define NPLACES ((int)(sizeof(PLACES) / sizeof(PLACES[0])))

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void button(int x, int y, int w, const char* label, int on) {
    uint32_t base = on ? C_ACCENT : C_BTN;
    gfx_fill_rect(x, y, w, 20, base);
    gfx_fill_rect(x, y, w, 1, 0x00535D6Eu);
    gfx_fill_rect(x, y + 19, w, 1, 0x0015191Fu);
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

/* opens the selected folder, or chooses the selected file */
static void choose(void) {
    if (g_sel < 0 || g_sel >= g_n) return;
    char p[FS_PATH_LEN];
    child(g_ent[g_sel].name, p, sizeof(p));
    if (g_ent[g_sel].is_dir) { load(p); return; }
    fc_done_t done = g_done;
    fc_close();
    if (done) done(p);
}

void fc_open(const char* title, const char* start_dir, fc_filter_t filter, fc_done_t done) {
    kstrlcpy(g_title, title, sizeof(g_title));
    g_filter = filter;
    g_done = done;
    g_on = 1;
    load(start_dir && *start_dir ? start_dir : "/home/banana");
}

int  fc_active(void) { return g_on; }
void fc_close(void) { g_on = 0; g_gen++; }
uint32_t fc_generation(void) { return g_gen ^ (uint32_t)g_sel << 20 ^ (uint32_t)g_top << 10; }

static int list_y(void) { return g_y + 50; }
static int list_h(void) { return g_h - 50 - 32; }
static int rows(void) { int r = list_h() / ROW_H; return r < 1 ? 1 : r; }

void fc_draw(int x, int y, int w, int h) {
    if (!g_on) return;
    g_x = x; g_y = y; g_w = w; g_h = h;
    gfx_fill_rect(x, y, w, h, C_BG);
    gfx_draw_text(x, y + 2, g_title, C_HEAD, C_BG);
    /* the folder, and Up */
    button(x, y + 20, 40, "Up", 0);
    char path[FS_PATH_LEN];
    kstrlcpy(path, g_dir, sizeof(path));
    int maxc = (w - 56) / 8;
    const char* shown = path;
    if ((int)strlen(path) > maxc && maxc > 3) shown = path + strlen(path) - (maxc - 2);
    gfx_fill_rect(x + 48, y + 20, w - 48, 20, C_LIST);
    if (shown != path) { gfx_draw_text(x + 52, y + 26, "..", C_DIM, C_LIST); gfx_draw_text(x + 68, y + 26, shown, C_TEXT, C_LIST); }
    else gfx_draw_text(x + 52, y + 26, shown, C_TEXT, C_LIST);

    /* places */
    int ly = list_y(), lh = list_h();
    gfx_fill_rect(x, ly, SIDE_W, lh, C_SIDE);
    for (int i = 0; i < NPLACES; i++) {
        int py = ly + 4 + i * 24;
        int on = !strcmp(g_dir, PLACES[i].path);
        uint32_t bg = on ? C_SEL : C_SIDE;
        gfx_fill_rect(x + 2, py, SIDE_W - 4, 22, bg);
        fileicon_draw(PLACES[i].icon, x + 6, py + 3, 16);
        gfx_draw_text(x + 26, py + 7, PLACES[i].name, on ? C_HEAD : C_DIM, bg);
    }

    /* the folder's contents */
    int lx = x + SIDE_W + 4, lw = w - SIDE_W - 4;
    gfx_fill_rect(lx, ly, lw, lh, C_LIST);
    int r = rows();
    if (g_n == 0) gfx_draw_text(lx + 8, ly + 8, g_filter ? "No folders or matching files here." : "This folder is empty.", C_DIM, C_LIST);
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
        gfx_fill_rect(lx + lw - 5, ty, 4, th, 0x00596678u);
    }

    /* Cancel, Open */
    int by = y + h - 24;
    int can_open = g_sel >= 0 && g_sel < g_n;
    button(x + w - 172, by, 80, "Cancel", 0);
    button(x + w - 84, by, 84, "Open", can_open);
    if (can_open && !g_ent[g_sel].is_dir) {
        char line[80];
        ksnprintf(line, sizeof(line), "%s", g_ent[g_sel].name);
        int maxl = (w - 180) / 8;
        if (maxl > 0 && maxl < (int)sizeof(line)) line[maxl] = 0;
        gfx_draw_text(x, by + 6, line, C_DIM, C_BG);
    }
}

int fc_click(int mx, int my) {
    if (!g_on || !inside(mx, my, g_x, g_y, g_w, g_h)) return 0;
    g_gen++;
    if (inside(mx, my, g_x, g_y + 20, 40, 20)) { up(); return 1; }
    int ly = list_y(), lh = list_h();
    for (int i = 0; i < NPLACES; i++)
        if (inside(mx, my, g_x, ly + 4 + i * 24, SIDE_W, 22)) { load(PLACES[i].path); return 1; }
    int lx = g_x + SIDE_W + 4, lw = g_w - SIDE_W - 4;
    if (inside(mx, my, lx, ly, lw, lh)) {
        int row = g_top + (my - ly) / ROW_H;
        if (row >= g_n) { g_sel = -1; return 1; }
        uint32_t now = timer_ms();
        int dbl = row == g_last_row && now - g_last_ms < 450;
        g_sel = row;
        g_last_row = row;
        g_last_ms = now;
        if (dbl) { g_last_row = -1; choose(); }
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

void fc_key(char c) {
    if (!g_on) return;
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
