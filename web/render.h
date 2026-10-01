#ifndef RENDER_H
#define RENDER_H

#include "layout.h"

/* Paints a page's display list into a 32-bit pixel buffer: the viewport
 * (vx, vy, vw, vh) of `buf` shows the page scrolled by `scroll` pixels.
 * Everything is clipped to the viewport. */
void render_page(const layout_t* L, uint32_t* buf, int stride, int buf_w, int buf_h,
                 int vx, int vy, int vw, int vh, int scroll);

#endif
