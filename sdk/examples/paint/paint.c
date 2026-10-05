/* paint - a desktop app: a window, mouse and key events, drawing, and
 * saving a .bmp file (which Files and the Wallpaper app can open) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <banana.h>

#define W        480
#define H        340
#define BAR_H    28
#define CANVAS_Y BAR_H

static const unsigned int PALETTE[] = {
    0x000000, 0xFFFFFF, 0xE04040, 0xF4A030, 0xF4D35E, 0x40C060, 0x3A7BD5, 0x8040C0, 0x805030, 0x808890,
};
#define NCOLORS (int)(sizeof(PALETTE) / sizeof(PALETTE[0]))

static bwin_t win;
static unsigned int color = 0x000000;
static int brush = 3;
static char status[64] = "Draw with the left button - right button erases";

static void draw_bar(void) {
    bwin_fill_rect(&win, 0, 0, W, BAR_H, 0x1D232C);
    for (int i = 0; i < NCOLORS; i++) {
        int x = 6 + i * 22;
        bwin_fill_rect(&win, x, 5, 18, 18, PALETTE[i]);
        bwin_rect(&win, x - 1, 4, 20, 20, PALETTE[i] == color ? 0xF4D35E : 0x505D72);
    }
    char b[16];
    snprintf(b, sizeof(b), "size %d", brush);
    bwin_button(&win, 232, 4, 22, 20, "-", 0);
    bwin_text(&win, 260, 10, b, 0xE8EEF6, BANANA_TRANSPARENT);
    bwin_button(&win, 322, 4, 22, 20, "+", 0);
    bwin_button(&win, 352, 4, 56, 20, "Clear", 0);
    bwin_button(&win, 414, 4, 56, 20, "Save", 0);
}

static void draw_status(void) {
    bwin_fill_rect(&win, 0, H - 12, W, 12, 0x1D232C);
    bwin_text(&win, 4, H - 10, status, 0xAAB6C6, BANANA_TRANSPARENT);
}

static void clear_canvas(void) {
    bwin_fill_rect(&win, 0, CANVAS_Y, W, H - CANVAS_Y - 12, 0xFFFFFF);
}

static void dab(int x, int y, unsigned int c) {
    if (y - brush < CANVAS_Y) return;
    if (y + brush >= H - 12) return;
    bwin_fill_circle(&win, x, y, brush, c);
}

/* a line of dabs, so fast strokes have no gaps */
static void stroke(int x0, int y0, int x1, int y1, unsigned int c) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0) { dab(x0, y0, c); return; }
    for (int i = 0; i <= steps; i++) dab(x0 + dx * i / steps, y0 + dy * i / steps, c);
}

/* the canvas as a 24-bit .bmp */
static int save_bmp(const char* path) {
    int cw = W, ch = H - CANVAS_Y - 12;
    int row = (cw * 3 + 3) & ~3;
    int size = 54 + row * ch;
    unsigned char* b = calloc(1, (size_t)size);
    if (!b) return -1;
    b[0] = 'B'; b[1] = 'M';
    memcpy(b + 2, &size, 4);
    int off = 54, hdr = 40, planes_bpp = 1 | (24 << 16);
    memcpy(b + 10, &off, 4);
    memcpy(b + 14, &hdr, 4);
    memcpy(b + 18, &cw, 4);
    memcpy(b + 22, &ch, 4);
    memcpy(b + 26, &planes_bpp, 4);
    for (int y = 0; y < ch; y++) {
        unsigned char* r = b + 54 + (ch - 1 - y) * row;     /* bottom-up */
        for (int x = 0; x < cw; x++) {
            unsigned int p = bwin_get_pixel(&win, x, CANVAS_Y + y);
            r[x * 3] = (unsigned char)p;
            r[x * 3 + 1] = (unsigned char)(p >> 8);
            r[x * 3 + 2] = (unsigned char)(p >> 16);
        }
    }
    FILE* f = fopen(path, "w");
    if (!f) { free(b); return -1; }
    size_t ok = fwrite(b, 1, (size_t)size, f);
    fclose(f);
    free(b);
    return ok == (size_t)size ? 0 : -1;
}

static void save(void) {
    char path[96];
    banana_time_t t;
    banana_time(&t);
    snprintf(path, sizeof(path), "/home/banana/Pictures/paint-%02d%02d%02d.bmp", t.hour, t.minute, t.second);
    if (save_bmp(path) == 0) snprintf(status, sizeof(status), "Saved %s", path + 13);
    else snprintf(status, sizeof(status), "Could not save %s", path);
}

int main(void) {
    if (bwin_open(&win, "Paint", W, H) != 0) {
        printf("paint: the desktop is not running - start it with `startx`\n");
        return 1;
    }
    clear_canvas();
    draw_bar();
    draw_status();
    bwin_update(&win);

    int drawing = 0, lx = 0, ly = 0;
    unsigned int ink = color;
    for (;;) {
        banana_event_t ev;
        if (!bwin_wait_event(&win, &ev, 1000)) continue;
        int dirty = 0;
        if (ev.type == BANANA_EV_CLOSE) break;
        if (ev.type == BANANA_EV_MOUSE_DOWN) {
            if (ev.y < BAR_H) {
                for (int i = 0; i < NCOLORS; i++)
                    if (ev.x >= 6 + i * 22 && ev.x < 24 + i * 22) color = PALETTE[i];
                if (ev.x >= 232 && ev.x < 254 && brush > 1) brush--;
                if (ev.x >= 322 && ev.x < 344 && brush < 20) brush++;
                if (ev.x >= 352 && ev.x < 408) { clear_canvas(); strcpy(status, "Cleared"); }
                if (ev.x >= 414 && ev.x < 470) save();
                draw_bar();
                draw_status();
                dirty = 1;
            } else {
                ink = ev.button == 2 ? 0xFFFFFF : color;
                drawing = ev.button == 1;
                lx = ev.x; ly = ev.y;
                dab(ev.x, ev.y, ink);
                dirty = 1;
            }
        } else if (ev.type == BANANA_EV_MOUSE_MOVE && drawing && (ev.buttons & 1)) {
            stroke(lx, ly, ev.x, ev.y, ink);
            lx = ev.x; ly = ev.y;
            dirty = 1;
        } else if (ev.type == BANANA_EV_MOUSE_UP) {
            drawing = 0;
        } else if (ev.type == BANANA_EV_KEY) {
            if (ev.key == 's' || ev.key == 19) { save(); draw_status(); dirty = 1; }      /* s / Ctrl+S */
            if (ev.key == '+' && brush < 20) { brush++; draw_bar(); dirty = 1; }
            if (ev.key == '-' && brush > 1) { brush--; draw_bar(); dirty = 1; }
            if (ev.key == 'c') { clear_canvas(); dirty = 1; }
            if (ev.key == 27) break;
        }
        if (dirty) bwin_update(&win);
    }
    bwin_close(&win);
    return 0;
}
