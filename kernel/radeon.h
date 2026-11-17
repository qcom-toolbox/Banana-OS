/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RADEON_H
#define RADEON_H
/* AMD Radeon HD 5000 - 7000 TeraScale 2 (Evergreen, Northern Islands:
 * Cedar ... Cayman - the HD 7450 is a Caicos), the parts of the driver
 * that share the card:
 *   radeon_atom.c   the video BIOS: found, its AtomBIOS tables run by the
 *                   interpreter (third_party/radeon/atom.c), the display
 *                   paths and clocks it describes, EDID through its I2C
 *   radeon_kms.c    mode setting: PLLs, CRTC timings, encoders
 *   radeon_gfx.c    the 3D engine: GART, command processor, ring, fences
 *                   (Caicos; the microcode in firmware/amd)
 *   gpu_radeon.c    the display driver (gpu_t) on top of them
 * After Linux's radeon driver (MIT licensed), written without the card at
 * hand: experimental, off unless asked for (gpu experimental on). */
#include "types.h"
#include "pci.h"
#include "../third_party/radeon/radeon_family.h"
#include "edid.h"

struct atom_context;

/* a display output the video BIOS lists (its display path) */
typedef struct {
    uint16_t device;          /* ATOM_DEVICE_*_SUPPORT: what it drives (CRT1, DFP1...) */
    uint16_t conn_obj;        /* the connector's object id (type | enum) */
    uint8_t  conn_type;       /* CONNECTOR_OBJECT_ID_* (DVI_I, HDMI_TYPE_A, VGA, DISPLAYPORT...) */
    uint8_t  i2c_id;          /* the DDC line (ProcessI2cChannelTransaction's line number); 0: none */
    uint8_t  nenc;
    uint16_t enc_obj[2];      /* its encoders' object ids (DVI-I: the DAC and the UNIPHY) */
} rad_path_t;

/* the pixel clock PLLs' limits (FirmwareInfo), in 10 kHz units */
typedef struct {
    uint32_t ref_freq, pll_out_min, pll_out_max, pll_in_min, pll_in_max;
    uint32_t max_pixel_clock, default_dispclk, dp_extclk;
} rad_clocks_t;

#define RAD_PATHS 8

typedef struct radeon {
    pci_dev_t pci;
    int       family;          /* CHIP_* (third_party/radeon/radeon_family.h) */
    const char* name;
    volatile uint8_t* mmio;    /* the registers (BAR 2) */
    uint32_t  mmio_size;
    uint16_t  io_base;         /* the I/O BAR: the BIOS's indirect register access (0: none) */
    uint8_t*  bios;            /* the video BIOS image (kmalloc'd) */
    uint32_t  bios_size;
    const char* bios_from;     /* "ACPI VFCT", "PCI ROM", "legacy shadow" */
    struct atom_context* atom;
    rad_path_t paths[RAD_PATHS];
    int       npaths;
    rad_clocks_t clk;
} radeon_t;

uint32_t rad_rd(radeon_t* r, uint32_t reg);
void     rad_wr(radeon_t* r, uint32_t reg, uint32_t v);

/* the family of a TeraScale 2 device id (CHIP_*), -1 if not one */
int  rad_family(uint16_t device);
const char* rad_family_name(int family);

/* radeon_atom.c: the video BIOS found and parsed (0), or -1 (why) */
int  rad_atom_init(radeon_t* r, char* why, int cap);
/* a command table run: 0, or -1 (not in this BIOS, or it failed) */
int  rad_atom_exec(radeon_t* r, int index, void* params, int size);
/* a command table's version: 0 (frev / crev set), or -1 if the BIOS has none */
int  rad_atom_cmd_rev(radeon_t* r, int index, uint8_t* frev, uint8_t* crev);
/* a 128-byte EDID block from the DDC line i2c_id: 0, or -1 */
int  rad_atom_read_edid(radeon_t* r, uint8_t i2c_id, uint8_t* edid);
/* the video BIOS's own description, for lsgpu */
void rad_atom_info(radeon_t* r, char* buf, int cap);

/* radeon_kms.c: what an output is driven as (digital: the monitor's EDID
 * says it is a digital one - DVI-I carries both) */
enum { RAD_OUT_NONE = 0, RAD_OUT_TMDS, RAD_OUT_VGA, RAD_OUT_DP };
int      rad_kms_path_kind(const rad_path_t* p, int digital);
uint32_t rad_kms_max_clock(radeon_t* r, const rad_path_t* p, int kind);   /* kHz */
/* mode m on CRTC crtc through output path, scanning out the w x h XRGB
 * surface at fb_mc (the memory controller's address), pitch_px pixels a
 * row: 0, or -1 (why) */
int      rad_kms_set(radeon_t* r, int crtc, int path, int digital, const edid_mode_t* m,
                     uint64_t fb_mc, int w, int h, int pitch_px, char* why, int cap);

/* radeon_gfx.c: the 3D engine brought up and tested (0), or -1 (why).
 * vram_cpu: the video memory BAR (the CPU's address); vram_mc: the memory
 * controller's address of the video memory; visible: the BAR's size (the
 * engine's area goes in its last MiB) */
int  rad_gfx_init(radeon_t* r, uintptr_t vram_cpu, uint64_t vram_mc, uint64_t visible, char* why, int cap);
int  rad_gfx_ready(void);
const char* rad_gfx_status(void);
/* n pages of system memory (physical addresses phys[]) at GTT page first:
 * their GPU address, or 0 */
uint64_t rad_gart_bind(uint32_t first, const uint64_t* phys, uint32_t n);
void     rad_gart_unbind(uint32_t first, uint32_t n);
uint64_t rad_gtt_base(void);
uint32_t rad_gtt_pages(void);
/* the video memory really there (MiB: probed by the engine), 0 before it runs */
uint32_t rad_vram_mb(void);
/* for the radeon DRM interface (kernel/radeon_drm.c) */
struct radeon_device* rad_gfx_rdev(void);         /* NULL until the engine is up */
uint8_t* rad_gfx_vram_cpu(void);                  /* the video memory window, as the CPU writes it */
uint64_t rad_gfx_area_off(void);                  /* the engine's own area (its last MiB) */
uint32_t rad_gfx_reserved_gtt_pages(void);        /* GTT pages 0 .. n-1 are the kernel's */
uint32_t rad_gfx_completed(void);                 /* the last fence written back */
int      rad_gfx_seq_done(uint32_t seq);
uint32_t rad_gfx_submit_ib(uint64_t gpu_addr, uint32_t ndw);   /* its fence, or 0 */
int      rad_gfx_wait(uint32_t seq, int timeout_ms);
/* kernel/radeon_drm.c: the radeon DRM interface for apps (gpu_gem.c hands
 * /dev/dri/card0's requests over while a Radeon is driven) */
int  rad_drm_active(void);
int  rad_drm_ioctl(int owner, unsigned long req, void* arg);
void rad_drm_release(int owner);
void rad_drm_stats(char* buf, int cap);
/* the microcode's licence (shown with the other notices) */
const char* rad_fw_notice(uint32_t* len);

#endif
