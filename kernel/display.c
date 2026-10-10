/* Screen resolution: through the active graphics driver (kernel/gpu.h);
 * without one the mode the firmware set stays. */
#include "display.h"
#include "gpu.h"
#include "fb.h"
#include "gui.h"
#include "terminal.h"

int display_can_change(void) {
    gpu_t* g = gpu_active();
    if (!g || !fb_available() || !g->modes) return 0;
    display_mode_t m[2];
    return g->modes(g, m, 2) > 1;
}

int display_modes(display_mode_t* out, int max) {
    gpu_t* g = gpu_active();
    if (g && g->modes && fb_available()) {
        int n = g->modes(g, out, max);
        if (n > 0) return n;
    }
    const fb_info_t* fi = fb_info();
    if (fi && fi->width && max > 0) { out[0].w = (int)fi->width; out[0].h = (int)fi->height; return 1; }
    return 0;
}

int display_set_mode(int w, int h) {
    gpu_t* g = gpu_active();
    if (!g || !fb_available()) return -1;
    const fb_info_t* fi = fb_info();
    if ((int)fi->width == w && (int)fi->height == h) return 0;
    if (g->set_mode(g, w, h) < 0) return -1;
    terminal_screen_changed();
    gui_screen_changed();
    gpu_flush(0, 0, w, h);
    return 0;
}
