#ifndef GFX_H
#define GFX_H

#include "types.h"

void gfx_init(void);
int  gfx_available(void);

void gfx_clear(uint32_t rgb);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);
void gfx_draw_char_scaled(int x, int y, int scale, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text_scaled(int x, int y, int scale, const char* s, uint32_t fg, uint32_t bg);
/* the dotted resize grip of a window whose bottom-right corner is (right, bottom) */
void gfx_draw_grip(int right, int bottom);

#endif

