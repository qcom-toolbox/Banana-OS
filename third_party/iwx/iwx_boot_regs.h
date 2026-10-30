/* SPDX-License-Identifier: BSD-3-Clause */
/* Bootstrap layouts and register definitions from pinned OpenBSD iwx.
 * The original copyright and dual-license notice follow. */
#ifndef BANANA_IWX_BOOT_REGS_H
#define BANANA_IWX_BOOT_REGS_H
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


/* maximum number of DRAM map entries supported by FW */
#define IWX_MAX_DRAM_ENTRY	64
#define IWX_CSR_CTXT_INFO_BA	0x40

/**
 * enum iwx_context_info_flags - Context information control flags
 * @IWX_CTXT_INFO_AUTO_FUNC_INIT: If set, FW will not wait before interrupting
 *	the init done for driver command that configures several system modes
 * @IWX_CTXT_INFO_EARLY_DEBUG: enable early debug
 * @IWX_CTXT_INFO_ENABLE_CDMP: enable core dump
 * @IWX_CTXT_INFO_RB_CB_SIZE_POS: position of the RBD Cyclic Buffer Size
 *	exponent, the actual size is 2**value, valid sizes are 8-2048.
 *	The value is four bits long. Maximum valid exponent is 12
 * @IWX_CTXT_INFO_TFD_FORMAT_LONG: use long TFD Format (the
 *	default is short format - not supported by the driver)
 * @IWX_CTXT_INFO_RB_SIZE_POS: RB size position
 *	(values are IWX_CTXT_INFO_RB_SIZE_*K)
 * @IWX_CTXT_INFO_RB_SIZE_1K: Value for 1K RB size
 * @IWX_CTXT_INFO_RB_SIZE_2K: Value for 2K RB size
 * @IWX_CTXT_INFO_RB_SIZE_4K: Value for 4K RB size
 * @IWX_CTXT_INFO_RB_SIZE_8K: Value for 8K RB size
 * @IWX_CTXT_INFO_RB_SIZE_12K: Value for 12K RB size
 * @IWX_CTXT_INFO_RB_SIZE_16K: Value for 16K RB size
 * @IWX_CTXT_INFO_RB_SIZE_20K: Value for 20K RB size
 * @IWX_CTXT_INFO_RB_SIZE_24K: Value for 24K RB size
 * @IWX_CTXT_INFO_RB_SIZE_28K: Value for 28K RB size
 * @IWX_CTXT_INFO_RB_SIZE_32K: Value for 32K RB size
 */
enum iwx_context_info_flags {
	IWX_CTXT_INFO_AUTO_FUNC_INIT	= (1 << 0),
	IWX_CTXT_INFO_EARLY_DEBUG	= (1 << 1),
	IWX_CTXT_INFO_ENABLE_CDMP	= (1 << 2),
	IWX_CTXT_INFO_RB_CB_SIZE_POS	= 4,
	IWX_CTXT_INFO_TFD_FORMAT_LONG	= (1 << 8),
	IWX_CTXT_INFO_RB_SIZE_POS	= 9,
	IWX_CTXT_INFO_RB_SIZE_1K	= 0x1,
	IWX_CTXT_INFO_RB_SIZE_2K	= 0x2,
	IWX_CTXT_INFO_RB_SIZE_4K	= 0x4,
	IWX_CTXT_INFO_RB_SIZE_8K	= 0x8,
	IWX_CTXT_INFO_RB_SIZE_12K	= 0x9,
	IWX_CTXT_INFO_RB_SIZE_16K	= 0xa,
	IWX_CTXT_INFO_RB_SIZE_20K	= 0xb,
	IWX_CTXT_INFO_RB_SIZE_24K	= 0xc,
	IWX_CTXT_INFO_RB_SIZE_28K	= 0xd,
	IWX_CTXT_INFO_RB_SIZE_32K	= 0xe,
};

/*
 * struct iwx_context_info_version - version structure
 * @mac_id: SKU and revision id
 * @version: context information version id
 * @size: the size of the context information in DWs
 */
struct iwx_context_info_version {
	uint16_t mac_id;
	uint16_t version;
	uint16_t size;
	uint16_t reserved;
} __packed;

/*
 * struct iwx_context_info_control - version structure
 * @control_flags: context information flags see &enum iwx_context_info_flags
 */
struct iwx_context_info_control {
	uint32_t control_flags;
	uint32_t reserved;
} __packed;

/*
 * struct iwx_context_info_dram - images DRAM map
 * each entry in the map represents a DRAM chunk of up to 32 KB
 * @umac_img: UMAC image DRAM map
 * @lmac_img: LMAC image DRAM map
 * @virtual_img: paged image DRAM map
 */
struct iwx_context_info_dram {
	uint64_t umac_img[IWX_MAX_DRAM_ENTRY];
	uint64_t lmac_img[IWX_MAX_DRAM_ENTRY];
	uint64_t virtual_img[IWX_MAX_DRAM_ENTRY];
} __packed;

struct iwx_pnvm_info_dram {
	uint64_t pnvm_img[IWX_MAX_DRAM_ENTRY];
} __packed;

/*
 * struct iwx_context_info_rbd_cfg - RBDs configuration
 * @free_rbd_addr: default queue free RB CB base address
 * @used_rbd_addr: default queue used RB CB base address
 * @status_wr_ptr: default queue used RB status write pointer
 */
struct iwx_context_info_rbd_cfg {
	uint64_t free_rbd_addr;
	uint64_t used_rbd_addr;
	uint64_t status_wr_ptr;
} __packed;

/*
 * struct iwx_context_info_hcmd_cfg  - command queue configuration
 * @cmd_queue_addr: address of command queue
 * @cmd_queue_size: number of entries
 */
struct iwx_context_info_hcmd_cfg {
	uint64_t cmd_queue_addr;
	uint8_t cmd_queue_size;
	uint8_t reserved[7];
} __packed;

/*
 * struct iwx_context_info_dump_cfg - Core Dump configuration
 * @core_dump_addr: core dump (debug DRAM address) start address
 * @core_dump_size: size, in DWs
 */
struct iwx_context_info_dump_cfg {
	uint64_t core_dump_addr;
	uint32_t core_dump_size;
	uint32_t reserved;
} __packed;

/*
 * struct iwx_context_info_pnvm_cfg - platform NVM data configuration
 * @platform_nvm_addr: Platform NVM data start address
 * @platform_nvm_size: size in DWs
 */
struct iwx_context_info_pnvm_cfg {
	uint64_t platform_nvm_addr;
	uint32_t platform_nvm_size;
	uint32_t reserved;
} __packed;

/*
 * struct iwx_context_info_early_dbg_cfg - early debug configuration for
 *	dumping DRAM addresses
 * @early_debug_addr: early debug start address
 * @early_debug_size: size in DWs
 */
struct iwx_context_info_early_dbg_cfg {
	uint64_t early_debug_addr;
	uint32_t early_debug_size;
	uint32_t reserved;
} __packed;

/*
 * struct iwx_context_info - device INIT configuration
 * @version: version information of context info and HW
 * @control: control flags of FH configurations
 * @rbd_cfg: default RX queue configuration
 * @hcmd_cfg: command queue configuration
 * @dump_cfg: core dump data
 * @edbg_cfg: early debug configuration
 * @pnvm_cfg: platform nvm configuration
 * @dram: firmware image addresses in DRAM
 */
struct iwx_context_info {
	struct iwx_context_info_version version;
	struct iwx_context_info_control control;
	uint64_t reserved0;
	struct iwx_context_info_rbd_cfg rbd_cfg;
	struct iwx_context_info_hcmd_cfg hcmd_cfg;
	uint32_t reserved1[4];
	struct iwx_context_info_dump_cfg dump_cfg;
	struct iwx_context_info_early_dbg_cfg edbg_cfg;
	struct iwx_context_info_pnvm_cfg pnvm_cfg;
	uint32_t reserved2[16];
	struct iwx_context_info_dram dram;
	uint32_t reserved3[16];
} __packed;


/*
 * Context info definitions for AX210 devices.
 */

#define IWX_CSR_CTXT_INFO_BOOT_CTRL         0x0
#define IWX_CSR_CTXT_INFO_ADDR              0x118
#define IWX_CSR_IML_DATA_ADDR               0x120
#define IWX_CSR_IML_SIZE_ADDR               0x128
#define IWX_CSR_IML_RESP_ADDR               0x12c

/* Set bit for enabling automatic function boot */
#define IWX_CSR_AUTO_FUNC_BOOT_ENA          (1 << 1)
/* Set bit for initiating function boot */
#define IWX_CSR_AUTO_FUNC_INIT              (1 << 7)

/**
 * iwx_prph_scratch_mtr_format - tfd size configuration
 * @IWX_PRPH_MTR_FORMAT_16B: 16 bit tfd
 * @IWX_PRPH_MTR_FORMAT_32B: 32 bit tfd
 * @IWX_PRPH_MTR_FORMAT_64B: 64 bit tfd
 * @IWX_PRPH_MTR_FORMAT_256B: 256 bit tfd
 */
#define IWX_PRPH_MTR_FORMAT_16B		0x0
#define IWX_PRPH_MTR_FORMAT_32B		0x40000
#define IWX_PRPH_MTR_FORMAT_64B		0x80000
#define IWX_PRPH_MTR_FORMAT_256B	0xC0000

/**
 * iwx_prph_scratch_flags - PRPH scratch control flags
 * @IWX_PRPH_SCRATCH_IMR_DEBUG_EN: IMR support for debug
 * @IWX_PRPH_SCRATCH_EARLY_DEBUG_EN: enable early debug conf
 * @IWX_PRPH_SCRATCH_EDBG_DEST_DRAM: use DRAM, with size allocated
 *	in hwm config.
 * @IWX_PRPH_SCRATCH_EDBG_DEST_INTERNAL: use buffer on SRAM
 * @IWX_PRPH_SCRATCH_EDBG_DEST_ST_ARBITER: use st arbiter, mainly for
 *	multicomm.
 * @IWX_PRPH_SCRATCH_EDBG_DEST_TB22DTF: route debug data to SoC HW
 * @IWX_PRPH_SCTATCH_RB_SIZE_4K: Use 4K RB size (the default is 2K)
 * @IWX_PRPH_SCRATCH_MTR_MODE: format used for completion - 0: for
 *	completion descriptor, 1 for responses (legacy)
 * @IWX_PRPH_SCRATCH_MTR_FORMAT: a mask for the size of the tfd.
 *	There are 4 optional values: 0: 16 bit, 1: 32 bit, 2: 64 bit,
 *	3: 256 bit.
 * @IWX_PRPH_SCRATCH_RB_SIZE_EXT_MASK: RB size full information, ignored
 *	by older firmware versions, so set IWX_PRPH_SCRATCH_RB_SIZE_4K
 *	appropriately; use the below values for this.
 * @IWX_PRPH_SCRATCH_RB_SIZE_EXT_8K: 8kB RB size
 * @IWX_PRPH_SCRATCH_RB_SIZE_EXT_12K: 12kB RB size
 * @IWX_PRPH_SCRATCH_RB_SIZE_EXT_16K: 16kB RB size
 */
#define IWX_PRPH_SCRATCH_IMR_DEBUG_EN		(1 << 1)
#define IWX_PRPH_SCRATCH_EARLY_DEBUG_EN		(1 << 4)
#define IWX_PRPH_SCRATCH_EDBG_DEST_DRAM		(1 << 8)
#define IWX_PRPH_SCRATCH_EDBG_DEST_INTERNAL	(1 << 9)
#define IWX_PRPH_SCRATCH_EDBG_DEST_ST_ARBITER	(1 << 10)
#define IWX_PRPH_SCRATCH_EDBG_DEST_TB22DTF	(1 << 11)
#define IWX_PRPH_SCRATCH_RB_SIZE_4K		(1 << 16)
#define IWX_PRPH_SCRATCH_MTR_MODE		(1 << 17)
#define IWX_PRPH_SCRATCH_MTR_FORMAT		((1 << 18) | (1 << 19))
#define IWX_PRPH_SCRATCH_RB_SIZE_EXT_MASK	(0xf << 20)
#define IWX_PRPH_SCRATCH_RB_SIZE_EXT_8K		(8 << 20)
#define IWX_PRPH_SCRATCH_RB_SIZE_EXT_12K	(9 << 20)
#define IWX_PRPH_SCRATCH_RB_SIZE_EXT_16K	(10 << 20)

/*
 * struct iwx_prph_scratch_version - version structure
 * @mac_id: SKU and revision id
 * @version: prph scratch information version id
 * @size: the size of the context information in DWs
 * @reserved: reserved
 */
struct iwx_prph_scratch_version {
	uint16_t mac_id;
	uint16_t version;
	uint16_t size;
	uint16_t reserved;
} __packed; /* PERIPH_SCRATCH_VERSION_S */

/*
 * struct iwx_prph_scratch_control - control structure
 * @control_flags: context information flags see &iwx_prph_scratch_flags
 * @reserved: reserved
 */
struct iwx_prph_scratch_control {
	uint32_t control_flags;
	uint32_t reserved;
} __packed; /* PERIPH_SCRATCH_CONTROL_S */

/*
 * struct iwx_prph_scratch_pnvm_cfg - ror config
 * @pnvm_base_addr: PNVM start address
 * @pnvm_size: PNVM size in DWs
 * @reserved: reserved
 */
struct iwx_prph_scratch_pnvm_cfg {
	uint64_t pnvm_base_addr;
	uint32_t pnvm_size;
	uint32_t reserved;
} __packed; /* PERIPH_SCRATCH_PNVM_CFG_S */

struct iwx_pnvm_section {
	uint32_t offset;
	const uint8_t data[];
} __packed;

/*
 * struct iwx_prph_scratch_hwm_cfg - hwm config
 * @hwm_base_addr: hwm start address
 * @hwm_size: hwm size in DWs
 * @debug_token_config: debug preset
 */
struct iwx_prph_scratch_hwm_cfg {
	uint64_t hwm_base_addr;
	uint32_t hwm_size;
	uint32_t debug_token_config;
} __packed; /* PERIPH_SCRATCH_HWM_CFG_S */

/*
 * struct iwx_prph_scratch_rbd_cfg - RBDs configuration
 * @free_rbd_addr: default queue free RB CB base address
 * @reserved: reserved
 */
struct iwx_prph_scratch_rbd_cfg {
	uint64_t free_rbd_addr;
	uint32_t reserved;
} __packed; /* PERIPH_SCRATCH_RBD_CFG_S */

/*
 * struct iwx_prph_scratch_uefi_cfg - prph scratch reduce power table
 * @base_addr: reduce power table address
 * @size: table size in dwords
 */
struct iwx_prph_scratch_uefi_cfg {
	uint64_t base_addr;
	uint32_t size;
	uint32_t reserved;
} __packed; /* PERIPH_SCRATCH_UEFI_CFG_S */

/*
 * struct iwx_prph_scratch_ctrl_cfg - prph scratch ctrl and config
 * @version: version information of context info and HW
 * @control: control flags of FH configurations
 * @pnvm_cfg: ror configuration
 * @hwm_cfg: hwm configuration
 * @rbd_cfg: default RX queue configuration
 */
struct iwx_prph_scratch_ctrl_cfg {
	struct iwx_prph_scratch_version version;
	struct iwx_prph_scratch_control control;
	struct iwx_prph_scratch_pnvm_cfg pnvm_cfg;
	struct iwx_prph_scratch_hwm_cfg hwm_cfg;
	struct iwx_prph_scratch_rbd_cfg rbd_cfg;
	struct iwx_prph_scratch_uefi_cfg reduce_power_cfg;
} __packed; /* PERIPH_SCRATCH_CTRL_CFG_S */

/*
 * struct iwx_prph_scratch - peripheral scratch mapping
 * @ctrl_cfg: control and configuration of prph scratch
 * @dram: firmware images addresses in DRAM
 * @reserved: reserved
 */
struct iwx_prph_scratch {
	struct iwx_prph_scratch_ctrl_cfg ctrl_cfg;
	uint32_t reserved[12];
	struct iwx_context_info_dram dram;
} __packed; /* PERIPH_SCRATCH_S */

/*
 * struct iwx_prph_info - peripheral information
 * @boot_stage_mirror: reflects the value in the Boot Stage CSR register
 * @ipc_status_mirror: reflects the value in the IPC Status CSR register
 * @sleep_notif: indicates the peripheral sleep status
 * @reserved: reserved
 */
struct iwx_prph_info {
	uint32_t boot_stage_mirror;
	uint32_t ipc_status_mirror;
	uint32_t sleep_notif;
	uint32_t reserved;
} __packed; /* PERIPH_INFO_S */

/*
 * struct iwx_context_info_gen3 - device INIT configuration
 * @version: version of the context information
 * @size: size of context information in DWs
 * @config: context in which the peripheral would execute - a subset of
 *	capability csr register published by the peripheral
 * @prph_info_base_addr: the peripheral information structure start address
 * @cr_head_idx_arr_base_addr: the completion ring head index array
 *	start address
 * @tr_tail_idx_arr_base_addr: the transfer ring tail index array
 *	start address
 * @cr_tail_idx_arr_base_addr: the completion ring tail index array
 *	start address
 * @tr_head_idx_arr_base_addr: the transfer ring head index array
 *	start address
 * @cr_idx_arr_size: number of entries in the completion ring index array
 * @tr_idx_arr_size: number of entries in the transfer ring index array
 * @mtr_base_addr: the message transfer ring start address
 * @mcr_base_addr: the message completion ring start address
 * @mtr_size: number of entries which the message transfer ring can hold
 * @mcr_size: number of entries which the message completion ring can hold
 * @mtr_doorbell_vec: the doorbell vector associated with the message
 *	transfer ring
 * @mcr_doorbell_vec: the doorbell vector associated with the message
 *	completion ring
 * @mtr_msi_vec: the MSI which shall be generated by the peripheral after
 *	completing a transfer descriptor in the message transfer ring
 * @mcr_msi_vec: the MSI which shall be generated by the peripheral after
 *	completing a completion descriptor in the message completion ring
 * @mtr_opt_header_size: the size of the optional header in the transfer
 *	descriptor associated with the message transfer ring in DWs
 * @mtr_opt_footer_size: the size of the optional footer in the transfer
 *	descriptor associated with the message transfer ring in DWs
 * @mcr_opt_header_size: the size of the optional header in the completion
 *	descriptor associated with the message completion ring in DWs
 * @mcr_opt_footer_size: the size of the optional footer in the completion
 *	descriptor associated with the message completion ring in DWs
 * @msg_rings_ctrl_flags: message rings control flags
 * @prph_info_msi_vec: the MSI which shall be generated by the peripheral
 *	after updating the Peripheral Information structure
 * @prph_scratch_base_addr: the peripheral scratch structure start address
 * @prph_scratch_size: the size of the peripheral scratch structure in DWs
 * @reserved: reserved
 */
struct iwx_context_info_gen3 {
	uint16_t version;
	uint16_t size;
	uint32_t config;
	uint64_t prph_info_base_addr;
	uint64_t cr_head_idx_arr_base_addr;
	uint64_t tr_tail_idx_arr_base_addr;
	uint64_t cr_tail_idx_arr_base_addr;
	uint64_t tr_head_idx_arr_base_addr;
	uint16_t cr_idx_arr_size;
	uint16_t tr_idx_arr_size;
	uint64_t mtr_base_addr;
	uint64_t mcr_base_addr;
	uint16_t mtr_size;
	uint16_t mcr_size;
	uint16_t mtr_doorbell_vec;
	uint16_t mcr_doorbell_vec;
	uint16_t mtr_msi_vec;
	uint16_t mcr_msi_vec;
	uint8_t mtr_opt_header_size;
	uint8_t mtr_opt_footer_size;
	uint8_t mcr_opt_header_size;
	uint8_t mcr_opt_footer_size;
	uint16_t msg_rings_ctrl_flags;
	uint16_t prph_info_msi_vec;
	uint64_t prph_scratch_base_addr;
	uint32_t prph_scratch_size;
	uint32_t reserved;
} __packed; /* IPC_CONTEXT_INFO_S */

#define IWX_MGMT_TID		15

#define IWX_MQ_RX_TABLE_SIZE	512

/* cb size is the exponent */
#define IWX_RX_QUEUE_CB_SIZE(x)	((sizeof(x) <= 4) ? (fls(x) - 1) : (flsl(x) - 1))

/*
 * CSR (control and status registers)
 *
 * CSR registers are mapped directly into PCI bus space, and are accessible
 * whenever platform supplies power to device, even when device is in
 * low power states due to driver-invoked device resets
 * (e.g. IWX_CSR_RESET_REG_FLAG_SW_RESET) or uCode-driven power-saving modes.
 *
 * Use iwl_write32() and iwl_read32() family to access these registers;
 * these provide simple PCI bus access, without waking up the MAC.
 * Do not use iwl_write_direct32() family for these registers;
 * no need to "grab nic access" via IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ.
 * The MAC (uCode processor, etc.) does not need to be powered up for accessing
 * the CSR registers.
 *
 * NOTE:  Device does need to be awake in order to read this memory
 *        via IWX_CSR_EEPROM and IWX_CSR_OTP registers
 */
#define IWX_CSR_HW_IF_CONFIG_REG    (0x000) /* hardware interface config */
#define IWX_CSR_INT_COALESCING      (0x004) /* accum ints, 32-usec units */
#define IWX_CSR_INT                 (0x008) /* host interrupt status/ack */
#define IWX_CSR_INT_MASK            (0x00c) /* host interrupt enable */
#define IWX_CSR_FH_INT_STATUS       (0x010) /* busmaster int status/ack*/
#define IWX_CSR_GPIO_IN             (0x018) /* read external chip pins */
#define IWX_CSR_RESET               (0x020) /* busmaster enable, NMI, etc*/
#define IWX_CSR_GP_CNTRL            (0x024)
#define IWX_CSR_FUNC_SCRATCH        (0x02c) /* Scratch register - used for FW dbg */

/* 2nd byte of IWX_CSR_INT_COALESCING, not accessible via iwl_write32()! */
#define IWX_CSR_INT_PERIODIC_REG	(0x005)

/*
 * Hardware revision info
 * Bit fields:
 * 31-16:  Reserved
 *  15-4:  Type of device:  see IWX_CSR_HW_REV_TYPE_xxx definitions
 *  3-2:  Revision step:  0 = A, 1 = B, 2 = C, 3 = D
 *  1-0:  "Dash" (-) value, as in A-1, etc.
 */
#define IWX_CSR_HW_REV              (0x028)

/*
 * RF ID revision info
 * Bit fields:
 * 31:24: Reserved (set to 0x0)
 * 23:12: Type
 * 11:8:  Step (A - 0x0, B - 0x1, etc)
 * 7:4:   Dash
 * 3:0:   Flavor
 */
#define IWX_CSR_HW_RF_ID		(0x09c)


#define IWX_CSR_GIO_REG		(0x03C)

/*
 * UCODE-DRIVER GP (general purpose) mailbox registers.
 * SET/CLR registers set/clear bit(s) if "1" is written.
 */
#define IWX_CSR_UCODE_DRV_GP1       (0x054)
#define IWX_CSR_UCODE_DRV_GP1_SET   (0x058)
#define IWX_CSR_UCODE_DRV_GP1_CLR   (0x05c)
#define IWX_CSR_UCODE_DRV_GP2       (0x060)

#define IWX_CSR_MBOX_SET_REG		(0x088)
#define IWX_CSR_MBOX_SET_REG_OS_ALIVE	0x20

#define IWX_CSR_DRAM_INT_TBL_REG	(0x0A0)
#define IWX_CSR_MAC_SHADOW_REG_CTRL	(0x0A8) /* 6000 and up */

/* LTR control */
#define IWX_CSR_LTR_LONG_VAL_AD				(0x0d4)
#define IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ		0x80000000
#define IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_MASK	0x1c000000
#define IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_SHIFT	24
#define IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_MASK	0x03ff0000
#define IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_SHIFT	16
#define IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ		0x00008000
#define IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_MASK	0x00001c00
#define IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_SHIFT	8
#define IWX_CSR_LTR_LONG_VAL_AD_SNOOP_VAL		0x000003ff
#define IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC		2

/* GIO Chicken Bits (PCI Express bus link power management) */
#define IWX_CSR_GIO_CHICKEN_BITS    (0x100)

/* Doorbell NMI since Bz. Connected to UREG_DOORBELL_TO_ISR6 (lower 16 bits only). */
#define IWX_CSR_DOORBELL_VECTOR		(0x130)

#define IWX_CSR_DBG_HPET_MEM_REG	(0x240)
#define IWX_CSR_DBG_LINK_PWR_MGMT_REG	(0x250)

/*
 * Scratch register initial configuration - this is set on init, and read
 * during a error FW error.
 */
#define IWX_CSR_FUNC_SCRATCH_INIT_VALUE		(0x01010101)
#define IWX_CSR_FUNC_SCRATCH_POWER_OFF_MASK	0xFFFF

/* Bits for IWX_CSR_HW_IF_CONFIG_REG */
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH	(0x00000003)
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP	(0x0000000C)
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_BOARD_VER	(0x000000C0)
#define IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI	(0x00000100)
#define IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI	(0x00000200)
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE	(0x00000C00)
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH	(0x00003000)
#define IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP	(0x0000C000)

#define IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH	(0)
#define IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP	(2)
#define IWX_CSR_HW_IF_CONFIG_REG_POS_BOARD_VER	(6)
#define IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE	(10)
#define IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH	(12)
#define IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP	(14)

#define IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A	(0x00080000)
#define IWX_CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM	(0x00200000)
#define IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY	(0x00400000) /* PCI_OWN_SEM */
#define IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE (0x02000000) /* ME_OWN */
#define IWX_CSR_HW_IF_CONFIG_REG_PREPARE	(0x08000000) /* WAKE_ME */
#define IWX_CSR_HW_IF_CONFIG_REG_ENABLE_PME	(0x10000000)
#define IWX_CSR_HW_IF_CONFIG_REG_PERSIST_MODE	(0x40000000) /* PERSISTENCE */

#define IWX_CSR_INT_PERIODIC_DIS		(0x00) /* disable periodic int*/
#define IWX_CSR_INT_PERIODIC_ENA		(0xFF) /* 255*32 usec ~ 8 msec*/

/* interrupt flags in INTA, set by uCode or hardware (e.g. dma),
 * acknowledged (reset) by host writing "1" to flagged bits. */
#define IWX_CSR_INT_BIT_FH_RX	(1U << 31) /* Rx DMA, cmd responses, FH_INT[17:16] */
#define IWX_CSR_INT_BIT_HW_ERR	(1 << 29) /* DMA hardware error FH_INT[31] */
#define IWX_CSR_INT_BIT_RX_PERIODIC	(1 << 28) /* Rx periodic */
#define IWX_CSR_INT_BIT_FH_TX	(1 << 27) /* Tx DMA FH_INT[1:0] */
#define IWX_CSR_INT_BIT_SCD	(1 << 26) /* TXQ pointer advanced */
#define IWX_CSR_INT_BIT_SW_ERR	(1 << 25) /* uCode error */
#define IWX_CSR_INT_BIT_RF_KILL	(1 << 7)  /* HW RFKILL switch GP_CNTRL[27] toggled */
#define IWX_CSR_INT_BIT_CT_KILL	(1 << 6)  /* Critical temp (chip too hot) rfkill */
#define IWX_CSR_INT_BIT_SW_RX	(1 << 3)  /* Rx, command responses */
#define IWX_CSR_INT_BIT_WAKEUP	(1 << 1)  /* NIC controller waking up (pwr mgmt) */
#define IWX_CSR_INT_BIT_ALIVE	(1 << 0)  /* uCode interrupts once it initializes */

#define IWX_CSR_INI_SET_MASK	(IWX_CSR_INT_BIT_FH_RX   | \
				 IWX_CSR_INT_BIT_HW_ERR  | \
				 IWX_CSR_INT_BIT_FH_TX   | \
				 IWX_CSR_INT_BIT_SW_ERR  | \
				 IWX_CSR_INT_BIT_RF_KILL | \
				 IWX_CSR_INT_BIT_SW_RX   | \
				 IWX_CSR_INT_BIT_WAKEUP  | \
				 IWX_CSR_INT_BIT_ALIVE   | \
				 IWX_CSR_INT_BIT_RX_PERIODIC)

/* interrupt flags in FH (flow handler) (PCI busmaster DMA) */
#define IWX_CSR_FH_INT_BIT_ERR       (1U << 31) /* Error */
#define IWX_CSR_FH_INT_BIT_HI_PRIOR  (1 << 30) /* High priority Rx, bypass coalescing */
#define IWX_CSR_FH_INT_BIT_RX_CHNL1  (1 << 17) /* Rx channel 1 */
#define IWX_CSR_FH_INT_BIT_RX_CHNL0  (1 << 16) /* Rx channel 0 */
#define IWX_CSR_FH_INT_BIT_TX_CHNL1  (1 << 1)  /* Tx channel 1 */
#define IWX_CSR_FH_INT_BIT_TX_CHNL0  (1 << 0)  /* Tx channel 0 */

#define IWX_CSR_FH_INT_RX_MASK	(IWX_CSR_FH_INT_BIT_HI_PRIOR | \
				IWX_CSR_FH_INT_BIT_RX_CHNL1 | \
				IWX_CSR_FH_INT_BIT_RX_CHNL0)

#define IWX_CSR_FH_INT_TX_MASK	(IWX_CSR_FH_INT_BIT_TX_CHNL1 | \
				IWX_CSR_FH_INT_BIT_TX_CHNL0)

/**
 * struct iwx_rx_transfer_desc - transfer descriptor AX210
 * @addr: ptr to free buffer start address
 * @rbid: unique tag of the buffer
 * @reserved: reserved
 */
struct iwx_rx_transfer_desc {
	uint16_t rbid;
	uint16_t reserved[3];
	uint64_t addr;
};

#define IWX_RX_CD_FLAGS_FRAGMENTED	(1 << 0)

/**
 * struct iwx_rx_completion_desc - completion descriptor AX210
 * @reserved1: reserved
 * @rbid: unique tag of the received buffer
 * @flags: flags (0: fragmented, all others: reserved)
 * @reserved2: reserved
 */
struct iwx_rx_completion_desc {
	uint32_t reserved1;
	uint16_t rbid;
	uint8_t flags;
	uint8_t reserved2[25];
};

/**
 * struct iwx_rx_completion_desc_bz - Bz completion descriptor
 * @rbid: unique tag of the received buffer
 * @flags: flags (0: fragmented, all others: reserved)
 * @reserved: reserved
 */
struct iwx_rx_completion_desc_bz {
	uint16_t rbid;
	uint8_t flags;
	uint8_t reserved[1];
} __packed;

/* RESET */
#define IWX_CSR_RESET_REG_FLAG_NEVO_RESET                (0x00000001)
#define IWX_CSR_RESET_REG_FLAG_FORCE_NMI                 (0x00000002)
#define IWX_CSR_RESET_REG_FLAG_SW_RESET                  (0x00000080)
#define IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED           (0x00000100)
#define IWX_CSR_RESET_REG_FLAG_STOP_MASTER               (0x00000200)
#define IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED             (0x80000000)

/*
 * GP (general purpose) CONTROL REGISTER
 * Bit fields:
 *    27:  HW_RF_KILL_SW
 *         Indicates state of (platform's) hardware RF-Kill switch
 * 26-24:  POWER_SAVE_TYPE
 *         Indicates current power-saving mode:
 *         000 -- No power saving
 *         001 -- MAC power-down
 *         010 -- PHY (radio) power-down
 *         011 -- Error
 *   9-6:  SYS_CONFIG
 *         Indicates current system configuration, reflecting pins on chip
 *         as forced high/low by device circuit board.
 *     4:  GOING_TO_SLEEP
 *         Indicates MAC is entering a power-saving sleep power-down.
 *         Not a good time to access device-internal resources.
 *     3:  MAC_ACCESS_REQ
 *         Host sets this to request and maintain MAC wakeup, to allow host
 *         access to device-internal resources.  Host must wait for
 *         MAC_CLOCK_READY (and !GOING_TO_SLEEP) before accessing non-CSR
 *         device registers.
 *     2:  INIT_DONE
 *         Host sets this to put device into fully operational D0 power mode.
 *         Host resets this after SW_RESET to put device into low power mode.
 *     0:  MAC_CLOCK_READY
 *         Indicates MAC (ucode processor, etc.) is powered up and can run.
 *         Internal resources are accessible.
 *         NOTE:  This does not indicate that the processor is actually running.
 *         NOTE:  This does not indicate that device has completed
 *                init or post-power-down restore of internal SRAM memory.
 *                Use IWX_CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP as indication that
 *                SRAM is restored and uCode is in normal operation mode.
 *                Later devices (5xxx/6xxx/1xxx) use non-volatile SRAM, and
 *                do not need to save/restore it.
 *         NOTE:  After device reset, this bit remains "0" until host sets
 *                INIT_DONE
 */
#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY        (0x00000001)
#define IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE              (0x00000004)
#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ         (0x00000008)
#define IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP         (0x00000010)

#define IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN           (0x00000001)

#define IWX_CSR_GP_CNTRL_REG_MSK_POWER_SAVE_TYPE         (0x07000000)
#define IWX_CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN     (0x04000000)
#define IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW          (0x08000000)

/* From Bz we use these instead during init/reset flow */
#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT			(1 << 6)
#define IWX_CSR_GP_CNTRL_REG_FLAG_ROM_START			(1 << 7)
#define IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS			(1 << 20)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ		(1 << 21)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS	(1 << 28)
#define IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ	(1 << 29)
#define IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET			(1 << 31)

/* HW REV */
#define IWX_CSR_HW_REV_DASH(_val)          (((_val) & 0x0000003) >> 0)
#define IWX_CSR_HW_REV_STEP(_val)          (((_val) & 0x000000C) >> 2)
#define IWX_CSR_HW_REV_TYPE(_val)          (((_val) & 0x000FFF0) >> 4)

#define IWX_CSR_HW_REV_TYPE_MSK		(0x000FFF0)
#define IWX_CSR_HW_REV_TYPE_QU_B0	(0x0000334)
#define IWX_CSR_HW_REV_TYPE_QU_C0	(0x0000338)
#define IWX_CSR_HW_REV_TYPE_QUZ		(0x0000354)
#define IWX_CSR_HW_REV_TYPE_SO		(0x0000370)
#define IWX_CSR_HW_REV_TYPE_TY		(0x0000420)

/* HW RFID */
#define IWX_CSR_HW_RFID_FLAVOR(_val)       (((_val) & 0x000000F) >> 0)
#define IWX_CSR_HW_RFID_DASH(_val)         (((_val) & 0x00000F0) >> 4)
#define IWX_CSR_HW_RFID_STEP(_val)         (((_val) & 0x0000F00) >> 8)
#define IWX_CSR_HW_RFID_TYPE(_val)         (((_val) & 0x0FFF000) >> 12)
#define IWX_CSR_HW_RFID_IS_CDB(_val)       (((_val) & 0x10000000) >> 28)
#define IWX_CSR_HW_RFID_IS_JACKET(_val)    (((_val) & 0x20000000) >> 29)

/* CSR GIO */
#define IWX_CSR_GIO_REG_VAL_L0S_DISABLED	(0x00000002)

/*
 * UCODE-DRIVER GP (general purpose) mailbox register 1
 * Host driver and uCode write and/or read this register to communicate with
 * each other.
 * Bit fields:
 *     4:  UCODE_DISABLE
 *         Host sets this to request permanent halt of uCode, same as
 *         sending CARD_STATE command with "halt" bit set.
 *     3:  CT_KILL_EXIT
 *         Host sets this to request exit from CT_KILL state, i.e. host thinks
 *         device temperature is low enough to continue normal operation.
 *     2:  CMD_BLOCKED
 *         Host sets this during RF KILL power-down sequence (HW, SW, CT KILL)
 *         to release uCode to clear all Tx and command queues, enter
 *         unassociated mode, and power down.
 *         NOTE:  Some devices also use HBUS_TARG_MBX_C register for this bit.
 *     1:  SW_BIT_RFKILL
 *         Host sets this when issuing CARD_STATE command to request
 *         device sleep.
 *     0:  MAC_SLEEP
 *         uCode sets this when preparing a power-saving power-down.
 *         uCode resets this when power-up is complete and SRAM is sane.
 *         NOTE:  device saves internal SRAM data to host when powering down,
 *                and must restore this data after powering back up.
 *                MAC_SLEEP is the best indication that restore is complete.
 *                Later devices (5xxx/6xxx/1xxx) use non-volatile SRAM, and
 *                do not need to save/restore it.
 */
#define IWX_CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP             (0x00000001)
#define IWX_CSR_UCODE_SW_BIT_RFKILL                     (0x00000002)
#define IWX_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED           (0x00000004)
#define IWX_CSR_UCODE_DRV_GP1_REG_BIT_CT_KILL_EXIT      (0x00000008)
#define IWX_CSR_UCODE_DRV_GP1_BIT_D3_CFG_COMPLETE       (0x00000020)

/* GIO Chicken Bits (PCI Express bus link power management) */
#define IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX  (0x00800000)
#define IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER  (0x20000000)

/* HPET MEM debug */
#define IWX_CSR_DBG_HPET_MEM_REG_VAL	(0xFFFF0000)

/* DRAM INT TABLE */
#define IWX_CSR_DRAM_INT_TBL_ENABLE		(1U << 31)
#define IWX_CSR_DRAM_INIT_TBL_WRITE_POINTER	(1 << 28)
#define IWX_CSR_DRAM_INIT_TBL_WRAP_CHECK	(1 << 27)

/* 22000 configuration registers */

/*
 * TFH Configuration register.
 *
 * BIT fields:
 *
 * Bits 3:0:
 * Define the maximum number of pending read requests.
 * Maximum configuration value allowed is 0xC
 * Bits 9:8:
 * Define the maximum transfer size. (64 / 128 / 256)
 * Bit 10:
 * When bit is set and transfer size is set to 128B, the TFH will enable
 * reading chunks of more than 64B only if the read address is aligned to 128B.
 * In case of DRAM read address which is not aligned to 128B, the TFH will
 * enable transfer size which doesn't cross 64B DRAM address boundary.
*/
#define IWX_TFH_TRANSFER_MODE			(0x1F40)
#define IWX_TFH_TRANSFER_MAX_PENDING_REQ	0xc
#define IWX_TFH_CHUNK_SIZE_128			(1 << 8)
#define IWX_TFH_CHUNK_SPLIT_MODE		(1 << 10)

/*
 * Defines the offset address in dwords referring from the beginning of the
 * Tx CMD which will be updated in DRAM.
 * Note that the TFH offset address for Tx CMD update is always referring to
 * the start of the TFD first TB.
 * In case of a DRAM Tx CMD update the TFH will update PN and Key ID
 */
#define IWX_TFH_TXCMD_UPDATE_CFG		(0x1F48)

/*
 * Controls TX DMA operation
 *
 * BIT fields:
 *
 * Bits 31:30: Enable the SRAM DMA channel.
 * Turning on bit 31 will kick the SRAM2DRAM DMA.
 * Note that the sram2dram may be enabled only after configuring the DRAM and
 * SRAM addresses registers and the byte count register.
 * Bits 25:24: Defines the interrupt target upon dram2sram transfer done. When
 * set to 1 - interrupt is sent to the driver
 * Bit 0: Indicates the snoop configuration
*/
#define IWX_TFH_SRV_DMA_CHNL0_CTRL	(0x1F60)
#define IWX_TFH_SRV_DMA_SNOOP		(1 << 0)
#define IWX_TFH_SRV_DMA_TO_DRIVER	(1 << 24)
#define IWX_TFH_SRV_DMA_START		(1U << 31)

/* Defines the DMA SRAM write start address to transfer a data block */
#define IWX_TFH_SRV_DMA_CHNL0_SRAM_ADDR	(0x1F64)

/* Defines the 64bits DRAM start address to read the DMA data block from */
#define IWX_TFH_SRV_DMA_CHNL0_DRAM_ADDR	(0x1F68)

/*
 * Defines the number of bytes to transfer from DRAM to SRAM.
 * Note that this register may be configured with non-dword aligned size.
 */
#define IWX_TFH_SRV_DMA_CHNL0_BC	(0x1F70)

/* 9000 rx series registers */

#define IWX_RFH_Q0_FRBDCB_BA_LSB 0xA08000 /* 64 bit address */
#define IWX_RFH_Q_FRBDCB_BA_LSB(q) (IWX_RFH_Q0_FRBDCB_BA_LSB + (q) * 8)
/* Write index table */
#define IWX_RFH_Q0_FRBDCB_WIDX 0xA08080
#define IWX_RFH_Q_FRBDCB_WIDX(q) (IWX_RFH_Q0_FRBDCB_WIDX + (q) * 4)
/* Write index table - shadow registers */
#define IWX_RFH_Q0_FRBDCB_WIDX_TRG 0x1C80
#define IWX_RFH_Q_FRBDCB_WIDX_TRG(q) (IWX_RFH_Q0_FRBDCB_WIDX_TRG + (q) * 4)
/* Read index table */
#define IWX_RFH_Q0_FRBDCB_RIDX 0xA080C0
#define IWX_RFH_Q_FRBDCB_RIDX(q) (IWX_RFH_Q0_FRBDCB_RIDX + (q) * 4)
/* Used list table */
#define IWX_RFH_Q0_URBDCB_BA_LSB 0xA08100 /* 64 bit address */
#define IWX_RFH_Q_URBDCB_BA_LSB(q) (IWX_RFH_Q0_URBDCB_BA_LSB + (q) * 8)
/* Write index table */
#define IWX_RFH_Q0_URBDCB_WIDX 0xA08180
#define IWX_RFH_Q_URBDCB_WIDX(q) (IWX_RFH_Q0_URBDCB_WIDX + (q) * 4)
#define IWX_RFH_Q0_URBDCB_VAID 0xA081C0
#define IWX_RFH_Q_URBDCB_VAID(q) (IWX_RFH_Q0_URBDCB_VAID + (q) * 4)
/* stts */
#define IWX_RFH_Q0_URBD_STTS_WPTR_LSB 0xA08200 /*64 bits address */
#define IWX_RFH_Q_URBD_STTS_WPTR_LSB(q) (IWX_RFH_Q0_URBD_STTS_WPTR_LSB + (q) * 8)

#define IWX_RFH_Q0_ORB_WPTR_LSB 0xA08280
#define IWX_RFH_Q_ORB_WPTR_LSB(q) (IWX_RFH_Q0_ORB_WPTR_LSB + (q) * 8)
#define IWX_RFH_RBDBUF_RBD0_LSB 0xA08300
#define IWX_RFH_RBDBUF_RBD_LSB(q) (IWX_RFH_RBDBUF_RBD0_LSB + (q) * 8)

/**
 * RFH Status Register
 *
 * Bit fields:
 *
 * Bit 29: RBD_FETCH_IDLE
 * This status flag is set by the RFH when there is no active RBD fetch from
 * DRAM.
 * Once the RFH RBD controller starts fetching (or when there is a pending
 * RBD read response from DRAM), this flag is immediately turned off.
 *
 * Bit 30: SRAM_DMA_IDLE
 * This status flag is set by the RFH when there is no active transaction from
 * SRAM to DRAM.
 * Once the SRAM to DRAM DMA is active, this flag is immediately turned off.
 *
 * Bit 31: RXF_DMA_IDLE
 * This status flag is set by the RFH when there is no active transaction from
 * RXF to DRAM.
 * Once the RXF-to-DRAM DMA is active, this flag is immediately turned off.
 */
#define IWX_RFH_GEN_STATUS          0xA09808
#define IWX_RFH_GEN_STATUS_GEN3     0xA07824
#define IWX_RBD_FETCH_IDLE  (1 << 29)
#define IWX_SRAM_DMA_IDLE   (1 << 30)
#define IWX_RXF_DMA_IDLE    (1U << 31)

/* DMA configuration */
#define IWX_RFH_RXF_DMA_CFG         0xA09820
#define IWX_RFH_RXF_DMA_CFG_GEN3    0xA07880
/* RB size */
#define IWX_RFH_RXF_DMA_RB_SIZE_MASK (0x000F0000) /* bits 16-19 */
#define IWX_RFH_RXF_DMA_RB_SIZE_POS 16
#define IWX_RFH_RXF_DMA_RB_SIZE_1K  (0x1 << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_2K  (0x2 << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_4K  (0x4 << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_8K  (0x8 << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_12K (0x9 << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_16K (0xA << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_20K (0xB << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_24K (0xC << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_28K (0xD << IWX_RFH_RXF_DMA_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RB_SIZE_32K (0xE << IWX_RFH_RXF_DMA_RB_SIZE_POS)
/* RB Circular Buffer size:defines the table sizes in RBD units */
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_MASK (0x00F00000) /* bits 20-23 */
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_POS 20
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_8        (0x3 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_16       (0x4 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_32       (0x5 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_64       (0x7 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_128      (0x7 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_256      (0x8 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_512      (0x9 << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_1024     (0xA << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_RBDCB_SIZE_2048     (0xB << IWX_RFH_RXF_DMA_RBDCB_SIZE_POS)
#define IWX_RFH_RXF_DMA_MIN_RB_SIZE_MASK    (0x03000000) /* bit 24-25 */
#define IWX_RFH_RXF_DMA_MIN_RB_SIZE_POS     24
#define IWX_RFH_RXF_DMA_MIN_RB_4_8          (3 << IWX_RFH_RXF_DMA_MIN_RB_SIZE_POS)
#define IWX_RFH_RXF_DMA_DROP_TOO_LARGE_MASK (0x04000000) /* bit 26 */
#define IWX_RFH_RXF_DMA_SINGLE_FRAME_MASK   (0x20000000) /* bit 29 */
#define IWX_RFH_DMA_EN_MASK                 (0xC0000000) /* bits 30-31*/
#define IWX_RFH_DMA_EN_ENABLE_VAL           (1U << 31)

#define IWX_RFH_RXF_RXQ_ACTIVE 0xA0980C

#define IWX_RFH_GEN_CFG     0xA09800
#define IWX_RFH_GEN_CFG_SERVICE_DMA_SNOOP   (1 << 0)
#define IWX_RFH_GEN_CFG_RFH_DMA_SNOOP       (1 << 1)
#define IWX_RFH_GEN_CFG_RB_CHUNK_SIZE_128   0x00000010
#define IWX_RFH_GEN_CFG_RB_CHUNK_SIZE_64    0x00000000
/* the driver assumes everywhere that the default RXQ is 0 */
#define IWX_RFH_GEN_CFG_DEFAULT_RXQ_NUM     0xF00

/* end of 9000 rx series registers */

/*
 * This register is written by driver and is read by uCode during boot flow.
 * Note this address is cleared after MAC reset.
 */
#define IWX_UREG_UCODE_LOAD_STATUS	(0xa05c40)
#define IWX_UREG_CPU_INIT_RUN		(0xa05c44)

/*
 * HBUS (Host-side Bus)
 *
 * HBUS registers are mapped directly into PCI bus space, but are used
 * to indirectly access device's internal memory or registers that
 * may be powered-down.
 *
 * Use iwl_write_direct32()/iwl_read_direct32() family for these registers;
 * host must "grab nic access" via CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ
 * to make sure the MAC (uCode processor, etc.) is powered up for accessing
 * internal resources.
 *
 * Do not use iwl_write32()/iwl_read32() family to access these registers;
 * these provide only simple PCI bus access, without waking up the MAC.
 */
#define IWX_HBUS_BASE	(0x400)

/*
 * Registers for accessing device's internal SRAM memory (e.g. SCD SRAM
 * structures, error log, event log, verifying uCode load).
 * First write to address register, then read from or write to data register
 * to complete the job.  Once the address register is set up, accesses to
 * data registers auto-increment the address by one dword.
 * Bit usage for address registers (read or write):
 *  0-31:  memory address within device
 */
#define IWX_HBUS_TARG_MEM_RADDR     (IWX_HBUS_BASE+0x00c)
#define IWX_HBUS_TARG_MEM_WADDR     (IWX_HBUS_BASE+0x010)
#define IWX_HBUS_TARG_MEM_WDAT      (IWX_HBUS_BASE+0x018)
#define IWX_HBUS_TARG_MEM_RDAT      (IWX_HBUS_BASE+0x01c)

/*
 * Registers for accessing device's internal peripheral registers
 * (e.g. SCD, BSM, etc.).  First write to address register,
 * then read from or write to data register to complete the job.
 * Bit usage for address registers (read or write):
 *  0-15:  register address (offset) within device
 * 24-25:  (# bytes - 1) to read or write (e.g. 3 for dword)
 */
#define IWX_HBUS_TARG_PRPH_WADDR    (IWX_HBUS_BASE+0x044)
#define IWX_HBUS_TARG_PRPH_RADDR    (IWX_HBUS_BASE+0x048)
#define IWX_HBUS_TARG_PRPH_WDAT     (IWX_HBUS_BASE+0x04c)
#define IWX_HBUS_TARG_PRPH_RDAT     (IWX_HBUS_BASE+0x050)

/* enable the ID buf for read */
#define IWX_WFPM_PS_CTL_CLR			0xa0300c
#define IWX_WFMP_MAC_ADDR_0			0xa03080
#define IWX_WFMP_MAC_ADDR_1			0xa03084
#define IWX_LMPM_PMG_EN				0xa01cec
#define IWX_RADIO_REG_SYS_MANUAL_DFT_0		0xad4078
#define IWX_RFIC_REG_RD				0xad0470
#define IWX_WFPM_CTRL_REG			0xa03030
#define IWX_WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK	0x08000000
#define IWX_ENABLE_WFPM				0x80000000

#define IWX_CNVI_AUX_MISC_CHIP			0xA200B0
#define IWX_CNVI_AUX_MISC_CHIP_MAC_STEP(_val)	(((_val) & 0xf000000) >> 24)
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE(_val)	((_val) & 0xfff)
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_GL	0x910
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_U	0x930
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_I	0x900
#define IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_W	0x901

#define IWX_AUX_MISC_MASTER1_EN			0xa20818
#define IWX_AUX_MISC_MASTER1_EN_SBE_MSK		0x1
#define IWX_AUX_MISC_MASTER1_SMPHR_STATUS	0xa20800
#define IWX_RSA_ENABLE				0xa24b08
#define IWX_PREG_AUX_BUS_WPROT_0		0xa04cc0
#define IWX_PREG_PRPH_WPROT_9000		0xa04ce0
#define IWX_PREG_PRPH_WPROT_22000		0xa04d00
#define IWX_SB_CFG_OVERRIDE_ADDR		0xa26c78
#define IWX_SB_CFG_OVERRIDE_ENABLE		0x8000
#define IWX_SB_CFG_BASE_OVERRIDE		0xa20000
#define IWX_SB_MODIFY_CFG_FLAG			0xa03088
#define IWX_UMAG_SB_CPU_1_STATUS		0xa038c0
#define IWX_UMAG_SB_CPU_2_STATUS		0xa038c4

#define IWX_UREG_CHICK				0xa05c00
#define IWX_UREG_CHICK_MSI_ENABLE		(1 << 24)
#define IWX_UREG_CHICK_MSIX_ENABLE		(1 << 25)

#define IWX_SD_REG_VER		0x00a29600
#define IWX_SD_REG_VER_GEN2	0x00a2b800

#define IWX_HPM_DEBUG			0xa03440
#define IWX_PERSISTENCE_BIT		(1 << 12)
#define IWX_PREG_WFPM_ACCESS		(1 << 12)

#define IWX_HPM_HIPM_GEN_CFG			0xa03458
#define IWX_HPM_HIPM_GEN_CFG_CR_PG_EN		(1 << 0)
#define IWX_HPM_HIPM_GEN_CFG_CR_SLP_EN		(1 << 1)
#define IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE	(1 << 10)

#define IWX_UREG_DOORBELL_TO_ISR6		0xa05c04
#define IWX_UREG_DOORBELL_TO_ISR6_NMI_BIT	(1 << 0)
#define IWX_UREG_DOORBELL_TO_ISR6_RESET_HANDSHAKE ((1 << 0) | (1 << 1))
#define IWX_UREG_DOORBELL_TO_ISR6_SUSPEND	(1 << 18)
#define IWX_UREG_DOORBELL_TO_ISR6_RESUME	(1 << 19)
#define IWX_UREG_DOORBELL_TO_ISR6_PNVM		(1 << 20)

/* LTR control (Qu only) */
#define IWX_HPM_MAC_LTR_CSR			0xa0348c
#define IWX_HPM_MAC_LRT_ENABLE_ALL		0xf
/* also uses CSR_LTR_* for values */
#define IWX_HPM_UMAC_LTR			0xa03480

/*
 * Per-Tx-queue write pointer (index, really!)
 * Indicates index to next TFD that driver will fill (1 past latest filled).
 * Bit usage:
 *  0-7:  queue write index
 * 11-8:  queue selector
 */
#define IWX_HBUS_TARG_WRPTR         (IWX_HBUS_BASE+0x060)
/* Bz: This register is common for Tx and Rx, Rx queues start from 512 */
#define IWX_HBUS_TARG_WRPTR_Q_SHIFT (16)
#define IWX_HBUS_TARG_WRPTR_RX_Q(q) (((q) + 512) << IWX_HBUS_TARG_WRPTR_Q_SHIFT)

/**********************************************************
 * CSR values
 **********************************************************/
 /*
 * host interrupt timeout value
 * used with setting interrupt coalescing timer
 * the CSR_INT_COALESCING is an 8 bit register in 32-usec unit
 *
 * default interrupt coalescing timer is 64 x 32 = 2048 usecs
 */
#define IWX_HOST_INT_TIMEOUT_MAX	(0xFF)
#define IWX_HOST_INT_TIMEOUT_DEF	(0x40)
#define IWX_HOST_INT_TIMEOUT_MIN	(0x0)
#define IWX_HOST_INT_OPER_MODE		(1U << 31)

/*****************************************************************************
 *                        MSIX related registers                             *
 *****************************************************************************/

#define IWX_CSR_MSIX_BASE			(0x2000)
#define IWX_CSR_MSIX_FH_INT_CAUSES_AD		(IWX_CSR_MSIX_BASE + 0x800)
#define IWX_CSR_MSIX_FH_INT_MASK_AD		(IWX_CSR_MSIX_BASE + 0x804)
#define IWX_CSR_MSIX_HW_INT_CAUSES_AD		(IWX_CSR_MSIX_BASE + 0x808)
#define IWX_CSR_MSIX_HW_INT_MASK_AD		(IWX_CSR_MSIX_BASE + 0x80C)
#define IWX_CSR_MSIX_AUTOMASK_ST_AD		(IWX_CSR_MSIX_BASE + 0x810)
#define IWX_CSR_MSIX_RX_IVAR_AD_REG		(IWX_CSR_MSIX_BASE + 0x880)
#define IWX_CSR_MSIX_IVAR_AD_REG		(IWX_CSR_MSIX_BASE + 0x890)
#define IWX_CSR_MSIX_PENDING_PBA_AD		(IWX_CSR_MSIX_BASE + 0x1000)
#define IWX_CSR_MSIX_RX_IVAR(cause)		(IWX_CSR_MSIX_RX_IVAR_AD_REG + (cause))
#define IWX_CSR_MSIX_IVAR(cause)		(IWX_CSR_MSIX_IVAR_AD_REG + (cause))

/*
 * Causes for the FH register interrupts
 */
enum msix_fh_int_causes {
	IWX_MSIX_FH_INT_CAUSES_Q0		= (1 << 0),
	IWX_MSIX_FH_INT_CAUSES_Q1		= (1 << 1),
	IWX_MSIX_FH_INT_CAUSES_D2S_CH0_NUM	= (1 << 16),
	IWX_MSIX_FH_INT_CAUSES_D2S_CH1_NUM	= (1 << 17),
	IWX_MSIX_FH_INT_CAUSES_S2D		= (1 << 19),
	IWX_MSIX_FH_INT_CAUSES_FH_ERR		= (1 << 21),
};

/*
 * Causes for the HW register interrupts
 */
enum mix_hw_int_causes {
	IWX_MSIX_HW_INT_CAUSES_REG_ALIVE	= (1 << 0),
	IWX_MSIX_HW_INT_CAUSES_REG_WAKEUP	= (1 << 1),
	IWX_MSIX_HW_INT_CAUSES_REG_RESET_DONE	= (1 << 2),
	IWX_MSIX_HW_INT_CAUSES_REG_TOP_FATAL_ERR= (1 << 3),
	IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_V2	= (1 << 5),
	IWX_MSIX_HW_INT_CAUSES_REG_CT_KILL	= (1 << 6),
	IWX_MSIX_HW_INT_CAUSES_REG_RF_KILL	= (1 << 7),
	IWX_MSIX_HW_INT_CAUSES_REG_PERIODIC	= (1 << 8),
	IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR	= (1 << 25),
	IWX_MSIX_HW_INT_CAUSES_REG_SCD		= (1 << 26),
	IWX_MSIX_HW_INT_CAUSES_REG_FH_TX	= (1 << 27),
	IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR	= (1 << 29),
	IWX_MSIX_HW_INT_CAUSES_REG_HAP		= (1 << 30),
};

/*
 * Registers to map causes to vectors
 */
enum msix_ivar_for_cause {
	IWX_MSIX_IVAR_CAUSE_D2S_CH0_NUM		= 0x0,
	IWX_MSIX_IVAR_CAUSE_D2S_CH1_NUM		= 0x1,
	IWX_MSIX_IVAR_CAUSE_S2D			= 0x3,
	IWX_MSIX_IVAR_CAUSE_FH_ERR		= 0x5,
	IWX_MSIX_IVAR_CAUSE_REG_ALIVE		= 0x10,
	IWX_MSIX_IVAR_CAUSE_REG_WAKEUP		= 0x11,
	IWX_MSIX_IVAR_CAUSE_REG_RESET_DONE	= 0x12,
	IWX_MSIX_IVAR_CAUSE_REG_SW_ERR_V2	= 0x15,
	IWX_MSIX_IVAR_CAUSE_REG_CT_KILL		= 0x16,
	IWX_MSIX_IVAR_CAUSE_REG_RF_KILL		= 0x17,
	IWX_MSIX_IVAR_CAUSE_REG_PERIODIC	= 0x18,
	IWX_MSIX_IVAR_CAUSE_REG_SW_ERR		= 0x29,
	IWX_MSIX_IVAR_CAUSE_REG_SCD		= 0x2a,
	IWX_MSIX_IVAR_CAUSE_REG_FH_TX		= 0x2b,
	IWX_MSIX_IVAR_CAUSE_REG_HW_ERR		= 0x2d,
	IWX_MSIX_IVAR_CAUSE_REG_HAP		= 0x2e,
};

#define IWX_MSIX_AUTO_CLEAR_CAUSE		(0 << 7)
#define IWX_MSIX_NON_AUTO_CLEAR_CAUSE		(1 << 7)

#define IWX_CSR_ADDR_BASE(sc)			((sc)->mac_addr_from_csr)
#define IWX_CSR_MAC_ADDR0_OTP(sc)		(IWX_CSR_ADDR_BASE(sc) + 0x00)
#define IWX_CSR_MAC_ADDR1_OTP(sc)		(IWX_CSR_ADDR_BASE(sc) + 0x04)
#define IWX_CSR_MAC_ADDR0_STRAP(sc)		(IWX_CSR_ADDR_BASE(sc) + 0x08)
#define IWX_CSR_MAC_ADDR1_STRAP(sc)		(IWX_CSR_ADDR_BASE(sc) + 0x0c)

/**
 * uCode API flags
 * @IWX_UCODE_TLV_FLAGS_PAN: This is PAN capable microcode; this previously
 *	was a separate TLV but moved here to save space.
 * @IWX_UCODE_TLV_FLAGS_NEWSCAN: new uCode scan behaviour on hidden SSID,
 *	treats good CRC threshold as a boolean
 * @IWX_UCODE_TLV_FLAGS_MFP: This uCode image supports MFP (802.11w).
 * @IWX_UCODE_TLV_FLAGS_P2P: This uCode image supports P2P.
 * @IWX_UCODE_TLV_FLAGS_DW_BC_TABLE: The SCD byte count table is in DWORDS
 * @IWX_UCODE_TLV_FLAGS_UAPSD: This uCode image supports uAPSD
 * @IWX_UCODE_TLV_FLAGS_SHORT_BL: 16 entries of black list instead of 64 in scan
 *	offload profile config command.
 * @IWX_UCODE_TLV_FLAGS_D3_6_IPV6_ADDRS: D3 image supports up to six
 *	(rather than two) IPv6 addresses
 * @IWX_UCODE_TLV_FLAGS_NO_BASIC_SSID: not sending a probe with the SSID element
 *	from the probe request template.
 * @IWX_UCODE_TLV_FLAGS_NEW_NSOFFL_SMALL: new NS offload (small version)
 * @IWX_UCODE_TLV_FLAGS_NEW_NSOFFL_LARGE: new NS offload (large version)
 * @IWX_UCODE_TLV_FLAGS_P2P_PS: P2P client power save is supported (only on a
 *	single bound interface).
 * @IWX_UCODE_TLV_FLAGS_UAPSD_SUPPORT: General support for uAPSD
 * @IWX_UCODE_TLV_FLAGS_EBS_SUPPORT: this uCode image supports EBS.
 * @IWX_UCODE_TLV_FLAGS_P2P_PS_UAPSD: P2P client supports uAPSD power save
 * @IWX_UCODE_TLV_FLAGS_BCAST_FILTERING: uCode supports broadcast filtering.
 * @IWX_UCODE_TLV_FLAGS_GO_UAPSD: AP/GO interfaces support uAPSD clients
 *
 */
#define IWX_UCODE_TLV_FLAGS_PAN			(1 << 0)
#define IWX_UCODE_TLV_FLAGS_NEWSCAN		(1 << 1)
#define IWX_UCODE_TLV_FLAGS_MFP			(1 << 2)
#define IWX_UCODE_TLV_FLAGS_P2P			(1 << 3)
#define IWX_UCODE_TLV_FLAGS_DW_BC_TABLE		(1 << 4)
#define IWX_UCODE_TLV_FLAGS_SHORT_BL		(1 << 7)
#define IWX_UCODE_TLV_FLAGS_D3_6_IPV6_ADDRS	(1 << 10)
#define IWX_UCODE_TLV_FLAGS_NO_BASIC_SSID	(1 << 12)
#define IWX_UCODE_TLV_FLAGS_NEW_NSOFFL_SMALL	(1 << 15)
#define IWX_UCODE_TLV_FLAGS_NEW_NSOFFL_LARGE	(1 << 16)
#define IWX_UCODE_TLV_FLAGS_P2P_PS		(1 << 21)
#define IWX_UCODE_TLV_FLAGS_BSS_P2P_PS_DCM	(1 << 22)
#define IWX_UCODE_TLV_FLAGS_BSS_P2P_PS_SCM	(1 << 23)
#define IWX_UCODE_TLV_FLAGS_UAPSD_SUPPORT	(1 << 24)
#define IWX_UCODE_TLV_FLAGS_EBS_SUPPORT		(1 << 25)
#define IWX_UCODE_TLV_FLAGS_P2P_PS_UAPSD	(1 << 26)
#define IWX_UCODE_TLV_FLAGS_BCAST_FILTERING	(1 << 29)
#define IWX_UCODE_TLV_FLAGS_GO_UAPSD		(1 << 30)
#define IWX_UCODE_TLV_FLAGS_LTE_COEX		(1U << 31)

#define IWX_UCODE_TLV_FLAG_BITS \
	"\020\1PAN\2NEWSCAN\3MFP\4P2P\5DW_BC_TABLE\6NEWBT_COEX\7PM_CMD\10SHORT_BL\11RX_ENERGY\12TIME_EVENT_V2\13D3_6_IPV6\14BF_UPDATED\15NO_BASIC_SSID\17D3_CONTINUITY\20NEW_NSOFFL_S\21NEW_NSOFFL_L\22SCHED_SCAN\24STA_KEY_CMD\25DEVICE_PS_CMD\26P2P_PS\27P2P_PS_DCM\30P2P_PS_SCM\31UAPSD_SUPPORT\32EBS\33P2P_PS_UAPSD\36BCAST_FILTERING\37GO_UAPSD\40LTE_COEX"

/**
 * uCode TLV api
 * @IWX_UCODE_TLV_API_FRAGMENTED_SCAN: This ucode supports active dwell time
 *	longer than the passive one, which is essential for fragmented scan.
 * @IWX_UCODE_TLV_API_WIFI_MCC_UPDATE: ucode supports MCC updates with source.
 * @IWX_UCODE_TLV_API_WIDE_CMD_HDR: ucode supports wide command header
 * @IWX_UCODE_TLV_API_LQ_SS_PARAMS: Configure STBC/BFER via LQ CMD ss_params
 * @IWX_UCODE_TLV_API_NEW_VERSION: new versioning format
 * @IWX_UCODE_TLV_API_TX_POWER_CHAIN: TX power API has larger command size
 *	(command version 3) that supports per-chain limits
 * @IWX_UCODE_TLV_API_SCAN_TSF_REPORT: Scan start time reported in scan
 *	iteration complete notification, and the timestamp reported for RX
 *	received during scan, are reported in TSF of the mac specified in the
 *	scan request.
 * @IWX_UCODE_TLV_API_TKIP_MIC_KEYS: This ucode supports version 2 of
 *	ADD_MODIFY_STA_KEY_API_S_VER_2.
 * @IWX_UCODE_TLV_API_STA_TYPE: This ucode supports station type assignment.
 * @IWX_UCODE_TLV_API_EXT_SCAN_PRIORITY: scan APIs use 8-level priority
 *	instead of 3.
 * @IWX_UCODE_TLV_API_NEW_RX_STATS: should new RX STATISTICS API be used
 * @IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG: This ucode supports v3 of
 *  SCAN_CONFIG_DB_CMD_API_S.
 *
 * @IWX_NUM_UCODE_TLV_API: number of bits used
 */
#define IWX_UCODE_TLV_API_FRAGMENTED_SCAN	8
#define IWX_UCODE_TLV_API_WIFI_MCC_UPDATE	9
#define IWX_UCODE_TLV_API_WIDE_CMD_HDR		14
#define IWX_UCODE_TLV_API_LQ_SS_PARAMS		18
#define IWX_UCODE_TLV_API_NEW_VERSION		20
#define IWX_UCODE_TLV_API_EXT_SCAN_PRIORITY	24
#define IWX_UCODE_TLV_API_TX_POWER_CHAIN	27
#define IWX_UCODE_TLV_API_SCAN_TSF_REPORT	28
#define IWX_UCODE_TLV_API_TKIP_MIC_KEYS         29
#define IWX_UCODE_TLV_API_STA_TYPE		30
#define IWX_UCODE_TLV_API_NAN2_VER2		31
#define IWX_UCODE_TLV_API_ADAPTIVE_DWELL	32
#define IWX_UCODE_TLV_API_NEW_RX_STATS		35
#define IWX_UCODE_TLV_API_WOWLAN_KEY_MATERIAL	36
#define IWX_UCODE_TLV_API_QUOTA_LOW_LATENCY	38
#define IWX_UCODE_TLV_API_DEPRECATE_TTAK	41
#define IWX_UCODE_TLV_API_ADAPTIVE_DWELL_V2	42
#define IWX_UCODE_TLV_API_NAN_NOTIF_V2		43
#define IWX_UCODE_TLV_API_FRAG_EBS		44
#define IWX_UCODE_TLV_API_REDUCE_TX_POWER	45
#define IWX_UCODE_TLV_API_SHORT_BEACON_NOTIF	46
#define IWX_UCODE_TLV_API_BEACON_FILTER_V4	47
#define IWX_UCODE_TLV_API_REGULATORY_NVM_INFO	48
#define IWX_UCODE_TLV_API_FTM_NEW_RANGE_REQ	49
#define IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG	56
#define IWX_UCODE_TLV_API_SCAN_OFFLOAD_CHANS	50
#define IWX_UCODE_TLV_API_MBSSID_HE		52
#define IWX_UCODE_TLV_API_WOWLAN_TCP_SYN_WAKE	53
#define IWX_UCODE_TLV_API_FTM_RTT_ACCURACY	54
#define IWX_UCODE_TLV_API_SAR_TABLE_VER		55
#define IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG	56
#define IWX_UCODE_TLV_API_ADWELL_HB_DEF_N_AP	57
#define IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER	58
#define IWX_UCODE_TLV_API_BAND_IN_RX_DATA	59
#define IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD	68
#define IWX_NUM_UCODE_TLV_API			128

#define IWX_UCODE_TLV_API_BITS \
	"\020\10FRAGMENTED_SCAN\11WIFI_MCC_UPDATE\16WIDE_CMD_HDR\22LQ_SS_PARAMS\30EXT_SCAN_PRIO\33TX_POWER_CHAIN\35TKIP_MIC_KEYS"

/**
 * uCode capabilities
 * @IWX_UCODE_TLV_CAPA_D0I3_SUPPORT: supports D0i3
 * @IWX_UCODE_TLV_CAPA_LAR_SUPPORT: supports Location Aware Regulatory
 * @IWX_UCODE_TLV_CAPA_UMAC_SCAN: supports UMAC scan.
 * @IWX_UCODE_TLV_CAPA_BEAMFORMER: supports Beamformer
 * @IWX_UCODE_TLV_CAPA_TOF_SUPPORT: supports Time of Flight (802.11mc FTM)
 * @IWX_UCODE_TLV_CAPA_TDLS_SUPPORT: support basic TDLS functionality
 * @IWX_UCODE_TLV_CAPA_TXPOWER_INSERTION_SUPPORT: supports insertion of current
 *	tx power value into TPC Report action frame and Link Measurement Report
 *	action frame
 * @IWX_UCODE_TLV_CAPA_DS_PARAM_SET_IE_SUPPORT: supports updating current
 *	channel in DS parameter set element in probe requests.
 * @IWX_UCODE_TLV_CAPA_WFA_TPC_REP_IE_SUPPORT: supports adding TPC Report IE in
 *	probe requests.
 * @IWX_UCODE_TLV_CAPA_QUIET_PERIOD_SUPPORT: supports Quiet Period requests
 * @IWX_UCODE_TLV_CAPA_DQA_SUPPORT: supports dynamic queue allocation (DQA),
 *	which also implies support for the scheduler configuration command
 * @IWX_UCODE_TLV_CAPA_TDLS_CHANNEL_SWITCH: supports TDLS channel switching
 * @IWX_UCODE_TLV_CAPA_CNSLDTD_D3_D0_IMG: Consolidated D3-D0 image
 * @IWX_UCODE_TLV_CAPA_HOTSPOT_SUPPORT: supports Hot Spot Command
 * @IWX_UCODE_TLV_CAPA_DC2DC_SUPPORT: supports DC2DC Command
 * @IWX_UCODE_TLV_CAPA_2G_COEX_SUPPORT: supports 2G coex Command
 * @IWX_UCODE_TLV_CAPA_CSUM_SUPPORT: supports TCP Checksum Offload
 * @IWX_UCODE_TLV_CAPA_RADIO_BEACON_STATS: support radio and beacon statistics
 * @IWX_UCODE_TLV_CAPA_P2P_STANDALONE_UAPSD: support p2p standalone U-APSD
 * @IWX_UCODE_TLV_CAPA_BT_COEX_PLCR: enabled BT Coex packet level co-running
 * @IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC: ucode supports LAR updates with different
 *	sources for the MCC. This TLV bit is a future replacement to
 *	IWX_UCODE_TLV_API_WIFI_MCC_UPDATE. When either is set, multi-source LAR
 *	is supported.
 * @IWX_UCODE_TLV_CAPA_BT_COEX_RRC: supports BT Coex RRC
 * @IWX_UCODE_TLV_CAPA_GSCAN_SUPPORT: supports gscan
 * @IWX_UCODE_TLV_CAPA_NAN_SUPPORT: supports NAN
 * @IWX_UCODE_TLV_CAPA_UMAC_UPLOAD: supports upload mode in umac (1=supported,
 *	0=no support)
 * @IWx_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS: firmware supports ultra high band
 *	(6 GHz).
 * @IWX_UCODE_TLV_CAPA_EXTENDED_DTS_MEASURE: extended DTS measurement
 * @IWX_UCODE_TLV_CAPA_SHORT_PM_TIMEOUTS: supports short PM timeouts
 * @IWX_UCODE_TLV_CAPA_BT_MPLUT_SUPPORT: supports bt-coex Multi-priority LUT
 * @IWX_UCODE_TLV_CAPA_BEACON_ANT_SELECTION: firmware will decide on what
 *	antenna the beacon should be transmitted
 * @IWX_UCODE_TLV_CAPA_BEACON_STORING: firmware will store the latest beacon
 *	from AP and will send it upon d0i3 exit.
 * @IWX_UCODE_TLV_CAPA_LAR_SUPPORT_V2: support LAR API V2
 * @IWX_UCODE_TLV_CAPA_CT_KILL_BY_FW: firmware responsible for CT-kill
 * @IWX_UCODE_TLV_CAPA_TEMP_THS_REPORT_SUPPORT: supports temperature
 *	thresholds reporting
 * @IWX_UCODE_TLV_CAPA_CTDP_SUPPORT: supports cTDP command
 * @IWX_UCODE_TLV_CAPA_USNIFFER_UNIFIED: supports usniffer enabled in
 *	regular image.
 * @IWX_UCODE_TLV_CAPA_EXTEND_SHARED_MEM_CFG: support getting more shared
 *	memory addresses from the firmware.
 * @IWX_UCODE_TLV_CAPA_LQM_SUPPORT: supports Link Quality Measurement
 * @IWX_UCODE_TLV_CAPA_LMAC_UPLOAD: supports upload mode in lmac (1=supported,
 *	0=no support)
 *
 * @IWX_NUM_UCODE_TLV_CAPA: number of bits used
 */
#define IWX_UCODE_TLV_CAPA_D0I3_SUPPORT			0
#define IWX_UCODE_TLV_CAPA_LAR_SUPPORT			1
#define IWX_UCODE_TLV_CAPA_UMAC_SCAN			2
#define IWX_UCODE_TLV_CAPA_BEAMFORMER			3
#define IWX_UCODE_TLV_CAPA_TOF_SUPPORT                  5
#define IWX_UCODE_TLV_CAPA_TDLS_SUPPORT			6
#define IWX_UCODE_TLV_CAPA_TXPOWER_INSERTION_SUPPORT	8
#define IWX_UCODE_TLV_CAPA_DS_PARAM_SET_IE_SUPPORT	9
#define IWX_UCODE_TLV_CAPA_WFA_TPC_REP_IE_SUPPORT	10
#define IWX_UCODE_TLV_CAPA_QUIET_PERIOD_SUPPORT		11
#define IWX_UCODE_TLV_CAPA_DQA_SUPPORT			12
#define IWX_UCODE_TLV_CAPA_TDLS_CHANNEL_SWITCH		13
#define IWX_UCODE_TLV_CAPA_CNSLDTD_D3_D0_IMG		17
#define IWX_UCODE_TLV_CAPA_HOTSPOT_SUPPORT		18
#define IWX_UCODE_TLV_CAPA_DC2DC_CONFIG_SUPPORT		19
#define IWX_UCODE_TLV_CAPA_2G_COEX_SUPPORT		20
#define IWX_UCODE_TLV_CAPA_CSUM_SUPPORT			21
#define IWX_UCODE_TLV_CAPA_RADIO_BEACON_STATS		22
#define IWX_UCODE_TLV_CAPA_P2P_STANDALONE_UAPSD		26
#define IWX_UCODE_TLV_CAPA_BT_COEX_PLCR			28
#define IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC		29
#define IWX_UCODE_TLV_CAPA_BT_COEX_RRC			30
#define IWX_UCODE_TLV_CAPA_GSCAN_SUPPORT		31
#define IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG		32
#define IWX_UCODE_TLV_CAPA_NAN_SUPPORT			34
#define IWX_UCODE_TLV_CAPA_UMAC_UPLOAD			35
#define IWM_UCODE_TLV_CAPA_SOC_LATENCY_SUPPORT		37
#define IWX_UCODE_TLV_CAPA_STA_PM_NOTIF			38
#define IWX_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT		39
#define IWX_UCODE_TLV_CAPA_CDB_SUPPORT			40
#define IWX_UCODE_TLV_CAPA_D0I3_END_FIRST		41
#define IWX_UCODE_TLV_CAPA_TLC_OFFLOAD			43
#define IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA		44
#define IWX_UCODE_TLV_CAPA_COEX_SCHEMA_2		45
#define IWX_UCODE_TLV_CAPA_CHANNEL_SWITCH_CMD		46
#define IWX_UCODE_TLV_CAPA_FTM_CALIBRATED		47
#define IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS		48
#define IWX_UCODE_TLV_CAPA_CS_MODIFY			49
#define IWX_UCODE_TLV_CAPA_SET_LTR_GEN2			50
#define IWX_UCODE_TLV_CAPA_SET_PPAG			52
#define IWX_UCODE_TLV_CAPA_TAS_CFG			53
#define IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD		54
#define IWX_UCODE_TLV_CAPA_PROTECTED_TWT		56
#define IWX_UCODE_TLV_CAPA_FW_RESET_HANDSHAKE		57
#define IWX_UCODE_TLV_CAPA_PASSIVE_6GHZ_SCAN		58
#define IWX_UCODE_TLV_CAPA_PROTECTED_TWT		56
#define IWX_UCODE_TLV_CAPA_FW_RESET_HANDSHAKE		57
#define IWX_UCODE_TLV_CAPA_PASSIVE_6GHZ_SCAN		58
#define IWX_UCODE_TLV_CAPA_BAID_ML_SUPPORT		63
#define IWX_UCODE_TLV_CAPA_EXTENDED_DTS_MEASURE		64
#define IWX_UCODE_TLV_CAPA_SHORT_PM_TIMEOUTS		65
#define IWX_UCODE_TLV_CAPA_BT_MPLUT_SUPPORT		67
#define IWX_UCODE_TLV_CAPA_MULTI_QUEUE_RX_SUPPORT	68
#define IWX_UCODE_TLV_CAPA_CSA_AND_TBTT_OFFLOAD		70
#define IWX_UCODE_TLV_CAPA_BEACON_ANT_SELECTION		71
#define IWX_UCODE_TLV_CAPA_BEACON_STORING		72
#define IWX_UCODE_TLV_CAPA_LAR_SUPPORT_V3		73
#define IWX_UCODE_TLV_CAPA_CT_KILL_BY_FW		74
#define IWX_UCODE_TLV_CAPA_TEMP_THS_REPORT_SUPPORT	75
#define IWX_UCODE_TLV_CAPA_CTDP_SUPPORT			76
#define IWX_UCODE_TLV_CAPA_USNIFFER_UNIFIED		77
#define IWX_UCODE_TLV_CAPA_LMAC_UPLOAD			79
#define IWX_UCODE_TLV_CAPA_EXTEND_SHARED_MEM_CFG	80
#define IWX_UCODE_TLV_CAPA_LQM_SUPPORT			81
#define IWX_UCODE_TLV_CAPA_TX_POWER_ACK			84
#define IWX_UCODE_TLV_CAPA_D3_DEBUG			87
#define IWX_UCODE_TLV_CAPA_LED_CMD_SUPPORT		88
#define IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT	89
#define IWX_UCODE_TLV_CAPA_CSI_REPORTING		90
#define IWX_UCODE_TLV_CAPA_CSI_REPORTING_V2		91
#define IWX_UCODE_TLV_CAPA_DBG_SUSPEND_RESUME_CMD_SUPP	92
#define IWX_UCODE_TLV_CAPA_DBG_BUF_ALLOC_CMD_SUPP	93
#define IWX_UCODE_TLV_CAPA_MLME_OFFLOAD			96
#define IWX_UCODE_TLV_CAPA_BIGTK_SUPPORT		100
#define IWX_UCODE_TLV_CAPA_RFIM_SUPPORT			102
#define IWX_UCODE_TLV_CAPA_MLD_API_SUPPORT		110 

#define IWX_NUM_UCODE_TLV_CAPA 160

/*
 * For 16.0 uCode and above, there is no differentiation between sections,
 * just an offset to the HW address.
 */
#define IWX_CPU1_CPU2_SEPARATOR_SECTION		0xFFFFCCCC
#define IWX_PAGING_SEPARATOR_SECTION		0xAAAABBBB

/* uCode version contains 4 values: Major/Minor/API/Serial */
#define IWX_UCODE_MAJOR(ver)	(((ver) & 0xFF000000) >> 24)
#define IWX_UCODE_MINOR(ver)	(((ver) & 0x00FF0000) >> 16)
#define IWX_UCODE_API(ver)	(((ver) & 0x0000FF00) >> 8)
#define IWX_UCODE_SERIAL(ver)	((ver) & 0x000000FF)

/*
 * Calibration control struct.
 * Sent as part of the phy configuration command.
 * @flow_trigger: bitmap for which calibrations to perform according to
 *		flow triggers.
 * @event_trigger: bitmap for which calibrations to perform according to
 *		event triggers.
 */
struct iwx_tlv_calib_ctrl {
	uint32_t flow_trigger;
	uint32_t event_trigger;
} __packed;

#define IWX_FW_PHY_CFG_RADIO_TYPE_POS	0
#define IWX_FW_PHY_CFG_RADIO_TYPE	(0x3 << IWX_FW_PHY_CFG_RADIO_TYPE_POS)
#define IWX_FW_PHY_CFG_RADIO_STEP_POS	2
#define IWX_FW_PHY_CFG_RADIO_STEP	(0x3 << IWX_FW_PHY_CFG_RADIO_STEP_POS)
#define IWX_FW_PHY_CFG_RADIO_DASH_POS	4
#define IWX_FW_PHY_CFG_RADIO_DASH	(0x3 << IWX_FW_PHY_CFG_RADIO_DASH_POS)
#define IWX_FW_PHY_CFG_TX_CHAIN_POS	16
#define IWX_FW_PHY_CFG_TX_CHAIN		(0xf << IWX_FW_PHY_CFG_TX_CHAIN_POS)
#define IWX_FW_PHY_CFG_RX_CHAIN_POS	20
#define IWX_FW_PHY_CFG_RX_CHAIN		(0xf << IWX_FW_PHY_CFG_RX_CHAIN_POS)

/**
 * struct iwx_fw_cipher_scheme - a cipher scheme supported by FW.
 * @cipher: a cipher suite selector
 * @flags: cipher scheme flags (currently reserved for a future use)
 * @hdr_len: a size of MPDU security header
 * @pn_len: a size of PN
 * @pn_off: an offset of pn from the beginning of the security header
 * @key_idx_off: an offset of key index byte in the security header
 * @key_idx_mask: a bit mask of key_idx bits
 * @key_idx_shift: bit shift needed to get key_idx
 * @mic_len: mic length in bytes
 * @hw_cipher: a HW cipher index used in host commands
 */
struct iwx_fw_cipher_scheme {
	uint32_t cipher;
	uint8_t flags;
	uint8_t hdr_len;
	uint8_t pn_len;
	uint8_t pn_off;
	uint8_t key_idx_off;
	uint8_t key_idx_mask;
	uint8_t key_idx_shift;
	uint8_t mic_len;
	uint8_t hw_cipher;
} __packed;

/**
 * struct iwx_fw_cscheme_list - a cipher scheme list
 * @size: a number of entries
 * @cs: cipher scheme entries
 */
struct iwx_fw_cscheme_list {
	uint8_t size;
	struct iwx_fw_cipher_scheme cs[];
} __packed;

/* v1/v2 uCode file layout */
struct iwx_ucode_header {
	uint32_t ver;	/* major/minor/API/serial */
	union {
		struct {
			uint32_t inst_size;	/* bytes of runtime code */
			uint32_t data_size;	/* bytes of runtime data */
			uint32_t init_size;	/* bytes of init code */
			uint32_t init_data_size;	/* bytes of init data */
			uint32_t boot_size;	/* bytes of bootstrap code */
			uint8_t data[0];		/* in same order as sizes */
		} v1;
		struct {
			uint32_t build;		/* build number */
			uint32_t inst_size;	/* bytes of runtime code */
			uint32_t data_size;	/* bytes of runtime data */
			uint32_t init_size;	/* bytes of init code */
			uint32_t init_data_size;	/* bytes of init data */
			uint32_t boot_size;	/* bytes of bootstrap code */
			uint8_t data[0];		/* in same order as sizes */
		} v2;
	} u;
};

/*
 * new TLV uCode file layout
 *
 * The new TLV file format contains TLVs, that each specify
 * some piece of data.
 */

#define IWX_UCODE_TLV_INVALID		0 /* unused */

#undef __packed
#endif
