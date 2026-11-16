/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EDID_H
#define EDID_H
/* A monitor's EDID (the 128-byte base block, version 1.3 / 1.4): the modes
 * it says it takes - its detailed timings (the first one the preferred,
 * native mode), the established timings and the standard timings, the last
 * two as the VESA DMT ones. Display drivers read the block (DDC). */
#include "types.h"

#define EDID_NHSYNC    1u     /* negative sync polarity */
#define EDID_NVSYNC    2u
#define EDID_INTERLACE 4u
#define EDID_PREFERRED 8u     /* the monitor's preferred (native) mode */

typedef struct {
    uint32_t clock;           /* pixel clock, kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    uint8_t  flags;           /* EDID_* */
    uint8_t  refresh;         /* Hz, rounded */
} edid_mode_t;

/* 1 if edid is a valid base block (header, checksum) */
int  edid_valid(const uint8_t* edid);
/* the modes it lists, the preferred first, no two of the same size and
 * refresh: how many (at most max) */
int  edid_modes(const uint8_t* edid, edid_mode_t* out, int max);
/* the monitor's name (its descriptor), or its 3-letter maker and product code */
void edid_name(const uint8_t* edid, char* out, int cap);
/* a VESA DMT mode of w x h at hz (60 if 0): 0, or -1 if the table has none */
int  edid_dmt(int w, int h, int hz, edid_mode_t* out);

#endif
