#include "editor.h"
#include "../kernel/utf8.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/fs.h"
#include "../kernel/gui.h"
#include "../kernel/timer.h"
#include "../kernel/task.h"
#include "../kernel/types.h"
#include "../kernel/textbuf.h"
#include "../kernel/clipboard.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"

/*
 * `edit`: a nano-like editor in the terminal. The text lives in a
 * textbuf_t (kernel/textbuf.c - no limit on lines or their length); the
 * screen follows the window's size, long lines scroll sideways.
 *
 * One editor state per vt (kernel/terminal.h): each GUI terminal window
 * can have its own "edit" session open at the same time.
 */

typedef struct {
    textbuf_t tb;
    int  scroll, hscroll;
    char filename[FS_PATH_LEN];
    char* cut;                  /* Ctrl+K lines, for Ctrl+U */
    uint32_t cut_len;
    char msg[80];               /* one-shot status message */
} editor_t;

static editor_t g_ed[TERMINAL_VT_MAX];

static int text_rows(void) {
    int h = (int)terminal_get_height() - 3;
    return h < 1 ? 1 : h;
}

static int width(void) {
    int w = (int)terminal_get_width();
    return w < 20 ? 20 : w;
}

/* a full-width bar (never the last column: that would wrap) */
static void bar(const char* s) {
    int w = width() - 1, n = 0;
    terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    for (; s[n] && n < w; n++) terminal_putchar(s[n]);
    for (; n < w; n++) terminal_putchar(' ');
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
}

static void fit_view(editor_t* e) {
    int rows = text_rows(), w = width() - 1;
    textbuf_t* t = &e->tb;
    if (t->cy < e->scroll) e->scroll = t->cy;
    if (t->cy >= e->scroll + rows) e->scroll = t->cy - rows + 1;
    int cc = tb_col(t, t->cy, t->cx);
    if (cc < e->hscroll) e->hscroll = cc;
    if (cc >= e->hscroll + w) e->hscroll = cc - w + 1;
}

static void draw(editor_t* e) {
    textbuf_t* t = &e->tb;
    fit_view(e);
    terminal_clear();
    int w = width() - 1, rows = text_rows();

    char line[160];
    ksnprintf(line, sizeof(line), "  Banana nano 0.2   %s%s", e->filename, t->dirty ? "  [Modified]" : "");
    bar(line);
    terminal_putchar('\n');

    int sx0, sy0, sx1, sy1;
    int has_sel = tb_sel_bounds(t, &sx0, &sy0, &sx1, &sy1);
    for (int r = 0; r < rows; r++) {
        int y = e->scroll + r;
        if (y < t->n) {
            tb_line_t* l = &t->lines[y];
            int x = u8_byte_at(l->s, (int)l->len, e->hscroll);
            for (int col = e->hscroll; x < (int)l->len && col < e->hscroll + w; col++) {
                int nx = u8_next(l->s, (int)l->len, x);
                int in = has_sel && (y > sy0 || (y == sy0 && x >= sx0)) && (y < sy1 || (y == sy1 && x < sx1));
                if (in) terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
                if (nx - x > 1) {
                    for (int k = x; k < nx; k++) terminal_putchar(l->s[k]);   /* one character */
                } else {
                    char c = l->s[x];
                    if (c == '\t' || (unsigned char)c < 32) c = ' ';
                    terminal_putchar(c);
                }
                x = nx;
                if (in) terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            }
        }
        terminal_putchar('\n');
    }

    if (e->msg[0]) {
        ksnprintf(line, sizeof(line), "  %s", e->msg);
        e->msg[0] = 0;
    } else {
        ksnprintf(line, sizeof(line), "  Line %d/%d, Col %d", t->cy + 1, t->n, tb_col(t, t->cy, t->cx) + 1);
    }
    bar(line);
    terminal_putchar('\n');
    bar("^X Exit ^S Save ^K Cut line ^U Paste line ^V Paste ^C Copy line ^W Find");

    terminal_set_cursor((size_t)(1 + t->cy - e->scroll), (size_t)(tb_col(t, t->cy, t->cx) - e->hscroll));
}

/* Cooperative wait for the next keystroke aimed at this window: keeps the
 * GUI compositor running and this editor's vt bound as the write target,
 * but only consumes a key once this vt actually has keyboard focus - the
 * same rule shell_readline() follows, so an "edit" running unfocused in
 * the background can't steal keys meant for whatever window is focused.
 * A window resize redraws. */
static char editor_wait_key(editor_t* e, int my_vt) {
    size_t w0 = terminal_get_width(), h0 = terminal_get_height();
    while (1) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (e && (terminal_get_width() != w0 || terminal_get_height() != h0)) {
            w0 = terminal_get_width();
            h0 = terminal_get_height();
            draw(e);
        }
        if (gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (c) return c;
        task_sleep_ms(10);
    }
}

static int save(editor_t* e) {
    uint32_t len;
    char* text = tb_text(&e->tb, &len);
    if (!text) { kstrlcpy(e->msg, "Out of memory - not saved", sizeof(e->msg)); return -1; }
    int idx = fs_find_file(e->filename);
    if (idx < 0) idx = fs_create(e->filename);
    int rc = idx >= 0 ? fs_write(idx, text, len) : -1;
    kfree(text);
    if (rc != 0) { kstrlcpy(e->msg, "Could not save (disk full or bad name)", sizeof(e->msg)); return -1; }
    e->tb.dirty = 0;
    ksnprintf(e->msg, sizeof(e->msg), "Saved %u bytes", len);
    return 0;
}

/* a one-line prompt in the status bar; 0 if cancelled */
static int prompt(editor_t* e, int my_vt, const char* label, char* out, int cap) {
    int n = 0;
    out[0] = 0;
    for (;;) {
        char line[160];
        ksnprintf(line, sizeof(line), "  %s%s_", label, out);
        draw(e);
        terminal_set_cursor((size_t)(1 + text_rows()), 0);
        bar(line);
        char c = editor_wait_key(NULL, my_vt);
        if (c == '\n') return n > 0;
        if (c == 27 || c == 3) return 0;
        if (c == '\b') { if (n) n = u8_backspace(out, n); continue; }
        if ((unsigned char)c >= 32 && n < cap - 1) { out[n++] = c; out[n] = 0; }
    }
}

/* ESC [ ... : arrows, Home/End/PgUp/PgDn/Delete (ours: H F I G P; xterm: 1~ 4~ 5~ 6~ 3~) */
static void escape(editor_t* e) {
    textbuf_t* t = &e->tb;
    char c2 = keyboard_getchar();
    if (c2 != '[') return;
    char c3 = keyboard_getchar();
    if (c3 >= '1' && c3 <= '8') {
        keyboard_getchar();           /* '~' */
        static const char map[] = { 'H', 0, 'P', 'F', 'I', 'G', 'H', 'F' };  /* 1~ .. 8~ (2~ = Insert) */
        c3 = map[c3 - '1'];
    }
    int page = text_rows() - 1;
    switch (c3) {
    case 'A': tb_move(t, 0, -1, 0); break;
    case 'B': tb_move(t, 0, 1, 0); break;
    case 'C': tb_move(t, 1, 0, 0); break;
    case 'D': tb_move(t, -1, 0, 0); break;
    case 'H': tb_home(t, 0); break;
    case 'F': tb_end(t, 0); break;
    case 'I': tb_move(t, 0, -page, 0); e->scroll -= page; if (e->scroll < 0) e->scroll = 0; break;
    case 'G': tb_move(t, 0, page, 0); e->scroll += page; break;
    case 'P': tb_delete(t); break;
    }
}

void editor_open(const char* fname) {
    int my_vt = terminal_vt_get_active();
    editor_t* e = &g_ed[my_vt];

    char path[FS_PATH_LEN];
    if (fname[0] == '/') kstrlcpy(path, fname, sizeof(path));
    else {
        char cwd[FS_PATH_LEN];
        fs_cwd_path(cwd, sizeof(cwd));
        ksnprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", fname);
    }
    int idx = fs_find_file(path);
    if (idx >= 0 && fs_is_binary(idx)) {
        /* saving would truncate it at the first NUL byte */
        terminal_write_color("editor: refusing to open a binary file (image/download?)\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    tb_free(&e->tb);
    tb_init(&e->tb);
    if (idx >= 0) {
        fs_file_t* f = fs_get_file(idx);
        tb_load(&e->tb, f->content, f->size);
    }
    kstrlcpy(e->filename, path, sizeof(e->filename));
    e->scroll = e->hscroll = 0;
    e->msg[0] = 0;
    if (idx < 0) kstrlcpy(e->msg, "New file", sizeof(e->msg));
    textbuf_t* t = &e->tb;

    for (;;) {
        draw(e);
        char c = editor_wait_key(e, my_vt);
        if (c == 24) {                                  /* Ctrl+X */
            if (t->dirty) {
                draw(e);
                terminal_set_cursor((size_t)(1 + text_rows()), 0);
                bar("  Save modified buffer? (Y/N, Esc = back)");
                char ans = editor_wait_key(NULL, my_vt);
                if (ans == 27) continue;
                if ((ans == 'y' || ans == 'Y') && save(e) != 0) continue;
            }
            terminal_clear();
            tb_free(t);
            return;
        }
        if (c == 15 || c == 19) { save(e); continue; }             /* Ctrl+O / Ctrl+S */
        if (c == 11) {                                              /* Ctrl+K */
            uint32_t n;
            char* line = tb_cut_line(t, &n);
            if (line) {
                if (e->cut) kfree(e->cut);
                e->cut = line;
                e->cut_len = n;
                clipboard_set(line, n);
            }
            continue;
        }
        if (c == 21) {                                              /* Ctrl+U */
            if (e->cut) { tb_home(t, 0); tb_insert(t, e->cut, e->cut_len); }
            continue;
        }
        if (c == 3) {                                               /* Ctrl+C: copy the line */
            tb_line_t* l = &t->lines[t->cy];
            clipboard_set(l->s, l->len);
            kstrlcpy(e->msg, "Line copied", sizeof(e->msg));
            continue;
        }
        if (c == 22) {                                              /* Ctrl+V */
            uint32_t n;
            const char* clip = clipboard_get(&n);
            tb_insert(t, clip, n);
            continue;
        }
        if (c == 23) {                                              /* Ctrl+W: find */
            static char needle[64];
            char q[64];
            if (prompt(e, my_vt, "Find: ", q, sizeof(q))) kstrlcpy(needle, q, sizeof(needle));
            if (needle[0] && !tb_find(t, needle)) ksnprintf(e->msg, sizeof(e->msg), "\"%s\" not found", needle);
            continue;
        }
        if (c == 27) { escape(e); continue; }
        if (c == '\n') { tb_insert(t, "\n", 1); continue; }
        if (c == '\b') { tb_backspace(t); continue; }
        if (c == '\t') { tb_insert(t, "    ", 4); continue; }
        if ((unsigned char)c >= 32) tb_insert(t, &c, 1);
    }
}
