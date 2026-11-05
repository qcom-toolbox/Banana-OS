/* SPDX-License-Identifier: GPL-3.0-or-later */
#define VT_KERNEL 1
#include "powercmds.h"
#include "../apps/terminal/vt.h"
#include "../kernel/vcons.h"
#include "shell.h"
#include "../kernel/boottime.h"
#include "editor.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/utf8.h"
#include "../kernel/fs.h"
#include "../kernel/sysinfo.h"
#include "../kernel/timer.h"
#include "../kernel/daemon.h"
#include "../kernel/usb.h"
#include "../kernel/task.h"
#include "../kernel/smp.h"
#include "../kernel/splash.h"
#include "../kernel/gui.h"
#include "../kernel/fb.h"
#include "../kernel/rtc.h"
#include "../kernel/types.h"
#include "../kernel/ata.h"
#include "../kernel/fsdisk.h"
#include "../kernel/zswap.h"
#include "../kernel/disk.h"
#include "../kernel/ahci.h"
#include "../kernel/clipboard.h"
#include "netcmds.h"
#include "srvcmds.h"
#include "syscmds.h"
#include "moncmds.h"
#include "fscmds.h"
#include "help.h"
#include "../kernel/vmtools.h"
#include "../kernel/serial.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/tty.h"
#include "../kernel/examples.h"
#include "../kernel/settings.h"
#include "../kernel/config.h"
#include "../kernel/hostname.h"
#include "../kernel/login.h"
#include "../kernel/passwd.h"
#include "../kernel/idt.h"
#include "../kernel/acpi.h"
#include "../kernel/acpi_sleep.h"
#include "../kernel/gpu.h"
#include "../kernel/driver.h"
#include "../kernel/builtin_apps.h"
#include "../kernel/display.h"
#include "../kernel/pkg.h"
#include "../kernel/app.h"
#include "../kernel/pty.h"
#include "../kernel/kheap.h"
#include "../kernel/meminfo.h"

extern char _kernel_end[];   /* boot/linker.ld */

#define SHELL_USER "banana"


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
static const char* path_value(void);
static void dispatch(const char* line, int persona);
static void run_script_text(const char* content, int persona);

#define SH_LINE_MAX     1024   /* long enough for real-world download URLs */
#define SH_HISTORY_MAX  16

/* ── shell personas ─────────────────────────────────────────────── *
 * Two shell "flavors" share one command engine (same builtins, same
 * filesystem, same history): the stock Banana sh (unchanged look and
 * feel) and a bash-compatible persona with a bash-style prompt/banner
 * plus bash staples (aliases, $VAR/export, `!!`). Which one a *new*
 * shell instance boots into is decided once, at shell_run()/
 * shell_run_window() startup, by reading g_default_shell_kind - exactly
 * like real Unix chsh(1), which only takes effect on your next login
 * and never touches the session you ran it from. */
#define SHELL_KIND_SH   0
#define SHELL_KIND_BASH 1

static int g_default_shell_kind = SHELL_KIND_SH;

static const char* shell_kind_name(int kind) {
    return (kind == SHELL_KIND_BASH) ? "bash" : "sh";
}

/* ── aliases (bash-flavored builtin) ───────────────────────────── */
#define ALIAS_MAX      16
#define ALIAS_NAME_LEN 32
#define ALIAS_VAL_LEN  128

static char alias_name[ALIAS_MAX][ALIAS_NAME_LEN];
static char alias_val[ALIAS_MAX][ALIAS_VAL_LEN];
static int  alias_count = 0;

static const char* alias_lookup(const char* name) {
    for (int i = 0; i < alias_count; i++)
        if (k_strcmp(alias_name[i], name) == 0) return alias_val[i];
    return (void*)0;
}

/* ── environment variables ($VAR / export, bash-flavored builtin) ─ */
#define ENV_MAX      16
#define ENV_NAME_LEN 32
#define ENV_VAL_LEN  128

static char env_name[ENV_MAX][ENV_NAME_LEN];
static char env_val[ENV_MAX][ENV_VAL_LEN];
static int  env_count = 0;

static char sh_history[SH_HISTORY_MAX][SH_LINE_MAX];
static int  sh_hist_count = 0;

static void k_strcpy_n(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* one word; quotes group words ("My Documents") and are dropped, like a real shell */
static int next_token(const char** p, char* out, int outlen) {
    const char* s = k_skip_spaces(*p);
    int i = 0, n = 0;
    char q = 0;
    if (!*s) return 0;
    while (s[i] && (q || s[i] != ' ')) {
        char c = s[i++];
        if (q) { if (c == q) { q = 0; continue; } }
        else if (c == 0x22 || c == 0x27) { q = c; continue; }
        if (n < outlen - 1) out[n++] = c;
    }
    out[n] = '\0';
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

/* ── alias / env storage ────────────────────────────────────────── */
static void alias_set(const char* name, const char* value) {
    for (int i = 0; i < alias_count; i++) {
        if (k_strcmp(alias_name[i], name) == 0) {
            k_strcpy_n(alias_val[i], value, ALIAS_VAL_LEN);
            return;
        }
    }
    if (alias_count < ALIAS_MAX) {
        k_strcpy_n(alias_name[alias_count], name, ALIAS_NAME_LEN);
        k_strcpy_n(alias_val[alias_count], value, ALIAS_VAL_LEN);
        alias_count++;
    } else {
        terminal_write_color("alias: table full\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    }
}

static int alias_unset(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (k_strcmp(alias_name[i], name) == 0) {
            for (int j = i; j < alias_count - 1; j++) {
                k_strcpy_n(alias_name[j], alias_name[j + 1], ALIAS_NAME_LEN);
                k_strcpy_n(alias_val[j],  alias_val[j + 1],  ALIAS_VAL_LEN);
            }
            alias_count--;
            return 1;
        }
    }
    return 0;
}

static const char* env_lookup(const char* name) {
    for (int i = 0; i < env_count; i++)
        if (k_strcmp(env_name[i], name) == 0) return env_val[i];
    return (void*)0;
}

static void env_set(const char* name, const char* value) {
    for (int i = 0; i < env_count; i++) {
        if (k_strcmp(env_name[i], name) == 0) {
            k_strcpy_n(env_val[i], value, ENV_VAL_LEN);
            return;
        }
    }
    if (env_count < ENV_MAX) {
        k_strcpy_n(env_name[env_count], name, ENV_NAME_LEN);
        k_strcpy_n(env_val[env_count], value, ENV_VAL_LEN);
        env_count++;
    } else {
        terminal_write_color("export: table full\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    }
}

static int env_unset(const char* name) {
    for (int i = 0; i < env_count; i++) {
        if (k_strcmp(env_name[i], name) == 0) {
            for (int j = i; j < env_count - 1; j++) {
                k_strcpy_n(env_name[j], env_name[j + 1], ENV_NAME_LEN);
                k_strcpy_n(env_val[j],  env_val[j + 1],  ENV_VAL_LEN);
            }
            env_count--;
            return 1;
        }
    }
    return 0;
}

/* $HOME/$USER/$SHELL/$PWD/$PATH are resolved live (not stored) so they
 * always track the current directory and the running instance's own
 * persona, rather than going stale like a snapshot taken at export time
 * would; anything else falls back to the user-defined table above. */
static void resolve_var(const char* name, int persona, char* out, int outlen) {
    if (k_strcmp(name, "HOME") == 0)  { k_strcpy_n(out, "/home/banana", outlen); return; }
    if (k_strcmp(name, "USER") == 0)  { k_strcpy_n(out, "banana", outlen); return; }
    if (k_strcmp(name, "SHELL") == 0) {
        k_strcpy_n(out, (persona == SHELL_KIND_BASH) ? "/bin/bash" : "/bin/sh", outlen);
        return;
    }
    if (k_strcmp(name, "PWD") == 0)  { fs_cwd_path(out, outlen); return; }
    if (k_strcmp(name, "PATH") == 0) { k_strcpy_n(out, path_value(), outlen); return; }
    const char* v = env_lookup(name);
    k_strcpy_n(out, v ? v : "", outlen);
}

static int is_ident_start(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Expands any "$NAME" variable references in a raw command line before
 * it's tokenized and dispatched - applied uniformly for both personas and
 * for script lines run via `run`, same as a real shell's variable
 * expansion. ("!!" history-bang is handled separately, in
 * shell_readline() - see the comment there for why.) */
static void expand_line(const char* in, char* out, int outlen, int persona) {
    int oi = 0;
    for (int i = 0; in[i] && oi < outlen - 1;) {
        if (in[i] == '$' && is_ident_start(in[i + 1])) {
            char name[ENV_NAME_LEN];
            int j = 0;
            i++;
            while (is_ident_char(in[i]) && j < ENV_NAME_LEN - 1) name[j++] = in[i++];
            name[j] = '\0';
            char val[ENV_VAL_LEN];
            resolve_var(name, persona, val, sizeof(val));
            for (int k = 0; val[k] && oi < outlen - 1; k++) out[oi++] = val[k];
        } else {
            out[oi++] = in[i++];
        }
    }
    out[oi] = '\0';
}

/* ── ACPI power off ──────────────────────────────────────────────── */
static void acpi_poweroff(void) {
    /* the firmware's own registers (FADT + the DSDT's \_S5_): VMware, real PCs... */
    acpi_s5_poweroff();
    /* still on: the well-known ports (VirtualBox 0x4004, QEMU PIIX4 and
     * Bochs 0xB004, QEMU q35 0x604) */
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x3400),"Nd"((uint16_t)0x4004));
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x2000),"Nd"((uint16_t)0xB004));
    __asm__ volatile("outw %0, %1"::"a"((uint16_t)0x2000),"Nd"((uint16_t)0x0604));
    /* nothing worked: stop the CPU quietly (no triple fault: VMware takes
     * that for a crash) */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── reboot: three ways, each tried if the one before did nothing ──── */
static void do_reboot(void) {
    __asm__ volatile("cli");
    lapic_before_reset();                    /* (VMs: the BIOS needs it after the reset) */
    /* 1. the keyboard controller's reset line (waiting for it a bounded
     *    time: some controllers - or none, behind USB - never get ready) */
    for (int i = 0; i < 100000; i++) {
        uint8_t v;
        __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"((uint16_t)0x64));
        if (!(v & 0x02)) break;
    }
    __asm__ volatile("outb %0,%1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (volatile int i = 0; i < 10000000; i++) { }
    /* 2. the chipset's reset control register (Intel PIIX/ICH and most others) */
    __asm__ volatile("outb %0,%1" :: "a"((uint8_t)0x02), "Nd"((uint16_t)0xCF9));
    __asm__ volatile("outb %0,%1" :: "a"((uint8_t)0x06), "Nd"((uint16_t)0xCF9));
    for (volatile int i = 0; i < 10000000; i++) { }
    /* 3. a triple fault: no interrupt table, then an interrupt */
    static const struct __attribute__((packed)) { uint16_t limit; uintptr_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int $3" :: "m"(none));
    for (;;) __asm__ volatile("hlt");
}

/* ── deferred power actions (non-blocking) ──────────────────────── */
static int      shutdown_pending = 0;
static uint32_t shutdown_deadline_tick = 0;

/* If this session installed to (or booted from) a persistent disk,
 * flush the current filesystem state to it before any power action -
 * same spirit as real Unix syncing before shutdown/reboot, just done
 * for us automatically instead of relying on the user to run `sync`. */
static void sync_if_installed(void) {
    if (!fsdisk_is_installed()) return;
    terminal_write_color("Syncing filesystem to disk...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    fsdisk_sync();
}

static void poll_deferred_actions(void) {
    if (!shutdown_pending) return;
    if ((int32_t)(timer_ticks() - shutdown_deadline_tick) < 0) return;

    shutdown_pending = 0;
    terminal_write_color("\nShutting down now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    sync_if_installed();
    timer_sleep_ms(150);
    acpi_poweroff();
}

/* the desktop's Start > Shut down: the files saved, then off or restart */
void shell_power(int restart) {
    sync_if_installed();
    timer_sleep_ms(150);
    if (restart) do_reboot();
    else acpi_poweroff();
}

void shell_sleep(void) {
    char reason[160];
    if(acpi_s3_status(reason,sizeof(reason))){terminal_writeln(reason);return;}
    terminal_writeln("Entering ACPI S3 sleep; use the VM's wake/power control to resume.");
    acpi_s3_suspend(0,reason,sizeof(reason));terminal_writeln(reason);
}
static void cmd_sleep(const char* args) {
    char reason[160];
    if(!strcmp(args,"status")) { acpi_s3_status(reason,sizeof(reason));terminal_writeln(reason);return; }
    unsigned seconds=0;
    if(*args) {
        int n=k_parse_u32(args,&seconds);
        if(!n || args[n] || !seconds || seconds>3600){terminal_writeln("usage: sleep [status | SECONDS (1..3600)]");return;}
    }
    if(acpi_s3_status(reason,sizeof(reason))){terminal_writeln(reason);return;}
    terminal_writeln(seconds?"Entering ACPI S3; RTC wake is armed...":"Entering ACPI S3; wake through the VM power control...");
    acpi_s3_suspend(seconds,reason,sizeof(reason));terminal_writeln(reason);
}

/* Installing and updating are the live CD's (or USB stick's) work: on an
 * installed system these commands are not there at all. */
static int live_only(const char* name) {
    return k_strcmp(name, "install") == 0 || k_strcmp(name, "update") == 0 || k_strcmp(name, "installer") == 0 ||
           k_strcmp(name, "repair") == 0;
}
static int cmd_hidden(const char* name) { return live_only(name) && !sysinfo_live_boot(); }

/* ── commands ────────────────────────────────────────────────────── */

/* autologin [on|off]: no password asked at boot (the login screen's switch,
 * Settings > Startup). The lock screen still asks for it. */
static void cmd_autologin(const char* args) {
    if (k_strcmp(args, "on") == 0) login_set_enabled(0);
    else if (k_strcmp(args, "off") == 0) login_set_enabled(1);
    else if (args[0]) { terminal_writeln("Usage: autologin [on|off]"); return; }
    if (!passwd_is_set(PASSWD_USER))
        terminal_writeln("autologin: on - no password is set, so none is ever asked (passwd sets one)");
    else
        terminal_writeln(login_enabled() ? "autologin: off - the password is asked at boot"
                                         : "autologin: on - Banana OS logs in at boot without the password");
}

static void cmd_reboot(const char* args) {
    (void)args;
    terminal_write_color("\nRebooting now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    sync_if_installed();
    timer_sleep_ms(150);
    do_reboot();
}

static void cmd_shutdown(const char* args) {
    if (k_strcmp(args, "now") == 0) {
        shutdown_pending = 0;
        terminal_write_color("\nShutting down now...\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        sync_if_installed();
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

static void cmd_neofetch(int persona) {
    const sysinfo_t* si = sysinfo_get();
    char mbuf[16];

    terminal_write_color("\n"
        "                     _\n"
        "                    ( )\n", VGA_COLOR_BROWN, VGA_COLOR_BLACK);
    terminal_write_color(
        "                    | |\n"
        "                   /  |\n"
        "                  /   |\n"
        "                 /    |\n"
        "               ,'     /\n"
        "             ,'      /\n"
        "          _,'       /\n"
        "     __,-'        ,'\n"
        " ,--'          _,'\n"
        " `-.____,,.--''\n\n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);

    terminal_write_color("  Banana OS", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln(" 0.6");
    terminal_writeln("  --------------------");

    terminal_write_color("  OS:       ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("Banana OS 0.6");
    terminal_write_color("  KERNEL:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("Banana Kernel 0.6");
    terminal_write_color("  ARCH:     ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(BANANA_ARCH_DESC);
    terminal_write_color("  BOOT:     ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->uefi ? "UEFI" : "BIOS");
    terminal_write_color("  SHELL:    ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(vcons_running() ? "GNU bash 5.2.37"
                     : (persona == SHELL_KIND_BASH) ? "bash (Banana bash)" : "sh (Banana sh)");
    terminal_write_color("  CPU:      ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Whatever your hypervisor gives you");
    terminal_write_color("  VENDOR:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_vendor[0] ? si->cpu_vendor : "Unknown");
    terminal_write_color("  MEMORY:   ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (si->mem_kb > 0) {
        char* ms = u32_to_str(si->mem_kb / 1024u, mbuf, sizeof(mbuf));
        terminal_write(ms);
        terminal_writeln(" MB");
    } else {
        terminal_writeln("Unknown");
    }
    terminal_write_color("  DISPLAY:  ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (fb_available()) {
        char d[48];
        ksnprintf(d, sizeof(d), "framebuffer %ux%u, %u bpp", fb_info()->width, fb_info()->height, (uint32_t)fb_info()->bpp);
        terminal_writeln(d);
    } else {
        terminal_writeln("VGA text 80x25");
    }
    terminal_write_color("\n  ** Powered by pure potassium **\n",
                         VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

/* sysinfo: the system's facts, one "key: value" a line - for scripts (the
 * neofetch port reads them: Banana OS has no /proc) */
static void cmd_sysinfo(void) {
    const sysinfo_t* si = sysinfo_get();
    char b[160];
    terminal_writeln("os: Banana OS");
    terminal_writeln("version: 0.6");
#ifdef __x86_64__
    terminal_writeln("arch: x86_64");
#else
    terminal_writeln("arch: i686");
#endif
    terminal_writeln(si->uefi ? "boot: UEFI" : "boot: BIOS");
    ksnprintf(b, sizeof(b), "cpu: %s", si->cpu_brand[0] ? si->cpu_brand : "unknown");
    terminal_writeln(b);
    ksnprintf(b, sizeof(b), "threads: %d", cpu_count());
    terminal_writeln(b);
    meminfo_t m;
    meminfo_get(&m);
    ksnprintf(b, sizeof(b), "memory_used_kb: %u", (uint32_t)m.used_kb);
    terminal_writeln(b);
    ksnprintf(b, sizeof(b), "memory_total_kb: %u", (uint32_t)m.total_kb);
    terminal_writeln(b);
    gpu_t* g = gpu_active();
    if (g && g->name[0]) { ksnprintf(b, sizeof(b), "gpu: %s", g->name); terminal_writeln(b); }
    if (fb_available()) {
        ksnprintf(b, sizeof(b), "resolution: %ux%u", fb_info()->width, fb_info()->height);
        terminal_writeln(b);
    }
    ksnprintf(b, sizeof(b), "uptime_s: %u", (uint32_t)(timer_ms() / 1000u));
    terminal_writeln(b);
    static pkg_info_t pk[256];
    ksnprintf(b, sizeof(b), "packages: %d", pkg_list(pk, 256));
    terminal_writeln(b);
    terminal_writeln(gui_is_enabled() ? "desktop: on" : "desktop: off");
}

static void cmd_uptime(void) {
    terminal_write_color("Uptime: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    print_uptime();
    uint32_t la[3];
    task_loadavg(la);
    char b[64];
    ksnprintf(b, sizeof(b), ",  load average: %u.%02u, %u.%02u, %u.%02u", la[0] / 100, la[0] % 100,
              la[1] / 100, la[1] % 100, la[2] / 100, la[2] % 100);
    terminal_write(b);
    terminal_putchar('\n');
}

/* the boot, step by step (kernel/boottime.c): when each started, how long
 * it took, the longest ones marked */
static void cmd_boottime(void) {
    static boot_step_t st[64];
    int n = boottime_steps(st, 64);
    if (!n) { terminal_writeln("boottime: no boot steps recorded"); return; }
    uint64_t total = st[n - 1].ns, longest[3] = { 0, 0, 0 };
    for (int i = 0; i + 1 < n; i++) {               /* the three longest steps */
        uint64_t d = st[i + 1].ns - st[i].ns;
        for (int k = 0; k < 3; k++) if (d > longest[k]) {
            for (int j = 2; j > k; j--) longest[j] = longest[j - 1];
            longest[k] = d;
            break;
        }
    }
    char b[128];
    uint64_t before = boottime_before_ns(), fw, menu, load, other;
    if (before) {
        ksnprintf(b, sizeof(b), "before the kernel (firmware, boot menu, loading it): about %u ms", (uint32_t)(before / 1000000u));
        terminal_writeln(b);
        if (boottime_loader(&fw, &menu, &load, &other)) {
            ksnprintf(b, sizeof(b), "  firmware %u ms, boot menu %u ms, loading the kernel %u ms, the loader's other work %u ms",
                      (uint32_t)(fw / 1000000u), (uint32_t)(menu / 1000000u), (uint32_t)(load / 1000000u), (uint32_t)(other / 1000000u));
            terminal_writeln(b);
        }
    }
    terminal_write_color("    at ms   took ms  step\n", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    for (int i = 0; i < n; i++) {
        uint64_t d = i + 1 < n ? st[i + 1].ns - st[i].ns : 0;
        if (i + 1 < n) ksnprintf(b, sizeof(b), "  %7u   %7u  %s", (uint32_t)(st[i].ns / 1000000u), (uint32_t)(d / 1000000u), st[i].what);
        else ksnprintf(b, sizeof(b), "  %7u         -  %s", (uint32_t)(st[i].ns / 1000000u), st[i].what);
        int slow = i + 1 < n && d >= longest[2] && d >= 50000000u;      /* (one of the 3 longest, 50 ms or more) */
        if (slow) terminal_write_color(b, VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        else terminal_write(b);
        terminal_putchar('\n');
    }
    ksnprintf(b, sizeof(b), "the kernel to the last step: %u ms%s", (uint32_t)(total / 1000000u),
              before ? "" : " (the time before the kernel is not known here)");
    terminal_writeln(b);
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
    /* kernel/meminfo.h: the kernel image plus what the heap handed out
     * (file contents, network buffers, decoded images...), of the RAM
     * Banana OS runs in - the same numbers as `free` and `top` */
    meminfo_t mi;
    meminfo_get(&mi);
    uint32_t total_kb = mi.total_kb;
    uint32_t used_kb = mi.used_kb;
    uint32_t free_kb = mi.free_kb;
    uint32_t pct = (total_kb > 0) ? (used_kb * 100u) / total_kb : 0;   /* (fits: under 3.5 GB) */
    int show_all = (!args || !*k_skip_spaces(args));
    char b[16];

    if (show_all || has_flag(args, "-t")) {
        terminal_write_color("Total RAM: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str((total_kb / 1024u) + 1u, b, sizeof(b)));
        terminal_writeln(" MB");
    }
    if (show_all || has_flag(args, "-u")) {
        terminal_write_color("Used (kernel + heap): ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_write(u32_to_str((used_kb / 1024u) + 1u, b, sizeof(b)));
        terminal_writeln(" MB");
    }
    if (show_all || has_flag(args, "-h")) {
        char line[96];
        ksnprintf(line, sizeof(line), "%u KiB used of %u KiB (files: %u KiB, largest free block %u KiB)",
                  kheap_used_bytes() / 1024u, kheap_total_bytes() / 1024u,
                  fs_ram_used_bytes() / 1024u, kheap_largest_free() / 1024u);
        terminal_write_color("Kernel heap: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(line);
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
        gpu_t* g = gpu_active();
        if (g) { terminal_write(g->name); terminal_write(" (driver "); terminal_write(g->driver); terminal_writeln(")"); }
        else terminal_writeln("Generic VBE/Multiboot framebuffer (no driver)");
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

/* the rest of an escape sequence: there at once from a keyboard, a few ms
 * apart over a serial line; nothing for a lone Escape (0 after ~30 ms) */
static char esc_follow(void) {
    for (int i = 0; i < 15; i++) {
        char c = keyboard_try_getchar();
        if (c) return c;
        task_sleep_ms(2);
    }
    return 0;
}

/* `resolution [WxH]`: the modes the display can show, or a new one */
static void cmd_resolution(const char* args) {
    const char* a = args ? k_skip_spaces(args) : "";
    display_mode_t m[24];
    int n = display_modes(m, 24);
    const fb_info_t* fi = fb_info();
    char b[64];
    if (!*a) {
        ksnprintf(b, sizeof(b), "resolution: %ux%u", fi->width, fi->height);
        terminal_writeln(b);
        if (!display_can_change()) {
            /* any PC: the firmware's sizes, which the boot loader sets */
            display_mode_t bm[48];
            int nb = display_boot_modes(bm, 48);
            if (!nb) { terminal_writeln("(this display stays at this mode)"); return; }
            terminal_write("at the next start (resolution WxH | auto):");
            for (int i = 0; i < nb; i++) { ksnprintf(b, sizeof(b), " %dx%d", bm[i].w, bm[i].h); terminal_write(b); }
            terminal_putchar(0x0A);
            return;
        }
        terminal_write("modes:");
        for (int i = 0; i < n; i++) { ksnprintf(b, sizeof(b), " %dx%d", m[i].w, m[i].h); terminal_write(b); }
        terminal_putchar(0x0A);
        return;
    }
    uint32_t w = 0, h = 0;
    int k = 0;
    if (k_strcmp(a, "auto") != 0) {
        k = k_parse_u32(a, &w);
        if (!k || (a[k] != 'x' && a[k] != 'X') || !k_parse_u32(a + k + 1, &h)) { terminal_writeln("Usage: resolution [WIDTHxHEIGHT | auto]"); return; }
    }
    if (!display_can_change()) {
        int rc = display_set_boot_mode((int)w, (int)h);
        if (rc == -2) terminal_writeln("resolution: live CD - choose the size in the Banana Boot menu (Left / Right)");
        else if (rc == -3) terminal_writeln("resolution: this computer does not offer that size (see: resolution)");
        else if (rc < 0) terminal_writeln("resolution: could not write the boot settings");
        else {
            if (w) ksnprintf(b, sizeof(b), "resolution: %ux%u from the next start", w, h);
            else ksnprintf(b, sizeof(b), "resolution: automatic from the next start");
            terminal_writeln(b);
        }
        return;
    }
    if (!w) { terminal_writeln("resolution: auto is for the boot loader's sizes"); return; }
    if (display_set_mode((int)w, (int)h) < 0) { terminal_writeln("resolution: the display cannot show that mode (see: resolution)"); return; }
    ksnprintf(b, sizeof(b), "resolution: %ux%u", w, h);
    terminal_writeln(b);
}

/* `brightness [percent]`: a laptop panel's backlight (graphics driver) */
static void cmd_brightness(const char* args) {
    const char* a = args ? k_skip_spaces(args) : "";
    uint32_t v;
    int cur = gpu_backlight(-1);
    if (cur < 0) { terminal_writeln("brightness: this display has no backlight Banana OS can set"); return; }
    if (*a && k_parse_u32(a, &v)) cur = gpu_backlight(v > 100 ? 100 : (int)v);
    else if (*a) { terminal_writeln("Usage: brightness [0-100]"); return; }
    char b[40];
    ksnprintf(b, sizeof(b), "brightness: %d%%", cur);
    terminal_writeln(b);
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
    uint32_t* last_cpu = (uint32_t*)kzalloc(TASK_MAX * sizeof(uint32_t));
    task_info_t* procs = (task_info_t*)kmalloc(TASK_MAX * sizeof(task_info_t));
    if (!last_cpu || !procs) {          /* (too big for a task's stack: on the heap) */
        kfree(last_cpu); kfree(procs);
        terminal_writeln("top: out of memory");
        return;
    }
    int my_vt = terminal_vt_get_active();

    while (1) {
        const sysinfo_t* si = sysinfo_get();
        int pcount;

        terminal_clear();

        /* ── HEADER ───────────────────────────── */
        terminal_write_color("Banana OS 0.6 top - press q to quit (htop: the colour version)\n",
                             VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        terminal_writeln("--------------------------------------------");

        char b1[16], b2[16], b3[16];

        /* CPU NAME */
        terminal_write_color("CPU: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Unknown CPU");

        /* OS VERSION */
        terminal_write_color("OS:  ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        terminal_writeln("Banana OS 0.6 (Banana Kernel 0.6)");

        /* PROCESSOR CORES (kernel/smp.c) */
        terminal_write_color("Cores: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        fmt_u32(b1, sizeof(b1), (uint32_t)cpu_count());
        terminal_write(b1);
        terminal_writeln(cpu_count() > 1 ? " (kernel on core 0, app code on every core)" : " in use");

        /* RAM USAGE (kernel/meminfo.h: the same numbers as `free`) - it
         * used to count only the files' data as used, against all the RAM */
        meminfo_t mi;
        meminfo_get(&mi);
        uint32_t total_mb = (mi.total_kb + 1023u) / 1024u;
        uint32_t used_mb  = (mi.used_kb + 1023u) / 1024u;
        uint32_t free_mb  = mi.free_kb / 1024u;

        terminal_write_color("RAM: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        fmt_u32(b1, sizeof(b1), used_mb);
        fmt_u32(b2, sizeof(b2), total_mb);
        fmt_u32(b3, sizeof(b3), free_mb);

        terminal_write(b1);
        terminal_write(" MB used / ");
        terminal_write(b2);
        terminal_write(" MB (free ");
        terminal_write(b3);
        terminal_writeln(" MB)");

        terminal_write("      [");
        draw_bar(mi.used_kb, mi.total_kb);
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
            char stb[16];
            const char* state = task_state_str(procs[i].state);
            if (procs[i].state == TASK_AWAY) {          /* app code on another core */
                if (procs[i].cpu > 0) ksnprintf(stb, sizeof(stb), "core %d", procs[i].cpu);
                else kstrlcpy(stb, "waitcore", sizeof(stb));
                state = stb;
            }
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
                kfree(last_cpu);
                kfree(procs);
                return;
            }

            task_sleep_ms(50);
        }
    }
}

/* help: the pages live in shell/help.c */
static void cmd_help(const char* args) { help_command(args); }


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
        settings_set("keyboard", keyboard_layout_name());     /* kept for the next boot too */
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
    if (f->size == 0) { terminal_writeln("(empty file)"); return; }
    if (fs_is_binary(idx)) {
        char b[16];
        terminal_write("cat: ");
        terminal_write(name);
        terminal_write(": binary file (");
        terminal_write(u32_to_str(f->size, b, sizeof(b)));
        terminal_writeln(" bytes) - not printed");
        return;
    }
    fs_pin(idx);                   /* (a long file prints for a while: kept in RAM meanwhile) */
    terminal_write(f->content);
    if (f->content[f->size - 1] != '\n') terminal_putchar('\n');
    fs_unpin(idx);
}

static void cmd_run(const char* name, int persona) {
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
    fs_pin(idx);                   /* the script stays in RAM while its commands run */
    run_script_text(f->content, persona);
    fs_unpin(idx);
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
    fs_pin(idx);
    uint32_t total = count_lines(f->content);
    print_line_range(f->content, 0, (n < total) ? n : total);
    fs_unpin(idx);
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
    fs_pin(idx);
    uint32_t total = count_lines(f->content);
    uint32_t from = (total > n) ? total - n : 0;
    print_line_range(f->content, from, total);
    fs_unpin(idx);
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
    fs_pin(idx);

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
    fs_unpin(idx);
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
    "help", "neofetch", "sysinfo", "echo", "clear", "uname", "whoami", "hostname", "date", "cal", "timezone", "theme", "language",
    "ls", "cd", "pwd", "mkdir", "rm", "touch", "cp", "mv", "edit", "cat", "run",
    "uptime", "boottime", "top", "exit", "start", "stop", "startx", "stopx",
    "keyboardctl", "loadctl", "usbctl", "proc_info", "ram_info", "gpu_info",
    "hw_info", "shutdown", "reboot", "halt", "sleep", "install", "sync", "drop_caches", "update", "repair", "installer", "disks", "history", "which", "type",
    "alias", "unalias", "export", "unset", "env", "chsh", "autologin",
    "grep", "wc", "head", "tail", "find", "time", "nice",
    /* shell/netcmds.c + shell/wpcmd.c */
    "ntp", "ifconfig", "dhcp", "ping", "nslookup", "host", "netstat", "arp", "curl", "wget",
    "cryptotest", "wallpaper", "lsusb", "usb", "httpd", "sshd", "rdpd", "rdp", "passwd", "files", "browser", "notepad",
    /* shell/syscmds.c */
    "mount", "umount", "eject", "fstest", "selftest", "crashtest", "pkg", "apt", "apt-get", "adb", "fastboot", "apps", "taskmgr", "settings", "notify", "play", "beep", "volume", "lsaudio",
    /* shell/moncmds.c */
    "laptop", "usbc", "free", "swap", "latency", "df", "htop", "sched", "renice", "lscpu", "sensors", "gamepad", "ipv6", "toggle6", "ping6", "firewall", "kptest", "lsgpu", "gpubench", "brightness", "resolution", "drivers", "driver",
    /* shell/fscmds.c */
    "chmod", "chown", "chgrp", "ln", "readlink", "stat", "fsck", "e2fsck", "screenshot",
    /* kernel/vmtools.c */
    "vmtools",
    /* shell/help.c */
    "man", "apropos", "lsblk", "devmgr", "lsdev", "lspci", "devmgmt.msc", "diskmgr", "diskmgmt.msc", "diskpart", "mkfs.fat", "mkfs.vfat", "mkfs.exfat", "gpu", "gpu3d", (void*)0
};

/* ── $PATH: where commands that are not built in are looked for ───── */
/* Each folder in $PATH (':'-separated, in order) is searched for a file of
 * the command's name - a program (ELF, built with the SDK) or a shell
 * script - and the folder /apps for the installed apps (pkg). A name with
 * a '/' in it (./script.sh, /home/banana/bin/x) is run as it is. */
#define DEFAULT_PATH "/bin:/apps:/home/banana/bin"

static const char* path_value(void) {
    const char* v = env_lookup("PATH");
    return v ? v : DEFAULT_PATH;
}

enum { PATH_NONE = 0, PATH_FILE, PATH_APP };

static int path_lookup(const char* name, char* out, int cap) {
    if (!name || !name[0]) return PATH_NONE;
    if (strchr(name, '/')) {
        if (fs_find_file(name) < 0) return PATH_NONE;
        kstrlcpy(out, name, cap);
        return PATH_FILE;
    }
    const char* p = path_value();
    for (;;) {
        const char* e = p;
        while (*e && *e != ':') e++;
        char dir[FS_PATH_LEN];
        int n = (int)(e - p);
        if (n >= (int)sizeof(dir)) n = (int)sizeof(dir) - 1;
        memcpy(dir, p, (size_t)n);
        dir[n] = 0;
        if (!n) kstrlcpy(dir, ".", sizeof(dir));          /* an empty entry: the current folder */
        while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
        pkg_info_t pi;
        if (k_strcmp(dir, PKG_DIR) == 0) {
            if (pkg_get(name, &pi) == 0) { ksnprintf(out, cap, "%s/%s", PKG_DIR, name); return PATH_APP; }
        } else {
            char cand[FS_PATH_LEN];
            ksnprintf(cand, sizeof(cand), "%s/%s", k_strcmp(dir, "/") == 0 ? "" : dir, name);
            if (fs_find_file(cand) >= 0) { kstrlcpy(out, cand, cap); return PATH_FILE; }
        }
        if (!*e) return PATH_NONE;
        p = e + 1;
    }
}

/* runs what path_lookup found for the command line `line`; 1 if it ran */
static int path_run(const char* line, int persona) {
    char buf[SH_LINE_MAX];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[16];
    int argc = shell_split_args(buf, argv, 16);
    if (argc == 0) return 0;
    char path[FS_PATH_LEN];
    int kind = path_lookup(argv[0], path, sizeof(path));
    if (kind == PATH_NONE) return 0;
    char err[128];
    err[0] = 0;
    if (kind == PATH_APP) {
        if (pkg_run(argv[0], argc, argv, 0, err, sizeof(err)) < 0 && err[0]) {
            terminal_write_color(argv[0], VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(err);
        }
        return 1;
    }
    int idx = fs_find_file(path);
    uint8_t hdr[4] = { 0 };
    fs_read(idx, 0, hdr, 4);
    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        if (app_exec(path, argc, argv, err, sizeof(err)) < 0 && err[0]) {
            terminal_write_color(argv[0], VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(err);
        }
        return 1;
    }
    if (fs_is_binary(idx)) {
        terminal_write_color(argv[0], VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_write_color(": cannot run this file (neither a program nor a script)\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return 1;
    }
    fs_file_t* f = fs_get_file(idx);
    fs_pin(idx);                   /* the script stays in RAM while its commands run */
    run_script_text(f->content, persona);
    fs_unpin(idx);
    return 1;
}

static void cmd_which(const char* args) {
    const char* p = args ? args : "";
    char tok[64];
    if (!next_token(&p, tok, sizeof(tok))) {
        terminal_write_color("Usage: which <command>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    for (int i = 0; known_cmds[i]; i++) {
        if (k_strcmp(tok, known_cmds[i]) == 0 && !cmd_hidden(tok)) {
            terminal_write(tok);
            terminal_writeln(": shell builtin");
            return;
        }
    }
    char path[FS_PATH_LEN];
    if (path_lookup(tok, path, sizeof(path)) != PATH_NONE) { terminal_writeln(path); return; }
    terminal_write(tok);
    terminal_writeln(": not found");
}

/* bash's `type`: unlike `which` above, this one also knows about aliases -
 * a deliberate, authentic difference between the two commands. */
static void cmd_type(const char* args) {
    const char* p = args ? args : "";
    char tok[64];
    if (!next_token(&p, tok, sizeof(tok))) {
        terminal_write_color("Usage: type <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    const char* av = alias_lookup(tok);
    if (av) {
        terminal_write(tok);
        terminal_write(" is aliased to `");
        terminal_write(av);
        terminal_writeln("'");
        return;
    }
    for (int i = 0; known_cmds[i]; i++) {
        if (k_strcmp(tok, known_cmds[i]) == 0 && !cmd_hidden(tok)) {
            terminal_write(tok);
            terminal_writeln(" is a shell builtin");
            return;
        }
    }
    char path[FS_PATH_LEN];
    int kind = path_lookup(tok, path, sizeof(path));
    if (kind != PATH_NONE) {
        terminal_write(tok);
        terminal_write(kind == PATH_APP ? " is an installed app, " : " is ");
        terminal_writeln(path);
        return;
    }
    terminal_write(tok);
    terminal_writeln(": not found");
}

/* "name=value" (optionally quoted value) shared by alias/export - unlike
 * next_token(), this does NOT split on spaces, so `alias ll='ls -l'`
 * keeps its value intact instead of getting cut at the first space. */
static void parse_assignment(const char* a, char* name, int name_len, char* value, int value_len) {
    int eq = -1;
    for (int i = 0; a[i]; i++) { if (a[i] == '=') { eq = i; break; } }
    if (eq < 0) { name[0] = '\0'; value[0] = '\0'; return; }

    int nl = (eq < name_len - 1) ? eq : name_len - 1;
    for (int i = 0; i < nl; i++) name[i] = a[i];
    name[nl] = '\0';

    const char* val = a + eq + 1;
    int vl = k_strlen(val);
    if (vl >= 2 && (val[0] == '\'' || val[0] == '"') && val[vl - 1] == val[0]) {
        int inner = vl - 2;
        if (inner >= value_len) inner = value_len - 1;
        for (int i = 0; i < inner; i++) value[i] = val[1 + i];
        value[inner] = '\0';
    } else {
        k_strcpy_n(value, val, value_len);
    }
}

static void cmd_alias(const char* args) {
    const char* a = args ? k_skip_spaces(args) : "";
    if (!*a) {
        for (int i = 0; i < alias_count; i++) {
            terminal_write("alias ");
            terminal_write(alias_name[i]);
            terminal_write("='");
            terminal_write(alias_val[i]);
            terminal_writeln("'");
        }
        return;
    }

    char name[ALIAS_NAME_LEN], value[ALIAS_VAL_LEN];
    parse_assignment(a, name, sizeof(name), value, sizeof(value));
    if (!name[0]) {
        /* "alias foo" with no '=' - show that one definition */
        char tok[ALIAS_NAME_LEN];
        const char* pp = a;
        next_token(&pp, tok, sizeof(tok));
        const char* v = alias_lookup(tok);
        if (v) {
            terminal_write("alias "); terminal_write(tok);
            terminal_write("='"); terminal_write(v); terminal_writeln("'");
        } else {
            terminal_write_color("alias: not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(tok);
        }
        return;
    }
    alias_set(name, value);
}

static void cmd_unalias(const char* args) {
    char name[ALIAS_NAME_LEN];
    const char* p = args ? args : "";
    if (!next_token(&p, name, sizeof(name))) {
        terminal_write_color("Usage: unalias <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (!alias_unset(name)) {
        terminal_write_color("unalias: not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(name);
    }
}

static void print_env(int persona) {
    static const char* const dyn_names[] = { "HOME", "USER", "SHELL", "PWD", "PATH" };
    for (int i = 0; i < 5; i++) {
        char val[FS_PATH_LEN];
        resolve_var(dyn_names[i], persona, val, sizeof(val));
        terminal_write(dyn_names[i]);
        terminal_write("=");
        terminal_writeln(val);
    }
    for (int i = 0; i < env_count; i++) {
        if (k_strcmp(env_name[i], "PATH") == 0) continue;     /* (shown above) */
        terminal_write(env_name[i]);
        terminal_write("=");
        terminal_writeln(env_val[i]);
    }
}

static void cmd_export(const char* args, int persona) {
    const char* a = args ? k_skip_spaces(args) : "";
    if (!*a) { print_env(persona); return; }

    char name[ENV_NAME_LEN], value[ENV_VAL_LEN];
    parse_assignment(a, name, sizeof(name), value, sizeof(value));
    if (!name[0]) return; /* "export NAME" with no value: no-op, nothing to scope here */
    env_set(name, value);
}

static void cmd_unset(const char* args) {
    char name[ENV_NAME_LEN];
    const char* p = args ? args : "";
    if (!next_token(&p, name, sizeof(name))) {
        terminal_write_color("Usage: unset <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (!env_unset(name)) {
        terminal_write_color("unset: not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(name);
    }
}

const char* shell_terminal_path(void) {
#ifndef __x86_64__
    return g_default_shell_kind == SHELL_KIND_BASH ? "/bin/bash" : "/bin/sh";
#else
    char selected[16];
    if (settings_get("shell", selected, sizeof(selected))) {
        if (!strcmp(selected, "zsh") && fs_find_file("/bin/zsh") >= 0) return "/bin/zsh";
        if (!strcmp(selected, "sh") && fs_find_file("/bin/sh") >= 0) return "/bin/sh";
    }
    return "/bin/bash";
#endif
}

static void cmd_chsh(const char* args, int persona) {
    (void)persona;
    const char* a = args ? k_skip_spaces(args) : "";
    if (!*a) {
        terminal_write("Default terminal shell: "); terminal_writeln(shell_terminal_path());
        terminal_writeln("Usage: chsh <sh|bash|zsh>");
        return;
    }
    char tok[32]; const char* rest = a;
    next_token(&rest, tok, sizeof(tok));
    const char* name = !strncmp(tok, "/bin/", 5) ? tok + 5 : tok;
    if (*k_skip_spaces(rest) || (strcmp(name, "sh") && strcmp(name, "bash") && strcmp(name, "zsh"))) {
        terminal_writeln("chsh: choose sh, bash or zsh"); return;
    }
#ifndef __x86_64__
    if (!strcmp(name, "zsh")) { terminal_writeln("chsh: zsh needs the 64-bit system"); return; }
#else
    char path[24]; ksnprintf(path, sizeof(path), "/bin/%s", name);
    if (fs_find_file(path) < 0) { terminal_writeln("chsh: that shell is not installed"); return; }
#endif
    if (cfg_set(CFG_SETTINGS, "shell", name, "# Banana OS settings\n") != 0) {
        terminal_writeln("chsh: could not save the shell"); return;
    }
    cfg_persist();
    g_default_shell_kind = !strcmp(name, "sh") ? SHELL_KIND_SH : SHELL_KIND_BASH;
    terminal_write("Default terminal shell set to: "); terminal_writeln(name);
    terminal_writeln("New Terminal windows and tabs use it; existing shells keep running.");
}

static void cmd_uname(const char* args, int persona) {
    args = k_skip_spaces(args);
    if (!strcmp(args, "-n") || !strcmp(args, "--nodename")) { terminal_writeln(hostname_get()); return; }
    if (!strcmp(args, "-a") || !strcmp(args, "--all")) {
        terminal_write(hostname_get()); terminal_write(" ");
    }
    if (persona == SHELL_KIND_BASH) terminal_writeln("Banana OS 0.6 " BANANA_ARCH " Banana Kernel 0.6 bash");
    else                             terminal_writeln("Banana OS 0.6 " BANANA_ARCH " Banana Kernel 0.6 sh");
}

static void cmd_whoami(void) { terminal_writeln("banana"); }
static int cmd_hostname(const char* args) {
    args = k_skip_spaces(args);
    if (!*args || !strcmp(args, "-s")) { terminal_writeln(hostname_get()); return 0; }
    if (!strcmp(args, "--help")) { terminal_writeln("usage: hostname [NAME] - show or change the PC name"); return 0; }
    char name[HOSTNAME_MAX + 2];
    const char* rest = args;
    next_token(&rest, name, sizeof(name));
    if (*k_skip_spaces(rest)) { terminal_writeln("usage: hostname [NAME]"); return 1; }
    int rc = hostname_set(name);
    if (rc == -1) terminal_writeln("hostname: use 1-63 letters, digits or hyphens; start and end with a letter or digit");
    else if (rc) terminal_writeln("hostname: could not save /etc/hostname");
    return rc ? 1 : 0;
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
        terminal_write_color(fs_perm_error() ? "mkdir: permission denied: " : "mkdir: cannot create directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
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
        if (fs_touch(tok) < 0) {
            terminal_write_color(fs_perm_error() ? "touch: permission denied: " : "touch: cannot create: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
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
        terminal_write_color(fs_perm_error() ? "cp: permission denied: " : "cp: cannot copy: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
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
        terminal_write_color(fs_perm_error() ? "mv: permission denied: " : "mv: cannot move: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(src);
    }
}

static void cmd_exit(void) {
    int tt = tty_current();
    if (tt >= 0) { tty_finish(tt); return; }      /* log out of SSH */
    int my_vt = terminal_vt_get_active();
    if (my_vt != 0 && gui_close_terminal_by_vt(my_vt)) return;
    terminal_write_color(
        "exit: nothing to close here (this isn't a GUI terminal window)\n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
}

static void cmd_halt(void) {
    sync_if_installed();
    terminal_write_color("\nSystem halted.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    __asm__ volatile("cli; hlt");
}

/* ── install / sync (persistent disk) ──────────────────────────────
 * `install` formats a dedicated ATA hard disk (not the boot CD -
 * ata_probe_disks() skips ATAPI drives) and writes the current in-memory
 * filesystem to it. Once installed, filesystem changes persist across
 * reboots: `sync` writes back on demand, and shutdown/reboot/halt do it
 * automatically (see sync_if_installed() above). Booting itself is
 * unaffected - this makes the filesystem *contents* persistent, it does
 * not make the disk bootable on its own. */
/* The keys typed before a question was shown are not its answer: the
 * Enter that ran the command (a second one from a keyboard that repeats
 * it, or a carriage return after it), keys pressed while `update` compared
 * the disk with the CD... Those are thrown away, and an Enter in the first
 * half second after the question is not taken either - it answered "no"
 * before anyone could press y. */
static void drop_typed_ahead(void) {
    for (int i = 0; i < 256 && keyboard_try_getchar(); i++) { }
}

static int prompt_yes_no(void) {
    int my_vt = terminal_vt_get_active();
    drop_typed_ahead();
    uint32_t shown = timer_ms();
    while (1) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (!c) { task_sleep_ms(10); continue; }
        if (c == 'y' || c == 'Y') { terminal_writeln("y"); return 1; }
        if ((c == '\n' || c == '\r') && timer_ms() - shown < 500) continue;   /* (typed before the question) */
        if (c == 'n' || c == 'N' || c == '\n' || c == '\r' || c == 3 || c == 27) { terminal_writeln("n"); return 0; }
    }
}

/* a number from 1 to max, typed and Enter; 0 if cancelled (Enter alone,
 * Esc, Ctrl+C) - a number out of range is asked again */
static int prompt_number(int max) {
    int my_vt = terminal_vt_get_active();
    char buf[8];
    int n = 0;
    drop_typed_ahead();
    uint32_t shown = timer_ms();
    for (;;) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (!c) { task_sleep_ms(10); continue; }
        if (c == 3 || c == 27) { terminal_putchar('\n'); return 0; }
        if (c == '\b' || c == 127) {
            if (n) { n--; terminal_cursor_left(); terminal_putchar(' '); terminal_cursor_left(); }
            continue;
        }
        if ((c == '\n' || c == '\r') && !n && timer_ms() - shown < 500) continue;   /* (typed before the question) */
        if (c == '\n' || c == '\r') {
            terminal_putchar('\n');
            if (!n) return 0;
            buf[n] = 0;
            uint32_t v;
            if (k_parse_u32(buf, &v) && v >= 1 && (int)v <= max) return (int)v;
            char again[48];
            ksnprintf(again, sizeof(again), "No disk %s - a number from 1 to %d: ", buf, max);
            terminal_write_color(again, VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            n = 0;
            continue;
        }
        if (c >= '0' && c <= '9' && n < (int)sizeof(buf) - 1) { buf[n++] = c; terminal_putchar(c); }
    }
}

static void print_disk_line(const ata_disk_t* d) {
    char b[16], where[32];
    disk_describe(d, where, sizeof(where));
    terminal_write("  ");
    terminal_write(where);
    terminal_write("  ");
    terminal_write(d->is_atapi ? "CD/DVD" : d->model[0] ? d->model : "(unknown model)");
    if (d->is_atapi && d->model[0]) { terminal_write(" "); terminal_write(d->model); }
    if (!d->is_atapi && d->sectors) {
        terminal_write("  ~");
        terminal_write(u32_to_str(d->sectors / 2048u, b, sizeof(b))); /* 512B sectors -> MB */
        terminal_write(" MB");
    }
    terminal_putchar('\n');
}

static void cmd_disks(void) {
    ata_disk_t all[DISK_MAX];
    int n = disk_probe(all, DISK_MAX);
    if (!n) terminal_writeln("disks: no IDE or SATA drive found");
    for (int i = 0; i < n; i++) print_disk_line(&all[i]);
    terminal_write(ahci_status());
}

/* the hard disks, numbered for `install N` / `update N`; how many */
static int list_targets(ata_disk_t* out, int max) {
    int n = fsdisk_targets(out, max);
    for (int i = 0; i < n; i++) {
        char b[8];
        terminal_write_color("  [", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        terminal_write_color(u32_to_str((uint32_t)i + 1u, b, sizeof(b)), VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        terminal_write_color("]", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        if (fsdisk_holds_banana(&out[i])) terminal_write_color(" (Banana OS)", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        else if (fsdisk_banana_unreadable(&out[i]))
            terminal_write_color(" (Banana OS - files unreadable)", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        print_disk_line(&out[i]);
    }
    return n;
}

/* the disk the number in args names (1-based), the only one, or the one
 * asked for when there are several; -1 (and why) */
static int pick_target(const char* cmd, const char* args, int n) {
    args = k_skip_spaces(args);
    uint32_t v;
    if (*args) {
        if (!k_parse_u32(args, &v) || v < 1 || (int)v > n) {
            terminal_write_color(cmd, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_write_color(": no such disk number - see the list above\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            return -1;
        }
        return (int)v - 1;
    }
    if (n == 1) return 0;
    /* several: ask which one (a number and Enter; Enter alone, Esc or Ctrl+C cancels) */
    char prompt[64];
    ksnprintf(prompt, sizeof(prompt), "Which disk? [1-%d, Enter cancels] ", n);
    terminal_write_color(prompt, VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    int k = prompt_number(n);
    if (k < 1) {
        terminal_write(cmd);
        terminal_writeln(": cancelled.");
        return -1;
    }
    return k - 1;
}

/* "install: 41 MB in 12.3 s (CD / stick 8.1 s, disk 3.9 s, files 0.4 s)" */
static void print_timing(const char* cmd, const char* what) {
    fsdisk_timing_t t;
    fsdisk_timing(&t);
    char b[160], f[32] = "";
    if (t.files_ms) ksnprintf(f, sizeof(f), ", files %u.%u s", t.files_ms / 1000u, t.files_ms % 1000u / 100u);
    ksnprintf(b, sizeof(b), "%s: %s %u MB in %u.%u s (CD / stick %u.%u s, disk %u.%u s%s)", cmd, what,
              t.written_bytes ? (t.written_bytes + 524287u) >> 20 : (t.image_bytes + 524287u) >> 20,
              t.total_ms / 1000u, t.total_ms % 1000u / 100u, t.read_ms / 1000u, t.read_ms % 1000u / 100u,
              t.disk_ms / 1000u, t.disk_ms % 1000u / 100u, f);
    terminal_writeln(b);
}

static void cmd_install(const char* args) {
    /* --full (the default), --standard, --minimal: the apps it puts on the disk */
    int profile = PROFILE_FULL;
    char num[16] = "";
    for (const char* p = k_skip_spaces(args); *p; p = k_skip_spaces(p)) {
        char w[24];
        int n = 0;
        while (p[n] && p[n] != ' ' && n < (int)sizeof(w) - 1) { w[n] = p[n]; n++; }
        w[n] = 0;
        while (p[n] && p[n] != ' ') n++;
        p += n;
        if (!k_strcmp(w, "--full")) profile = PROFILE_FULL;
        else if (!k_strcmp(w, "--standard")) profile = PROFILE_STANDARD;
        else if (!k_strcmp(w, "--minimal")) profile = PROFILE_MINIMAL;
        else if (w[0] == '-') {
            terminal_writeln("usage: install [number] [--full | --standard | --minimal]");
            return;
        } else k_strcpy_n(num, w, sizeof(num));
    }
    args = num;
    ata_disk_t all[DISK_MAX];
    int found = list_targets(all, DISK_MAX);

    if (found == 0) {
        terminal_write_color(
            "install: no hard disk found. Attach a (blank) IDE, SATA, NVMe, virtio or SCSI\n"
            "disk (or an eMMC / SD card) to the VM - besides the boot CD - then try again.\n",
            VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    int k = pick_target("install", args, found);
    if (k < 0) return;
    ata_disk_t target = all[k];

    if (fsdisk_holds_banana(&target))
        terminal_write_color(
            "This disk already has Banana OS on it, with your files. To get the new version\n"
            "and KEEP them, answer N and run 'update' instead.\n",
            VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    else if (fsdisk_banana_unreadable(&target))
        terminal_write_color(
            "This disk has a Banana OS system on it, but its files could not be read.\n"
            "Installing ERASES them for good. If they matter, answer N and copy them off\n"
            "the disk first (from another computer: it is an ext4 filesystem).\n",
            VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color("This will ERASE ", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_write(target.model[0] ? target.model : "the disk above");
    terminal_write_color(
        " and make it directly bootable (copying the boot CD or USB stick onto it, then\n"
        "writing the current filesystem). This can take a little while.\n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    char ap[160];
    ksnprintf(ap, sizeof(ap), "Apps: %s - %s\n", builtin_profile_name(profile), builtin_profile_apps(profile));
    terminal_write(ap);
    terminal_write_color("Continue? [y/N] ", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    if (!prompt_yes_no()) {
        terminal_writeln("install: cancelled.");
        return;
    }

    if (profile != PROFILE_FULL) {
        int r = builtin_profile_apply(profile);
        ksnprintf(ap, sizeof(ap), "install: %d app%s left out (App Store > Discover installs them)", r, r == 1 ? "" : "s");
        terminal_writeln(ap);
    }
    terminal_writeln("install: copying boot image and filesystem, please wait...");
    int rc = fsdisk_install_to(&target);
    if (rc == FSDISK_OK) print_timing("install", "wrote");
    switch (rc) {
        case FSDISK_OK:
            terminal_write_color(
                "install: done. This disk now boots Banana OS on its own - no CD or stick needed\n"
                "(e.g. `qemu-system-i386 -drive file=disk.img,format=raw,if=ide`).\n"
                "Files and settings are saved to it automatically, a few seconds after\n"
                "each change ('sync' saves right away).\n",
                VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            break;
        case FSDISK_ERR_NO_SOURCE:
            terminal_write_color(
                "install: no Banana OS CD or USB stick to copy the system from. Boot from the\n"
                "ISO (a CD, or a USB stick it was written to) and leave it plugged in.\n",
                VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
        case FSDISK_ERR_TOO_SMALL:
            terminal_write_color(
                "install: target disk is too small (need room for the boot image\n"
                "plus the files). Use a larger disk and retry.\n",
                VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
        case FSDISK_ERR_ISO_TOO_BIG:
            terminal_write_color(
                "install: the boot image no longer fits the reserved space - this is a\n"
                "Banana OS build issue, not something fixable from here.\n",
                VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
        case FSDISK_ERR_FULL:
            terminal_write_color("install: the files are too big for this disk - delete some first.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
        default:
            terminal_write_color("install: failed (disk I/O error).\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
    }
}

/* `update`: from a (newer) Banana OS CD, rewrites the installed disk's
 * system and leaves its files and settings alone */
/* update, or repair (repair: also when the system is the same, then every
 * built-in app installed again at the next start - files, settings and the
 * password kept) */
static void cmd_update_mode(const char* args, int repair);
static void cmd_update(const char* args) { cmd_update_mode(args, 0); }
static void cmd_update_mode(const char* args, int repair) {
    ata_disk_t all[DISK_MAX], inst[DISK_MAX];
    int n = fsdisk_targets(all, DISK_MAX), ni = 0;
    for (int i = 0; i < n; i++) if (fsdisk_holds_banana(&all[i])) inst[ni++] = all[i];
    if (!ni) {
        int broken = 0;
        for (int i = 0; i < n; i++) broken |= fsdisk_banana_unreadable(&all[i]);
        if (broken)
            /* (never "use install": that erases the files the update was meant to keep) */
            terminal_write_color("update: a disk has Banana OS on it, but its files could not be read - it\n"
                                 "        cannot be updated. 'install' would ERASE those files: copy them off\n"
                                 "        first if they matter (it is an ext4 filesystem).\n",
                                 VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        else
            terminal_write_color("update: no disk with Banana OS installed was found (use 'install').\n",
                                 VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    for (int i = 0; i < ni; i++) {
        char b[8];
        if (ni > 1) { terminal_write("  ["); terminal_write(u32_to_str((uint32_t)i + 1u, b, sizeof(b))); terminal_write("]"); }
        print_disk_line(&inst[i]);
    }
    int k = pick_target("update", args, ni);
    if (k < 0) return;
    ata_disk_t d = inst[k];
    terminal_writeln("update: comparing the installed system with this CD / USB stick...");
    terminal_flush();
    int same = k == 0 ? fsdisk_compare_boot() : 0;    /* (the comparison reads the first install) */
    if (k == 0 && same >= 0) print_timing("update", "compared");
    if (same == FSDISK_ERR_NO_SOURCE) {
        terminal_write_color("update: no Banana OS CD or USB stick found - plug in the one with the new version.\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    if (same == FSDISK_ERR_ISO_TOO_BIG) {
        terminal_write_color("update: this system image is too big for the installed disk's boot area.\n"
                             "        Back up your files and reinstall to create a larger boot area.\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    if (same < 0) {
        terminal_write_color("update: could not read the disk or the CD / USB stick.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    if (fsdisk_install_layout() == 3)
        terminal_writeln("update: this install uses the old disk layout (32 MB boot area) - it will be\n"
                         "        moved to the new one: a 128 MB boot area, your files using the whole disk.");
    if (same == 1 && !repair) {
        terminal_write_color("update: the installed system is already the same as this CD - nothing to do.\n"
                             "        (repair: install every built-in app on it again)\n",
                             VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        return;
    }
    if (repair)
        terminal_write_color(
            "Repair Banana OS on this disk: write the system from the CD again and install\n"
            "every built-in app again at the next start (bash, Terminal, the App Store...)?\n"
            "Your files, settings and password are kept. [y/N] ",
            VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    else
        terminal_write_color(
            "Replace the Banana OS system on this disk with the one on the CD?\n"
            "Your files, settings and password are kept. [y/N] ",
            VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    if (!prompt_yes_no()) {
        terminal_writeln(repair ? "repair: cancelled." : "update: cancelled.");
        return;
    }
    int rc = FSDISK_OK;
    if (same != 1) {
        terminal_writeln("update: writing the new system, please wait (do not switch off)...");
        terminal_flush();
        rc = fsdisk_update_on(&d);
    }
    if (repair && rc == FSDISK_OK) {
        int rr = fsdisk_request_repair(&d);
        if (rr == FSDISK_OK)
            terminal_write_color("repair: every built-in app will be installed again at the next start from the disk.\n",
                                 VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        else
            terminal_write_color(rr == FSDISK_ERR_NO_INSTALL
                                 ? "repair: this disk's files are not on ext4 (an older layout): the system was\n"
                                   "        written again, but the apps cannot be marked for reinstalling.\n"
                                 : "repair: could not write the request onto the disk.\n",
                                 VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    }
    if (rc == FSDISK_OK) print_timing("update", "wrote");
    if (rc == FSDISK_OK) {
        terminal_write_color(
            "update: done. Remove the CD (or boot from the disk) and restart to use the new version.\n",
            VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    } else if (rc == FSDISK_ERR_ISO_TOO_BIG) {
        terminal_write_color("update: this CD's system is too big for the disk's boot area.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    } else if (rc == FSDISK_ERR_TOO_SMALL || rc == FSDISK_ERR_FULL) {
        terminal_write_color("update: the disk is too small for the new layout (128 MB boot area + your files).\n"
                             "        Nothing was changed.\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    } else {
        terminal_write_color("update: failed (disk I/O error). Your files are untouched; run 'update' again.\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    }
}

static void cmd_sync(void) {
    if (!fsdisk_is_installed()) {
        terminal_write_color("sync: not installed - run 'install' first.\n",
                             VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    int rc = fsdisk_sync();
    if (rc == FSDISK_ERR_TOO_SMALL || rc == FSDISK_ERR_FULL) {
        terminal_write_color(rc == FSDISK_ERR_FULL
                             ? "sync: failed - the files no longer fit in the disk's save area (delete some).\n"
                             : "sync: failed - the files no longer fit on the disk (delete some, or use a bigger disk).\n",
                             VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    if (rc != 0) {
        terminal_write_color("sync: failed (disk I/O error).\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    uint64_t used, cap;
    fsdisk_space(&used, &cap);
    char line[96];
    ksnprintf(line, sizeof(line), "sync: filesystem written to disk (%u MB of %u MB).",
              (uint32_t)((used + 1048575u) >> 20), (uint32_t)(cap >> 20));
    terminal_writeln(line);
}

/* ── prompt ─────────────────────────────────────────────────────── */
static void print_prompt(int persona) {
    char cwd_buf[FS_PATH_LEN];
    fs_cwd_path(cwd_buf, sizeof(cwd_buf));

    if (persona == SHELL_KIND_BASH) {
        /* real bash's default PS1: whole user@host in one bright-green
         * block, rather than sh's two-tone yellow/green split below. */
        terminal_write_color("banana@", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        terminal_write_color(hostname_get(), VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    } else {
        terminal_write_color("banana",         VGA_COLOR_YELLOW,      VGA_COLOR_BLACK);
        terminal_write_color("@", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        terminal_write_color(hostname_get(), VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }
    terminal_write_color(":",     VGA_COLOR_WHITE,      VGA_COLOR_BLACK);
    terminal_write_color(cwd_buf, VGA_COLOR_LIGHT_BLUE,  VGA_COLOR_BLACK);
    terminal_write_color("$ ",    VGA_COLOR_WHITE,       VGA_COLOR_BLACK);
}

/* ── line editor painting ───────────────────────────────────────── */

/* where the first n bytes of the input text end, as a linear cell offset
 * from start: a wide character (an emoji) is two cells, and one that would
 * start in a row's last cell goes to the next row (kernel/terminal.c) */
static int line_pos(int start, const char* buf, int n) {
    int w = (int)terminal_get_width(), pos = start;
    for (int i = 0; i < n; ) {
        int cw = u8_wcwidth(u8_decode(buf, n, &i));
        if (cw == 2 && w > 1 && pos % w == w - 1) pos++;
        pos += cw;
    }
    return pos;
}

/* the cursor after the first n bytes of the input text */
static void line_place_cursor(int start, const char* buf, int n) {
    int w = (int)terminal_get_width();
    int pos = line_pos(start, buf, n);
    terminal_serial_mirror(0);
    terminal_set_cursor((size_t)(pos / w), (size_t)(pos % w));
    terminal_serial_mirror(1);
}

/* Rewrites the input text from `*start`, blanking what a longer line
 * left behind (prev_len: the cells it took). Writing past the last row
 * scrolls the screen, so the start offset moves up by as many rows as
 * the cursor ended up short of where the text ends. */
static void line_repaint(int* start, const char* buf, int len, int prev_len, int cur) {
    int w = (int)terminal_get_width();
    int cells = line_pos(*start, buf, len) - *start;
    int total = (prev_len > cells) ? prev_len : cells;
    terminal_serial_mirror(0);
    terminal_set_cursor((size_t)(*start / w), (size_t)(*start % w));
    for (int i = 0; i < len; i++) terminal_putchar(buf[i]);
    for (int i = cells; i < total; i++) terminal_putchar(' ');
    size_t r, c;
    terminal_get_cursor(&r, &c);
    *start -= *start + total - (int)(r * (size_t)w + c);
    if (*start < 0) *start = 0;
    terminal_serial_mirror(1);
    line_place_cursor(*start, buf, cur);
}

/* The serial console gets a plain ANSI repaint of the line instead. */
/* An SSH session gets the same repaint through its own stream. */
static void echo_out(int tt, const char* s) {
    if (tt >= 0) tty_write(tt, s);
    else serial_write(s);
}

static void line_serial_echo(const char* buf, int len, int cur, int persona) {
    int tt = tty_current();
    if (tt < 0 && terminal_vt_get_active() != 0) return;
    char cwd_buf[FS_PATH_LEN];
    fs_cwd_path(cwd_buf, sizeof(cwd_buf));
    (void)persona;
    echo_out(tt, "\r" SHELL_USER "@");
    echo_out(tt, hostname_get());
    echo_out(tt, ":");
    echo_out(tt, cwd_buf);
    echo_out(tt, "$ ");
    {
        char line[SH_LINE_MAX];
        int n = len < SH_LINE_MAX - 1 ? len : SH_LINE_MAX - 1;
        memcpy(line, buf, (uint32_t)n);
        line[n] = 0;
        echo_out(tt, line);
    }
    echo_out(tt, "\x1b[K");
    if (len > cur) {
        char seq[16];
        ksnprintf(seq, sizeof(seq), "\x1b[%dD", u8_width(buf + cur, len - cur));
        echo_out(tt, seq);
    }
}

static void shell_readline(char* buf, int maxlen, int persona) {
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

    /* where the editable text begins (just after the prompt), as a
     * linear cell offset - lines longer than the screen wrap */
    int start;
    {
        size_t r, cc;
        terminal_get_cursor(&r, &cc);
        start = (int)(r * terminal_get_width() + cc);
    }

    while (1) {
        poll_deferred_actions();
        daemon_poll(len == 0);
        gui_poll();
        terminal_vt_set_active(my_vt);

        if (gui_focused_vt() != my_vt) {
            /* Another window (or none) has keyboard focus: keep this
             * shell's own command running/idling but don't steal input
             * meant for whoever the user is actually looking at. */
            task_sleep_ms(10);
            continue;
        }

        char c = keyboard_try_getchar();
        if (!c) { task_sleep_ms(10); continue; }

        if (c == 27) {
            /* Escape, or the lead byte of an arrow-key sequence
             * (ESC [ A/B/C/D). This has to be resolved before
             * gui_handle_key() below: it treats every bare ESC as
             * "close the open Start menu", which would otherwise
             * swallow the lead byte of every arrow press before the
             * arrow itself ever reaches gui_handle_arrow() - making
             * arrow-key menu navigation unreachable whenever the menu
             * is open. */
            /* an arrow key queues its three bytes at once: a lone ESC
             * (the Escape key) has nothing after it - never wait for more */
            char c2 = esc_follow(); /* '[' */
            if (c2 != '[') {
                gui_handle_key(c);
                if (c2) keyboard_unget(c2);
                continue;
            }
            char c3 = esc_follow(); /* A/B/C/D */
            terminal_vt_set_active(my_vt);   /* keyboard_getchar() may yield */

            if (gui_handle_arrow(c3)) continue;

            if (c3 == 'D' || c3 == 'C' || c3 == 'H' || c3 == 'F') {
                if (c3 == 'D' && cur > 0) cur = u8_prev(buf, cur);
                else if (c3 == 'C' && cur < len) cur = u8_next(buf, len, cur);
                else if (c3 == 'H' && cur > 0) cur = 0;            /* Home */
                else if (c3 == 'F' && cur < len) cur = len;        /* End */
                else continue;
                line_place_cursor(start, buf, cur);
                line_serial_echo(buf, len, cur, persona);
                continue;
            }
            if (c3 == 'P') {                                  /* Delete */
                if (cur >= len) continue;
                int d = u8_next(buf, len, cur) - cur;
                for (int i = cur; i + d <= len; i++) buf[i] = buf[i + d];
                len -= d;
                buf[len] = 0;
                line_repaint(&start, buf, len, prev_len, cur);
                line_serial_echo(buf, len, cur, persona);
                prev_len = line_pos(start, buf, len) - start;
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
        } else if (c == 22) { /* Ctrl+V: paste the clipboard (as one line: it never runs anything by itself) */
            uint32_t n;
            const char* clip = clipboard_get(&n);
            for (uint32_t k = 0; k < n && len < maxlen - 1; k++) {
                char ch = clip[k];
                if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                if ((unsigned char)ch < 32) continue;
                for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
                buf[cur++] = ch;
                len++;
            }
        } else if (c == 3) { /* Ctrl+C: cancel line, do not execute history entry */
            buf[0] = '\0';
            line_place_cursor(start, buf, len);
            terminal_write("^C\n");
            return;
        } else if (c == '\n') {
            buf[len] = '\0'; /* ensure empty Enter stays empty */
            line_place_cursor(start, buf, len);   /* newline after the whole (wrapped) line */
            terminal_putchar('\n');
            break;
        } else if (c == '\b') {
            if (cur > 0) {
                int pv = u8_prev(buf, cur), d = cur - pv;
                for (int i = pv; i + d <= len; i++) buf[i] = buf[i + d];
                cur = pv;
                len -= d;
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

        /* part of a character like é or €: repaint once all of it is in */
        if ((unsigned char)c >= 0x80 && cur > 0) {
            int p0 = u8_prev(buf, cur);
            unsigned char lb = (unsigned char)buf[p0];
            int need = lb >= 0xF0 ? 4 : lb >= 0xE0 ? 3 : lb >= 0xC0 ? 2 : 1;
            if (cur - p0 < need) continue;
        }

        /* Repaint the editable text in place (it may span several rows) */
        line_repaint(&start, buf, len, prev_len, cur);
        line_serial_echo(buf, len, cur, persona);
        prev_len = line_pos(start, buf, len) - start;
    }

    /* "!!" (bash/csh-style history-bang) has to resolve here, against
     * whatever is still the *previous* history entry, and before the
     * append below - otherwise it would see itself ("!!") as "the last
     * command" instead of the command it's supposed to repeat. Real bash
     * likewise records the expanded command in history, not the literal
     * "!!", so typing it twice in a row repeats the original both times
     * rather than degenerating after the first expansion. */
    if (k_strcmp(buf, "!!") == 0) {
        if (sh_hist_count > 0) {
            k_strcpy_n(buf, sh_history[sh_hist_count - 1], maxlen);
            len = k_strlen(buf);
            terminal_writeln(buf);
        } else {
            buf[0] = '\0';
            len = 0;
        }
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

/* The desktop is the machine's own screen: an SSH session does not start or
 * stop it (that went wrong - the remote shell and the desktop then fought
 * over the screen and the keyboard), and a terminal window does not close
 * the desktop it is part of (Start > Shut down > Exit to the shell does). */
static void cmd_startx(void) {
    if (tty_current() >= 0) {
        terminal_writeln("startx: not over SSH - the desktop belongs to the computer's own screen");
        return;
    }
    if (terminal_vt_get_active() != 0) {            /* a terminal window of the desktop */
        terminal_writeln("startx: the desktop is already running (this is one of its windows)");
        return;
    }
    gui_set_enabled(1);
    terminal_writeln("startx: GUI enabled (Ctrl+T or click [Start]). Type 'stopx' to return to shell-only view.");
}

static void cmd_stopx(void) {
    if (tty_current() >= 0) {
        terminal_writeln("stopx: not over SSH - the desktop belongs to the computer's own screen");
        return;
    }
    if (terminal_vt_get_active() != 0) {            /* a terminal window of the desktop */
        terminal_writeln("stopx: not from a desktop window - use Start > Shut down > Exit to the shell");
        return;
    }
    gui_set_enabled(0);
    terminal_writeln("stopx: GUI disabled.");
}

/* ── dispatch ───────────────────────────────────────────────────── */
static void dispatch_cmd(const char* raw_line, int persona);
static void dispatch_plain(const char* line, int persona);

/* `cmd > file` / `cmd >> file`: runs cmd with its output captured
 * (kernel/terminal.c) and writes that to the file instead of the screen. */
static void dispatch(const char* raw_line, int persona) {
    /* `a; b` and `a && b` run one after the other */
    {
        char q = 0;
        for (int i = 0; raw_line[i]; i++) {
            char c = raw_line[i];
            if (q) { if (c == q) q = 0; continue; }
            if (c == 0x22 || c == 0x27) { q = c; continue; }
            int amp = (c == '&' && raw_line[i + 1] == '&');
            if (c != ';' && !amp) continue;
            char first[SH_LINE_MAX];
            int n = i < SH_LINE_MAX - 1 ? i : SH_LINE_MAX - 1;
            memcpy(first, raw_line, (uint32_t)n);
            while (n > 0 && (first[n - 1] == ' ' || first[n - 1] == '\t')) n--;
            first[n] = 0;
            if (*k_skip_spaces(first)) dispatch(first, persona);
            const char* rest = k_skip_spaces(raw_line + i + (amp ? 2 : 1));
            if (*rest) dispatch(rest, persona);
            return;
        }
    }

    int pos = -1;
    char q = 0;
    for (int i = 0; raw_line[i]; i++) {
        char c = raw_line[i];
        if (q) { if (c == q) q = 0; }
        else if (c == 0x22 || c == 0x27) q = c;
        else if (c == 0x3E) { pos = i; break; }
    }
    if (pos < 0) { dispatch_cmd(raw_line, persona); return; }

    int append = raw_line[pos + 1] == 0x3E;
    char cmd[SH_LINE_MAX], path[FS_PATH_LEN];
    int n = pos < SH_LINE_MAX - 1 ? pos : SH_LINE_MAX - 1;
    memcpy(cmd, raw_line, (uint32_t)n);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t')) n--;
    cmd[n] = 0;
    const char* t = k_skip_spaces(raw_line + pos + (append ? 2 : 1));
    path[0] = 0;
    next_token(&t, path, sizeof(path));          /* "quoted name" too */
    if (!path[0]) {
        terminal_write_color("sh: syntax error: file name expected after >\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    if (!terminal_capture_start()) {
        terminal_write_color("sh: another redirection is running, try again\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        return;
    }
    dispatch_cmd(cmd, persona);
    uint32_t len;
    char* out = terminal_capture_stop(&len);
    int idx = append ? fs_find_file(path) : -1;
    int ok = idx >= 0 ? fs_append(idx, out, len) == 0 : fs_write_path(path, out, len) >= 0;
    kfree(out);
    if (!ok) {
        terminal_write_color("sh: cannot write ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_write_color(path, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_putchar(0x0A);
    }
}

/* Commands that take one path as the rest of the line: `cd "My files"`
 * works like in a real shell (the quotes only group the words). */
static const char* unquote_path_cmd(const char* line, char* out, int cap) {
    static const char* const cmds[] = { "cd", "edit", "cat", "run" };
    const char* s = k_skip_spaces(line);
    int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    int match = 0;
    for (uint32_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if ((int)strlen(cmds[i]) == wl && strncmp(s, cmds[i], (size_t)wl) == 0) match = 1;
    if (!match || (!strchr(s, 0x22) && !strchr(s, 0x27))) return line;
    int n = 0;
    for (; *s && n < cap - 1; s++)
        if (*s != 0x22 && *s != 0x27) out[n++] = *s;
    out[n] = '\0';
    return out;
}

static void dispatch_cmd(const char* raw_line, int persona) {
    char unquoted[SH_LINE_MAX];
    raw_line = unquote_path_cmd(raw_line, unquoted, sizeof(unquoted));
    /* "!!" / "$VAR" expansion, then a single non-recursive alias
     * substitution of the leading word - same order a real shell applies
     * these in, and shared by both personas (and by `run`'s scripts). */
    char expanded[SH_LINE_MAX];
    expand_line(raw_line, expanded, sizeof(expanded), persona);

    char final_buf[SH_LINE_MAX];
    {
        const char* s = k_skip_spaces(expanded);
        char first[ALIAS_NAME_LEN];
        const char* rest_p = s;
        if (next_token(&rest_p, first, sizeof(first))) {
            const char* av = alias_lookup(first);
            if (av) {
                int n = 0;
                for (int i = 0; av[i] && n < (int)sizeof(final_buf) - 1; i++) final_buf[n++] = av[i];
                const char* rest = k_skip_spaces(rest_p);
                if (*rest && n < (int)sizeof(final_buf) - 1) final_buf[n++] = ' ';
                for (int i = 0; rest[i] && n < (int)sizeof(final_buf) - 1; i++) final_buf[n++] = rest[i];
                final_buf[n] = '\0';
            } else {
                k_strcpy_n(final_buf, s, sizeof(final_buf));
            }
        } else {
            final_buf[0] = '\0';
        }
    }

    dispatch_plain(k_skip_spaces(final_buf), persona);
}

/* kernel/kcmd.c: a command line as a program gives it (bash has expanded
 * it already: no $, !! or aliases here) - 0, or 127 if no such command */
static int g_no_path;                           /* (dispatch_plain: no $PATH search - it would find the caller) */
static int g_not_found;
static int g_command_status;
int shell_exec_line(const char* line) {
    char buf[SH_LINE_MAX];
    char unquoted[SH_LINE_MAX];
    k_strcpy_n(buf, line, sizeof(buf));
    const char* l = unquote_path_cmd(buf, unquoted, sizeof(unquoted));
    int was = g_no_path;
    g_no_path = 1;
    g_not_found = 0;
    g_command_status = 0;
    dispatch_plain(k_skip_spaces(l), g_default_shell_kind);
    g_no_path = was;
    return g_not_found ? 127 : g_command_status;
}

static void dispatch_plain(const char* line, int persona) {
    if (!*line) return;

    /* install / update / installer: not there on an installed system */
    {
        char first[16];
        int n = 0;
        while (line[n] && line[n] != ' ' && n < 15) { first[n] = line[n]; n++; }
        first[n] = 0;
        if (cmd_hidden(first)) {
            terminal_write_color(shell_kind_name(persona), VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_write_color(": command not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(first);
            terminal_write_color("(installing and updating Banana OS are done from the live CD or USB stick)\n",
                                 VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            return;
        }
    }
    if (k_strcmp(line, "autologin") == 0 || k_strncmp(line, "autologin ", 10) == 0) {
        cmd_autologin(k_skip_spaces(line + 9));
        return;
    }

    /* <command> --help: its page (shell/help.c) */
    {
        int l = k_strlen(line);
        if (l > 7 && k_strcmp(line + l - 7, " --help") == 0) {
            char first[32];
            int n = 0;
            while (line[n] && line[n] != ' ' && n < (int)sizeof(first) - 1) { first[n] = line[n]; n++; }
            first[n] = 0;
            if (help_show_page(first)) return;
        }
    }

    /* exact matches */
    if (k_strcmp(line, "help")     == 0) { cmd_help("");     return; }
    if (k_strncmp(line, "help ", 5) == 0) { cmd_help(line + 5); return; }
    if (k_strcmp(line, "man") == 0 || k_strncmp(line, "man ", 4) == 0) { help_man(line + 3); return; }
    if (k_strcmp(line, "apropos") == 0 || k_strncmp(line, "apropos ", 8) == 0) { help_apropos(line + 7); return; }
    if (k_strcmp(line, "clear")    == 0) { terminal_clear(); return; }
    if (k_strcmp(line, "neofetch") == 0) { cmd_neofetch(persona); return; }
    if (k_strcmp(line, "sysinfo") == 0) { cmd_sysinfo(); return; }
    if (k_strcmp(line, "uname")    == 0) { cmd_uname(line + 5, persona);    return; }
    if (k_strncmp(line, "uname ", 6) == 0) { cmd_uname(line + 5, persona); return; }   /* (uname -a ...: the same) */
    if (k_strcmp(line, "halt")     == 0) { cmd_halt();       return; }
    if (k_strcmp(line, "ls")       == 0) { cmd_ls("");       return; }
    if (k_strcmp(line, "pwd")      == 0) { fs_pwd();         return; }
    if (k_strcmp(line, "whoami")   == 0) { cmd_whoami();     return; }
    if (!strcmp(line, "hostname") || !strncmp(line, "hostname ", 9)) { g_command_status = cmd_hostname(line + 8); return; }
    if (k_strcmp(line, "history")  == 0) { cmd_history();    return; }
    if (k_strcmp(line, "find")     == 0) { cmd_find("");     return; }
    if (k_strcmp(line, "uptime")   == 0) { cmd_uptime();     return; }
    if (k_strcmp(line, "boottime") == 0) { cmd_boottime();   return; }
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
    if (k_strcmp(line, "alias")    == 0) { cmd_alias("");         return; }
    if (k_strcmp(line, "unalias")  == 0) {
        terminal_write_color("Usage: unalias <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "export")   == 0) { cmd_export("", persona); return; }
    if (k_strcmp(line, "unset")    == 0) {
        terminal_write_color("Usage: unset <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "env")      == 0) { print_env(persona);    return; }
    if (k_strcmp(line, "type")     == 0) {
        terminal_write_color("Usage: type <name>\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        return;
    }
    if (k_strcmp(line, "chsh")     == 0) { cmd_chsh("", persona); return; }

    if (k_strcmp(line, "disks")    == 0) { cmd_disks();       return; }
    if (k_strcmp(line, "install")  == 0 || k_strncmp(line, "install ", 8) == 0) { cmd_install(line + 7); return; }
    if (k_strcmp(line, "sync")     == 0) { cmd_sync();        return; }
    if (k_strcmp(line, "update")   == 0 || k_strncmp(line, "update ", 7) == 0) { cmd_update(line + 6); return; }
    if (k_strcmp(line, "repair")   == 0 || k_strncmp(line, "repair ", 7) == 0) { cmd_update_mode(line + 6, 1); return; }
    if (k_strcmp(line, "nice") == 0 || k_strncmp(line, "nice ", 5) == 0) {
        /* nice [-n N | -N] command: the command runs with its nice value
         * raised by N (default 10: a smaller share of the processor;
         * negative: more) */
        int pid = task_current_pid(), old = task_get_nice(pid), n = 10;
        const char* p = k_skip_spaces(line + 4);
        if (!*p) { char b[32]; ksnprintf(b, sizeof(b), "%d\n", old); terminal_write(b); return; }
        int have = 0;
        if (p[0] == '-' && p[1] == 'n' && (p[2] == ' ' || !p[2])) { p = k_skip_spaces(p + 2); have = 1; }
        else if (p[0] == '-' && ((p[1] >= '0' && p[1] <= '9') || p[1] == '-')) { p++; have = 1; }
        if (have) {
            int neg = 0;
            if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
            if (*p < '0' || *p > '9') { terminal_write_color("usage: nice [-n N] command\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK); return; }
            n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            if (neg) n = -n;
            p = k_skip_spaces(p);
        }
        if (!*p) { terminal_write_color("usage: nice [-n N] command\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK); return; }
        task_set_nice(pid, old + n);        /* (like Linux: added to the current value) */
        dispatch(p, persona);
        task_set_nice(pid, old);
        return;
    }
    if (k_strncmp(line, "time ", 5) == 0) {
        /* bash-style `time`: wall-clock duration of one command */
        uint32_t t0 = timer_ms();
        dispatch(k_skip_spaces(line + 5), persona);
        uint32_t ms = timer_ms() - t0;
        char tb[48];
        ksnprintf(tb, sizeof(tb), "\nreal    %um%u.%03us\n", ms / 60000u, (ms / 1000u) % 60u, ms % 1000u);
        terminal_write(tb);
        return;
    }

    /* shutdown / reboot with optional arg */
    if (k_strcmp(line,"sleep")==0) { cmd_sleep("");return; }
    if (k_strncmp(line,"sleep ",6)==0) { cmd_sleep(k_skip_spaces(line+6));return; }
    if (k_strcmp(line, "shutdown")       == 0) { cmd_shutdown("");    return; }
    if (k_strncmp(line, "shutdown ", 9)  == 0) { cmd_shutdown(k_skip_spaces(line+9)); return; }
    if (k_strcmp(line, "reboot")         == 0) { cmd_reboot("");      return; }
    if (k_strncmp(line, "reboot ", 7)    == 0) { cmd_reboot(k_skip_spaces(line+7));   return; }

    /* commands with arguments */
    if (k_strncmp(line, "echo ",  5) == 0) {
        /* quotes group words, like a real shell: they are not printed */
        const char* s = k_skip_spaces(line + 5);
        char q = 0;
        for (; *s; s++) {
            if (q) { if (*s == q) { q = 0; continue; } }
            else if (*s == 0x22 || *s == 0x27) { q = *s; continue; }
            terminal_putchar(*s);
        }
        terminal_putchar(0x0A);
        return;
    }
    if (k_strncmp(line, "cd ",    3) == 0) { fs_cd(k_skip_spaces(line+3));            return; }
    if (k_strncmp(line, "ls ",    3) == 0) { cmd_ls(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "mkdir ", 6) == 0) { cmd_mkdir(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "rm ",   3) == 0) { cmd_rm(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "touch ", 6) == 0) { cmd_touch(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "cp ",   3) == 0) { cmd_cp(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "mv ",   3) == 0) { cmd_mv(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "edit ", 5) == 0 && tty_current() >= 0) {
        terminal_writeln("edit: the full-screen editor only runs on the local screen - over SSH use");
        terminal_writeln("      echo \"text\" > file, cat, cp, or open the file in a GUI terminal");
        return;
    }
    if (k_strncmp(line, "edit ", 5) == 0) { editor_open(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "cat ",  4) == 0) { cmd_cat(k_skip_spaces(line+4)); return; }
    if (k_strncmp(line, "run ",  4) == 0) { cmd_run(k_skip_spaces(line+4), persona); return; }
    if (k_strncmp(line, "grep ", 5) == 0) { cmd_grep(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "wc ",   3) == 0) { cmd_wc(k_skip_spaces(line+3)); return; }
    if (k_strncmp(line, "head ", 5) == 0) { cmd_head(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "tail ", 5) == 0) { cmd_tail(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "find ", 5) == 0) { cmd_find(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "which ", 6) == 0) { cmd_which(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "alias ",   6) == 0) { cmd_alias(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "unalias ", 8) == 0) { cmd_unalias(k_skip_spaces(line+8)); return; }
    if (k_strncmp(line, "export ",  7) == 0) { cmd_export(k_skip_spaces(line+7), persona); return; }
    if (k_strncmp(line, "unset ",   6) == 0) { cmd_unset(k_skip_spaces(line+6)); return; }
    if (k_strncmp(line, "type ",    5) == 0) { cmd_type(k_skip_spaces(line+5)); return; }
    if (k_strncmp(line, "chsh ",    5) == 0) { cmd_chsh(k_skip_spaces(line+5), persona); return; }
    if (k_strncmp(line, "keyboardctl ", 12) == 0) { cmd_keyboardctl(k_skip_spaces(line+12)); return; }
    if (k_strncmp(line, "loadctl ", 8) == 0) { cmd_keyboardctl(k_skip_spaces(line+8)); return; }
    if (k_strncmp(line, "proc_info ", 10) == 0) { cmd_proc_info(k_skip_spaces(line+10)); return; }
    if (k_strncmp(line, "ram_info ", 9) == 0) { cmd_ram_info(k_skip_spaces(line+9)); return; }
    if (k_strncmp(line, "gpu_info ", 9) == 0) { cmd_gpu_info(k_skip_spaces(line+9)); return; }
    if (k_strcmp(line, "gpubench") == 0) { gpu_bench(); return; }
    if (k_strcmp(line, "lsgpu") == 0) { gpu_list(); return; }
    if (k_strcmp(line, "drivers") == 0) { drivers_list(); return; }
    if (k_strncmp(line, "driver load ", 12) == 0) {
        const char* p = k_skip_spaces(line + 12);
        const char* b = strrchr(p, '/');
        char name[32], err[96];
        kstrlcpy(name, b ? b + 1 : p, sizeof(name));
        if (driver_load(p, name, err, sizeof(err)) < 0) { terminal_write("driver: "); terminal_writeln(err); }
        else terminal_writeln("driver: loaded (see: drivers)");
        return;
    }
    if (k_strcmp(line, "resolution") == 0) { cmd_resolution(""); return; }
    if (k_strncmp(line, "resolution ", 11) == 0) { cmd_resolution(line + 11); return; }
    if (k_strcmp(line, "laptop") == 0) { cmd_laptop(""); return; }
    if (k_strcmp(line, "usbc") == 0) { cmd_usbc(""); return; }
    if (k_strncmp(line, "usbc ", 5) == 0) { cmd_usbc(line + 5); return; }
    if (k_strcmp(line, "brightness") == 0) { cmd_brightness(""); return; }
    if (k_strncmp(line, "brightness ", 11) == 0) { cmd_brightness(line + 11); return; }
    if (k_strncmp(line, "hw_info ", 8) == 0) { cmd_hw_info(k_skip_spaces(line+8)); return; }

    /* network commands (shell/netcmds.c) */
    if (syscmd_dispatch(line)) return;
    if (moncmd_dispatch(line)) return;
    if (fscmd_dispatch(line)) return;
    if (k_strcmp(line, "vmtools") == 0 || k_strncmp(line, "vmtools ", 8) == 0) { vmtools_command(line + 7); return; }
    if (srvcmd_dispatch(line)) return;
    if (netcmd_dispatch(line)) return;

    /* a program, a script or an installed app found through $PATH */
    if (!g_no_path && path_run(line, persona)) return;

    /* unknown */
    g_not_found = 1;
    terminal_write_color(shell_kind_name(persona), VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(": command not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(line);
    help_suggest(line);
}

static void run_script_text(const char* content, int persona) {
    char linebuf[SH_LINE_MAX];
    int pos = 0;
    for (int i = 0;; i++) {
        char ch = content[i];
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\0') {
            linebuf[pos] = '\0';
            const char* cmd = k_skip_spaces(linebuf);
            if (*cmd && *cmd != '#') dispatch(cmd, persona);
            pos = 0;
            if (ch == '\0') break;
            continue;
        }
        if (pos < (int)sizeof(linebuf) - 1) linebuf[pos++] = ch;
    }
}

static void print_banner(int persona) {
    terminal_write_color(
        "  ____                                   ___  ____  \n"
        " | __ )  __ _ _ __   __ _ _ __   __ _   / _ \\/ ___| \n"
        " |  _ \\ / _` | '_ \\ / _` | '_ \\ / _` | | | | \\___ \\ \n"
        " | |_) | (_| | | | | (_| | | | | (_| | | |_| |___) |\n"
        " |____/ \\__,_|_| |_|\\__,_|_| |_|\\__,_|  \\___/|____/ \n",
        VGA_COLOR_YELLOW, VGA_COLOR_BLACK);

    terminal_writeln("");
    terminal_write_color("  Welcome to Banana OS 0.6  --  ",
                         VGA_COLOR_WHITE, VGA_COLOR_BLACK);
#ifdef __x86_64__
    (void)persona;                      /* (bash on the consoles: kernel/vcons.c) */
    terminal_writeln("GNU bash. Type 'help' to get started, Alt+F2..F6 for more consoles.");
#else
    if (persona == SHELL_KIND_BASH)
        terminal_writeln("bash-compatible shell. Type 'help' to get started.");
    else
        terminal_writeln("type 'help' to get started.");
#endif
    terminal_writeln("");
}

static void print_banner_window(int persona) {
    terminal_write_color("  Welcome to Banana OS 0.6  --  ",
                         VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    if (persona == SHELL_KIND_BASH)
        terminal_writeln("bash-compatible shell. Type 'help' to get started.");
    else
        terminal_writeln("type 'help' to get started.");
    terminal_writeln("");
}

/* /etc/init.sh: shell commands run once at boot, before the first prompt
 * (after the network, the settings and the services). A fresh system gets
 * a commented example. */
#define INIT_FILE "/etc/init.sh"
static const char INIT_DEFAULT[] =
    "# /etc/init.sh - commands Banana OS runs once at boot, before the first prompt.\n"
    "# One shell command per line; lines starting with '#' are ignored.\n"
    "# Edit it with:  edit /etc/init.sh   (or Notepad), then reboot.\n"
    "#\n"
    "# Examples (remove the '#' to use one):\n"
    "#   startx                 start the desktop\n"
    "#   keyboardctl fr_CH      keyboard layout\n"
    "#   cd /home/banana\n"
    "#   echo Welcome back!\n";

static void run_init_file(int persona) {
    int idx = fs_find_file(INIT_FILE);
    if (idx < 0) {
        fs_mkdir_p("/etc");
        fs_write_path(INIT_FILE, INIT_DEFAULT, (uint32_t)(sizeof(INIT_DEFAULT) - 1));
        return;
    }
    fs_file_t* f = fs_get_file(idx);
    fs_pin(idx);
    if (f && f->content) run_script_text(f->content, persona);
    fs_unpin(idx);
}

/* ── the consoles (64-bit): bash on tty1..6 and the serial line
 * (kernel/vcons.c) - this task drives them, and the desktop: the keys go
 * to the console shown, or to the desktop while it is up ── */
static int console_esc_key(char c3) {
    switch (c3) {
    case 'A': return VT_KEY_UP;    case 'B': return VT_KEY_DOWN;
    case 'C': return VT_KEY_RIGHT; case 'D': return VT_KEY_LEFT;
    case 'H': return VT_KEY_HOME;  case 'F': return VT_KEY_END;
    case 'I': return VT_KEY_PGUP;  case 'G': return VT_KEY_PGDN;
    case 'P': return VT_KEY_DELETE;
    }
    return 0;
}

static void console_loop(void) {
    for (;;) {
        poll_deferred_actions();
        int busy = vcons_pump();
        daemon_poll(1);
        gui_poll();                 /* (yields once: the other tasks run) */
        terminal_vt_set_active(0);

        if (gui_is_enabled()) {
            /* the desktop has the keys (the way the console shell gave them to it) */
            if (gui_focused_vt() != 0) { task_sleep_ms(10); continue; }
            char c = keyboard_try_getchar();
            if (!c) { if (!busy) task_sleep_ms(10); continue; }
            if (c == 27) {
                char c2 = esc_follow();
                if (c2 != '[') { gui_handle_key(c); if (c2) keyboard_unget(c2); continue; }
                gui_handle_arrow(esc_follow());
                continue;
            }
            gui_handle_key(c);
            continue;
        }

        int k;
        while ((k = keyboard_take_fkey())) vcons_fkey(k);
        char c = keyboard_try_getchar();
        if (!c) { if (!busy) task_sleep_ms(5); continue; }
        int mods = keyboard_mods() & 1 ? VT_MOD_SHIFT : 0;
        if (c == 27) {
            char c2 = esc_follow();
            if (c2 == '[') {
                int key = console_esc_key(esc_follow());
                if (key) vcons_key(key, mods);
                continue;
            }
            vcons_key(27, 0);
            if (c2) keyboard_unget(c2);
            continue;
        }
        if (c == '\n') c = '\r';                    /* (Enter: a terminal's CR) */
        else if (c == '\b') c = 0x7F;                /* (Backspace: DEL) */
        vcons_key((unsigned char)c, 0);
    }
}

/* ── entry ──────────────────────────────────────────────────────── */
void shell_run(void) {
    char buf[SH_LINE_MAX];
    help_set_known(known_cmds);         /* (help --check, did you mean) */
    /* Which persona a *new* shell instance boots into is decided once,
     * here, from g_default_shell_kind (as set by `chsh`) - see the
     * "shell personas" comment near the top of this file. */
    int persona = g_default_shell_kind;

    if (!strstr(sysinfo_get()->cmdline, "verbose")) terminal_clear();   /* (boot messages: the steps stay) */
    print_banner(persona);

    /* If a dedicated ATA disk was previously `install`ed, load its saved
     * filesystem instead of reseeding the defaults - this is what makes
     * files persist across reboots. See kernel/fsdisk.c. */
    /* Started from the CD: the live system, fresh - an installed disk is
     * left alone (no login, no autosave onto it); `update` can update it. */
    int live = sysinfo_live_boot();
    boottime_mark("disks (an installed Banana OS?)");
    int loaded = live ? 0 : fsdisk_try_load();
    if (loaded <= 0) {
        fs_init();
        examples_seed();          /* the SDK example apps in ~/Examples */
    }
    if (loaded < 0)
        terminal_write_color(
            "The installed disk's files could not be read (damaged). Starting with a fresh\n"
            "filesystem; the disk is not written to unless you run 'install' again.\n\n",
            VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    hostname_boot();            /* the saved PC name, before apps and services start */
    fsdisk_start_autosave();      /* changes reach the installed disk by themselves */
    zswap_start();                /* memory low: apps' idle pages compressed (kernel/zswap.c) */
    if (fsdisk_is_installed()) zswap_disk_init();   /* and a swap area on the disk for what does not fit */
    disk_attach_extra(fsdisk_holds_banana);   /* the other hard disks: their volumes mounted in /mnt */
    if (live)
        terminal_write_color("Running the LIVE CD.\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    else if (fsdisk_is_installed())
        terminal_write_color("Running the installed system (from the disk).\n\n", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    if (live && !fsdisk_find_install(NULL))
        terminal_writeln("Changes are lost at shutdown - to keep them, open Install Banana OS on the desktop (or type install).\n");
    if (live && fsdisk_find_install(NULL))
        terminal_write_color(
            "Live CD - Banana OS is also installed on a disk here (its files are not used now).\n"
            "Type 'update' to update that installation with this CD (files kept), or boot from the disk.\n\n",
            VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

    boottime_mark("built-in apps");
    builtin_apps_install();         /* Media Player, Music (kernel/builtin_apps.c) */
    builtin_release();              /* installed: the packages on the disk, their RAM to the heap */
    boottime_mark("driver modules");
    drivers_load_installed();       /* (installed with pkg: driver-sdk) */

    boottime_mark("settings, servers");
    /* saved network settings, services enabled at boot (shell/srvcmds.c) */
    services_boot();
    vmtools_init();                 /* guest tools: VMware, VirtualBox, the QEMU / Proxmox agent (kernel/vmtools.c) */
    {
        char sk[8];
        if (settings_get("shell", sk, sizeof(sk)))
            g_default_shell_kind = persona = (k_strcmp(sk, "bash") == 0) ? SHELL_KIND_BASH : SHELL_KIND_SH;
    }

    boottime_mark("ready");
    splash_end();                 /* ready: the boot screen gives way to the console */
    login_screen();               /* the password, when one is set */
    boottime_mark("logged in");
    if (services_desktop_at_boot()) gui_set_enabled(1);

    run_init_file(persona);

    if (vcons_start() == 0) console_loop();   /* (64-bit, with bash: never returns) */

    while (1) {
        buf[0] = '\0';
        print_prompt(persona);
        shell_readline(buf, sizeof(buf), persona);
        dispatch(buf, persona);
    }
}

void shell_run_window(int vt) {
    char buf[SH_LINE_MAX];
    int persona = g_default_shell_kind;

    terminal_vt_set_active(vt);
    terminal_clear();
    print_banner_window(persona);

    /* Note: fs_init() is intentionally not called here - the filesystem
     * is a single OS-wide resource shell_run() already set up once; every
     * window shares it (and its current directory), same as every other
     * process on a real OS sharing one mounted filesystem. */
    while (1) {
        buf[0] = '\0';
        print_prompt(persona);
        shell_readline(buf, sizeof(buf), persona);
        dispatch(buf, persona);
    }
}

#ifdef __x86_64__
/* an SSH session with the consoles' bash about: bash on a pty, its bytes
 * to and from the client (exec: bash -c the command); 0 if it cannot start */
static int remote_bash(int tty) {
    pty_t* t = pty_new();
    if (!t) return 0;
    pty_set_locked(t, 0);
    int cols, rows;
    tty_take_size(tty, &cols, &rows);
    app_pty_resize(t, cols, rows);
    const char* cmd = tty_exec_cmd(tty);
    char* argv[4] = { "-bash", NULL, NULL, NULL };
    int argc = 1;
    if (cmd[0]) { argv[0] = "bash"; argv[1] = "-c"; argv[2] = (char*)cmd; argc = 3; }
    char err[64];
    uint8_t* b = (uint8_t*)kmalloc(4096);
    int pid = b ? app_spawn_tty("/bin/bash", argc, argv, vcons_env(), t, err, sizeof(err)) : -1;
    if (pid < 0) { kfree(b); app_pty_close(t); return 0; }
    for (;;) {
        int busy = 0;
        int n = tty_read(tty, b, 4096);
        if (n < 0) break;                               /* (the client went: a hangup) */
        if (n > 0) { app_pty_type(t, b, (unsigned long)n); busy = 1; }
        if (tty_take_size(tty, &cols, &rows)) app_pty_resize(t, cols, rows);
        long m;
        while ((m = pty_master_read(t, b, 4096)) > 0) { tty_write_n(tty, b, (uint32_t)m); busy = 1; }
        if (!busy && !app_alive(pid)) {                 /* (it ended, and all it wrote went) */
            int code = app_exit_code(pid);
            tty_set_exit(tty, code < 0 ? 0 : code);
            break;
        }
        if (!busy) task_sleep_ms(5);
    }
    kfree(b);
    app_pty_close(t);
    return 1;
}
#endif

/* SSH sessions (net/sshd.c): one of these runs per remote terminal, for
 * the OS's lifetime, and serves one client after another. */
void shell_run_remote(int tty) {
    char buf[SH_LINE_MAX];
    tty_bind(tty);
    terminal_vt_set_active(tty_vt(tty));
    for (;;) {
        while (!tty_active(tty)) task_sleep_ms(50);
        terminal_vt_set_active(tty_vt(tty));
        int persona = g_default_shell_kind;
#ifdef __x86_64__
        if (vcons_running() && remote_bash(tty)) {
            terminal_flush();
            tty_finish(tty);
            continue;
        }
#endif

        const char* cmd = tty_exec_cmd(tty);
        if (cmd[0]) {                       /* ssh host <command> */
            k_strcpy_n(buf, cmd, sizeof(buf));
            dispatch(buf, persona);
            terminal_flush();
            tty_finish(tty);
            continue;
        }

        terminal_write_color("Banana OS 0.6", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        terminal_writeln(" - SSH session. Type 'help' for commands, 'exit' to log out.");
        terminal_writeln("");
        while (tty_active(tty)) {
            buf[0] = '\0';
            print_prompt(persona);
            shell_readline(buf, sizeof(buf), persona);
            if (!tty_active(tty)) break;
            dispatch(buf, persona);
        }
        tty_finish(tty);
    }
}
