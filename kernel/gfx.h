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

/* UI text in DejaVu Sans Mono (1, the default) or the classic 8x8 bitmap font */
void gfx_set_smooth_text(int on);
int  gfx_smooth_text(void);

/* Text size (Settings > Fonts): 0 normal (8x8 cells) .. 3 largest. Terminal
 * windows and Notepad draw their text in cells of this size (in the
 * interface font, or the classic one scaled up); desktop icon names grow
 * with it too. */
#define GFX_TEXT_SIZES 4
void gfx_set_text_size(int size);
int  gfx_text_size(void);
const char* gfx_text_size_name(int size);
int  gfx_cell_w(void);
int  gfx_cell_h(void);
/* one terminal / Notepad cell: c is ASCII or Latin-1 */
void gfx_draw_cell_char(int x, int y, char c, uint32_t fg, uint32_t bg);
int  gfx_label_px(void);

/* a desktop icon's name: centred on cx, wrapped onto up to two lines max_w wide,
 * straight on the wallpaper with a dark shadow (or on highlight, if not 0);
 * returns its height */
int  gfx_draw_label(int cx, int y, int max_w, const char* s, uint32_t fg, uint32_t highlight);

#endif

