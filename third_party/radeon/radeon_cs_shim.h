/* SPDX-License-Identifier: MIT */
/* What the command-stream checker (radeon_cs_check.c, from Linux) uses of
 * the radeon driver's structures - the parser, its chunks, the buffers it
 * relocates - and the kernel helpers it calls, on Banana OS. */
#ifndef RADEON_CS_SHIM_H
#define RADEON_CS_SHIM_H

#include "radeon_device.h"
#include "evergreend.h"

/* (radeon_drm.h's tiling flags; the kernel has no <drm/drm.h>) */
#define RADEON_TILING_MACRO                     0x1
#define RADEON_TILING_MICRO                     0x2
#define RADEON_TILING_SWAP_16BIT                0x4
#define RADEON_TILING_R600_NO_SCANOUT           RADEON_TILING_SWAP_16BIT
#define RADEON_TILING_SWAP_32BIT                0x8
#define RADEON_TILING_SURFACE                   0x10
#define RADEON_TILING_MICRO_SQUARE              0x20
#define RADEON_TILING_EG_BANKW_SHIFT            8
#define RADEON_TILING_EG_BANKW_MASK             0xf
#define RADEON_TILING_EG_BANKH_SHIFT            12
#define RADEON_TILING_EG_BANKH_MASK             0xf
#define RADEON_TILING_EG_MACRO_TILE_ASPECT_SHIFT 16
#define RADEON_TILING_EG_MACRO_TILE_ASPECT_MASK 0xf
#define RADEON_TILING_EG_TILE_SPLIT_SHIFT       24
#define RADEON_TILING_EG_TILE_SPLIT_MASK        0xf
#define RADEON_TILING_EG_STENCIL_TILE_SPLIT_SHIFT 28
#define RADEON_TILING_EG_STENCIL_TILE_SPLIT_MASK 0xf
#define RADEON_CS_KEEP_TILING_FLAGS             0x01
#define RADEON_CS_USE_VM                        0x02
#define RADEON_CS_END_OF_FRAME                  0x04

/* packets (radeon_reg.h) */
#define RADEON_CP_PACKET_GET_TYPE(h)       (((h) >> 30) & 3)
#define RADEON_CP_PACKET_GET_COUNT(h)      (((h) >> 16) & 0x3FFF)
#define RADEON_CP_PACKET0_GET_ONE_REG_WR(h) (((h) >> 15) & 1)
#define RADEON_CP_PACKET3_GET_OPCODE(h)    (((h) >> 8) & 0xFF)
#define R100_CP_PACKET0_GET_REG(h)         (((h) & 0x1FFF) << 2)
#define R600_CP_PACKET0_GET_REG(h)         (((h) & 0xFFFF) << 2)
#define RADEON_PACKET3_NOP                 0x10

/* the texture formats (r600d.h: SQ_TEX_RESOURCE_WORD1 0x038004) */
#define     V_038004_COLOR_INVALID                     0x00000000
#define     V_038004_COLOR_8                           0x00000001
#define     V_038004_COLOR_4_4                         0x00000002
#define     V_038004_COLOR_3_3_2                       0x00000003
#define     V_038004_COLOR_16                          0x00000005
#define     V_038004_COLOR_16_FLOAT                    0x00000006
#define     V_038004_COLOR_8_8                         0x00000007
#define     V_038004_COLOR_5_6_5                       0x00000008
#define     V_038004_COLOR_6_5_5                       0x00000009
#define     V_038004_COLOR_1_5_5_5                     0x0000000A
#define     V_038004_COLOR_4_4_4_4                     0x0000000B
#define     V_038004_COLOR_5_5_5_1                     0x0000000C
#define     V_038004_COLOR_32                          0x0000000D
#define     V_038004_COLOR_32_FLOAT                    0x0000000E
#define     V_038004_COLOR_16_16                       0x0000000F
#define     V_038004_COLOR_16_16_FLOAT                 0x00000010
#define     V_038004_COLOR_8_24                        0x00000011
#define     V_038004_COLOR_8_24_FLOAT                  0x00000012
#define     V_038004_COLOR_24_8                        0x00000013
#define     V_038004_COLOR_24_8_FLOAT                  0x00000014
#define     V_038004_COLOR_10_11_11                    0x00000015
#define     V_038004_COLOR_10_11_11_FLOAT              0x00000016
#define     V_038004_COLOR_11_11_10                    0x00000017
#define     V_038004_COLOR_11_11_10_FLOAT              0x00000018
#define     V_038004_COLOR_2_10_10_10                  0x00000019
#define     V_038004_COLOR_8_8_8_8                     0x0000001A
#define     V_038004_COLOR_10_10_10_2                  0x0000001B
#define     V_038004_COLOR_X24_8_32_FLOAT              0x0000001C
#define     V_038004_COLOR_32_32                       0x0000001D
#define     V_038004_COLOR_32_32_FLOAT                 0x0000001E
#define     V_038004_COLOR_16_16_16_16                 0x0000001F
#define     V_038004_COLOR_16_16_16_16_FLOAT           0x00000020
#define     V_038004_COLOR_32_32_32_32                 0x00000022
#define     V_038004_COLOR_32_32_32_32_FLOAT           0x00000023
#define     V_038004_FMT_1                             0x00000025
#define     V_038004_FMT_GB_GR                         0x00000027
#define     V_038004_FMT_BG_RG                         0x00000028
#define     V_038004_FMT_32_AS_8                       0x00000029
#define     V_038004_FMT_32_AS_8_8                     0x0000002A
#define     V_038004_FMT_5_9_9_9_SHAREDEXP             0x0000002B
#define     V_038004_FMT_8_8_8                         0x0000002C
#define     V_038004_FMT_16_16_16                      0x0000002D
#define     V_038004_FMT_16_16_16_FLOAT                0x0000002E
#define     V_038004_FMT_32_32_32                      0x0000002F
#define     V_038004_FMT_32_32_32_FLOAT                0x00000030
#define     V_038004_FMT_BC1                           0x00000031
#define     V_038004_FMT_BC2                           0x00000032
#define     V_038004_FMT_BC3                           0x00000033
#define     V_038004_FMT_BC4                           0x00000034
#define     V_038004_FMT_BC5                           0x00000035
#define     V_038004_FMT_BC6                           0x00000036
#define     V_038004_FMT_BC7                           0x00000037
#define     V_038004_FMT_32_AS_32_32_32_32             0x00000038

/* a buffer as the checker sees it: its size */
struct radeon_bo { uint64_t size; };
static inline unsigned long radeon_bo_size(struct radeon_bo* bo) { return (unsigned long)bo->size; }

struct radeon_bo_list {
    struct radeon_bo* robj;
    uint64_t gpu_offset;           /* where the GPU has it */
    unsigned preferred_domains, allowed_domains;
    uint32_t tiling_flags;
    uint32_t handle, write;        /* (Banana OS: its handle, written by the submission) */
};

struct radeon_cs_chunk {
    uint32_t  length_dw;
    uint32_t* kdata;
    void*     user_ptr;
};

struct radeon_cs_packet {
    unsigned idx, type, reg, opcode;
    int      count;
    unsigned one_reg_wr;
};

struct radeon_cs_parser {
    void*                   dev;
    struct radeon_device*   rdev;
    unsigned                idx;
    unsigned                nrelocs;
    struct radeon_bo_list*  relocs;
    struct radeon_cs_chunk* chunk_ib;
    struct radeon_cs_chunk* chunk_relocs;
    struct radeon_cs_chunk* chunk_flags;
    struct radeon_ib        ib;
    void*                   track;
    unsigned                family;
    int                     parser_error;
    u32                     cs_flags;
    u32                     ring;
};

static inline u32 radeon_get_ib_value(struct radeon_cs_parser* p, int idx) {
    struct radeon_cs_chunk* ibc = p->chunk_ib;
    if (ibc->kdata) return ibc->kdata[idx];
    return p->ib.ptr[idx];
}

int  radeon_cs_packet_parse(struct radeon_cs_parser* p, struct radeon_cs_packet* pkt, unsigned idx);
bool radeon_cs_packet_next_is_pkt3_nop(struct radeon_cs_parser* p);
void radeon_cs_dump_packet(struct radeon_cs_parser* p, struct radeon_cs_packet* pkt);
int  radeon_cs_packet_next_reloc(struct radeon_cs_parser* p, struct radeon_bo_list** cs_reloc, int nomm);
int  evergreen_cs_parse(struct radeon_cs_parser* p);
/* (display waits - WAIT_REG_MEM on a CRTC's vline - are refused: Banana OS's
 *  apps draw off screen, the desktop presents) */
static inline int r600_cs_common_vline_parse(struct radeon_cs_parser* p, uint32_t* vline_start_end, uint32_t* vline_status) {
    (void)p; (void)vline_start_end; (void)vline_status;
    return -EINVAL;
}

/* the kernel's helpers it calls */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#endif
#ifndef IS_ALIGNED
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#endif
#ifndef DRM_ERROR
#define DRM_ERROR(...) klog("radeon cs: " __VA_ARGS__)
#endif
#ifndef DRM_INFO
#define DRM_INFO(...) klog("radeon cs: " __VA_ARGS__)
#endif
#define DRM_DEBUG(...) do { } while (0)
#define dev_warn_once(dev, ...) klog("radeon cs: " __VA_ARGS__)
#ifndef printk
#define printk(...) klog(__VA_ARGS__)
#endif
#define WARN_ON(c) (c)
#define dev_warn_ratelimited(dev, ...) klog("radeon cs: " __VA_ARGS__)
#define BUILD_BUG_ON(c) _Static_assert(!(c), #c)
#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))(y) - 1)) + 1)
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
static inline unsigned long roundup_pow_of_two(unsigned long n) { unsigned long r = 1; while (r < n) r <<= 1; return r; }
/* (evergreen_reg.h: surface tiling field values) */
#       define EVERGREEN_ADDR_SURF_2_BANK               0
#       define EVERGREEN_ADDR_SURF_4_BANK               1
#       define EVERGREEN_ADDR_SURF_8_BANK               2
#       define EVERGREEN_ADDR_SURF_16_BANK              3
#       define EVERGREEN_ADDR_SURF_BANK_WIDTH_1         0
#       define EVERGREEN_ADDR_SURF_BANK_WIDTH_2         1
#       define EVERGREEN_ADDR_SURF_BANK_WIDTH_4         2
#       define EVERGREEN_ADDR_SURF_BANK_WIDTH_8         3
#       define EVERGREEN_ADDR_SURF_BANK_HEIGHT_1        0
#       define EVERGREEN_ADDR_SURF_BANK_HEIGHT_2        1
#       define EVERGREEN_ADDR_SURF_BANK_HEIGHT_4        2
#       define EVERGREEN_ADDR_SURF_BANK_HEIGHT_8        3
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_64B       0
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_128B      1
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_256B      2
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_512B      3
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_1KB       4
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_2KB       5
#       define EVERGREEN_ADDR_SURF_TILE_SPLIT_4KB       6
#       define EVERGREEN_ADDR_SURF_MACRO_TILE_ASPECT_1  0
#       define EVERGREEN_ADDR_SURF_MACRO_TILE_ASPECT_2  1
#       define EVERGREEN_ADDR_SURF_MACRO_TILE_ASPECT_4  2
#       define EVERGREEN_ADDR_SURF_MACRO_TILE_ASPECT_8  3
/* (the CRTCs' vline registers: evergreen_reg.h) */
#define EVERGREEN_VLINE_START_END                       0x6b08
#define EVERGREEN_VLINE_STATUS                          0x6bb8
#define EVERGREEN_CRTC0_REGISTER_OFFSET                 (0x6df0 - 0x6df0)
#define EVERGREEN_CRTC1_REGISTER_OFFSET                 (0x79f0 - 0x6df0)
#define EVERGREEN_CRTC2_REGISTER_OFFSET                 (0x105f0 - 0x6df0)
#define EVERGREEN_CRTC3_REGISTER_OFFSET                 (0x111f0 - 0x6df0)
#define EVERGREEN_CRTC4_REGISTER_OFFSET                 (0x11df0 - 0x6df0)
#define EVERGREEN_CRTC5_REGISTER_OFFSET                 (0x129f0 - 0x6df0)
#define ENOENT 2

#endif
