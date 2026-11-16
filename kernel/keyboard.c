/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "keyboard.h"
#include "latency.h"
#include "terminal.h"
#include "types.h"
#include "usb.h"
#include "task.h"
#include "serial.h"
#include "tty.h"
#include "../usb/usbcore.h"
#include "utf8.h"
#include "audio.h"

#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_CMD_PORT     0x64

/* US QWERTY scancode set 1 */
static const char sc_normal[128] = {
 0,  27, '1','2','3','4','5','6','7','8',
'9','0','-','=','\b','\t',
'q','w','e','r','t','y','u','i','o','p',
'[',']','\n', 0,
'a','s','d','f','g','h','j','k','l',';',
'\'','`', 0,'\\',
'z','x','c','v','b','n','m',',','.','/',
 0,'*', 0,' ', 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* F1-F10 */
 0, 0,
'7','8','9','-','4','5','6','+','1','2','3','0','.',
 0, 0, 0, 0, 0, 0,
};

static const char sc_shift[128] = {
 0, 27,'!','@','#','$','%','^','&','*',
'(',')','_','+','\b','\t',
'Q','W','E','R','T','Y','U','I','O','P',
'{','}','\n', 0,
'A','S','D','F','G','H','J','K','L',':',
'"','~', 0,'|',
'Z','X','C','V','B','N','M','<','>','?',
 0,'*', 0,' ', 0,
};

#include "kblayouts.h"
#include "kstring.h"

static int g_layout;             /* kb_layouts[]: 0 the US keyboard */

static int ch_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int strieq(const char* a, const char* b) {
    while (*a && *b) {
        if (ch_tolower((unsigned char)*a) != ch_tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}

static void kb_wait_write(void) {
    int t = 100000;
    while ((inb(KB_STATUS_PORT) & 0x02) && t--);
}
static void kb_wait_read(void) {
    int t = 100000;
    while (!(inb(KB_STATUS_PORT) & 0x01) && t--);
}

/* ── modifier state ─────────────────────────────────────────────── */
static int shift_held = 0;
static int ctrl_held  = 0;
static int alt_held   = 0;
static int altgr_held = 0;   /* right Alt */
static int caps_lock  = 0;
static int caps_down  = 0;   /* held: the keyboard repeats it, it toggles once */
static int g_from_usb = 0;   /* the scancode came from a USB keyboard (no PS/2 LEDs) */
static int win_held = 0;     /* the Windows key is down */
static int win_used = 0;     /* ... and another key went with it (no Start menu then) */
static int alt_tab = 0;      /* Alt+Tab happened while Alt is down */

/* Caps Lock: letters the other case (with Shift: back to small), é -> É too */
static uint32_t caps_flip(uint32_t v) {
    if ((v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z')) return v ^ 0x20;
    if (v >= 0xE0 && v <= 0xFE && v != 0xF7) return v - 0x20;
    if (v >= 0xC0 && v <= 0xDE && v != 0xD7) return v + 0x20;
    if (v >= 0x430 && v <= 0x44F) return v - 0x20;           /* Cyrillic */
    if (v >= 0x410 && v <= 0x42F) return v + 0x20;
    if (v >= 0x450 && v <= 0x45F) return v - 0x50;
    if (v >= 0x400 && v <= 0x40F) return v + 0x50;
    if (v >= 0x3B1 && v <= 0x3C9 && v != 0x3C2) return v - 0x20;   /* Greek */
    if (v >= 0x391 && v <= 0x3A9) return v + 0x20;
    /* Latin Extended-A: pairs of capital and small, the capital even ... */
    if ((v >= 0x100 && v <= 0x137 && v != 0x130 && v != 0x131) || (v >= 0x14A && v <= 0x177)) return v ^ 1;
    /* ... or odd (Ĺ ĺ ... Ň ň, Ź ź Ż ż Ž ž) */
    if ((v >= 0x139 && v <= 0x148) || (v >= 0x179 && v <= 0x17E)) return (v & 1) ? v + 1 : v - 1;
    return v;
}

/* the PS/2 keyboard's lights: command 0xED, then the mask (bit 2 = Caps Lock),
 * each answered by an ACK before the next byte is sent */
static void kb_set_leds(void) {
    if (!ps2_controller_present()) return;
    uint8_t leds = caps_lock ? 0x04 : 0;
    for (int step = 0; step < 2; step++) {
        kb_wait_write();
        outb(KB_DATA_PORT, step == 0 ? 0xED : leds);
        for (int t = 0; t < 20000; t++) {
            uint8_t st = inb(KB_STATUS_PORT);
            if (!(st & 0x01)) continue;
            uint8_t b = inb(KB_DATA_PORT);
            if (st & 0x20) { mouse_on_aux_byte(b); continue; }
            if (b == 0xFA || b == 0xFE) break;
        }
    }
}

static void caps_press(void) {
    if (caps_down) return;
    caps_down = 1;
    caps_lock = !caps_lock;
    if (!g_from_usb) kb_set_leds();
}
static int use_set2 = 0;
static int set2_break = 0;
static int set2_e0 = 0;
static int set1_e0 = 0;

/* One-shot Ctrl+Alt+Delete flag: set when the dedicated Delete key is
 * pressed while both Ctrl and Alt are held, cleared by whoever consumes
 * it via keyboard_ctrl_alt_del_pending(). Kept separate from the normal
 * character stream since Delete isn't a letter (nothing for the
 * existing Ctrl+letter -> control-code mapping to produce). */
static int ctrl_alt_del_pending = 0;

/* ── tiny char queue (fixes static-inside-loop bug) ─────────────── */
#define QUEUE_SIZE 256   /* also holds text typed by keyboard_inject() */
static char  q_buf[QUEUE_SIZE];
static uint64_t q_stamp[QUEUE_SIZE];
static int   q_head = 0;
static int   q_tail = 0;

static int q_empty(void) { return q_head == q_tail; }
static void q_push(char c) {
    int next = (q_tail + 1) % QUEUE_SIZE;
    if (next != q_head) { q_buf[q_tail] = c; q_stamp[q_tail] = latency_stamp(); q_tail = next; }
    else latency_drop();
}
static char q_pop(void) {
    char c = q_buf[q_head];
    latency_input(q_stamp[q_head]);
    q_head = (q_head + 1) % QUEUE_SIZE;
    return c;
}

static void push_arrow(char letter) {
    q_push(27); q_push('['); q_push(letter);
}


/* ── function and media keys: events of their own (keyboard_take_fkey) ── */
static int g_fk[16];
static uint64_t g_fk_stamp[16];
static int g_fk_head, g_fk_tail;

static void fkey(int code) {
    if (code == KEYF_MUTE || code == KEYF_VOLDOWN || code == KEYF_VOLUP) {
        /* the volume keys work everywhere, desktop or not */
        static int muted_from = -1;
        int v = audio_get_volume();
        if (code == KEYF_MUTE) {
            if (v > 0) { muted_from = v; audio_set_volume(0); }
            else audio_set_volume(muted_from > 0 ? muted_from : 50);
        } else {
            v += code == KEYF_VOLUP ? 5 : -5;
            audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
            muted_from = -1;
        }
    }
    if (code >= KEYF_F1 && code < KEYF_F1 + 12) {
        if (shift_held) code |= KEYF_SHIFT;
        if (ctrl_held)  code |= KEYF_CTRL;
        if (alt_held)   code |= KEYF_ALT;
    }
    int next = (g_fk_tail + 1) % 16;
    if (next != g_fk_head) { g_fk[g_fk_tail] = code; g_fk_stamp[g_fk_tail] = latency_stamp(); g_fk_tail = next; }
    else latency_drop();
}

/* a key while the Windows key is down: a shortcut, not a character */
static int win_combo(int code) {
    if (!win_held || !code) return 0;
    win_used = 1;
    if (code >= 'A' && code <= 'Z') code += 32;
    fkey(KEYF_WINKEY | code);
    return 1;
}
static void win_down(void) { if (!win_held) { win_held = 1; win_used = 0; } }
static void win_up(void) { if (win_held && !win_used) fkey(KEYF_WIN); win_held = 0; }
static void alt_up(void) { if (alt_tab) { alt_tab = 0; fkey(KEYF_ALTUP); } }
/* Alt+Tab, Ctrl+Esc; 1 if the key was one of them (sc1: its set-1 code) */
static int global_key(uint8_t sc1) {
    if (sc1 == 0x0F && alt_held && !altgr_held) {
        fkey(KEYF_TAB | KEYF_ALT | (shift_held ? KEYF_SHIFT : 0));
        alt_tab = 1;
        return 1;
    }
    if (sc1 == 0x01 && ctrl_held && !alt_held) { fkey(KEYF_WIN); return 1; }
    if (win_held) {
        char c = sc1 == 0x0F ? '\t' : sc1 < 128 ? sc_normal[sc1] : 0;
        if (c) return win_combo(c);
    }
    return 0;
}

/* ── a task's keys from elsewhere (kernel/kcmd.c: a kernel command run for
 * a program reads its stdin) ── */
#define KIN_MAX 32
typedef struct { int pid; int (*get)(void* ctx, int wait); void* ctx; char back[16]; int nback; } kin_t;
static kin_t g_kin[KIN_MAX];
static int g_nkin;

static kin_t* kin_cur(void) {
    if (!g_nkin) return NULL;
    int me = task_current_pid() + 1;
    for (int i = 0; i < KIN_MAX; i++) if (g_kin[i].pid == me) return &g_kin[i];
    return NULL;
}

int keyboard_redirect_begin(int (*get)(void* ctx, int wait), void* ctx) {
    for (int i = 0; i < KIN_MAX; i++) {
        if (g_kin[i].pid) continue;
        g_kin[i].get = get;
        g_kin[i].ctx = ctx;
        g_kin[i].nback = 0;
        g_kin[i].pid = task_current_pid() + 1;
        g_nkin++;
        return 0;
    }
    return -1;
}

void keyboard_redirect_end(void) {
    kin_t* k = kin_cur();
    if (!k) return;
    k->pid = 0;
    g_nkin--;
}

int keyboard_take_fkey(void) {
    if (kin_cur()) return 0;                          /* (a program's: no F-keys of the console's) */
    if (g_fk_head == g_fk_tail) return 0;
    int c = g_fk[g_fk_head];
    latency_input(g_fk_stamp[g_fk_head]);
    g_fk_head = (g_fk_head + 1) % 16;
    return c;
}

/* set 1: F1-F10 are 3B-44, F11 57, F12 58; set 2 has its own codes */
static int set1_fn(uint8_t sc) {
    if (sc >= 0x3B && sc <= 0x44) return KEYF_F1 + (sc - 0x3B);
    if (sc == 0x57) return KEYF_F1 + 10;
    if (sc == 0x58) return KEYF_F1 + 11;
    return 0;
}
static int set2_fn(uint8_t sc) {
    static const uint8_t T[12] = { 0x05, 0x06, 0x04, 0x0C, 0x03, 0x0B, 0x83, 0x0A, 0x01, 0x09, 0x78, 0x07 };
    for (int i = 0; i < 12; i++) if (T[i] == sc) return KEYF_F1 + i;
    return 0;
}
/* the E0-prefixed ones: Windows key and the multimedia keys */
static int set1_e0_key(uint8_t sc) {
    switch (sc) {
    case 0x5B: case 0x5C: return KEYF_WIN;
    case 0x20: return KEYF_MUTE;
    case 0x2E: return KEYF_VOLDOWN;
    case 0x30: return KEYF_VOLUP;
    case 0x22: return KEYF_PLAY;
    case 0x24: return KEYF_STOP;
    case 0x19: return KEYF_NEXT;
    case 0x10: return KEYF_PREV;
    default:   return 0;
    }
}
static int set2_e0_key(uint8_t sc) {
    switch (sc) {
    case 0x1F: case 0x27: return KEYF_WIN;
    case 0x23: return KEYF_MUTE;
    case 0x21: return KEYF_VOLDOWN;
    case 0x32: return KEYF_VOLUP;
    case 0x34: return KEYF_PLAY;
    case 0x3B: return KEYF_STOP;
    case 0x4D: return KEYF_NEXT;
    case 0x15: return KEYF_PREV;
    default:   return 0;
    }
}

static char set2_map_char(uint8_t sc, int shifted) {
    switch (sc) {
        case 0x16: return shifted ? '!' : '1';
        case 0x1E: return shifted ? '@' : '2';
        case 0x26: return shifted ? '#' : '3';
        case 0x25: return shifted ? '$' : '4';
        case 0x2E: return shifted ? '%' : '5';
        case 0x36: return shifted ? '^' : '6';
        case 0x3D: return shifted ? '&' : '7';
        case 0x3E: return shifted ? '*' : '8';
        case 0x46: return shifted ? '(' : '9';
        case 0x45: return shifted ? ')' : '0';
        case 0x4E: return shifted ? '_' : '-';
        case 0x55: return shifted ? '+' : '=';
        case 0x0D: return '\t';
        case 0x5A: return '\n';
        case 0x66: return '\b';
        case 0x29: return ' ';
        case 0x54: return shifted ? '{' : '[';
        case 0x5B: return shifted ? '}' : ']';
        case 0x4C: return shifted ? ':' : ';';
        case 0x52: return shifted ? '"' : '\'';
        case 0x0E: return shifted ? '~' : '`';
        case 0x5D: return shifted ? '|' : '\\';
        case 0x41: return shifted ? '<' : ',';
        case 0x49: return shifted ? '>' : '.';
        case 0x4A: return shifted ? '?' : '/';

        case 0x1C: return shifted ? 'A' : 'a';
        case 0x32: return shifted ? 'B' : 'b';
        case 0x21: return shifted ? 'C' : 'c';
        case 0x23: return shifted ? 'D' : 'd';
        case 0x24: return shifted ? 'E' : 'e';
        case 0x2B: return shifted ? 'F' : 'f';
        case 0x34: return shifted ? 'G' : 'g';
        case 0x33: return shifted ? 'H' : 'h';
        case 0x43: return shifted ? 'I' : 'i';
        case 0x3B: return shifted ? 'J' : 'j';
        case 0x42: return shifted ? 'K' : 'k';
        case 0x4B: return shifted ? 'L' : 'l';
        case 0x3A: return shifted ? 'M' : 'm';
        case 0x31: return shifted ? 'N' : 'n';
        case 0x44: return shifted ? 'O' : 'o';
        case 0x4D: return shifted ? 'P' : 'p';
        case 0x15: return shifted ? 'Q' : 'q';
        case 0x2D: return shifted ? 'R' : 'r';
        case 0x1B: return shifted ? 'S' : 's';
        case 0x2C: return shifted ? 'T' : 't';
        case 0x3C: return shifted ? 'U' : 'u';
        case 0x2A: return shifted ? 'V' : 'v';
        case 0x1D: return shifted ? 'W' : 'w';
        case 0x22: return shifted ? 'X' : 'x';
        case 0x35: return shifted ? 'Y' : 'y';
        case 0x1A: return shifted ? 'Z' : 'z';
        default: return 0;
    }
}

/* ── the layouts (kernel/kblayouts.c): every key by its position ──
 * AltGr (right Alt, or Ctrl+Alt), Shift+AltGr and the dead accents.
 * Characters past ASCII are sent as UTF-8, so é arrives as two bytes. */

static uint32_t g_dead;          /* a pending dead accent, or 0 */

/* bytes of the character still to deliver after the first one */
static char g_rest[12];
static int  g_rest_n;

static int has_table(void) { return kb_layouts[g_layout].keys != 0; }

/* the first byte of what cps[] encodes goes to *out, the others wait in g_rest */
static int emit_cps(const uint32_t* cps, int n, char* out) {
    char buf[16];
    int len = 0;
    for (int i = 0; i < n; i++) len += u8_encode(cps[i], buf + len);
    *out = buf[0];
    g_rest_n = 0;
    for (int i = 1; i < len; i++) g_rest[g_rest_n++] = buf[i];
    return 1;
}

/* hands the waiting bytes over to the queue (after *out was delivered) */
static void flush_rest(void) {
    for (int i = 0; i < g_rest_n; i++) q_push(g_rest[i]);
    g_rest_n = 0;
}

/* a key on the layout's table (set-1 scancode); -1 when the shared US
 * tables decide (Enter, Tab, the keypad...) */
static int layout_key(uint8_t sc, char* out) {
    const kb_layout_desc_t* L = &kb_layouts[g_layout];
    const kb_key_t* k = 0;
    for (int i = 0; i < L->nkeys; i++) if (L->keys[i].sc == sc) { k = &L->keys[i]; break; }
    int altgr = altgr_held || (ctrl_held && alt_held);
    if (!k) {
        /* Enter, Backspace, Tab, Esc, the keypad...: a pending accent just goes */
        if (g_dead && sc != 0x39) { g_dead = 0; return -1; }
        if (!g_dead) return -1;
    }

    uint32_t v;
    if (!k) v = ' ';                                  /* the space bar */
    else if (altgr) v = shift_held && k->sag ? k->sag : k->ag;
    else v = shift_held ? k->s : k->n;
    if (!v) return 0;

    if (caps_lock && !altgr && !IS_DEAD(v)) v = caps_flip(v);

    /* Ctrl+letter: control codes, by the letter printed on the key */
    if (ctrl_held && !altgr && !IS_DEAD(v)) {
        if (v < 128 && ((v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z'))) {
            *out = (char)((v & 0x1F));
            return 1;
        }
        if (v < 128) { *out = (char)v; return 1; }
        /* (a letter of another alphabet - Russian, Greek: the Latin one at its place) */
        char us = sc < 128 ? sc_normal[sc] : 0;
        if (us >= 'a' && us <= 'z') { *out = (char)(us & 0x1F); return 1; }
        return 0;
    }

    if (IS_DEAD(v)) {
        uint32_t acc = v & 0xFFFF;
        if (g_dead) {                                 /* twice: the accent itself */
            uint32_t two[2] = { g_dead, acc };
            g_dead = 0;
            return emit_cps(two, acc == two[0] ? 1 : 2, out);
        }
        g_dead = acc;
        return 0;
    }

    if (g_dead) {
        uint32_t acc = g_dead;
        g_dead = 0;
        if (v == ' ') return emit_cps(&acc, 1, out);
        uint32_t cp = kb_compose(acc, v);
        if (cp) return emit_cps(&cp, 1, out);
        uint32_t two[2] = { acc, v };                 /* no such letter: both */
        return emit_cps(two, 2, out);
    }
    return emit_cps(&v, 1, out);
}

/* set-2 key codes (no translation by the controller) -> set-1 positions */
static uint8_t set2_to_set1(uint8_t sc) {
    static const uint8_t T[][2] = {
        {0x0E,0x29},{0x16,0x02},{0x1E,0x03},{0x26,0x04},{0x25,0x05},{0x2E,0x06},{0x36,0x07},
        {0x3D,0x08},{0x3E,0x09},{0x46,0x0A},{0x45,0x0B},{0x4E,0x0C},{0x55,0x0D},
        {0x15,0x10},{0x1D,0x11},{0x24,0x12},{0x2D,0x13},{0x2C,0x14},{0x35,0x15},{0x3C,0x16},
        {0x43,0x17},{0x44,0x18},{0x4D,0x19},{0x54,0x1A},{0x5B,0x1B},
        {0x1C,0x1E},{0x1B,0x1F},{0x23,0x20},{0x2B,0x21},{0x34,0x22},{0x33,0x23},{0x3B,0x24},
        {0x42,0x25},{0x4B,0x26},{0x4C,0x27},{0x52,0x28},{0x5D,0x2B},
        {0x61,0x56},{0x1A,0x2C},{0x22,0x2D},{0x21,0x2E},{0x2A,0x2F},{0x32,0x30},{0x31,0x31},
        {0x3A,0x32},{0x41,0x33},{0x49,0x34},{0x4A,0x35},{0x29,0x39},
    };
    for (int i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++) if (T[i][0] == sc) return T[i][1];
    return 0;
}

static int translate_scancode_set2(uint8_t sc, char* out) {
    if (set2_break) {
        if (sc == 0x12 || sc == 0x59) shift_held = 0;
        if (sc == 0x58) caps_down = 0;
        if (sc == 0x14) ctrl_held = 0;
        if (sc == 0x11) { alt_held = 0; if (set2_e0) altgr_held = 0; alt_up(); }
        if (set2_e0 && (sc == 0x1F || sc == 0x27)) win_up();
        set2_break = 0;
        set2_e0 = 0;
        return 0;
    }

    if (set2_e0) {
        if (sc == 0x1F || sc == 0x27) { win_down(); set2_e0 = 0; return 0; }
        if (sc == 0x2F) { fkey(KEYF_MENU); set2_e0 = 0; return 0; }
        if (win_held && (sc == 0x75 || sc == 0x72 || sc == 0x74 || sc == 0x6B)) {
            win_combo(sc == 0x75 ? KEYW_UP : sc == 0x72 ? KEYW_DOWN : sc == 0x74 ? KEYW_RIGHT : KEYW_LEFT);
            set2_e0 = 0;
            return 0;
        }
        if (sc == 0x75) { push_arrow('A'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x72) { push_arrow('B'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x74) { push_arrow('C'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x6B) { push_arrow('D'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x14) { ctrl_held = 1; set2_e0 = 0; return 0; }
        if (sc == 0x11) { alt_held = 1; altgr_held = 1; set2_e0 = 0; return 0; }  /* right alt */
        /* Home End PgUp PgDn Delete: ESC [ H/F/I/G/P (three bytes, like the arrows) */
        if (sc == 0x6C) { push_arrow('H'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x69) { push_arrow('F'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x7D) { push_arrow('I'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x7A) { push_arrow('G'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x71) {                                          /* Delete key */
            set2_e0 = 0;
            if (ctrl_held && alt_held) { ctrl_alt_del_pending = 1; return 0; }
            push_arrow('P'); *out = q_pop(); return 1;
        }
        { int k = set2_e0_key(sc); if (k) { fkey(k); set2_e0 = 0; return 0; } }
        set2_e0 = 0;
        return 0;
    }

    { int k = set2_fn(sc); if (k) { fkey(k); return 0; } }
    if (sc == 0x12 || sc == 0x59) { shift_held = 1; return 0; }
    if (sc == 0x14) { ctrl_held = 1; return 0; }
    if (sc == 0x11) { alt_held = 1; return 0; }  /* left alt */
    if (sc == 0x58) { caps_press(); return 0; }   /* Caps Lock */
    {
        uint8_t g1 = sc == 0x0D ? 0x0F : sc == 0x76 ? 0x01 : set2_to_set1(sc);
        if (g1 && global_key(g1)) return 0;
    }

    if (has_table()) {
        uint8_t s1 = set2_to_set1(sc);
        int r = s1 ? layout_key(s1, out) : (g_dead = 0, -1);
        if (r >= 0) return r;
    }

    char c = set2_map_char(sc, shift_held);
    if (!c) return 0;
    if (caps_lock) c = (char)caps_flip((unsigned char)c);

    if (ctrl_held) {
        char base = (c >= 'A' && c <= 'Z') ? c :
                    (c >= 'a' && c <= 'z') ? (char)(c - 32) : 0;
        if (base) { *out = (char)(base - 'A' + 1); return 1; }
    }
    *out = c;
    return 1;
}

static int translate_scancode(uint8_t sc, char* out) {
    /* Arrow keys → ESC [ X sequences */
    if (sc == 0x48) { push_arrow('A'); *out = q_pop(); return 1; } /* up    */
    if (sc == 0x50) { push_arrow('B'); *out = q_pop(); return 1; } /* down  */
    if (sc == 0x4D) { push_arrow('C'); *out = q_pop(); return 1; } /* right */
    if (sc == 0x4B) { push_arrow('D'); *out = q_pop(); return 1; } /* left  */

    /* Home/End/PgUp/PgDn → ignore for now */
    if (sc == 0x47 || sc == 0x4F || sc == 0x49 || sc == 0x51) return 0;
    if (sc >= 128) return 0;

    if (has_table()) {
        int r = layout_key(sc, out);
        if (r >= 0) return r;
    }

    char c = shift_held ? sc_shift[sc] : sc_normal[sc];
    if (!c && !shift_held) c = sc_normal[sc];
    if (!c) return 0;
    if (caps_lock) c = (char)caps_flip((unsigned char)c);

    /* Ctrl+letter → codes 1-26 */
    if (ctrl_held) {
        char base = (c >= 'A' && c <= 'Z') ? c :
                    (c >= 'a' && c <= 'z') ? (char)(c - 32) : 0;
        if (base) { *out = (char)(base - 'A' + 1); return 1; }
    }
    *out = c;
    return 1;
}

/* ── the keys held now (keyboard_held) ─────────────────────────────
 * Every byte goes through here first, with prefix state of its own:
 * the decoder below clears its flags as it pleases. */
static uint8_t g_held[32];           /* bit: set-1 code, + 0x80 for E0 */
static uint32_t g_escape_sequence;
uint32_t keyboard_escape_sequence(void) { return g_escape_sequence; }
static uint8_t g_pressed[32];        /* went down since keyboard_take_pressed() looked */
static int     g_held_e0, g_held_break;

static void held_clear(void) {
    for (int i = 0; i < 32; i++) g_held[i] = g_pressed[i] = 0;
    g_held_e0 = g_held_break = 0;
}

/* a set-2 key -> its set-1 position (+ 0x80 for E0), -1 if not tracked */
static int set2_held_code(uint8_t sc, int e0) {
    static const uint8_t E[][2] = {
        {0x75,0x48},{0x72,0x50},{0x74,0x4D},{0x6B,0x4B},{0x6C,0x47},{0x69,0x4F},{0x7D,0x49},
        {0x7A,0x51},{0x70,0x52},{0x71,0x53},{0x14,0x1D},{0x11,0x38},{0x5A,0x1C},{0x4A,0x35},
        {0x1F,0x5B},{0x27,0x5C},{0x2F,0x5D},
    };
    static const uint8_t N[][2] = {
        {0x76,0x01},{0x66,0x0E},{0x0D,0x0F},{0x5A,0x1C},{0x14,0x1D},{0x12,0x2A},{0x59,0x36},
        {0x11,0x38},{0x58,0x3A},{0x7C,0x37},{0x7B,0x4A},{0x79,0x4E},{0x70,0x52},{0x69,0x4F},
        {0x72,0x50},{0x7A,0x51},{0x6B,0x4B},{0x73,0x4C},{0x74,0x4D},{0x6C,0x47},{0x75,0x48},
        {0x7D,0x49},{0x71,0x53},{0x77,0x45},{0x7E,0x46},
    };
    if (e0) {
        for (int i = 0; i < (int)(sizeof(E) / sizeof(E[0])); i++) if (E[i][0] == sc) return E[i][1] | 0x80;
        return -1;
    }
    for (int i = 0; i < (int)(sizeof(N) / sizeof(N[0])); i++) if (N[i][0] == sc) return N[i][1];
    int f = set2_fn(sc);
    if (f) { int i = f - KEYF_F1; return i < 10 ? 0x3B + i : 0x57 + (i - 10); }
    int s = set2_to_set1(sc);
    return s ? s : -1;
}

static void track_held(uint8_t sc) {
    if (sc == 0xE0) { g_held_e0 = 1; return; }
    if (sc == 0xE1 || sc == 0xFA || sc == 0xFE) return;     /* Pause's prefix, ACK, resend */
    if (sc == 0xF0) {
        /* a set-2 keyboard: what was read as set 1 before is wrong */
        if (!use_set2) held_clear();
        g_held_break = 1;
        return;
    }
    int code, down;
    if (use_set2 || g_held_break) { code = set2_held_code(sc, g_held_e0); down = !g_held_break; }
    else { code = (sc & 0x7F) | (g_held_e0 ? 0x80 : 0); down = !(sc & 0x80); }
    g_held_e0 = g_held_break = 0;
    if (code < 0) return;
    if (down) {
        if (code == 1 && !(g_held[0] & 2)) g_escape_sequence++;
        g_held[code >> 3] |= (uint8_t)(1u << (code & 7));
        g_pressed[code >> 3] |= (uint8_t)(1u << (code & 7));
    }
    else      g_held[code >> 3] &= (uint8_t)~(1u << (code & 7));
}

char keyboard_us_char(int code, int shift) {
    if (code <= 0 || code > 0x39) return 0;
    return shift ? sc_shift[code] : sc_normal[code];
}

/* ── raw set-1 capture for the RDP client ─────────────────────────── */
static int      g_raw_on;
static int      g_raw_e0;
static uint64_t g_raw_stamp[64];
static uint16_t g_raw_q[64];         /* code | (e0<<8) | (down<<9) */
static int      g_raw_h, g_raw_t;

void keyboard_capture_raw(int on) {
    g_raw_on = on;
    g_raw_e0 = 0;
    if (!on) g_raw_h = g_raw_t = 0;
}

static void raw_capture(uint8_t sc) {
    /* the stream is set 1 here: PS/2 with controller translation, and USB
     * keyboards feed set-1 scancodes through keyboard_feed_scancode */
    if (sc == 0xE0) { g_raw_e0 = 1; return; }
    if (sc == 0xE1) return;                         /* Pause: ignored */
    uint16_t v = (uint16_t)((sc & 0x7F) | (g_raw_e0 ? 0x100 : 0) | ((sc & 0x80) ? 0 : 0x200));
    g_raw_e0 = 0;
    int next = (g_raw_h + 1) % 64;
    if (next != g_raw_t) { g_raw_q[g_raw_h] = v; g_raw_stamp[g_raw_h] = latency_stamp(); g_raw_h = next; }
    else latency_drop();
}

int keyboard_poll_raw(uint8_t* code, int* down, int* e0) {
    if (g_raw_t == g_raw_h) return 0;
    uint16_t v = g_raw_q[g_raw_t];
    latency_input(g_raw_stamp[g_raw_t]);
    g_raw_t = (g_raw_t + 1) % 64;
    *code = (uint8_t)(v & 0x7F);
    *e0 = (v >> 8) & 1;
    *down = (v >> 9) & 1;
    return 1;
}

static int process_scancode_byte(uint8_t sc, char* out) {
    track_held(sc);
    if (g_raw_on) { raw_capture(sc); return 0; }   /* the RDP window has the keyboard */
    if (sc == 0xF0) {
        use_set2 = 1;
        set2_break = 1;
        return 0;
    }
    if (sc == 0xE0) {
        if (use_set2) set2_e0 = 1;
        else          set1_e0 = 1;
        return 0;
    }

    if (use_set2) return translate_scancode_set2(sc, out);

    /* set 1 handling */
    if (set1_e0) {
        if (sc == 0x5B || sc == 0x5C) { win_down(); set1_e0 = 0; return 0; }
        if (sc == 0xDB || sc == 0xDC) { win_up(); set1_e0 = 0; return 0; }
        if (sc == 0x5D) { fkey(KEYF_MENU); set1_e0 = 0; return 0; }
        if (win_held && (sc == 0x48 || sc == 0x50 || sc == 0x4D || sc == 0x4B)) {
            win_combo(sc == 0x48 ? KEYW_UP : sc == 0x50 ? KEYW_DOWN : sc == 0x4D ? KEYW_RIGHT : KEYW_LEFT);
            set1_e0 = 0;
            return 0;
        }
        /* set1 extended arrows: E0 48/50/4D/4B */
        if (sc == 0x48) { push_arrow('A'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x50) { push_arrow('B'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x4D) { push_arrow('C'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x4B) { push_arrow('D'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x1D) { ctrl_held = 1; set1_e0 = 0; return 0; }  /* right ctrl down */
        if (sc == 0x9D) { ctrl_held = 0; set1_e0 = 0; return 0; }  /* right ctrl up */
        if (sc == 0x38) { alt_held = 1; altgr_held = 1; set1_e0 = 0; return 0; }   /* right alt (AltGr) down */
        if (sc == 0xB8) { alt_held = 0; altgr_held = 0; set1_e0 = 0; alt_up(); return 0; }   /* right alt up */
        if (sc == 0x47) { push_arrow('H'); *out = q_pop(); set1_e0 = 0; return 1; }   /* Home */
        if (sc == 0x4F) { push_arrow('F'); *out = q_pop(); set1_e0 = 0; return 1; }   /* End */
        if (sc == 0x49) { push_arrow('I'); *out = q_pop(); set1_e0 = 0; return 1; }   /* PgUp */
        if (sc == 0x51) { push_arrow('G'); *out = q_pop(); set1_e0 = 0; return 1; }   /* PgDn */
        if (sc == 0x53) {                                          /* Delete key down */
            set1_e0 = 0;
            if (ctrl_held && alt_held) { ctrl_alt_del_pending = 1; return 0; }
            push_arrow('P'); *out = q_pop(); return 1;
        }
        { int k = set1_e0_key(sc); if (k) { fkey(k); set1_e0 = 0; return 0; } }
        set1_e0 = 0;
        return 0;
    }

    if (sc == 0x2A || sc == 0x36) { shift_held = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift_held = 0; return 0; }
    if (sc == 0x1D)               { ctrl_held  = 1; return 0; }
    if (sc == 0x9D)               { ctrl_held  = 0; return 0; }
    if (sc == 0x38)               { alt_held   = 1; return 0; }  /* left alt down */
    if (sc == 0xB8)               { alt_held   = 0; alt_up(); return 0; }  /* left alt up */
    if (sc == 0x3A)               { caps_press();  return 0; }  /* Caps Lock */
    if (sc == 0xBA)               { caps_down  = 0; return 0; }
    if (sc & 0x80)                return 0; /* key-up */
    { int k = set1_fn(sc); if (k) { fkey(k); return 0; } }
    if (global_key(sc)) return 0;

    return translate_scancode(sc, out);
}

int keyboard_caps_lock(void) { return caps_lock; }

const char* keyboard_layout_name(void) {
    return kb_layouts[g_layout].name;
}

const char* keyboard_layouts_help(void) {
    static char s[256];
    if (!s[0])
        for (int i = 0; i < kb_layout_count; i++) {
            if (i) kstrlcat(s, ", ", sizeof(s));
            kstrlcat(s, kb_layouts[i].name, sizeof(s));
        }
    return s;
}

int keyboard_layout_count(void) { return kb_layout_count; }
const char* keyboard_layout_at(int i, const char** label) {
    if (i < 0 || i >= kb_layout_count) return 0;
    if (label) *label = kb_layouts[i].label;
    return kb_layouts[i].name;
}

/* Win+Space: English (US) and back to the layout chosen (for typing
 * commands and web addresses on a Russian or Greek keyboard): the layout
 * now in use */
static int g_layout_back = -1;
const char* keyboard_toggle_layout(void) {
    if (g_layout != 0) { g_layout_back = g_layout; g_layout = 0; }
    else if (g_layout_back > 0) g_layout = g_layout_back;
    g_dead = 0;
    return kb_layouts[g_layout].label;
}

int keyboard_set_layout(const char* name) {
    if (!name || !*name) return -1;
    if (strieq(name, "EN") || strieq(name, "US") || strieq(name, "default")) name = kb_layouts[0].name;
    for (int i = 0; i < kb_layout_count; i++)
        if (strieq(name, kb_layouts[i].name)) {
            g_dead = 0;
            g_layout = i;
            g_layout_back = -1;
            return 0;
        }
    return -1;
}

/* USB keyboards (usb/usbhid.c) translate their key events into set-1
 * scancodes and feed them through the same decoder as PS/2, so layouts,
 * modifiers, arrows and Ctrl+Alt+Del behave identically. */
void keyboard_feed_scancode(uint8_t sc) {
    char out = 0;
    g_from_usb = 1;
    int got = process_scancode_byte(sc, &out);
    g_from_usb = 0;
    if (!got) return;
    if (out == 27) {
        /* an arrow: the decoder queued "[X" and handed back the ESC that
         * must come first */
        q_head = (q_head + QUEUE_SIZE - 1) % QUEUE_SIZE;
        q_buf[q_head] = out;
        q_stamp[q_head] = latency_stamp();
    } else {
        q_push(out);
        flush_rest();
    }
}

void keyboard_feed_set1(uint8_t sc) {
    /* a USB keyboard may have switched the decoder to set 2; the RDP client
     * always sends set-1 scancodes, so force set 1 for these bytes */
    use_set2 = 0; set2_break = 0; set2_e0 = 0; g_held_break = 0;
    keyboard_feed_scancode(sc);
}

/* Serial console input (see serial.h): terminals send CR for Enter and
 * DEL for Backspace; map them to what the shell expects. Arrow keys
 * already arrive as the same ESC [ A-D sequences the PS/2 path queues. */
static char serial_key(void) {
    static int last_cr = 0;
    int b = serial_try_getc();
    if (b < 0) return 0;
    if (b == '\n' && last_cr) { last_cr = 0; return 0; }
    last_cr = (b == '\r');
    if (b == '\r' || b == '\n') return '\n';
    if (b == 0x7F) return '\b';
    return (char)b;
}

/* the serial line types on the console (0: it is a terminal of its own, kernel/vcons.c) */
static int g_serial_keys = 1;
void keyboard_serial_input(int on) { g_serial_keys = on; }

/* while the screen is locked, only the lock screen's task gets keys */
static int g_key_owner = -1;
void keyboard_set_owner(int pid) { g_key_owner = pid; }

int keyboard_held(int code) {
    if (code < 0 || code > 0xFF) return 0;
    if (g_key_owner >= 0 && task_current_pid() != g_key_owner) return 0;
    return (g_held[code >> 3] >> (code & 7)) & 1;
}

int keyboard_take_pressed(int code) {
    if (code < 0 || code > 0xFF) return 0;
    int r = (g_pressed[code >> 3] >> (code & 7)) & 1;
    g_pressed[code >> 3] &= (uint8_t)~(1u << (code & 7));
    return r && !(g_key_owner >= 0 && task_current_pid() != g_key_owner);
}

int keyboard_held_char(int c) {
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c <= 0 || c >= 128) return 0;
    if (c == '\n' && keyboard_held(0x9C)) return 1;          /* the keypad's Enter */
    for (int i = 1; i <= 0x39; i++)                          /* (the main block, not the keypad) */
        if (sc_normal[i] == c) return keyboard_held(i);
    return 0;
}

char keyboard_try_getchar(void) {
    kin_t* k = kin_cur();
    if (k) {
        if (k->nback) return k->back[--k->nback];
        int c = k->get(k->ctx, 0);
        return c > 0 ? (char)c : 0;
    }
    /* daemons have no keyboard; an SSH session's shell reads its client */
    if (task_is_background()) return 0;
    int tt = tty_current();
    if (tt >= 0) return tty_getkey(tt);
    if (g_key_owner >= 0 && task_current_pid() != g_key_owner) return 0;

    latency_input_poll();
    usb_poll();           /* USB keyboards report through here */
    if (!q_empty()) return q_pop();

    char sk = g_serial_keys ? serial_key() : 0;
    if (sk) return sk;

    /* the timer interrupt takes mouse bytes from the controller too
     * (mouse_irq_motion): status and data are read in one go */
    if (!ps2_controller_present()) return 0;    /* (USB keyboards come through usb_poll) */
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint8_t st = ps2_status();                  /* (0 once the controller has gone away) */
    if (!(st & 0x01)) {                         /* no data */
        if (fl & 0x200) __asm__ volatile("sti");
        return 0;
    }
    if (st & 0x20) {
        uint8_t b = inb(KB_DATA_PORT);
        mouse_on_aux_byte(b);
        if (fl & 0x200) __asm__ volatile("sti");
        return 0;
    }
    uint8_t sc = inb(KB_DATA_PORT);
    if (fl & 0x200) __asm__ volatile("sti");

    char out = 0;
    if (process_scancode_byte(sc, &out)) { flush_rest(); return out; }
    return 0;
}

void keyboard_init(void) {
    shift_held = 0; ctrl_held = 0; alt_held = 0; altgr_held = 0;
    if (!ps2_controller_present()) {           /* a Mac: nothing to set up (it would hang reading 0xFF) */
        g_dead = 0; g_rest_n = 0;
        caps_lock = 0; caps_down = 0;
        q_head = 0; q_tail = 0;
        held_clear();
        return;
    }
    /*
     * Re-enable the first PS/2 port and translation.
     * USB legacy emulation in VMs/BIOS often depends on this state.
     */
    kb_wait_write();
    outb(KB_CMD_PORT, 0xAD); /* disable first port while editing cmd byte */
    /* Do NOT disable second port (mouse). It breaks PS/2 mouse in VMs. */

    /* drain output buffer */
    for (int i = 0; i < 64 && (inb(KB_STATUS_PORT) & 0x01); i++) inb(KB_DATA_PORT);

    kb_wait_write();
    outb(KB_CMD_PORT, 0x20); /* read command byte */
    kb_wait_read();
    uint8_t cb = inb(KB_DATA_PORT);
    cb &= ~(1u << 4);        /* enable first port clock */
    cb |=  (1u << 6);        /* enable translation (set2 -> set1) */
    cb &= ~(1u << 0);        /* no IRQ needed for polling mode */

    kb_wait_write();
    outb(KB_CMD_PORT, 0x60); /* write command byte */
    kb_wait_write();
    outb(KB_DATA_PORT, cb);

    kb_wait_write();
    outb(KB_CMD_PORT, 0xAE); /* enable first port */

    shift_held = 0; ctrl_held = 0; alt_held = 0; altgr_held = 0;
    g_dead = 0; g_rest_n = 0;
    caps_lock = 0; caps_down = 0;
    use_set2 = 0; set2_break = 0; set2_e0 = 0; set1_e0 = 0;
    ctrl_alt_del_pending = 0;
    q_head = 0; q_tail = 0;
    held_clear();
}

/* One-shot: returns 1 exactly once per Ctrl+Alt+Delete press, then
 * clears itself. Callers should poll this regularly (e.g. once per
 * frame) rather than only when otherwise idle. */
int keyboard_ctrl_alt_del_pending(void) {
    int p = ctrl_alt_del_pending;
    ctrl_alt_del_pending = 0;
    return p;
}

char keyboard_getchar(void) {
    kin_t* k = kin_cur();
    if (k) {
        if (k->nback) return k->back[--k->nback];
        int c = k->get(k->ctx, 1);
        return c > 0 ? (char)c : 4;                    /* (its end: ^D) */
    }
    while (1) {
        /* PS/2 and serial both feed keyboard_try_getchar(); while neither
         * has anything, let other tasks run (and the CPU idle). */
        char c = keyboard_try_getchar();
        if (c) return c;
        terminal_flush();
        task_sleep_ms(2);
    }
}

void keyboard_readline(char* buf, int maxlen) {
    int pos = 0;
    while (pos < maxlen - 1) {
        char c = keyboard_getchar();
        if (c == '\n')  { terminal_putchar('\n'); break; }
        if (c == '\b')  {
            if (pos > 0) { pos--; terminal_putchar('\b'); }
            continue;
        }
        if (c == 27) {
            /* swallow ESC [ X arrow sequence */
            keyboard_getchar();
            keyboard_getchar();
            continue;
        }
        if ((unsigned char)c < 32) continue;
        buf[pos++] = c;
        terminal_putchar(c);
    }
    buf[pos] = '\0';
}

/* Types text as if from the keyboard (the GUI uses it to start a command
 * in a terminal window it just opened). */
void keyboard_inject(const char* s) {
    while (*s) q_push(*s++);
}

int keyboard_mods(void) { return (shift_held ? 1 : 0) | (ctrl_held ? 2 : 0) | (alt_held ? 4 : 0); }

/* a character read too early goes back to the front of the queue */
void keyboard_unget(char c) {
    kin_t* k = kin_cur();
    if (k) { if (k->nback < 16) k->back[k->nback++] = c; return; }
    int prev = (q_head + QUEUE_SIZE - 1) % QUEUE_SIZE;
    if (prev == q_tail) return;                 /* full */
    q_head = prev;
    q_buf[q_head] = c;
    q_stamp[q_head] = 0; /* replay is not a new hardware event */
}

/* (selftest) the text typing set-1 codes gives on layout `name` - Shift is
 * 0x2A / 0xAA, AltGr 0xE0 0x38 / 0xE0 0xB8 - as UTF-8 in out; the layout,
 * the modifiers and the keys waiting are put back. 0, or -1 (no such layout) */
int keyboard_test_type(const char* name, const uint8_t* codes, int n, char* out, int cap) {
    int was = g_layout;
    static char keep[QUEUE_SIZE];
    int kn = 0;
    while (!q_empty() && kn < QUEUE_SIZE) keep[kn++] = q_pop();
    int sh = shift_held, ct = ctrl_held, al = alt_held, ag = altgr_held, cl = caps_lock;
    shift_held = ctrl_held = alt_held = altgr_held = caps_lock = 0;
    g_dead = 0;
    int rc = keyboard_set_layout(name), len = 0;
    if (rc == 0)
        for (int i = 0; i < n; i++) {
            keyboard_feed_set1(codes[i]);
            while (!q_empty()) {
                char c = q_pop();
                if (len < cap - 1) out[len++] = c;
            }
        }
    if (cap > 0) out[len] = 0;
    g_layout = was;
    g_dead = 0;
    shift_held = sh; ctrl_held = ct; alt_held = al; altgr_held = ag; caps_lock = cl;
    for (int i = 0; i < (int)sizeof(g_held); i++) g_held[i] = 0;   /* (no key let go in the test: none held) */
    for (int i = 0; i < kn; i++) q_push(keep[i]);
    return rc;
}
