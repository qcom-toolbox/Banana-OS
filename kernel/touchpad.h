#ifndef TOUCHPAD_H
#define TOUCHPAD_H

#include "types.h"

/*
 * Touchpad gestures, shared by the drivers that see fingers (Synaptics
 * PS/2 in absolute mode, I2C HID precision touchpads): they report what
 * is on the pad, this turns it into pointer motion, clicks and scrolling
 * (through mouse_inject, like a USB mouse):
 *
 *   one finger moves        the pointer (faster for quick moves)
 *   tap                     left click; tap, then touch again and move: drag
 *   two-finger tap          right click        three-finger tap: middle click
 *   two fingers moving      scroll
 *   ClickPad press          left click; right click with two fingers down
 *                           or in the bottom-right corner
 */

typedef struct {
    /* set by the driver */
    int xmin, xmax, ymin, ymax;     /* the pad's area, y growing downwards */
    int upm;                        /* units per millimetre */
    int clickpad;                   /* the whole pad is the (one) button */

    /* state */
    int      fingers, maxf, valid, x, y, sx, sy, moved, palm, had_button;
    uint32_t start_ms, tapped_ms, freeze_until;
    int      tap_state;             /* 0, TAPPED (button down, waiting), DRAG */
    int      pad_prev, cbtn;        /* ClickPad: the button its press became */
    int      scroll_acc, fx, fy;    /* fractions carried to the next report */
    int      out;                   /* the buttons last sent */
} touchpad_t;

#define TPF_PALM 1                  /* a palm (or something big) is on the pad */
#define TPF_JUMP 2                  /* the position is another finger's now: no motion */

/* one report: the number of fingers (0 = none), the position of the one
 * that moves the pointer, the buttons (bit 0 left / the ClickPad's press,
 * bit 1 right, bit 2 middle) and TPF_* flags */
void tp_frame(touchpad_t* t, int fingers, int x, int y, int buttons, int flags);
/* called often (each mouse_read): ends a tap that was not a drag */
void tp_tick(touchpad_t* t);

/* the user's choices (Settings > Mouse) */
extern int tp_tap_to_click;         /* default 1 */
extern int tp_natural_scroll;       /* default 1: the page follows the fingers */

#endif
