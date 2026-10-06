#ifndef SVG_H
#define SVG_H

#include "types.h"
#include "arena.h"
#include "layout.h"

/* SVG images (web/svg.c): logos and icons, <img src=x.svg> and inline <svg> */

/* 1 if the data looks like SVG markup */
int svg_sniff(const uint8_t* data, uint32_t len);
/* draws the SVG into out (pixels + alpha in the arena), at want_w x want_h
 * (0: its own size); 0, or -1 if it is not usable SVG */
int svg_render(const char* src, uint32_t len, int want_w, int want_h, img_data_t* out, arena_t* A);

#endif
