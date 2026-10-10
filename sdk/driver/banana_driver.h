#ifndef BANANA_DRIVER_H
#define BANANA_DRIVER_H

/*
 * Banana OS Driver Kit - drivers for hardware Banana OS does not know,
 * written and shipped by whoever makes it.
 *
 * A driver is a small program of the kernel's own (it runs in ring 0,
 * with the kernel, not as an app): built with sdk/driver/driver.mk into a
 * .bpk package of type "driver", installed with `pkg install`, and loaded
 * at every boot (and right after it is installed). Its entry point,
 *
 *     int banana_driver_main(const banana_driver_api_t* api);
 *
 * gets the kernel's table below, registers what it handles (PCI devices,
 * a display...) and returns 0; it stays in memory until the computer is
 * turned off. `drivers` lists the loaded ones and what they drive.
 *
 * Rules of the place:
 *  - no floating point (the kernel does not save FPU state for it);
 *  - no blocking in an interrupt handler: it runs with interrupts off;
 *  - memory: api->alloc (any), api->alloc_dma (physically contiguous, for
 *    devices - its pointer is also its physical address);
 *  - device memory: the address from api->pci_bar() is usable as is
 *    (identity-mapped, uncached);
 *  - a display driver gives the screen a 32-bit XRGB framebuffer
 *    (0x00RRGGBB per pixel, little endian): bdrv_display_t below.
 *
 * Built-in drivers come after the driver modules for displays: a module
 * that takes a graphics card replaces the built-in driver of that card.
 * For other kinds of devices, a module gets the devices it asks for; it
 * should be for hardware Banana OS has no driver of its own for.
 */

#ifdef BANANA_KERNEL                     /* (the kernel's own build, kernel/driver.c) */
#include "../../kernel/types.h"
#else
#include <stdint.h>
#endif

#define BANANA_DRV_MAGIC   0x56524442u     /* "BDRV" */
#define BANANA_DRV_VERSION 1u

/* a PCI function */
typedef struct {
    uint8_t  bus, dev, fn;
    uint8_t  irq_line;          /* legacy interrupt line (0xFF: none) */
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if, revision;
} bdrv_pci_t;

/* one entry of a driver's device list: 0xFFFF matches any vendor / device,
 * class 0xFF any class (so { 0xFFFF, 0xFFFF, 0x02, 0xFF } is "every
 * network controller") */
typedef struct {
    uint16_t vendor, device;
    uint8_t  class_code, subclass;
} bdrv_pci_id_t;

typedef struct {
    const char*          name;          /* "acme-gpu" */
    const char*          description;   /* "ACME Graphics 9000" */
    const bdrv_pci_id_t* ids;
    int                  nids;
    /* a device of the list: 0 if the driver took it (it then drives it) */
    int (*probe)(const bdrv_pci_t* dev);
} bdrv_pci_driver_t;

/* a display (graphics card): every hook but set_mode may be NULL */
typedef struct { int w, h; } bdrv_mode_t;
typedef struct bdrv_display bdrv_display_t;
struct bdrv_display {
    const char* name;                   /* shown by lsgpu */
    uint32_t    vram;                   /* bytes of video memory (0: unknown) */
    int  (*modes)(bdrv_display_t* d, bdrv_mode_t* out, int max);
    /* shows w x h (32-bit): the framebuffer's address and bytes per row; 0 or -1 */
    int  (*set_mode)(bdrv_display_t* d, int w, int h, uint64_t* fb_addr, int* pitch);
    /* the area was drawn: shows it (for cards that do not scan memory out) */
    void (*flush)(bdrv_display_t* d, int x, int y, int w, int h);
    /* a pointer image (<= 64x64, 0xAARRGGBB) shown by the card: 0, or -1 */
    int  (*cursor_image)(bdrv_display_t* d, const uint32_t* argb, int w, int h, int hot_x, int hot_y);
    void (*cursor_move)(bdrv_display_t* d, int x, int y, int visible);
    void (*wait_vblank)(bdrv_display_t* d);
    int  (*backlight)(bdrv_display_t* d, int percent);    /* percent < 0 reads; -1: none */
    void* priv;                         /* the driver's */
};

typedef struct banana_driver_api {
    uint32_t    magic;                  /* BANANA_DRV_MAGIC */
    uint32_t    version;                /* BANANA_DRV_VERSION */
    uint32_t    size;                   /* sizeof(banana_driver_api_t) on the running system */
    const char* arch;                   /* "i686" or "x86_64" */
    const char* os_version;

    /* the kernel log (serial port, `dmesg`-like): printf formats %d %u %x %s %c %p, %ll... */
    void (*log)(const char* fmt, ...);
    void (*vlog)(const char* fmt, __builtin_va_list ap);

    /* ── PCI ─────────────────────────────────────────────── */
    int      (*pci_get)(int index, bdrv_pci_t* out);       /* the index-th function: 0, or -1 past the last */
    uint32_t (*pci_read)(const bdrv_pci_t* d, int offset, int width);           /* width 1, 2 or 4 */
    void     (*pci_write)(const bdrv_pci_t* d, int offset, int width, uint32_t v);
    /* a BAR: memory (mapped, usable as a pointer) or I/O port base (*is_io = 1); 0 if none */
    uint64_t (*pci_bar)(const bdrv_pci_t* d, int bar, int* is_io);
    void     (*pci_enable)(const bdrv_pci_t* d);           /* memory + I/O decoding, bus mastering */

    /* ── I/O ports ───────────────────────────────────────── */
    uint32_t (*port_in)(uint16_t port, int width);          /* width 1, 2 or 4 */
    void     (*port_out)(uint16_t port, int width, uint32_t v);

    /* ── memory ──────────────────────────────────────────── */
    void* (*mmio_map)(uint64_t phys, uint64_t size);        /* device memory: a pointer, or NULL */
    void* (*alloc)(unsigned long size);                     /* zeroed */
    void  (*free)(void* p);
    /* physically contiguous, aligned (power of two); *phys gets its address */
    void* (*alloc_dma)(unsigned long size, unsigned long align, uint64_t* phys);

    /* ── time ────────────────────────────────────────────── */
    uint32_t (*ticks_ms)(void);
    void     (*delay_us)(uint32_t us);                      /* busy wait (also with interrupts off) */
    void     (*sleep_ms)(uint32_t ms);                      /* lets the system run (not in an interrupt) */

    /* ── interrupts (legacy lines, shared) ───────────────── */
    int (*irq_install)(int line, void (*handler)(void* ctx), void* ctx);   /* 0, or -1 */

    /* ── a task of the driver's own (a loop: polling, a worker) ── */
    int (*task_create)(const char* name, void (*fn)(void* arg), void* arg); /* 0, or -1 */

    /* ── what the driver handles ─────────────────────────── */
    int (*register_pci_driver)(const bdrv_pci_driver_t* d);   /* probes the devices now; how many it took */
    /* the screen: it becomes the active display (dev: the card) */
    int (*register_display)(bdrv_display_t* d, const bdrv_pci_t* dev);
} banana_driver_api_t;

/* the driver's own entry point */
int banana_driver_main(const banana_driver_api_t* api);

/* the table, for the driver's code (driver_crt.c keeps it) */
extern const banana_driver_api_t* bdrv;

/* small helpers that need no C library (driver_lib.c) */
void*  bdrv_memset(void* d, int c, unsigned long n);
void*  bdrv_memcpy(void* d, const void* s, unsigned long n);
int    bdrv_memcmp(const void* a, const void* b, unsigned long n);
unsigned long bdrv_strlen(const char* s);

static inline volatile uint32_t* bdrv_reg32(uint64_t base, uint32_t off) { return (volatile uint32_t*)(uintptr_t)(base + off); }

#endif
