/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

/* Runtime image decoding: PNG, JPEG, BMP, GIF (stb_image), static lossy and
 * lossless WebP (libwebp), and resampling for browser images and wallpapers. */

typedef struct {
    uint8_t* rgb;     /* RGB888, row 0 first */
    int      w, h;
} image_t;

typedef enum {
    IMAGE_FILL = 0,   /* scale to cover the screen, crop the overflow (default) */
    IMAGE_FIT,        /* scale to fit entirely, letterbox with a solid colour */
    IMAGE_STRETCH,    /* ignore aspect ratio */
    IMAGE_CENTER,     /* no scaling; centered, cropped if bigger */
    IMAGE_MODE_COUNT
} image_mode_t;

const char* image_mode_name(image_mode_t m);
int         image_mode_parse(const char* s, image_mode_t* out);   /* 1 = ok */

/* "PNG", "JPEG", "BMP", "GIF", "WebP" or NULL, from magic bytes */
const char* image_format(const uint8_t* data, uint32_t len);

/* Decodes into out->rgb (kmalloc'd). Returns 0, or -1 with a reason in err. */
int  image_decode(const uint8_t* data, uint32_t len, image_t* out, char* err, uint32_t errlen);
void image_free(image_t* img);
#ifdef BANANA_USER_WORKER
/* the same, RGBA8888 in img->rgb (4 bytes a pixel): the web engine's images */
int  image_decode_rgba(const uint8_t* data, uint32_t len, image_t* out, char* err, uint32_t errlen);
#endif

/* Renders `src` into a dw x dh XRGB8888 buffer per `mode`. */
void image_render(const image_t* src, uint32_t* dst, int dw, int dh, image_mode_t mode, uint32_t bg);

#endif
