/* Banana Code - an editor for Banana OS in the spirit of VS Code.
 *
 *   main.c     the window: activity bar, side bar (Explorer, Search, Build,
 *              Extensions), tabs, the editor, the panel, the status bar,
 *              the command palette and its quick inputs
 *   editor.c   text buffers: editing, selection, undo, find, syntax colours
 *   build.c    projects (banana.json): building with TinyCC into a .bpk,
 *              installing and running it; new-project templates
 *   ext.c      extensions: HTML / JavaScript views talking to the editor
 *   json.c     a small JSON reader / writer
 */
#ifndef CODE_H
#define CODE_H

#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_ 512

/* ── json.c ─────────────────────────────────────────────────────────── */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef struct json json_t;
struct json {
    jtype_t  type;
    char*    key;           /* in an object */
    char*    str;           /* J_STR */
    double   num;           /* J_NUM, J_BOOL */
    json_t*  child;         /* J_ARR / J_OBJ: the first member */
    json_t*  next;
};
json_t*     json_parse(const char* text);
void        json_free(json_t* j);
json_t*     json_get(const json_t* obj, const char* key);
const char* json_str(const json_t* obj, const char* key, const char* def);
double      json_num(const json_t* obj, const char* key, double def);
/* a growing string, for writing JSON */
typedef struct { char* s; int len, cap; } sbuf_t;
void sb_init(sbuf_t* b);
void sb_add(sbuf_t* b, const char* s);
void sb_addn(sbuf_t* b, const char* s, int n);
void sb_printf(sbuf_t* b, const char* fmt, ...);
void sb_json_str(sbuf_t* b, const char* s, int n);    /* "..." escaped; n < 0: strlen */
void sb_free(sbuf_t* b);

/* ── editor.c ───────────────────────────────────────────────────────── */
typedef struct { char* s; int len, cap; unsigned char state; } line_t;   /* state: in a comment at its start */

enum { LANG_TEXT, LANG_C, LANG_JS, LANG_HTML, LANG_CSS, LANG_PY, LANG_JSON, LANG_SH, LANG_MAKE, LANG_MD };

typedef struct {
    char*   text;
    int     cy, cx;
} snap_t;

#define UNDO_MAX 64
typedef struct doc {
    char    path[PATH_MAX_];
    char    name[64];
    line_t* lines;
    int     n, cap;
    int     cy, cx;             /* cursor: line, byte */
    int     sy, sx;             /* selection anchor (sy < 0: none) */
    int     want_vx;            /* the visual column up / down keep */
    int     top, left;          /* first line / visual column shown */
    int     dirty;
    int     lang;
    snap_t  undo[UNDO_MAX];
    int     nundo;
    snap_t  redo[UNDO_MAX];
    int     nredo;
    int     last_op;            /* what the last change was (typing groups into one undo) */
    unsigned last_ms;
} doc_t;

doc_t* doc_new(const char* path);               /* reads the file (missing: empty) */
void   doc_free(doc_t* d);
int    doc_save(doc_t* d);                      /* 0 or -1 */
char*  doc_text(const doc_t* d, int* len);      /* the whole text (malloc'd) */
void   doc_set_text(doc_t* d, const char* text);
int    doc_lang_for(const char* name);
const char* doc_lang_name(int lang);

int    doc_has_sel(const doc_t* d);
void   doc_sel_range(const doc_t* d, int* y0, int* x0, int* y1, int* x1);
char*  doc_sel_text(const doc_t* d);            /* malloc'd ("" if none) */
void   doc_clear_sel(doc_t* d);

enum { OP_TYPE = 1, OP_DEL, OP_OTHER };
void   doc_snapshot(doc_t* d, int op);          /* before a change */
void   doc_undo(doc_t* d);
void   doc_redo(doc_t* d);

void   doc_insert(doc_t* d, const char* s, int n);   /* at the cursor, replacing a selection */
void   doc_delete_sel(doc_t* d);
void   doc_backspace(doc_t* d);
void   doc_delete(doc_t* d);
void   doc_newline(doc_t* d);                   /* keeps the indentation */
void   doc_indent(doc_t* d, int out);           /* Tab / Shift+Tab on a selection */
void   doc_toggle_comment(doc_t* d);

/* cursor moves (shift: extend the selection) */
void   doc_move(doc_t* d, int dy, int dx, int shift);
void   doc_home(doc_t* d, int shift);
void   doc_end(doc_t* d, int shift);
void   doc_word(doc_t* d, int dir, int shift);
void   doc_goto(doc_t* d, int y, int x, int shift);
void   doc_select_all(doc_t* d);
void   doc_select_word(doc_t* d);

int    doc_vcol(const doc_t* d, int y, int x);      /* byte -> visual column (tabs: 4) */
int    doc_byte_at(const doc_t* d, int y, int vx);  /* visual column -> byte */

/* find: the next (dir 1) / previous match from the cursor, selected; 0 if none */
int    doc_find(doc_t* d, const char* what, int dir, int match_case);
int    doc_replace_all(doc_t* d, const char* what, const char* with, int match_case);

/* colours for line y (c[i] per byte, a palette index) */
enum { HL_TEXT, HL_KEYWORD, HL_CONTROL, HL_TYPE, HL_STRING, HL_NUMBER, HL_COMMENT, HL_PREPROC, HL_FUNC, HL_TAG, HL_ATTR, HL_PUNCT, HL_COUNT };
void   doc_highlight(doc_t* d, int y, unsigned char* c);

/* ── build.c ────────────────────────────────────────────────────────── */
typedef struct {
    char file[PATH_MAX_];
    int  line;
    int  is_error;
    char msg[200];
} problem_t;
#define MAX_PROBLEMS 200

extern char      g_folder[PATH_MAX_];       /* the folder open ("" none) */
extern sbuf_t    g_output;                   /* the Output panel's text */
extern problem_t g_problems[MAX_PROBLEMS];
extern int       g_nproblems;

void out_clear(void);
void out_printf(const char* fmt, ...);

int  project_exists(void);
int  build_project(void);                   /* 0 if it built */
int  run_project(void);                     /* build, install, start */
/* a new app in folder/name from a template (console / gui); 0 or -1 */
int  new_project(const char* parent, const char* name, int gui, char* out_dir, int cap);
/* a banana.json for an existing folder of C files ("Port a program"); 0 or -1 */
int  port_project(void);

/* ── ext.c ──────────────────────────────────────────────────────────── */
#define MAX_EXT 16
#define MAX_EXT_CMDS 16
typedef struct {
    char id[48], name[64], version[16], description[160], publisher[48];
    char dir[PATH_MAX_];
    char view[96];            /* its HTML view ("" none) */
    char icon[8];             /* a letter for the activity bar */
    unsigned color;
    int  builtin, enabled;
    int  ncmds;
    char cmd_id[MAX_EXT_CMDS][64], cmd_title[MAX_EXT_CMDS][80];
    int  webview;             /* -1: not started */
} ext_t;
extern ext_t g_ext[MAX_EXT];
extern int   g_next;
extern int   g_ext_open;      /* the extension whose view is shown (-1 none) */

void ext_load_all(void);
void ext_set_enabled(int i, int on);
int  ext_uninstall(int i, char* msg, int cap);
int  ext_install_folder(const char* dir, char* msg, int cap);
void ext_open_view(int i, int w, int h);    /* starts / shows its view */
void ext_resize(int w, int h);
void ext_close_view(void);
void ext_command(const char* cmd);          /* an extension's command */
int  ext_poll(void);                        /* messages, network results; 1 if its view changed */

/* ── main.c: what the extensions and the build reach ────────────────── */
doc_t* cur_doc(void);
doc_t* open_file(const char* path);         /* in a tab (or the tab it has) */
void   set_status(const char* fmt, ...);
void   show_output(void);
void   open_ext_panel(int i);               /* shows extension i's view on the right */
void   run_builtin_command(const char* id);
char*  read_file(const char* path, int* len);       /* malloc'd, NUL-terminated */
int    write_file(const char* path, const char* data, int len);
void   join_path(char* out, int cap, const char* dir, const char* name);
int    is_dir(const char* path);
int    mkdir_p(const char* path);
extern int g_ui_dirty;

#endif
