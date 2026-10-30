/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BANANA_IWX_MSG_REGS_H
#define BANANA_IWX_MSG_REGS_H
#include "../../kernel/types.h"
#define __packed __attribute__((packed))
/*	$OpenBSD: if_iwxreg.h,v 1.75 2026/05/28 10:51:52 kirill Exp $	*/

/*-
 * Based on BSD-licensed source modules in the Linux iwlwifi driver,
 * which were used as the reference documentation for this implementation.
 *
 ******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************
 */


#define IWX_TFH_NUM_TBS		25

/**
 * struct iwx_tfh_tb transmit buffer descriptor within transmit frame descriptor
 *
 * This structure contains dma address and length of transmission address
 *
 * @tb_len length of the tx buffer
 * @addr 64 bits dma address
 */
struct iwx_tfh_tb {
	uint16_t tb_len;
	uint64_t addr;
} __packed;

/**
 * Each Tx queue uses a circular buffer of 256 TFDs stored in host DRAM.
 * Both driver and device share these circular buffers, each of which must be
 * contiguous 256 TFDs.
 * For pre 22000 HW it is 256 x 128 bytes-per-TFD = 32 KBytes
 * For 22000 HW and on it is 256 x 256 bytes-per-TFD = 65 KBytes
 *
 * Each TFD contains pointer/size information for up to 25 data buffers
 * in host DRAM.  These buffers collectively contain the (one) frame described
 * by the TFD.  Each buffer must be a single contiguous block of memory within
 * itself, but buffers may be scattered in host DRAM.  Each buffer has max size
 * of (4K - 4).  The concatenates all of a TFD's buffers into a single
 * Tx frame, up to 8 KBytes in size.
 *
 * A maximum of 255 (not 256!) TFDs may be on a queue waiting for Tx.
 */

/**
 * struct iwx_tfh_tfd - Transmit Frame Descriptor (TFD)
 * @ num_tbs 0-4 number of active tbs
 *	     5 -15   reserved
 * @ tbs[25]	transmit frame buffer descriptors
 * @ __pad	padding
 */
struct iwx_tfh_tfd {
	uint16_t num_tbs;
	struct iwx_tfh_tb tbs[IWX_TFH_NUM_TBS];
	uint32_t __pad;
} __packed;


#define IWX_ALIVE_STATUS_OK 0xCAFE

struct iwx_lmac_debug_addrs {
	uint32_t error_event_table_ptr;	/* SRAM address for error log */
	uint32_t log_event_table_ptr;	/* SRAM address for LMAC event log */
	uint32_t cpu_register_ptr;
	uint32_t dbgm_config_ptr;
	uint32_t alive_counter_ptr;
	uint32_t scd_base_ptr;		/* SRAM address for SCD */
	uint32_t st_fwrd_addr;		/* pointer to Store and forward */
	uint32_t st_fwrd_size;
} __packed; /* UCODE_DEBUG_ADDRS_API_S_VER_2 */

struct iwx_lmac_alive {
	uint32_t ucode_major;
	uint32_t ucode_minor;
	uint8_t ver_subtype;
	uint8_t ver_type;
	uint8_t mac;
	uint8_t opt;
	uint32_t timestamp;
	struct iwx_lmac_debug_addrs dbg_ptrs;
} __packed; /* UCODE_ALIVE_NTFY_API_S_VER_3 */

struct iwx_umac_debug_addrs {
	uint32_t error_info_addr;		/* SRAM address for UMAC error log */
	uint32_t dbg_print_buff_addr;
} __packed; /* UMAC_DEBUG_ADDRS_API_S_VER_1 */

struct iwx_umac_alive {
	uint32_t umac_major;		/* UMAC version: major */
	uint32_t umac_minor;		/* UMAC version: minor */
	struct iwx_umac_debug_addrs dbg_ptrs;
} __packed; /* UMAC_ALIVE_DATA_API_S_VER_2 */

struct iwx_alive_resp_v4 {
	uint16_t status;
	uint16_t flags;
	struct iwx_lmac_alive lmac_data[2];
	struct iwx_umac_alive umac_data;
} __packed; /* ALIVE_RES_API_S_VER_4 */

struct iwx_sku_id {
	uint32_t data[3];
} __packed; /* SKU_ID_API_S_VER_1 */

struct iwx_alive_resp_v5 {
	uint16_t status;
	uint16_t flags;
	struct iwx_lmac_alive lmac_data[2];
	struct iwx_umac_alive umac_data;
	struct iwx_sku_id sku_id;
} __packed; /* UCODE_ALIVE_NTFY_API_S_VER_5 */

struct iwx_imr_alive_info {
	uint64_t base_addr;
	uint32_t size;
	uint32_t enabled;
} __packed; /* IMR_ALIVE_INFO_API_S_VER_1 */

struct iwx_alive_resp_v6 {
	uint16_t status;
	uint16_t flags;
	struct iwx_lmac_alive lmac_data[2];
	struct iwx_umac_alive umac_data;
	struct iwx_sku_id sku_id;
	struct iwx_imr_alive_info imr;
} __packed; /* UCODE_ALIVE_NTFY_API_S_VER_6 / VER_7 */



struct iwx_cmd_header {
	uint8_t code;
	uint8_t flags;
	uint8_t idx;
	uint8_t qid;
} __packed;

struct iwx_cmd_header_wide {
	uint8_t opcode;
	uint8_t group_id;
	uint8_t idx;
	uint8_t qid;
	uint16_t length;
	uint8_t reserved;
	uint8_t version;
} __packed;

#define IWX_POWER_SCHEME_CAM	1
#define IWX_POWER_SCHEME_BPS	2
#define IWX_POWER_SCHEME_LP	3

#define IWX_DEF_CMD_PAYLOAD_SIZE 320
#define IWX_MAX_CMD_PAYLOAD_SIZE (4096 - sizeof(struct iwx_cmd_header_wide))
#define IWX_CMD_FAILED_MSK 0x40

/**
 * struct iwx_device_cmd
 *
 * For allocation of the command and tx queues, this establishes the overall
 * size of the largest command we send to uCode, except for commands that
 * aren't fully copied and use other TFD space.
 */
struct iwx_device_cmd {
	union {
		struct {
			struct iwx_cmd_header hdr;
			uint8_t data[IWX_DEF_CMD_PAYLOAD_SIZE];
		};
		struct {
			struct iwx_cmd_header_wide hdr_wide;
			uint8_t data_wide[IWX_DEF_CMD_PAYLOAD_SIZE -
					sizeof(struct iwx_cmd_header_wide) +
					sizeof(struct iwx_cmd_header)];
		};
	};
} __packed;

struct iwx_rx_packet {
	/*
	 * The first 4 bytes of the RX frame header contain both the RX frame
	 * size and some flags.
	 * Bit fields:
	 * 31:    flag flush RB request
	 * 30:    flag ignore TC (terminal counter) request
	 * 29:    flag fast IRQ request
	 * 28-26: Reserved
	 * 25:    Offload enabled
	 * 24:    RPF enabled
	 * 23:    RSS enabled
	 * 22:    Checksum enabled
	 * 21-16: RX queue
	 * 15-14: Reserved
	 * 13-00: RX frame size
	 */
	uint32_t len_n_flags;
	struct iwx_cmd_header hdr;
	uint8_t data[];
} __packed;


#undef __packed
#endif
