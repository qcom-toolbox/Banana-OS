/*
 * Copyright 2010 Advanced Micro Devices, Inc.
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
 * Authors: Alex Deucher
 */
/* Banana OS: the display watermarks of Linux's evergreen.c (v6.12),
 * evergreen_dram_bandwidth() .. evergreen_check_latency_hiding() and the
 * struct they take, unchanged; drm_fixed.h's 20.12 fixed point below as
 * Linux has it. evergreen_wm_compute() is evergreen_program_watermarks()'s
 * arithmetic for one pipe, without its register writes (kernel/gpu_radeon.c
 * does them). */
#include "../../kernel/types.h"
#include "evergreen_wm.h"

#ifndef __cplusplus
typedef _Bool bool;
#define true 1
#define false 0
#endif
typedef uint32_t u32;
typedef uint64_t u64;
typedef union dfixed { u32 full; } fixed20_12;
#define dfixed_const(A) (u32)(((A) << 12))
#define dfixed_mul(A, B) ((u64)((u64)(A).full * (B).full + 2048) >> 12)
#define dfixed_trunc(A) ((A).full >> 12)
static inline u32 dfixed_div(fixed20_12 A, fixed20_12 B) {
	u64 tmp = ((u64)A.full << 13);
	tmp /= B.full;
	tmp += 1;
	tmp /= 2;
	return (u32)tmp;
}
#define min(a, b) ((a) < (b) ? (a) : (b))

struct evergreen_wm_params {
	u32 dram_channels; /* number of dram channels */
	u32 yclk;          /* bandwidth per dram data pin in kHz */
	u32 sclk;          /* engine clock in kHz */
	u32 disp_clk;      /* display clock in kHz */
	u32 src_width;     /* viewport width */
	u32 active_time;   /* active display time in ns */
	u32 blank_time;    /* blank time in ns */
	bool interlaced;    /* mode is interlaced */
	fixed20_12 vsc;    /* vertical scale ratio */
	u32 num_heads;     /* number of active crtcs */
	u32 bytes_per_pixel; /* bytes per pixel display + overlay */
	u32 lb_size;       /* line buffer allocated to pipe */
	u32 vtaps;         /* vertical scaler taps */
};

static u32 evergreen_dram_bandwidth(struct evergreen_wm_params *wm)
{
	/* Calculate DRAM Bandwidth and the part allocated to display. */
	fixed20_12 dram_efficiency; /* 0.7 */
	fixed20_12 yclk, dram_channels, bandwidth;
	fixed20_12 a;

	a.full = dfixed_const(1000);
	yclk.full = dfixed_const(wm->yclk);
	yclk.full = dfixed_div(yclk, a);
	dram_channels.full = dfixed_const(wm->dram_channels * 4);
	a.full = dfixed_const(10);
	dram_efficiency.full = dfixed_const(7);
	dram_efficiency.full = dfixed_div(dram_efficiency, a);
	bandwidth.full = dfixed_mul(dram_channels, yclk);
	bandwidth.full = dfixed_mul(bandwidth, dram_efficiency);

	return dfixed_trunc(bandwidth);
}

static u32 evergreen_dram_bandwidth_for_display(struct evergreen_wm_params *wm)
{
	/* Calculate DRAM Bandwidth and the part allocated to display. */
	fixed20_12 disp_dram_allocation; /* 0.3 to 0.7 */
	fixed20_12 yclk, dram_channels, bandwidth;
	fixed20_12 a;

	a.full = dfixed_const(1000);
	yclk.full = dfixed_const(wm->yclk);
	yclk.full = dfixed_div(yclk, a);
	dram_channels.full = dfixed_const(wm->dram_channels * 4);
	a.full = dfixed_const(10);
	disp_dram_allocation.full = dfixed_const(3); /* XXX worse case value 0.3 */
	disp_dram_allocation.full = dfixed_div(disp_dram_allocation, a);
	bandwidth.full = dfixed_mul(dram_channels, yclk);
	bandwidth.full = dfixed_mul(bandwidth, disp_dram_allocation);

	return dfixed_trunc(bandwidth);
}

static u32 evergreen_data_return_bandwidth(struct evergreen_wm_params *wm)
{
	/* Calculate the display Data return Bandwidth */
	fixed20_12 return_efficiency; /* 0.8 */
	fixed20_12 sclk, bandwidth;
	fixed20_12 a;

	a.full = dfixed_const(1000);
	sclk.full = dfixed_const(wm->sclk);
	sclk.full = dfixed_div(sclk, a);
	a.full = dfixed_const(10);
	return_efficiency.full = dfixed_const(8);
	return_efficiency.full = dfixed_div(return_efficiency, a);
	a.full = dfixed_const(32);
	bandwidth.full = dfixed_mul(a, sclk);
	bandwidth.full = dfixed_mul(bandwidth, return_efficiency);

	return dfixed_trunc(bandwidth);
}

static u32 evergreen_dmif_request_bandwidth(struct evergreen_wm_params *wm)
{
	/* Calculate the DMIF Request Bandwidth */
	fixed20_12 disp_clk_request_efficiency; /* 0.8 */
	fixed20_12 disp_clk, bandwidth;
	fixed20_12 a;

	a.full = dfixed_const(1000);
	disp_clk.full = dfixed_const(wm->disp_clk);
	disp_clk.full = dfixed_div(disp_clk, a);
	a.full = dfixed_const(10);
	disp_clk_request_efficiency.full = dfixed_const(8);
	disp_clk_request_efficiency.full = dfixed_div(disp_clk_request_efficiency, a);
	a.full = dfixed_const(32);
	bandwidth.full = dfixed_mul(a, disp_clk);
	bandwidth.full = dfixed_mul(bandwidth, disp_clk_request_efficiency);

	return dfixed_trunc(bandwidth);
}

static u32 evergreen_available_bandwidth(struct evergreen_wm_params *wm)
{
	/* Calculate the Available bandwidth. Display can use this temporarily but not in average. */
	u32 dram_bandwidth = evergreen_dram_bandwidth(wm);
	u32 data_return_bandwidth = evergreen_data_return_bandwidth(wm);
	u32 dmif_req_bandwidth = evergreen_dmif_request_bandwidth(wm);

	return min(dram_bandwidth, min(data_return_bandwidth, dmif_req_bandwidth));
}

static u32 evergreen_average_bandwidth(struct evergreen_wm_params *wm)
{
	/* Calculate the display mode Average Bandwidth
	 * DisplayMode should contain the source and destination dimensions,
	 * timing, etc.
	 */
	fixed20_12 bpp;
	fixed20_12 line_time;
	fixed20_12 src_width;
	fixed20_12 bandwidth;
	fixed20_12 a;

	a.full = dfixed_const(1000);
	line_time.full = dfixed_const(wm->active_time + wm->blank_time);
	line_time.full = dfixed_div(line_time, a);
	bpp.full = dfixed_const(wm->bytes_per_pixel);
	src_width.full = dfixed_const(wm->src_width);
	bandwidth.full = dfixed_mul(src_width, bpp);
	bandwidth.full = dfixed_mul(bandwidth, wm->vsc);
	bandwidth.full = dfixed_div(bandwidth, line_time);

	return dfixed_trunc(bandwidth);
}

static u32 evergreen_latency_watermark(struct evergreen_wm_params *wm)
{
	/* First calcualte the latency in ns */
	u32 mc_latency = 2000; /* 2000 ns. */
	u32 available_bandwidth = evergreen_available_bandwidth(wm);
	u32 worst_chunk_return_time = (512 * 8 * 1000) / available_bandwidth;
	u32 cursor_line_pair_return_time = (128 * 4 * 1000) / available_bandwidth;
	u32 dc_latency = 40000000 / wm->disp_clk; /* dc pipe latency */
	u32 other_heads_data_return_time = ((wm->num_heads + 1) * worst_chunk_return_time) +
		(wm->num_heads * cursor_line_pair_return_time);
	u32 latency = mc_latency + other_heads_data_return_time + dc_latency;
	u32 max_src_lines_per_dst_line, lb_fill_bw, line_fill_time;
	fixed20_12 a, b, c;

	if (wm->num_heads == 0)
		return 0;

	a.full = dfixed_const(2);
	b.full = dfixed_const(1);
	if ((wm->vsc.full > a.full) ||
	    ((wm->vsc.full > b.full) && (wm->vtaps >= 3)) ||
	    (wm->vtaps >= 5) ||
	    ((wm->vsc.full >= a.full) && wm->interlaced))
		max_src_lines_per_dst_line = 4;
	else
		max_src_lines_per_dst_line = 2;

	a.full = dfixed_const(available_bandwidth);
	b.full = dfixed_const(wm->num_heads);
	a.full = dfixed_div(a, b);

	lb_fill_bw = min(dfixed_trunc(a), wm->disp_clk * wm->bytes_per_pixel / 1000);

	a.full = dfixed_const(max_src_lines_per_dst_line * wm->src_width * wm->bytes_per_pixel);
	b.full = dfixed_const(1000);
	c.full = dfixed_const(lb_fill_bw);
	b.full = dfixed_div(c, b);
	a.full = dfixed_div(a, b);
	line_fill_time = dfixed_trunc(a);

	if (line_fill_time < wm->active_time)
		return latency;
	else
		return latency + (line_fill_time - wm->active_time);

}

static bool evergreen_average_bandwidth_vs_dram_bandwidth_for_display(struct evergreen_wm_params *wm)
{
	if (evergreen_average_bandwidth(wm) <=
	    (evergreen_dram_bandwidth_for_display(wm) / wm->num_heads))
		return true;
	else
		return false;
};

static bool evergreen_average_bandwidth_vs_available_bandwidth(struct evergreen_wm_params *wm)
{
	if (evergreen_average_bandwidth(wm) <=
	    (evergreen_available_bandwidth(wm) / wm->num_heads))
		return true;
	else
		return false;
};

static bool evergreen_check_latency_hiding(struct evergreen_wm_params *wm)
{
	u32 lb_partitions = wm->lb_size / wm->src_width;
	u32 line_time = wm->active_time + wm->blank_time;
	u32 latency_tolerant_lines;
	u32 latency_hiding;
	fixed20_12 a;

	a.full = dfixed_const(1);
	if (wm->vsc.full > a.full)
		latency_tolerant_lines = 1;
	else {
		if (lb_partitions <= (wm->vtaps + 1))
			latency_tolerant_lines = 1;
		else
			latency_tolerant_lines = 2;
	}

	latency_hiding = (latency_tolerant_lines * line_time + wm->blank_time);

	if (evergreen_latency_watermark(wm) <= latency_hiding)
		return true;
	else
		return false;
}

void evergreen_wm_compute(const struct evergreen_wm_in *in, struct evergreen_wm_out *out)
{
	struct evergreen_wm_params wm;
	u32 active_time = (u32)((u64)in->hdisplay * 1000000 / in->clock_khz);
	u32 line_time = (u32)((u64)in->htotal * 1000000 / in->clock_khz);

	line_time = min(line_time, (u32)65535);
	wm.yclk = in->mclk_khz;
	wm.sclk = in->sclk_khz;
	wm.disp_clk = in->clock_khz;
	wm.src_width = in->hdisplay;
	wm.active_time = active_time;
	wm.blank_time = line_time - active_time;
	wm.interlaced = false;
	wm.vsc.full = dfixed_const(1);
	wm.vtaps = 1;
	wm.bytes_per_pixel = 4;
	wm.lb_size = in->lb_size;
	wm.dram_channels = in->dram_channels;
	wm.num_heads = in->num_heads;

	out->line_time = line_time;
	out->latency = min(evergreen_latency_watermark(&wm), (u32)65535);
	out->linux_forces_high = !evergreen_average_bandwidth_vs_dram_bandwidth_for_display(&wm) ||
		!evergreen_average_bandwidth_vs_available_bandwidth(&wm) ||
		!evergreen_check_latency_hiding(&wm);

	/* the priority mark: latency (ns) x pixel clock (MHz) x hsc (1) / 16.
	 * (Linux computes it in 20.12 fixed point, whose product wraps past 32
	 * bits for latencies over ~7 us - 1080p on a 64-bit DDR3 card gives 17 -
	 * and then programs a mark five times too small; here in 64 bits.) */
	out->priority_mark = (u32)((u64)out->latency * in->clock_khz / 1000 / 1000 / 16);
}
