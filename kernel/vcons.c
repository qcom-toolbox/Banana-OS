/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The consoles: tty1..tty6 on the screen, ttyS0 on the serial line (see
 * vcons.h). */
#define VT_KERNEL 1
#include "vcons.h"
#include "../apps/terminal/vt.h"
#include "pty.h"
#include "app.h"
#include "terminal.h"
#include "serial.h"
#include "keyboard.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "task.h"
#include "fs.h"
#include "pkg.h"

#ifdef __x86_64__

#define VC_SERIAL    VCONS_SCREENS
#define VC_ALL       (VCONS_SCREENS + 1)
#define VC_SCROLLBACK 500
#define SHELL_PATH   "/bin/bash"

typedef struct {
    pty_t*   pty;
    vt_t*    vt;                /* (the screens; made when first shown) */
    int      pid;               /* its shell; 0: none */
    int      wanted;            /* a shell runs here (tty1, the serial line: from the start; the others once shown) */
    uint32_t started, next;     /* when it started; when it may start again */
    int      quick;             /* shells that ended right after starting, in a row */
    int      fails;                /* bash could not be started (the first console: the built-in shell after 2) */
} vc_t;

static vc_t g_vc[VC_ALL];
static int g_on, g_cur, g_view, g_redraw = 1, g_cols, g_rows, g_gui;

/* the kernel's own text for the screen (terminal_console_sink): any task
 * writes it here, vcons_pump takes it to the console shown */
#define KRING 16384
static char g_kring[KRING];
static uint32_t g_kh, g_kt;

static void sink_put(void* ctx, const char* s, uint32_t n) {
    (void)ctx;
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (g_kh + 1) % KRING;
        if (next == g_kt) break;                    /* (full: the rest is dropped) */
        g_kring[g_kh] = s[i];
        g_kh = next;
    }
    if (fl & 0x200) __asm__ volatile("sti");
}

static uint32_t kring_take(char* out, uint32_t cap) {
    uintptr_t fl;
    uint32_t n = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    while (n < cap && g_kt != g_kh) {
        out[n++] = g_kring[g_kt];
        g_kt = (g_kt + 1) % KRING;
    }
    if (fl & 0x200) __asm__ volatile("sti");
    return n;
}

/* bash's readline reads $TERMCAP (the Terminal app gives the same: apps/terminal/main.c) */
static char g_termcap[] =
    "TERMCAP=xterm-256color|xterm with 256 colors:am:bs:km:mi:ms:xn:ut:co#80:it#8:li#24:Co#256:pa#32767:"
    "AB=\\E[48;5;%dm:AF=\\E[38;5;%dm:"
    "AL=\\E[%dL:DC=\\E[%dP:DL=\\E[%dM:DO=\\E[%dB:IC=\\E[%d@:K1=\\EOw:K2=\\EOu:K3=\\EOy:K4=\\EOq:K5=\\EOs:"
    "LE=\\E[%dD:RI=\\E[%dC:SF=\\E[%dS:SR=\\E[%dT:UP=\\E[%dA:ae=\\E(B:al=\\E[L:as=\\E(0:bl=^G:bt=\\E[Z:"
    "cd=\\E[J:ce=\\E[K:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:cr=\\r:cs=\\E[%i%d;%dr:ct=\\E[3g:dc=\\E[P:dl=\\E[M:"
    "do=\\n:ec=\\E[%dX:ei=\\E[4l:ho=\\E[H:im=\\E[4h:k1=\\EOP:k2=\\EOQ:k3=\\EOR:k4=\\EOS:k5=\\E[15~:"
    "k6=\\E[17~:k7=\\E[18~:k8=\\E[19~:k9=\\E[20~:kD=\\E[3~:kI=\\E[2~:kN=\\E[6~:kP=\\E[5~:kb=\\177:"
    "kd=\\EOB:ke=\\E[?1l\\E>:kh=\\EOH:@7=\\EOF:kl=\\EOD:kr=\\EOC:ks=\\E[?1h\\E=:ku=\\EOA:le=^H:mb=\\E[5m:"
    "md=\\E[1m:me=\\E[0m:mh=\\E[2m:mr=\\E[7m:nd=\\E[C:nw=\\EE:rc=\\E8:sc=\\E7:se=\\E[27m:sf=\\n:"
    "so=\\E[7m:sr=\\EM:st=\\EH:ta=^I:te=\\E[?1049l:ti=\\E[?1049h:ue=\\E[24m:up=\\E[A:us=\\E[4m:"
    "vb=\\E[?5h\\E[?5l:ve=\\E[?12l\\E[?25h:vi=\\E[?25l:vs=\\E[?12;25h:";
static char* g_env[] = {
    "HOME=/home/banana", "USER=banana", "LOGNAME=banana", "PATH=/bin:/usr/bin:/apps",
    "LANG=C.UTF-8", "SHELL=/bin/bash", "TMPDIR=/tmp", "TERM=xterm-256color", g_termcap, NULL
};

/* ── colours: the emulator's (256, 24-bit) as the console's 16 ── */
static const uint32_t VGA_RGB[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static const uint8_t ANSI_VGA[16] = { 0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15 };

static uint8_t nearest(uint32_t rgb) {
    int r = (int)(rgb >> 16 & 0xFF), g = (int)(rgb >> 8 & 0xFF), b = (int)(rgb & 0xFF);
    int best = 0;
    long bd = -1;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)(VGA_RGB[i] >> 16 & 0xFF), dg = g - (int)(VGA_RGB[i] >> 8 & 0xFF), db = b - (int)(VGA_RGB[i] & 0xFF);
        long d = (long)dr * dr * 3 + (long)dg * dg * 4 + (long)db * db * 2;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

static uint8_t vga_color(uint32_t c, uint8_t def) {
    if (!(c & (VT_PAL | VT_RGB))) return def;
    if ((c & VT_PAL) && (c & 0xFF) < 16) return ANSI_VGA[c & 0xFF];
    static uint8_t cache[256];
    static uint8_t have[256];
    if (c & VT_PAL) {
        unsigned i = c & 0xFF;
        if (!have[i]) { cache[i] = nearest(vt_rgb(c, 0)); have[i] = 1; }
        return cache[i];
    }
    return nearest(c & 0xFFFFFF);
}

/* ── the screen ── */
static void render(void) {
    vc_t* v = &g_vc[g_cur];
    vt_t* vt = v->vt;
    if (!vt) return;
    int full = g_redraw || vt->all_dirty || g_view;
    for (int r = 0; r < g_rows; r++) {
        if (!full && (r >= vt->rows || !vt->dirty[r])) continue;
        const vt_line_t* l = r < vt->rows ? vt_get_line(vt, r - g_view) : NULL;
        for (int c = 0; c < g_cols; c++) {
            if (!l || c >= l->len) { terminal_console_cell(r, c, ' ', 7, 0); continue; }
            const vt_cell_t* k = &l->c[c];
            uint32_t fgc = k->fg;
            if ((k->attr & VT_BOLD) && (fgc & VT_PAL) && (fgc & 0xFF) < 8) fgc = VT_PAL | ((fgc & 0xFF) + 8);
            uint8_t fg = vga_color(fgc, (k->attr & VT_BOLD) ? 15 : 7), bg = vga_color(k->bg, 0);
            if (!!(k->attr & VT_REVERSE) != !!vt->reverse_video) { uint8_t t = fg; fg = bg; bg = t; }
            if (k->attr & VT_INVISIBLE) fg = bg;
            uint32_t ch = k->attr & VT_WIDE_CONT ? TERMINAL_WIDE_CONT : k->ch ? k->ch : ' ';
            terminal_console_cell(r, c, ch, fg, bg);
        }
        if (r < vt->rows) vt->dirty[r] = 0;
    }
    vt->all_dirty = 0;
    terminal_console_cursor(vt->cur.y, vt->cur.x < g_cols ? vt->cur.x : g_cols - 1, vt->cursor_visible && !g_view);
    if (g_redraw) terminal_console_repaint();          /* (every pixel: what the desktop left there goes) */
    else terminal_console_flush();
    g_redraw = 0;
}

static vt_t* screen_vt(int i) {
    vc_t* v = &g_vc[i];
    if (!v->vt) {
        v->vt = vt_new(g_cols, g_rows, VC_SCROLLBACK);
        if (v->vt) app_pty_resize(v->pty, g_cols, g_rows);
    }
    return v->vt;
}

/* bash cannot run on the first console or the serial line (damaged on the disk, it will not
 * start, or it ends at once again and again): the console goes back to the
 * kernel's own shell (shell/shell.c), which nothing on the disk can break */
static int  g_broken;
static char g_broken_why[96];
int vcons_broken(const char** why) { if (why) *why = g_broken_why; return g_broken; }
static void give_up(const char* why) {
    if (g_broken) return;
    kstrlcpy(g_broken_why, why, sizeof(g_broken_why));
    klog("console: %s - the built-in shell takes over\n", why);
    g_broken = 1;
}

static void spawn(int i) {
    vc_t* v = &g_vc[i];
    char* argv[] = { "-bash", NULL };                  /* (a login shell: /etc/profile, ~/.bash_profile) */
    char err[64];
    uint32_t now = timer_ms();
    int pid = app_spawn_tty(SHELL_PATH, 1, argv, g_env, v->pty, err, sizeof(err));
    if (pid < 0) {
        char m[128];
        ksnprintf(m, sizeof(m), "\r\nconsole: cannot start %s: %s\r\n", SHELL_PATH, err);
        if (i == VC_SERIAL) serial_write(m);
        else if (v->vt) vt_write(v->vt, m, strlen(m));
        v->next = now + 5000;
        if ((i == 0 || i == VC_SERIAL) && ++v->fails >= 2) {
            char why[96];
            ksnprintf(why, sizeof(why), "bash cannot start (%s)", err);
            give_up(why);
        }
        return;
    }
    v->pid = pid;
    v->started = now;
}

/* a shell that is not there (ended, or never started) started; one that
 * keeps ending right away (a broken ~/.bashrc...) waits a while first */
static int respawn(void) {
    int did = 0;
    uint32_t now = timer_ms();
    for (int i = 0; i < VC_ALL; i++) {
        vc_t* v = &g_vc[i];
        if (!v->wanted || !v->pty) continue;
        if (v->pid && app_alive(v->pid)) continue;
        if (v->pid) {                                   /* it ended */
            v->pid = 0;
            v->quick = now - v->started < 2000 ? v->quick + 1 : 0;
            v->next = now + (v->quick >= 3 ? 10000 : 0);
            pty_flush(v->pty, 2);
            if (v->quick >= 3 && (i == 0 || i == VC_SERIAL)) give_up("bash ends as soon as it starts");
            if (v->quick >= 3) {
                const char* m = "\r\nconsole: the shell keeps ending - starting it again in 10 seconds\r\n";
                if (i == VC_SERIAL) serial_write(m);
                else if (v->vt) vt_write(v->vt, m, strlen(m));
            }
        }
        if ((int32_t)(now - v->next) < 0) continue;
        if (i != VC_SERIAL && !screen_vt(i)) continue;
        spawn(i);
        did = 1;
    }
    return did;
}

int vcons_running(void) { return g_on; }
char** vcons_env(void) { return g_env; }
int vcons_current(void) { return g_cur; }

int vcons_minor(pty_t* t) {
    if (!g_on || !t) return 0;
    for (int i = 0; i < VC_ALL; i++) if (g_vc[i].pty == t) return i == VC_SERIAL ? 64 : i + 1;
    return 0;
}

pty_t* vcons_pty(int n) {
    return g_on && n >= 0 && n < VC_ALL ? g_vc[n].pty : NULL;
}

pty_t* vcons_tty(const char* name) {
    if (!g_on || strncmp(name, "tty", 3)) return NULL;
    name += 3;
    if (!strcmp(name, "S0")) return g_vc[VC_SERIAL].pty;
    if (name[0] >= '0' && name[0] <= '0' + VCONS_SCREENS && !name[1])
        return g_vc[name[0] == '0' ? g_cur : name[0] - '1'].pty;
    return NULL;
}

void vcons_switch(int n) {
    if (!g_on || n < 0 || n >= VCONS_SCREENS) return;
    if (!screen_vt(n)) return;
    g_cur = n;
    g_view = 0;
    g_redraw = 1;
    g_vc[n].wanted = 1;
    respawn();
    render();
}

/* what was on the screen goes into tty1's emulator: the boot's messages stay */
static void seed(vt_t* vt) {
    int crow, ccol;
    terminal_console_get_cursor(&crow, &ccol);
    if (crow >= g_rows) crow = g_rows - 1;
    char buf[64];
    int lfg = -1, lbg = -1;
    for (int r = 0; r <= crow; r++) {
        int last = -1;
        for (int c = 0; c < g_cols; c++) {
            uint32_t cp; uint8_t fg, bg;
            terminal_console_read(r, c, &cp, &fg, &bg);
            if ((cp != ' ' && cp) || bg) last = c;
        }
        if (r == crow && last < ccol - 1) last = ccol - 1;
        for (int c = 0; c <= last; c++) {
            uint32_t cp; uint8_t fg, bg;
            terminal_console_read(r, c, &cp, &fg, &bg);
            if (cp == TERMINAL_WIDE_CONT) continue;
            if (fg != lfg || bg != lbg) {
                static const uint8_t VGA_ANSI[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
                ksnprintf(buf, sizeof(buf), "\x1b[0;%u;%um", (fg & 8 ? 90u : 30u) + VGA_ANSI[fg & 7], (bg & 8 ? 100u : 40u) + VGA_ANSI[bg & 7]);
                vt_write(vt, buf, strlen(buf));
                lfg = fg; lbg = bg;
            }
            int n = 0;
            if (!cp) cp = ' ';
            if (cp < 0x80) buf[n++] = (char)cp;
            else if (cp < 0x800) { buf[n++] = (char)(0xC0 | cp >> 6); buf[n++] = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { buf[n++] = (char)(0xE0 | cp >> 12); buf[n++] = (char)(0x80 | (cp >> 6 & 63)); buf[n++] = (char)(0x80 | (cp & 63)); }
            else { buf[n++] = (char)(0xF0 | cp >> 18); buf[n++] = (char)(0x80 | (cp >> 12 & 63)); buf[n++] = (char)(0x80 | (cp >> 6 & 63)); buf[n++] = (char)(0x80 | (cp & 63)); }
            vt_write(vt, buf, (size_t)n);
        }
        if (r < crow) vt_write(vt, "\r\n", 2);
    }
    vt_write(vt, "\x1b[0m", 4);
}

/* login shells read /etc/profile: the system's bashrc (a prompt, colours)
 * and ~/.bashrc, as for a shell in the Terminal */
static const char PROFILE[] =
    "# /etc/profile - read by login shells (bash on the consoles: tty1..6, ttyS0)\n"
    "if [ -n \"$BASH_VERSION\" ]; then\n"
    "    case $- in *i*)\n"
    "        [ -r /apps/bash/bashrc ] && . /apps/bash/bashrc\n"
    "        [ -r ~/.bashrc ] && . ~/.bashrc ;;\n"
    "    esac\n"
    "fi\n";

int vcons_start(void) {
    if (g_on) return 0;
    if (fs_find_file(SHELL_PATH) < 0) { give_up("bash is not installed"); return -1; }
    /* (the programs' contents, as pkg verify checks them: a bash that is the
     * right size but damaged - "exec format error" - is not started) */
    char first[64];
    if (pkg_verify("bash", 2, first, sizeof(first)) > 0) {
        char why[96];
        ksnprintf(why, sizeof(why), "bash is damaged on the disk (%s)", first);
        give_up(why);
        return -1;
    }
    pkg_bin_sync();                                     /* (the apps installed: commands in /bin) */
    if (fs_find_file("/etc/profile") < 0) {
        fs_mkdir_p("/etc");
        fs_write_path("/etc/profile", PROFILE, (uint32_t)(sizeof(PROFILE) - 1));
    }
    terminal_console_size(&g_cols, &g_rows);
    for (int i = 0; i < VC_ALL; i++) {
        g_vc[i].pty = pty_new();
        if (!g_vc[i].pty) return -1;                    /* (the ones made: kept, for the next try) */
        pty_set_locked(g_vc[i].pty, 0);
        app_pty_resize(g_vc[i].pty, i == VC_SERIAL ? 80 : g_cols, i == VC_SERIAL ? 24 : g_rows);
    }
    vt_t* vt = screen_vt(0);
    if (!vt) return -1;
    seed(vt);
    g_cur = 0;
    g_on = 1;
    g_vc[0].wanted = 1;
    g_vc[VC_SERIAL].wanted = 1;
    keyboard_serial_input(0);                           /* (the serial line is ttyS0's now) */
    terminal_console_sink(sink_put, NULL, g_cols, g_rows);
    g_redraw = 1;
    respawn();
    render();
    return 0;
}

int vcons_pump(void) {
    if (!g_on) return 0;
    int busy = 0;
    static char buf[4096];

    /* the screen's size (a new resolution, the font's scale) */
    int cols, rows;
    terminal_console_size(&cols, &rows);
    if (cols != g_cols || rows != g_rows) {
        g_cols = cols;
        g_rows = rows;
        for (int i = 0; i < VCONS_SCREENS; i++) {
            if (g_vc[i].vt) vt_resize(g_vc[i].vt, cols, rows);
            app_pty_resize(g_vc[i].pty, cols, rows);
        }
        terminal_console_sink_size(cols, rows);
        g_redraw = 1;
    }
    /* back from the desktop: all of it again */
    int gui = terminal_get_mode() != TERMINAL_MODE_FRAMEBUFFER && terminal_get_mode() != TERMINAL_MODE_VGA_TEXT;
    if (gui != g_gui) { g_gui = gui; g_redraw = 1; }

    /* the kernel's text (\n: a new line) */
    for (;;) {
        char in[512];
        uint32_t n = kring_take(in, sizeof(in));
        if (!n) break;
        uint32_t o = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (in[i] == '\n') buf[o++] = '\r';
            buf[o++] = in[i];
        }
        if (g_vc[g_cur].vt) vt_write(g_vc[g_cur].vt, buf, o);
        busy = 1;
    }

    /* what the programs wrote */
    for (int i = 0; i < VC_ALL; i++) {
        vc_t* v = &g_vc[i];
        for (int k = 0; k < 8; k++) {                   /* (32 KiB at most each time round: the keys stay quick) */
            long n = pty_master_read(v->pty, (uint8_t*)buf, sizeof(buf));
            if (n <= 0) break;
            busy = 1;
            if (i == VC_SERIAL) {
                for (long j = 0; j < n; j++) serial_putc(buf[j]);
                continue;
            }
            if (!v->vt) continue;
            vt_write(v->vt, buf, (size_t)n);
            if (v->vt->reply_len) {                     /* (answers: the cursor's position...) */
                app_pty_type(v->pty, (const uint8_t*)v->vt->reply, (unsigned long)v->vt->reply_len);
                v->vt->reply_len = 0;
            }
        }
    }

    /* typed on the serial line */
    {
        uint8_t in[256];
        int n = 0, b;
        while (n < (int)sizeof(in) && (b = serial_try_getc()) >= 0) in[n++] = (uint8_t)b;
        if (n) { app_pty_type(g_vc[VC_SERIAL].pty, in, (unsigned long)n); busy = 1; }
    }

    if (respawn()) busy = 1;

    if (!g_gui) {
        vt_t* vt = g_vc[g_cur].vt;
        int dirty = g_redraw || (vt && vt->all_dirty);
        for (int r = 0; vt && !dirty && r < vt->rows; r++) dirty = vt->dirty[r];
        static int ly = -1, lx = -1, lv = -1;
        if (vt && (vt->cur.y != ly || vt->cur.x != lx || vt->cursor_visible != lv)) { dirty = 1; ly = vt->cur.y; lx = vt->cur.x; lv = vt->cursor_visible; }
        if (dirty) render();
    }
    return busy;
}

void vcons_key(int key, int mods) {
    if (!g_on) return;
    vc_t* v = &g_vc[g_cur];
    if (!v->vt) return;
    /* Shift+PgUp / PgDn: the scrollback */
    if ((mods & VT_MOD_SHIFT) && (key == VT_KEY_PGUP || key == VT_KEY_PGDN) && !v->vt->alt_on) {
        int step = g_rows / 2 > 0 ? g_rows / 2 : 1;
        g_view += key == VT_KEY_PGUP ? step : -step;
        if (g_view > v->vt->sb_count) g_view = v->vt->sb_count;
        if (g_view < 0) g_view = 0;
        g_redraw = 1;
        render();
        return;
    }
    if (g_view) { g_view = 0; g_redraw = 1; }
    char out[32];
    int n;
    if (key >= 0x100) n = vt_key_seq(v->vt, key, mods, out);
    else { out[0] = (char)key; n = 1; }
    if (n > 0) app_pty_type(v->pty, (const uint8_t*)out, (unsigned long)n);
}

void vcons_fkey(int code) {
    int f = KEYF_CODE(code);
    if (f < KEYF_F1 || f >= KEYF_F1 + 12) return;
    int i = f - KEYF_F1;
    if ((code & KEYF_ALT) && i < VCONS_SCREENS) { vcons_switch(i); return; }
    int mods = (code & KEYF_SHIFT ? VT_MOD_SHIFT : 0) | (code & KEYF_CTRL ? VT_MOD_CTRL : 0) | (code & KEYF_ALT ? VT_MOD_ALT : 0);
    vcons_key(VT_KEY_F1 + i, mods);
}

/* the consoles off: the screen, the keyboard and the serial line back to
 * the kernel's shell (bash broken: vcons_broken) */
void vcons_stop(void) {
    if (!g_on) return;
    g_on = 0;
    terminal_console_sink(NULL, NULL, 0, 0);
    keyboard_serial_input(1);
}

#else   /* the 32-bit kernel: its own shell on the console */
int  vcons_broken(const char** why) { if (why) *why = ""; return 0; }
void vcons_stop(void) {}

int  vcons_start(void) { return -1; }
int  vcons_running(void) { return 0; }
char** vcons_env(void) { return NULL; }
int  vcons_pump(void) { return 0; }
void vcons_key(int key, int mods) { (void)key; (void)mods; }
void vcons_fkey(int code) { (void)code; }
void vcons_switch(int n) { (void)n; }
int  vcons_current(void) { return 0; }
struct pty* vcons_tty(const char* name) { (void)name; return NULL; }
struct pty* vcons_pty(int n) { (void)n; return NULL; }
int vcons_minor(struct pty* t) { (void)t; return 0; }

#endif
