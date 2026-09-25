#include "explorer.h"
#include "gfx.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "image.h"
#include "wallpaper.h"

#define WIN_W    600
#define WIN_H    404
#define TITLE_H  20
#define LIST_X   8
#define LIST_Y   70
#define LIST_W   356
#define ROW_H    16
#define ROWS     17                     /* visible rows */
#define PREV_X   (LIST_X + LIST_W + 10)
#define PREV_W   (WIN_W - PREV_X - 8)
#define THUMB_W  (PREV_W - 8)
#define THUMB_H  140
#define MAX_ITEMS 320

#define C_PANEL   0x001D232Cu
#define C_TITLE   0x00384562u
#define C_TEXT    0x00E8EEF6u
#define C_DIM     0x00AAB6C6u
#define C_SEL     0x003A5A8Au
#define C_LIST    0x00141920u
#define C_FOLDER  0x00F4D35Eu
#define C_WARN    0x00F0A060u

typedef struct {
    int is_dir;
    int idx;                            /* fs_get_dir / fs_get_file index */
} item_t;

static int      g_open;
static int      g_x = 100, g_y = 56;
static int      g_dragging, g_drag_dx, g_drag_dy;
static char     g_path[FS_PATH_LEN] = "/home/banana";
static item_t   g_items[MAX_ITEMS];
static int      g_count;
static int      g_scroll;
static int      g_sel = -1;
static int      g_confirm_delete;
static uint32_t g_last_click_ms;
static int      g_last_click_row = -1;
static char     g_status[96];
static uint32_t g_gen;                  /* bumped on every state change */

/* picture preview: decoded once when a picture is selected */
static uint32_t* g_thumb;
static int      g_thumb_for = -1;       /* file index the thumbnail shows */
static char     g_thumb_err[48];

/* ── helpers ──────────────────────────────────────────────────────── */

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void button(int x, int y, int w, const char* label, int enabled) {
    uint32_t base = enabled ? 0x00303740u : 0x00262B33u;
    bevel(x, y, w, 18, base, 0x00535D6Eu, 0x0015191Fu);
    int tw = (int)strlen(label) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 5, label, enabled ? C_TEXT : 0x00707A88u, base);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* text clipped to `max` characters, "..." when cut */
static void draw_clip(int x, int y, const char* s, int max, uint32_t fg, uint32_t bg) {
    char buf[80];
    int n = (int)strlen(s);
    if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;
    if (n <= max) {
        gfx_draw_text(x, y, s, fg, bg);
        return;
    }
    memcpy(buf, s, (size_t)(max - 3));
    memcpy(buf + max - 3, "...", 4);
    gfx_draw_text(x, y, buf, fg, bg);
}

static void human_size(uint32_t n, char* out, int cap) {
    if (n < 1024) ksnprintf(out, (size_t)cap, "%u B", n);
    else if (n < 1024 * 1024) ksnprintf(out, (size_t)cap, "%u.%u KB", n / 1024, (n % 1024) * 10 / 1024);
    else ksnprintf(out, (size_t)cap, "%u.%u MB", n >> 20, ((n >> 10) & 1023) * 10 / 1024);
}

static const char* item_name(const item_t* it) {
    if (it->is_dir) { const fs_dir_t* d = fs_get_dir(it->idx); return d ? d->name : "?"; }
    fs_file_t* f = fs_get_file(it->idx);
    return (f && f->used) ? f->name : "?";
}

static void child_path(const char* name, char* out, int cap) {
    if (strcmp(g_path, "/") == 0) ksnprintf(out, (size_t)cap, "/%s", name);
    else ksnprintf(out, (size_t)cap, "%s/%s", g_path, name);
}

static void set_status(const char* s) {
    kstrlcpy(g_status, s, sizeof(g_status));
    g_gen++;
}

static void drop_thumb(void) {
    kfree(g_thumb);
    g_thumb = NULL;
    g_thumb_for = -1;
    g_thumb_err[0] = '\0';
}

/* the folder's contents: sub-folders first, then files, both A-Z */
static void scan(void) {
    int d[FS_MAX_DIRS], f[FS_MAX_FILES];
    int nd = fs_list_dirs(g_path, d, FS_MAX_DIRS);
    if (nd < 0) {                       /* folder vanished: go home */
        kstrlcpy(g_path, "/home/banana", sizeof(g_path));
        nd = fs_list_dirs(g_path, d, FS_MAX_DIRS);
        if (nd < 0) { kstrlcpy(g_path, "/", sizeof(g_path)); nd = fs_list_dirs(g_path, d, FS_MAX_DIRS); }
    }
    int nf = fs_list_files(g_path, f, FS_MAX_FILES);
    if (nd > FS_MAX_DIRS) nd = FS_MAX_DIRS;
    if (nf > FS_MAX_FILES) nf = FS_MAX_FILES;
    g_count = 0;
    for (int i = 0; i < nd && g_count < MAX_ITEMS; i++) g_items[g_count++] = (item_t){ 1, d[i] };
    for (int i = 0; i < nf && g_count < MAX_ITEMS; i++) g_items[g_count++] = (item_t){ 0, f[i] };
    /* insertion sort, folders before files */
    for (int i = 1; i < g_count; i++) {
        item_t t = g_items[i];
        int j = i - 1;
        while (j >= 0) {
            item_t* o = &g_items[j];
            int before = (t.is_dir != o->is_dir) ? t.is_dir : strcasecmp(item_name(&t), item_name(o)) < 0;
            if (!before) break;
            g_items[j + 1] = *o;
            j--;
        }
        g_items[j + 1] = t;
    }
    if (g_sel >= g_count) g_sel = -1;
    if (g_scroll > g_count - ROWS) g_scroll = g_count - ROWS;
    if (g_scroll < 0) g_scroll = 0;
}

static void go(const char* path) {
    kstrlcpy(g_path, path, sizeof(g_path));
    g_sel = -1;
    g_scroll = 0;
    g_confirm_delete = 0;
    g_status[0] = '\0';
    drop_thumb();
    scan();
    g_gen++;
}

static void go_up(void) {
    if (strcmp(g_path, "/") == 0) return;
    char p[FS_PATH_LEN];
    kstrlcpy(p, g_path, sizeof(p));
    char* slash = strrchr(p, '/');
    if (slash == p) p[1] = '\0';
    else if (slash) *slash = '\0';
    go(p);
}

static int is_text_file(int fidx) {
    fs_file_t* f = fs_get_file(fidx);
    return f && f->used && !fs_is_binary(fidx);
}

static int is_image_file(int fidx) {
    fs_file_t* f = fs_get_file(fidx);
    return f && f->used && wallpaper_is_image_name(f->name);
}

/* ── public API ───────────────────────────────────────────────────── */

void explorer_open(const char* path) {
    g_open = 1;
    g_dragging = 0;
    go(path && path[0] ? path : "/home/banana");
}

void explorer_close(void) {
    g_open = 0;
    g_dragging = 0;
    drop_thumb();
    g_gen++;
}

int explorer_is_open(void) { return g_open; }

int explorer_contains(int mx, int my) {
    return g_open && inside(mx, my, g_x, g_y, WIN_W, WIN_H);
}

uint32_t explorer_signature(void) {
    if (!g_open) return 0;
    /* files changed by a terminal meanwhile show up too */
    return g_gen * 2654435761u ^ fs_used_files() * 40503u ^ fs_used_dirs() * 977u ^
           fs_ram_used_bytes() ^ (uint32_t)(g_x << 16 | g_y);
}

/* ── actions ──────────────────────────────────────────────────────── */

static void new_folder(void) {
    char name[FS_NAME_LEN], path[FS_PATH_LEN];
    for (int n = 1; n < 100; n++) {
        if (n == 1) kstrlcpy(name, "New folder", sizeof(name));
        else ksnprintf(name, sizeof(name), "New folder %d", n);
        child_path(name, path, sizeof(path));
        if (fs_find_dir(path) >= 0 || fs_find_file(path) >= 0) continue;
        if (fs_mkdir(path) < 0) { set_status("Could not create the folder"); return; }
        scan();
        for (int i = 0; i < g_count; i++)
            if (g_items[i].is_dir && strcmp(item_name(&g_items[i]), name) == 0) g_sel = i;
        char msg[64];
        ksnprintf(msg, sizeof(msg), "Created \"%s\" (rename it with mv in a terminal)", name);
        set_status(msg);
        return;
    }
}

static void delete_selected(void) {
    if (g_sel < 0) return;
    char name[FS_NAME_LEN], path[FS_PATH_LEN], cwd[FS_PATH_LEN], msg[96];
    kstrlcpy(name, item_name(&g_items[g_sel]), sizeof(name));
    child_path(name, path, sizeof(path));
    if (!g_confirm_delete) {
        g_confirm_delete = 1;
        ksnprintf(msg, sizeof(msg), "Delete \"%s\"%s? Click Delete again", name,
                  g_items[g_sel].is_dir ? " and everything in it" : "");
        set_status(msg);
        return;
    }
    g_confirm_delete = 0;
    if (g_items[g_sel].is_dir) {
        /* the shells' working directory (or a parent of it) stays */
        fs_cwd_path(cwd, sizeof(cwd));
        size_t pl = strlen(path);
        if (strncmp(cwd, path, pl) == 0 && (cwd[pl] == '\0' || cwd[pl] == '/')) {
            set_status("That folder is a terminal's current folder - cd elsewhere first");
            return;
        }
    }
    fs_delete(path, 1);
    drop_thumb();
    g_sel = -1;
    scan();
    ksnprintf(msg, sizeof(msg), "Deleted \"%s\"", name);
    set_status(msg);
}

static void open_item(int i) {
    const item_t* it = &g_items[i];
    char path[FS_PATH_LEN], cmd[FS_PATH_LEN + 16];
    child_path(item_name(it), path, sizeof(path));
    if (it->is_dir) { go(path); return; }
    if (is_image_file(it->idx)) {
        char err[80], msg[96];
        set_status("Setting the wallpaper...");
        if (wallpaper_set_file(path, IMAGE_FILL, err, sizeof(err)) == 0)
            ksnprintf(msg, sizeof(msg), "Wallpaper: %s", item_name(it));
        else
            ksnprintf(msg, sizeof(msg), "Not a usable picture: %s", err);
        set_status(msg);
        return;
    }
    ksnprintf(cmd, sizeof(cmd), "edit \"%s\"\n", path);
    gui_terminal_run(cmd);
}

static void terminal_here(void) {
    char cmd[FS_PATH_LEN + 16];
    ksnprintf(cmd, sizeof(cmd), "cd \"%s\"\n", g_path);
    gui_terminal_run(cmd);
}

/* ── mouse ────────────────────────────────────────────────────────── */

/* toolbar buttons: x offset, width, label */
static const struct { int x, w; const char* label; } TOOLS[] = {
    { 8, 40, "Up" }, { 52, 48, "Home" }, { 104, 96, "New folder" }, { 204, 64, "Delete" },
    { 272, 80, "Terminal" }, { 356, 64, "Refresh" },
};
#define TOOL_COUNT (int)(sizeof(TOOLS) / sizeof(TOOLS[0]))

/* preview-pane action button (Open / Edit / Set as wallpaper) */
static int action_rect(int* bx, int* by, int* bw) {
    *bx = g_x + PREV_X + 4;
    *by = g_y + LIST_Y + ROW_H * (ROWS + 1) - 24;
    *bw = PREV_W - 8;
    return g_sel >= 0;
}

void explorer_click(int mx, int my) {
    if (!explorer_contains(mx, my)) return;
    int lx = mx - g_x, ly = my - g_y;
    g_gen++;

    if (ly < TITLE_H + 2) {
        if (lx >= WIN_W - 28 && lx < WIN_W - 8) { explorer_close(); return; }
        g_dragging = 1;
        g_drag_dx = lx;
        g_drag_dy = ly;
        return;
    }
    if (ly >= 26 && ly < 44) {
        for (int i = 0; i < TOOL_COUNT; i++) {
            if (lx < TOOLS[i].x || lx >= TOOLS[i].x + TOOLS[i].w) continue;
            if (i != 3) g_confirm_delete = 0;
            switch (i) {
            case 0: go_up(); break;
            case 1: go("/home/banana"); break;
            case 2: new_folder(); break;
            case 3: delete_selected(); break;
            case 4: terminal_here(); break;
            case 5: scan(); set_status("Refreshed"); break;
            }
            return;
        }
        return;
    }
    /* scrollbar */
    int sb_x = LIST_X + LIST_W - 14;
    if (lx >= sb_x && lx < LIST_X + LIST_W && ly >= LIST_Y && ly < LIST_Y + ROW_H * (ROWS + 1)) {
        int half = LIST_Y + ROW_H * (ROWS + 1) / 2;
        g_scroll += (ly < half) ? -(ROWS - 1) : (ROWS - 1);
        if (g_scroll > g_count - ROWS) g_scroll = g_count - ROWS;
        if (g_scroll < 0) g_scroll = 0;
        return;
    }
    /* list rows (row 0 is the header) */
    if (lx >= LIST_X && lx < sb_x && ly >= LIST_Y + ROW_H && ly < LIST_Y + ROW_H * (ROWS + 1)) {
        int row = (ly - LIST_Y - ROW_H) / ROW_H + g_scroll;
        if (row >= g_count) { g_sel = -1; g_confirm_delete = 0; return; }
        uint32_t now = timer_ms();
        int dbl = row == g_last_click_row && now - g_last_click_ms < 450;
        g_last_click_row = row;
        g_last_click_ms = now;
        if (row != g_sel) { g_sel = row; g_confirm_delete = 0; g_status[0] = '\0'; }
        if (dbl) { g_last_click_row = -1; open_item(row); }
        return;
    }
    int bx, by, bw;
    if (action_rect(&bx, &by, &bw) && inside(mx, my, bx, by, bw, 18)) open_item(g_sel);
}

void explorer_mouse(int mx, int my, int left) {
    if (!left) { g_dragging = 0; return; }
    if (!g_dragging) return;
    const fb_info_t* fi = fb_info();
    int nx = mx - g_drag_dx, ny = my - g_drag_dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (fi && nx + WIN_W > (int)fi->width) nx = (int)fi->width - WIN_W;
    if (fi && ny + WIN_H > (int)fi->height - 28) ny = (int)fi->height - 28 - WIN_H;
    if (nx != g_x || ny != g_y) { g_x = nx; g_y = ny; g_gen++; }
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void draw_folder_icon(int x, int y) {
    gfx_fill_rect(x, y + 2, 6, 2, C_FOLDER);
    gfx_fill_rect(x, y + 4, 12, 8, C_FOLDER);
    gfx_fill_rect(x + 1, y + 5, 10, 1, 0x00FFF1A8u);
}

static void draw_file_icon(int x, int y, uint32_t accent) {
    gfx_fill_rect(x + 1, y, 9, 12, 0x00D8DEE8u);
    gfx_fill_rect(x + 3, y + 3, 5, 1, accent);
    gfx_fill_rect(x + 3, y + 6, 5, 1, accent);
    gfx_fill_rect(x + 3, y + 9, 5, 1, accent);
}

static void make_thumb(int fidx) {
    drop_thumb();
    g_thumb_for = fidx;
    fs_file_t* f = fs_get_file(fidx);
    image_t img;
    char err[64];
    if (image_decode((const uint8_t*)f->content, f->size, &img, err, sizeof(err)) != 0) {
        kstrlcpy(g_thumb_err, err, sizeof(g_thumb_err));
        return;
    }
    g_thumb = (uint32_t*)kmalloc((uint32_t)THUMB_W * THUMB_H * 4);
    if (g_thumb) image_render(&img, g_thumb, THUMB_W, THUMB_H, IMAGE_FIT, C_LIST);
    image_free(&img);
}

static void blit_thumb(int x, int y) {
    int stride, tw, th;
    uint32_t* dst = fb_target(&stride, &tw, &th);
    if (!dst || !g_thumb) return;
    for (int r = 0; r < THUMB_H; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= th) continue;
        for (int c = 0; c < THUMB_W; c++) {
            int xx = x + c;
            if (xx >= 0 && xx < tw) dst[yy * stride + xx] = g_thumb[r * THUMB_W + c];
        }
    }
}

static void draw_preview(int px, int py, int ph) {
    bevel(px, py, PREV_W, ph, C_LIST, 0x0010141Cu, 0x00404B5Cu);
    int x = px + 6, y = py + 8;
    int cols = (PREV_W - 12) / 8;
    char line[64];
    if (g_sel < 0) {
        int nd = 0, nf = 0;
        uint32_t bytes = 0;
        for (int i = 0; i < g_count; i++) {
            if (g_items[i].is_dir) nd++;
            else { nf++; bytes += fs_get_file(g_items[i].idx)->size; }
        }
        draw_clip(x, y, g_path, cols, C_TEXT, C_LIST);
        ksnprintf(line, sizeof(line), "%d folders, %d files", nd, nf);
        gfx_draw_text(x, y + 18, line, C_DIM, C_LIST);
        human_size(bytes, line, sizeof(line));
        gfx_draw_text(x, y + 32, line, C_DIM, C_LIST);
        gfx_draw_text(x, y + 60, "Click to select,", C_DIM, C_LIST);
        gfx_draw_text(x, y + 74, "double-click to open.", C_DIM, C_LIST);
        return;
    }
    const item_t* it = &g_items[g_sel];
    draw_clip(x, y, item_name(it), cols, C_TEXT, C_LIST);
    const char* action;
    if (it->is_dir) {
        char path[FS_PATH_LEN];
        int d[1], f[1];
        child_path(item_name(it), path, sizeof(path));
        int n = fs_list_dirs(path, d, 1) + fs_list_files(path, f, 1);
        ksnprintf(line, sizeof(line), "Folder, %d item%s", n, n == 1 ? "" : "s");
        gfx_draw_text(x, y + 16, line, C_DIM, C_LIST);
        action = "Open";
    } else {
        fs_file_t* f = fs_get_file(it->idx);
        char size[24];
        human_size(f->size, size, sizeof(size));
        int img = is_image_file(it->idx), txt = !img && is_text_file(it->idx);
        ksnprintf(line, sizeof(line), "%s, %s", img ? "Picture" : txt ? "Text" : "Binary", size);
        gfx_draw_text(x, y + 16, line, C_DIM, C_LIST);
        action = img ? "Set as wallpaper" : "Edit";
        int top = y + 36;
        if (img) {
            if (g_thumb_for != it->idx) make_thumb(it->idx);
            if (g_thumb) blit_thumb(px + 4, top);
            else draw_clip(x, top, g_thumb_err[0] ? g_thumb_err : "no preview", cols, C_WARN, C_LIST);
        } else if (txt) {
            /* the first lines of the file */
            const char* s = f->content;
            int max_lines = (ph - 36 - 60) / 10;
            for (int ln = 0; ln < max_lines && *s; ln++) {
                int n = 0;
                while (s[n] && s[n] != '\n' && n < cols) {
                    char c = s[n];
                    line[n] = (c == '\t') ? ' ' : ((unsigned char)c < 32 ? '.' : c);
                    n++;
                }
                line[n] = '\0';
                gfx_draw_text(x, top + ln * 10, line, 0x00C8D2DEu, C_LIST);
                s += n;                             /* a long line wraps */
                if (*s == '\n') s++;
            }
        } else {
            gfx_draw_text(x, top, "(no preview for this", C_DIM, C_LIST);
            gfx_draw_text(x, top + 10, " kind of file)", C_DIM, C_LIST);
        }
    }
    int bx, by, bw;
    action_rect(&bx, &by, &bw);
    button(bx, by, bw, action, 1);
}

void explorer_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    scan();
    int x = g_x, y = g_y;
    bevel(x, y, WIN_W, WIN_H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    bevel(x + 3, y + 3, WIN_W - 6, TITLE_H - 1, C_TITLE, 0x00647692u, 0x00111923u);
    draw_folder_icon(x + 8, y + 5);
    gfx_draw_text(x + 26, y + 7, "Files", 0x00FFFFFFu, C_TITLE);
    bevel(x + WIN_W - 28, y + 4, 20, 12, 0x006D2F2Fu, 0x00A14747u, 0x00301717u);
    gfx_draw_text(x + WIN_W - 22, y + 6, "x", 0x00FFFFFFu, 0x006D2F2Fu);

    for (int i = 0; i < TOOL_COUNT; i++) {
        int enabled = (i != 3 || g_sel >= 0) && (i != 0 || strcmp(g_path, "/") != 0);
        const char* label = (i == 3 && g_confirm_delete) ? "Sure?" : TOOLS[i].label;
        button(x + TOOLS[i].x, y + 26, TOOLS[i].w, label, enabled);
    }

    /* location */
    bevel(x + LIST_X, y + 48, WIN_W - 16, 18, C_LIST, 0x0010141Cu, 0x00404B5Cu);
    draw_clip(x + LIST_X + 6, y + 53, g_path, (WIN_W - 28) / 8, C_TEXT, C_LIST);

    /* list */
    int lh = ROW_H * (ROWS + 1);
    bevel(x + LIST_X, y + LIST_Y, LIST_W, lh, C_LIST, 0x0010141Cu, 0x00404B5Cu);
    gfx_fill_rect(x + LIST_X + 1, y + LIST_Y + 1, LIST_W - 16, ROW_H - 1, 0x00252C37u);
    gfx_draw_text(x + LIST_X + 24, y + LIST_Y + 5, "Name", C_DIM, 0x00252C37u);
    gfx_draw_text(x + LIST_X + LIST_W - 94, y + LIST_Y + 5, "Size", C_DIM, 0x00252C37u);
    if (g_count == 0)
        gfx_draw_text(x + LIST_X + 24, y + LIST_Y + ROW_H + 8, "(empty folder)", C_DIM, C_LIST);
    for (int r = 0; r < ROWS && g_scroll + r < g_count; r++) {
        int i = g_scroll + r;
        const item_t* it = &g_items[i];
        int ry = y + LIST_Y + ROW_H * (r + 1);
        uint32_t bg = (i == g_sel) ? C_SEL : C_LIST;
        if (i == g_sel) gfx_fill_rect(x + LIST_X + 1, ry, LIST_W - 16, ROW_H, bg);
        if (it->is_dir) draw_folder_icon(x + LIST_X + 6, ry + 1);
        else draw_file_icon(x + LIST_X + 6, ry + 2, is_image_file(it->idx) ? 0x003C8D4Fu : 0x00607088u);
        draw_clip(x + LIST_X + 24, ry + 4, item_name(it), 27, it->is_dir ? C_FOLDER : C_TEXT, bg);
        char size[24];
        if (it->is_dir) kstrlcpy(size, "folder", sizeof(size));
        else human_size(fs_get_file(it->idx)->size, size, sizeof(size));
        gfx_draw_text(x + LIST_X + LIST_W - 94, ry + 4, size, C_DIM, bg);
    }
    /* scrollbar: top half pages up, bottom half pages down */
    int sbx = x + LIST_X + LIST_W - 14;
    gfx_fill_rect(sbx, y + LIST_Y + 1, 13, lh - 2, 0x00252C37u);
    gfx_draw_text(sbx + 3, y + LIST_Y + 4, "^", C_TEXT, 0x00252C37u);
    gfx_draw_text(sbx + 3, y + LIST_Y + lh - 12, "v", C_TEXT, 0x00252C37u);
    if (g_count > ROWS) {
        int track = lh - 32;
        int th = track * ROWS / g_count;
        if (th < 10) th = 10;
        int ty = y + LIST_Y + 16 + (track - th) * g_scroll / (g_count - ROWS);
        gfx_fill_rect(sbx + 2, ty, 9, th, 0x00596680u);
    }

    draw_preview(x + PREV_X, y + LIST_Y, lh);

    /* status line */
    char st[96];
    if (g_status[0]) kstrlcpy(st, g_status, sizeof(st));
    else ksnprintf(st, sizeof(st), "%d item%s", g_count, g_count == 1 ? "" : "s");
    draw_clip(x + LIST_X, y + WIN_H - 16, st, (WIN_W - 16) / 8,
              g_confirm_delete ? C_WARN : C_DIM, C_PANEL);
}
