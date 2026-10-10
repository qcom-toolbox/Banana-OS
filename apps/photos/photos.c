/* Photos - Banana OS's picture viewer.
 *
 * `photos <picture>` (Files opens pictures with it): the picture fitted to
 * the window, the others of its folder one key away.
 *   Left / Right, the < > buttons    the previous / next picture
 *   wheel, + / -                      zoom (drag to move a zoomed picture)
 *   0 / F                             fit to the window, 1: actual size
 *   R                                 rotate a quarter turn
 *   W, "Set as wallpaper"             the desktop's wallpaper (Settings lists it)
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_H   44
#define C_BG    0x101318u
#define C_BAR   0x1D232Cu
#define C_TEXT  0xE8EEF6u
#define C_DIM   0x9AA6B6u
#define MAXPICS 512

static bwin_t win;
static char   dir[512];
static char   names[MAXPICS][64];
static int    npics, cur = -1;

static unsigned int* img;          /* the picture (rotated as shown) */
static int    iw, ih;
static char   err[96];
static double zoom;                /* screen pixels per picture pixel; 0: fit */
static double cx, cy;              /* the picture point at the view's centre */
static char   status[96];

static int is_picture(const char* n) {
    const char* e = strrchr(n, '.');
    if (!e) return 0;
    char x[8];
    int i = 0;
    for (e++; *e && i < 7; e++) x[i++] = (char)(*e >= 'A' && *e <= 'Z' ? *e + 32 : *e);
    x[i] = 0;
    return !strcmp(x, "png") || !strcmp(x, "jpg") || !strcmp(x, "jpeg") || !strcmp(x, "bmp") || !strcmp(x, "gif");
}

static int cmp_names(const void* a, const void* b) { return strcasecmp((const char*)a, (const char*)b); }

/* the pictures of path's folder; cur: path's place among them */
static void scan(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (slash) {
        int n = (int)(slash - path);
        if (n == 0) n = 1;
        if (n >= (int)sizeof(dir)) n = sizeof(dir) - 1;
        memcpy(dir, path, n);
        dir[n] = 0;
    } else {
        __banana->getcwd(dir, sizeof(dir));
    }
    npics = 0;
    banana_dirent_t e;
    for (int i = 0; npics < MAXPICS && __banana->readdir(dir, i, &e) == 0; i++)
        if (!e.is_dir && is_picture(e.name)) { strncpy(names[npics], e.name, 63); names[npics][63] = 0; npics++; }
    qsort(names, npics, sizeof(names[0]), cmp_names);
    cur = -1;
    for (int i = 0; i < npics; i++) if (!strcmp(names[i], base)) cur = i;
    if (cur < 0 && npics < MAXPICS) { strncpy(names[npics], base, 63); cur = npics++; }
}

static void full_path(int i, char* out, int cap) {
    if (!strcmp(dir, "/")) snprintf(out, cap, "/%s", names[i]);
    else snprintf(out, cap, "%s/%s", dir, names[i]);
}

static int view_h(void) { return win.h - BAR_H > 1 ? win.h - BAR_H : 1; }

static double fit_zoom(void) {
    if (!img) return 1;
    double zx = (double)win.w / iw, zy = (double)view_h() / ih;
    double z = zx < zy ? zx : zy;
    return z > 1 ? 1 : z;              /* small pictures stay at their size */
}
static double cur_zoom(void) { return zoom > 0 ? zoom : fit_zoom(); }

/* ── drawing ── */
static void draw_bar(void) {
    int y = win.h - BAR_H;
    bwin_fill_rect(&win, 0, y, win.w, BAR_H, C_BAR);
    bwin_fill_rect(&win, 0, y, win.w, 1, 0x2C3440u);
    int by = y + 10;
    bwin_button(&win, 8, by, 30, 24, "<", 0);
    bwin_button(&win, 42, by, 30, 24, ">", 0);
    bwin_button(&win, 84, by, 30, 24, "-", 0);
    bwin_button(&win, 118, by, 30, 24, "+", 0);
    bwin_button(&win, 152, by, 44, 24, zoom > 0 ? "Fit" : "1:1", 0);
    bwin_button(&win, 200, by, 60, 24, "Rotate", 0);
    bwin_button(&win, 264, by, 140, 24, "Set as wallpaper", 0);
    char line[160];
    if (status[0]) snprintf(line, sizeof(line), "%s", status);
    else if (img) snprintf(line, sizeof(line), "%d of %d  %dx%d  %d%%", cur + 1, npics, iw, ih, (int)(cur_zoom() * 100 + 0.5));
    else line[0] = 0;
    bwin_font(&win, 416, by + 4, BANANA_FONT_SANS, 13, line, status[0] ? 0xFFD27Au : C_DIM);
}

static void draw(void) {
    int vh = view_h();
    bwin_fill_rect(&win, 0, 0, win.w, vh, C_BG);
    if (!img) {
        const char* m = err[0] ? err : "Open a picture: double-click it in Files.";
        int tw = banana_font_width(BANANA_FONT_SANS, 15, m);
        bwin_font(&win, (win.w - tw) / 2, vh / 2 - 10, BANANA_FONT_SANS, 15, m, C_DIM);
    } else {
        double z = cur_zoom();
        double sw = iw * z, sh = ih * z;
        /* the picture's top-left on screen */
        double ox = sw <= win.w ? (win.w - sw) / 2 : win.w / 2.0 - cx * z;
        double oy = sh <= vh ? (vh - sh) / 2 : vh / 2.0 - cy * z;
        int x0 = ox < 0 ? 0 : (int)ox, y0 = oy < 0 ? 0 : (int)oy;
        int x1 = ox + sw > win.w ? win.w : (int)(ox + sw), y1 = oy + sh > vh ? vh : (int)(oy + sh);
        double inv = 1.0 / z;
        static int sxs[8192];
        int n = x1 - x0;
        if (n > 8192) n = 8192;
        for (int x = 0; x < n; x++) {
            int sx = (int)((x0 + x - ox) * inv);
            sxs[x] = sx < 0 ? 0 : sx >= iw ? iw - 1 : sx;
        }
        for (int y = y0; y < y1; y++) {
            int sy = (int)((y - oy) * inv);
            if (sy < 0) sy = 0;
            if (sy >= ih) sy = ih - 1;
            const unsigned int* src = img + (long)sy * iw;
            unsigned int* dst = win.px + (long)y * win.w + x0;
            if (z < 1) {
                /* smaller: the average of a 2x2 block (less shimmer) */
                int sy2 = sy + 1 < ih ? sy + 1 : sy;
                const unsigned int* src2 = img + (long)sy2 * iw;
                for (int x = 0; x < n; x++) {
                    int a = sxs[x], b = a + 1 < iw ? a + 1 : a;
                    unsigned int p0 = src[a], p1 = src[b], p2 = src2[a], p3 = src2[b];
                    unsigned int rb = ((p0 & 0xFF00FF) + (p1 & 0xFF00FF) + (p2 & 0xFF00FF) + (p3 & 0xFF00FF)) >> 2;
                    unsigned int g = ((p0 & 0xFF00) + (p1 & 0xFF00) + (p2 & 0xFF00) + (p3 & 0xFF00)) >> 2;
                    dst[x] = (rb & 0xFF00FF) | (g & 0xFF00);
                }
            } else {
                for (int x = 0; x < n; x++) dst[x] = src[sxs[x]];
            }
        }
    }
    draw_bar();
    bwin_update(&win);
}

/* ── pictures ── */
static void load(int i) {
    if (i < 0 || i >= npics) return;
    cur = i;
    free(img);
    img = NULL;
    err[0] = 0;
    status[0] = 0;
    char p[600], title[100];
    full_path(i, p, sizeof(p));
    snprintf(title, sizeof(title), "%s - Photos", names[i]);
    bwin_title(&win, title);
    /* a big photo takes a moment */
    snprintf(status, sizeof(status), "Opening %s...", names[i]);
    draw_bar();
    bwin_update(&win);
    status[0] = 0;
    char e[96];
    img = banana_image_load(p, &iw, &ih, e, sizeof(e));
    if (!img) snprintf(err, sizeof(err), "%s: %s", names[i], e);
    zoom = 0;
    cx = iw / 2.0;
    cy = ih / 2.0;
}

static void rotate(void) {
    if (!img) return;
    unsigned int* r = malloc((size_t)iw * ih * 4);
    if (!r) return;
    /* a quarter turn clockwise: (x, y) -> (ih - 1 - y, x) */
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) r[(long)x * ih + (ih - 1 - y)] = img[(long)y * iw + x];
    free(img);
    img = r;
    int t = iw; iw = ih; ih = t;
    double c = cx; cx = iw - cy; cy = c;
}

static void set_zoom(double z) {
    if (!img) return;
    if (z < 0.02) z = 0.02;
    if (z > 16) z = 16;
    zoom = z;
}

static void wallpaper(void) {
    if (cur < 0 || !img) return;
    char p[600], e[96];
    full_path(cur, p, sizeof(p));
    snprintf(status, sizeof(status), "Setting the wallpaper...");
    draw_bar();
    bwin_update(&win);
    if (banana_set_wallpaper(p, 0, e, sizeof(e)) == 0) snprintf(status, sizeof(status), "Wallpaper set - Settings > Wallpaper lists it");
    else snprintf(status, sizeof(status), "Not set: %s", e);
}

static void clamp_centre(void) {
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx > iw) cx = iw;
    if (cy > ih) cy = ih;
}

int main(int argc, char** argv) {
    if (bwin_open(&win, "Photos", 820, 600) != 0) {
        printf("photos: the desktop is not running\n");
        return 1;
    }
    bwin_resizable(&win, 480, 320);
    if (argc > 1) { scan(argv[1]); load(cur); }
    draw();
    int dragging = 0, lx = 0, ly = 0;
    banana_event_t ev;
    for (;;) {
        if (!bwin_wait_event(&win, &ev, -1)) continue;
        int redraw = 1;
        if (ev.type == BANANA_EV_CLOSE) break;
        if (ev.type == BANANA_EV_KEY) {
            int k = ev.key;
            status[0] = 0;
            if (k == BANANA_KEY_LEFT || k == BANANA_KEY_PGUP) load(cur > 0 ? cur - 1 : npics - 1);
            else if (k == BANANA_KEY_RIGHT || k == BANANA_KEY_PGDN || k == ' ') load(cur + 1 < npics ? cur + 1 : 0);
            else if (k == BANANA_KEY_HOME) load(0);
            else if (k == BANANA_KEY_END) load(npics - 1);
            else if (k == BANANA_KEY_UP || k == '+' || k == '=') set_zoom(cur_zoom() * 1.1);
            else if (k == BANANA_KEY_DOWN || k == '-') set_zoom(cur_zoom() / 1.1);
            else if (k == '0' || k == 'f' || k == 'F') zoom = 0;
            else if (k == '1') set_zoom(1);
            else if (k == 'r' || k == 'R') rotate();
            else if (k == 'w' || k == 'W') wallpaper();
            else if (k == 27) break;
            else redraw = 0;
        } else if (ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1) {
            if (ev.y >= win.h - BAR_H) {
                int bx = ev.x, by = ev.y - (win.h - BAR_H);
                status[0] = 0;
                if (by >= 10 && by < 34) {
                    if (bx >= 8 && bx < 38) load(cur > 0 ? cur - 1 : npics - 1);
                    else if (bx >= 42 && bx < 72) load(cur + 1 < npics ? cur + 1 : 0);
                    else if (bx >= 84 && bx < 114) set_zoom(cur_zoom() / 1.25);
                    else if (bx >= 118 && bx < 148) set_zoom(cur_zoom() * 1.25);
                    else if (bx >= 152 && bx < 196) { if (zoom > 0) zoom = 0; else set_zoom(1); }
                    else if (bx >= 200 && bx < 260) rotate();
                    else if (bx >= 264 && bx < 404) wallpaper();
                }
            } else {
                dragging = 1;
                lx = ev.x;
                ly = ev.y;
                redraw = 0;
            }
        } else if (ev.type == BANANA_EV_MOUSE_UP) {
            dragging = 0;
            redraw = 0;
        } else if (ev.type == BANANA_EV_MOUSE_MOVE && dragging && (ev.buttons & 1) && img) {
            double z = cur_zoom();
            cx -= (ev.x - lx) / z;
            cy -= (ev.y - ly) / z;
            clamp_centre();
            lx = ev.x;
            ly = ev.y;
        } else if (ev.type == BANANA_EV_RESIZE) {
            /* bwin_event took the new size */
        } else {
            redraw = 0;
        }
        if (redraw) draw();
    }
    free(img);
    bwin_close(&win);
    return 0;
}
