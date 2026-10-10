#ifndef GPU_H
#define GPU_H

#include "types.h"
#include "display.h"
#include "pci.h"

/*
 * Graphics drivers (kernel/gpu_*.c, and driver modules: kernel/driver.h).
 *
 * One display is active: the one the screen (fb.c) shows. Its driver can
 * change the resolution, give the framebuffer itself (when the firmware
 * set none, or to take the screen over), push what was drawn to the
 * screen (virtual GPUs that do not scan guest memory out), show the mouse
 * pointer in hardware, wait for the vertical blank and set a laptop
 * panel's backlight. Every hook but set_mode may be NULL.
 *
 * Built in:
 *   bga     Bochs / QEMU standard VGA, bochs-display, VirtualBox VBoxVGA
 *   vmsvga  VMware SVGA II: VMware, VirtualBox VMSVGA / VBoxSVGA, QEMU -vga vmware
 *   virtio  virtio-gpu: QEMU / KVM -vga virtio, -device virtio-gpu-pci, crosvm
 *   intel   Intel GMA 950 (945G/GM/GME), HD Graphics 2000/3000 (Sandy Bridge),
 *           HD Graphics 2500/4000 (Ivy Bridge)
 */

typedef struct gpu gpu_t;
struct gpu {
    char        name[64];          /* "VMware SVGA II" */
    const char* driver;            /* "vmsvga" */
    pci_dev_t   pci;
    uint32_t    vram;              /* bytes of video memory (0: unknown) */
    int  (*modes)(gpu_t* g, display_mode_t* out, int max);
    int  (*set_mode)(gpu_t* g, int w, int h);     /* 0 (fb.c then shows the new mode), or -1 */
    /* the screen shows memory by itself without it (NULL) */
    void (*flush)(gpu_t* g, int x, int y, int w, int h);
    /* a w x h (<= 64 x 64) pointer image, 0xAARRGGBB: 0 if the hardware shows it */
    int  (*cursor_image)(gpu_t* g, const uint32_t* argb, int w, int h, int hot_x, int hot_y);
    void (*cursor_move)(gpu_t* g, int x, int y, int visible);
    int  cursor_irq_safe;           /* cursor_move may be called from an interrupt (register writes only) */
    void (*wait_vblank)(gpu_t* g);
    int  (*backlight)(gpu_t* g, int percent);     /* percent < 0 reads it; -1: no backlight */
    void (*info)(gpu_t* g, char* buf, int cap);   /* more lines for lsgpu */
    void* priv;
};

/* the built-in drivers (before the terminal: one may set the framebuffer up) */
void   gpu_init(void);
/* after task_init: pushes the screen of virtual GPUs that need it */
void   gpu_start(void);

gpu_t* gpu_active(void);
/* a driver took a display device: it becomes the active one (a later
 * one - a driver module - takes over from a built-in driver) */
int    gpu_register(gpu_t* g);
/* fb.c: an area was drawn */
void   gpu_flush(int x, int y, int w, int h);
/* the screen shows memory a driver gives (it is 32-bit XRGB) */
void   gpu_set_framebuffer(uintptr_t addr, int w, int h, int pitch);

/* the hardware pointer: 0 if the active display has one (then it moves it) */
int    gpu_cursor_image(const uint32_t* argb, int w, int h, int hot_x, int hot_y);
void   gpu_cursor_move(int x, int y, int visible);
int    gpu_has_hw_cursor(void);
void   gpu_wait_vblank(void);
int    gpu_backlight(int percent);     /* percent < 0 reads; -1 without one */

void   gpu_list(void);                  /* lsgpu */
/* after gpu_init: a card with no driver here (AMD, NVIDIA, Intel Arc / Xe)
 * keeps the firmware's screen - named, its framebuffer write-combining */
void   gpu_firmware_init(void);
const char* gpu_family_name(uint16_t vendor, uint16_t device);   /* "NVIDIA GeForce RTX 30 series (Ampere)", NULL */

/* the built-in drivers: 0 if it took the device */
int gpu_bga_probe(const pci_dev_t* d);
int gpu_vmsvga_probe(const pci_dev_t* d);
int gpu_virtio_probe(const pci_dev_t* d);
int gpu_intel_probe(const pci_dev_t* d);

#endif
