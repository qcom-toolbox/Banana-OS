/* clock - an analog + digital clock in a resizable window: math.h for
 * the hands, BANANA_EV_RESIZE to scale the face with the window */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <banana.h>

static void hand(bwin_t* w, int cx, int cy, double turns, double len, unsigned int c, int thick) {
    double a = turns * 2 * M_PI;
    int x = cx + (int)(sin(a) * len), y = cy - (int)(cos(a) * len);
    for (int d = -thick; d <= thick; d++) {
        bwin_line(w, cx + d, cy, x + d, y, c);
        bwin_line(w, cx, cy + d, x, y + d, c);
    }
}

static void draw(bwin_t* w, const banana_time_t* t) {
    static const char* const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    int text_h = 56;
    int r = (w->w < w->h - text_h ? w->w : w->h - text_h) / 2 - 12;
    if (r < 20) r = 20;
    int cx = w->w / 2, cy = 10 + r + 6;
    bwin_clear(w, 0x1D232C);
    bwin_fill_circle(w, cx, cy, r + 6, 0x384562);
    bwin_fill_circle(w, cx, cy, r, 0xFDF6D8);
    for (int i = 0; i < 60; i++) {
        double a = i * 2 * M_PI / 60;
        double inner = i % 5 ? r * 0.91 : r * 0.82, outer = r * 0.96;
        bwin_line(w, cx + (int)(sin(a) * inner), cy - (int)(cos(a) * inner),
                  cx + (int)(sin(a) * outer), cy - (int)(cos(a) * outer), i % 5 ? 0x808890 : 0x202830);
    }
    double s = t->second, m = t->minute + s / 60.0, h = (t->hour % 12) + m / 60.0;
    int thick = r > 120 ? 2 : 1;
    hand(w, cx, cy, h / 12.0, r * 0.55, 0x202830, thick + 1);
    hand(w, cx, cy, m / 60.0, r * 0.80, 0x202830, thick);
    hand(w, cx, cy, s / 60.0, r * 0.88, 0xE04040, 0);
    bwin_fill_circle(w, cx, cy, r / 20 + 2, 0xE04040);

    char str[40];
    snprintf(str, sizeof(str), "%02d:%02d:%02d", t->hour, t->minute, t->second);
    int scale = w->w >= 320 ? 3 : 2;
    bwin_text_scaled(w, (w->w - 8 * 8 * scale) / 2, w->h - text_h + 4, scale, str, 0xF4D35E, BANANA_TRANSPARENT);
    snprintf(str, sizeof(str), "%s %04d-%02d-%02d", days[t->weekday % 7], t->year, t->month, t->day);
    bwin_text(w, (w->w - (int)strlen(str) * 8) / 2, w->h - 14, str, 0xAAB6C6, BANANA_TRANSPARENT);
    bwin_update(w);
}

int main(void) {
    bwin_t w;
    if (bwin_open(&w, "Clock", 240, 290) != 0) {
        printf("clock: the desktop is not running - start it with `startx`\n");
        return 1;
    }
    bwin_resizable(&w, 160, 200);        /* drag the corner, or double-click the title */
    int last = -1;
    for (;;) {
        banana_event_t ev;
        int redraw = 0;
        while (bwin_event(&w, &ev)) {
            if (ev.type == BANANA_EV_CLOSE || (ev.type == BANANA_EV_KEY && ev.key == 27)) { bwin_close(&w); return 0; }
            if (ev.type == BANANA_EV_RESIZE) redraw = 1;
        }
        banana_time_t t;
        banana_time(&t);
        if (t.second != last || redraw) {
            last = t.second;
            draw(&w, &t);
        }
        banana_sleep(50);
    }
}
