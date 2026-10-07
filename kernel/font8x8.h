#ifndef FONT8X8_H
#define FONT8X8_H

#include "types.h"

/* Minimal 8x8 font: only ASCII 32..127 needed for UI */
extern const uint8_t font8x8_basic[128][8];
extern const uint8_t font8x8_latin1[96][8];   /* U+00A0..U+00FF */

#endif

