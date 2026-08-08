#include "keyboard.h"
#include "terminal.h"
#include "types.h"
#include "usb.h"
#include "task.h"

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

typedef enum {
    KB_LAYOUT_EN_DEFAULT = 0,
    KB_LAYOUT_FR_CH,
    KB_LAYOUT_FR,
    KB_LAYOUT_DE,
    KB_LAYOUT_DE_CH,
    KB_LAYOUT_BEPO,
    KB_LAYOUT_COUNT
} kb_layout_t;

static kb_layout_t g_layout = KB_LAYOUT_EN_DEFAULT;

static const char* kbd_layout_names[KB_LAYOUT_COUNT] = {
    "EN (Default)",
    "fr_CH",
    "FR",
    "DE",
    "de_CH",
    "BEPO",
};

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

static char map_layout_char(char c, int shifted) {
    if (g_layout == KB_LAYOUT_EN_DEFAULT) return c;

    /* QWERTZ families */
    if (g_layout == KB_LAYOUT_FR_CH || g_layout == KB_LAYOUT_DE || g_layout == KB_LAYOUT_DE_CH) {
        if (c == 'y') return 'z';
        if (c == 'Y') return 'Z';
        if (c == 'z') return 'y';
        if (c == 'Z') return 'Y';
        return c;
    }

    /* FR AZERTY (core alpha layout + common punctuation moves) */
    if (g_layout == KB_LAYOUT_FR) {
        if (c == 'q') return 'a';
        if (c == 'Q') return 'A';
        if (c == 'w') return 'z';
        if (c == 'W') return 'Z';
        if (c == 'a') return 'q';
        if (c == 'A') return 'Q';
        if (c == 'z') return 'w';
        if (c == 'Z') return 'W';
        if (c == ';') return 'm';
        if (c == ':') return 'M';
        if (c == 'm') return shifted ? '?' : ',';
        if (c == ',') return ';';
        if (c == '<') return '.';
        if (c == '.') return ':';
        if (c == '>') return '/';
        if (c == '/') return '!';
        return c;
    }

    /* BEPO (letter-focused remap on US physical keys) */
    if (g_layout == KB_LAYOUT_BEPO) {
        if (c == 'q') return 'b';
        if (c == 'Q') return 'B';
        if (c == 'w') return 'e';
        if (c == 'W') return 'E';
        if (c == 'e') return 'p';
        if (c == 'E') return 'P';
        if (c == 'r') return 'o';
        if (c == 'R') return 'O';
        if (c == 'y') return 'v';
        if (c == 'Y') return 'V';
        if (c == 'u') return 'd';
        if (c == 'U') return 'D';
        if (c == 'i') return 'l';
        if (c == 'I') return 'L';
        if (c == 'o') return 'j';
        if (c == 'O') return 'J';
        if (c == 'p') return 'z';
        if (c == 'P') return 'Z';
        if (c == 's') return 'u';
        if (c == 'S') return 'U';
        if (c == 'd') return 'i';
        if (c == 'D') return 'I';
        if (c == 'f') return 'e';
        if (c == 'F') return 'E';
        if (c == 'g') return 'c';
        if (c == 'G') return 'C';
        if (c == 'h') return 't';
        if (c == 'H') return 'T';
        if (c == 'j') return 's';
        if (c == 'J') return 'S';
        if (c == 'k') return 'r';
        if (c == 'K') return 'R';
        if (c == 'l') return 'n';
        if (c == 'L') return 'N';
        if (c == 'z') return 'y';
        if (c == 'Z') return 'Y';
        return c;
    }

    return c;
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
#define QUEUE_SIZE 8
static char  q_buf[QUEUE_SIZE];
static int   q_head = 0;
static int   q_tail = 0;

static int q_empty(void) { return q_head == q_tail; }
static void q_push(char c) {
    int next = (q_tail + 1) % QUEUE_SIZE;
    if (next != q_head) { q_buf[q_tail] = c; q_tail = next; }
}
static char q_pop(void) {
    char c = q_buf[q_head];
    q_head = (q_head + 1) % QUEUE_SIZE;
    return c;
}

static void push_arrow(char letter) {
    q_push(27); q_push('['); q_push(letter);
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

static int translate_scancode_set2(uint8_t sc, char* out) {
    if (set2_break) {
        if (sc == 0x12 || sc == 0x59) shift_held = 0;
        if (sc == 0x14) ctrl_held = 0;
        if (sc == 0x11) alt_held = 0;
        set2_break = 0;
        set2_e0 = 0;
        return 0;
    }

    if (set2_e0) {
        if (sc == 0x75) { push_arrow('A'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x72) { push_arrow('B'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x74) { push_arrow('C'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x6B) { push_arrow('D'); *out = q_pop(); set2_e0 = 0; return 1; }
        if (sc == 0x14) { ctrl_held = 1; set2_e0 = 0; return 0; }
        if (sc == 0x11) { alt_held = 1; set2_e0 = 0; return 0; }  /* right alt */
        if (sc == 0x71) {                                          /* Delete key */
            if (ctrl_held && alt_held) ctrl_alt_del_pending = 1;
            set2_e0 = 0;
            return 0;
        }
        set2_e0 = 0;
        return 0;
    }

    if (sc == 0x12 || sc == 0x59) { shift_held = 1; return 0; }
    if (sc == 0x14) { ctrl_held = 1; return 0; }
    if (sc == 0x11) { alt_held = 1; return 0; }  /* left alt */

    char c = set2_map_char(sc, shift_held);
    if (!c) return 0;
    c = map_layout_char(c, shift_held);

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

    char c = shift_held ? sc_shift[sc] : sc_normal[sc];
    if (!c && !shift_held) c = sc_normal[sc];
    if (!c) return 0;
    c = map_layout_char(c, shift_held);

    /* Ctrl+letter → codes 1-26 */
    if (ctrl_held) {
        char base = (c >= 'A' && c <= 'Z') ? c :
                    (c >= 'a' && c <= 'z') ? (char)(c - 32) : 0;
        if (base) { *out = (char)(base - 'A' + 1); return 1; }
    }
    *out = c;
    return 1;
}

static int process_scancode_byte(uint8_t sc, char* out) {
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
        /* set1 extended arrows: E0 48/50/4D/4B */
        if (sc == 0x48) { push_arrow('A'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x50) { push_arrow('B'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x4D) { push_arrow('C'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x4B) { push_arrow('D'); *out = q_pop(); set1_e0 = 0; return 1; }
        if (sc == 0x1D) { ctrl_held = 1; set1_e0 = 0; return 0; }  /* right ctrl down */
        if (sc == 0x9D) { ctrl_held = 0; set1_e0 = 0; return 0; }  /* right ctrl up */
        if (sc == 0x38) { alt_held = 1; set1_e0 = 0; return 0; }   /* right alt down */
        if (sc == 0xB8) { alt_held = 0; set1_e0 = 0; return 0; }   /* right alt up */
        if (sc == 0x53) {                                          /* Delete key down */
            if (ctrl_held && alt_held) ctrl_alt_del_pending = 1;
            set1_e0 = 0;
            return 0;
        }
        set1_e0 = 0;
        return 0;
    }

    if (sc == 0x2A || sc == 0x36) { shift_held = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift_held = 0; return 0; }
    if (sc == 0x1D)               { ctrl_held  = 1; return 0; }
    if (sc == 0x9D)               { ctrl_held  = 0; return 0; }
    if (sc == 0x38)               { alt_held   = 1; return 0; }  /* left alt down */
    if (sc == 0xB8)               { alt_held   = 0; return 0; }  /* left alt up */
    if (sc & 0x80)                return 0; /* key-up */

    return translate_scancode(sc, out);
}

const char* keyboard_layout_name(void) {
    return kbd_layout_names[(int)g_layout];
}

const char* keyboard_layouts_help(void) {
    return "EN (Default), fr_CH, FR, DE, de_CH, BEPO";
}

int keyboard_set_layout(const char* name) {
    if (!name || !*name) return -1;

    if (strieq(name, "EN") || strieq(name, "EN (Default)") || strieq(name, "default")) {
        g_layout = KB_LAYOUT_EN_DEFAULT;
        return 0;
    }
    if (strieq(name, "fr_CH")) { g_layout = KB_LAYOUT_FR_CH; return 0; }
    if (strieq(name, "FR"))    { g_layout = KB_LAYOUT_FR;    return 0; }
    if (strieq(name, "DE"))    { g_layout = KB_LAYOUT_DE;    return 0; }
    if (strieq(name, "de_CH")) { g_layout = KB_LAYOUT_DE_CH; return 0; }
    if (strieq(name, "BEPO"))  { g_layout = KB_LAYOUT_BEPO;  return 0; }

    return -1;
}

/* ── raw scancode reader ─────────────────────────────────────────── */
static uint8_t read_sc(void) {
    while (1) {
        /* Real blocking wait: let other cooperative tasks (e.g. sysmon)
         * run while no scancode is available yet, instead of a pure
         * host-CPU-hogging spin. */
        while (!(inb(KB_STATUS_PORT) & 0x01)) task_yield();
        uint8_t st = inb(KB_STATUS_PORT);  /* re-read after data ready */
        if (st & 0x20) {
            /* AUX byte: route to mouse assembler so we don't break PS/2 mouse */
            uint8_t b = inb(KB_DATA_PORT);
            mouse_on_aux_byte(b);
            continue;
        }

        uint8_t sc = inb(KB_DATA_PORT);

        return sc;
    }
}

char keyboard_try_getchar(void) {
    if (!q_empty()) return q_pop();

    if (!(inb(KB_STATUS_PORT) & 0x01)) return 0; /* no data */
    uint8_t st = inb(KB_STATUS_PORT);
    if (st & 0x20) {
        uint8_t b = inb(KB_DATA_PORT);
        mouse_on_aux_byte(b);
        return 0;
    }

    uint8_t sc = inb(KB_DATA_PORT);

    char out = 0;
    if (process_scancode_byte(sc, &out)) return out;
    return 0;
}

void keyboard_init(void) {
    /*
     * Re-enable the first PS/2 port and translation.
     * USB legacy emulation in VMs/BIOS often depends on this state.
     */
    kb_wait_write();
    outb(KB_CMD_PORT, 0xAD); /* disable first port while editing cmd byte */
    /* Do NOT disable second port (mouse). It breaks PS/2 mouse in VMs. */

    /* drain output buffer */
    while (inb(KB_STATUS_PORT) & 0x01) inb(KB_DATA_PORT);

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

    shift_held = 0; ctrl_held = 0; alt_held = 0;
    use_set2 = 0; set2_break = 0; set2_e0 = 0; set1_e0 = 0;
    ctrl_alt_del_pending = 0;
    q_head = 0; q_tail = 0;
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
    while (1) {
        /* drain queue first */
        if (!q_empty()) return q_pop();

        uint8_t sc = read_sc();
        char out = 0;
        if (process_scancode_byte(sc, &out)) return out;
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
