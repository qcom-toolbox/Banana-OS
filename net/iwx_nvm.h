/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_NVM_H
#define IWX_NVM_H
#include "iwx_fw.h"
#define IWX_NVM_CHANNELS 110
typedef struct {
    uint16_t number,mhz;
    uint8_t band;
    uint32_t flags;
} iwx_nvm_channel_t;
typedef struct {
    uint8_t mac[6],addresses,tx_chains,rx_chains;
    uint16_t version;
    uint32_t sku,lar,nchannels;
    iwx_nvm_channel_t channels[IWX_NVM_CHANNELS];
} iwx_nvm_t;
int iwx_mac_valid(const uint8_t mac[6]);
/* CSR words contain the MAC's network-order octets, not a six-byte LE array. */
int iwx_mac_decode(uint8_t out[6],uint32_t first,uint32_t last);
/* Decode only the advertised v3/v4 response ABI. Failure clears output.
 * Channel flags are metadata, never authorization to transmit. Runtime MCC/
 * regulatory configuration and station policy must still restrict every use. */
int iwx_nvm_read(iwx_nvm_t*,const iwx_firmware_t*,int model,const uint8_t mac[6],
                  const uint8_t* payload,uint32_t size);
#endif
