/* mandel - the Mandelbrot set: double-precision math in a resizable
 * window. Left click zooms in there, right click zooms out, r resets. */
#include <stdio.h>
#include <math.h>
#include <banana.h>

static double cx = -0.6, cy = 0.0, span = 3.2;   /* centre and width of the view */

static unsigned int shade(int it, int max, double zr, double zi) {
    if (it >= max) return 0x000000;
    /* smooth colouring: fractional escape count */
    double mu = it + 1 - log(log(sqrt(zr * zr + zi * zi))) / M_LN2;
    double t = mu / max;
    int r = (int)(9 * (1 - t) * t * t * t * 255);
    int g = (int)(15 * (1 - t) * (1 - t) * t * t * 255);
    int b = (int)(8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return BANANA_RGB(r, g, b);
}

static void render(bwin_t* w) {
    int max = 64 + (int)(40 * log2(3.2 / span));
    if (max > 2000) max = 2000;
    double scale = span / w->w;
    for (int y = 0; y < w->h; y++) {
        double ci = cy + (y - w->h / 2) * scale;
        for (int x = 0; x < w->w; x++) {
            double cr = cx + (x - w->w / 2) * scale;
            double zr = 0, zi = 0;
            int it = 0;
            while (it < max && zr * zr + zi * zi < 256) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                it++;
            }
            w->px[y * w->w + x] = shade(it, max, zr, zi);
        }
        if ((y & 15) == 0) { bwin_update(w); banana_yield(); }   /* show progress, stay responsive */
    }
    char title[96];
    snprintf(title, sizeof(title), "Mandelbrot  x=%.6f y=%.6f  zoom %.0fx", cx, cy, 3.2 / span);
    bwin_title(w, title);
    bwin_update(w);
}

int main(void) {
    bwin_t w;
    if (bwin_open(&w, "Mandelbrot", 420, 300) != 0) {
        printf("mandel: the desktop is not running - start it with `startx`\n");
        return 1;
    }
    bwin_resizable(&w, 120, 80);
    render(&w);
    for (;;) {
        banana_event_t ev;
        if (!bwin_wait_event(&w, &ev, -1)) continue;
        if (ev.type == BANANA_EV_CLOSE || (ev.type == BANANA_EV_KEY && ev.key == 27)) break;
        if (ev.type == BANANA_EV_RESIZE) render(&w);
        if (ev.type == BANANA_EV_MOUSE_DOWN) {
            double scale = span / w.w;
            cx += (ev.x - w.w / 2) * scale;
            cy += (ev.y - w.h / 2) * scale;
            span *= ev.button == 2 ? 2.0 : 0.4;
            render(&w);
        }
        if (ev.type == BANANA_EV_KEY && (ev.key == 'r' || ev.key == 'R')) {
            cx = -0.6; cy = 0; span = 3.2;
            render(&w);
        }
    }
    bwin_close(&w);
    return 0;
}
