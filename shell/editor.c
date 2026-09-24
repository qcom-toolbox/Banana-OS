#include "editor.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/fs.h"
#include "../kernel/gui.h"
#include "../kernel/timer.h"
#include "../kernel/task.h"
#include "../kernel/types.h"

#define ED_MAX_LINES  64
#define ED_LINE_LEN   79
#define ED_ROWS       21

/* One buffer per vt (kernel/terminal.h), not a single shared instance:
 * each GUI terminal window can have its own "edit" session open at the
 * same time, and they must not stomp on each other's text. Indexed by
 * terminal_vt_get_active() at editor_open() time. */
typedef struct {
    char lines[ED_MAX_LINES][ED_LINE_LEN];
    int  nlines;
    int  cx, cy;
    int  scroll;
    int  dirty;
    char filename[FS_NAME_LEN];
    char cut_buf[ED_LINE_LEN];
} editor_buf_t;

static editor_buf_t g_ed[TERMINAL_VT_MAX];

/* helpers */
static int k_strlen(const char* s) { int n=0; while(s[n]) n++; return n; }
static void k_strcpy(char* d, const char* s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]='\0';
}
static void k_memmove(char* d, const char* s, int n) {
    if (d < s) { for(int i=0;i<n;i++) d[i]=s[i]; }
    else       { for(int i=n-1;i>=0;i--) d[i]=s[i]; }
}

/* buffer → file */
static void buf_to_file(editor_buf_t* e, int file_idx) {
    static char text[ED_MAX_LINES * ED_LINE_LEN];   /* lines + newlines always fit */
    int pos = 0;
    for (int i = 0; i < e->nlines; i++) {
        int l = k_strlen(e->lines[i]);
        for (int j = 0; j < l; j++) text[pos++] = e->lines[i][j];
        if (i < e->nlines - 1) text[pos++] = '\n';
    }
    fs_write(file_idx, text, (uint32_t)pos);
}

/* file → buffer */
static void file_to_buf(editor_buf_t* e, const char* src) {
    e->nlines = 0;
    int col = 0;
    /* stop one line early: the final (unterminated) line below needs a slot */
    for (int i = 0; src[i] && e->nlines < ED_MAX_LINES - 1; i++) {
        if (src[i] == '\n') {
            e->lines[e->nlines][col] = '\0';
            e->nlines++;
            col = 0;
        } else if (col < ED_LINE_LEN - 1) {
            e->lines[e->nlines][col++] = src[i];
        }
    }
    e->lines[e->nlines][col] = '\0';
    e->nlines++;
    if (e->nlines == 0) { e->lines[0][0] = '\0'; e->nlines = 1; }
}

/* draw */
static void draw(editor_buf_t* e) {
    terminal_clear();

    /* title bar */
    terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    terminal_write("  GNU nano 0.1  (Banana Edition)    File: ");
    terminal_write(e->filename);
    if (e->dirty) terminal_write(" [Modified]");

    int used = 42 + k_strlen(e->filename) + (e->dirty ? 11 : 0);
    for (int i = used; i < 80; i++) terminal_putchar(' ');

    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_putchar('\n');

    /* text */
    for (int row = 0; row < ED_ROWS; row++) {
        int li = e->scroll + row;
        if (li < e->nlines) terminal_write(e->lines[li]);
        terminal_putchar('\n');
    }

    /* status */
    terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);

    char lbuf[8]; char cbuf[8];
    char* ls = u32_to_str((uint32_t)(e->cy+1), lbuf, sizeof(lbuf));
    char* cs = u32_to_str((uint32_t)(e->cx+1), cbuf, sizeof(cbuf));

    terminal_write("  Line ");
    terminal_write(ls);
    terminal_write(", Col ");
    terminal_write(cs);

    for (int i = 14 + k_strlen(ls) + k_strlen(cs); i < 80; i++)
        terminal_putchar(' ');

    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_putchar('\n');

    /* shortcuts */
    terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    terminal_write("^X Exit  ^O/^S Save  ^K Cut line  ^U Paste");
    for (int i = 46; i < 80; i++) terminal_putchar(' ');
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    /* ✅ FIXED CURSOR POSITION */
    int screen_row = 2 + (e->cy - e->scroll);

    if (screen_row < 1) screen_row = 1;
    if (screen_row > ED_ROWS) screen_row = ED_ROWS;

    int screen_col = e->cx;
    if (screen_col < 0) screen_col = 0;
    if (screen_col > 79) screen_col = 79;

    terminal_set_cursor((size_t)screen_row, (size_t)screen_col);
}

/* clamp */
static void clamp(editor_buf_t* e) {
    if (e->cy < 0) e->cy = 0;
    if (e->cy >= e->nlines) e->cy = e->nlines - 1;

    int ll = k_strlen(e->lines[e->cy]);
    if (e->cx < 0) e->cx = 0;
    if (e->cx > ll) e->cx = ll;

    if (e->cy < e->scroll) e->scroll = e->cy;
    if (e->cy >= e->scroll + ED_ROWS) e->scroll = e->cy - ED_ROWS + 1;
}

/* Cooperative wait for the next keystroke aimed at this window: keeps the
 * GUI compositor running and this editor's vt bound as the write target,
 * but only consumes a key once this vt actually has keyboard focus - the
 * same rule shell_readline() follows, so an "edit" running unfocused in
 * the background can't steal keys meant for whatever window is focused. */
static char editor_wait_key(int my_vt) {
    while (1) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (c) return c;
        task_sleep_ms(10);
    }
}

static void save_file(editor_buf_t* e, int file_idx, int my_vt) {
    buf_to_file(e, file_idx);
    e->dirty = 0;
    draw(e);
    terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
    terminal_write("  Saved. Press any key...");
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    editor_wait_key(my_vt);
    draw(e);
}

void editor_open(const char* fname) {
    int my_vt = terminal_vt_get_active();
    editor_buf_t* e = &g_ed[my_vt];

    k_strcpy(e->filename, fname, FS_NAME_LEN);

    int idx = fs_find_file(fname);
    if (idx < 0) idx = fs_create(fname);
    if (idx < 0) {
        terminal_write_color("editor: cannot open\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }

    if (fs_is_binary(idx)) {
        /* saving would truncate it at the first NUL byte */
        terminal_write_color("editor: refusing to open a binary file (image/download?)\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }

    fs_file_t* f = fs_get_file(idx);
    file_to_buf(e, f->content);

    e->cx = 0; e->cy = 0; e->scroll = 0; e->dirty = 0;
    e->cut_buf[0] = '\0';

    draw(e);

    while (1) {
        char c = editor_wait_key(my_vt);

        if (c == 24) { /* Ctrl+X */
            if (e->dirty) {
                terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
                terminal_write("\n  Save modified buffer? (Y/N): ");
                terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                char ans = editor_wait_key(my_vt);
                if (ans == 'y' || ans == 'Y') buf_to_file(e, idx);
            }
            terminal_clear();
            return;
        }

        if (c == 15 || c == 19) { /* Ctrl+O or Ctrl+S */
            save_file(e, idx, my_vt);
            continue;
        }

        if (c == 11) { /* Ctrl+K */
            k_strcpy(e->cut_buf, e->lines[e->cy], ED_LINE_LEN);
            if (e->nlines > 1) {
                for (int i = e->cy; i < e->nlines - 1; i++)
                    k_strcpy(e->lines[i], e->lines[i+1], ED_LINE_LEN);
                e->nlines--;
            } else e->lines[0][0] = '\0';

            clamp(e);
            e->dirty = 1;
            draw(e);
            continue;
        }

        if (c == 21) { /* Ctrl+U */
            if (e->cut_buf[0] && e->nlines < ED_MAX_LINES) {
                for (int i = e->nlines; i > e->cy; i--)
                    k_strcpy(e->lines[i], e->lines[i-1], ED_LINE_LEN);
                k_strcpy(e->lines[e->cy], e->cut_buf, ED_LINE_LEN);
                e->nlines++;
                e->cy++;
                clamp(e);
                e->dirty = 1;
                draw(e);
            }
            continue;
        }

        if (c == 27) { /* arrows */
            char c2 = keyboard_getchar();
            if (c2 == '[') {
                char c3 = keyboard_getchar();
                if (c3 == 'A') e->cy--;
                if (c3 == 'B') e->cy++;
                if (c3 == 'C') e->cx++;
                if (c3 == 'D') e->cx--;
                clamp(e);
                draw(e);
            }
            continue;
        }

        if (c == '\n') {
            if (e->nlines >= ED_MAX_LINES) continue;

            char rest[ED_LINE_LEN];
            k_strcpy(rest, e->lines[e->cy] + e->cx, ED_LINE_LEN);
            e->lines[e->cy][e->cx] = '\0';

            for (int i = e->nlines; i > e->cy + 1; i--)
                k_strcpy(e->lines[i], e->lines[i-1], ED_LINE_LEN);

            e->nlines++;
            e->cy++;
            k_strcpy(e->lines[e->cy], rest, ED_LINE_LEN);
            e->cx = 0;

            clamp(e);
            e->dirty = 1;
            draw(e);
            continue;
        }

        if (c == '\b') {
            if (e->cx > 0) {
                char* ln = e->lines[e->cy];
                int ll = k_strlen(ln);
                k_memmove(ln + e->cx - 1, ln + e->cx, ll - e->cx + 1);
                e->cx--;
            } else if (e->cy > 0) {
                int prev_len = k_strlen(e->lines[e->cy-1]);
                int cur_len  = k_strlen(e->lines[e->cy]);

                if (prev_len + cur_len < ED_LINE_LEN - 1) {
                    k_strcpy(e->lines[e->cy-1] + prev_len, e->lines[e->cy], ED_LINE_LEN - prev_len);
                    for (int i = e->cy; i < e->nlines - 1; i++)
                        k_strcpy(e->lines[i], e->lines[i+1], ED_LINE_LEN);
                    e->nlines--;
                    e->cy--;
                    e->cx = prev_len;
                }
            }
            clamp(e);
            e->dirty = 1;
            draw(e);
            continue;
        }

        if (c >= 32 && c < 127) {
            char* ln = e->lines[e->cy];
            int ll = k_strlen(ln);

            if (ll < ED_LINE_LEN - 1) {
                k_memmove(ln + e->cx + 1, ln + e->cx, ll - e->cx + 1);
                ln[e->cx] = c;
                e->cx++;
            }

            clamp(e);
            e->dirty = 1;
            draw(e);
        }
    }
}