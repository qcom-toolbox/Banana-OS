/* SPDX-License-Identifier: BSD-2-Clause */
/* shelltest -i: bash and zsh as a person uses them, on a pseudo-terminal
 * of 100 x 30 (TERM=xterm-256color, their rc files read as in the Terminal
 * app): keys are typed one by one, and what the shell shows - its escapes
 * taken out - must have the text a check waits for, within a few seconds.
 * Line editing, history, Tab completion, ^R, ^C, ^Z and fg, ^D, here a
 * program's window size, less and a prompt for input. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_m = -1, g_pass, g_fail, g_verbose;
static pid_t g_pid;
static char g_out[65536];              /* what the shell showed since the last mark, escapes out */
static int g_len;
static unsigned long g_received;
/* Failure-only diagnostics for this test PTY, never global terminal input. */
static unsigned char g_raw[2048],g_typed[128];
static unsigned g_raw_head,g_raw_count,g_typed_head,g_typed_count;
static long g_last_input,g_last_output;
static void trace_add(unsigned char* bytes,unsigned capacity,unsigned* head,unsigned* count,
                      const char* data,unsigned n) {
    for (unsigned i=0;i<n;i++) {
        bytes[*head]=(unsigned char)data[i];*head=(*head+1)%capacity;
        if (*count<capacity) (*count)++;
    }
}
static void trace_print(FILE* out,const unsigned char* bytes,unsigned capacity,unsigned head,unsigned count) {
    unsigned first=(head+capacity-count)%capacity;
    for (unsigned i=0;i<count;i++) {
        unsigned char c=bytes[(first+i)%capacity];
        if(c>=32&&c<127&&c!='\\')fputc(c,out);
        else fprintf(out,"\\x%02x",c);
    }
    fputc('\n',out);
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* the shell's output, without CSI / OSC escapes and \r (the text a person reads) */
static int g_esc, g_osc;
static void take(const char* b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (g_osc) { if (c == 7 || c == '\\') g_osc = 0; continue; }
        if (g_esc == 1) {
            if (c == '[') { g_esc = 2; continue; }
            if (c == ']') { g_esc = 0; g_osc = 1; continue; }
            g_esc = (c == '(' || c == ')' || c == '#') ? 3 : 0;
            continue;
        }
        if (g_esc == 3) { g_esc = 0; continue; }
        if (g_esc == 2) { if (c >= 0x40 && c <= 0x7E) g_esc = 0; continue; }
        if (c == 27) { g_esc = 1; continue; }
        if (c == '\r' || c == 7) continue;
        if (c == 8) { if (g_len) g_len--; continue; }       /* (backspace: as the screen shows it) */
        if (g_len < (int)sizeof(g_out) - 1) g_out[g_len++] = (char)c;
    }
    g_out[g_len] = 0;
}

static void pump(int ms) {
    long end = now_ms() + ms;
    for (;;) {
        if (now_ms() >= end) return;
        long left = end - now_ms();
        if (left < 0) left = 0;
        struct pollfd p = { g_m, POLLIN, 0 };
        if (poll(&p, 1, (int)left) > 0 && (p.revents & POLLIN)) {
            char b[4096];
            int n = (int)read(g_m, b, sizeof b);
            if (n > 0) { g_received += (unsigned long)n; g_last_output=now_ms();
                trace_add(g_raw,sizeof(g_raw),&g_raw_head,&g_raw_count,b,(unsigned)n); take(b, n); continue; }
        }
        if (now_ms() >= end) return;
    }
}

static void mark(void) { g_len = 0; g_out[0] = 0; }

/* waits for text in the output (ms at most): 1 if it came */
static int wait_for(const char* text, int ms) {
    long end = now_ms() + ms;
    while (!strstr(g_out, text)) {
        if (now_ms() > end) return 0;
        pump(50);
    }
    return 1;
}

/* Seeing a filename/help heading does not mean a full-screen program is
 * ready for input: startup and redraw may still be changing the terminal.
 * Wait for raw/no-echo mode and a quiet output interval, within a fixed bound. */
static int editor_ready(int ms) {
    long end = now_ms() + ms, quiet_since = now_ms();
    unsigned long received = g_received;
    int was_raw = 0;
    while (now_ms() < end) {
        struct termios t;
        if (tcgetattr(g_m, &t) != 0) return 0;
        int raw = !(t.c_lflag & (ICANON | ECHO));
        if (!raw || !was_raw) quiet_since = now_ms();
        was_raw = raw;
        if (raw && now_ms() - quiet_since >= 100) return 1;
        pump(20);
        if (g_received != received) { received = g_received; quiet_since = now_ms(); }
    }
    return 0;
}

static int write_all(int fd,const char* bytes,size_t length) {
    while (length) {
        ssize_t n=write(fd,bytes,length);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        bytes+=n;length-=(size_t)n;
    }
    return 0;
}

static void type(const char* s) {
    for (; *s; s++) {
        if (write_all(g_m,s,1)!=0) {
            printf("  FAIL  PTY input write (errno %d)\n",errno);g_fail++;return;
        }
        g_last_input=now_ms();
        trace_add(g_typed,sizeof(g_typed),&g_typed_head,&g_typed_count,s,1);
        pump(15);
    }
}

static void check(const char* what, int ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        /* what was there instead: its last lines */
        const char* t = g_out + (g_len > 300 ? g_len - 300 : 0);
        printf("        (the screen had: ");
        for (; *t; t++) putchar(*t == '\n' ? '|' : *t);
        printf(")\n");
    }
    if (ok) g_pass++; else g_fail++;
}

static int start(const char* shell, char* const argv[]) {
    struct winsize ws = { 30, 100, 0, 0 };
    char* env[] = { "HOME=/home/banana", "USER=banana", "LOGNAME=banana", "PATH=/bin:/usr/bin",
                    "TERM=xterm-256color", "LANG=C.UTF-8", "SHELL=/bin/bash", "TMPDIR=/tmp", NULL };
    g_pid = forkpty(&g_m, NULL, NULL, &ws);
    if (g_pid < 0) return -1;
    if (g_pid == 0) {
        if (chdir("/home/banana")!=0) _exit(126);
        execve(shell, argv, env);
        _exit(127);
    }
    g_esc=g_osc=0;
    g_raw_head=g_raw_count=g_typed_head=g_typed_count=0;
    g_last_input=g_last_output=now_ms();
    mark();
    return 0;
}

/* Query only: keep queued bytes intact for diagnosis before terminating the
 * timed-out child. Escaping preserves redraw/control bytes without executing
 * them on the CI console or mistaking a cursor redraw for duplicated input. */
static void pty_diagnostic(FILE* out) {
    struct termios t;
    if(tcgetattr(g_m,&t)==0)
        fprintf(out,"        PTY input=%x local=%x VMIN=%u VTIME=%u\n",
                (unsigned)t.c_iflag,(unsigned)t.c_lflag,(unsigned)t.c_cc[VMIN],(unsigned)t.c_cc[VTIME]);
    char* path=ptsname(g_m);
    int slave=path?open(path,O_RDONLY|O_NONBLOCK|O_NOCTTY):-1,queued=-1;
    if(slave>=0){if(ioctl(slave,FIONREAD,&queued)<0)queued=-1;close(slave);}
    fprintf(out,"        PTY queued input=%d, input age=%ld ms, output age=%ld ms\n",
            queued,now_ms()-g_last_input,now_ms()-g_last_output);
    fputs("        PTY sent (escaped): ",out);
    trace_print(out,g_typed,sizeof(g_typed),g_typed_head,g_typed_count);
    fputs("        PTY raw output (escaped): ",out);
    trace_print(out,g_raw,sizeof(g_raw),g_raw_head,g_raw_count);
}

static int finish(void) {
    int st = -1;
    for (int i = 0; i < 100; i++) {
        pump(20);
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) break;
    }
    if (waitpid(g_pid, &st, WNOHANG) == 0) {
        pty_diagnostic(stdout);
        /* still there: what it and its children are doing, for the report */
        fflush(stdout);
        if (access("/bin/ps", X_OK) == 0) {
            int diagnostic=system("ps -eo pid,ppid,pgid,tty,tpgid,stat,sched,wchan,uip,pending,args | sed 's/^/        | /'");
            if (diagnostic<0) printf("        (process diagnostics unavailable)\n");
        }
        kill(g_pid, SIGKILL); waitpid(g_pid, &st, 0); st = -1;
    }
    close(g_m);
    g_m = -1;
    return st;
}

/* a prompt is back: the rc files' prompt ends "$ " (bash) or "% " (zsh) */
static int prompt(const char* p, int ms) { return wait_for(p, ms); }

static int g_steps = -1;                 /* (shelltest -b N: only N steps of bash_checks, then ^D) */
#define STEP() do { if (g_steps >= 0 && g_steps-- == 0) goto eof; } while (0)
static void bash_checks(void) {
    printf("bash -i\n");
    char* av[] = { "bash", "-i", NULL };
    /* ^D first, alone: a fresh bash ends */
    if (start("/bin/bash", av)) { check("forkpty + bash", 0); return; }
    prompt("$ ", 8000);
    mark(); type("\x04");
    int st0 = finish();
    check("^D at a fresh prompt: bash ends (exit)", st0 >= 0 && WIFEXITED(st0));
    if (start("/bin/bash", av)) { check("forkpty + bash", 0); return; }
    check("the prompt (from /apps/bash/bashrc)", prompt("banana@", 8000) && prompt("$ ", 2000));
    STEP();
    mark(); type("echo hello-$((6*7))\r");
    check("a command runs", wait_for("hello-42", 3000) && prompt("$ ", 3000));
    STEP();
    mark(); type("echo ac\x1b[Db\r");
    check("line editing: the left arrow, a character put in", wait_for("\nabc", 3000));
    STEP();
    mark(); type("cho one\x01" "e\x05" " two\r");                  /* ^A, e; ^E */
    check("^A / ^E: start and end of the line", wait_for("\none two", 3000));
    STEP();
    mark(); type("echo first-cmd\r"); prompt("$ ", 3000);
    STEP();
    mark(); type("\x1b[A\r");
    check("the up arrow: the last command again", wait_for("first-cmd\nfirst-cmd", 3000) || wait_for("echo first-cmd\nfirst-cmd", 100));
    STEP();
    mark(); type("\x12" "first\r");
    check("^R: reverse search in the history", wait_for("first-cmd", 3000) && prompt("$ ", 3000));
    STEP();
    mark(); type("ech\t hi-tab\r");
    check("Tab completes a command name (ech -> echo)", wait_for("hi-tab", 3000));
    STEP();
    mark(); type("ls /us\t\r");
    check("Tab completes a path (/us -> /usr/)", wait_for("ls /usr/", 3000) && wait_for("share", 3000));
    STEP();
    mark(); type("cd /tm\t\rpwd\r");
    check("cd with a completed path", wait_for("/tmp", 3000));
    STEP();
    mark(); type("sleep 20\r"); pump(500); type("\x03");
    check("^C stops a running program", prompt("$ ", 3000));
    STEP();
    mark(); type("echo status=$?\r");
    check("its status: 130", wait_for("status=130", 3000));
    STEP();
    mark(); type("sleep 20\r"); pump(500); type("\x1a");
    check("^Z stops it (Stopped)", wait_for("Stopped", 3000) && prompt("$ ", 3000));
    STEP();
    mark(); type("jobs\r");
    check("jobs lists it", wait_for("sleep 20", 3000));
    STEP();
    mark(); type("fg\r"); pump(500); type("\x03");
    check("fg, then ^C", prompt("$ ", 3000));
    STEP();
    mark(); type("kill %1 2>/dev/null; echo jobs-$(jobs | wc -l)\r");
    check("no job left", wait_for("jobs-0", 3000));
    STEP();
    mark(); type("stty size; tput cols\r");
    check("the window size: stty size, tput cols (30 100)", wait_for("30 100", 3000) && wait_for("\n100", 3000));
    STEP();
    mark(); type("read -p 'name? ' n; echo got-$n\r");
    check("read with a prompt", wait_for("name? ", 3000));
    type("banana\r");
    check("  ...and the answer", wait_for("got-banana", 3000));
    STEP();
    mark(); type("printf 'l1\\nl2\\nl3\\n' | less\r");
    check("less shows its input", wait_for("l3", 4000));
    type("q");
    check("q leaves less", prompt("$ ", 3000));
    STEP();
    mark(); type("ls --color=always -d /bin | cat -v | head -1\r");
    check("ls colours (an escape in its output)", wait_for("^[[", 3000));
    STEP();
    mark(); type("echo $SHELL $TERM\r");
    check("$SHELL and $TERM", wait_for("/bin/bash xterm-256color", 3000));
    STEP();
    mark(); type("history | tail -1\r");
    check("history", wait_for("history | tail -1", 3000));
eof:
    mark(); type("\x04");
    int st = finish();
    check("^D on an empty line: bash ends", st >= 0 && WIFEXITED(st));
}

/* a fresh bash: keys, then ^D at its prompt must end it (what can leave
 * the terminal or readline in a state where ^D no longer does?) */
static void eof_after(const char* what, const char* keys, const char* wait) {
    char* av[] = { "bash", "-i", NULL };
    if (start("/bin/bash", av)) { check("forkpty + bash", 0); return; }
    prompt("$ ", 8000);
    mark();
    for (const char* k = keys; *k; ) {               /* (keys: pieces split by \001\001: a pause between) */
        const char* e = strstr(k, "\001\001");
        int n = e ? (int)(e - k) : (int)strlen(k);
        for (int i = 0; i < n; i++) { char key[2]={k[i],0};type(key); }
        pump(700);
        k += n + (e ? 2 : 0);
    }
    if (wait) wait_for(wait, 4000);
    prompt("$ ", 3000);
    pump(300);
    mark(); type("\x04");
    int st = finish();
    char label[160];
    snprintf(label, sizeof label, "^D ends bash after %s", what);
    check(label, st >= 0 && WIFEXITED(st));
}

static void zsh_checks(void) {
    printf("zsh -i\n");
    if (access("/bin/zsh", X_OK) != 0) { printf("  (no /bin/zsh: skipped)\n"); return; }
    char* av[] = { "zsh", "-i", NULL };
    if (start("/bin/zsh", av)) { check("forkpty + zsh", 0); return; }
    long t0 = now_ms();
    int up = prompt("banana@", 30000) && prompt("% ", 3000);
    char what[96];
    snprintf(what, sizeof what, "the prompt (from /apps/zsh/zshrc), after %ld ms", now_ms() - t0);
    check(what, up);
    pump(300);
    mark(); type("print -r -- zsh-$((6*7))\r");
    check("a command runs", wait_for("zsh-42", 3000) && prompt("% ", 3000));
    mark(); type("echo ac\x1b[Db\r");
    check("line editing: the left arrow", wait_for("\nabc", 3000));
    mark(); type("echo first-z\r"); prompt("% ", 3000);
    mark(); type("\x1b[A\r");
    check("the up arrow: the last command again", wait_for("first-z\nfirst-z", 3000) || wait_for("echo first-z\nfirst-z", 100));
    mark(); type("ech\t z-tab\r");
    check("Tab completes a command name", wait_for("z-tab", 4000));
    long tc = now_ms();
    mark(); type("ls /us\t");
    int got = wait_for("/usr/", 20000);
    char lab[96];
    snprintf(lab, sizeof lab, "Tab completes a path (%ld ms)", now_ms() - tc);
    type("\r");
    check(lab, got && wait_for("share", 4000));
    tc = now_ms();
    mark(); type("ls --\t");
    got = wait_for("--all", 20000) || wait_for("possibilities", 100);
    snprintf(lab, sizeof lab, "Tab lists options (compinit's completion of ls; %ld ms)", now_ms() - tc);
    check(lab, got);
    type("\x03"); prompt("% ", 3000);
    mark(); type("sleep 20\r"); pump(500); type("\x03");
    check("^C stops a running program", prompt("% ", 3000));
    mark(); type("sleep 20\r"); pump(500); type("\x1a");
    check("^Z stops it", wait_for("suspended", 3000));
    mark(); type("fg\r"); pump(500); type("\x03");
    check("fg, then ^C", prompt("% ", 3000));
    mark(); type("cd /tmp; pwd; cd -\r");
    check("cd, pwd, cd -", wait_for("/tmp", 3000));
    mark(); type("exit\r");
    int st = finish();
    check("exit: zsh ends", st >= 0 && WIFEXITED(st));
}

/* shelltest -k shell keys [ms]: the keys typed at a shell (\\t, \\r, \\e,
 * \\xNN, ^X for control keys), then what it showed - for looking into a
 * failure by hand */
static void unescape(const char* in, char* out) {
    while (*in) {
        if (in[0] == '^' && in[1] >= '@' && in[1] <= '_') { *out++ = (char)(in[1] - '@'); in += 2; continue; }
        if (in[0] != '\\' || !in[1]) { *out++ = *in++; continue; }
        in++;
        char c = *in++;
        if (c == 't') *out++ = '\t'; else if (c == 'r') *out++ = '\r'; else if (c == 'n') *out++ = '\n';
        else if (c == 'e') *out++ = 27;
        else if (c == 'x') { int v = 0; for (int i = 0; i < 2 && *in; i++, in++) v = v * 16 + (*in <= '9' ? *in - '0' : (*in | 32) - 'a' + 10); *out++ = (char)v; }
        else *out++ = c;
    }
    *out = 0;
}

int shelltest_keys(const char* shell, const char* keys, int ms) {
    char path[64], k[1024];
    snprintf(path, sizeof path, "/bin/%s", shell);
    char* av[] = { (char*)shell, "-i", NULL };
    if (start(path, av)) { printf("no %s\n", path); return 1; }
    long t0 = now_ms();
    prompt(!strcmp(shell, "zsh") ? "% " : "$ ", 30000);
    printf("(prompt after %ld ms)\n", now_ms() - t0);
    unescape(keys, k);
    mark();
    type(k);
    pump(ms);
    printf("%s\n(end)\n", g_out);
    fflush(stdout);
    /* what the processes are doing (a stuck one: where) */
    if (access("/bin/ps", X_OK) == 0) {
        int diagnostic=system("ps -eo pid,ppid,pgid,tty,tpgid,stat,sched,wchan,uip,pending,args");
        if (diagnostic<0) printf("(process diagnostics unavailable)\n");
    }
    kill(g_pid, SIGKILL);
    finish();
    return 0;
}

int shelltest_bash_steps(int n) {
    g_steps = n;
    bash_checks();
    printf("shelltest -b %d: %d passed, %d failed\n", n, g_pass, g_fail);
    return g_fail;
}

int shelltest_interactive(int verbose) {
    g_verbose = verbose;
    signal(SIGPIPE, SIG_IGN);
    bash_checks();
    printf("bash -i, then ^D\n");
    eof_after("a command", "echo x\r", NULL);
    eof_after("^C on a program", "sleep 20\r\001\001\x03", NULL);
    eof_after("^Z, fg, ^C", "sleep 20\r\001\001\x1a\001\001fg\r\001\001\x03", NULL);
    eof_after("^Z, kill %1", "sleep 20\r\001\001\x1a\001\001kill %1\r", NULL);
    eof_after("less", "printf 'a\\nb\\n' | less\r\001\001q", NULL);
    eof_after("read -p", "read -p 'n? ' v\r\001\001yes\r", NULL);
    eof_after("stty size; tput cols", "stty size; tput cols\r", NULL);
    eof_after("Tab completion", "ls /us\t\r", NULL);
    eof_after("^R", "echo hh\r\001\001\x12hh\r", NULL);
    eof_after("history", "history | tail -1\r", NULL);
    zsh_checks();
    printf("shelltest -i: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

/* ── shelltest -e: the editors (nano, vim and vi come with the system;
 * neovim is optional: apt install neovim) - each one that is installed is started on a file, typed at,
 * saved and quit; then the file must hold what was typed. ─────────────── */
static char* slurp(const char* path) {
    static char b[4096];
    int fd = open(path, O_RDONLY), n = fd < 0 ? -1 : (int)read(fd, b, sizeof b - 1);
    if (fd >= 0) close(fd);
    b[n < 0 ? 0 : n] = 0;
    return b;
}
static void put(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd<0) { check("create editor fixture",0);return; }
    if (write_all(fd,text,strlen(text))!=0) check("write editor fixture",0);
    if (close(fd)!=0) check("close editor fixture",0);
}

/* the editor at path on file: keys typed after it shows ready; 1 if it ended */
static int edit(const char* path, const char* file, const char* ready, const char* keys) {
    const char* name = strrchr(path, '/') + 1;
    char* av[] = { (char*)name, (char*)file, NULL };
    if (start(path, av)) return 0;
    int up = wait_for(ready, 20000) && editor_ready(8000);
    check("starts and draws its screen", up);
    type(keys);
    int st = finish();
    return up && st == 0;
}

/* a command line typed in vim / nvim: what it shows must have want */
static void ex_shows(const char* what, const char* cmd, const char* want) {
    mark();
    type(cmd);
    check(what, wait_for(want, 8000) && editor_ready(8000));
}

int shelltest_editors(void) {
    signal(SIGPIPE, SIG_IGN);
    int any = 0;
    if (access("/bin/nano", X_OK) == 0) {
        any = 1;
        printf("nano\n");
        unlink("/tmp/ed-nano.txt");
        char* av[] = { "nano", "/tmp/ed-nano.txt", NULL };
        if (!start("/bin/nano", av)) {
            check("starts and draws its screen", wait_for("GNU nano", 20000) && editor_ready(8000));
            type("hello nano");
            mark();
            type("\x0f");
            check("^O asks for the file name", wait_for("Write to File", 5000));
            type("\r");
            check("it wrote the file", wait_for("Wrote", 5000));
            type("\x18");
            check("^X quits", finish() == 0);
        }
        check("the file has the text", !strcmp(slurp("/tmp/ed-nano.txt"), "hello nano\n"));
    }
    if (access("/bin/vim", X_OK) == 0) {
        any = 1;
        printf("vim\n");
        unlink("/tmp/ed-vim.txt");
        check("i, text, Esc, :wq", edit("/bin/vim", "/tmp/ed-vim.txt", "ed-vim.txt", "ihello vim\x1b:wq\r"));
        check("the file has the text", !strcmp(slurp("/tmp/ed-vim.txt"), "hello vim\n"));
        put("/tmp/ed-vim.c", "int main(void) { return 0; }\n");
        char* av[] = { "vim", "/tmp/ed-vim.c", NULL };
        if (!start("/bin/vim", av)) {
            check("syntax session ready", wait_for("ed-vim.c", 20000) && editor_ready(8000));
            ex_shows("syntax colouring for C", ":echo 'syn=' . get(b:, 'current_syntax', 'none')\r", "syn=c");
            ex_shows(":help opens", ":help\r", "VIM - main help file");
            type(":qa!\r");
            check(":qa! quits", finish() == 0);
        }
    }
    if (access("/bin/vi", X_OK) == 0) {
        any = 1;
        printf("vi\n");
        put("/tmp/ed-vi.txt", "one\ntwo\nthree\n");
        check("dd, :wq", edit("/bin/vi", "/tmp/ed-vi.txt", "ed-vi.txt", "dd:wq\r"));
        check("the file is a line shorter (cut, not left over)", !strcmp(slurp("/tmp/ed-vi.txt"), "two\nthree\n"));
        unlink("/tmp/ed-vi-utf8.txt");
        check("vi inserts UTF-8 and saves", edit("/bin/vi", "/tmp/ed-vi-utf8.txt",
              "~~~~", "iété banana\x1b:wq\r"));
        check("vi preserves UTF-8 bytes", !strcmp(slurp("/tmp/ed-vi-utf8.txt"), "été banana\n"));
        check("vi ships its independent Neatvi licence",
              strstr(slurp("/apps/vi/LICENSE"), "NEATVI Editor") != NULL);
        /* ex must start in line mode, not open a Vim screen. */
        put("/tmp/ed-ex.txt", "first\nsecond\n");
        char* av[] = { "ex", "/tmp/ed-ex.txt", NULL };
        if (!start("/bin/ex", av)) {
            pump(400);
            type("1d\nwq\n");
            check("ex line mode saves and exits", finish() == 0);
            check("ex shares the independent vi editor",
                  !strcmp(slurp("/tmp/ed-ex.txt"), "second\n"));
        } else check("ex starts", 0);
    }
    if (access("/bin/nvim", X_OK) == 0) {
        any = 1;
        printf("nvim\n");
        unlink("/tmp/ed-nvim.txt");
        check("i, text, Esc, :wq", edit("/bin/nvim", "/tmp/ed-nvim.txt", "ed-nvim.txt", "ihello nvim\x1b:wq\r"));
        check("the file has the text", !strcmp(slurp("/tmp/ed-nvim.txt"), "hello nvim\n"));
        put("/tmp/ed-nvim.lua", "local x = 1\nprint(x)\n");
        char* av[] = { "nvim", "/tmp/ed-nvim.lua", NULL };
        if (!start("/bin/nvim", av)) {
            check("Lua session ready", wait_for("ed-nvim.lua", 20000) && editor_ready(8000));
            ex_shows("Lua runs", ":lua print('lua=' .. (1 + 1))\r", "lua=2");
            ex_shows("a tree-sitter parser (Lua, built in)", ":lua print('ts=' .. tostring(pcall(vim.treesitter.get_parser, 0, 'lua')))\r", "ts=true");
            ex_shows(":help opens", ":help\r", "help.txt [Help]");
            ex_shows("the help's file type", ":lua print('ft=' .. vim.bo.filetype)\r", "ft=help");
            ex_shows(":terminal runs a program on a pty", ":terminal echo term-$((6*7))\r", "term-42");
            type(":qa!\r");
            check(":qa! quits", finish() == 0);
        }
    }
    if (!any) printf("no editor installed (nano, vim and vi come with the 64-bit system; apt install neovim)\n");
    printf("shelltest -e: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
