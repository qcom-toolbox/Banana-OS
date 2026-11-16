/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GPU_H
#define GPU_H

#include "types.h"
#include "display.h"
#include "pci.h"
#include "gpu_surface.h"

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
void   gpu_dirty_raw(void); /* a caller will write through a raw framebuffer pointer */
/* Built-in drivers may provide a bounded, allocation-free panic update.
 * Kept outside gpu_t so the driver-module structure ABI stays unchanged. */
void   gpu_register_panic(gpu_t* g, void (*present)(void));
void   gpu_panic_present(void);
/* the screen shows memory a driver gives (it is 32-bit XRGB) */
void   gpu_set_framebuffer(uintptr_t addr, int w, int h, int pitch);

/* the hardware pointer: 0 if the active display has one (then it moves it) */
int    gpu_cursor_image(const uint32_t* argb, int w, int h, int hot_x, int hot_y);
void   gpu_cursor_move(int x, int y, int visible);
int    gpu_has_hw_cursor(void);
void   gpu_wait_vblank(void);
int    gpu_backlight_step(int delta); /* next firmware level, or a native 0..100 step */
int    gpu_backlight(int percent);     /* percent < 0 reads; -1 without one */

/* Telemetry is separate from gpu_t: loadable driver ABI stays stable. */
typedef struct {
    uint64_t frames, copied_bytes, copy_ns, flushes, flushed_bytes, flush_ns;
    uint64_t commands, command_ns, timeouts;
    uint64_t damage_frames, candidate_bytes, damage_bytes;
    uint64_t flips, vblank_waits, vblank_timeouts;
} gpu_stats_t;
void gpu_stats(gpu_stats_t* out);
void gpu_note_present(uint64_t bytes, uint64_t ns);
void gpu_note_damage(uint64_t candidate, uint64_t copied);
void gpu_note_command(uint64_t ns, int timeout);
void gpu_diagnostics(void);
void gpu_bench(void);
void gpu_tests(void (*report)(const char*, int, const char*));
/* Optional built-in extensions, kept outside the driver-module ABI. */
typedef struct {
    /* 0: unavailable, 1: existing pages, 2: initialize both pages. */
    int (*begin)(gpu_surface_t* target);
    void (*end)(gpu_surface_t source, const gpu_rect_t* changed, int count);
} gpu_present_ops_t;
void gpu_register_present(gpu_t* g, const gpu_present_ops_t* ops);
int gpu_present_begin(gpu_surface_t* target);
void gpu_present_end(gpu_surface_t source, const gpu_rect_t* changed, int count);
void gpu_note_flip(void);
void gpu_note_vblank(int timeout);
void gpu_register_service(gpu_t* g, void (*service)(void),
                          void (*tests)(void (*report)(const char*, int, const char*)));
void gpu_driver_tests(void (*report)(const char*, int, const char*));

void   gpu_list(void);                  /* lsgpu */
/* after gpu_init: a card with no driver here (AMD, NVIDIA, Intel Arc / Xe)
 * keeps the firmware's screen - named, its framebuffer write-combining */
void   gpu_firmware_init(void);
const char* gpu_family_name(uint16_t vendor, uint16_t device);   /* "NVIDIA GeForce RTX 30 series (Ampere)", NULL */

/* the built-in drivers: 0 if it took the device */
int gpu_bga_probe(const pci_dev_t* d);
int gpu_bga_s3_resume(void);
int gpu_vmsvga_probe(const pci_dev_t* d);
int gpu_virtio_probe(const pci_dev_t* d);
int gpu_intel_probe(const pci_dev_t* d);
/* AMD Radeon HD 5000-7000 (TeraScale 2) and RX 400/500 (Polaris): hardware
 * cursor and vsync, EXPERIMENTAL - only with `gpu experimental on`
 * (kernel/gpu_radeon.c) */
int  gpu_radeon_try(void);               /* now: 0, or -1 (gpu_radeon_status says why) */
/* the video BIOS and display registers saved in dir (a report): 0, or -1; msg says what */
int  gpu_radeon_dump(const char* dir, char* msg, int cap);
const char* gpu_radeon_status(void);
/* `gpu radeon 3d`: the 3D engine brought up and tested (Caicos): 0, or -1 (msg) */
int  gpu_radeon_3d(char* msg, int cap);
void gpu_radeon_boot(void);              /* settings_boot: when asked for */
int  gpu_experimental(void);             /* the setting */
void gpu_set_experimental(int on);

#endif
