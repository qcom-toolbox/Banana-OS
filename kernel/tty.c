#include "tty.h"
#include "terminal.h"
#include "task.h"
#include "timer.h"
#include "kstring.h"

#define IN_SIZE  1024u
#define OUT_SIZE 32768u

typedef struct {
    int      used;
    int      vt;
    int      pid;                       /* the shell task, -1 until bound */
    volatile int attached, hungup, finished;
    char     user[32];
    char     exec[256];
    uint8_t  in[IN_SIZE];
    uint32_t in_head, in_len;
    int      last_cr;
    uint8_t  out[OUT_SIZE];
    uint32_t out_head, out_len;
} tty_t;

static tty_t g_tty[TTY_MAX];
static signed char g_vt_tty[TERMINAL_VT_MAX];
static int g_vt_map_init = 0;

static void map_init(void) {
    if (g_vt_map_init) return;
    for (int i = 0; i < TERMINAL_VT_MAX; i++) g_vt_tty[i] = -1;
    g_vt_map_init = 1;
}

int tty_create(void) {
    map_init();
    for (int t = 0; t < TTY_MAX; t++) {
        if (g_tty[t].used) continue;
        int vt = terminal_vt_alloc();
        if (vt < 0) return -1;
        memset(&g_tty[t], 0, sizeof(g_tty[t]));
        g_tty[t].used = 1;
        g_tty[t].vt = vt;
        g_tty[t].pid = -1;
        g_vt_tty[vt] = (signed char)t;
        return t;
    }
    return -1;
}

static tty_t* get(int t) {
    return (t >= 0 && t < TTY_MAX && g_tty[t].used) ? &g_tty[t] : NULL;
}

int tty_vt(int t) { tty_t* y = get(t); return y ? y->vt : 0; }

void tty_bind(int t) {
    tty_t* y = get(t);
    if (y) y->pid = task_current_pid();
}

int tty_current(void) {
    int pid = task_current_pid();
    for (int t = 0; t < TTY_MAX; t++)
        if (g_tty[t].used && g_tty[t].pid == pid) return t;
    return -1;
}

/* ── server side ─────────────────────────────────────────────────── */

void tty_attach(int t, const char* user, const char* exec_cmd) {
    tty_t* y = get(t);
    if (!y) return;
    y->in_head = y->in_len = 0;
    y->out_head = y->out_len = 0;
    y->last_cr = 0;
    kstrlcpy(y->user, user ? user : "", sizeof(y->user));
    kstrlcpy(y->exec, exec_cmd ? exec_cmd : "", sizeof(y->exec));
    y->hungup = 0;
    y->finished = 0;
    y->attached = 1;
    if (y->pid >= 0) task_wake(y->pid);
}

void tty_hangup(int t) { tty_t* y = get(t); if (y) y->hungup = 1; }

void tty_detach(int t) {
    tty_t* y = get(t);
    if (!y) return;
    y->attached = 0;
    y->hungup = 0;
}

int tty_in_use(int t) { tty_t* y = get(t); return y ? y->attached : 0; }
int tty_finished(int t) { tty_t* y = get(t); return y ? y->finished : 1; }

void tty_input(int t, const uint8_t* d, uint32_t n) {
    tty_t* y = get(t);
    if (!y) return;
    for (uint32_t i = 0; i < n && y->in_len < IN_SIZE; i++) {
        y->in[(y->in_head + y->in_len) % IN_SIZE] = d[i];
        y->in_len++;
    }
    if (y->pid >= 0) task_wake(y->pid);
}

uint32_t tty_output(int t, uint8_t* out, uint32_t max) {
    tty_t* y = get(t);
    if (!y) return 0;
    uint32_t n = 0;
    while (n < max && y->out_len) {
        out[n++] = y->out[y->out_head];
        y->out_head = (y->out_head + 1) % OUT_SIZE;
        y->out_len--;
    }
    return n;
}

uint32_t tty_output_pending(int t) { tty_t* y = get(t); return y ? y->out_len : 0; }

/* ── shell side ──────────────────────────────────────────────────── */

int tty_active(int t) { tty_t* y = get(t); return y && y->attached && !y->hungup && !y->finished; }
const char* tty_exec_cmd(int t) { tty_t* y = get(t); return y ? y->exec : ""; }
const char* tty_user(int t) { tty_t* y = get(t); return y ? y->user : ""; }
void tty_finish(int t) { tty_t* y = get(t); if (y) y->finished = 1; }

char tty_getkey(int t) {
    tty_t* y = get(t);
    if (!y) return 0;
    /* a dead session reads as Ctrl+C, so whatever command is waiting for
     * input (ping, a pager, a y/n question) gives up and the shell ends */
    if (!y->attached || y->hungup || y->finished) return 3;
    while (y->in_len) {
        uint8_t b = y->in[y->in_head];
        y->in_head = (y->in_head + 1) % IN_SIZE;
        y->in_len--;
        /* terminals send CR (or CR LF) for Enter and DEL for Backspace */
        if (b == '\n' && y->last_cr) { y->last_cr = 0; continue; }
        y->last_cr = (b == '\r');
        if (b == '\r' || b == '\n') return '\n';
        if (b == 0x7F) return '\b';
        if (b == 0) continue;
        return (char)b;
    }
    return 0;
}

static void out_put(tty_t* y, uint8_t c) {
    if (y->out_len == OUT_SIZE) {
        /* full: the shell writing its own output waits for the network to
         * drain it (the SSH task runs meanwhile); anyone else drops */
        if (y->pid != task_current_pid()) return;
        uint32_t start = timer_ms();
        while (y->out_len == OUT_SIZE && y->attached && !y->hungup && timer_ms() - start < 10000)
            task_sleep_ms(2);
        if (y->out_len == OUT_SIZE) return;
    }
    y->out[(y->out_head + y->out_len) % OUT_SIZE] = c;
    y->out_len++;
}

void tty_write(int t, const char* s) {
    tty_t* y = get(t);
    if (!y || !y->attached || y->hungup) return;
    while (*s) out_put(y, (uint8_t)*s++);
}

void tty_mirror(int vt, char c) {
    if (vt < 0 || vt >= TERMINAL_VT_MAX || !g_vt_map_init) return;
    int t = g_vt_tty[vt];
    if (t < 0) return;
    tty_t* y = &g_tty[t];
    if (!y->attached || y->hungup) return;
    if (c == '\n') out_put(y, '\r');
    out_put(y, (uint8_t)c);
}

void tty_clear_screen(int vt) {
    if (vt < 0 || vt >= TERMINAL_VT_MAX || !g_vt_map_init) return;
    int t = g_vt_tty[vt];
    if (t >= 0) tty_write(t, "\x1b[H\x1b[2J");
}
