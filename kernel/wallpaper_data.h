#ifndef WALLPAPER_DATA_H
#define WALLPAPER_DATA_H

#include "types.h"

/* Real wallpaper pixel data, generated from the images in assets/wallpapers
 * by tools/gen_wallpaper_data.py (downsampled to WALLPAPER_IMG_W x
 * WALLPAPER_IMG_H, packed as RGB888, row 0 first). Regenerate that script
 * if the source images change - see kernel/wallpaper_data.c's header
 * comment. PNG and JPEG sources are both supported.
 *
 * 200x150 was chosen because it divides the 800x600 desktop exactly (4x
 * upscale, no fractional scaling) while giving gui.c's bilinear upscale
 * (draw_wallpaper_bitmap) enough source detail to hold up for real photos,
 * without needing a runtime PNG/JPEG decoder. */
#define WALLPAPER_IMG_W 200
#define WALLPAPER_IMG_H 150

extern const uint8_t wallpaper_midnight_blue[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_forest_night[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_graphite[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_royal_purple[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_sunset_grid[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_ocean_wave[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_cyber_mint[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_amber_mesh[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_azure_flow[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];
extern const uint8_t wallpaper_plain_black[WALLPAPER_IMG_W * WALLPAPER_IMG_H * 3];

#endif
