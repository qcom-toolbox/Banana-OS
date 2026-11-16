/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../net/wifi.h"
#include "fs.h"
extern const uint8_t mt7601_notices_start[],mt7601_notices_end[];
extern const uint8_t rtl8188eu_notice_start[],rtl8188eu_notice_end[];
extern const uint8_t rtl8811cu_notice_start[],rtl8811cu_notice_end[];
extern const uint8_t igc_notices_start[],igc_notices_end[];
extern const uint8_t uacpi_notice_start[],uacpi_notice_end[];
extern const uint8_t ar9280_notice_start[],ar9280_notice_end[];
extern const uint8_t ax88179a_notice_start[],ax88179a_notice_end[];
extern const uint8_t aqc107_notice_start[],aqc107_notice_end[];
extern const uint8_t r8125_notice_start[],r8125_notice_end[];
extern const uint8_t radeon_notice_start[],radeon_notice_end[];
extern const uint8_t rad_fw_licence[],rad_fw_licence_end[];
static void install_notice(const char* dir,const char* path,const uint8_t* start,const uint8_t* end) {
    if(fs_mkdir_p(dir)<0) return;
    int f=fs_find_file(path);
    if(f<0) f=fs_create(path);
    if(f>=0) fs_write(f,start,(uint32_t)((uintptr_t)end-(uintptr_t)start));
}
void wifi_boot_files(void) {
    install_notice("/usr/share/licenses/rtl8811cu","/usr/share/licenses/rtl8811cu/NOTICES",
                   rtl8811cu_notice_start,rtl8811cu_notice_end);
    install_notice("/usr/share/licenses/ar9380","/usr/share/licenses/ar9380/NOTICES",
                   ar9280_notice_start,ar9280_notice_end);
    install_notice("/usr/share/licenses/ar9280","/usr/share/licenses/ar9280/NOTICES",
                   ar9280_notice_start,ar9280_notice_end);
    install_notice("/usr/share/licenses/ax88179a","/usr/share/licenses/ax88179a/NOTICES",
                   ax88179a_notice_start,ax88179a_notice_end);
    install_notice("/usr/share/licenses/aqc107","/usr/share/licenses/aqc107/NOTICES",
                   aqc107_notice_start,aqc107_notice_end);
    install_notice("/usr/share/licenses/rtl8188eu","/usr/share/licenses/rtl8188eu/NOTICES",
                   rtl8188eu_notice_start,rtl8188eu_notice_end);
    install_notice("/usr/share/licenses/r8125","/usr/share/licenses/r8125/NOTICES",
                   r8125_notice_start,r8125_notice_end);
    install_notice("/usr/share/licenses/uacpi","/usr/share/licenses/uacpi/LICENSE",
                   uacpi_notice_start,uacpi_notice_end);
    install_notice("/usr/share/licenses/mt7601u","/usr/share/licenses/mt7601u/NOTICES",
                   mt7601_notices_start,mt7601_notices_end);
    install_notice("/usr/share/licenses/igc","/usr/share/licenses/igc/NOTICES",
                   igc_notices_start,igc_notices_end);
    install_notice("/usr/share/licenses/radeon","/usr/share/licenses/radeon/NOTICES",
                   radeon_notice_start,radeon_notice_end);
    install_notice("/usr/share/licenses/radeon","/usr/share/licenses/radeon/LICENSE.radeon-firmware",
                   rad_fw_licence,rad_fw_licence_end);
}
