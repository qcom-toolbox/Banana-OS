#ifndef BROWSER_H
#define BROWSER_H

#include "types.h"
#include "fb.h"

/*
 * "Browser": the desktop's web browser window (kernel/gui.c draws it and
 * routes the mouse to it). Pages are loaded, laid out and scripted by
 * the web engine (web/) in the browser's own task, which has a big stack;
 * the GUI only ever paints the last rendered frame.
 */

/* the keyboard focus value gui_focused_vt() returns while the browser is in front */
#define BROWSER_VT 1000

void browser_open(const char* url);     /* NULL: keep the current page (or the home page) */
void browser_close(void);
int  browser_is_open(void);

void browser_draw(const fb_info_t* fi);
int  browser_contains(int mx, int my);
/* the current tab is loading (the desktop shows a busy cursor over it) */
int  browser_busy(void);
void browser_wheel(int mx, int my, int dz);   /* scrolls the page (+ = down) */
void browser_click(int mx, int my);
void browser_mouse(int mx, int my, int left);
uint32_t browser_signature(void);
/* Ctrl+V / right-click: the clipboard into the focused field or the address bar */
void browser_paste(void);
void browser_fkey(int k);          /* F5 reload (KEYF_*) */

/* for web views (webview.c): the browser's network, cookies and image decoding */
struct page_env;
struct page_env* browser_env(void);
uint32_t browser_page_mem(void);
void     browser_cookies_save(void);
int      browser_fetch_media(const char* url, char** data, uint32_t* len, char* err, int ecap);
int      browser_fetch_document(const char* url, const char* post, uint32_t post_len,
                                char** data, uint32_t* len, char* final_url, int fcap);
/* right-click: the menu (open / save a link, back, reload, copy, paste, save page...) */
void browser_rclick(int mx, int my);

/* provided by gui.c: 1 while the browser window is the front window */
int  gui_browser_focused(void);

/* sets up the script engine's clock (kernel_main) */
void web_init(void);

#endif
