/* clock - an analog + digital clock window, redrawn every second
 * (integer-only: the hand positions come from a sine table) */
#include <stdio.h>
#include <banana.h>

#define W 240
#define H 290
#define CX 120
#define CY 120
#define R  100

/* sin(i * 6 degrees) * 1000, i = 0..15 (a quarter turn) */
static const int SIN[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };

static int isin(int i) {          /* i in 60ths of a turn */
    i = ((i % 60) + 60) % 60;
    if (i <= 15) return SIN[i];
    if (i <= 30) return SIN[30 - i];
    if (i <= 45) return -SIN[i - 30];
    return -SIN[60 - i];
}
static int icos(int i) { return isin(i + 15); }

static void hand(bwin_t* w, int pos60, int len, unsigned int c, int thick) {
    int x = CX + isin(pos60) * len / 1000, y = CY - icos(pos60) * len / 1000;
    for (int d = -thick; d <= thick; d++) {
        bwin_line(w, CX + d, CY, x + d, y, c);
        bwin_line(w, CX, CY + d, x, y + d, c);
    }
}

int main(void) {
    bwin_t w;
    if (bwin_open(&w, "Clock", W, H) != 0) {
        printf("clock: the desktop is not running - start it with `startx`\n");
        return 1;
    }
    static const char* const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    int last = -1;
    for (;;) {
        banana_event_t ev;
        while (bwin_event(&w, &ev))
            if (ev.type == BANANA_EV_CLOSE || (ev.type == BANANA_EV_KEY && ev.key == 27)) { bwin_close(&w); return 0; }
        banana_time_t t;
        banana_time(&t);
        if (t.second == last) { banana_sleep(100); continue; }
        last = t.second;

        bwin_clear(&w, 0x1D232C);
        bwin_fill_circle(&w, CX, CY, R + 6, 0x384562);
        bwin_fill_circle(&w, CX, CY, R, 0xFDF6D8);
        for (int i = 0; i < 60; i++) {
            int outer = R - 4, inner = i % 5 ? R - 9 : R - 18;
            bwin_line(&w, CX + isin(i) * inner / 1000, CY - icos(i) * inner / 1000,
                      CX + isin(i) * outer / 1000, CY - icos(i) * outer / 1000, i % 5 ? 0x808890 : 0x202830);
        }
        hand(&w, (t.hour % 12) * 5 + t.minute / 12, 55, 0x202830, 2);
        hand(&w, t.minute, 80, 0x202830, 1);
        hand(&w, t.second, 88, 0xE04040, 0);
        bwin_fill_circle(&w, CX, CY, 5, 0xE04040);

        char s[32];
        snprintf(s, sizeof(s), "%02d:%02d:%02d", t.hour, t.minute, t.second);
        bwin_text_scaled(&w, (W - 8 * 8 * 3) / 2, 236, 3, s, 0xF4D35E, BANANA_TRANSPARENT);
        snprintf(s, sizeof(s), "%s %04d-%02d-%02d", days[t.weekday % 7], t.year, t.month, t.day);
        bwin_text(&w, (W - (int)__builtin_strlen(s) * 8) / 2, 268, s, 0xAAB6C6, BANANA_TRANSPARENT);
        bwin_update(&w);
    }
}
