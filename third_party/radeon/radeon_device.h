/* SPDX-License-Identifier: MIT */
/* What the 3D engine code taken from Linux's radeon driver (evergreen_gfx.c,
 * r600_gfx.c, ni_gfx.c) uses of the driver's own structures - the fields
 * it reads and writes, nothing more - and the ring helpers it calls, which
 * Banana OS's kernel/radeon_gfx.c provides. */
#ifndef RADEON_DEVICE_SHIM_H
#define RADEON_DEVICE_SHIM_H

#include "linux_compat.h"
#include "radeon_family.h"

struct radeon;                                   /* kernel/radeon.h's */

#define RADEON_NUM_RINGS            8
#define RADEON_RING_TYPE_GFX_INDEX  0
#define CAYMAN_RING_TYPE_CP1_INDEX  1
#define CAYMAN_RING_TYPE_CP2_INDEX  2
#define R600_RING_TYPE_DMA_INDEX    3
#define CAYMAN_RING_TYPE_DMA1_INDEX 4
#define R600_RING_TYPE_UVD_INDEX    5

#define RADEON_GPU_PAGE_SIZE        4096
#define RADEON_GPU_PAGE_MASK        (RADEON_GPU_PAGE_SIZE - 1)
#define RADEON_GPU_PAGE_SHIFT       12

/* the writeback page: where the GPU reports back */
#define RADEON_WB_SCRATCH_OFFSET    0
#define RADEON_WB_RING0_NEXT_RPTR   256
#define RADEON_WB_CP_RPTR_OFFSET    1024
#define RADEON_WB_CP1_RPTR_OFFSET   1280
#define RADEON_WB_CP2_RPTR_OFFSET   1536
#define R600_WB_DMA_RPTR_OFFSET     1792
#define R600_WB_IH_WPTR_OFFSET      2048
#define CAYMAN_WB_DMA1_RPTR_OFFSET  2304
#define R600_WB_EVENT_OFFSET        3072

/* card flags (radeon_drv.h) */
#define RADEON_IS_MOBILITY          0x00010000UL
#define RADEON_IS_IGP               0x00020000UL
#define RADEON_IS_AGP               0x00080000UL
#define RADEON_IS_PCIE              0x00200000UL

#define RADEON_CP_PACKET2           0x80000000
#define RADEON_PACKET_TYPE0         0
#define RADEON_PACKET_TYPE1         1
#define RADEON_PACKET_TYPE2         2
#define RADEON_PACKET_TYPE3         3

struct firmware { const u8* data; size_t size; };

struct radeon_ring {
    u32*     ring;                  /* its memory, as the CPU writes it */
    u64      gpu_addr;
    unsigned ring_size;             /* bytes */
    unsigned wptr, wptr_old;
    unsigned ptr_mask;              /* dwords - 1 */
    unsigned align_mask;
    unsigned count_dw;
    u32      nop;
    int      idx;
    bool     ready;
    u32      rptr_save_reg;
    u64      next_rptr_gpu_addr;
    u32*     next_rptr_cpu_addr;
};

struct radeon_fence { int ring; u64 seq; };
struct radeon_ib { int ring; u64 gpu_addr; u32* ptr; u32 length_dw; struct radeon_fence* fence; bool is_const_ib; unsigned vm_id; };
struct radeon_fence_driver { u64 gpu_addr; u32* cpu_addr; u32 scratch_reg; u64 sync_seq; };

struct evergreen_config {
    unsigned num_ses, max_pipes, max_tile_pipes, max_simds, max_backends, max_gprs, max_threads, max_gs_threads;
    unsigned max_stack_entries, sx_num_of_sets, sx_max_export_size, sx_max_export_pos_size, sx_max_export_smx_size;
    unsigned max_hw_contexts, sq_num_cf_insts, sc_prim_fifo_size, sc_hiz_tile_fifo_size, sc_earlyz_tile_fifo_size;
    unsigned tile_config, backend_map, active_simds;
};

struct radeon_device {
    struct radeon* rt;              /* Banana OS's card (its registers) */
    int      family;
    unsigned long flags;
    int      usec_timeout;
    struct { struct evergreen_config evergreen; } config;
    struct { bool ready; void* robj; u64 table_addr; u32* ptr; unsigned num_gpu_pages; } gart;
    struct { u64 vram_start, vram_end, real_vram_size, visible_vram_size, gtt_start, gtt_end, gtt_size, mc_vram_size; } mc;
    struct { u64 addr; } dummy_page;
    const struct firmware *pfp_fw, *me_fw, *rlc_fw, *mc_fw, *smc_fw;
    struct radeon_ring ring[RADEON_NUM_RINGS];
    struct radeon_fence_driver fence_drv[RADEON_NUM_RINGS];
    struct { bool enabled, use_event; u64 gpu_addr; u32* wb; } wb;
    struct { unsigned device; } pdev_store, *pdev;
    void*    dev;
    struct { struct { int copy_ring_index; } copy; } asic_store, *asic;
    u32      scratch_free;          /* (a bit per SCRATCH_REG) */
};

/* register access (kernel/radeon.h's) */
uint32_t rad_rd(struct radeon* r, uint32_t reg);
void     rad_wr(struct radeon* r, uint32_t reg, uint32_t v);
#define RREG32(reg)        rad_rd(rdev->rt, (reg))
#define WREG32(reg, v)     rad_wr(rdev->rt, (reg), (v))
#define WREG32_P(reg, val, mask) do { u32 tmp_ = RREG32(reg); tmp_ &= (mask); tmp_ |= ((val) & ~(mask)); WREG32(reg, tmp_); } while (0)
#define WREG32_AND(reg, and) WREG32_P(reg, 0, and)
#define WREG32_OR(reg, or)   WREG32_P(reg, or, ~(or))
u32  radeon_rcu_rreg(struct radeon_device* rdev, u32 reg);
#define RREG32_RCU(reg)    radeon_rcu_rreg(rdev, (reg))

/* the ring (kernel/radeon_gfx.c) */
int  radeon_ring_lock(struct radeon_device* rdev, struct radeon_ring* ring, unsigned ndw);
void radeon_ring_unlock_commit(struct radeon_device* rdev, struct radeon_ring* ring, bool hdp_flush);
static inline void radeon_ring_write(struct radeon_ring* ring, u32 v) {
    if (ring->count_dw <= 0) return;           /* (more than was locked: dropped, as Linux warns) */
    ring->ring[ring->wptr++] = v;
    ring->wptr &= ring->ptr_mask;
    ring->count_dw--;
}
int  radeon_ring_test(struct radeon_device* rdev, int idx, struct radeon_ring* ring);
int  radeon_scratch_get(struct radeon_device* rdev, u32* reg);
void radeon_scratch_free(struct radeon_device* rdev, u32 reg);
static inline int  radeon_gart_table_vram_pin(struct radeon_device* rdev) { (void)rdev; return 0; }
static inline void radeon_gart_table_vram_unpin(struct radeon_device* rdev) { (void)rdev; }
static inline void radeon_ttm_set_active_vram_size(struct radeon_device* rdev, u64 size) { (void)rdev; (void)size; }
int  pcie_get_readrq(void* pdev);
int  pcie_set_readrq(void* pdev, int rq);

/* the parts each file gives the others (and kernel/radeon_gfx.c) */
int  r600_count_pipe_bits(uint32_t val);
u32  r6xx_remap_render_backend(struct radeon_device* rdev, u32 tiling_pipe_num, u32 max_rb_num,
                               u32 total_max_rb_num, u32 disabled_rb_mask);
int  r600_ring_test(struct radeon_device* rdev, struct radeon_ring* ring);
void r600_fence_ring_emit(struct radeon_device* rdev, struct radeon_fence* fence);
void r600_rlc_stop(struct radeon_device* rdev);
void r700_cp_stop(struct radeon_device* rdev);
int  evergreen_mc_wait_for_idle(struct radeon_device* rdev);
void evergreen_pcie_gart_tlb_flush(struct radeon_device* rdev);
int  evergreen_pcie_gart_enable(struct radeon_device* rdev);
void evergreen_pcie_gart_disable(struct radeon_device* rdev);
void evergreen_ring_ib_execute(struct radeon_device* rdev, struct radeon_ib* ib);
int  evergreen_cp_load_microcode(struct radeon_device* rdev);
int  evergreen_cp_start(struct radeon_device* rdev);
int  evergreen_cp_resume(struct radeon_device* rdev);
void evergreen_gpu_init(struct radeon_device* rdev);
void evergreen_rlc_start(struct radeon_device* rdev);
int  evergreen_rlc_resume(struct radeon_device* rdev);
void evergreen_fix_pci_max_read_req_size(struct radeon_device* rdev);
int  ni_mc_load_microcode(struct radeon_device* rdev);

/* small helpers the code uses */
static inline u32 be32_to_cpup(const u32* p) { return __builtin_bswap32(*p); }
#define __be32 u32
static inline u32 upper_32_bits(u64 v) { return (u32)(v >> 32); }
static inline u32 lower_32_bits(u64 v) { return (u32)v; }
static inline unsigned hweight32(u32 v) { return (unsigned)__builtin_popcount(v); }
static inline int ffs(int v) { return __builtin_ffs(v); }
static inline unsigned order_base_2(unsigned long n) { unsigned r = 0; while ((1ul << r) < n) r++; return r; }
#define dev_err(dev, ...) klog("radeon: " __VA_ARGS__)
#define dev_warn(dev, ...) klog("radeon: " __VA_ARGS__)
#define pr_warn(...) klog("radeon: " __VA_ARGS__)
#define pr_err(...) klog("radeon: " __VA_ARGS__)
#define DRM_DEBUG_DRIVER(...) do { } while (0)
#define BUG_ON(c) do { if (c) klog("radeon: BUG_ON(%s) at %s:%d\n", #c, __FILE__, __LINE__); } while (0)
#define unlikely(x) (x)
#define likely(x) (x)
#define mb()  __asm__ volatile("mfence" ::: "memory")
#define wmb() __asm__ volatile("sfence" ::: "memory")
#define rmb() __asm__ volatile("lfence" ::: "memory")
#define EBUSY 16
#define ETIMEDOUT 110
#define EIO 5

#endif
