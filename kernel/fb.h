#ifndef FB_H
#define FB_H

#include "types.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;   /* bytes per row */
    uint8_t  bpp;     /* bits per pixel */
    uint8_t  type;    /* 1 = RGB */
    uintptr_t addr;   /* framebuffer physical address (identity-mapped here) */
} fb_info_t;

int  fb_init_multiboot2(uint32_t mb2_info_addr);
int  fb_available(void);
const fb_info_t* fb_info(void);

void fb_putpixel(int x, int y, uint32_t rgb);
void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb);

/* Optional backbuffer to reduce flicker */
void fb_set_backbuffer(uint32_t* buf, uint32_t buf_width, uint32_t buf_height);
void fb_clear_backbuffer(void);
void fb_present(void);              /* copy backbuffer -> framebuffer */
void fb_putpixel_direct(int x, int y, uint32_t rgb); /* always to framebuffer */
/* copies one rectangle of the backbuffer to the screen */
void fb_present_rect(int x, int y, int w, int h);

/* The buffer drawing currently goes to: the backbuffer when one is set,
 * else the framebuffer itself. Returns NULL without a framebuffer. */
uint32_t* fb_target(int* stride_px, int* w, int* h);

/* Moves rows [top+dy, top+height) up by dy pixels and fills the freed
 * strip at the bottom (fast console scrolling). */
void fb_scroll_up(int top, int height, int dy, uint32_t fill);

#endif

