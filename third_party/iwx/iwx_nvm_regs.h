/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BANANA_IWX_NVM_REGS_H
#define BANANA_IWX_NVM_REGS_H
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
struct iwx_nvm_get_info_general {
	uint32_t flags;
	uint16_t nvm_version;
	uint8_t board_type;
	uint8_t n_hw_addrs;
} __packed; /* REGULATORY_NVM_GET_INFO_GENERAL_S_VER_2 */

/**
 * iwx_nvm_mac_sku_flags - flags in &iwl_nvm_get_info_sku
 * @NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED: true if 2.4 band enabled
 * @NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED: true if 5.2 band enabled
 * @NVM_MAC_SKU_FLAGS_802_11N_ENABLED: true if 11n enabled
 * @NVM_MAC_SKU_FLAGS_802_11AC_ENABLED: true if 11ac enabled
 * @NVM_MAC_SKU_FLAGS_802_11AX_ENABLED: true if 11ax enabled
 * @NVM_MAC_SKU_FLAGS_MIMO_DISABLED: true if MIMO disabled
 * @NVM_MAC_SKU_FLAGS_WAPI_ENABLED: true if WAPI enabled
 * @NVM_MAC_SKU_FLAGS_REG_CHECK_ENABLED: true if regulatory checker enabled
 * @NVM_MAC_SKU_FLAGS_API_LOCK_ENABLED: true if API lock enabled
 */
#define IWX_NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED	(1 << 0)
#define IWX_NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED	(1 << 1)
#define IWX_NVM_MAC_SKU_FLAGS_802_11N_ENABLED	(1 << 2)
#define IWX_NVM_MAC_SKU_FLAGS_802_11AC_ENABLED	(1 << 3)
#define IWX_NVM_MAC_SKU_FLAGS_802_11AX_ENABLED	(1 << 4)
#define IWX_NVM_MAC_SKU_FLAGS_MIMO_DISABLED	(1 << 5)
#define IWX_NVM_MAC_SKU_FLAGS_WAPI_ENABLED	(1 << 8)
#define IWX_NVM_MAC_SKU_FLAGS_REG_CHECK_ENABLED	(1 << 14)
#define IWX_NVM_MAC_SKU_FLAGS_API_LOCK_ENABLED	(1 << 15)

/**
 * struct iwx_nvm_get_info_sku - mac information
 * @mac_sku_flags: flags for SKU, see &enum iwl_nvm_mac_sku_flags
 */
struct iwx_nvm_get_info_sku {
	uint32_t mac_sku_flags;
} __packed; /* REGULATORY_NVM_GET_INFO_MAC_SKU_SECTION_S_VER_2 */

/**
 * struct iwx_nvm_get_info_phy - phy information
 * @tx_chains: BIT 0 chain A, BIT 1 chain B
 * @rx_chains: BIT 0 chain A, BIT 1 chain B
 */
struct iwx_nvm_get_info_phy {
	uint32_t tx_chains;
	uint32_t rx_chains;
} __packed; /* REGULATORY_NVM_GET_INFO_PHY_SKU_SECTION_S_VER_1 */

#define IWX_NUM_CHANNELS_V1	51
#define IWX_NUM_CHANNELS	110

/**
 * struct iwx_nvm_get_info_regulatory - regulatory information
 * @lar_enabled: is LAR enabled
 * @channel_profile: regulatory data of this channel
 * @reserved: reserved
 */
struct iwx_nvm_get_info_regulatory_v1 {
	uint32_t lar_enabled;
	uint16_t channel_profile[IWX_NUM_CHANNELS_V1];
	uint16_t reserved;
} __packed; /* REGULATORY_NVM_GET_INFO_REGULATORY_S_VER_1 */

/**
 * struct iwx_nvm_get_info_regulatory - regulatory information
 * @lar_enabled: is LAR enabled
 * @n_channels: number of valid channels in the array
 * @channel_profile: regulatory data of this channel
 */
struct iwx_nvm_get_info_regulatory {
	uint32_t lar_enabled;
	uint32_t n_channels;
	uint32_t channel_profile[IWX_NUM_CHANNELS];
} __packed; /* REGULATORY_NVM_GET_INFO_REGULATORY_S_VER_2 */

/**
 * struct iwx_nvm_get_info_rsp_v3 - response to get NVM data
 * @general: general NVM data
 * @mac_sku: data relating to MAC sku
 * @phy_sku: data relating to PHY sku
 * @regulatory: regulatory data
 */
struct iwx_nvm_get_info_rsp_v3 {
	struct iwx_nvm_get_info_general general;
	struct iwx_nvm_get_info_sku mac_sku;
	struct iwx_nvm_get_info_phy phy_sku;
	struct iwx_nvm_get_info_regulatory_v1 regulatory;
} __packed; /* REGULATORY_NVM_GET_INFO_RSP_API_S_VER_3 */

/**
 * struct iwx_nvm_get_info_rsp - response to get NVM data
 * @general: general NVM data
 * @mac_sku: data relating to MAC sku
 * @phy_sku: data relating to PHY sku
 * @regulatory: regulatory data
 */
struct iwx_nvm_get_info_rsp {
	struct iwx_nvm_get_info_general general;
	struct iwx_nvm_get_info_sku mac_sku;
	struct iwx_nvm_get_info_phy phy_sku;
	struct iwx_nvm_get_info_regulatory regulatory;
} __packed; /* REGULATORY_NVM_GET_INFO_RSP_API_S_VER_4 */
#undef __packed
#endif
