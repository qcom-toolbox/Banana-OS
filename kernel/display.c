/* Screen resolution: through the active graphics driver (kernel/gpu.h);
 * without one the mode the firmware set stays. */
#include "display.h"
#include "gpu.h"
#include "fb.h"
#include "gui.h"
#include "terminal.h"
#include "fsdisk.h"
#include "kstring.h"

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

int display_boot_modes(display_mode_t* out, int max) {
    int w[48], h[48];
    int n = fb_boot_modes(w, h, max < 48 ? max : 48);
    for (int i = 0; i < n; i++) { out[i].w = w[i]; out[i].h = h[i]; }
    return n;
}

int display_set_boot_mode(int w, int h) {
    if (!fsdisk_is_installed()) return -2;
    if (w > 0) {                                   /* one the firmware offers */
        display_mode_t m[48];
        int n = display_boot_modes(m, 48), ok = 0;
        for (int i = 0; i < n; i++) ok |= m[i].w == w && m[i].h == h;
        if (!ok) return -3;
    }
    char v[16];
    if (w > 0) ksnprintf(v, sizeof(v), "%dx%d", w, h);
    else kstrlcpy(v, "auto", sizeof(v));
    return fsdisk_set_boot_video(v) > 0 ? 0 : -1;
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
