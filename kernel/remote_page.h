/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REMOTE_PAGE_H
#define REMOTE_PAGE_H
#include "../web/page.h"
#include "../web/render.h"
page_t* remote_page_new(page_env_t*, uint32_t);
page_t* remote_page_new_libjs(page_env_t*, uint32_t);
void remote_page_free(page_t*);
void remote_page_load(page_t*, const char*, const char*, uint32_t, int);
void remote_page_update(page_t*, int);
void remote_page_click(page_t*, int, int);
int remote_page_key(page_t*, char);
int remote_page_key_live(page_t*, int, const char*, const char*, int, int);
int remote_page_tick(page_t*);
int remote_page_link_at(page_t*, int, int, char*, int);
void remote_page_set_files(page_t*, dom_node_t*, const char*);
int remote_page_eval(page_t*, const char*, char*, int);
void remote_page_post(page_t*, const char*);
int remote_page_frame_diagnostics(page_t*,char*,unsigned);
void remote_render(const layout_t*, uint32_t*, int, int, int, int, int, int, int, int);
int remote_text_pos(layout_t*, int, int, int*, int*);
uint32_t remote_text_range(layout_t*, int, int, int, int, char*, uint32_t);
void web_libjs_dom_tests(void (*report)(const char*,int,const char*));
void web_libjs_tests(void (*report)(const char*,int,const char*));
void web_isolation_tests(void (*report)(const char*,int,const char*));
/* a restricted worker decoding one media stream (kernel/media.c drives it
 * with the WW_MEDIA_* commands); NULL if none could start */
void* media_worker_start(void);
int  media_worker_call(void* w, unsigned int op, const int* args, const void* in, uint32_t n,
                       void* out, uint32_t cap, uint32_t* out_used);
void media_worker_stop(void* w);
/* Browser and SDK webviews use only remote proxies. Other trusted engine
 * selftests may still link the engine, but never parse network data here. */
#ifndef REMOTE_PAGE_IMPLEMENTATION
#define page_new remote_page_new
#define page_free remote_page_free
#define page_load remote_page_load
#define page_update remote_page_update
#define page_click remote_page_click
#define page_key remote_page_key
#define page_key_live remote_page_key_live
#define page_tick remote_page_tick
#define page_link_at remote_page_link_at
#define page_set_files remote_page_set_files
#define render_page remote_render
#define layout_text_pos remote_text_pos
#define layout_text_range remote_text_range
#endif
#endif
