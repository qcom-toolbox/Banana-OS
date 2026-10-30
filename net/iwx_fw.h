/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_FW_H
#define IWX_FW_H
#include "../kernel/types.h"
/* Intel MVM API 77, shared by AX200 (CC) and AX210 (TY).
 * These are borrowed read-only views: the owner must hold the input resident
 * until all DMA copies have finished. Parsing never starts or registers a radio. */
#define IWX_FW_SECTIONS 64
#define IWX_FW_COMMANDS 256
#define IWX_FW_BITMAP_WORDS 16
#define IWX_FW_CPU_SEPARATOR 0xffffccccu
#define IWX_FW_PAGING_SEPARATOR 0xaaaabbbbu
typedef struct {
    uint32_t offset, size;
    const uint8_t* data;
} iwx_fw_section_t;
typedef struct { uint8_t command, group, command_version, notification_version; } iwx_fw_command_t;
typedef struct {
    uint32_t api, build, flags, phy_config, paging_size, cpus, nsections, ncommands;
    uint32_t capabilities[IWX_FW_BITMAP_WORDS], api_flags[IWX_FW_BITMAP_WORDS];
    uint32_t hardware[3], version[3];
    iwx_fw_section_t sections[IWX_FW_SECTIONS];
    iwx_fw_command_t commands[IWX_FW_COMMANDS];
    const uint8_t* iml;
    uint32_t iml_size;
} iwx_firmware_t;
enum { IWX_FW_AX200 = 200, IWX_FW_AX210 = 210 };
/* 0 on success; -1 malformed, -2 unsupported ABI/model. Failure clears output. */
int iwx_firmware_parse(iwx_firmware_t*,const uint8_t*,uint32_t,int model);
int iwx_firmware_command_version(const iwx_firmware_t*,uint8_t group,uint8_t command,int notification);
#endif
