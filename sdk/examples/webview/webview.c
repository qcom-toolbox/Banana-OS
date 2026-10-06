/* A small browser built on the system's web view: back / forward / reload,
 * an address bar, and a start page that talks to the app (banana.postMessage
 * one way, bweb_post the other). */
#include <banana.h>
#include <string.h>
#include <stdio.h>

#define BAR_H 28

static const char* START_PAGE =
    "<html><head><title>Web view demo</title><style>"
    "body{font-family:sans-serif;background:#1d232c;color:#e8eef6;margin:24px}"
    "h1{color:#ffd23f}button{background:#ffd23f;color:#1d232c;border:0;border-radius:8px;padding:8px 16px}"
    "#reply{margin-top:12px;color:#8fe388}a{color:#7cc4ff}"
    "</style></head><body>"
    "<h1>Hello from a web view</h1>"
    "<p>This page runs inside an SDK app. The button sends a message to the app, "
    "which answers back.</p>"
    "<button id=b>Say hi to the app</button><div id=reply>(no answer yet)</div>"
    "<p>Type an address in the bar above, or try <a href='https://example.com'>example.com</a>.</p>"
    "<script>"
    "var n = 0;"
    "document.getElementById('b').onclick = function(){ banana.postMessage('hi #' + (++n)); };"
    "window.addEventListener('message', function(e){ document.getElementById('reply').textContent = e.data; });"
    "</script></body></html>";

static bwin_t win;
static int view = -1;
static char addr[1024] = "";
static int addr_focus;
static char status[160] = "";

static void draw_bar(void) {
    bwin_fill_rect(&win, 0, 0, win.w, BAR_H, 0x2b3442);
    bwin_button(&win, 4, 4, 24, 20, "<", 0);
    bwin_button(&win, 30, 4, 24, 20, ">", 0);
    bwin_button(&win, 56, 4, 24, 20, "R", 0);
    int ax = 86, aw = win.w - ax - 6;
    bwin_fill_rect(&win, ax, 4, aw, 20, addr_focus ? 0xffffff : 0xe8eef6);
    bwin_rect(&win, ax, 4, aw, 20, addr_focus ? 0x3060c0 : 0x808080);
    const char* shown = addr;                   /* the end of a long address */
    while (*shown && banana_font_width(BANANA_FONT_SANS, 12, shown) > aw - 10) shown++;
    bwin_font(&win, ax + 5, 7, BANANA_FONT_SANS, 12, shown, 0x000000);
    if (status[0]) {
        bwin_fill_rect(&win, 0, win.h - 16, win.w, 16, 0x2b3442);
        bwin_font(&win, 6, win.h - 15, BANANA_FONT_SANS, 11, status, 0xe8eef6);
    }
}

static int view_h(void) { return win.h - BAR_H - (status[0] ? 16 : 0); }

static void redraw(void) {
    bweb_draw(view, &win, 0, BAR_H);
    draw_bar();
    bwin_update(&win);
}

int main(void) {
    if (!bweb_available()) { printf("This Banana OS has no web views (it needs API version 5).\n"); return 1; }
    if (bwin_open(&win, "Web view", 720, 520) != 0) { printf("No desktop: run startx first.\n"); return 1; }
    bwin_resizable(&win, 320, 200);
    view = bweb_open(win.w, win.h - BAR_H);
    if (view < 0) { printf("Could not open a web view.\n"); return 1; }
    bweb_html(view, START_PAGE, "about:demo");
    redraw();
    for (;;) {
        banana_event_t ev;
        while (bwin_event(&win, &ev)) {
            if (ev.type == BANANA_EV_CLOSE) { bweb_close(view); return 0; }
            if (ev.type == BANANA_EV_RESIZE) { bweb_resize(view, win.w, view_h()); redraw(); continue; }
            if (ev.type == BANANA_EV_MOUSE_DOWN && ev.y < BAR_H) {
                addr_focus = 0;
                if (ev.x < 28) bweb_back(view);
                else if (ev.x < 54) bweb_forward(view);
                else if (ev.x < 80) bweb_reload(view);
                else addr_focus = 1;
                redraw();
                continue;
            }
            if (ev.type == BANANA_EV_KEY && addr_focus) {
                int n = (int)strlen(addr);
                if (ev.key == '\n') { addr_focus = 0; bweb_load(view, addr); }
                else if (ev.key == '\b') { if (n) addr[n - 1] = 0; }
                else if (ev.key == 27) addr_focus = 0;
                else if (ev.key >= 32 && ev.key < 127 && n < (int)sizeof(addr) - 1) { addr[n] = (char)ev.key; addr[n + 1] = 0; }
                redraw();
                continue;
            }
            if (ev.type == BANANA_EV_MOUSE_DOWN || ev.type == BANANA_EV_MOUSE_UP || ev.type == BANANA_EV_MOUSE_MOVE) {
                if (ev.type == BANANA_EV_MOUSE_DOWN) addr_focus = 0;
                ev.y -= BAR_H;                          /* view coordinates */
            }
            bweb_event(view, &ev);
        }
        int f = bweb_poll(view);
        if (f & BANANA_WEB_TITLE) {
            char title[128], url[1024];
            bweb_info(view, title, sizeof(title), url, sizeof(url));
            bwin_title(&win, title[0] ? title : "Web view");
            if (!addr_focus) snprintf(addr, sizeof(addr), "%s", url);
            f |= BANANA_WEB_DIRTY;
        }
        if (f & BANANA_WEB_MESSAGE) {
            char msg[256], reply[300];
            while (bweb_message(view, msg, sizeof(msg)) >= 0) {
                snprintf(status, sizeof(status), "The page says: %s", msg);
                snprintf(reply, sizeof(reply), "The app got \"%s\" and says hello back!", msg);
                bweb_post(view, reply);
            }
            bweb_resize(view, win.w, view_h());
            f |= BANANA_WEB_DIRTY;
        }
        int loading = (f & BANANA_WEB_LOADING) != 0;
        static int was_loading;
        if (loading != was_loading) {
            was_loading = loading;
            snprintf(status, sizeof(status), "%s", loading ? "Loading..." : "");
            bweb_resize(view, win.w, view_h());
            f |= BANANA_WEB_DIRTY;
        }
        if (f & BANANA_WEB_DIRTY) redraw();
        banana_sleep(10);
    }
}
