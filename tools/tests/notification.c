/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of the desktop notification service with mock clock and graphics */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#define THEME_H
#define TC(c) (c)
size_t kstrlcpy(char* d,const char* s,size_t n) {size_t z=strlen(s);if(n){size_t k=z<n-1?z:n-1;memcpy(d,s,k);d[k]=0;}return z;}
int ksnprintf(char* b,size_t n,const char* f,...) {va_list a;va_start(a,f);int r=vsnprintf(b,n,f,a);va_end(a);return r;}

#include "../../web/test/shim/types.h"

/* Mock spinlock for host test */
#define SPINLOCK_H
typedef struct { volatile int v; } spinlock_t;
#define SPINLOCK_INIT { 0 }
static inline void spin_lock(spinlock_t* l) { (void)l; }
static inline void spin_unlock(spinlock_t* l) { (void)l; }
static inline uintptr_t spin_lock_irq(spinlock_t* l) { (void)l; return 0; }
static inline void spin_unlock_irq(spinlock_t* l, uintptr_t f) { (void)l; (void)f; }

/* Mock timer */
#define TIMER_H
static uint32_t g_mock_ms = 1000;
uint32_t timer_ms(void) { return g_mock_ms; }

/* Mock gfx */
#define GFX_H
static int g_fill_rect_calls = 0;
static int g_draw_text_calls = 0;
void gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    (void)x; (void)y; (void)w; (void)h; (void)rgb;
    g_fill_rect_calls++;
}
void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg;
    g_draw_text_calls++;
}

#include "../../kernel/notification.c"

static int g_last_action = -1;
static void* g_last_action_udata = NULL;
static void test_action_handler(int act, void* udata) {
    g_last_action = act;
    g_last_action_udata = udata;
}

int main(void) {
    notify_init();
    notify_set_action_callback(test_action_handler, (void*)0x1234);

    assert(notify_count() == 0);
    assert(!notify_has_active_toast());

    /* 1. Basic posting */
    int id1 = notify_post("AppA", "Title 1", "Body 1", 101);
    assert(id1 == 1);
    assert(notify_count() == 1);
    assert(notify_has_active_toast());

    notify_item_t item;
    assert(notify_peek(0, &item) == 0);
    assert(item.id == 1);
    assert(strcmp(item.app, "AppA") == 0);
    assert(strcmp(item.title, "Title 1") == 0);
    assert(strcmp(item.body, "Body 1") == 0);
    assert(item.action_id == 101);
    assert(item.read == 0);

    /* 2. Rate limiting (max 3 burst within 2000 ms per app) */
    int id2 = notify_post("AppA", "Title 2", "Body 2", 102);
    int id3 = notify_post("AppA", "Title 3", "Body 3", 103);
    assert(id2 == 2);
    assert(id3 == 3);
    assert(notify_count() == 3);

    /* 4th notification in same timestamp should be rate-limited */
    int id4_dropped = notify_post("AppA", "Title 4", "Body 4", 104);
    assert(id4_dropped == 0);
    assert(notify_count() == 3);

    /* Different app is NOT rate-limited */
    int id4_appB = notify_post("AppB", "Title B", "Body B", 201);
    assert(id4_appB > 0);
    assert(notify_count() == 4);

    /* Advance clock past rate window (2000 ms) */
    g_mock_ms += 2001;
    int id5 = notify_post("AppA", "Title 5", "Body 5", 105);
    assert(id5 > 0);
    assert(notify_count() == 5);

    /* 3. Action triggering */
    g_last_action = -1;
    assert(notify_trigger_action(id1) == 0);
    assert(g_last_action == 101);
    assert(g_last_action_udata == (void*)0x1234);

    /* Check read flag */
    assert(notify_peek(4, &item) == 0); /* id1 is now oldest item at index 4 */
    assert(item.id == 1);
    assert(item.read == 1);

    /* Trigger nonexistent action */
    assert(notify_trigger_action(99999) == -1);

    /* 4. Active toast duration & polling */
    /* id5 was posted at mock_ms = 3001, toast expires at 3001 + 5000 = 8001 */
    assert(notify_has_active_toast());
    uint32_t rem = 0;
    assert(notify_get_active_toast(&item, &rem) == 1);
    assert(item.id == (uint32_t)id5);
    assert(rem == 5000);

    /* Advance time to 4000 (after 1s) */
    g_mock_ms += 1000;
    notify_poll();
    assert(notify_has_active_toast());
    assert(notify_get_active_toast(&item, &rem) == 1);
    assert(rem == 4000);

    /* Advance time past expiration */
    g_mock_ms += 4500;
    notify_poll();
    assert(!notify_has_active_toast());
    assert(notify_get_active_toast(&item, &rem) == 0);

    /* 5. History overflow & ring buffer bound (NOTIFY_HIST_MAX = 16) */
    notify_clear_history();
    assert(notify_count() == 0);
    for (int i = 0; i < 25; i++) {
        g_mock_ms += 1000; /* avoid rate limit */
        int post_id = notify_post("StressApp", "Title", "Text", i);
        assert(post_id > 0);
    }
    assert(notify_count() == NOTIFY_HIST_MAX);
    /* Peek newest (index 0) */
    assert(notify_peek(0, &item) == 0);
    assert(item.action_id == 24);
    /* Peek oldest (index 15) */
    assert(notify_peek(15, &item) == 0);
    assert(item.action_id == 25 - NOTIFY_HIST_MAX);

    /* 6. Toast click handling */
    fb_info_t fi = { .width = 1024, .height = 768, .pitch = 4096, .bpp = 32, .type = 1, .addr = 0 };
    /* Bottom-right, twelve pixels above the 28-pixel taskbar: y = 664. */
    /* Dismiss with click on 'x' */
    assert(notify_has_active_toast());
    assert(notify_click(&fi, 760, 25) == 0);    /* former top-right position */
    assert(notify_click(&fi, 760, 750) == 0);   /* taskbar remains clickable */
    assert(notify_has_active_toast());
    assert(notify_click(&fi, 752 + 260 - 10, 664 + 10) == 1);
    assert(!notify_has_active_toast());

    /* Post new toast and click body to trigger action */
    g_mock_ms += 2500;
    int id_act = notify_post("AppClick", "Click Me", "Test Action", 777);
    assert(id_act > 0);
    assert(notify_has_active_toast());
    g_last_action = -1;
    assert(notify_click(&fi, 760, 690) == 1);
    assert(g_last_action == 777);
    assert(!notify_has_active_toast());

    /* Draw toast verification */
    g_mock_ms += 2500;
    notify_post("AppDraw", "Draw Title", "Draw Body", 888);
    int prev_rects = g_fill_rect_calls;
    int prev_text = g_draw_text_calls;
    notify_draw_toast(&fi);
    assert(g_fill_rect_calls > prev_rects);
    assert(g_draw_text_calls > prev_text);

    /* Resolution changes re-anchor the same toast without entering the bar. */
    const unsigned sizes[][2] = {{640,480},{800,600},{1024,768},{1920,1080},{3840,2160},{260,92}};
    for (unsigned i=0; i<sizeof(sizes)/sizeof(sizes[0]); i++) {
        fi.width=sizes[i][0];fi.height=sizes[i][1];
        int x,y,w,h;
        assert(get_toast_rect(&fi,&x,&y,&w,&h));
        assert(x>=0 && y>=0 && x+w<=(int)fi.width);
        assert(y+h<=(int)fi.height-WIN_TASKBAR);
        if(fi.width>=284) assert(x+w==(int)fi.width-12);
        if(fi.height>=104) assert(y+h==(int)fi.height-WIN_TASKBAR-12);
        assert(notify_click(&fi,x,(int)fi.height-1)==0);
    }
    int no_fit_rects=g_fill_rect_calls,no_fit_text=g_draw_text_calls;
    fi.width=259;fi.height=480;notify_draw_toast(&fi);
    assert(notify_click(&fi,0,0)==0);
    fi.width=800;fi.height=91;notify_draw_toast(&fi);
    assert(notify_click(&fi,0,0)==0);
    assert(g_fill_rect_calls==no_fit_rects && g_draw_text_calls==no_fit_text);

    notify_init();
    g_mock_ms = 0xfffffff0u;
    char long_source[100]; memset(long_source, 'a', 99); long_source[99] = 0;
    for (int i = 0; i < 3; i++) assert(notify_post(long_source, "long", "source", -1) > 0);
    assert(notify_post(long_source, "long", "source", -1) == 0);
    g_mock_ms += NOTIFY_RATE_WINDOW_MS;
    assert(notify_post(long_source, "after wrap", "", -1) > 0);
    g_mock_ms += NOTIFY_TOAST_DURATION_MS;
    assert(!notify_has_active_toast());
    g_next_id = 0xffffffffu;
    assert(notify_post("wrap", "id", "", -1) == 1);
    puts("notification: all host unit tests passed successfully");
    return 0;
}
