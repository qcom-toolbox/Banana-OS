#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"

/* Screen resolution (Settings > Screen): changeable on QEMU / Bochs /
 * VirtualBox displays (the VBE DISPI interface); elsewhere the boot mode. */

typedef struct { int w, h; } display_mode_t;

int  display_can_change(void);
int  display_modes(display_mode_t* out, int max);   /* those the display can show */
int  display_set_mode(int w, int h);                /* 0, or -1 */

#endif
