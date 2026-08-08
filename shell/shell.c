#include "shell.h"
#include "editor.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/fs.h"
#include "../kernel/sysinfo.h"
#include "../kernel/timer.h"
#include "../kernel/daemon.h"
#include "../kernel/usb.h"
#include "../kernel/task.h"
#include "../kernel/gui.h"
#include "../kernel/fb.h"
#include "../kernel/rtc.h"
#include "../kernel/types.h"

/* ── string helpers ─────────────────────────────────────────────── */
static int k_strlen(const char* s) { int n=0; while(s[n]) n++; return n; }
static int k_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int k_strncmp(const char* a, const char* b, int n) {
    for(int i=0;i<n;i++){
        if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i];
        if(!a[i]) return 0;
    } return 0;
}
static int k_endswith(const char* s, const char* suffix) {
    int ls = k_strlen(s), lt = k_strlen(suffix);
    if (lt > ls) return 0;
    return k_strcmp(s + (ls - lt), suffix) == 0;
}
static const char* k_skip_spaces(const char* s) {
    while (*s == ' ') s++;
    return s;
}

static void print_uptime(void);
static void dispatch(const char* line);
static void run_script_text(const char* content);

#define SH_LINE_MAX     256
#define SH_HISTORY_MAX  16

static char sh_history[SH_HISTORY_MAX][SH_LINE_MAX];
static int  sh_hist_count = 0;

static void k_strcpy_n(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int next_token(const char** p, char* out, int outlen) {
    const char* s = k_skip_spaces(*p);
    int i = 0;
    if (!*s) return 0;
    while (s[i] && s[i] != ' ') {
        if (i < outlen - 1) out[i] = s[i];
        i++;
    }
    out[(i < outlen - 1) ? i : (outlen - 1)] = '\0';
    *p = s + i;
    return 1;
}

static int has_flag(const char* args, const char* flag) {
    const char* p = args ? args : "";
    char tok[32];
    while (next_token(&p, tok, sizeof(tok))) {
        if (k_strcmp(tok, flag) == 0) return 1;
    }
    return 0;
}

/* ── ACPI power off ──────────────────────────────────────────────── */
static void acpi_poweroff(void) {
    /*
     * Try several well-known ACPI PM1a control port + SLP_TYP combos.
     * VirtualBox:  port 0x4004, value 0x3400
     * QEMU (-M pc): port 0xB004, value 0x2000  (PIIX4 ACPI)
     * QEMU (-M q35): port 0x0604, value 0x2000
     * Bochs:       port 0xB004, value 0x2000
     * Fallback: triple-fault (halts in most VMs)
     */
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x3400),"Nd"((uint16_t)0x4004));
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x2000),"Nd"((uint16_t)0xB004));
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x2000),"Nd"((uint16_t)0x0604));
    /* last resort: triple fault */
    __asm__ volatile("cli; lidt 0; int $0");
}

/* ── reboot via 8042 pulse ───────────────────────────────────────── */
static void do_reboot(void) {
    /* drain 8042 buffer */
    while (__extension__({
        uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"((uint16_t)0x64));
        v;
    }) & 0x02);
    /* pulse reset line */
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0xFE),"Nd"((uint16_t)0x64));
    __asm__ volatile("hlt");
}

/* ── deferred power actions (non-blocking) ──────────────────────── */
static int      shutdown_pending = 0;
static uint32_t shutdown_deadline_tick = 0;

static void poll_deferred_actions(void) {
    if (!shutdown_pending) return;
    if ((int32_t)(timer_ticks() - shutdown_deadline_tick) < 0) return;

    shutdown_pending = 0;
    terminal_write_color("\nShutting down now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    timer_sleep_ms(150);
    acpi_poweroff();
}

/* ── commands ────────────────────────────────────────────────────── */

static void cmd_reboot(const char* args) {
    (void)args;
    terminal_write_color("\nRebooting now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    timer_sleep_ms(150);
    do_reboot();
}

static void cmd_shutdown(const char* args) {
    if (k_strcmp(args, "now") == 0) {
        shutdown_pending = 0;
        terminal_write_color("\nShutting down now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        timer_sleep_ms(150);
        acpi_poweroff();
        return;
    }

    if (k_strcmp(args, "-c") == 0) {
        if (shutdown_pending) {
            shutdown_pending = 0;
            terminal_write_color("Shutdown cancelled.\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        } else {
            terminal_write_color("No shutdown pending.\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        }
        return;
    }

    shutdown_pending = 1;
    shutdown_deadline_tick = timer_ticks() + 60u * 100u;
    terminal_write_color("System will shutdown in 60 seconds.\n",
                         VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_write_color("Use 'shutdown -c' to cancel.\n",
                         VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

static void cmd_neofetch(void) {
    const sysinfo_t* si = sysinfo_get();
    char mbuf[16];

    terminal_write_color(
        "\n"
        "   ,--.\n"
        "  ( () )\n"
        "   `--'\n"
        "    ||\n"
        "    ||\n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);

    terminal_write_color("  Banana OS", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln(" 0.3");
    terminal_writeln("  --------------------");

    terminal_write_color("  OS:       ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("Banana OS 0.3");
    terminal_write_color("  KERNEL:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("Banana Kernel 0.3");
    terminal_write_color("  ARCH:     ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("x86 (i686)");
    terminal_write_color("  SHELL:    ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("sh (Banana sh)");
    terminal_write_color("  CPU:      ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Whatever your hypervisor gives you");
    terminal_write_color("  VENDOR:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_vendor[0] ? si->cpu_vendor : "Unknown");
    terminal_write_color("  MEMORY:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (si->mem_kb > 0) {
        char* ms = u32_to_str((si->mem_kb / 1024u) + 1u, mbuf, sizeof(mbuf));
        terminal_write(ms);
        terminal_writeln(" MB");
    } else {
        terminal_writeln("Unknown");
    }
    terminal_write_color("  DISPLAY:  ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("VGA text 80x25");
    terminal_write_color("  LICENSE:  ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("Banana Public License v1");
    terminal_write_color("\n  ** Powered by pure potassium **\n",
                         VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

static void cmd_uptime(void) {
    terminal_write_color("Uptime: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    print_uptime();
    terminal_putchar('\n');
}

static void cmd_proc_info(const char* args) {
    const sysinfo_t* si = sysinfo_get();
    int show_all = (!args || !*k_skip_spaces(args));

    if (show_all || has_flag(args, "-n")) {
        terminal_write_color("CPU name: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Unknown");
    }
    if (show_all || has_flag(args, "-v")) {
        terminal_write_color("Vendor: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(si->cpu_vendor[0] ? si->cpu_vendor : "Unknown");
    }
    if (show_all || has_flag(args, "-c")) {
        char b[16];
        terminal_write_color("Cores: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(si->cpu_cores, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-t")) {
        char b[16];
        terminal_write_color("Threads: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(si->cpu_threads, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-f")) {
        char b[16];
        terminal_write_color("Family: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(si->cpu_family, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-m")) {
        char b[16];
        terminal_write_color("Model: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(si->cpu_model, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-s")) {
        char b[16];
        terminal_write_color("Stepping: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(si->cpu_stepping, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-ht")) {
        terminal_write_color("Hyper-Threading: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(si->cpu_has_ht ? "yes" : "no");
    }
}

static void cmd_ram_info(const char* args) {
    const sysinfo_t* si = sysinfo_get();
    uint32_t total_kb = si->mem_kb;
    uint32_t used_bytes = fs_ram_used_bytes();
    uint32_t used_kb = (used_bytes + 1023u) / 1024u;
    uint32_t free_kb = (total_kb > used_kb) ? (total_kb - used_kb) : 0;
    uint32_t pct = (total_kb > 0) ? ((used_kb * 100u) / total_kb) : 0;
    int show_all = (!args || !*k_skip_spaces(args));
    char b[16];

    if (show_all || has_flag(args, "-t")) {
        terminal_write_color("Total RAM: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str((total_kb / 1024u) + 1u, b, sizeof(b)));
        terminal_writeln(" MB");
    }
    if (show_all || has_flag(args, "-u")) {
        terminal_write_color("Used (RAM FS): ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str((used_kb / 1024u) + 1u, b, sizeof(b)));
        terminal_writeln(" MB");
    }
    if (show_all || has_flag(args, "-f")) {
        terminal_write_color("Estimated free: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str((free_kb / 1024u) + 1u, b, sizeof(b)));
        terminal_writeln(" MB");
    }
    if (show_all || has_flag(args, "-p")) {
        terminal_write_color("Used percent: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str(pct, b, sizeof(b)));
        terminal_writeln("%");
    }
    if (show_all || has_flag(args, "-m")) {
        terminal_write_color("Raw totals: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write("total=");
        terminal_write(u32_to_str(total_kb, b, sizeof(b)));
        terminal_write("KiB used=");
        terminal_write(u32_to_str(used_kb, b, sizeof(b)));
        terminal_write("KiB free=");
        terminal_write(u32_to_str(free_kb, b, sizeof(b)));
        terminal_writeln("KiB");
    }
}

static void cmd_gpu_info(const char* args) {
    const fb_info_t* fi = fb_info();
    int show_all = (!args || !*k_skip_spaces(args));
    char b[16];

    if (!fb_available() || !fi || fi->width == 0 || fi->height == 0) {
        terminal_writeln("No framebuffer GPU info available.");
        return;
    }

    if (show_all || has_flag(args, "-n")) {
        terminal_write_color("GPU: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln("Generic VBE/Multiboot framebuffer");
    }
    if (show_all || has_flag(args, "-r")) {
        terminal_write_color("Resolution: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str(fi->width, b, sizeof(b)));
        terminal_write("x");
        terminal_writeln(u32_to_str(fi->height, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-b")) {
        terminal_write_color("BPP: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str(fi->bpp, b, sizeof(b)));
    }
    if (show_all || has_flag(args, "-p")) {
        terminal_write_color("Pitch: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str(fi->pitch, b, sizeof(b)));
        terminal_writeln(" bytes/row");
    }
    if (show_all || has_flag(args, "-m")) {
        terminal_write_color("Framebuffer addr: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(u32_to_str((uint32_t)fi->addr, b, sizeof(b)));
    }
}

static void cmd_hw_info(const char* args) {
    int show_all = (!args || !*k_skip_spaces(args));
    rtc_datetime_t dt;

    if (show_all || has_flag(args, "-c")) cmd_proc_info("-n -v -c -t");
    if (show_all || has_flag(args, "-m")) cmd_ram_info("-t -u -f -p");
    if (show_all || has_flag(args, "-g")) cmd_gpu_info("-n -r -b");
    if (show_all || has_flag(args, "-u")) {
        terminal_write_color("USB: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(usb_status());
    }
    if (show_all || has_flag(args, "-k")) {
        terminal_write_color("Keyboard layout: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(keyboard_layout_name());
    }
    if (show_all || has_flag(args, "-r")) {
        char b[16];
        if (rtc_read_datetime(&dt) == 0) {
            terminal_write_color("RTC: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            if (dt.hour < 10) terminal_putchar('0');
            terminal_write(u32_to_str(dt.hour, b, sizeof(b)));
            terminal_putchar(':');
            if (dt.minute < 10) terminal_putchar('0');
            terminal_write(u32_to_str(dt.minute, b, sizeof(b)));
            terminal_putchar(':');
            if (dt.second < 10) terminal_putchar('0');
            terminal_writeln(u32_to_str(dt.second, b, sizeof(b)));
        } else {
            terminal_writeln("RTC: unavailable");
        }
    }
}

static void fmt_u32(char* dst, int dstlen, uint32_t v) {
    char tmp[16];
    char* s = u32_to_str(v, tmp, sizeof(tmp));
    int i = 0;
    while (s[i] && i < dstlen - 1) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void print_uptime(void) {
    uint32_t sec = timer_ticks() / 100;
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    char b1[16], b2[16], b3[16];
    fmt_u32(b1, sizeof(b1), h);
    fmt_u32(b2, sizeof(b2), m);
    fmt_u32(b3, sizeof(b3), s);
    terminal_write(b1); terminal_write("h ");
    terminal_write(b2); terminal_write("m ");
    terminal_write(b3); terminal_write("s");
}

static void draw_bar(uint32_t val, uint32_t max) {
    int bars = (max == 0) ? 0 : (val * 10) / max;
    if (bars > 10) bars = 10;

    for (int i = 0; i < 10; i++) {
        terminal_putchar((i < bars) ? '#' : ' ');
    }
}

static void cmd_top(void) {
    /* Not `static`: each GUI terminal window running "top" is a separate
     * cooperative task with its own call stack, so this needs to be its
     * own per-invocation smoothing buffer rather than shared state that
     * different windows' refreshes would stomp on. */
    uint32_t last_cpu[TASK_MAX] = {0};
    int my_vt = terminal_vt_get_active();

    while (1) {
        const sysinfo_t* si = sysinfo_get();
        task_info_t procs[TASK_MAX];
        int pcount;

        terminal_clear();

        /* ── HEADER ───────────────────────────── */
        terminal_write_color("Banana OS 0.3 htop - press q to quit\n",
                             VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        terminal_writeln("--------------------------------------------");

        char b1[16], b2[16], b3[16];

        /* CPU NAME */
        terminal_write_color("CPU: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Unknown CPU");

        /* OS VERSION */
        terminal_write_color("OS:  ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln("Banana OS 0.3 (Banana Kernel 0.3)");

        /* RAM USAGE */
        uint32_t total_mb = (si->mem_kb / 1024u) + 1u;
        uint32_t used_mb  = (fs_ram_used_bytes() + 1024u*1024u - 1u) / (1024u*1024u);
        uint32_t free_mb  = (total_mb > used_mb) ? (total_mb - used_mb) : 0;

        terminal_write_color("RAM: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        fmt_u32(b1, sizeof(b1), used_mb);
        fmt_u32(b2, sizeof(b2), total_mb);
        fmt_u32(b3, sizeof(b3), free_mb);

        terminal_write(b1);
        terminal_write(" MB / ");
        terminal_write(b2);
        terminal_write(" MB (free ");
        terminal_write(b3);
        terminal_writeln(" MB)");

        terminal_write("      [");
        draw_bar(used_mb, total_mb);
        terminal_writeln("]");

        terminal_writeln("--------------------------------------------");

        /* ── PROCESS LIST ─────────────────────── */

        daemon_poll(0);
        pcount = task_count();
        if (pcount > TASK_MAX) pcount = TASK_MAX;

        task_snapshot(procs, pcount);

        /* sort by CPU */
        for (int i = 0; i < pcount - 1; i++) {
            for (int j = i + 1; j < pcount; j++) {
                if (procs[j].cpu_pct > procs[i].cpu_pct) {
                    task_info_t tmp = procs[i];
                    procs[i] = procs[j];
                    procs[j] = tmp;
                }
            }
        }

        terminal_writeln(" PID   CPU%   BAR        STATE     NAME");
        terminal_writeln("----- ------ ---------- -------- ----------------");

        for (int i = 0; i < pcount; i++) {
            char pidb[16], cpub[16];

            uint32_t cpu = procs[i].cpu_pct;
            cpu = (cpu + last_cpu[i]) / 2;
            last_cpu[i] = cpu;

            terminal_write(" ");
            terminal_write(u32_to_str(procs[i].pid, pidb, sizeof(pidb)));

            if (procs[i].pid < 10) terminal_write("   ");
            else if (procs[i].pid < 100) terminal_write("  ");
            else terminal_write(" ");

            terminal_write(" ");
            terminal_write(u32_to_str(cpu, cpub, sizeof(cpub)));
            terminal_write("%   ");

            draw_bar(cpu, 100);

            terminal_write("   ");
            const char* state = task_state_str(procs[i].state);
            terminal_write(state);

            int sl = k_strlen(state);
            for (int s = sl; s < 8; s++) terminal_putchar(' ');

            terminal_write(" ");
            terminal_writeln(procs[i].name);
        }

        /* ── INPUT ───────────────────────────── */

        for (int i = 0; i < 20; i++) {
            gui_poll();       /* also yields once, giving sysmon a slice */
            terminal_vt_set_active(my_vt);

            char c = (gui_focused_vt() == my_vt) ? keyboard_try_getchar() : 0;

            if (c == 'q' || c == 'Q' || c == 3) {
                terminal_clear();
                return;
            }

            timer_sleep_ms(50);
        }
    }
}

static void cmd_help(void) {
    static const char* lines[] = {
        "Banana OS 0.3 - available commands:",
        "",
        "  help               show this message",
        "  neofetch           system information",
        "  echo <text>        print text",
        "  clear              clear screen",
        "  uname              print system name",
        "  whoami             print current user",
        "  hostname           print system hostname",
        "  date               print current date/time",
        "  ls [-l] [path]     list directory (absolute or relative path)",
        "  cd [path]          change directory (no arg / ~ -> home, .. up)",
        "  pwd                print working directory (full path)",
        "  mkdir [-p] <dir>   create directory (-p makes parents too)",
        "  rm [-r] <name>     remove file, or directory with -r",
        "  touch <file>       create an empty file (or update if it exists)",
        "  cp <src> <dst>     copy a file",
        "  mv <src> <dst>     move/rename a file or directory",
        "  edit <file>        open nano-style text editor",
        "  cat <file>         print file contents",
        "  grep [-n] <p> <f>  print lines in a file matching a substring",
        "  wc <file>          count lines/words/bytes in a file",
        "  head [-n N] <file> print first N lines (default 10)",
        "  tail [-n N] <file> print last N lines (default 10)",
        "  find [path]        recursively list files/dirs under path",
        "  history            show command history",
        "  which <cmd>        show whether a command is a shell builtin",
        "  run <file.sh>      run script file line by line",
        "  uptime             print current uptime",
        "  top                live system monitor (press q to quit)",
        "  exit               close this GUI terminal window",
        "  start              alias of startx",
        "  stop               alias of stopx",
        "  startx             start GUI desktop",
        "  stopx              quit GUI desktop",
        "  keyboardctl [lay]  show/set keyboard layout",
        "  loadctl [lay]      alias of keyboardctl",
        "  usbctl             show USB legacy handoff status",
        "  proc_info [flags]  cpu info (-c -t -n -v -f -m -s -ht)",
        "  ram_info [flags]   ram info (-t -u -f -p -m)",
        "  gpu_info [flags]   gpu/fb info (-n -r -b -p -m)",
        "  hw_info [flags]    hardware summary (-c -m -g -u -k -r)",
        "  shutdown [now|-c]  schedule shutdown (60s), now, or cancel",
        "  reboot             immediate reboot",
        "  halt               hard halt (no ACPI)",
        "",
        "  Editor: arrows move, ^O/^S save, ^X exit, ^K cut line, ^U paste",
        "",
    };
    const int line_count = (int)(sizeof(lines) / sizeof(lines[0]));
    const int view_rows = 21; /* keep last rows for status/help */
    int top = 0;
    int max_top = (line_count > view_rows) ? (line_count - view_rows) : 0;
    int my_vt = terminal_vt_get_active();

    while (1) {
        terminal_clear();
        for (int r = 0; r < view_rows; r++) {
            int idx = top + r;
            if (idx >= line_count) {
                terminal_putchar('\n');
                continue;
            }
            if (idx == 0) {
                terminal_write_color(lines[idx], VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                terminal_putchar('\n');
            } else if (idx >= 2 && idx <= 44) {
                terminal_write_color(lines[idx], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                terminal_putchar('\n');
            } else if (idx == 46) {
                terminal_write_color(lines[idx], VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                terminal_putchar('\n');
            } else {
                terminal_writeln(lines[idx]);
            }
        }

        terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        terminal_write(" help: up/down or j/k to scroll, q to quit ");
        for (int i = 42; i < 80; i++) terminal_putchar(' ');
        terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        terminal_putchar('\n');

        terminal_setcolor(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
        terminal_write(" lines ");
        {
            char b1[16], b2[16];
            terminal_write(u32_to_str((uint32_t)(top + 1), b1, sizeof(b1)));
            terminal_write("-");
            int end_line = top + view_rows;
            if (end_line > line_count) end_line = line_count;
            terminal_write(u32_to_str((uint32_t)end_line, b2, sizeof(b2)));
            terminal_write("/");
            terminal_write(u32_to_str((uint32_t)line_count, b2, sizeof(b2)));
        }
        for (int i = 0; i < 62; i++) terminal_putchar(' ');
        terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

        char c = 0;
        while (!c) {
            gui_poll();
            terminal_vt_set_active(my_vt);
            if (gui_focused_vt() != my_vt) { timer_sleep_ms(10); continue; }
            c = keyboard_try_getchar();
            if (!c) timer_sleep_ms(10);
        }
        if (c == 'q' || c == 'Q' || c == 3) {
            terminal_putchar('\n'); /* keep visible help text on screen */
            return;
        }
        if (c == 'j' || c == 'J') {
            if (top < max_top) top++;
            continue;
        }
        if (c == 'k' || c == 'K') {
            if (top > 0) top--;
            continue;
        }
        if (c == 27) {
            char c2 = keyboard_getchar();
            char c3 = keyboard_getchar();
            if (c2 != '[') continue;
            if (c3 == 'A' && top > 0) top--;
            if (c3 == 'B' && top < max_top) top++;
            continue;
        }
    }
}

static void cmd_keyboardctl(const char* args) {
    const char* layout = k_skip_spaces(args);
    if (!layout || !*layout) {
        terminal_write_color("Current layout: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(keyboard_layout_name());
        terminal_write_color("Available: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(keyboard_layouts_help());
        terminal_writeln("Usage: keyboardctl <layout>  (alias: loadctl)");
        return;
    }

    if (keyboard_set_layout(layout) == 0) {
        terminal_write_color("Keyboard layout set to: ", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        terminal_writeln(keyboard_layout_name());
        return;
    }

    terminal_write_color("keyboardctl: unknown layout: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(layout);
    terminal_write_color("Available: ", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln(keyboard_layouts_help());
}

static void cmd_cat(const char* name) {
    if (!name || !name[0]) {
        terminal_write_color("cat: missing filename\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(name);
    if (idx < 0) {
        terminal_write_color("cat: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(name);
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    if (!f->content[0]) terminal_writeln("(empty file)");
    else                 terminal_writeln(f->content);
}

static void cmd_run(const char* name) {
    if (!name || !name[0]) {
        terminal_write_color("run: missing filename\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (!k_endswith(name, ".sh")) {
        terminal_write_color("run: only .sh scripts are supported\n",
                             VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(name);
    if (idx < 0) {
        terminal_write_color("run: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(name);
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    run_script_text(f->content);
}

/* ── text-processing / discovery utilities ─────────────────────── */
static int k_atoi(const char* s) {
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static int k_strstr(const char* hay, const char* needle) {
    if (!*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* counts lines in fs_file content, treating a trailing unterminated
 * line the same way `cat`/the editor already do: as one more line */
static uint32_t count_lines(const char* c) {
    if (!c[0]) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; c[i]; i++) if (c[i] == '\n') n++;
    if (c[k_strlen(c) - 1] != '\n') n++;
    return n;
}

/* prints lines [from, to) (0-indexed, half-open) of content */
static void print_line_range(const char* c, uint32_t from, uint32_t to) {
    uint32_t line = 0;
    uint32_t i = 0;
    int printed_open_line = 0;
    while (c[i] && line < to) {
        if (line >= from) {
            terminal_putchar(c[i]);
            printed_open_line = (c[i] != '\n');
        }
        if (c[i] == '\n') { line++; printed_open_line = 0; }
        i++;
    }
    if (printed_open_line) terminal_putchar('\n');
}

static void cmd_wc(const char* args) {
    char fname[FS_PATH_LEN];
    const char* p = args ? args : "";
    if (!next_token(&p, fname, sizeof(fname))) {
        terminal_write_color("Usage: wc <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(fname);
    if (idx < 0) {
        terminal_write_color("wc: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(fname);
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    const char* c = f->content;
    uint32_t bytes = 0, words = 0, lines = 0;
    int in_word = 0;
    for (; c[bytes]; bytes++) {
        char ch = c[bytes];
        if (ch == '\n') lines++;
        if (ch == ' ' || ch == '\n' || ch == '\t') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    char b[16];
    terminal_write("  "); terminal_write(u32_to_str(lines, b, sizeof(b)));
    terminal_write("  "); terminal_write(u32_to_str(words, b, sizeof(b)));
    terminal_write("  "); terminal_write(u32_to_str(bytes, b, sizeof(b)));
    terminal_write("  "); terminal_writeln(fname);
}

static void cmd_head(const char* args) {
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    uint32_t n = 10;
    char fname[FS_PATH_LEN]; fname[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (k_strcmp(tok, "-n") == 0) {
            char numtok[16];
            if (next_token(&p, numtok, sizeof(numtok))) n = (uint32_t)k_atoi(numtok);
        } else if (tok[0] == '-' && tok[1] >= '0' && tok[1] <= '9') {
            n = (uint32_t)k_atoi(tok + 1);
        } else {
            k_strcpy_n(fname, tok, sizeof(fname));
        }
    }
    if (!fname[0]) {
        terminal_write_color("Usage: head [-n N] <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(fname);
    if (idx < 0) {
        terminal_write_color("head: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(fname);
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    uint32_t total = count_lines(f->content);
    print_line_range(f->content, 0, (n < total) ? n : total);
}

static void cmd_tail(const char* args) {
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    uint32_t n = 10;
    char fname[FS_PATH_LEN]; fname[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (k_strcmp(tok, "-n") == 0) {
            char numtok[16];
            if (next_token(&p, numtok, sizeof(numtok))) n = (uint32_t)k_atoi(numtok);
        } else if (tok[0] == '-' && tok[1] >= '0' && tok[1] <= '9') {
            n = (uint32_t)k_atoi(tok + 1);
        } else {
            k_strcpy_n(fname, tok, sizeof(fname));
        }
    }
    if (!fname[0]) {
        terminal_write_color("Usage: tail [-n N] <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(fname);
    if (idx < 0) {
        terminal_write_color("tail: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(fname);
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    uint32_t total = count_lines(f->content);
    uint32_t from = (total > n) ? total - n : 0;
    print_line_range(f->content, from, total);
}

static void cmd_grep(const char* args) {
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    int show_num = 0;
    char pattern[FS_PATH_LEN]; pattern[0] = '\0';
    char fname[FS_PATH_LEN];   fname[0]   = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (k_strcmp(tok, "-n") == 0) { show_num = 1; continue; }
        if (!pattern[0])      k_strcpy_n(pattern, tok, sizeof(pattern));
        else if (!fname[0])   k_strcpy_n(fname, tok, sizeof(fname));
    }
    if (!pattern[0] || !fname[0]) {
        terminal_write_color("Usage: grep [-n] <pattern> <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int idx = fs_find_file(fname);
    if (idx < 0) {
        terminal_write_color("grep: no such file: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(fname);
        return;
    }
    fs_file_t* f = fs_get_file(idx);

    char line[256];
    int lp = 0;
    uint32_t lineno = 1;
    for (int i = 0; ; i++) {
        char ch = f->content[i];
        if (ch == '\n' || ch == '\0') {
            line[lp] = '\0';
            if (k_strstr(line, pattern)) {
                if (show_num) {
                    char b[16];
                    terminal_write(u32_to_str(lineno, b, sizeof(b)));
                    terminal_write(": ");
                }
                terminal_writeln(line);
            }
            lp = 0;
            lineno++;
            if (ch == '\0') break;
            continue;
        }
        if (lp < (int)sizeof(line) - 1) line[lp++] = ch;
    }
}

static void cmd_find(const char* args) {
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    char path[FS_PATH_LEN];        path[0] = '\0';
    char name_filter[FS_NAME_LEN]; name_filter[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (k_strcmp(tok, "-name") == 0) {
            char ftok[FS_NAME_LEN];
            if (next_token(&p, ftok, sizeof(ftok))) k_strcpy_n(name_filter, ftok, sizeof(name_filter));
        } else if (!path[0]) {
            k_strcpy_n(path, tok, sizeof(path));
        }
    }
    fs_find(path, name_filter);
}

static void cmd_history(void) {
    for (int i = 0; i < sh_hist_count; i++) {
        char b[8];
        terminal_write("  ");
        terminal_write(u32_to_str((uint32_t)(i + 1), b, sizeof(b)));
        terminal_write("  ");
        terminal_writeln(sh_history[i]);
    }
}

static const char* const known_cmds[] = {
    "help", "neofetch", "echo", "clear", "uname", "whoami", "hostname", "date",
    "ls", "cd", "pwd", "mkdir", "rm", "touch", "cp", "mv", "edit", "cat", "run",
    "uptime", "top", "exit", "start", "stop", "startx", "stopx",
    "keyboardctl", "loadctl", "usbctl", "proc_info", "ram_info", "gpu_info",
    "hw_info", "shutdown", "reboot", "halt", "history", "which",
    "grep", "wc", "head", "tail", "find", (void*)0
};

static void cmd_which(const char* args) {
    const char* p = args ? args : "";
    char tok[64];
    if (!next_token(&p, tok, sizeof(tok))) {
        terminal_write_color("Usage: which <command>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    for (int i = 0; known_cmds[i]; i++) {
        if (k_strcmp(tok, known_cmds[i]) == 0) {
            terminal_write(tok);
            terminal_writeln(": shell builtin");
            return;
        }
    }
    terminal_write(tok);
    terminal_writeln(": not found");
}

static void cmd_uname(void) {
    terminal_writeln("Banana OS 0.3 x86 Banana Kernel 0.3 sh");
}

static void cmd_whoami(void) { terminal_writeln("banana"); }
static void cmd_hostname(void) { terminal_writeln("banana-os-0.3"); }

static void cmd_date(void) {
    rtc_datetime_t dt;
    if (rtc_read_datetime(&dt) != 0) {
        terminal_writeln("date: RTC unavailable");
        return;
    }
    char b[16];
    terminal_write(u32_to_str(dt.year, b, sizeof(b)));
    terminal_putchar('-');
    if (dt.month < 10) terminal_putchar('0');
    terminal_write(u32_to_str(dt.month, b, sizeof(b)));
    terminal_putchar('-');
    if (dt.day < 10) terminal_putchar('0');
    terminal_write(u32_to_str(dt.day, b, sizeof(b)));
    terminal_putchar(' ');
    if (dt.hour < 10) terminal_putchar('0');
    terminal_write(u32_to_str(dt.hour, b, sizeof(b)));
    terminal_putchar(':');
    if (dt.minute < 10) terminal_putchar('0');
    terminal_write(u32_to_str(dt.minute, b, sizeof(b)));
    terminal_putchar(':');
    if (dt.second < 10) terminal_putchar('0');
    terminal_writeln(u32_to_str(dt.second, b, sizeof(b)));
}

/* ── path-taking commands (parse an optional flag + a path token) ──── */
static void cmd_ls(const char* args) {
    const char* p = args ? args : "";
    int longfmt = has_flag(p, "-l");
    char tok[FS_PATH_LEN];
    char path[FS_PATH_LEN];
    path[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (tok[0] != '-') { k_strcpy_n(path, tok, sizeof(path)); break; }
    }
    if (longfmt) fs_ls_long(path);
    else         fs_ls(path);
}

static void cmd_mkdir(const char* args) {
    int recursive = has_flag(args, "-p");
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    char path[FS_PATH_LEN];
    path[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (tok[0] != '-') { k_strcpy_n(path, tok, sizeof(path)); break; }
    }
    if (!path[0]) {
        terminal_write_color("Usage: mkdir [-p] <dir>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int r = recursive ? fs_mkdir_p(path) : fs_mkdir(path);
    if (r < 0) {
        terminal_write_color("mkdir: cannot create directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
    }
}

static void cmd_rm(const char* args) {
    int recursive = has_flag(args, "-r") || has_flag(args, "-rf") || has_flag(args, "-fr");
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    char path[FS_PATH_LEN];
    path[0] = '\0';
    while (next_token(&p, tok, sizeof(tok))) {
        if (tok[0] != '-') { k_strcpy_n(path, tok, sizeof(path)); break; }
    }
    if (!path[0]) {
        terminal_write_color("Usage: rm [-r] <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    fs_delete(path, recursive);
}

static void cmd_touch(const char* args) {
    const char* p = args ? args : "";
    char tok[FS_PATH_LEN];
    int any = 0;
    while (next_token(&p, tok, sizeof(tok))) {
        any = 1;
        if (fs_create(tok) < 0) {
            terminal_write_color("touch: cannot create: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(tok);
        }
    }
    if (!any) terminal_write_color("Usage: touch <file> [file...]\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

static void cmd_cp(const char* args) {
    const char* p = args ? args : "";
    char src[FS_PATH_LEN], dst[FS_PATH_LEN];
    if (!next_token(&p, src, sizeof(src)) || !next_token(&p, dst, sizeof(dst))) {
        terminal_write_color("Usage: cp <src> <dst>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (fs_copy(src, dst) < 0) {
        terminal_write_color("cp: cannot copy: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(src);
    }
}

static void cmd_mv(const char* args) {
    const char* p = args ? args : "";
    char src[FS_PATH_LEN], dst[FS_PATH_LEN];
    if (!next_token(&p, src, sizeof(src)) || !next_token(&p, dst, sizeof(dst))) {
        terminal_write_color("Usage: mv <src> <dst>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (fs_move(src, dst) < 0) {
        terminal_write_color("mv: cannot move: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(src);
    }
}

static void cmd_exit(void) {
    int my_vt = terminal_vt_get_active();
    if (my_vt != 0 && gui_close_terminal_by_vt(my_vt)) return;
    terminal_write_color(
        "exit: nothing to close here (this isn't a GUI terminal window)\n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

static void cmd_halt(void) {
    terminal_write_color("\nSystem halted.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    __asm__ volatile("cli; hlt");
}

/* ── prompt ─────────────────────────────────────────────────────── */
static void print_prompt(void) {
    char cwd_buf[FS_PATH_LEN];
    fs_cwd_path(cwd_buf, sizeof(cwd_buf));
    terminal_write_color("banana",      VGA_COLOR_YELLOW,      VGA_COLOR_BLACK);
    terminal_write_color("@banana-os-0.3",  VGA_COLOR_LIGHT_GREEN,  VGA_COLOR_BLACK);
    terminal_write_color(":",           VGA_COLOR_WHITE,        VGA_COLOR_BLACK);
    terminal_write_color(cwd_buf,       VGA_COLOR_LIGHT_BLUE,   VGA_COLOR_BLACK);
    terminal_write_color("$ ",          VGA_COLOR_WHITE,        VGA_COLOR_BLACK);
}

static void shell_readline(char* buf, int maxlen) {
    if (maxlen <= 0) return;
    buf[0] = '\0'; /* always start from clean command buffer */

    /* Captured once, before this task can yield: identifies which vt (and
     * so which GUI terminal window, if any) this shell instance owns.
     * Re-asserted after every yield below, since another cooperative task
     * may have run in between and pointed the "active vt" write target at
     * its own window - see shell_tick(). */
    int my_vt = terminal_vt_get_active();

    int len = 0;
    int cur = 0;
    int prev_len = 0;
    int hist_nav = sh_hist_count; /* one-past-last means "live line" */
    char live_line[SH_LINE_MAX];
    live_line[0] = '\0';

    while (1) {
        poll_deferred_actions();
        daemon_poll(len == 0);
        gui_poll();
        terminal_vt_set_active(my_vt);

        if (gui_focused_vt() != my_vt) {
            /* Another window (or none) has keyboard focus: keep this
             * shell's own command running/idling but don't steal input
             * meant for whoever the user is actually looking at. */
            timer_sleep_ms(10);
            continue;
        }

        char c = keyboard_try_getchar();
        if (!c) { timer_sleep_ms(10); continue; }

        if (c == 27) {
            /* Escape, or the lead byte of an arrow-key sequence
             * (ESC [ A/B/C/D). This has to be resolved before
             * gui_handle_key() below: it treats every bare ESC as
             * "close the open Start menu", which would otherwise
             * swallow the lead byte of every arrow press before the
             * arrow itself ever reaches gui_handle_arrow() - making
             * arrow-key menu navigation unreachable whenever the menu
             * is open. */
            char c2 = keyboard_getchar(); /* '[' */
            if (c2 != '[') {
                gui_handle_key(c);
                continue;
            }
            char c3 = keyboard_getchar(); /* A/B/C/D */

            if (gui_handle_arrow(c3)) continue;

            if (c3 == 'D') {
                if (cur > 0) { cur--; terminal_cursor_left(); }
                continue;
            }
            if (c3 == 'C') {
                if (cur < len) { cur++; terminal_cursor_right(); }
                continue;
            }
            if (c3 == 'A') { /* history up */
                if (sh_hist_count == 0) continue;
                if (hist_nav == sh_hist_count) {
                    k_strcpy_n(live_line, buf, sizeof(live_line));
                }
                if (hist_nav > 0) hist_nav--;
                k_strcpy_n(buf, sh_history[hist_nav], maxlen);
                len = k_strlen(buf);
                cur = len;
            } else if (c3 == 'B') { /* history down */
                if (sh_hist_count == 0) continue;
                if (hist_nav < sh_hist_count - 1) {
                    hist_nav++;
                    k_strcpy_n(buf, sh_history[hist_nav], maxlen);
                } else {
                    hist_nav = sh_hist_count;
                    k_strcpy_n(buf, live_line, maxlen);
                }
                len = k_strlen(buf);
                cur = len;
            } else {
                continue;
            }
            /* fall through: A/B updated buf, redraw the line below */
        } else if (gui_handle_key(c)) {
            /* GUI consumed this key (e.g. Ctrl+T Start menu) */
            continue;
        } else if (c == 3) { /* Ctrl+C: cancel line, do not execute history entry */
            buf[0] = '\0';
            terminal_write("^C\n");
            return;
        } else if (c == '\n') {
            buf[len] = '\0'; /* ensure empty Enter stays empty */
            terminal_putchar('\n');
            break;
        } else if (c == '\b') {
            if (cur > 0) {
                for (int i = cur - 1; i < len; i++) buf[i] = buf[i + 1];
                cur--;
                len--;
            }
        } else if ((unsigned char)c >= 32 && len < maxlen - 1) {
            for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
            buf[cur] = c;
            len++;
            cur++;
        } else {
            continue;
        }

        buf[len] = '\0';

        /* Redraw editable command line after prompt */
        terminal_putchar('\r');
        print_prompt();
        terminal_write(buf);

        if (prev_len > len) {
            for (int i = 0; i < prev_len - len; i++) terminal_putchar(' ');
        }

        int draw_len = (prev_len > len) ? prev_len : len;
        for (int i = 0; i < draw_len - cur; i++) terminal_cursor_left();
        prev_len = len;
    }

    if (len > 0) {
        if (sh_hist_count < SH_HISTORY_MAX) {
            k_strcpy_n(sh_history[sh_hist_count++], buf, SH_LINE_MAX);
        } else {
            for (int i = 1; i < SH_HISTORY_MAX; i++)
                k_strcpy_n(sh_history[i - 1], sh_history[i], SH_LINE_MAX);
            k_strcpy_n(sh_history[SH_HISTORY_MAX - 1], buf, SH_LINE_MAX);
        }
    }
}

static void cmd_startx(void) {
    gui_set_enabled(1);
    terminal_writeln("startx: GUI enabled (Ctrl+T or click [Start]). Type 'stopx' to return to shell-only view.");
}

static void cmd_stopx(void) {
    gui_set_enabled(0);
    terminal_writeln("stopx: GUI disabled.");
}

/* ── dispatch ───────────────────────────────────────────────────── */
static void dispatch(const char* line) {
    line = k_skip_spaces(line);
    if (!*line) return;

    /* exact matches */
    if (k_strcmp(line, "help")     == 0) { cmd_help();       return; }
    if (k_strcmp(line, "clear")    == 0) { terminal_clear(); return; }
    if (k_strcmp(line, "neofetch") == 0) { cmd_neofetch();   return; }
    if (k_strcmp(line, "uname")    == 0) { cmd_uname();      return; }
    if (k_strcmp(line, "halt")     == 0) { cmd_halt();       return; }
    if (k_strcmp(line, "ls")       == 0) { cmd_ls("");       return; }
    if (k_strcmp(line, "pwd")      == 0) { fs_pwd();         return; }
    if (k_strcmp(line, "whoami")   == 0) { cmd_whoami();     return; }
    if (k_strcmp(line, "hostname") == 0) { cmd_hostname();   return; }
    if (k_strcmp(line, "date")     == 0) { cmd_date();       return; }
    if (k_strcmp(line, "history")  == 0) { cmd_history();    return; }
    if (k_strcmp(line, "find")     == 0) { cmd_find("");     return; }
    if (k_strcmp(line, "uptime")   == 0) { cmd_uptime();     return; }
    if (k_strcmp(line, "top")      == 0) { cmd_top();        return; }
    if (k_strcmp(line, "exit")     == 0) { cmd_exit();       return; }
    if (k_strcmp(line, "start")    == 0) { cmd_startx();     return; }
    if (k_strcmp(line, "stop")     == 0) { cmd_stopx();      return; }
    if (k_strcmp(line, "startx")   == 0) { cmd_startx();     return; }
    if (k_strcmp(line, "stopx")    == 0) { cmd_stopx();      return; }
    if (k_strcmp(line, "keyboardctl") == 0) { cmd_keyboardctl(""); return; }
    if (k_strcmp(line, "loadctl")  == 0) { cmd_keyboardctl(""); return; }
    if (k_strcmp(line, "usbctl")   == 0) { terminal_writeln(usb_status()); return; }
    if (k_strcmp(line, "proc_info") == 0) { cmd_proc_info(""); return; }
    if (k_strcmp(line, "ram_info")  == 0) { cmd_ram_info(""); return; }
    if (k_strcmp(line, "gpu_info")  == 0) { cmd_gpu_info(""); return; }
    if (k_strcmp(line, "hw_info")   == 0) { cmd_hw_info(""); return; }
    if (k_strcmp(line, "cd")       == 0) { fs_cd("");        return; }
    if (k_strcmp(line, "echo")     == 0) { terminal_putchar('\n'); return; }
    if (k_strcmp(line, "run")      == 0) {
        terminal_write_color("Usage: run <file.sh>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "edit")     == 0) {
        terminal_write_color("Usage: edit <filename>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "mkdir")    == 0) {
        terminal_write_color("Usage: mkdir [-p] <dir>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "rm")       == 0) {
        terminal_write_color("Usage: rm [-r] <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "touch")    == 0) {
        terminal_write_color("Usage: touch <file> [file...]\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "cp")       == 0) {
        terminal_write_color("Usage: cp <src> <dst>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "mv")       == 0) {
        terminal_write_color("Usage: mv <src> <dst>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "grep")     == 0) {
        terminal_write_color("Usage: grep [-n] <pattern> <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "wc")       == 0) {
        terminal_write_color("Usage: wc <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "head")     == 0) {
        terminal_write_color("Usage: head [-n N] <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "tail")     == 0) {
        terminal_write_color("Usage: tail [-n N] <file>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "which")    == 0) {
        terminal_write_color("Usage: which <command>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }

    /* shutdown / reboot with optional arg */
    if (k_strcmp(line, "shutdown")       == 0) { cmd_shutdown("");    return; }
    if (k_strncmp(line, "shutdown ", 9)  == 0) { cmd_shutdown(k_skip_spaces(line+9)); return; }
    if (k_strcmp(line, "reboot")         == 0) { cmd_reboot("");      return; }
    if (k_strncmp(line, "reboot ", 7)    == 0) { cmd_reboot(k_skip_spaces(line+7));   return; }

    /* commands with arguments */
    if (k_strncmp(line, "echo ",  5) == 0) { terminal_writeln(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "cd ",    3) == 0) { fs_cd(k_skip_spaces(line+3));            return; }
    if (k_strncmp(line, "ls ",    3) == 0) { cmd_ls(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "mkdir ", 6) == 0) { cmd_mkdir(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "rm ",   3) == 0) { cmd_rm(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "touch ", 6) == 0) { cmd_touch(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "cp ",   3) == 0) { cmd_cp(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "mv ",   3) == 0) { cmd_mv(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "edit ", 5) == 0) { editor_open(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "cat ",  4) == 0) { cmd_cat(k_skip_spaces(line+4)); return; }
    if (k_strncmp(line, "run ",  4) == 0) { cmd_run(k_skip_spaces(line+4)); return; }
    if (k_strncmp(line, "grep ", 5) == 0) { cmd_grep(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "wc ",   3) == 0) { cmd_wc(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "head ", 5) == 0) { cmd_head(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "tail ", 5) == 0) { cmd_tail(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "find ", 5) == 0) { cmd_find(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "which ", 6) == 0) { cmd_which(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "keyboardctl ", 12) == 0) { cmd_keyboardctl(k_skip_spaces(line+12)); return; }
    if (k_strncmp(line, "loadctl ", 8) == 0) { cmd_keyboardctl(k_skip_spaces(line+8)); return; }
    if (k_strncmp(line, "proc_info ", 10) == 0) { cmd_proc_info(k_skip_spaces(line+10)); return; }
    if (k_strncmp(line, "ram_info ", 9) == 0) { cmd_ram_info(k_skip_spaces(line+9)); return; }
    if (k_strncmp(line, "gpu_info ", 9) == 0) { cmd_gpu_info(k_skip_spaces(line+9)); return; }
    if (k_strncmp(line, "hw_info ", 8) == 0) { cmd_hw_info(k_skip_spaces(line+8)); return; }

    /* unknown */
    terminal_write_color("sh: command not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(line);
}

static void run_script_text(const char* content) {
    char linebuf[256];
    int pos = 0;
    for (int i = 0;; i++) {
        char ch = content[i];
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\0') {
            linebuf[pos] = '\0';
            const char* cmd = k_skip_spaces(linebuf);
            if (*cmd && *cmd != '#') dispatch(cmd);
            pos = 0;
            if (ch == '\0') break;
            continue;
        }
        if (pos < (int)sizeof(linebuf) - 1) linebuf[pos++] = ch;
    }
}

/* ── entry ──────────────────────────────────────────────────────── */
void shell_run(void) {
    char buf[256];
    terminal_clear();
    terminal_write_color(
        "  ____                                   _     ___  ____  \n"
        " | __ )  __ _ _ __   __ _ _ __   __ _  / \\   / _ \\/ ___| \n"
        " |  _ \\ / _` | '_ \\ / _` | '_ \\ / _` |/ _ \\ | | | \\___ \\  \n"
        " | |_) | (_| | | | | (_| | | | | (_| / ___ \\| |_| |___) | \n"
        " |____/ \\__,_|_| |_|\\__,_|_| |_|\\__,_/_/   \\_\\\\___/|____/  \n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    
    terminal_writeln("");
    terminal_write_color("  Welcome to Banana OS 0.3  --  ",
                         VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    terminal_writeln("type 'help' to get started.");
    terminal_writeln("");

    fs_init();

    while (1) {
        buf[0] = '\0';
        print_prompt();
        shell_readline(buf, sizeof(buf));
        dispatch(buf);
    }
}

void shell_run_window(int vt) {
    char buf[256];
    terminal_vt_set_active(vt);
    terminal_clear();
    terminal_write_color("  Welcome to Banana OS 0.3  --  ",
                         VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    terminal_writeln("type 'help' to get started.");
    terminal_writeln("");

    /* Note: fs_init() is intentionally not called here - the filesystem
     * is a single OS-wide resource shell_run() already set up once; every
     * window shares it (and its current directory), same as every other
     * process on a real OS sharing one mounted filesystem. */
    while (1) {
        buf[0] = '\0';
        print_prompt();
        shell_readline(buf, sizeof(buf));
        dispatch(buf);
    }
}