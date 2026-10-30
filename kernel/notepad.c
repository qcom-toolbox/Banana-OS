/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "notepad.h"
#include "i18n.h"
#include "theme.h"
#include "keyboard.h"
#include "utf8.h"
#include "gfx.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "textbuf.h"
#include "clipboard.h"
#include "winframe.h"
#include "gui.h"
#include "filechooser.h"
#include "kbnav.h"

/*
 * Notepad: a textbuf_t (kernel/textbuf.c) in a window. Click to place the
 * cursor, drag to select, double-click selects a word; Ctrl+A/C/X/V,
 * Ctrl+S/O/N, Ctrl+F (find) and F3-like Ctrl+G (find next); scrollbars.
 */

#define TITLE_H  20
#define TOOL_Y   23
#define TOOL_H   20
#define TEXT_Y   (TOOL_Y + TOOL_H + 4)
#define LINE_H   (gfx_cell_h() + 3)     /* the text size's cells (Settings > Fonts) */
#define CHAR_W   gfx_cell_w()
#define SB       12
#define STATUS_H 16

#define C_PANEL  TC(0x1D232C)
#define C_TITLE  TC(0x384562)
#define C_TEXT   TC(0xE8EEF6)
#define C_DIM    TC(0xAAB6C6)
#define C_PAGE   0x00FCFCF8u
#define C_INK    0x00181818u
#define C_SELBG  0x00A8C8F0u
#define C_ERR    TC(0xF08070)

enum { P_NONE = 0, P_OPEN, P_SAVEAS, P_FIND, P_CLOSE };

static int        g_open;
static win_geom_t g_win = { .x = 120, .y = 40, .w = 560, .h = 420, .min_w = 300, .min_h = 180 };
static textbuf_t  g_tb;
static int        g_tb_ready;
static char       g_path[FS_PATH_LEN];
static int        g_top, g_left;             /* first visible line / column */
static uint32_t   g_gen;
static int        g_selecting;               /* mouse drag selection */
static uint32_t   g_click_ms;
static int        g_click_x = -1, g_click_y = -1;
static int        g_sb_drag, g_sb_dy;
static int        g_esc;                     /* ESC sequence state */
static char       g_esc_digit;
static char       g_status[96];
static int        g_status_err;
static int        g_prompt;
static char       g_input[FS_PATH_LEN];
static char       g_find[64];
static int        g_after_save;              /* P_CLOSE: close once saved */
static int        g_confirm_new;             /* New pressed once with unsaved changes */
static int        chooser_on(void);

static void new_text(void);
static void status(const char* s, int err);

/* New: with unsaved changes, only the second press throws them away */
static void cmd_new(void) {
    if (g_tb.dirty && !g_confirm_new) {
        status("Unsaved changes: Save first, or New again to discard them", 1);
        g_confirm_new = 1;
        return;
    }
    g_confirm_new = 0;
    new_text();
}

static const struct { int x, w; const char* label; } TOOLS[] = {
    { 4, 36, "New" }, { 42, 44, "Open" }, { 88, 44, "Save" }, { 134, 64, "Save as" },
    { 204, 36, "Cut" }, { 242, 44, "Copy" }, { 288, 48, "Paste" }, { 342, 44, "Find" },
};
#define TOOL_COUNT (int)(sizeof(TOOLS) / sizeof(TOOLS[0]))

/* ── geometry ─────────────────────────────────────────────────────── */

static int text_w(void) { return g_win.w - 8 - SB; }
static int text_h(void) { return g_win.h - TEXT_Y - STATUS_H - 4 - SB; }
static int vis_rows(void) { int r = text_h() / LINE_H; return r < 1 ? 1 : r; }
static int vis_cols(void) { int c = (text_w() - 6) / CHAR_W; return c < 1 ? 1 : c; }

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void status(const char* s, int err) {
    kstrlcpy(g_status, s, sizeof(g_status));
    g_status_err = err;
    g_gen++;
}

static int longest_line(void) {
    int m = 0;
    for (int i = 0; i < g_tb.n; i++) if ((int)g_tb.lines[i].len > m) m = (int)g_tb.lines[i].len;
    return m;
}

/* scrolls so the cursor is visible */
static void follow_cursor(void) {
    int rows = vis_rows(), cols = vis_cols();
    if (g_tb.cy < g_top) g_top = g_tb.cy;
    if (g_tb.cy >= g_top + rows) g_top = g_tb.cy - rows + 1;
    int cc = tb_col(&g_tb, g_tb.cy, g_tb.cx);
    if (cc < g_left) g_left = cc;
    if (cc >= g_left + cols) g_left = cc - cols + 1;
    if (g_top < 0) g_top = 0;
    if (g_left < 0) g_left = 0;
}

static void clamp_scroll(void);

void notepad_wheel(int mx, int my, int dz) {
    (void)mx; (void)my;
    if (chooser_on()) { fc_wheel(dz); g_gen++; return; }   /* (the Open / Save As list) */
    g_top += dz * 3;
    clamp_scroll();
    g_gen++;
}

static void clamp_scroll(void) {
    int max_top = g_tb.n - vis_rows();
    if (g_top > max_top) g_top = max_top;
    if (g_top < 0) g_top = 0;
    int max_left = longest_line() - vis_cols() + 1;
    if (g_left > max_left) g_left = max_left;
    if (g_left < 0) g_left = 0;
}

/* ── files ────────────────────────────────────────────────────────── */

static void full_path(const char* in, char* out, int cap) {
    while (*in == ' ') in++;
    if (in[0] == '/') kstrlcpy(out, in, (size_t)cap);
    else if (in[0] == '~') ksnprintf(out, (size_t)cap, "/home/banana%s", in + 1);
    else ksnprintf(out, (size_t)cap, "/home/banana/%s", in);
}

static void new_text(void) {
    if (tb_load(&g_tb, "", 0) != 0) { status("Out of memory - text unchanged", 1); return; }
    g_tb_ready = 1;
    g_path[0] = 0;
    g_top = g_left = 0;
    status("New text", 0);
}

static int load(const char* path) {
    int idx = fs_find_file(path);
    char msg[96];
    if (idx < 0) {
        if (fs_find_dir(path) >= 0) { status("That is a folder", 1); return -1; }
        /* a new file: it is created by the first save */
        if (tb_load(&g_tb, "", 0) != 0) { status("Out of memory - text unchanged", 1); return -1; }
        g_tb_ready = 1;
        kstrlcpy(g_path, path, sizeof(g_path));
        g_top = g_left = 0;
        status("New file (saved on Ctrl+S)", 0);
        return 0;
    }
    fs_pin(idx);
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->loaded || !f->content) {
        fs_unpin(idx);
        status("Could not read file - text unchanged", 1);
        return -1;
    }
    if (fs_is_binary(idx)) { fs_unpin(idx); status("Not a text file (a picture or a download?)", 1); return -1; }
    uint32_t size = f->size;
    int rc = tb_load(&g_tb, f->content, size);
    fs_unpin(idx);
    if (rc != 0) { status("Out of memory or too many lines - text unchanged", 1); return -1; }
    g_tb_ready = 1;
    kstrlcpy(g_path, path, sizeof(g_path));
    g_top = g_left = 0;
    ksnprintf(msg, sizeof(msg), "Opened %s (%u bytes)", path, size);
    status(msg, 0);
    return 0;
}

static int save_to(const char* path) {
    uint32_t len;
    char* text = tb_text(&g_tb, &len);
    if (!text) { status("Out of memory - not saved", 1); return -1; }
    int idx = fs_find_file(path);
    if (idx < 0) idx = fs_create(path);
    int rc = idx >= 0 ? fs_write(idx, text, len) : -1;
    kfree(text);
    if (rc != 0) { status("Could not save (bad name, missing folder, or out of space)", 1); return -1; }
    kstrlcpy(g_path, path, sizeof(g_path));
    g_tb.dirty = 0;
    char msg[96];
    ksnprintf(msg, sizeof(msg), "Saved %u bytes to %s", len, path);
    status(msg, 0);
    return 0;
}

static void start_prompt(int kind, const char* initial) {
    g_prompt = kind;
    kstrlcpy(g_input, initial ? initial : "", sizeof(g_input));
    g_gen++;
}

/* ── Open / Save As: the file chooser (kernel/filechooser.c) over the text,
 *    like Windows 7's dialogs ── */

static int g_fc;                             /* the chooser open is Notepad's */

static void np_open_done(const char* path) { g_fc = 0; load(path); g_gen++; }

static void np_save_done(const char* path) {
    g_fc = 0;
    if (save_to(path) == 0 && g_after_save) { g_after_save = 0; notepad_close(); }
    g_gen++;
}

/* where the dialogs start: the open file's folder, else Documents */
static void start_dir(char* out, int cap) {
    if (g_path[0]) {
        kstrlcpy(out, g_path, (size_t)cap);
        char* s = strrchr(out, '/');
        if (s && s != out) { *s = 0; return; }
    }
    kstrlcpy(out, fs_find_dir("/home/banana/Documents") >= 0 ? "/home/banana/Documents" : "/home/banana", (size_t)cap);
}

static void open_dialog(void) {
    char dir[FS_PATH_LEN];
    start_dir(dir, sizeof(dir));
    g_after_save = 0;
    g_fc = 1;
    fc_open(_("Open"), dir, NULL, np_open_done);
    g_gen++;
}

static void save_dialog(void) {
    char dir[FS_PATH_LEN];
    start_dir(dir, sizeof(dir));
    const char* name = g_path[0] && strrchr(g_path, '/') ? strrchr(g_path, '/') + 1 : "Untitled.txt";
    g_fc = 1;
    fc_save("Save As", dir, name, np_save_done);
    g_gen++;
}

/* the chooser is open over the text (and is Notepad's) */
static int chooser_on(void) {
    if (g_fc && !fc_active()) g_fc = 0;          /* (closed: Cancel, Esc) */
    return g_fc;
}

static void save(void) {
    if (!g_path[0]) { save_dialog(); return; }
    save_to(g_path);
}

/* ── editing commands ─────────────────────────────────────────────── */

static void copy(void) {
    uint32_t n;
    char* s = tb_sel_text(&g_tb, &n);
    if (!s) {                                   /* nothing selected: the current line */
        tb_line_t* l = &g_tb.lines[g_tb.cy];
        clipboard_set(l->s, l->len);
        status("Copied the line", 0);
        return;
    }
    clipboard_set(s, n);
    kfree(s);
    status("Copied", 0);
}

static void cut(void) {
    uint32_t n;
    char* s = tb_sel_text(&g_tb, &n);
    if (s) {
        clipboard_set(s, n);
        kfree(s);
        tb_sel_delete(&g_tb);
    } else {
        s = tb_cut_line(&g_tb, &n);
        if (s) { clipboard_set(s, n); kfree(s); }
    }
    follow_cursor();
    g_gen++;
}

void notepad_paste(void) {
    if (!g_open) return;
    uint32_t n;
    const char* clip = clipboard_get(&n);
    if (g_prompt) {                                     /* into the prompt field */
        size_t l = strlen(g_input);
        for (uint32_t i = 0; i < n && l < sizeof(g_input) - 1; i++)
            if ((unsigned char)clip[i] >= 32) g_input[l++] = clip[i];
        g_input[l] = 0;
    } else {
        tb_insert(&g_tb, clip, n);
        follow_cursor();
    }
    g_gen++;
}

static void find_next(void) {
    if (!g_find[0]) { start_prompt(P_FIND, ""); return; }
    if (tb_find(&g_tb, g_find)) { follow_cursor(); status("", 0); }
    else {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "\"%s\" not found", g_find);
        status(msg, 1);
    }
}

static void request_close(void) {
    if (g_tb.dirty) { start_prompt(P_CLOSE, ""); status("", 0); return; }   /* (the box asks: draw_close_box) */
    notepad_close();
}

/* "Do you want to save changes to ...?" - Save / Don't Save / Cancel, in
 * the middle of the window, as Windows 7's Notepad asks */
#define CB_W 380
#define CB_H 108
static void close_box(int* bx, int* by, int* bw) {
    *bw = g_win.w - 40 < CB_W ? g_win.w - 40 : CB_W;
    *bx = g_win.x + (g_win.w - *bw) / 2;
    *by = g_win.y + (g_win.h - CB_H) / 2;
}

/* its buttons, right to left: Cancel, Don't Save, Save (0, 1, 2), or -1 */
static int close_box_hit(int mx, int my) {
    int bx, by, bw;
    close_box(&bx, &by, &bw);
    if (my < by + CB_H - 32 || my >= by + CB_H - 10) return -1;
    for (int i = 0; i < 3; i++) {
        int x = bx + bw - 12 - (i + 1) * 100 + 8;
        if (mx >= x && mx < x + 92) return i;
    }
    return -1;
}

static void close_choose(int k) {
    g_prompt = P_NONE;
    if (k == 2) {                                          /* Save (and close once saved) */
        if (!g_path[0]) { save_dialog(); g_after_save = 1; }
        else if (save_to(g_path) == 0) notepad_close();
    } else if (k == 1) { g_tb.dirty = 0; notepad_close(); }   /* Don't Save */
    g_gen++;
}

static void draw_close_box(void) {
    int bx, by, bw;
    close_box(&bx, &by, &bw);
    gfx_fill_rect(bx + 4, by + 4, bw, CB_H, TC(0x080A0E));
    gfx_fill_rect(bx, by, bw, CB_H, TC(0x262C36));
    gfx_fill_rect(bx, by, bw, 20, TC(0x304868));
    gfx_draw_text(bx + 8, by + 6, _("Notepad"), TC(0xFFFFFF), TC(0x304868));
    char line[96];
    const char* name = g_path[0] && strrchr(g_path, '/') ? strrchr(g_path, '/') + 1 : "Untitled";
    ksnprintf(line, sizeof(line), "Do you want to save changes to %s?", name);
    int ml = (bw - 24) / 8;
    if (ml > 0 && ml < (int)sizeof(line) && (int)strlen(line) > ml) { line[ml - 1] = '?'; line[ml] = 0; }
    gfx_draw_text(bx + 12, by + 36, line, TC(0xE8EEF6), TC(0x262C36));
    static const char* const B[3] = { "Cancel", "Don't Save", "Save" };
    for (int i = 0; i < 3; i++) {
        int x = bx + bw - 12 - (i + 1) * 100 + 8, y = by + CB_H - 32;
        uint32_t base = i == 2 ? TC(0x3A7BD5) : TC(0x303740);
        gfx_fill_rect(x, y, 92, 22, base);
        gfx_fill_rect(x, y, 92, 1, TC(0x535D6E));
        gfx_draw_text(x + (92 - (int)strlen(B[i]) * 8) / 2, y + 7, B[i], TC(0xFFFFFF), base);
    }
}

static void prompt_done(int ok) {
    int kind = g_prompt;
    g_prompt = P_NONE;
    g_gen++;
    if (!ok) { status("", 0); return; }
    char path[FS_PATH_LEN];
    switch (kind) {
    case P_OPEN:
        if (!g_input[0]) return;
        full_path(g_input, path, sizeof(path));
        load(path);
        break;
    case P_SAVEAS:
        if (!g_input[0]) return;
        full_path(g_input, path, sizeof(path));
        if (save_to(path) == 0 && g_after_save) { g_after_save = 0; notepad_close(); }
        break;
    case P_FIND:
        kstrlcpy(g_find, g_input, sizeof(g_find));
        find_next();
        break;
    }
}

/* ── window ───────────────────────────────────────────────────────── */

void notepad_open(const char* path) {
    if (!g_tb_ready) {
        if (tb_load(&g_tb, "", 0) != 0) { status("Out of memory - cannot open editor", 1); return; }
        g_tb_ready = 1;
    }
    if (path && *path) {
        if (g_open && g_tb.dirty && strcmp(path, g_path) != 0) {
            /* keep the unsaved text: ask first */
            status("Save or close the current text first (Ctrl+S)", 1);
        } else {
            char full[FS_PATH_LEN];
            full_path(path, full, sizeof(full));
            load(full);
        }
    } else if (!g_open && !g_path[0] && !g_tb.dirty) {
        status("Ctrl+O open, Ctrl+S save, Ctrl+F find, Ctrl+A/C/X/V, right-click pastes", 0);
    }
    g_open = 1;
    win_clamp(&g_win);
    g_gen++;
}

void notepad_close(void) {
    g_open = 0;
    g_prompt = P_NONE;
    g_selecting = 0;
    g_win.dragging = g_win.resizing = 0;
    if (g_tb_ready && !g_tb.dirty) {          /* nothing to lose: free the memory */
        tb_free(&g_tb);
        g_tb_ready = 0;
        g_path[0] = 0;
    }
    g_gen++;
}

int notepad_is_open(void) { return g_open; }

int notepad_contains(int mx, int my) {
    return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h);
}

uint32_t notepad_signature(void) {
    if (!g_open) return 0;
    /* the caret blinks while Notepad has the keyboard */
    uint32_t blink = gui_notepad_focused() ? (timer_ms() / 500) & 1 : 0;
    return g_gen * 2654435761u ^ (uint32_t)((uint32_t)g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^ blink ^
           (g_fc ? fc_generation() * 40503u : 0);       /* (the Open / Save As dialog) */
}

/* text position under the mouse */
static void pos_at(int mx, int my, int* x, int* y) {
    int row = (my - (g_win.y + TEXT_Y + 2)) / LINE_H;
    int col = (mx - (g_win.x + 4 + 3) + CHAR_W / 2) / CHAR_W;
    if (my < g_win.y + TEXT_Y + 2) row = -1;
    *y = g_top + row;
    *x = g_left + (col < 0 ? 0 : col);
    if (*y < 0) { *y = 0; *x = 0; }
    if (*y >= g_tb.n) { *y = g_tb.n - 1; *x = 1 << 28; }
    else *x = tb_byte(&g_tb, *y, *x);         /* the column's character */
}

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 128;
}

static void sb_geom(int* ty, int* th) {
    int track = text_h();
    int total = g_tb.n > vis_rows() ? g_tb.n : vis_rows();
    *th = track * vis_rows() / total;
    if (*th < 16) *th = 16;
    int range = g_tb.n - vis_rows();
    *ty = range > 0 ? (track - *th) * g_top / range : 0;
}

void notepad_click(int mx, int my) {
    if (!notepad_contains(mx, my)) return;
    int lx = mx - g_win.x, ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { request_close(); return; }
        if (b) { clamp_scroll(); return; }
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    if (chooser_on()) { fc_click(mx, my); return; }     /* Open / Save As */
    if (g_prompt == P_CLOSE) { int k = close_box_hit(mx, my); if (k == 0) { g_prompt = P_NONE; g_gen++; } else if (k > 0) close_choose(k); return; }
    if (ly >= TOOL_Y && ly < TOOL_Y + TOOL_H) {
        for (int i = 0; i < TOOL_COUNT; i++) {
            if (lx < TOOLS[i].x || lx >= TOOLS[i].x + TOOLS[i].w) continue;
            switch (i) {
            case 0: cmd_new(); break;
            case 1: open_dialog(); break;
            case 2: save(); break;
            case 3: save_dialog(); break;
            case 4: cut(); break;
            case 5: copy(); break;
            case 6: notepad_paste(); break;
            case 7: start_prompt(P_FIND, g_find); break;
            }
            return;
        }
        return;
    }
    /* vertical scrollbar */
    int sbx = 4 + text_w();
    if (inside(lx, ly, sbx, TEXT_Y, SB, text_h())) {
        int ty, th;
        sb_geom(&ty, &th);
        int rel = ly - TEXT_Y;
        if (rel >= ty && rel < ty + th) { g_sb_drag = 1; g_sb_dy = rel - ty; }
        else g_top += rel < ty ? -(vis_rows() - 1) : vis_rows() - 1;
        clamp_scroll();
        return;
    }
    /* horizontal scrollbar */
    if (inside(lx, ly, 4, TEXT_Y + text_h(), text_w(), SB)) {
        g_left += lx < 4 + text_w() / 2 ? -(vis_cols() / 2) : vis_cols() / 2;
        clamp_scroll();
        return;
    }
    /* text */
    if (inside(lx, ly, 4, TEXT_Y, text_w(), text_h())) {
        if (g_prompt && g_prompt != P_CLOSE) { g_prompt = P_NONE; }
        int x, y;
        pos_at(mx, my, &x, &y);
        uint32_t now = timer_ms();
        if (now - g_click_ms < 400 && y == g_click_y && (x == g_click_x || x == g_click_x + 1 || x == g_click_x - 1)) {
            /* double-click: the word */
            tb_line_t* l = &g_tb.lines[y];
            int a = x > (int)l->len ? (int)l->len : x, b = a;
            while (a > 0 && is_word(l->s[a - 1])) a--;
            while (b < (int)l->len && is_word(l->s[b])) b++;
            tb_goto(&g_tb, a, y, 0);
            tb_goto(&g_tb, b, y, 1);
            g_click_ms = 0;
            return;
        }
        g_click_ms = now;
        g_click_x = x;
        g_click_y = y;
        tb_goto(&g_tb, x, y, 0);
        g_tb.sel = 1;                          /* an anchor for dragging */
        g_tb.ax = g_tb.cx;
        g_tb.ay = g_tb.cy;
        g_selecting = 1;
    }
}

void notepad_mouse(int mx, int my, int left) {
    if (!g_open) return;
    if (win_mouse(&g_win, mx, my, left)) { clamp_scroll(); g_gen++; }
    if (!left) { g_selecting = 0; g_sb_drag = 0; return; }
    if (g_sb_drag) {
        int ty, th;
        sb_geom(&ty, &th);
        int track = text_h() - th, range = g_tb.n - vis_rows();
        if (track > 0 && range > 0) {
            int nt = (my - g_win.y - TEXT_Y - g_sb_dy) * range / track;
            if (nt != g_top) { g_top = nt; clamp_scroll(); g_gen++; }
        }
        return;
    }
    if (g_selecting) {
        int x, y;
        pos_at(mx, my, &x, &y);
        /* dragging past the edges scrolls */
        if (my < g_win.y + TEXT_Y && g_top > 0) g_top--;
        if (my > g_win.y + TEXT_Y + text_h() && g_top < g_tb.n - vis_rows()) g_top++;
        int ocx = g_tb.cx, ocy = g_tb.cy;
        tb_goto(&g_tb, x, y, 1);
        if (g_tb.cx != ocx || g_tb.cy != ocy) { follow_cursor(); g_gen++; }
    }
}

/* ── keyboard ─────────────────────────────────────────────────────── */

static void key_motion(char k) {
    int page = vis_rows() - 1;
    switch (k) {
    case 'A': tb_move(&g_tb, 0, -1, 0); break;
    case 'B': tb_move(&g_tb, 0, 1, 0); break;
    case 'C': tb_move(&g_tb, 1, 0, 0); break;
    case 'D': tb_move(&g_tb, -1, 0, 0); break;
    case 'H': tb_home(&g_tb, 0); break;
    case 'F': tb_end(&g_tb, 0); break;
    case 'I': tb_move(&g_tb, 0, -page, 0); g_top -= page; break;
    case 'G': tb_move(&g_tb, 0, page, 0); g_top += page; break;
    case 'P': tb_delete(&g_tb); break;
    }
    clamp_scroll();
    follow_cursor();
}

static void prompt_key(char c) {
    size_t n = strlen(g_input);
    if (g_prompt == P_CLOSE) {                     /* Enter / s / y: Save, n: Don't Save, Esc: Cancel */
        if (c == '\n' || c == 's' || c == 'S' || c == 'y' || c == 'Y') close_choose(2);
        else if (c == 'n' || c == 'N') close_choose(1);
        else if (c == 27) prompt_done(0);
        return;
    }
    if (c == '\n') prompt_done(1);
    else if (c == 27) prompt_done(0);
    else if (c == '\b') { if (n) u8_backspace(g_input, (int)n); }
    else if (c == 22) notepad_paste();
    else if ((unsigned char)c >= 32 && n < sizeof(g_input) - 1) { g_input[n] = c; g_input[n + 1] = 0; }
}

void notepad_key(char c) {
    if (!g_open) return;
    g_gen++;
    /* a dialog (Open / Save As, or "save the changes?") has the keys; an
     * arrow's ESC [ X comes in at once, so a lone Esc is known right away */
    if (chooser_on() || g_prompt == P_CLOSE) {
        g_esc = 0;
        if (c == 27) {
            char c2 = keyboard_try_getchar();
            if (c2 == '[') {
                char c3 = keyboard_try_getchar();
                if (c3 >= '0' && c3 <= '9') { keyboard_try_getchar(); c3 = c3 == '5' ? 'I' : c3 == '6' ? 'G' : 0; }
                int code = c3 == 'A' ? KB_UP : c3 == 'B' ? KB_DOWN : c3 == 'C' ? KB_RIGHT : c3 == 'D' ? KB_LEFT :
                           c3 == 'H' ? KB_HOME : c3 == 'F' ? KB_END : c3 == 'I' ? KB_PGUP : c3 == 'G' ? KB_PGDN : 0;
                if (g_fc && code) fc_nav(code);
                return;
            }
            if (g_fc) fc_key(27); else prompt_key(27);
            return;
        }
        if (g_fc) fc_key(c); else prompt_key(c);
        return;
    }
    /* ESC [ X (ours) or ESC [ n ~ (xterm, over SSH never reaches here but cheap) */
    if (g_esc == 1) {
        if (c == '[') { g_esc = 2; return; }
        g_esc = 0;
        if (g_prompt) prompt_key(27);
        else g_tb.sel = 0;
        /* fall through: c is an ordinary key */
    } else if (g_esc == 2) {
        if (c >= '0' && c <= '9') { g_esc_digit = c; g_esc = 3; return; }
        g_esc = 0;
        if (!g_prompt) key_motion(c);
        return;
    } else if (g_esc == 3) {
        g_esc = 0;
        static const char map[] = { 0, 'H', 0, 'P', 'F', 'I', 'G', 'H', 'F', 0 };
        if (!g_prompt && map[g_esc_digit - '0']) key_motion(map[g_esc_digit - '0']);
        return;
    }
    if (c == 27) { g_esc = 1; return; }
    if (g_prompt) { prompt_key(c); return; }
    if (c != 14) g_confirm_new = 0;
    g_status[0] = 0;                 /* the last message goes away with the next key */

    switch (c) {
    case 1:  tb_select_all(&g_tb); return;                         /* Ctrl+A */
    case 3:  copy(); return;                                       /* Ctrl+C */
    case 24: cut(); return;                                        /* Ctrl+X */
    case 22: notepad_paste(); return;                              /* Ctrl+V */
    case 19: save(); return;                                       /* Ctrl+S */
    case 15: open_dialog(); return;                                /* Ctrl+O */
    case 14: cmd_new(); return;                                    /* Ctrl+N */
    case 6:  start_prompt(P_FIND, g_find); return;                 /* Ctrl+F */
    case 7:  find_next(); return;                                  /* Ctrl+G */
    case 23: request_close(); return;                              /* Ctrl+W */
    case '\t': tb_insert(&g_tb, "    ", 4); break;
    case '\b': tb_backspace(&g_tb); break;
    case '\n': tb_insert(&g_tb, "\n", 1); break;
    default:
        if ((unsigned char)c < 32) return;
        tb_insert(&g_tb, &c, 1);
    }
    follow_cursor();
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void draw_clip(int x, int y, const char* s, int max, uint32_t fg, uint32_t bg) {
    char buf[128];
    if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;
    if (max < 1) return;
    int n = (int)strlen(s);
    if (n > max) { memcpy(buf, s, (size_t)max); buf[max] = 0; buf[max - 1] = '>'; s = buf; }
    gfx_draw_text(x, y, s, fg, bg);
}

void notepad_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, w = g_win.w, h = g_win.h;
    int focused = gui_notepad_focused();
    bevel(x, y, w, h, C_PANEL, TC(0x505D72), TC(0x10141C));
    uint32_t tbg = focused ? C_TITLE : TC(0x2A3240);
    bevel(x + 3, y + 3, w - 6, TITLE_H - 1, tbg, TC(0x647692), TC(0x111923));
    char title[128];
    const char* name = g_path[0] ? strrchr(g_path, '/') + 1 : "Untitled";
    ksnprintf(title, sizeof(title), "%s%s - Notepad", g_tb.dirty ? "*" : "", name);
    draw_clip(x + 10, y + 7, title, (w - 50) / 8, TC(0xFFFFFF), tbg);
    win_draw_buttons(&g_win, 4, 12);

    for (int i = 0; i < TOOL_COUNT; i++) {
        if (TOOLS[i].x + TOOLS[i].w > w - 6) break;
        bevel(x + TOOLS[i].x, y + TOOL_Y, TOOLS[i].w, TOOL_H - 2, TC(0x303740), TC(0x535D6E), TC(0x15191F));
        gfx_draw_text(x + TOOLS[i].x + (TOOLS[i].w - (int)strlen(_(TOOLS[i].label)) * 8) / 2, y + TOOL_Y + 5,
                      _(TOOLS[i].label), C_TEXT, TC(0x303740));
    }

    /* the page */
    int px = x + 4, py = y + TEXT_Y, pw = text_w(), ph = text_h();
    gfx_fill_rect(px, py, pw, ph, C_PAGE);
    int rows = vis_rows(), cols = vis_cols();
    int sx0, sy0, sx1, sy1;
    int has_sel = tb_sel_bounds(&g_tb, &sx0, &sy0, &sx1, &sy1);
    for (int r = 0; r < rows; r++) {
        int li = g_top + r;
        if (li >= g_tb.n) break;
        tb_line_t* l = &g_tb.lines[li];
        int ty = py + 2 + r * LINE_H;
        int xi = u8_byte_at_col(l->s, (int)l->len, g_left);
        int cidx = u8_width(l->s, xi) - g_left;
        if (cidx < 0) { xi = u8_next(l->s, (int)l->len, xi); cidx = 1; }  /* (half a wide character scrolled off) */
        while (cidx < cols) {
            int in = has_sel && (li > sy0 || (li == sy0 && xi >= sx0)) && (li < sy1 || (li == sy1 && xi < sx1));
            /* the selection covers the line break too */
            if (xi >= (int)l->len) {
                if (in && xi == (int)l->len && li < sy1) gfx_fill_rect(px + 3 + cidx * CHAR_W, ty - 1, CHAR_W / 2, LINE_H, C_SELBG);
                break;
            }
            uint32_t cp = u8_decode(l->s, (int)l->len, &xi);    /* (xi moves to the next one) */
            uint32_t ch = (cp == '\t' || cp < 32) ? ' ' : cp;
            int cw = gfx_char_cols(ch);                          /* (an emoji or CJK character: two cells) */
            if (!cw) continue;
            uint32_t bg = in ? C_SELBG : C_PAGE;
            if (in) gfx_fill_rect(px + 3 + cidx * CHAR_W, ty - 1, CHAR_W * cw, LINE_H, bg);
            if (ch != ' ') gfx_draw_cell_cp(px + 3 + cidx * CHAR_W, ty, ch, C_INK, bg);   /* (any character: kernel/gfx.c) */
            cidx += cw;
        }
    }
    /* caret */
    if (focused && !g_prompt && (timer_ms() / 500) % 2 == 0) {
        int cr = g_tb.cy - g_top, cc = tb_col(&g_tb, g_tb.cy, g_tb.cx) - g_left;
        if (cr >= 0 && cr < rows && cc >= 0 && cc <= cols)
            gfx_fill_rect(px + 3 + cc * CHAR_W - 1, py + 1 + cr * LINE_H, 2, LINE_H, TC(0x202060));
    }

    /* scrollbars */
    gfx_fill_rect(px + pw, py, SB, ph, TC(0x2A3038));
    int ty, th;
    sb_geom(&ty, &th);
    bevel(px + pw + 1, py + ty, SB - 2, th, TC(0x596678), TC(0x7A889C), TC(0x303844));
    gfx_fill_rect(px, py + ph, pw, SB, TC(0x2A3038));
    int maxw = longest_line() + 1;
    if (maxw > cols) {
        int tw = pw * cols / maxw;
        if (tw < 16) tw = 16;
        int tx = (pw - tw) * g_left / (maxw - cols);
        bevel(px + tx, py + ph + 1, tw, SB - 2, TC(0x596678), TC(0x7A889C), TC(0x303844));
    }

    /* status line, or the prompt */
    int sy = y + h - STATUS_H - 2;
    gfx_fill_rect(x + 3, sy, w - 6, STATUS_H, TC(0x161B22));
    char line[160];
    if (g_prompt == P_OPEN || g_prompt == P_SAVEAS || g_prompt == P_FIND) {
        const char* label = g_prompt == P_OPEN ? "Open: " : g_prompt == P_SAVEAS ? "Save as: " : "Find: ";
        ksnprintf(line, sizeof(line), "%s%s_   (Enter = OK, Esc = cancel)", label, g_input);
        draw_clip(x + 8, sy + 4, line, (w - 24) / 8, TC(0xFFE08A), TC(0x161B22));
    } else if (g_status[0]) {
        draw_clip(x + 8, sy + 4, g_status, (w - 24) / 8, g_status_err ? C_ERR : C_DIM, TC(0x161B22));
    } else {
        ksnprintf(line, sizeof(line), "Ln %d, Col %d   %d lines   %s", g_tb.cy + 1, tb_col(&g_tb, g_tb.cy, g_tb.cx) + 1, g_tb.n,
                  g_path[0] ? g_path : "(not saved yet)");
        draw_clip(x + 8, sy + 4, line, (w - 24) / 8, C_DIM, TC(0x161B22));
    }
    gfx_draw_grip(x + w, y + h);
    /* the dialogs, over the text */
    if (chooser_on()) fc_draw(x + 6, y + TOOL_Y, w - 12, h - TOOL_Y - STATUS_H - 10);   /* (the status line stays) */
    if (g_prompt == P_CLOSE) draw_close_box();
}

void notepad_fkey(int k) {
    if (k == KEYF_F1 + 2) find_next();                    /* F3, like Ctrl+G */
}

/* the window manager (kernel/gui.c) moves, snaps and maximizes it through its geometry */
win_geom_t* notepad_geom(void) { return &g_win; }
