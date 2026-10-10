#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"

/* Screen resolution (Settings > Screen): changeable on QEMU / Bochs /
 * VirtualBox displays (the VBE DISPI interface); elsewhere the boot mode. */

typedef struct { int w, h; } display_mode_t;

int  display_can_change(void);
int  display_modes(display_mode_t* out, int max);   /* those the display can show */
int  display_set_mode(int w, int h);                /* 0, or -1 */

/* Any PC: the sizes the firmware offers (UEFI GOP / BIOS VBE), which the
 * boot loader sets before Banana OS starts. The choice is written into the
 * installed disk's boot loader settings (0: from the next start; w = 0:
 * automatic); on the live CD it is chosen in the Banana Boot menu (-2);
 * -1 if the disk could not be written, -3 if the firmware does not offer that size. */
int  display_boot_modes(display_mode_t* out, int max);
int  display_set_boot_mode(int w, int h);

#endif
