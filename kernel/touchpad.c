/*
 * Touchpad gestures (see touchpad.h): fingers -> pointer, clicks, scroll.
 */
#include "touchpad.h"
#include "timer.h"
#include "usb.h"

int tp_tap_to_click = 1;
int tp_natural_scroll = 1;

#define TAP_MS        180     /* a touch shorter than this, that did not move, is a tap */
#define TAP_MOVE_MM   3       /* ... and stayed within this */
#define DRAG_MS       200     /* after a tap: touch again within this to drag */
#define FREEZE_MS     120     /* no motion just after a ClickPad press/release (the finger rolls) */
#define SCROLL_MM10   25      /* scroll: one wheel notch per 2.5 mm */
#define CORNER_X_PCT  58      /* ClickPad right button: right of this... */
#define CORNER_Y_PCT  80      /* ... and below this (percent of the pad) */

enum { TS_IDLE = 0, TS_TAPPED, TS_DRAG };

static int iabs(int v) { return v < 0 ? -v : v; }

static int held(const touchpad_t* t) { return t->tap_state != TS_IDLE ? 1 : 0; }

/* sends motion and the buttons: what the pad's buttons say, plus the
 * left button of a tap / drag */
static void send(touchpad_t* t, int dx, int dy_down, int pad_buttons) {
    t->out = pad_buttons | held(t);
    mouse_inject(dx, -dy_down, t->out);        /* (PS/2 convention: + is up) */
}

/* a whole click of `bit`, now */
static void click(touchpad_t* t, int bit) {
    mouse_inject(0, 0, t->out | bit);
    mouse_inject(0, 0, t->out);
}

/* device units -> pixels, faster for quick moves: 3 px/mm when slow,
 * up to 9 px/mm (and Settings > Mouse > pointer speed on top) */
static int to_px(touchpad_t* t, int d, int speed_q8, int* frac) {
    int upm = t->upm > 0 ? t->upm : 40;
    int mm_q8 = d * 256 / upm;
    int gain_q8 = 768 + (speed_q8 < 384 ? speed_q8 : 384) * 4;
    int v = mm_q8 * gain_q8 + *frac;           /* 1/65536 px */
    int px = v / 65536;
    *frac = v - px * 65536;
    return px;
}

void tp_frame(touchpad_t* t, int fingers, int x, int y, int buttons, int flags) {
    uint32_t now = timer_ms();
    if (flags & TPF_PALM) { t->palm = 1; fingers = 0; }
    int upm = t->upm > 0 ? t->upm : 40;

    /* the ClickPad's press: which button it is, decided when it goes down */
    int pad = buttons & 6;
    if (buttons & 1) {
        if (!(t->pad_prev & 1)) {
            if (!t->clickpad) t->cbtn = 1;
            else if (fingers >= 3) t->cbtn = 4;
            else if (fingers == 2) t->cbtn = 2;
            else if (fingers == 1 && t->xmax > t->xmin &&
                     x > t->xmin + (t->xmax - t->xmin) * CORNER_X_PCT / 100 &&
                     y > t->ymin + (t->ymax - t->ymin) * CORNER_Y_PCT / 100) t->cbtn = 2;
            else t->cbtn = 1;
            if (t->clickpad) t->freeze_until = now + FREEZE_MS;
            if (t->tap_state == TS_TAPPED) t->tap_state = TS_IDLE;   /* a real click instead */
        }
        pad |= t->cbtn;
    } else if (t->pad_prev & 1) {
        if (t->clickpad) t->freeze_until = now + FREEZE_MS;
    }
    t->pad_prev = buttons;
    if (pad) t->had_button = 1;

    int dx = 0, dy = 0, wheel = 0;
    if (fingers > 0 && t->fingers == 0) {
        /* a new touch */
        t->start_ms = now;
        t->sx = x; t->sy = y;
        t->maxf = fingers;
        t->moved = 0;
        t->palm = 0;
        t->had_button = pad != 0;
        t->scroll_acc = 0;
        t->valid = 0;
        if (t->tap_state == TS_TAPPED) {
            if (fingers == 1 && now - t->tapped_ms < DRAG_MS) t->tap_state = TS_DRAG;
            else { t->tap_state = TS_IDLE; send(t, 0, 0, pad); }
        }
    }
    if (fingers != t->fingers || (flags & TPF_JUMP)) t->valid = 0;   /* no jump when a finger comes or goes */
    if (fingers > t->maxf) t->maxf = fingers;

    if (fingers > 0) {
        if (iabs(x - t->sx) > TAP_MOVE_MM * upm || iabs(y - t->sy) > TAP_MOVE_MM * upm) t->moved = 1;
        if (t->valid) {
            int ux = x - t->x, uy = y - t->y;
            if (fingers == 1 || (pad && fingers == 2)) {
                /* the pointer (also: a finger pressing the ClickPad, another dragging) */
                if ((int32_t)(now - t->freeze_until) >= 0) {
                    int sp = (iabs(ux) > iabs(uy) ? iabs(ux) : iabs(uy)) * 256 / upm;
                    dx = to_px(t, ux, sp, &t->fx);
                    dy = to_px(t, uy, sp, &t->fy);
                }
            } else if (fingers == 2 && !pad) {
                /* two fingers: scroll */
                t->scroll_acc += uy * 10;
                int notch = SCROLL_MM10 * upm;
                while (t->scroll_acc >= notch) { t->scroll_acc -= notch; wheel += tp_natural_scroll ? -1 : 1; }
                while (t->scroll_acc <= -notch) { t->scroll_acc += notch; wheel += tp_natural_scroll ? 1 : -1; }
                t->moved = 1;
            }
        } else {
            t->fx = t->fy = 0;
        }
        t->x = x; t->y = y;
        t->valid = 1;
    }

    int was = t->fingers;
    t->fingers = fingers;

    if (fingers == 0 && was > 0) {
        /* the touch ended: was it a tap? */
        int tap = tp_tap_to_click && !t->moved && !t->palm && !t->had_button &&
                  now - t->start_ms < TAP_MS;
        if (t->tap_state == TS_DRAG) {
            t->tap_state = TS_IDLE;
            send(t, dx, dy, pad);                       /* the drag ends */
            dx = dy = 0;
            if (tap && t->maxf == 1) click(t, 1);       /* tap, tap: a double click */
        } else if (tap) {
            if (t->maxf == 1) {
                t->tap_state = TS_TAPPED;               /* down now, up when no drag follows */
                t->tapped_ms = now;
            } else {
                send(t, dx, dy, pad);
                dx = dy = 0;
                click(t, t->maxf == 2 ? 2 : 4);
            }
        }
    }

    if (dx || dy || (pad | held(t)) != t->out) send(t, dx, dy, pad);
    if (wheel) mouse_inject_wheel(wheel);
}

void tp_tick(touchpad_t* t) {
    if (t->tap_state == TS_TAPPED && t->fingers == 0 && timer_ms() - t->tapped_ms >= DRAG_MS) {
        t->tap_state = TS_IDLE;
        send(t, 0, 0, t->out & ~1 & 6);
    }
}
