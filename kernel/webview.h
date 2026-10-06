#ifndef WEBVIEW_H
#define WEBVIEW_H

#include "types.h"
#include "../sdk/include/banana_api.h"

/*
 * Web views: the browser's engine for apps (banana_api_t web_*, version 5).
 * One kernel task ("webview") drives every view: it loads pages, runs
 * their timers, carries out the clicks and keys the apps pass on, and
 * renders each view into a picture of its own that the app copies out
 * with web_draw(). The calls below are made from the apps' tasks; owner
 * is the calling app's id (a view belongs to one app and is closed with it).
 */

int  webview_open(int owner, int w, int h);                       /* id, or -1 */
void webview_close(int owner, int id);
void webview_close_owner(int owner);                              /* the app exited */
int  webview_load(int owner, int id, const char* url);
int  webview_load_html(int owner, int id, const char* html, const char* base_url);
void webview_resize(int owner, int id, int w, int h);
int  webview_poll(int owner, int id);                             /* BANANA_WEB_* */
void webview_draw(int owner, int id, unsigned int* px, int stride, int x, int y, int w, int h);
void webview_event(int owner, int id, const banana_event_t* ev);
void webview_scroll(int owner, int id, int dy);
void webview_go(int owner, int id, int delta);
int  webview_info(int owner, int id, char* title, int tcap, char* url, int ucap);
int  webview_eval(int owner, int id, const char* js, char* out, int cap);
int  webview_message(int owner, int id, char* out, int cap);
int  webview_post(int owner, int id, const char* msg);

#endif
