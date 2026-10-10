/* Banana Code - the window. See code.h for the parts. */
#include "code.h"
#include <stdarg.h>

/* ── colours (VS Code's dark theme) ── */
#define C_BG        0x1E1E1Eu
#define C_SIDE      0x252526u
#define C_ACT       0x333333u
#define C_TOP       0x3C3C3Cu
#define C_TAB       0x2D2D2Du
#define C_TABON     0x1E1E1Eu
#define C_STATUS    0x007ACCu
#define C_TEXT      0xD4D4D4u
#define C_DIM       0x858585u
#define C_BRIGHT    0xFFFFFFu
#define C_SEL       0x264F78u
#define C_CURLINE   0x2A2D2Eu
#define C_HOVER     0x2A2D2Eu
#define C_LISTSEL   0x37373Du
#define C_ACCENT    0x0E639Cu
#define C_INPUT     0x3C3C3Cu
#define C_BORDER    0x454545u
#define C_ERR       0xF14C4Cu
#define C_WARN      0xCCA700u
#define C_PANEL     0x1E1E1Eu

static const unsigned HL_COL[HL_COUNT] = {
    0xD4D4D4u, 0x569CD6u, 0xC586C0u, 0x4EC9B0u, 0xCE9178u, 0xB5CEA8u, 0x6A9955u, 0xC586C0u, 0xDCDCAAu, 0x569CD6u, 0x9CDCFEu, 0xD4D4D4u,
};

#define TOP_H    30
#define STATUS_H 22
#define ACT_W    46
#define TAB_H    32
#define ROW_H    22
#define PANEL_TABS 30

static bwin_t win;
int g_ui_dirty = 1;

/* layout */
static int g_side_on = 1, g_side_w = 240, g_view;          /* views: 0 explorer, 1 search, 2 build, 3 extensions */
static int g_panel_on, g_panel_h = 180, g_panel_tab = 1;   /* 0 problems, 1 output */
static int g_right_w = 400;
static int g_focus;                                        /* F_* */
enum { F_EDITOR, F_SEARCH, F_QUICK, F_FIND, F_EXT };

/* the editor font: a cell per character (pre-rendered glyphs) */
#define ED_PX 14
static int g_cw = 8, g_lh = 18, g_asc = 13;
static unsigned char* g_glyph[128];
static int g_gw;

/* tabs */
#define MAX_DOCS 24
static doc_t* g_docs[MAX_DOCS];
static int g_ndocs, g_cur = -1;

static char g_status[160];
static unsigned g_status_ms;

/* ── small helpers ── */
static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

void join_path(char* out, int cap, const char* dir, const char* name) {
    if (!dir || !dir[0]) snprintf(out, (size_t)cap, "%s", name);
    else if (!strcmp(dir, "/")) snprintf(out, (size_t)cap, "/%s", name);
    else snprintf(out, (size_t)cap, "%s/%s", dir, name);
}

char* read_file(const char* path, int* len) {
    if (!path || !*path) return NULL;
    banana_stat_t st;
    if (__banana->stat(path, &st) != 0 || st.is_dir) return NULL;
    int fd = __banana->open(path, BANANA_O_READ);
    if (fd < 0) return NULL;
    char* b = malloc(st.size + 1);
    long got = 0;
    if (b) {
        while (got < (long)st.size) {
            long r = __banana->read(fd, b + got, st.size - (unsigned long)got);
            if (r <= 0) break;
            got += r;
        }
        b[got] = 0;
    }
    __banana->close(fd);
    if (len) *len = (int)got;
    return b;
}

int write_file(const char* path, const char* data, int len) {
    int fd = __banana->open(path, BANANA_O_WRITE | BANANA_O_CREATE | BANANA_O_TRUNC);
    if (fd < 0) return -1;
    long w = len > 0 ? __banana->write(fd, data, (unsigned long)len) : 0;
    __banana->close(fd);
    return w == len ? 0 : -1;
}

int is_dir(const char* path) { banana_stat_t st; return __banana->stat(path, &st) == 0 && st.is_dir; }

int mkdir_p(const char* path) {
    char p[PATH_MAX_];
    snprintf(p, sizeof(p), "%s", path);
    for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; if (!is_dir(p)) __banana->mkdir(p); *s = '/'; }
    if (!is_dir(p)) __banana->mkdir(p);
    return is_dir(p) ? 0 : -1;
}

void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_ms = banana_ticks();
    g_ui_dirty = 1;
}

static int ui_w(const char* s, int px) { return banana_font_width(BANANA_FONT_SANS, px, s); }
/* UI text, cut with ".." to max_w pixels */
static int ui_text(int x, int y, const char* s, int px, unsigned col, int max_w) {
    if (max_w <= 0) return x;
    if (ui_w(s, px) <= max_w) return bwin_font(&win, x, y, BANANA_FONT_SANS, px, s, col);
    char t[256];
    int n = (int)strlen(s);
    if (n > 250) n = 250;
    while (n > 0) {
        memcpy(t, s, (size_t)n);
        strcpy(t + n, "..");
        if (ui_w(t, px) <= max_w) break;
        n--;
    }
    return bwin_font(&win, x, y, BANANA_FONT_SANS, px, n ? t : "", col);
}
static void ui_bold(int x, int y, const char* s, int px, unsigned col) { bwin_font(&win, x, y, BANANA_FONT_SANS_BOLD, px, s, col); }

/* ── the editor's glyphs ── */
static void glyphs_init(void) {
    int w10 = banana_font_width(BANANA_FONT_MONO, ED_PX, "MMMMMMMMMM");
    g_cw = (w10 + 5) / 10;
    int asc, desc, lh;
    banana_font_metrics(BANANA_FONT_MONO, ED_PX, &asc, &desc, &lh);
    g_lh = asc + desc + 4;
    g_asc = asc;
    g_gw = g_cw + 4;
    unsigned* tmp = malloc((size_t)g_gw * (size_t)g_lh * 4);
    for (int c = 33; c < 127; c++) {
        memset(tmp, 0, (size_t)g_gw * (size_t)g_lh * 4);
        char s[2] = { (char)c, 0 };
        __banana->font_draw(tmp, g_gw, g_gw, g_lh, 0, 2, BANANA_FONT_MONO, ED_PX, s, 0xFFFFFF);
        g_glyph[c] = malloc((size_t)g_gw * (size_t)g_lh);
        for (int i = 0; i < g_gw * g_lh; i++) g_glyph[c][i] = (unsigned char)(tmp[i] >> 8);
    }
    free(tmp);
}

static inline unsigned blend(unsigned d, unsigned s, unsigned a) {
    unsigned rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a)) >> 8;
    unsigned g = ((s & 0xFF00) * a + (d & 0xFF00) * (255 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0xFF00);
}

/* one character cell at (x, y), clipped to [cx0, cx1) */
static void put_char(int x, int y, unsigned char c, unsigned col, int cx0, int cx1, int cy1) {
    if (c < 33 || c > 126 || !g_glyph[c]) return;
    const unsigned char* g = g_glyph[c];
    for (int r = 0; r < g_lh; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= win.h || yy >= cy1) break;
        unsigned* row = win.px + yy * win.w;
        for (int k = 0; k < g_gw; k++) {
            unsigned a = g[r * g_gw + k];
            int xx = x + k;
            if (!a || xx < cx0 || xx >= cx1 || xx >= win.w) continue;
            row[xx] = a >= 250 ? col : blend(row[xx], col, a);
        }
    }
}

/* ── documents ── */
doc_t* cur_doc(void) { return g_cur >= 0 && g_cur < g_ndocs ? g_docs[g_cur] : NULL; }

doc_t* open_file(const char* path) {
    for (int i = 0; i < g_ndocs; i++)
        if (path && path[0] && !strcmp(g_docs[i]->path, path)) { g_cur = i; g_focus = F_EDITOR; g_ui_dirty = 1; return g_docs[i]; }
    if (g_ndocs >= MAX_DOCS) { set_status("Too many files open: close some tabs"); return NULL; }
    if (path && path[0] && is_dir(path)) return NULL;
    doc_t* d = doc_new(path);
    if (!d) return NULL;
    g_docs[g_ndocs] = d;
    g_cur = g_ndocs++;
    g_focus = F_EDITOR;
    g_ui_dirty = 1;
    return d;
}

static void close_doc(int i) {
    if (i < 0 || i >= g_ndocs) return;
    doc_free(g_docs[i]);
    memmove(g_docs + i, g_docs + i + 1, sizeof(doc_t*) * (size_t)(g_ndocs - i - 1));
    g_ndocs--;
    if (g_cur >= g_ndocs) g_cur = g_ndocs - 1;
    g_ui_dirty = 1;
}

/* ── the explorer tree ── */
typedef struct { char path[PATH_MAX_]; char name[64]; int depth, dir; } node_t;
#define MAX_NODES 1500
static node_t* g_nodes;
static int g_nnodes, g_tree_top, g_tree_sel = -1;
#define MAX_OPEN 64
static char g_open_dirs[MAX_OPEN][PATH_MAX_];
static int g_nopen;

static int dir_open(const char* p) { for (int i = 0; i < g_nopen; i++) if (!strcmp(g_open_dirs[i], p)) return 1; return 0; }
static void toggle_dir(const char* p) {
    for (int i = 0; i < g_nopen; i++) if (!strcmp(g_open_dirs[i], p)) { memmove(g_open_dirs[i], g_open_dirs[i + 1], (size_t)(g_nopen - i - 1) * PATH_MAX_); g_nopen--; return; }
    if (g_nopen < MAX_OPEN) snprintf(g_open_dirs[g_nopen++], PATH_MAX_, "%s", p);
}

typedef struct { char name[64]; int dir; } ent_t;
static int ent_cmp(const void* a, const void* b) {
    const ent_t* x = a; const ent_t* y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void scan_tree(const char* dir, int depth) {
    if (depth > 10) return;
    ent_t* e = malloc(sizeof(ent_t) * 400);
    if (!e) return;
    int n = 0;
    banana_dirent_t de;
    for (int i = 0; n < 400 && __banana->readdir(dir, i, &de) == 0; i++) {
        if (de.name[0] == '.' && strcmp(de.name, ".bcode")) continue;
        snprintf(e[n].name, 64, "%s", de.name);
        e[n].dir = de.is_dir;
        n++;
    }
    qsort(e, (size_t)n, sizeof(ent_t), ent_cmp);
    for (int i = 0; i < n && g_nnodes < MAX_NODES; i++) {
        node_t* nd = &g_nodes[g_nnodes++];
        join_path(nd->path, sizeof(nd->path), dir, e[i].name);
        snprintf(nd->name, sizeof(nd->name), "%s", e[i].name);
        nd->depth = depth;
        nd->dir = e[i].dir;
        if (nd->dir && dir_open(nd->path)) scan_tree(nd->path, depth + 1);
    }
    free(e);
}

static void refresh_tree(void) {
    g_nnodes = 0;
    if (g_folder[0]) scan_tree(g_folder, 0);
    if (g_tree_sel >= g_nnodes) g_tree_sel = -1;
    g_ui_dirty = 1;
}

static void open_folder(const char* p) {
    snprintf(g_folder, sizeof(g_folder), "%s", p);
    g_nopen = 0;
    g_tree_top = 0;
    g_tree_sel = -1;
    refresh_tree();
    char t[160];
    const char* b = strrchr(p, '/');
    snprintf(t, sizeof(t), "%s - Banana Code", b && b[1] ? b + 1 : p);
    bwin_title(&win, t);
    write_file("/home/banana/.bcode/last-folder", p, (int)strlen(p));
    g_view = 0;
    g_side_on = 1;
}

/* ── search in files ── */
static char g_search[128];
typedef struct { char path[PATH_MAX_]; int line; char text[96]; } hit_t;
#define MAX_HITS 300
static hit_t* g_hits;
static int g_nhits, g_hits_top;

static void search_dir(const char* dir, int depth) {
    if (depth > 8 || g_nhits >= MAX_HITS) return;
    banana_dirent_t de;
    for (int i = 0; g_nhits < MAX_HITS && __banana->readdir(dir, i, &de) == 0; i++) {
        if (de.name[0] == '.' || !strcmp(de.name, "build")) continue;
        char p[PATH_MAX_];
        join_path(p, sizeof(p), dir, de.name);
        if (de.is_dir) { search_dir(p, depth + 1); continue; }
        if (de.size > 1 << 20) continue;
        char* t = read_file(p, NULL);
        if (!t) continue;
        int ln = 1;
        char* ls = t;
        for (char* c = t; ; c++) {
            if (*c == '\n' || !*c) {
                char save = *c;
                *c = 0;
                if (strstr(ls, g_search) && g_nhits < MAX_HITS) {
                    hit_t* h = &g_hits[g_nhits++];
                    snprintf(h->path, sizeof(h->path), "%s", p);
                    h->line = ln;
                    char* s = ls;
                    while (*s == ' ' || *s == '\t') s++;
                    snprintf(h->text, sizeof(h->text), "%s", s);
                }
                *c = save;
                if (!save) break;
                ls = c + 1;
                ln++;
            }
        }
        free(t);
    }
}

static void run_search(void) {
    g_nhits = 0;
    g_hits_top = 0;
    if (g_search[0] && g_folder[0]) search_dir(g_folder, 0);
    set_status("%d result%s for \"%s\"", g_nhits, g_nhits == 1 ? "" : "s", g_search);
}

/* ── the quick input (command palette and friends) ── */
enum { Q_NONE, Q_PALETTE, Q_FILES, Q_GOTO, Q_NEWFILE, Q_NEWFOLDER, Q_NEWAPP, Q_NEWAPP_KIND, Q_OPENFOLDER, Q_EXTFOLDER, Q_SAVEAS, Q_CLOSE };
static int g_q;
static char g_qtext[256];
static char g_qtitle[96];
static char g_qdir[PATH_MAX_];           /* folder browsing */
static char g_qarg[64];
static int g_qsel, g_qtop;
#define MAX_ITEMS 2000
static char (*g_items)[PATH_MAX_];      /* what a list shows / does */
static char (*g_item_label)[128];
static char (*g_item_key)[24];
static int g_nitems;
static int* g_shown;                     /* filtered */
static int g_nshown;

typedef struct { const char* id; const char* title; const char* key; } cmd_t;
static const cmd_t CMDS[] = {
    { "file.openFolder", "File: Open Folder...", "Ctrl+K" },
    { "file.newFile", "File: New File...", "Ctrl+N" },
    { "file.newFolder", "File: New Folder...", "" },
    { "file.save", "File: Save", "Ctrl+S" },
    { "file.saveAll", "File: Save All", "" },
    { "file.close", "View: Close Editor", "Ctrl+W" },
    { "file.quickOpen", "Go to File...", "Ctrl+P" },
    { "edit.undo", "Edit: Undo", "Ctrl+Z" },
    { "edit.redo", "Edit: Redo", "Ctrl+Y" },
    { "edit.find", "Edit: Find", "Ctrl+F" },
    { "edit.replace", "Edit: Replace All...", "Ctrl+H" },
    { "edit.goto", "Go to Line...", "Ctrl+G" },
    { "edit.comment", "Edit: Toggle Line Comment", "Ctrl+/" },
    { "edit.duplicate", "Edit: Duplicate Line", "Ctrl+D" },
    { "edit.selectAll", "Edit: Select All", "Ctrl+A" },
    { "view.explorer", "View: Show Explorer", "Ctrl+Shift+E" },
    { "view.search", "View: Search in Files", "Ctrl+Shift+F" },
    { "view.build", "View: Show Build and Run", "Ctrl+Shift+D" },
    { "view.extensions", "View: Show Extensions", "Ctrl+Shift+X" },
    { "view.sidebar", "View: Toggle Side Bar", "Ctrl+B" },
    { "view.panel", "View: Toggle Panel", "Ctrl+J" },
    { "view.problems", "View: Show Problems", "Ctrl+Shift+M" },
    { "view.output", "View: Show Output", "Ctrl+Shift+U" },
    { "build.build", "Build: Build the App", "F7" },
    { "build.run", "Build: Build, Install and Run", "F5" },
    { "build.newApp", "Banana OS: New App...", "" },
    { "build.port", "Banana OS: Port a Program (make banana.json)", "" },
    { "ext.install", "Extensions: Install from Folder...", "" },
    { "ext.reload", "Developer: Reload Extensions", "" },
    { "help.about", "Help: About Banana Code", "" },
};
#define NCMDS ((int)(sizeof(CMDS) / sizeof(CMDS[0])))

static int fuzzy(const char* hay, const char* needle) {
    /* every character of needle, in order */
    for (; *needle; needle++) {
        char n = *needle >= 'A' && *needle <= 'Z' ? (char)(*needle + 32) : *needle;
        if (n == ' ') continue;
        for (;; hay++) {
            if (!*hay) return 0;
            char h = *hay >= 'A' && *hay <= 'Z' ? (char)(*hay + 32) : *hay;
            if (h == n) { hay++; break; }
        }
    }
    return 1;
}

static void q_filter(void) {
    g_nshown = 0;
    int list = g_q == Q_PALETTE || g_q == Q_FILES || g_q == Q_OPENFOLDER || g_q == Q_EXTFOLDER || g_q == Q_NEWAPP_KIND || g_q == Q_CLOSE;
    if (!list) return;
    int filt = g_q == Q_PALETTE || g_q == Q_FILES;
    for (int i = 0; i < g_nitems; i++)
        if (!filt || !g_qtext[0] || fuzzy(g_item_label[i], g_qtext)) g_shown[g_nshown++] = i;
    if (g_qsel >= g_nshown) g_qsel = g_nshown - 1;
    if (g_qsel < 0) g_qsel = 0;
    g_qtop = 0;
}

static void add_item(const char* label, const char* act, const char* key) {
    if (g_nitems >= MAX_ITEMS) return;
    snprintf(g_item_label[g_nitems], 128, "%s", label);
    snprintf(g_items[g_nitems], PATH_MAX_, "%s", act);
    snprintf(g_item_key[g_nitems], 24, "%s", key ? key : "");
    g_nitems++;
}

static void list_files(const char* dir, int depth) {
    if (depth > 8) return;
    banana_dirent_t de;
    for (int i = 0; g_nitems < MAX_ITEMS && __banana->readdir(dir, i, &de) == 0; i++) {
        if (de.name[0] == '.' || (!strcmp(de.name, "build") && depth == 0)) continue;
        char p[PATH_MAX_];
        join_path(p, sizeof(p), dir, de.name);
        if (de.is_dir) { list_files(p, depth + 1); continue; }
        add_item(p + strlen(g_folder) + (g_folder[1] ? 1 : 0), p, "");
    }
}

static void list_dirs(void) {
    g_nitems = 0;
    add_item(g_q == Q_EXTFOLDER ? "[ Install the extension in this folder ]" : "[ Open this folder ]", ".", "");
    if (strcmp(g_qdir, "/")) add_item(".. (up)", "..", "");
    banana_dirent_t de;
    ent_t* e = malloc(sizeof(ent_t) * 300);
    int n = 0;
    for (int i = 0; e && n < 300 && __banana->readdir(g_qdir, i, &de) == 0; i++) if (de.is_dir) { snprintf(e[n].name, 64, "%s", de.name); e[n].dir = 1; n++; }
    if (e) qsort(e, (size_t)n, sizeof(ent_t), ent_cmp);
    for (int i = 0; i < n; i++) { char l[80]; snprintf(l, sizeof(l), "%s/", e[i].name); add_item(l, e[i].name, ""); }
    free(e);
    snprintf(g_qtext, sizeof(g_qtext), "%s", g_qdir);
    g_qsel = 0;
    q_filter();
}

static void q_open(int mode, const char* title, const char* initial) {
    g_q = mode;
    snprintf(g_qtitle, sizeof(g_qtitle), "%s", title);
    snprintf(g_qtext, sizeof(g_qtext), "%s", initial ? initial : "");
    g_nitems = 0;
    g_qsel = 0;
    if (mode == Q_PALETTE) {
        for (int i = 0; i < NCMDS; i++) add_item(CMDS[i].title, CMDS[i].id, CMDS[i].key);
        for (int e = 0; e < g_next; e++) if (g_ext[e].enabled)
            for (int c = 0; c < g_ext[e].ncmds; c++) add_item(g_ext[e].cmd_title[c], g_ext[e].cmd_id[c], "");
    } else if (mode == Q_FILES) {
        if (g_folder[0]) list_files(g_folder, 0);
    } else if (mode == Q_OPENFOLDER || mode == Q_EXTFOLDER) {
        snprintf(g_qdir, sizeof(g_qdir), "%s", g_folder[0] ? g_folder : "/home/banana");
        list_dirs();
    } else if (mode == Q_NEWAPP_KIND) {
        add_item("Desktop app - a window (banana.h)", "gui", "");
        add_item("Console app - runs in a terminal (stdio)", "console", "");
    } else if (mode == Q_CLOSE) {
        add_item("Save", "save", "");
        add_item("Don't Save", "discard", "");
        add_item("Cancel", "cancel", "");
    }
    q_filter();
    g_focus = F_QUICK;
    g_ui_dirty = 1;
}

static void q_close(void) { g_q = Q_NONE; g_focus = F_EDITOR; g_ui_dirty = 1; }

/* ── find bar ── */
static int g_find_on;
static char g_find[128], g_repl[128];
static int g_find_field;                  /* 0 find, 1 replace */
static int g_replace_on;

/* ── commands ── */
static void save_doc(doc_t* d) {
    if (!d) return;
    if (!d->path[0]) { q_open(Q_SAVEAS, "Save as (a name in the folder, or a full path)", "untitled.c"); return; }
    if (doc_save(d) == 0) set_status("Saved %s", d->name);
    else set_status("Could not save %s", d->path);
    refresh_tree();
}

static void show_view(int v) {
    if (g_side_on && g_view == v) g_side_on = 0;
    else { g_side_on = 1; g_view = v; }
    if (v == 1 && g_side_on) g_focus = F_SEARCH;
    g_ui_dirty = 1;
}

void show_output(void) { g_panel_on = 1; g_panel_tab = 1; g_ui_dirty = 1; }

void open_ext_panel(int i) {
    if (i < 0 || i >= g_next) return;
    int h = win.h - TOP_H - STATUS_H - 30;
    ext_open_view(i, g_right_w, h > 50 ? h : 50);
    g_ui_dirty = 1;
}

static void redraw(void);

void run_builtin_command(const char* id) {
    doc_t* d = cur_doc();
    if (!strcmp(id, "file.openFolder")) q_open(Q_OPENFOLDER, "Open Folder", NULL);
    else if (!strcmp(id, "file.newFile")) q_open(Q_NEWFILE, "New file name", "");
    else if (!strcmp(id, "file.newFolder")) q_open(Q_NEWFOLDER, "New folder name", "");
    else if (!strcmp(id, "file.save") || !strcmp(id, "save")) save_doc(d);
    else if (!strcmp(id, "file.saveAll")) { for (int i = 0; i < g_ndocs; i++) if (g_docs[i]->dirty && g_docs[i]->path[0]) doc_save(g_docs[i]); set_status("Saved all"); }
    else if (!strcmp(id, "file.close")) { if (d && d->dirty) q_open(Q_CLOSE, "Save the changes?", NULL); else close_doc(g_cur); }
    else if (!strcmp(id, "file.quickOpen")) q_open(Q_FILES, "Go to file", "");
    else if (!strcmp(id, "edit.undo")) { if (d) doc_undo(d); }
    else if (!strcmp(id, "edit.redo")) { if (d) doc_redo(d); }
    else if (!strcmp(id, "edit.find")) {
        g_find_on = 1; g_replace_on = 0; g_find_field = 0; g_focus = F_FIND;
        if (d && doc_has_sel(d)) { char* s = doc_sel_text(d); if (s && !strchr(s, '\n')) snprintf(g_find, sizeof(g_find), "%s", s); free(s); }
    }
    else if (!strcmp(id, "edit.replace")) { g_find_on = 1; g_replace_on = 1; g_find_field = 0; g_focus = F_FIND; }
    else if (!strcmp(id, "edit.goto")) q_open(Q_GOTO, "Go to line", "");
    else if (!strcmp(id, "edit.comment")) { if (d) { doc_snapshot(d, OP_OTHER); doc_toggle_comment(d); } }
    else if (!strcmp(id, "edit.duplicate")) {
        if (d) { doc_snapshot(d, OP_OTHER); line_t* l = &d->lines[d->cy]; char* s = malloc((size_t)l->len + 2); if (s) { s[0] = '\n'; memcpy(s + 1, l->s, (size_t)l->len); int cx = d->cx; doc_clear_sel(d); d->cx = l->len; doc_insert(d, s, l->len + 1); d->cx = cx; free(s); } }
    }
    else if (!strcmp(id, "edit.selectAll")) { if (d) doc_select_all(d); }
    else if (!strcmp(id, "view.explorer")) show_view(0);
    else if (!strcmp(id, "view.search")) show_view(1);
    else if (!strcmp(id, "view.build")) show_view(2);
    else if (!strcmp(id, "view.extensions")) show_view(3);
    else if (!strcmp(id, "view.sidebar")) g_side_on = !g_side_on;
    else if (!strcmp(id, "view.panel")) g_panel_on = !g_panel_on;
    else if (!strcmp(id, "view.problems")) { g_panel_on = 1; g_panel_tab = 0; }
    else if (!strcmp(id, "view.output")) show_output();
    else if (!strcmp(id, "build.build") || !strcmp(id, "build") || !strcmp(id, "build.run") || !strcmp(id, "run")) {
        for (int i = 0; i < g_ndocs; i++) if (g_docs[i]->dirty && g_docs[i]->path[0]) doc_save(g_docs[i]);
        show_output();
        set_status("Building...");
        out_clear();
        out_printf("Building...\n");
        redraw();
        int run = !strcmp(id, "build.run") || !strcmp(id, "run");
        int rc = run ? run_project() : build_project();
        if (g_nproblems) g_panel_tab = rc ? 0 : 1;
        set_status(rc ? "Build failed - see Problems" : run ? "Built and started" : "Build succeeded");
        refresh_tree();
    }
    else if (!strcmp(id, "build.newApp")) q_open(Q_NEWAPP, "Name of the new app (a-z, 0-9, - _)", "");
    else if (!strcmp(id, "build.port")) {
        if (!g_folder[0]) { set_status("Open the program's folder first"); q_open(Q_OPENFOLDER, "Open the folder of the program to port", NULL); }
        else { port_project(); show_output(); refresh_tree(); char p[PATH_MAX_]; join_path(p, sizeof(p), g_folder, "banana.json"); open_file(p); }
    }
    else if (!strcmp(id, "ext.install")) q_open(Q_EXTFOLDER, "Install an extension: choose its folder", NULL);
    else if (!strcmp(id, "ext.reload")) { ext_load_all(); set_status("%d extension%s", g_next, g_next == 1 ? "" : "s"); }
    else if (!strcmp(id, "help.about")) {
        show_output();
        out_clear();
        out_printf("Banana Code 1.0 - an editor for Banana OS in the spirit of VS Code.\n"
                   "C compiler: TinyCC %s (built in). Projects: banana.json, built into a .bpk for this computer.\n"
                   "Extensions: HTML + JavaScript views (see /apps/code/extensions/claude for an example).\n", "0.9.28rc");
    }
    else ext_command(id);
    g_ui_dirty = 1;
}

/* ── quick input: Enter ── */
static void q_accept(void) {
    int mode = g_q;
    char text[256];
    snprintf(text, sizeof(text), "%s", g_qtext);
    int item = g_nshown > 0 && g_qsel < g_nshown ? g_shown[g_qsel] : -1;
    if (mode == Q_PALETTE) { q_close(); if (item >= 0) run_builtin_command(g_items[item]); return; }
    if (mode == Q_FILES) { q_close(); if (item >= 0) open_file(g_items[item]); return; }
    if (mode == Q_GOTO) {
        q_close();
        doc_t* d = cur_doc();
        if (d && atoi(text) > 0) { doc_goto(d, atoi(text) - 1, 0, 0); d->top = d->cy - 10 > 0 ? d->cy - 10 : 0; }
        return;
    }
    if (mode == Q_NEWFILE || mode == Q_NEWFOLDER || mode == Q_SAVEAS) {
        q_close();
        if (!text[0] || strstr(text, "..")) return;
        char p[PATH_MAX_];
        if (text[0] == '/') snprintf(p, sizeof(p), "%s", text);
        else {
            /* in the folder selected in the explorer, else the open one */
            const char* base = g_folder[0] ? g_folder : "/home/banana";
            char selp[PATH_MAX_];
            if (g_tree_sel >= 0 && g_tree_sel < g_nnodes) {
                snprintf(selp, sizeof(selp), "%s", g_nodes[g_tree_sel].path);
                if (!g_nodes[g_tree_sel].dir) { char* s = strrchr(selp, '/'); if (s) *s = 0; }
                base = selp;
            }
            join_path(p, sizeof(p), base, text);
        }
        if (mode == Q_NEWFOLDER) { mkdir_p(p); set_status("Made %s", text); }
        else if (mode == Q_SAVEAS) {
            doc_t* d = cur_doc();
            if (d) { snprintf(d->path, sizeof(d->path), "%s", p); const char* b = strrchr(p, '/'); snprintf(d->name, sizeof(d->name), "%s", b ? b + 1 : p); d->lang = doc_lang_for(d->name); save_doc(d); }
        } else {
            char dir[PATH_MAX_];
            snprintf(dir, sizeof(dir), "%s", p);
            char* s = strrchr(dir, '/');
            if (s && s != dir) { *s = 0; mkdir_p(dir); }
            if (!read_file(p, NULL)) write_file(p, "", 0);
            open_file(p);
        }
        refresh_tree();
        return;
    }
    if (mode == Q_NEWAPP) {
        snprintf(g_qarg, sizeof(g_qarg), "%s", text);
        q_open(Q_NEWAPP_KIND, "What kind of app?", NULL);
        return;
    }
    if (mode == Q_NEWAPP_KIND) {
        q_close();
        char dir[PATH_MAX_];
        mkdir_p("/home/banana/Projects");
        if (item < 0 || new_project("/home/banana/Projects", g_qarg, !strcmp(g_items[item], "gui"), dir, sizeof(dir)) != 0) {
            set_status("Could not make the app (name: a-z, 0-9, - and _)");
            return;
        }
        open_folder(dir);
        char p[PATH_MAX_];
        join_path(p, sizeof(p), dir, "main.c");
        open_file(p);
        set_status("New app in %s - F5 builds and runs it", dir);
        return;
    }
    if (mode == Q_OPENFOLDER || mode == Q_EXTFOLDER) {
        if (item < 0) return;
        const char* a = g_items[item];
        if (!strcmp(a, ".")) {
            q_close();
            if (mode == Q_OPENFOLDER) open_folder(g_qdir);
            else { char msg[160]; ext_install_folder(g_qdir, msg, sizeof(msg)); set_status("%s", msg); }
            return;
        }
        if (!strcmp(a, "..")) { char* s = strrchr(g_qdir, '/'); if (s && s != g_qdir) *s = 0; else snprintf(g_qdir, sizeof(g_qdir), "/"); }
        else { char p[PATH_MAX_]; join_path(p, sizeof(p), g_qdir, a); snprintf(g_qdir, sizeof(g_qdir), "%s", p); }
        list_dirs();
        return;
    }
    if (mode == Q_CLOSE) {
        q_close();
        if (item < 0) return;
        doc_t* d = cur_doc();
        if (!strcmp(g_items[item], "save")) { if (d && d->path[0]) { doc_save(d); close_doc(g_cur); } else save_doc(d); }
        else if (!strcmp(g_items[item], "discard")) close_doc(g_cur);
        return;
    }
    q_close();
}

/* ── layout ── */
typedef struct { int x, y, w, h; } rect_t;
static rect_t R_side, R_edit, R_tabs, R_text, R_panel, R_right;
static int g_gutter;

static void layout(void) {
    int top = TOP_H, bottom = win.h - STATUS_H;
    int x = ACT_W;
    R_side = (rect_t){ x, top, g_side_on ? g_side_w : 0, bottom - top };
    x += R_side.w;
    int right = g_ext_open >= 0 ? g_right_w : 0;
    if (win.w - x - right < 260) right = 0;
    R_right = (rect_t){ win.w - right, top, right, bottom - top };
    int ew = win.w - right - x;
    int ph = g_panel_on ? g_panel_h : 0;
    if (bottom - top - ph < 120) ph = 0;
    R_edit = (rect_t){ x, top, ew, bottom - top - ph };
    R_panel = (rect_t){ x, bottom - ph, ew, ph };
    R_tabs = (rect_t){ x, top, ew, TAB_H };
    doc_t* d = cur_doc();
    int digits = 3;
    for (int n = d ? d->n : 1; n >= 1000; n /= 10) digits++;
    g_gutter = digits * g_cw + 28;
    R_text = (rect_t){ x + g_gutter, top + TAB_H + (g_find_on ? 0 : 0), ew - g_gutter - 12, R_edit.h - TAB_H };
}

static int text_rows(void) { int r = R_text.h / g_lh; return r < 1 ? 1 : r; }
static int text_cols(void) { int c = R_text.w / g_cw; return c < 1 ? 1 : c; }

static void ensure_visible(doc_t* d) {
    int rows = text_rows(), cols = text_cols();
    if (d->cy < d->top) d->top = d->cy;
    if (d->cy >= d->top + rows) d->top = d->cy - rows + 1;
    int vx = doc_vcol(d, d->cy, d->cx);
    if (vx < d->left) d->left = vx;
    if (vx >= d->left + cols - 1) d->left = vx - cols + 2;
    if (d->top < 0) d->top = 0;
}

/* ── drawing ── */
static void fill(int x, int y, int w, int h, unsigned c) { bwin_fill_rect(&win, x, y, w, h, c); }

static void icon_files(int x, int y, unsigned c) {
    bwin_rect(&win, x + 4, y + 2, 12, 16, c); bwin_rect(&win, x + 8, y + 6, 12, 16, c); fill(x + 9, y + 7, 10, 14, C_ACT);
    bwin_rect(&win, x + 8, y + 6, 12, 16, c);
}
static void icon_search(int x, int y, unsigned c) { bwin_circle(&win, x + 10, y + 10, 7, c); bwin_line(&win, x + 15, y + 15, x + 21, y + 21, c); bwin_line(&win, x + 16, y + 15, x + 22, y + 21, c); }
static void icon_run(int x, int y, unsigned c) { for (int i = 0; i < 9; i++) bwin_line(&win, x + 6, y + 3 + i, x + 6 + (9 - i) * 1, y + 3 + i, c), bwin_line(&win, x + 6, y + 21 - i, x + 6 + (9 - i), y + 21 - i, c); bwin_line(&win, x + 6, y + 12, x + 16, y + 12, c); fill(x + 6, y + 3, 2, 19, c); }
static void icon_ext(int x, int y, unsigned c) { bwin_rect(&win, x + 3, y + 9, 7, 7, c); bwin_rect(&win, x + 11, y + 9, 7, 7, c); bwin_rect(&win, x + 3, y + 17, 7, 7, c); bwin_rect(&win, x + 13, y + 2, 7, 7, c); }

#define MAX_ACT_EXT 6
static int g_act_ext[MAX_ACT_EXT], g_nact_ext;

static void draw_activity(void) {
    fill(0, TOP_H, ACT_W, win.h - TOP_H - STATUS_H, C_ACT);
    void (*icons[4])(int, int, unsigned) = { icon_files, icon_search, icon_run, icon_ext };
    for (int i = 0; i < 4; i++) {
        int y = TOP_H + 6 + i * 46;
        int on = g_side_on && g_view == i;
        if (on) fill(0, y - 4, 2, 40, C_BRIGHT);
        icons[i](11, y + 4, on ? C_BRIGHT : C_DIM);
    }
    /* extensions with a view */
    g_nact_ext = 0;
    for (int e = 0; e < g_next && g_nact_ext < MAX_ACT_EXT; e++) {
        if (!g_ext[e].enabled || !g_ext[e].view[0]) continue;
        int y = TOP_H + 6 + (4 + g_nact_ext) * 46 + 8;
        int on = g_ext_open == e;
        if (on) fill(0, y - 4, 2, 40, C_BRIGHT);
        bwin_fill_circle(&win, 23, y + 16, 13, g_ext[e].color);
        int tw = banana_font_width(BANANA_FONT_SANS_BOLD, 15, g_ext[e].icon);
        ui_bold(23 - tw / 2, y + 7, g_ext[e].icon, 15, C_BRIGHT);
        g_act_ext[g_nact_ext++] = e;
    }
}

static void draw_top(void) {
    fill(0, 0, win.w, TOP_H, C_TOP);
    /* the banana */
    bwin_fill_circle(&win, 16, 15, 8, 0xF4D35Eu);
    bwin_fill_circle(&win, 19, 12, 7, C_TOP);
    const char* btn[] = { "Open Folder", "New App", "Save", "Build", "Run" };
    int x = 34;
    for (int i = 0; i < 5; i++) {
        int w = ui_w(btn[i], 13) + 18;
        unsigned bg = i >= 3 ? 0x0E639Cu : C_TOP;
        fill(x, 4, w, 22, bg);
        bwin_font(&win, x + 9, 8, BANANA_FONT_SANS, 13, btn[i], C_BRIGHT);
        x += w + 4;
    }
    /* the command centre */
    int cw = 360, cx = (win.w - cw) / 2;
    if (cx < x + 10) cx = x + 10;
    if (cx + cw > win.w - 10) cw = win.w - 10 - cx;
    if (cw > 120) {
        fill(cx, 4, cw, 22, 0x2B2B2Bu);
        bwin_rect(&win, cx, 4, cw, 22, C_BORDER);
        const char* f = g_folder[0] ? strrchr(g_folder, '/') + 1 : "Banana Code";
        char t[160];
        snprintf(t, sizeof(t), "%s  -  Search commands (Ctrl+Shift+P)", f && *f ? f : "/");
        ui_text(cx + 10, 8, t, 13, C_DIM, cw - 20);
    }
}

static void draw_side(void) {
    if (!g_side_on) return;
    rect_t r = R_side;
    fill(r.x, r.y, r.w, r.h, C_SIDE);
    static const char* const TITLES[4] = { "EXPLORER", "SEARCH", "RUN AND BUILD", "EXTENSIONS" };
    ui_text(r.x + 18, r.y + 9, TITLES[g_view], 11, C_TEXT, r.w - 30);
    int y = r.y + 34;
    if (g_view == 0) {
        if (!g_folder[0]) {
            ui_text(r.x + 16, y, "You have not yet opened a folder.", 13, C_TEXT, r.w - 24);
            const char* b[3] = { "Open Folder", "New Banana OS App", "Port a Program" };
            for (int i = 0; i < 3; i++) {
                fill(r.x + 16, y + 30 + i * 36, r.w - 32, 26, C_ACCENT);
                int tw = ui_w(b[i], 13);
                bwin_font(&win, r.x + 16 + (r.w - 32 - tw) / 2, y + 35 + i * 36, BANANA_FONT_SANS, 13, b[i], C_BRIGHT);
            }
            return;
        }
        /* folder header with its buttons */
        const char* b = strrchr(g_folder, '/');
        char t[80];
        snprintf(t, sizeof(t), "%s", b && b[1] ? b + 1 : g_folder);
        for (char* c = t; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        ui_bold(r.x + 10, y, t, 12, C_TEXT);
        const char* tb[3] = { "+F", "+D", "R" };
        for (int i = 0; i < 3; i++) {
            int bx = r.x + r.w - 84 + i * 26;
            ui_text(bx + 3, y, tb[i], 12, C_DIM, 24);
        }
        y += 22;
        int rows = (r.y + r.h - y) / ROW_H;
        if (g_tree_top > g_nnodes - rows) g_tree_top = g_nnodes - rows;
        if (g_tree_top < 0) g_tree_top = 0;
        doc_t* d = cur_doc();
        for (int i = 0; i < rows && g_tree_top + i < g_nnodes; i++) {
            node_t* n = &g_nodes[g_tree_top + i];
            int ry = y + i * ROW_H;
            int sel = g_tree_top + i == g_tree_sel || (d && !strcmp(d->path, n->path));
            if (sel) fill(r.x, ry, r.w, ROW_H, g_tree_top + i == g_tree_sel && g_focus != F_EDITOR ? 0x094771u : C_LISTSEL);
            int x = r.x + 12 + n->depth * 12;
            if (n->dir) {
                int open = dir_open(n->path);
                if (open) { for (int k = 0; k < 4; k++) fill(x + k, ry + 9 + k, 8 - 2 * k, 1, C_TEXT); }
                else { for (int k = 0; k < 4; k++) fill(x + 2 + k, ry + 7 + k, 1, 8 - 2 * k, C_TEXT); }
            } else {
                int lang = doc_lang_for(n->name);
                unsigned c = lang == LANG_C ? 0x519ABAu : lang == LANG_JS ? 0xCBCB41u : lang == LANG_HTML ? 0xE37933u :
                             lang == LANG_JSON ? 0xCBCB41u : lang == LANG_CSS ? 0x519ABAu : lang == LANG_MD ? 0x519ABAu : C_DIM;
                fill(x + 1, ry + 6, 8, 10, c);
            }
            ui_text(x + 14, ry + 4, n->name, 13, C_TEXT, r.x + r.w - x - 18);
        }
    } else if (g_view == 1) {
        fill(r.x + 10, y, r.w - 20, 24, C_INPUT);
        if (g_focus == F_SEARCH) bwin_rect(&win, r.x + 10, y, r.w - 20, 24, 0x007FD4u);
        int ex = ui_text(r.x + 16, y + 5, g_search[0] ? g_search : (g_focus == F_SEARCH ? "" : "Search"), 13, g_search[0] ? C_TEXT : C_DIM, r.w - 32);
        if (g_focus == F_SEARCH && (banana_ticks() / 500) % 2 == 0) fill(g_search[0] ? ex + 1 : r.x + 16, y + 5, 1, 15, C_TEXT);
        y += 32;
        if (!g_folder[0]) { ui_text(r.x + 12, y, "Open a folder to search in it.", 13, C_DIM, r.w - 20); return; }
        int rows = (r.y + r.h - y) / 34;
        for (int i = 0; i < rows && g_hits_top + i < g_nhits; i++) {
            hit_t* h = &g_hits[g_hits_top + i];
            int ry = y + i * 34;
            const char* b = strrchr(h->path, '/');
            char t[96];
            snprintf(t, sizeof(t), "%s:%d", b ? b + 1 : h->path, h->line);
            ui_text(r.x + 12, ry, t, 12, C_TEXT, r.w - 20);
            ui_text(r.x + 20, ry + 15, h->text, 12, C_DIM, r.w - 28);
        }
    } else if (g_view == 2) {
        const char* b[4] = { "Build (F7)", "Build and Run (F5)", "New Banana OS App", "Port a Program" };
        for (int i = 0; i < 4; i++) {
            fill(r.x + 16, y + i * 34, r.w - 32, 26, i < 2 ? C_ACCENT : 0x3A3D41u);
            int tw = ui_w(b[i], 13);
            bwin_font(&win, r.x + 16 + (r.w - 32 - tw) / 2, y + 5 + i * 34, BANANA_FONT_SANS, 13, b[i], C_BRIGHT);
        }
        y += 4 * 34 + 10;
        if (g_folder[0] && project_exists()) {
            char p[PATH_MAX_];
            join_path(p, sizeof(p), g_folder, "banana.json");
            char* t = read_file(p, NULL);
            json_t* j = t ? json_parse(t) : NULL;
            free(t);
            char line[160];
            ui_bold(r.x + 16, y, "THIS APP", 11, C_TEXT); y += 20;
            snprintf(line, sizeof(line), "%s %s (%s)", json_str(j, "title", "?"), json_str(j, "version", ""), json_str(j, "type", "console"));
            ui_text(r.x + 16, y, line, 13, C_TEXT, r.w - 24); y += 20;
            json_t* s = json_get(j, "sources");
            int n = 0;
            for (json_t* c = s && s->type == J_ARR ? s->child : NULL; c; c = c->next) n++;
            snprintf(line, sizeof(line), "%d source file%s - package: build/%s.bpk", n, n == 1 ? "" : "s", json_str(j, "name", "app"));
            ui_text(r.x + 16, y, line, 12, C_DIM, r.w - 24); y += 18;
            json_free(j);
        } else if (g_folder[0]) {
            ui_text(r.x + 16, y, "No banana.json here: \"Port a Program\" makes one.", 12, C_DIM, r.w - 24); y += 18;
        }
        y += 10;
        ui_text(r.x + 16, y, "The compiler is TinyCC, inside Banana Code.", 12, C_DIM, r.w - 24); y += 16;
        ui_text(r.x + 16, y, "Apps get the SDK's C library and banana.h.", 12, C_DIM, r.w - 24);
    } else {
        fill(r.x + 16, y, r.w - 32, 26, 0x3A3D41u);
        const char* lb = "Install from Folder...";
        bwin_font(&win, r.x + 16 + (r.w - 32 - ui_w(lb, 13)) / 2, y + 5, BANANA_FONT_SANS, 13, lb, C_BRIGHT);
        y += 36;
        ui_bold(r.x + 16, y, "INSTALLED", 11, C_TEXT);
        y += 20;
        for (int i = 0; i < g_next; i++) {
            ext_t* e = &g_ext[i];
            int cy = y + i * 92;
            if (cy + 90 > r.y + r.h) break;
            bwin_fill_circle(&win, r.x + 30, cy + 20, 14, e->enabled ? e->color : 0x555555u);
            int tw = banana_font_width(BANANA_FONT_SANS_BOLD, 15, e->icon);
            ui_bold(r.x + 30 - tw / 2, cy + 11, e->icon, 15, C_BRIGHT);
            char t[96];
            snprintf(t, sizeof(t), "%s  %s", e->name, e->version);
            ui_bold(r.x + 54, cy + 4, t, 13, e->enabled ? C_BRIGHT : C_DIM);
            ui_text(r.x + 54, cy + 22, e->description, 12, C_DIM, r.w - 64);
            snprintf(t, sizeof(t), "%s%s", e->publisher, e->builtin ? " - built in" : "");
            ui_text(r.x + 54, cy + 38, t, 11, C_DIM, r.w - 64);
            fill(r.x + 54, cy + 58, 76, 22, 0x3A3D41u);
            bwin_font(&win, r.x + 62, cy + 61, BANANA_FONT_SANS, 12, e->enabled ? "Disable" : "Enable", C_BRIGHT);
            if (!e->builtin) {
                fill(r.x + 136, cy + 58, 84, 22, 0x3A3D41u);
                bwin_font(&win, r.x + 144, cy + 61, BANANA_FONT_SANS, 12, "Uninstall", C_BRIGHT);
            }
        }
        if (!g_next) ui_text(r.x + 16, y, "No extensions yet.", 13, C_DIM, r.w - 24);
    }
}

static void draw_tabs(void) {
    rect_t r = R_tabs;
    fill(r.x, r.y, r.w, r.h, 0x252526u);
    int x = r.x;
    for (int i = 0; i < g_ndocs; i++) {
        doc_t* d = g_docs[i];
        int w = ui_w(d->name, 13) + 48;
        if (x + w > r.x + r.w) break;
        int on = i == g_cur;
        fill(x, r.y, w, r.h, on ? C_TABON : C_TAB);
        if (on) fill(x, r.y, w, 1, 0x007ACCu);
        int lang = d->lang;
        unsigned c = lang == LANG_C ? 0x519ABAu : lang == LANG_JS || lang == LANG_JSON ? 0xCBCB41u : lang == LANG_HTML ? 0xE37933u : C_DIM;
        fill(x + 10, r.y + 12, 7, 9, c);
        bwin_font(&win, x + 22, r.y + 8, BANANA_FONT_SANS, 13, d->name, on ? C_BRIGHT : 0x969696u);
        int cx = x + w - 18;
        if (d->dirty) bwin_fill_circle(&win, cx + 4, r.y + 16, 4, on ? C_BRIGHT : 0x969696u);
        else { bwin_line(&win, cx, r.y + 12, cx + 8, r.y + 20, 0x969696u); bwin_line(&win, cx + 8, r.y + 12, cx, r.y + 20, 0x969696u); }
        fill(x + w - 1, r.y, 1, r.h, 0x252526u);
        x += w;
    }
}

static void draw_welcome(void) {
    rect_t r = R_edit;
    fill(r.x, r.y + TAB_H, r.w, r.h - TAB_H, C_BG);
    int x = r.x + 50, y = r.y + TAB_H + 40;
    if (r.w < 300) return;
    bwin_font(&win, x, y, BANANA_FONT_SANS, 34, "Banana Code", 0xE8E8E8u);
    bwin_font(&win, x, y + 44, BANANA_FONT_SANS, 16, "Write, build and port apps on Banana OS", C_DIM);
    y += 90;
    ui_bold(x, y, "Start", 15, C_TEXT);
    const char* s[5] = { "New Banana OS App...", "Open Folder...", "Port a Program (C) to Banana OS...", "Go to File... (Ctrl+P)", "Show All Commands (Ctrl+Shift+P)" };
    for (int i = 0; i < 5; i++) bwin_font(&win, x, y + 28 + i * 24, BANANA_FONT_SANS, 14, s[i], 0x3794FFu);
    y += 28 + 5 * 24 + 20;
    ui_bold(x, y, "Extensions", 15, C_TEXT);
    char t[160];
    for (int i = 0; i < g_next && i < 4; i++) {
        snprintf(t, sizeof(t), "%s - %s", g_ext[i].name, g_ext[i].description);
        ui_text(x, y + 28 + i * 22, t, 13, C_DIM, r.w - 100);
    }
    if (!g_next) ui_text(x, y + 28, "None installed.", 13, C_DIM, 400);
}

static void draw_editor(void) {
    doc_t* d = cur_doc();
    draw_tabs();
    if (!d) { draw_welcome(); return; }
    rect_t r = R_edit;
    fill(r.x, r.y + TAB_H, r.w, r.h - TAB_H, C_BG);
    /* breadcrumb-like path under the tabs is skipped: more room for code */
    int rows = text_rows();
    if (d->top > d->n - 1) d->top = d->n - 1 > 0 ? d->n - 1 : 0;
    int y0s, x0s, y1s, x1s, has = doc_has_sel(d);
    if (has) doc_sel_range(d, &y0s, &x0s, &y1s, &x1s);
    static unsigned char col[65536];
    int clip1 = R_text.x + R_text.w + 2;
    for (int i = 0; i <= rows && d->top + i < d->n; i++) {
        int y = d->top + i;
        int py = R_text.y + i * g_lh;
        if (py >= R_edit.y + R_edit.h) break;
        line_t* l = &d->lines[y];
        if (y == d->cy && !has) fill(r.x, py, r.w - 12, g_lh, C_CURLINE);
        /* the line number */
        char num[12];
        snprintf(num, sizeof(num), "%d", y + 1);
        int nl = (int)strlen(num);
        for (int k = 0; k < nl; k++) put_char(r.x + g_gutter - 18 - (nl - k) * g_cw, py, (unsigned char)num[k], y == d->cy ? 0xC6C6C6u : C_DIM, r.x, r.x + g_gutter, R_edit.y + R_edit.h);
        if (l->len < (int)sizeof(col)) doc_highlight(d, y, col);
        else memset(col, HL_TEXT, sizeof(col));
        /* the selection on this line */
        int sa = -1, sb = -1;
        if (has && y >= y0s && y <= y1s) { sa = y == y0s ? x0s : 0; sb = y == y1s ? x1s : l->len + 1; }
        int vx = 0;
        for (int b = 0; b <= l->len; ) {
            unsigned char c = b < l->len ? (unsigned char)l->s[b] : ' ';
            int adv = 1, cl = 1;
            if (c == '\t') adv = 4 - vx % 4;
            else if (c >= 0xC0) { while (b + cl < l->len && ((unsigned char)l->s[b + cl] & 0xC0) == 0x80) cl++; }
            int sx = R_text.x + (vx - d->left) * g_cw;
            if (sa >= 0 && b >= sa && b < sb && vx >= d->left) fill(sx, py, adv * g_cw, g_lh, C_SEL);
            if (b == l->len) break;
            if (vx >= d->left && sx < clip1) {
                if (c >= 0xC0) {
                    char u[8];
                    memcpy(u, l->s + b, (size_t)(cl < 7 ? cl : 7));
                    u[cl < 7 ? cl : 7] = 0;
                    __banana->font_draw(win.px, win.w, clip1 < win.w ? clip1 : win.w, win.h, sx, py + 2, BANANA_FONT_MONO, ED_PX, u, HL_COL[col[b]]);
                } else if (c > ' ') put_char(sx, py, c, HL_COL[col[b] < HL_COUNT ? col[b] : 0], R_text.x - 2, clip1, R_edit.y + R_edit.h);
            }
            vx += adv;
            b += cl;
        }
        /* the cursor */
        if (y == d->cy && (g_focus == F_EDITOR || g_focus == F_FIND)) {
            int cvx = doc_vcol(d, y, d->cx) - d->left;
            if (cvx >= 0 && (banana_ticks() / 530) % 2 == 0) fill(R_text.x + cvx * g_cw, py + 1, 2, g_lh - 2, 0xAEAFADu);
        }
    }
    /* the scroll bar, with the problems marked */
    int sbx = r.x + r.w - 12, sbh = r.h - TAB_H;
    fill(sbx, r.y + TAB_H, 12, sbh, 0x252526u);
    if (d->n > rows) {
        int th = sbh * rows / d->n;
        if (th < 20) th = 20;
        int ty = r.y + TAB_H + (sbh - th) * d->top / (d->n - rows > 0 ? d->n - rows : 1);
        fill(sbx + 2, ty, 8, th, 0x4E4E4Eu);
    }
    for (int k = 0; k < g_nproblems; k++)
        if (!strcmp(g_problems[k].file, d->path) && g_problems[k].line > 0)
            fill(sbx + 1, r.y + TAB_H + sbh * (g_problems[k].line - 1) / (d->n > 0 ? d->n : 1), 10, 3, g_problems[k].is_error ? C_ERR : C_WARN);
    /* squiggles under lines with problems */
    for (int k = 0; k < g_nproblems; k++) {
        if (strcmp(g_problems[k].file, d->path)) continue;
        int y = g_problems[k].line - 1 - d->top;
        if (y < 0 || y >= rows) continue;
        line_t* l = &d->lines[g_problems[k].line - 1];
        int w = doc_vcol(d, g_problems[k].line - 1, l->len) - d->left;
        int py = R_text.y + y * g_lh + g_lh - 2;
        for (int x = 0; x < w * g_cw && R_text.x + x < clip1; x += 2) fill(R_text.x + x, py + ((x / 2) & 1), 2, 1, g_problems[k].is_error ? C_ERR : C_WARN);
    }

    /* the find bar */
    if (g_find_on) {
        int fw = 380, fx = r.x + r.w - fw - 20, fy = r.y + TAB_H;
        int fh = g_replace_on ? 64 : 36;
        fill(fx, fy, fw, fh, 0x252526u);
        bwin_rect(&win, fx, fy, fw, fh, C_BORDER);
        for (int f = 0; f < (g_replace_on ? 2 : 1); f++) {
            int iy = fy + 6 + f * 28;
            fill(fx + 8, iy, 250, 24, C_INPUT);
            if (g_focus == F_FIND && g_find_field == f) bwin_rect(&win, fx + 8, iy, 250, 24, 0x007FD4u);
            const char* v = f ? g_repl : g_find;
            int ex = ui_text(fx + 14, iy + 5, v[0] ? v : (f ? "Replace" : "Find"), 13, v[0] ? C_TEXT : C_DIM, 236);
            if (g_focus == F_FIND && g_find_field == f && (banana_ticks() / 500) % 2 == 0) fill(v[0] ? ex + 1 : fx + 14, iy + 5, 1, 15, C_TEXT);
        }
        ui_text(fx + 268, fy + 11, g_replace_on ? "Enter: all" : "Enter / F3", 12, C_DIM, 100);
    }
}

static void draw_panel(void) {
    if (!R_panel.h) return;
    rect_t r = R_panel;
    fill(r.x, r.y, r.w, r.h, C_PANEL);
    fill(r.x, r.y, r.w, 1, C_BORDER);
    char t[48];
    int ne = 0, nw = 0;
    for (int k = 0; k < g_nproblems; k++) { if (g_problems[k].is_error) ne++; else nw++; }
    snprintf(t, sizeof(t), "PROBLEMS  %d", g_nproblems);
    const char* names[2] = { t, "OUTPUT" };
    int x = r.x + 16;
    for (int i = 0; i < 2; i++) {
        int on = g_panel_tab == i;
        bwin_font(&win, x, r.y + 8, BANANA_FONT_SANS, 11, names[i], on ? C_BRIGHT : C_DIM);
        int w = ui_w(names[i], 11);
        if (on) fill(x, r.y + 25, w, 1, C_BRIGHT);
        x += w + 26;
    }
    int y = r.y + PANEL_TABS + 4;
    int rows = (r.y + r.h - y) / 18;
    if (g_panel_tab == 0) {
        if (!g_nproblems) ui_text(r.x + 16, y, "No problems have been detected.", 13, C_DIM, r.w - 30);
        for (int k = 0; k < g_nproblems && k < rows; k++) {
            problem_t* p = &g_problems[k];
            int ry = y + k * 18;
            bwin_fill_circle(&win, r.x + 22, ry + 8, 5, p->is_error ? C_ERR : C_WARN);
            const char* b = strrchr(p->file, '/');
            char line[320];
            snprintf(line, sizeof(line), "%s   %s:%d", p->msg, b ? b + 1 : p->file, p->line);
            ui_text(r.x + 34, ry + 1, line, 13, C_TEXT, r.w - 50);
        }
    } else {
        /* the last lines of the output */
        const char* s = g_output.s ? g_output.s : "";
        int nlines = 0;
        for (const char* c = s; *c; c++) if (*c == '\n') nlines++;
        int skip = nlines - rows + 1;
        const char* p = s;
        while (skip > 0 && *p) { if (*p == '\n') skip--; p++; }
        int ry = y;
        while (*p && ry + 16 <= r.y + r.h) {
            const char* e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            char line[300];
            if (n > 299) n = 299;
            memcpy(line, p, (size_t)n);
            line[n] = 0;
            unsigned c = strstr(line, "error") ? C_ERR : strstr(line, "warning") ? C_WARN : C_TEXT;
            bwin_font(&win, r.x + 16, ry, BANANA_FONT_MONO, 12, line, c);
            ry += 18;
            if (!e) break;
            p = e + 1;
        }
    }
}

static void draw_right(void) {
    if (!R_right.w || g_ext_open < 0) return;
    rect_t r = R_right;
    ext_t* e = &g_ext[g_ext_open];
    fill(r.x, r.y, r.w, 30, C_SIDE);
    fill(r.x, r.y, 1, r.h, C_BORDER);
    char t[80];
    snprintf(t, sizeof(t), "%s", e->name);
    for (char* c = t; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    ui_text(r.x + 14, r.y + 9, t, 11, C_TEXT, r.w - 50);
    bwin_line(&win, r.x + r.w - 22, r.y + 10, r.x + r.w - 12, r.y + 20, C_DIM);
    bwin_line(&win, r.x + r.w - 12, r.y + 10, r.x + r.w - 22, r.y + 20, C_DIM);
    if (e->webview >= 0) bweb_draw(e->webview, &win, r.x + 1, r.y + 30);
    if (g_focus == F_EXT) fill(r.x + 1, r.y + 29, r.w - 1, 1, 0x007FD4u);
}

static void draw_status(void) {
    int y = win.h - STATUS_H;
    fill(0, y, win.w, STATUS_H, g_folder[0] ? C_STATUS : 0x68217Au);
    int ne = 0, nw = 0;
    for (int k = 0; k < g_nproblems; k++) { if (g_problems[k].is_error) ne++; else nw++; }
    char t[200];
    snprintf(t, sizeof(t), "Banana OS   x %d   ! %d", ne, nw);
    int x = bwin_font(&win, 10, y + 4, BANANA_FONT_SANS, 12, t, C_BRIGHT) + 16;
    if (g_status[0] && banana_ticks() - g_status_ms < 8000) ui_text(x, y + 4, g_status, 12, C_BRIGHT, win.w / 2 - x);
    doc_t* d = cur_doc();
    if (d) {
        snprintf(t, sizeof(t), "Ln %d, Col %d    Spaces: 4    UTF-8    %s", d->cy + 1, doc_vcol(d, d->cy, d->cx) + 1, doc_lang_name(d->lang));
        int w = ui_w(t, 12);
        bwin_font(&win, win.w - w - 14, y + 4, BANANA_FONT_SANS, 12, t, C_BRIGHT);
    }
}

static void draw_quick(void) {
    if (g_q == Q_NONE) return;
    int w = 560 < win.w - 40 ? 560 : win.w - 40, x = (win.w - w) / 2, y = TOP_H + 4;
    int list = g_q == Q_PALETTE || g_q == Q_FILES || g_q == Q_OPENFOLDER || g_q == Q_EXTFOLDER || g_q == Q_NEWAPP_KIND || g_q == Q_CLOSE;
    int rows = list ? (g_nshown < 12 ? g_nshown : 12) : 0;
    int h = 64 + rows * 24 + (list ? 4 : 0);
    fill(x + 3, y + 3, w, h, 0x101010u);
    fill(x, y, w, h, 0x252526u);
    bwin_rect(&win, x, y, w, h, C_BORDER);
    ui_text(x + 10, y + 6, g_qtitle, 12, C_DIM, w - 20);
    int editable = g_q != Q_OPENFOLDER && g_q != Q_EXTFOLDER && g_q != Q_NEWAPP_KIND && g_q != Q_CLOSE;
    fill(x + 8, y + 26, w - 16, 26, C_INPUT);
    bwin_rect(&win, x + 8, y + 26, w - 16, 26, 0x007FD4u);
    int ex = ui_text(x + 14, y + 31, g_qtext, 14, editable ? C_TEXT : C_DIM, w - 30);
    if (editable && (banana_ticks() / 500) % 2 == 0) fill(ex + 1, y + 30, 1, 17, C_TEXT);
    if (g_qsel < g_qtop) g_qtop = g_qsel;
    if (g_qsel >= g_qtop + 12) g_qtop = g_qsel - 11;
    for (int i = 0; i < rows; i++) {
        int k = g_qtop + i;
        if (k >= g_nshown) break;
        int it = g_shown[k];
        int ry = y + 58 + i * 24;
        if (k == g_qsel) fill(x + 2, ry, w - 4, 24, 0x04395Eu);
        ui_text(x + 14, ry + 4, g_item_label[it], 13, C_TEXT, w - 150);
        if (g_item_key[it][0]) {
            int kw = ui_w(g_item_key[it], 12);
            bwin_font(&win, x + w - kw - 14, ry + 5, BANANA_FONT_SANS, 12, g_item_key[it], C_DIM);
        }
    }
    if (list && !g_nshown) ui_text(x + 14, y + 60, "No matching results", 13, C_DIM, w - 30);
}

static void redraw(void) {
    layout();
    doc_t* d = cur_doc();
    if (d) ensure_visible(d);
    fill(0, 0, win.w, win.h, C_BG);
    draw_top();
    draw_activity();
    draw_side();
    draw_editor();
    draw_panel();
    draw_right();
    draw_status();
    draw_quick();
    bwin_update(&win);
    g_ui_dirty = 0;
}

/* ── input ── */
static int g_mx, g_my, g_drag, g_drag_sb;
static unsigned g_click_ms;
static int g_click_y, g_click_x, g_clicks;

static void text_edit(char* buf, int cap, int k) {
    int n = (int)strlen(buf);
    if (k == 8 || k == 127) { if (n) { n--; while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--; buf[n] = 0; } }
    else if (k >= 32 && k < 256 && n < cap - 1) { buf[n] = (char)k; buf[n + 1] = 0; }
}

static void editor_key(doc_t* d, int k, int mods) {
    int shift = mods & BANANA_MOD_SHIFT, ctrl = mods & BANANA_MOD_CTRL;
    int rows = text_rows();
    switch (k) {
    case BANANA_KEY_LEFT: if (ctrl) doc_word(d, -1, shift); else doc_move(d, 0, -1, shift); return;
    case BANANA_KEY_RIGHT: if (ctrl) doc_word(d, 1, shift); else doc_move(d, 0, 1, shift); return;
    case BANANA_KEY_UP: if (ctrl) { if (d->top > 0) d->top--; } else doc_move(d, -1, 0, shift); return;
    case BANANA_KEY_DOWN: if (ctrl) { if (d->top < d->n - 1) d->top++; } else doc_move(d, 1, 0, shift); return;
    case BANANA_KEY_HOME: if (ctrl) doc_goto(d, 0, 0, shift); else doc_home(d, shift); return;
    case BANANA_KEY_END: if (ctrl) doc_goto(d, d->n - 1, d->lines[d->n - 1].len, shift); else doc_end(d, shift); return;
    case BANANA_KEY_PGUP: doc_move(d, -rows, 0, shift); d->top -= rows; if (d->top < 0) d->top = 0; return;
    case BANANA_KEY_PGDN: doc_move(d, rows, 0, shift); d->top += rows; return;
    case BANANA_KEY_DELETE: doc_snapshot(d, OP_DEL); doc_delete(d); return;
    }
    if (k == 8 || k == 127) { doc_snapshot(d, OP_DEL); doc_backspace(d); return; }
    if (k == '\n' || k == '\r') { doc_snapshot(d, OP_OTHER); doc_newline(d); return; }
    if (k == '\t') {
        doc_snapshot(d, OP_OTHER);
        if (doc_has_sel(d) || shift) doc_indent(d, shift);
        else { int vx = doc_vcol(d, d->cy, d->cx); int n = 4 - vx % 4; doc_insert(d, "    ", n); }
        return;
    }
    if (k >= 32 && k < 256) {
        char c = (char)k;
        doc_snapshot(d, OP_TYPE);
        /* closing brackets and quotes: typed over when they are next */
        line_t* l = &d->lines[d->cy];
        if (!doc_has_sel(d) && strchr(")]}\"'", c) && d->cx < l->len && l->s[d->cx] == c) { d->cx++; return; }
        doc_insert(d, &c, 1);
        const char* pairs = "()[]{}";
        const char* p = strchr(pairs, c);
        if (p && (p - pairs) % 2 == 0 && d->lang != LANG_TEXT && d->lang != LANG_MD) {
            l = &d->lines[d->cy];
            if (d->cx >= l->len || strchr(" )]};,", l->s[d->cx])) { char cl = p[1]; int cx = d->cx; doc_insert(d, &cl, 1); d->cx = cx; }
        }
        /* a '}' on a line of spaces: one level less */
        if (c == '}') {
            l = &d->lines[d->cy];
            int only = 1;
            for (int i = 0; i < d->cx - 1; i++) if (l->s[i] != ' ') only = 0;
            if (only && d->cx - 1 >= 4) { int cx = d->cx; d->cx = 0; doc_indent(d, 1); d->cx = cx - 4; }
        }
    }
}

static void copy_sel(doc_t* d, int cut) {
    if (!d) return;
    char* s;
    if (!doc_has_sel(d)) {                 /* no selection: the whole line */
        line_t* l = &d->lines[d->cy];
        s = malloc((size_t)l->len + 2);
        if (!s) return;
        memcpy(s, l->s, (size_t)l->len);
        s[l->len] = '\n';
        s[l->len + 1] = 0;
        __banana->clipboard_set(s, (unsigned long)l->len + 1);
        if (cut) { doc_snapshot(d, OP_OTHER); d->sy = d->cy; d->sx = 0; if (d->cy + 1 < d->n) { d->cy++; d->cx = 0; } else d->cx = l->len; doc_delete_sel(d); }
        free(s);
        return;
    }
    s = doc_sel_text(d);
    if (s) __banana->clipboard_set(s, strlen(s));
    free(s);
    if (cut) { doc_snapshot(d, OP_OTHER); doc_delete_sel(d); }
}

static void paste(doc_t* d) {
    unsigned long n = 0;
    const char* t = __banana->clipboard_get(&n);
    if (!d || !t || !n) return;
    doc_snapshot(d, OP_OTHER);
    doc_insert(d, t, (int)n);
}

/* Ctrl + a letter (the terminal-style control code) */
static int ctrl_command(int k, int mods) {
    int shift = mods & BANANA_MOD_SHIFT;
    doc_t* d = cur_doc();
    char letter = k >= 1 && k <= 26 ? (char)('a' + k - 1) : (char)k;
    if (letter >= 'A' && letter <= 'Z') letter = (char)(letter + 32);
    if (shift) {
        switch (letter) {
        case 'p': q_open(Q_PALETTE, "Command Palette", ""); return 1;
        case 'e': run_builtin_command("view.explorer"); return 1;
        case 'f': g_side_on = 1; g_view = 1; g_focus = F_SEARCH; return 1;
        case 'd': run_builtin_command("view.build"); return 1;
        case 'x': run_builtin_command("view.extensions"); return 1;
        case 'm': run_builtin_command("view.problems"); return 1;
        case 'u': run_builtin_command("view.output"); return 1;
        case 'b': run_builtin_command("build.build"); return 1;
        case 'z': if (d) doc_redo(d); return 1;
        }
        return 0;
    }
    switch (letter) {
    case 's': save_doc(d); return 1;
    case 'o': case 'k': run_builtin_command("file.openFolder"); return 1;
    case 'n': run_builtin_command("file.newFile"); return 1;
    case 'w': run_builtin_command("file.close"); return 1;
    case 'p': run_builtin_command("file.quickOpen"); return 1;
    case 'z': if (d) doc_undo(d); return 1;
    case 'y': if (d) doc_redo(d); return 1;
    case 'f': run_builtin_command("edit.find"); return 1;
    case 'h': run_builtin_command("edit.replace"); return 1;
    case 'g': run_builtin_command("edit.goto"); return 1;
    case 'd': run_builtin_command("edit.duplicate"); return 1;
    case 'a': if (d) doc_select_all(d); return 1;
    case 'c': copy_sel(d, 0); return 1;
    case 'x': copy_sel(d, 1); return 1;
    case 'v': paste(d); return 1;
    case 'b': run_builtin_command("view.sidebar"); return 1;
    case 'j': run_builtin_command("view.panel"); return 1;
    case '/': case '_': run_builtin_command("edit.comment"); return 1;
    }
    return 0;
}

static void on_key(int k) {
    int mods = banana_key_mods();
    int ctrl = mods & BANANA_MOD_CTRL;
    doc_t* d = cur_doc();
    if (k == BANANA_KEY_F1) { q_open(Q_PALETTE, "Command Palette", ""); return; }
    if (k == BANANA_KEY_F1 + 4) { run_builtin_command("build.run"); return; }       /* F5 */
    if (k == BANANA_KEY_F1 + 6) { run_builtin_command("build.build"); return; }     /* F7 */
    if (k == BANANA_KEY_F1 + 2) { if (d && g_find[0]) doc_find(d, g_find, (mods & BANANA_MOD_SHIFT) ? -1 : 1, 0); return; }   /* F3 */

    if (g_focus == F_QUICK) {
        if (k == 27) { q_close(); return; }
        if (k == '\n' || k == '\r') { q_accept(); return; }
        if (k == BANANA_KEY_DOWN) { if (g_qsel < g_nshown - 1) g_qsel++; return; }
        if (k == BANANA_KEY_UP) { if (g_qsel > 0) g_qsel--; return; }
        if (k == BANANA_KEY_PGDN) { g_qsel += 11; if (g_qsel >= g_nshown) g_qsel = g_nshown - 1; return; }
        if (k == BANANA_KEY_PGUP) { g_qsel -= 11; if (g_qsel < 0) g_qsel = 0; return; }
        int editable = g_q != Q_OPENFOLDER && g_q != Q_EXTFOLDER && g_q != Q_NEWAPP_KIND && g_q != Q_CLOSE;
        if (editable && !ctrl) { text_edit(g_qtext, sizeof(g_qtext), k); q_filter(); }
        if (!editable && k == 8 && (g_q == Q_OPENFOLDER || g_q == Q_EXTFOLDER)) {     /* Backspace: up */
            char* s = strrchr(g_qdir, '/'); if (s && s != g_qdir) *s = 0; else snprintf(g_qdir, sizeof(g_qdir), "/"); list_dirs();
        }
        return;
    }
    if (ctrl && k != 27 && ctrl_command(k, mods)) return;
    if (g_focus == F_FIND) {
        if (k == 27) { g_find_on = 0; g_focus = F_EDITOR; return; }
        if (k == '\t') { if (g_replace_on) g_find_field ^= 1; return; }
        if (k == '\n' || k == '\r') {
            if (!d) return;
            if (g_replace_on && g_find_field == 1) { int n = doc_replace_all(d, g_find, g_repl, 0); set_status("Replaced %d", n); }
            else if (!doc_find(d, g_find, (mods & BANANA_MOD_SHIFT) ? -1 : 1, 0)) set_status("No results for \"%s\"", g_find);
            return;
        }
        text_edit(g_find_field ? g_repl : g_find, 128, k);
        return;
    }
    if (g_focus == F_SEARCH) {
        if (k == 27) { g_focus = F_EDITOR; return; }
        if (k == '\n' || k == '\r') { run_search(); return; }
        text_edit(g_search, sizeof(g_search), k);
        return;
    }
    if (g_focus == F_EXT && g_ext_open >= 0 && g_ext[g_ext_open].webview >= 0) {
        if (k == 27 && !ctrl) { g_focus = F_EDITOR; return; }
        banana_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = BANANA_EV_KEY;
        ev.key = k;
        bweb_event(g_ext[g_ext_open].webview, &ev);
        return;
    }
    if (k == 27) { if (d) doc_clear_sel(d); g_find_on = 0; return; }
    if (d) editor_key(d, k, mods);
}

/* the text position under (x, y) */
static void hit_text(doc_t* d, int x, int y, int* ly, int* lx) {
    int row = (y - R_text.y) / g_lh + d->top;
    if (y < R_text.y) row = d->top - 1;
    if (row < 0) row = 0;
    if (row >= d->n) row = d->n - 1;
    int vx = (x - R_text.x + g_cw / 2) / g_cw + d->left;
    if (vx < 0) vx = 0;
    *ly = row;
    *lx = doc_byte_at(d, row, vx);
}

static void click_side(int x, int y, int button) {
    rect_t r = R_side;
    int top = r.y + 34;
    if (g_view == 0) {
        if (!g_folder[0]) {
            for (int i = 0; i < 3; i++)
                if (inside(x, y, r.x + 16, top + 30 + i * 36, r.w - 32, 26))
                    run_builtin_command(i == 0 ? "file.openFolder" : i == 1 ? "build.newApp" : "build.port");
            return;
        }
        if (y < top + 20) {
            int b = (x - (r.x + r.w - 84)) / 26;
            if (x >= r.x + r.w - 84 && b >= 0 && b < 3) run_builtin_command(b == 0 ? "file.newFile" : b == 1 ? "file.newFolder" : "ext.__refresh");
            if (b == 2) refresh_tree();
            return;
        }
        int i = g_tree_top + (y - top - 22) / ROW_H;
        if (i < 0 || i >= g_nnodes) { g_tree_sel = -1; return; }
        g_tree_sel = i;
        if (g_nodes[i].dir) { toggle_dir(g_nodes[i].path); refresh_tree(); }
        else if (button == 1) open_file(g_nodes[i].path);
    } else if (g_view == 1) {
        if (y < top + 26) { g_focus = F_SEARCH; return; }
        int i = g_hits_top + (y - top - 32) / 34;
        if (i >= 0 && i < g_nhits) {
            doc_t* d = open_file(g_hits[i].path);
            if (d) { doc_goto(d, g_hits[i].line - 1, 0, 0); d->top = d->cy - 8 > 0 ? d->cy - 8 : 0; int n = (int)strlen(g_search); char* f = strstr(d->lines[d->cy].s, g_search); if (f) { d->sy = d->cy; d->sx = (int)(f - d->lines[d->cy].s); d->cx = d->sx + n; } }
        }
    } else if (g_view == 2) {
        const char* c[4] = { "build.build", "build.run", "build.newApp", "build.port" };
        for (int i = 0; i < 4; i++) if (inside(x, y, r.x + 16, top + i * 34, r.w - 32, 26)) run_builtin_command(c[i]);
    } else {
        if (inside(x, y, r.x + 16, top, r.w - 32, 26)) { run_builtin_command("ext.install"); return; }
        int ly = top + 56;
        for (int i = 0; i < g_next; i++) {
            int cy = ly + i * 92;
            if (inside(x, y, r.x + 54, cy + 58, 76, 22)) { ext_set_enabled(i, !g_ext[i].enabled); set_status("%s %s", g_ext[i].name, g_ext[i].enabled ? "enabled" : "disabled"); return; }
            if (!g_ext[i].builtin && inside(x, y, r.x + 136, cy + 58, 84, 22)) { char m[128]; ext_uninstall(i, m, sizeof(m)); set_status("%s", m); return; }
        }
    }
}

static void forward_ext(const banana_event_t* ev, int x, int y) {
    if (g_ext_open < 0 || g_ext[g_ext_open].webview < 0) return;
    banana_event_t e = *ev;
    e.x = x - (R_right.x + 1);
    e.y = y - (R_right.y + 30);
    bweb_event(g_ext[g_ext_open].webview, &e);
}

static void on_mouse(const banana_event_t* ev) {
    int x = ev->x, y = ev->y;
    g_mx = x; g_my = y;
    doc_t* d = cur_doc();
    if (ev->type == BANANA_EV_MOUSE_UP) {
        if (g_focus == F_EXT && inside(x, y, R_right.x, R_right.y + 30, R_right.w, R_right.h - 30)) forward_ext(ev, x, y);
        g_drag = 0; g_drag_sb = 0;
        return;
    }
    if (ev->type == BANANA_EV_MOUSE_MOVE) {
        if (g_drag && d && (ev->buttons & 1)) {
            int ly, lx;
            hit_text(d, x, y, &ly, &lx);
            if (d->sy < 0) { d->sy = d->cy; d->sx = d->cx; }
            d->cy = ly; d->cx = lx;
            g_ui_dirty = 1;
        } else if (g_drag_sb && d && (ev->buttons & 1)) {
            int sbh = R_edit.h - TAB_H;
            d->top = (y - R_edit.y - TAB_H) * d->n / (sbh > 0 ? sbh : 1) - text_rows() / 2;
            if (d->top < 0) d->top = 0;
            if (d->top > d->n - 1) d->top = d->n - 1;
            g_ui_dirty = 1;
        } else if (inside(x, y, R_right.x, R_right.y + 30, R_right.w, R_right.h - 30)) forward_ext(ev, x, y);
        return;
    }
    if (ev->type != BANANA_EV_MOUSE_DOWN) return;
    g_ui_dirty = 1;
    /* the quick input: a click outside closes it */
    if (g_q != Q_NONE) {
        int w = 560 < win.w - 40 ? 560 : win.w - 40, qx = (win.w - w) / 2, qy = TOP_H + 4;
        if (inside(x, y, qx, qy + 58, w, 12 * 24)) {
            int k = g_qtop + (y - qy - 58) / 24;
            if (k >= 0 && k < g_nshown) { g_qsel = k; q_accept(); }
            return;
        }
        if (!inside(x, y, qx, qy, w, 60)) q_close();
        return;
    }
    /* the top bar */
    if (y < TOP_H) {
        const char* btn[] = { "Open Folder", "New App", "Save", "Build", "Run" };
        const char* cmd[] = { "file.openFolder", "build.newApp", "file.save", "build.build", "build.run" };
        int bx = 34;
        for (int i = 0; i < 5; i++) {
            int w = ui_w(btn[i], 13) + 18;
            if (x >= bx && x < bx + w) { run_builtin_command(cmd[i]); return; }
            bx += w + 4;
        }
        if (x > win.w / 2 - 200 && x < win.w / 2 + 200) q_open(Q_PALETTE, "Command Palette", "");
        return;
    }
    /* the activity bar */
    if (x < ACT_W && y < win.h - STATUS_H) {
        int i = (y - TOP_H - 2) / 46;
        if (i >= 0 && i < 4) { show_view(i); return; }
        for (int k = 0; k < g_nact_ext; k++) {
            int iy = TOP_H + 6 + (4 + k) * 46 + 8;
            if (y >= iy - 4 && y < iy + 36) {
                int e = g_act_ext[k];
                if (g_ext_open == e) { ext_close_view(); g_focus = F_EDITOR; }
                else { open_ext_panel(e); g_focus = F_EXT; }
                return;
            }
        }
        return;
    }
    if (g_side_on && inside(x, y, R_side.x, R_side.y, R_side.w, R_side.h)) { if (g_focus != F_SEARCH || g_view != 1) g_focus = F_EDITOR; click_side(x, y, ev->button); return; }
    if (R_right.w && inside(x, y, R_right.x, R_right.y, R_right.w, R_right.h)) {
        if (y < R_right.y + 30) { if (x > R_right.x + R_right.w - 30) { ext_close_view(); g_focus = F_EDITOR; } return; }
        g_focus = F_EXT;
        forward_ext(ev, x, y);
        return;
    }
    if (R_panel.h && inside(x, y, R_panel.x, R_panel.y, R_panel.w, R_panel.h)) {
        if (y < R_panel.y + PANEL_TABS) {
            char t[48];
            snprintf(t, sizeof(t), "PROBLEMS  %d", g_nproblems);
            int w0 = ui_w(t, 11);
            g_panel_tab = x < R_panel.x + 16 + w0 + 13 ? 0 : 1;
            return;
        }
        if (g_panel_tab == 0) {
            int k = (y - R_panel.y - PANEL_TABS - 4) / 18;
            if (k >= 0 && k < g_nproblems && g_problems[k].file[0]) {
                doc_t* od = open_file(g_problems[k].file);
                if (od && g_problems[k].line > 0) { doc_goto(od, g_problems[k].line - 1, 0, 0); od->top = od->cy - 8 > 0 ? od->cy - 8 : 0; }
            }
        }
        return;
    }
    /* tabs */
    if (inside(x, y, R_tabs.x, R_tabs.y, R_tabs.w, R_tabs.h)) {
        int tx = R_tabs.x;
        for (int i = 0; i < g_ndocs; i++) {
            int w = ui_w(g_docs[i]->name, 13) + 48;
            if (x >= tx && x < tx + w) {
                if (x >= tx + w - 22) { g_cur = i; run_builtin_command("file.close"); }
                else g_cur = i;
                g_focus = F_EDITOR;
                return;
            }
            tx += w;
        }
        return;
    }
    if (!d) {
        /* the welcome page's links */
        int wy = R_edit.y + TAB_H + 40 + 90 + 28;
        const char* c[5] = { "build.newApp", "file.openFolder", "build.port", "file.quickOpen", "__palette" };
        for (int i = 0; i < 5; i++)
            if (inside(x, y, R_edit.x + 50, wy + i * 24, 320, 22)) {
                if (i == 4) q_open(Q_PALETTE, "Command Palette", ""); else run_builtin_command(c[i]);
                return;
            }
        return;
    }
    g_focus = F_EDITOR;
    if (x >= R_edit.x + R_edit.w - 12) { g_drag_sb = 1; return; }
    if (g_find_on && inside(x, y, R_edit.x + R_edit.w - 400, R_edit.y + TAB_H, 380, g_replace_on ? 64 : 36)) {
        g_focus = F_FIND;
        g_find_field = g_replace_on && y >= R_edit.y + TAB_H + 34;
        return;
    }
    int ly, lx;
    hit_text(d, x, y, &ly, &lx);
    unsigned now = banana_ticks();
    if (now - g_click_ms < 450 && g_click_y == ly && abs(g_click_x - lx) <= 1) g_clicks++;
    else g_clicks = 1;
    g_click_ms = now; g_click_y = ly; g_click_x = lx;
    int shift = banana_key_mods() & BANANA_MOD_SHIFT;
    doc_goto(d, ly, lx, shift);
    if (g_clicks == 2) doc_select_word(d);
    else if (g_clicks >= 3) { d->sy = ly; d->sx = 0; d->cx = d->lines[ly].len; }
    else g_drag = 1;
}

static void on_wheel(int dz) {
    int x = g_mx, y = g_my;
    doc_t* d = cur_doc();
    if (g_q != Q_NONE) { g_qsel += dz; if (g_qsel < 0) g_qsel = 0; if (g_qsel >= g_nshown) g_qsel = g_nshown - 1; return; }
    if (R_right.w && inside(x, y, R_right.x, R_right.y, R_right.w, R_right.h)) {
        if (g_ext_open >= 0 && g_ext[g_ext_open].webview >= 0) bweb_scroll(g_ext[g_ext_open].webview, dz * 48);
        return;
    }
    if (g_side_on && inside(x, y, R_side.x, R_side.y, R_side.w, R_side.h)) {
        if (g_view == 0) g_tree_top += dz * 3;
        if (g_view == 1) { g_hits_top += dz * 2; if (g_hits_top < 0) g_hits_top = 0; }
        if (g_tree_top < 0) g_tree_top = 0;
        return;
    }
    if (d && inside(x, y, R_edit.x, R_edit.y, R_edit.w, R_edit.h)) {
        d->top += dz * 3;
        if (d->top > d->n - 3) d->top = d->n - 3;
        if (d->top < 0) d->top = 0;
        /* the cursor may leave the view: keep it where it is */
        if (d->cy < d->top) doc_goto(d, d->top, d->cx, 0), d->cy = d->top;
        if (d->cy >= d->top + text_rows()) d->cy = d->top + text_rows() - 1;
        if (d->cx > d->lines[d->cy].len) d->cx = d->lines[d->cy].len;
    }
}

int main(int argc, char** argv) {
    if (bwin_open(&win, "Banana Code", 1100, 720) != 0) { printf("code: the desktop is not running (startx)\n"); return 1; }
    bwin_resizable(&win, 640, 420);
    bwin_wheel(&win);
    g_nodes = calloc(MAX_NODES, sizeof(node_t));
    g_hits = calloc(MAX_HITS, sizeof(hit_t));
    g_items = calloc(MAX_ITEMS, PATH_MAX_);
    g_item_label = calloc(MAX_ITEMS, 128);
    g_item_key = calloc(MAX_ITEMS, 24);
    g_shown = calloc(MAX_ITEMS, sizeof(int));
    if (!g_nodes || !g_hits || !g_items || !g_item_label || !g_item_key || !g_shown) return 1;
    glyphs_init();
    mkdir_p("/home/banana/.bcode/extensions");
    ext_load_all();

    /* a folder or a file given, else the last folder */
    if (argc > 1) {
        if (is_dir(argv[1])) open_folder(argv[1]);
        else {
            char dir[PATH_MAX_];
            snprintf(dir, sizeof(dir), "%s", argv[1]);
            char* s = strrchr(dir, '/');
            if (s && s != dir) { *s = 0; open_folder(dir); }
            open_file(argv[1]);
        }
    } else {
        char* last = read_file("/home/banana/.bcode/last-folder", NULL);
        if (last && is_dir(last)) open_folder(last);
        free(last);
    }
    unsigned last_blink = 0;
    for (;;) {
        if (g_ui_dirty) redraw();
        banana_event_t ev;
        int got = bwin_wait_event(&win, &ev, 60);
        if (ext_poll()) g_ui_dirty = 1;
        if (!got) {
            unsigned b = banana_ticks() / 530;
            if (b != last_blink) { last_blink = b; g_ui_dirty = 1; }
            continue;
        }
        g_ui_dirty = 1;
        switch (ev.type) {
        case BANANA_EV_CLOSE: {
            int dirty = 0;
            for (int i = 0; i < g_ndocs; i++) if (g_docs[i]->dirty && g_docs[i]->path[0]) dirty++;
            if (dirty) { for (int i = 0; i < g_ndocs; i++) if (g_docs[i]->dirty && g_docs[i]->path[0]) doc_save(g_docs[i]); }
            bwin_close(&win);
            return 0;
        }
        case BANANA_EV_KEY: on_key(ev.key); break;
        case BANANA_EV_MOUSE_DOWN: case BANANA_EV_MOUSE_UP: case BANANA_EV_MOUSE_MOVE: on_mouse(&ev); break;
        case BANANA_EV_WHEEL: on_wheel(ev.y); break;
        case BANANA_EV_RESIZE: {
            layout();
            int h = win.h - TOP_H - STATUS_H - 30;
            ext_resize(g_right_w, h > 50 ? h : 50);
            break;
        }
        }
    }
}
