/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "appwin.h"
#include "mouse_capture.h"
#include "../../sdk/include/banana.h"
const banana_api_t* __banana;
static int api_calls;
static int fake_capture(int win,int capture) { api_calls++;return win==3 && capture==1?0:-1; }
static int fake_captured(int win) { api_calls++;return win==3; }
static int failures,checks;static uint32_t escape_sequence;static int escape_held;
static void report(const char* label,int ok,const char* note) {
    checks++;failures+=!ok;printf("%s %s%s%s\n",ok?"ok  ":"FAIL",label,*note?" - ":"",note);
}
uint32_t theme_color(uint32_t c) { return c; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
void* app_mem_alloc(int owner,size_t n) { (void)owner;return malloc(n); }
void app_mem_free(int owner,void* p) { (void)owner;free(p); }
void memset32(void* dst,uint32_t value,size_t n) { uint32_t* p=dst;while(n--)*p++=value; }
size_t kstrlcpy(char* dst,const char* src,size_t size) { size_t n=strlen(src);if(size){size_t c=n<size-1?n:size-1;memcpy(dst,src,c);dst[c]=0;}return n; }
const fb_info_t* fb_info(void) { static fb_info_t f={.width=1280,.height=720};return &f; }
uint32_t keyboard_escape_sequence(void) { return escape_sequence; }
int keyboard_held_char(int c) { return c==27 && escape_held; }
static void drain(int win,int owner) { banana_event_t ev;while(appwin_event(win,owner,&ev)){} }
int main(void) {
    banana_api_t api={0};__banana=&api;bwin_t window={.id=3};
    api.size=__builtin_offsetof(banana_api_t,win_capture_mouse);
    report("SDK capture fails safely on an older system",bwin_capture_mouse(&window,1)<0 && !api_calls,"");
    report("SDK query is safe on an older system",!bwin_mouse_captured(&window) && !api_calls,"");
    api.size=sizeof(api);api.win_capture_mouse=fake_capture;api.win_mouse_captured=fake_captured;
    report("SDK forwards capture through the new call table",!bwin_capture_mouse(&window,1) && api_calls==1,"");
    report("SDK queries the active capture",bwin_mouse_captured(&window)==1 && api_calls==2,"");
    window.id=-1;report("SDK rejects a closed window",bwin_capture_mouse(&window,1)<0 && !bwin_mouse_captured(&window) && api_calls==2,"");
    mouse_capture_tests(report);
    int a=appwin_open(7,"Game",320,200);appwin_focus(1);
    report("actual window captures its mouse",a>=0 && !appwin_capture_mouse(a,7,1) && appwin_mouse_captured(a,7)==1,"");
    report("window ownership is checked",appwin_capture_mouse(a,8,0)<0 && appwin_mouse_captured(a,7)==1,"");drain(a,7);
    mouse_state_t m={.dx=4,.dy=2};for(int i=0;i<100;i++)appwin_capture_input(&m,1280,720);
    banana_event_t ev;int found=appwin_event(a,7,&ev);
    report("relative event coalescing keeps every delta",found && ev.type==BANANA_EV_MOUSE_REL && ev.x==400 && ev.y==-200 && !appwin_event(a,7,&ev),"");
    m=(mouse_state_t){.btn_left=1,.btn_right=1,.btn_middle=1,.dz=2};appwin_capture_input(&m,1280,720);
    int downs=0,wheel=0;while(appwin_event(a,7,&ev)){if(ev.type==BANANA_EV_MOUSE_DOWN)downs|=1<<(ev.button-1);if(ev.type==BANANA_EV_WHEEL && ev.y==2)wheel++;}
    report("all three held buttons and wheel reach the game",downs==7 && wheel==1,"");
    appwin_focus(0);int ups=0,lost=0;while(appwin_event(a,7,&ev)){if(ev.type==BANANA_EV_MOUSE_UP)ups|=1<<(ev.button-1);if(ev.type==BANANA_EV_CAPTURE && !ev.x)lost++;}
    report("focus loss releases buttons and capture",ups==7 && lost==1 && !appwin_capture_pending(),"");
    report("background capture is denied",appwin_capture_mouse(a,7,1)<0,"");appwin_focus(1);appwin_capture_mouse(a,7,1);drain(a,7);
    appwin_key(27);appwin_key('[');appwin_key('A');
    report("arrow-key Escape sequence does not release capture",appwin_capture_active(),"");
    escape_sequence++;
    report("physical Escape releases independently of app input",!appwin_capture_active(),"");
    escape_held=1;report("holding Escape prevents immediate recapture",appwin_capture_mouse(a,7,1)<0,"");escape_held=0;
    appwin_capture_mouse(a,7,1);drain(a,7);for(int i=0;i<100;i++)appwin_key('x');escape_sequence++;appwin_capture_active();lost=0;
    while(appwin_event(a,7,&ev))if(ev.type==BANANA_EV_CAPTURE && !ev.x)lost++;
    report("capture loss is delivered even when the queue is full",lost==1,"");
    appwin_capture_mouse(a,7,1);appwin_minimize(a,1);
    report("minimize releases capture",!appwin_capture_pending(),"");appwin_activate(a);
    appwin_capture_mouse(a,7,1);appwin_set_hidden(a,1);
    report("virtual desktop hide releases capture",!appwin_capture_pending(),"");appwin_set_hidden(a,0);
    appwin_capture_mouse(a,7,1);int b=appwin_open(8,"Other app",160,100);
    report("opening another window releases capture",b>=0 && !appwin_capture_pending(),"");
    appwin_activate(a);appwin_capture_mouse(a,7,1);appwin_activate(b);
    report("switching app windows releases capture",!appwin_capture_pending(),"");
    appwin_activate(a);appwin_capture_mouse(a,7,1);appwin_request_close(a);
    report("close request releases capture",!appwin_capture_pending(),"");
    appwin_capture_mouse(a,7,1);appwin_close_owner(7);
    report("app exit releases capture",!appwin_capture_pending(),"");appwin_close_owner(8);
    printf("Mouse capture: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
