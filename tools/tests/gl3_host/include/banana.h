/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tools/tests/gl3_host: the part of <banana.h> sdk/lib/gl3.c uses - a window
 * is its pixels (0x00RRGGBB, rows top first); updates are counted */
#ifndef GL3_HOST_BANANA_H
#define GL3_HOST_BANANA_H
typedef struct { int id, w, h; unsigned int* px; } bwin_t;
extern int g_updates;
static inline void bwin_update(bwin_t* w) { (void)w; g_updates++; }
#endif
