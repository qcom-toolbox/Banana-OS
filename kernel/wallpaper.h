#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "types.h"
#include "image.h"

/*
 * Desktop wallpaper state: one of the built-in presets (baked into the
 * kernel, see wallpaper_data.h) or a user image file from the
 * filesystem (PNG/JPEG/BMP/GIF, e.g. downloaded with wget into
 * ~/Pictures). The choice is saved in /etc/wallpaper, so it survives a
 * reboot on an installed disk.
 */

#define WALLPAPER_CONFIG "/etc/wallpaper"
#define WALLPAPER_DIR    "/home/banana/Pictures"
#define WALLPAPER_DEFAULT_PRESET 8     /* Azure Flow */

typedef struct {
    const char*    name;
    const char*    file;        /* source image in assets/wallpapers */
    uint32_t       base;        /* representative colour (swatch) */
    const uint8_t* pixels;      /* WALLPAPER_IMG_W x WALLPAPER_IMG_H RGB888 */
} wallpaper_preset_t;

int                       wallpaper_preset_count(void);
const wallpaper_preset_t* wallpaper_preset(int i);
/* by (case-insensitive) name, or 1-based number; -1 if none */
int                       wallpaper_find_preset(const char* s);

int          wallpaper_current_preset(void);     /* -1 when a file is in use */
const char*  wallpaper_current_file(void);       /* "" when a preset is in use */
image_mode_t wallpaper_current_mode(void);
/* bumped on every change: the GUI re-renders its cached desktop when it moves */
uint32_t     wallpaper_generation(void);

/* Renders the current wallpaper as XRGB8888 into dst (w x h, `stride`
 * pixels per row). */
void wallpaper_render(uint32_t* dst, int w, int h, int stride);

/* Switch wallpaper; both save the choice to /etc/wallpaper. */
void wallpaper_set_preset(int i);
/* Decodes and prepares the image now (can take a moment for big photos).
 * Returns 0, or -1 with the reason in err (the current wallpaper stays). */
int  wallpaper_set_file(const char* path, image_mode_t mode, char* err, uint32_t errlen);

/* Applies /etc/wallpaper once, the first time the desktop starts. */
void wallpaper_load_config(void);

/* 1 if the file name ends in an image extension we can decode */
int  wallpaper_is_image_name(const char* name);

#endif
