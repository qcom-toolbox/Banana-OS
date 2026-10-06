/* mandel - the Mandelbrot set: double-precision math in a resizable
 * window, rendered by several threads that take rows from a shared
 * counter. Left click zooms in there, right click zooms out, r resets. */
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

#define WORKERS 4

typedef struct {
    bwin_t* w;
    int max;
    double scale;
} frame_t;

static volatile int g_next_row;      /* the next row a thread takes */
static volatile int g_rows_done;

static int render_rows(void* arg) {
    frame_t* f = (frame_t*)arg;
    bwin_t* w = f->w;
    for (;;) {
        int y = __sync_fetch_and_add(&g_next_row, 1);
        if (y >= w->h) break;
        double ci = cy + (y - w->h / 2) * f->scale;
        for (int x = 0; x < w->w; x++) {
            double cr = cx + (x - w->w / 2) * f->scale;
            double zr = 0, zi = 0;
            int it = 0;
            while (it < f->max && zr * zr + zi * zi < 256) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                it++;
            }
            w->px[y * w->w + x] = shade(it, f->max, zr, zi);
        }
        __sync_fetch_and_add(&g_rows_done, 1);
    }
    return 0;
}

static void render(bwin_t* w) {
    frame_t f;
    f.w = w;
    f.max = 64 + (int)(40 * log2(3.2 / span));
    if (f.max > 2000) f.max = 2000;
    f.scale = span / w->w;
    g_next_row = 0;
    g_rows_done = 0;
    unsigned t0 = banana_ticks();
    int ids[WORKERS], n = 0;
    for (int i = 0; i < WORKERS; i++) {
        ids[i] = banana_thread(render_rows, &f);
        if (ids[i] > 0) n++;
    }
    if (!n) {
        render_rows(&f);                        /* no threads (an older Banana OS) */
    } else {
        /* the main thread shows the progress while the workers compute */
        while (g_rows_done < w->h) {
            bwin_update(w);
            banana_sleep(40);
        }
        for (int i = 0; i < WORKERS; i++) if (ids[i] > 0) banana_join(ids[i]);
    }
    char title[112];
    snprintf(title, sizeof(title), "Mandelbrot  zoom %.0fx  %d threads, %u ms", 3.2 / span, n ? n : 1, banana_ticks() - t0);
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
