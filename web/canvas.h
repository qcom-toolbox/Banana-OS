/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WEB_CANVAS_H
#define WEB_CANVAS_H
/* <canvas> 2D contexts (canvas.c): isolated workers only */
#include "html.h"
#include "script.h"

struct page;
typedef struct canvas canvas_t;

#ifdef BANANA_USER_WORKER
canvas_t* canvas_of(struct page* p, dom_node_t* n, int create);
void canvas_attr_changed(struct page* p, dom_node_t* n);   /* width / height set */
void canvas_free_all(struct page* p) __attribute__((weak));
void canvas_install(interp_t* I);                          /* __cv() */
void canvas_put_rgba(struct page* p, dom_node_t* n, int w, int h, const uint8_t* rgba, int premultiplied);
uint8_t* canvas_element_rgba(struct page* p, dom_node_t* n, int* w, int* h);   /* img / video / canvas pixels */
void webgl_install(interp_t* I);                           /* __gl() (webgl.c) */
void webgl_free_all(struct page* p) __attribute__((weak));
#endif
dom_node_t* jsdom_node_of(value_t v);                       /* jsdom.c: the element a wrapper is */
#endif
