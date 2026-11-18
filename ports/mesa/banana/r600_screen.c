/* SPDX-License-Identifier: MIT
 * Banana OS: Mesa's r600 (AMD Radeon HD 2000 - 6000: the HD 5450 / 6450 /
 * 7450 the kernel drives, TeraScale 2) behind OSMesa, through Mesa's radeon
 * winsys. The GPU is the system's radeon interface on /dev/dri/card0
 * (kernel/radeon_drm.c), there while a Radeon drives the screen; the frames
 * are read back for the window as with the other OSMesa back ends.
 */
struct pipe_screen;
struct pipe_screen* banana_r600_create_screen(int asked);
#if defined(GALLIUM_R600)        /* (the Linux reference build too) */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "drm-uapi/drm.h"
#include "pipe/p_screen.h"
#include "frontend/drm_driver.h"
#include "winsys/radeon_winsys.h"
#include "r600/r600_public.h"

struct pipe_screen* banana_r600_create_screen(int asked) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (asked) fprintf(stderr, "Banana r600: no GPU for apps (%s)\n", strerror(errno));
        return NULL;
    }
    /* the Radeon's interface, not the Intel one: its name */
    char name[16] = "";
    struct drm_version v;
    memset(&v, 0, sizeof(v));
    v.name = name; v.name_len = sizeof(name) - 1;
    int radeon = !ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "radeon");
    if (!radeon) {
        if (asked) fprintf(stderr, "Banana r600: the GPU for apps is not a Radeon\n");
        close(fd);
        return NULL;
    }
    struct pipe_screen_config config = { 0 };                  /* (r600 reads no options) */
    struct radeon_winsys* rw = radeon_drm_winsys_create(fd, &config, r600_screen_create);   /* (it keeps a dup of fd) */
    close(fd);
    if (!rw) {
        fprintf(stderr, "Banana r600: the driver did not take this GPU\n");
        return NULL;
    }
    return rw->screen;
}
#else
struct pipe_screen* banana_r600_create_screen(int asked) { (void)asked; return 0; }
#endif
