#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

/* Runtime image decoding (PNG, JPEG incl. progressive, BMP, GIF - via
 * the vendored stb_image) and resampling, for user wallpapers. */

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

/* "PNG", "JPEG", "BMP", "GIF" or NULL, from the file's magic bytes */
const char* image_format(const uint8_t* data, uint32_t len);

/* Decodes into out->rgb (kmalloc'd). Returns 0, or -1 with a reason in err. */
int  image_decode(const uint8_t* data, uint32_t len, image_t* out, char* err, uint32_t errlen);
void image_free(image_t* img);

/* Renders `src` into a dw x dh XRGB8888 buffer per `mode`. */
void image_render(const image_t* src, uint32_t* dst, int dw, int dh, image_mode_t mode, uint32_t bg);

#endif
