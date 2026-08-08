#ifndef WALLPAPER_DATA_H
#define WALLPAPER_DATA_H

#include "types.h"

/* Real wallpaper pixel data, generated from the PNGs in assets/wallpapers by
 * tools/gen_wallpaper_data.py (downsampled to WALLPAPER_IMG_W x
 * WALLPAPER_IMG_H, packed as RGB888, row 0 first). Regenerate that script
 * if the source PNGs change - see kernel/wallpaper_data.c's header comment.
 *
 * 100x75 was chosen because it divides the 800x600 desktop exactly (8x
 * upscale, no fractional scaling), keeping the embedded kernel image size
 * reasonable without needing a runtime PNG/DEFLATE decoder. */
#define WALLPAPER_IMG_W 100
#define WALLPAPER_IMG_H 75

extern const uint8_t wallpaper_midnight_blue[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_forest_night[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_graphite[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_royal_purple[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_sunset_grid[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_ocean_wave[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_cyber_mint[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_amber_mesh[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];

#endif
