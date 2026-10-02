#ifndef TEXTBUF_H
#define TEXTBUF_H

#include "types.h"

/*
 * An editable text: lines on the heap (any length, any number), a cursor
 * and an optional selection. Shared by the terminal editor (`edit`) and
 * the desktop's Notepad.
 */

typedef struct {
    char*    s;
    uint32_t len, cap;
} tb_line_t;

typedef struct {
    tb_line_t* lines;
    int        n, cap;
    int        cx, cy;          /* cursor: column (byte) and line */
    int        want_x;          /* column kept while moving up/down */
    int        sel;             /* selection active: from (ax, ay) to the cursor */
    int        ax, ay;
    int        dirty;
} textbuf_t;

void  tb_init(textbuf_t* t);
void  tb_free(textbuf_t* t);
void  tb_load(textbuf_t* t, const char* text, uint32_t len);   /* CRLF -> LF */
char* tb_text(const textbuf_t* t, uint32_t* len);              /* kmalloc'd, NUL-terminated */

void  tb_insert(textbuf_t* t, const char* s, uint32_t n);      /* replaces the selection; '\n' splits */
void  tb_backspace(textbuf_t* t);
void  tb_delete(textbuf_t* t);

/* cursor movement; select = extend the selection (Shift) */
void  tb_move(textbuf_t* t, int dx, int dy, int select);
void  tb_home(textbuf_t* t, int select);
void  tb_end(textbuf_t* t, int select);
void  tb_goto(textbuf_t* t, int x, int y, int select);
void  tb_select_all(textbuf_t* t);

/* selection: ordered bounds, text (kmalloc'd), removal */
int   tb_sel_bounds(const textbuf_t* t, int* x0, int* y0, int* x1, int* y1);   /* 0 if none */
char* tb_sel_text(const textbuf_t* t, uint32_t* len);
void  tb_sel_delete(textbuf_t* t);

/* the whole line: cut (returned kmalloc'd, with its "\n") */
char* tb_cut_line(textbuf_t* t, uint32_t* len);

/* finds text after the cursor (wrapping); 1 if found (selects it) */
int   tb_find(textbuf_t* t, const char* needle);

#endif
