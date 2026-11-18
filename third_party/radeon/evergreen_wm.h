/* SPDX-License-Identifier: MIT */
/* Banana OS: Linux's Evergreen display watermarks (evergreen_wm.c) */
#ifndef EVERGREEN_WM_H
#define EVERGREEN_WM_H
#include "../../kernel/types.h"
struct evergreen_wm_in {
	uint32_t clock_khz, htotal, hdisplay;   /* the mode */
	uint32_t mclk_khz, sclk_khz;            /* memory, engine clocks */
	uint32_t dram_channels, lb_size, num_heads;
};
struct evergreen_wm_out {
	uint32_t line_time, latency, priority_mark;   /* ns, ns, PRIORITY_A/B_CNT's mark */
	int linux_forces_high;                         /* (Linux's checks would force priority high) */
};
void evergreen_wm_compute(const struct evergreen_wm_in *in, struct evergreen_wm_out *out);
#endif
