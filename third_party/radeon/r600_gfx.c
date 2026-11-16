/*
 * Copyright 2010 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Alex Deucher, Dave Airlie, Jerome Glisse
 */
/* From Linux's drivers/gpu/drm/radeon/r600.c (v6.12): the render backend
 * map, the ring test, fences, the RLC's stop / start - shared by every part
 * since the R600. Banana OS: radeon_device.h stands in for the driver's
 * structures. */
#include "radeon_device.h"
#include "r600d.h"

int r600_count_pipe_bits(uint32_t val)
{
	return hweight32(val);
}

u32 r6xx_remap_render_backend(struct radeon_device *rdev,
			      u32 tiling_pipe_num,
			      u32 max_rb_num,
			      u32 total_max_rb_num,
			      u32 disabled_rb_mask)
{
	u32 rendering_pipe_num, rb_num_width, req_rb_num;
	u32 pipe_rb_ratio, pipe_rb_remain, tmp;
	u32 data = 0, mask = 1 << (max_rb_num - 1);
	unsigned i, j;

	/* mask out the RBs that don't exist on that asic */
	tmp = disabled_rb_mask | ((0xff << max_rb_num) & 0xff);
	/* make sure at least one RB is available */
	if ((tmp & 0xff) != 0xff)
		disabled_rb_mask = tmp;

	rendering_pipe_num = 1 << tiling_pipe_num;
	req_rb_num = total_max_rb_num - r600_count_pipe_bits(disabled_rb_mask);
	BUG_ON(rendering_pipe_num < req_rb_num);

	pipe_rb_ratio = rendering_pipe_num / req_rb_num;
	pipe_rb_remain = rendering_pipe_num - pipe_rb_ratio * req_rb_num;

	if (rdev->family <= CHIP_RV740) {
		/* r6xx/r7xx */
		rb_num_width = 2;
	} else {
		/* eg+ */
		rb_num_width = 4;
	}

	for (i = 0; i < max_rb_num; i++) {
		if (!(mask & disabled_rb_mask)) {
			for (j = 0; j < pipe_rb_ratio; j++) {
				data <<= rb_num_width;
				data |= max_rb_num - i - 1;
			}
			if (pipe_rb_remain) {
				data <<= rb_num_width;
				data |= max_rb_num - i - 1;
				pipe_rb_remain--;
			}
		}
		mask >>= 1;
	}

	return data;
}

int r600_ring_test(struct radeon_device *rdev, struct radeon_ring *ring)
{
	uint32_t scratch;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	r = radeon_scratch_get(rdev, &scratch);
	if (r) {
		DRM_ERROR("radeon: cp failed to get scratch reg (%d).\n", r);
		return r;
	}
	WREG32(scratch, 0xCAFEDEAD);
	r = radeon_ring_lock(rdev, ring, 3);
	if (r) {
		DRM_ERROR("radeon: cp failed to lock ring %d (%d).\n", ring->idx, r);
		radeon_scratch_free(rdev, scratch);
		return r;
	}
	radeon_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
	radeon_ring_write(ring, ((scratch - PACKET3_SET_CONFIG_REG_OFFSET) >> 2));
	radeon_ring_write(ring, 0xDEADBEEF);
	radeon_ring_unlock_commit(rdev, ring, false);
	for (i = 0; i < rdev->usec_timeout; i++) {
		tmp = RREG32(scratch);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}
	if (i < rdev->usec_timeout) {
		DRM_INFO("ring test on %d succeeded in %d usecs\n", ring->idx, i);
	} else {
		DRM_ERROR("radeon: ring %d test failed (scratch(0x%04X)=0x%08X)\n",
			  ring->idx, scratch, tmp);
		r = -EINVAL;
	}
	radeon_scratch_free(rdev, scratch);
	return r;
}

void r600_fence_ring_emit(struct radeon_device *rdev,
			  struct radeon_fence *fence)
{
	struct radeon_ring *ring = &rdev->ring[fence->ring];
	u32 cp_coher_cntl = PACKET3_TC_ACTION_ENA | PACKET3_VC_ACTION_ENA |
		PACKET3_SH_ACTION_ENA;

	if (rdev->family >= CHIP_RV770)
		cp_coher_cntl |= PACKET3_FULL_CACHE_ENA;

	if (rdev->wb.use_event) {
		u64 addr = rdev->fence_drv[fence->ring].gpu_addr;
		/* flush read cache over gart */
		radeon_ring_write(ring, PACKET3(PACKET3_SURFACE_SYNC, 3));
		radeon_ring_write(ring, cp_coher_cntl);
		radeon_ring_write(ring, 0xFFFFFFFF);
		radeon_ring_write(ring, 0);
		radeon_ring_write(ring, 10); /* poll interval */
		/* EVENT_WRITE_EOP - flush caches, send int */
		radeon_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
		radeon_ring_write(ring, EVENT_TYPE(CACHE_FLUSH_AND_INV_EVENT_TS) | EVENT_INDEX(5));
		radeon_ring_write(ring, lower_32_bits(addr));
		radeon_ring_write(ring, (upper_32_bits(addr) & 0xff) | DATA_SEL(1) | INT_SEL(2));
		radeon_ring_write(ring, fence->seq);
		radeon_ring_write(ring, 0);
	} else {
		/* flush read cache over gart */
		radeon_ring_write(ring, PACKET3(PACKET3_SURFACE_SYNC, 3));
		radeon_ring_write(ring, cp_coher_cntl);
		radeon_ring_write(ring, 0xFFFFFFFF);
		radeon_ring_write(ring, 0);
		radeon_ring_write(ring, 10); /* poll interval */
		radeon_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE, 0));
		radeon_ring_write(ring, EVENT_TYPE(CACHE_FLUSH_AND_INV_EVENT) | EVENT_INDEX(0));
		/* wait for 3D idle clean */
		radeon_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
		radeon_ring_write(ring, (WAIT_UNTIL - PACKET3_SET_CONFIG_REG_OFFSET) >> 2);
		radeon_ring_write(ring, WAIT_3D_IDLE_bit | WAIT_3D_IDLECLEAN_bit);
		/* Emit fence sequence & fire IRQ */
		radeon_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
		radeon_ring_write(ring, ((rdev->fence_drv[fence->ring].scratch_reg - PACKET3_SET_CONFIG_REG_OFFSET) >> 2));
		radeon_ring_write(ring, fence->seq);
		/* CP_INTERRUPT packet 3 no longer exists, use packet 0 */
		radeon_ring_write(ring, PACKET0(CP_INT_STATUS, 0));
		radeon_ring_write(ring, RB_INT_STAT);
	}
}

void r600_rlc_stop(struct radeon_device *rdev)
{

	if ((rdev->family >= CHIP_RV770) &&
	    (rdev->family <= CHIP_RV740)) {
		/* r7xx asics need to soft reset RLC before halting */
		WREG32(SRBM_SOFT_RESET, SOFT_RESET_RLC);
		RREG32(SRBM_SOFT_RESET);
		mdelay(15);
		WREG32(SRBM_SOFT_RESET, 0);
		RREG32(SRBM_SOFT_RESET);
	}

	WREG32(RLC_CNTL, 0);
}

