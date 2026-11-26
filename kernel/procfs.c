/* SPDX-License-Identifier: GPL-3.0-or-later */
/* /proc as Linux has it, made at each read, and /sys/devices/system/cpu.
 *
 * Programs ported from Linux, and libraries in Java, ask the system the
 * Linux way: OSHI (Minecraft's F3 screen: "CPU: 8x Intel(R) Core(TM) ..."),
 * Java's OperatingSystemMXBean, top / htop / free / uptime / neofetch, ps.
 * /proc is made of text written when it is opened (kernel/app.c: a
 * descriptor of its own, KF_MEM), never a file of kernel/fs.c - only the
 * empty folder is, so that `cd /proc` and Files find it - and so never on
 * the disk. /sys/devices/system/cpu is real files, written at the start
 * (what it says does not change while the system runs). */
#include "procfs.h"
#include "fs.h"
#include "fsdisk.h"
#include "kstring.h"
#include "kheap.h"
#include "sysinfo.h"
#include "smp.h"
#include "timer.h"
#include "task.h"
#include "meminfo.h"
#include "hostname.h"
#include "rtc.h"
#include "io.h"
#include "../build/build_info.h"

#define USER_HZ 100                 /* (Linux's clock ticks in /proc: 1/100 s) */
#define PROC_MAX 128

static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

/* ── a growing text ── */
typedef struct { char* b; uint32_t n, cap; int oom; } sb_t;
static void sb_add(sb_t* s, const char* t, uint32_t len) {
    if (s->oom) return;
    if (s->n + len + 1 > s->cap) {
        uint32_t nc = s->cap ? s->cap : 512;
        while (s->n + len + 1 > nc) nc *= 2;
        char* nb = (char*)kmalloc(nc);
        if (!nb) { s->oom = 1; return; }
        if (s->b) { memcpy(nb, s->b, s->n); kfree(s->b); }
        s->b = nb;
        s->cap = nc;
    }
    memcpy(s->b + s->n, t, len);
    s->n += len;
    s->b[s->n] = 0;
}
static void sb_puts(sb_t* s, const char* t) { sb_add(s, t, (uint32_t)strlen(t)); }
#define sb_printf(s, ...) do { char _l[512]; int _k = ksnprintf(_l, sizeof(_l), __VA_ARGS__); \
                                sb_add((s), _l, (uint32_t)(_k < 0 ? 0 : _k >= (int)sizeof(_l) ? (int)sizeof(_l) - 1 : _k)); } while (0)

/* ── the processor ── */
typedef struct { uint8_t reg, bit; const char* name; } flag_t;
enum { L1D, L1C, L7B, L7C, L7D, X1D, X1C };
static const flag_t g_flags[] = {
    { L1D, 0, "fpu" }, { L1D, 1, "vme" }, { L1D, 2, "de" }, { L1D, 3, "pse" }, { L1D, 4, "tsc" }, { L1D, 5, "msr" },
    { L1D, 6, "pae" }, { L1D, 7, "mce" }, { L1D, 8, "cx8" }, { L1D, 9, "apic" }, { L1D, 11, "sep" }, { L1D, 12, "mtrr" },
    { L1D, 13, "pge" }, { L1D, 14, "mca" }, { L1D, 15, "cmov" }, { L1D, 16, "pat" }, { L1D, 17, "pse36" },
    { L1D, 19, "clflush" }, { L1D, 23, "mmx" }, { L1D, 24, "fxsr" }, { L1D, 25, "sse" }, { L1D, 26, "sse2" },
    { L1D, 27, "ss" }, { L1D, 28, "ht" },
    { X1D, 11, "syscall" }, { X1D, 20, "nx" }, { X1D, 22, "mmxext" }, { X1D, 26, "pdpe1gb" }, { X1D, 27, "rdtscp" },
    { X1D, 29, "lm" },
    { L1C, 0, "pni" }, { L1C, 1, "pclmulqdq" }, { L1C, 3, "monitor" }, { L1C, 5, "vmx" }, { L1C, 9, "ssse3" },
    { L1C, 12, "fma" }, { L1C, 13, "cx16" }, { L1C, 19, "sse4_1" }, { L1C, 20, "sse4_2" }, { L1C, 21, "x2apic" },
    { L1C, 22, "movbe" }, { L1C, 23, "popcnt" }, { L1C, 25, "aes" }, { L1C, 26, "xsave" }, { L1C, 28, "avx" },
    { L1C, 29, "f16c" }, { L1C, 30, "rdrand" }, { L1C, 31, "hypervisor" },
    { X1C, 0, "lahf_lm" }, { X1C, 2, "svm" }, { X1C, 5, "abm" }, { X1C, 6, "sse4a" }, { X1C, 8, "3dnowprefetch" },
    { X1C, 16, "fma4" },
    { L7B, 0, "fsgsbase" }, { L7B, 3, "bmi1" }, { L7B, 5, "avx2" }, { L7B, 7, "smep" }, { L7B, 8, "bmi2" },
    { L7B, 9, "erms" }, { L7B, 10, "invpcid" }, { L7B, 16, "avx512f" }, { L7B, 17, "avx512dq" }, { L7B, 18, "rdseed" },
    { L7B, 19, "adx" }, { L7B, 20, "smap" }, { L7B, 23, "clflushopt" }, { L7B, 24, "clwb" }, { L7B, 28, "avx512cd" },
    { L7B, 29, "sha_ni" }, { L7B, 30, "avx512bw" }, { L7B, 31, "avx512vl" },
    { L7C, 1, "avx512vbmi" }, { L7C, 8, "gfni" }, { L7C, 9, "vaes" }, { L7C, 10, "vpclmulqdq" }, { L7C, 22, "rdpid" },
    { L7D, 4, "fsrm" },
};

static char g_cpu_flags[900];
static uint32_t g_max_basic, g_mhz;

/* the speed in MHz: as the name says ("@ 3.20GHz"), as cpuid leaf 0x16 says,
 * or measured (to 100 MHz) */
static uint32_t cpu_mhz(const char* brand) {
    const char* at = strstr(brand, "@ ");
    if (at) {
        const char* s = at + 2;
        uint32_t whole = 0, frac = 0, div = 1;
        while (*s >= '0' && *s <= '9') whole = whole * 10 + (uint32_t)(*s++ - '0');
        if (*s == '.') for (s++; *s >= '0' && *s <= '9' && div < 1000; s++) { frac = frac * 10 + (uint32_t)(*s - '0'); div *= 10; }
        if (!strncmp(s, "GHz", 3) && whole < 20) return whole * 1000 + frac * 1000 / div;
        if (!strncmp(s, "MHz", 3) && whole) return whole;
    }
    if (g_max_basic >= 0x16) {
        uint32_t r[4];
        cpuid2(0x16, 0, r);
        if ((r[0] & 0xFFFF) >= 100) return r[0] & 0xFFFF;
    }
    uint32_t t0 = timer_ms();
    while (timer_ms() == t0) __asm__ volatile("pause");
    uint64_t c0 = rdtsc();
    uint32_t t1 = timer_ms();
    while (timer_ms() - t1 < 50) __asm__ volatile("pause");
    uint64_t mhz = (rdtsc() - c0) / 50000u;
    return (uint32_t)((mhz + 50) / 100 * 100);
}

static int ncpu(void) { int n = cpu_count(); return n < 1 ? 1 : n > SMP_MAX_CPUS ? SMP_MAX_CPUS : n; }
static const char* cpu_name(void) { const sysinfo_t* si = sysinfo_get(); return si->cpu_brand[0] ? si->cpu_brand : si->cpu_vendor; }

/* the processors that share cpu i's core (what 0) or package (what 1): "0,1" */
static void cpu_list(int i, int what, char* out, int cap) {
    const cpu_topo_t* t = cpu_topo(i);
    out[0] = 0;
    for (int k = 0; k < ncpu(); k++) {
        const cpu_topo_t* o = cpu_topo(k);
        if (what == 0 ? o->phys != t->phys || o->pkg != t->pkg : o->pkg != t->pkg) continue;
        char n[8];
        ksnprintf(n, sizeof(n), out[0] ? ",%d" : "%d", k);
        kstrlcat(out, n, (size_t)cap);
    }
    if (!out[0]) ksnprintf(out, (size_t)cap, "%d", i);
}
/* threads in cpu i's package, and its cores */
static void pkg_counts(int i, int* threads, int* cores) {
    const cpu_topo_t* t = cpu_topo(i);
    int th = 0, co = 0;
    for (int k = 0; k < ncpu(); k++) {
        const cpu_topo_t* o = cpu_topo(k);
        if (o->pkg != t->pkg) continue;
        th++;
        int first = 1;
        for (int j = 0; j < k; j++) if (cpu_topo(j)->pkg == t->pkg && cpu_topo(j)->phys == o->phys) { first = 0; break; }
        co += first;
    }
    *threads = th ? th : 1;
    *cores = co ? co : 1;
}

static void gen_cpuinfo(sb_t* s) {
    const sysinfo_t* si = sysinfo_get();
    for (int i = 0; i < ncpu(); i++) {
        const cpu_topo_t* t = cpu_topo(i);
        int th, co;
        pkg_counts(i, &th, &co);
        sb_printf(s, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n",
                  i, si->cpu_vendor, si->cpu_family, si->cpu_model, cpu_name(), si->cpu_stepping);
        sb_printf(s, "cpu MHz\t\t: %u.000\nphysical id\t: %u\nsiblings\t: %d\ncore id\t\t: %u\ncpu cores\t: %d\napicid\t\t: %u\n",
                  g_mhz, t->pkg, th, t->core, co, t->apic);
        sb_printf(s, "fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\n", g_max_basic);
        sb_puts(s, "flags\t\t: ");
        sb_puts(s, g_cpu_flags);
        sb_printf(s, "\nbogomips\t: %u.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 39 bits physical, 48 bits virtual\n\n",
                  g_mhz * 2);
    }
}

/* ── memory, time, load ── */
static void gen_meminfo(sb_t* s) {
    meminfo_t m;
    meminfo_get(&m);
    uint32_t avail = m.avail_kb > m.total_kb ? m.total_kb : m.avail_kb;
    sb_printf(s, "MemTotal:       %8u kB\nMemFree:        %8u kB\nMemAvailable:   %8u kB\n", m.total_kb, m.free_kb, avail);
    sb_printf(s, "Buffers:        %8u kB\nCached:         %8u kB\nSwapCached:     %8u kB\n", 0u, m.cached_kb, 0u);
    sb_printf(s, "Active:         %8u kB\nInactive:       %8u kB\n", m.used_kb - m.cached_kb, m.cached_kb);
    sb_printf(s, "SwapTotal:      %8u kB\nSwapFree:       %8u kB\nShmem:          %8u kB\n", 0u, 0u, 0u);
    sb_printf(s, "Slab:           %8u kB\nSReclaimable:   %8u kB\nKernelStack:    %8u kB\n", m.kernel_kb, 0u, 0u);
    sb_printf(s, "CommitLimit:    %8u kB\nCommitted_AS:   %8u kB\n", m.total_kb, m.used_kb);
}

/* processor time in ticks: each core's busy time, the rest idle */
static void core_ticks(int c, uint64_t* busy, uint64_t* idle) {
    uint64_t up = timer_ms();
    uint64_t b = task_core_busy_ms(c);
    if (b > up) b = up;
    *busy = b / (1000 / USER_HZ);
    *idle = (up - b) / (1000 / USER_HZ);
}

static void gen_stat(sb_t* s, const procfs_proc_t* ps, int np) {
    uint64_t tb = 0, ti = 0;
    for (int c = 0; c < ncpu(); c++) { uint64_t b, i; core_ticks(c, &b, &i); tb += b; ti += i; }
    sb_printf(s, "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n", (unsigned long long)tb, (unsigned long long)ti);
    for (int c = 0; c < ncpu(); c++) {
        uint64_t b, i;
        core_ticks(c, &b, &i);
        sb_printf(s, "cpu%d %llu 0 0 %llu 0 0 0 0 0 0\n", c, (unsigned long long)b, (unsigned long long)i);
    }
    int running = 0, last = 0;
    for (int k = 0; k < np; k++) { if (ps[k].i.state == 'R') running++; if (ps[k].i.pid > last) last = ps[k].i.pid; }
    sb_printf(s, "intr 0\nctxt 0\nbtime %u\nprocesses %d\nprocs_running %d\nprocs_blocked 0\n",
              rtc_now() - timer_ms() / 1000u, last, running ? running : 1);
}

static void gen_uptime(sb_t* s) {
    uint32_t ms = timer_ms();
    uint64_t idle = 0;
    for (int c = 0; c < ncpu(); c++) { uint64_t b, i; core_ticks(c, &b, &i); idle += i; }
    sb_printf(s, "%u.%02u %llu.%02llu\n", ms / 1000u, ms / 10u % 100u,
              (unsigned long long)(idle / USER_HZ), (unsigned long long)(idle % USER_HZ));
}

static void gen_loadavg(sb_t* s, const procfs_proc_t* ps, int np) {
    uint32_t la[3];
    task_loadavg(la);
    int running = 0, last = 0;
    for (int k = 0; k < np; k++) { if (ps[k].i.state == 'R') running++; if (ps[k].i.pid > last) last = ps[k].i.pid; }
    sb_printf(s, "%u.%02u %u.%02u %u.%02u %d/%d %d\n", la[0] / 100, la[0] % 100, la[1] / 100, la[1] % 100,
              la[2] / 100, la[2] % 100, running ? running : 1, np, last);
}

static void gen_mounts(sb_t* s) {
    sb_puts(s, fsdisk_is_installed() ? "/dev/sda2 / ext4 rw,relatime 0 0\n" : "rootfs / ramfs rw 0 0\n");
    sb_puts(s, "proc /proc proc rw,nosuid,nodev,noexec 0 0\n");
    int disk = 1;
    for (int m = 1; m < 64; m++) {
        const char* p = fs_mount_point(m);
        if (!p || !p[0]) continue;
        sb_printf(s, "/dev/sd%c1 %s vfat rw,relatime 0 0\n", 'a' + (disk++ % 26), p);
    }
}

/* ── a process ── */
static const procfs_proc_t* find_proc(const procfs_proc_t* ps, int np, int pid) {
    for (int k = 0; k < np; k++) if (ps[k].i.pid == pid) return &ps[k];
    return NULL;
}
static char state_of(const banana_procinfo_t* i) {
    if (i->state == 'Z') return 'Z';
    if (i->state == 'T') return 'T';
    return i->sched == 'R' || i->sched == 'r' || i->sched == 'A' ? 'R' : 'S';
}
static const char* state_name(char c) {
    return c == 'R' ? "R (running)" : c == 'Z' ? "Z (zombie)" : c == 'T' ? "T (stopped)" : "S (sleeping)";
}
static int tty_nr(const banana_procinfo_t* i) { return i->tty >= 0 && i->tty < 0x100 ? (136 << 8) | i->tty : 0; }

static void gen_pid_stat(sb_t* s, const procfs_proc_t* p) {
    const banana_procinfo_t* i = &p->i;
    unsigned long long ut = i->cpu_ms / (1000 / USER_HZ), st = i->start_ms / (1000 / USER_HZ);
    sb_printf(s, "%d (%s) %c %d %d %d %d %d 4194304 0 0 0 0 %llu 0 0 0 20 0 %u 0 %llu %llu %llu ",
              i->pid, i->name, state_of(i), i->ppid, i->pgid, i->sid, tty_nr(i), i->tpgid,
              ut, i->threads, st, i->mem, i->mem / 4096);
    sb_puts(s, "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}
static void gen_pid_status(sb_t* s, const procfs_proc_t* p) {
    const banana_procinfo_t* i = &p->i;
    sb_printf(s, "Name:\t%s\nUmask:\t0022\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n",
              i->name, state_name(state_of(i)), i->pid, i->pid, i->ppid);
    sb_printf(s, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t256\n",
              p->ruid, p->euid, p->euid, p->euid, p->rgid, p->egid, p->egid, p->egid);
    sb_printf(s, "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmRSS:\t%8llu kB\nThreads:\t%u\nSigPnd:\t%016llx\n",
              i->mem / 1024, i->mem / 1024, i->mem / 1024, i->threads, i->sigpending);
    sb_printf(s, "Cpus_allowed_list:\t0-%d\nvoluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n", ncpu() - 1);
}

/* ── the paths ── */
static int parse_pid(const char* c, int len) {
    if (len <= 0 || len > 9) return -1;
    int v = 0;
    for (int k = 0; k < len; k++) { if (c[k] < '0' || c[k] > '9') return -1; v = v * 10 + (c[k] - '0'); }
    return v;
}

int procfs_path(const char* path, char* out, int cap) {
    if (!path || !path[0] || cap < 8) return 0;
    char full[FS_PATH_LEN * 2];
    if (path[0] == '/') kstrlcpy(full, path, sizeof(full));
    else {
        fs_cwd_path(full, FS_PATH_LEN);
        if (strncmp(full, "/proc", 5) && strncmp(path, "proc", 4) && strncmp(path, "..", 2)) return 0;   /* (quick: nowhere near) */
        kstrlcat(full, "/", sizeof(full));
        kstrlcat(full, path, sizeof(full));
    }
    /* "." and ".." taken out, "//" made one */
    char res[FS_PATH_LEN];
    int n = 0;
    res[0] = 0;
    const char* c = full;
    while (*c) {
        while (*c == '/') c++;
        const char* e = c;
        while (*e && *e != '/') e++;
        int len = (int)(e - c);
        if (len == 0) break;
        if (len == 1 && c[0] == '.') { c = e; continue; }
        if (len == 2 && c[0] == '.' && c[1] == '.') {
            while (n > 0 && res[n - 1] != '/') n--;
            if (n > 0) n--;
            res[n] = 0;
            c = e;
            continue;
        }
        if (n + len + 2 >= (int)sizeof(res)) return 0;
        res[n++] = '/';
        memcpy(res + n, c, (size_t)len);
        n += len;
        res[n] = 0;
        c = e;
    }
    if (strcmp(res, "/proc") && strncmp(res, "/proc/", 6)) return 0;
    kstrlcpy(out, res, (size_t)cap);
    return 1;
}

static const char* const g_root_files[] = { "cpuinfo", "meminfo", "stat", "uptime", "loadavg", "version",
                                            "mounts", "filesystems", "cmdline", "swaps", NULL };
static const char* const g_pid_files[] = { "stat", "status", "cmdline", "comm", "statm", "mounts", NULL };
static const char* const g_kernel_files[] = { "hostname", "ostype", "osrelease", "pid_max", NULL };

static int in_list(const char* const* l, const char* name, int len) {
    for (int k = 0; l[k]; k++) if ((int)strlen(l[k]) == len && !strncmp(l[k], name, (size_t)len)) return 1;
    return 0;
}

/* the parts after /proc: up to 3 (pointers into apath, lengths) */
static int split(const char* apath, const char* part[3], int len[3]) {
    const char* c = apath + 5;
    int n = 0;
    while (*c) {
        while (*c == '/') c++;
        if (!*c) break;
        const char* e = c;
        while (*e && *e != '/') e++;
        if (n == 3) return 4;
        part[n] = c;
        len[n] = (int)(e - c);
        n++;
        c = e;
    }
    return n;
}

/* a process folder's pid ("self": the caller's); -1 if not one */
static int pid_of(const char* part, int len) {
    if (len == 4 && !strncmp(part, "self", 4)) return app_current_pid();
    if (len == 11 && !strncmp(part, "thread-self", 11)) return app_current_pid();
    return parse_pid(part, len);
}

int procfs_kind(const char* apath) {
    const char* part[3];
    int len[3];
    int n = split(apath, part, len);
    if (n == 0) return 2;
    if (n > 3) return 0;
    if (n == 1) {
        if (in_list(g_root_files, part[0], len[0])) return 1;
        if (len[0] == 3 && !strncmp(part[0], "sys", 3)) return 2;
        if ((len[0] == 4 && !strncmp(part[0], "self", 4)) || (len[0] == 11 && !strncmp(part[0], "thread-self", 11)))
            return app_current_pid() > 0 ? 3 : 0;
    }
    if (!strncmp(part[0], "sys", 3) && len[0] == 3) {
        if (n == 2) return len[1] == 6 && !strncmp(part[1], "kernel", 6) ? 2 : 0;
        return len[1] == 6 && !strncmp(part[1], "kernel", 6) && in_list(g_kernel_files, part[2], len[2]) ? 1 : 0;
    }
    int pid = pid_of(part[0], len[0]);
    if (pid <= 0 || n == 3) return 0;
    procfs_proc_t* ps = (procfs_proc_t*)kmalloc(sizeof(procfs_proc_t) * PROC_MAX);
    if (!ps) return 0;
    int np = app_proc_snapshot(ps, PROC_MAX);
    int there = find_proc(ps, np, pid) != NULL;
    kfree(ps);
    if (!there) return 0;
    if (n == 1) return 2;
    if (in_list(g_pid_files, part[1], len[1])) return 1;
    if ((len[1] == 3 && !strncmp(part[1], "exe", 3)) || (len[1] == 3 && !strncmp(part[1], "cwd", 3))) return 3;
    return 0;
}

char* procfs_read(const char* apath, uint32_t* len) {
    const char* part[3];
    int pl[3];
    int n = split(apath, part, pl);
    if (n < 1 || n > 3) return NULL;
    char name[32];
    procfs_proc_t* ps = (procfs_proc_t*)kmalloc(sizeof(procfs_proc_t) * PROC_MAX);
    if (!ps) return NULL;
    int np = app_proc_snapshot(ps, PROC_MAX);
    sb_t s = { 0 };
    int ok = 1;
    if (n == 1) {
        kstrlcpy(name, part[0], (size_t)(pl[0] + 1 < (int)sizeof(name) ? pl[0] + 1 : (int)sizeof(name)));
        if (!strcmp(name, "cpuinfo")) gen_cpuinfo(&s);
        else if (!strcmp(name, "meminfo")) gen_meminfo(&s);
        else if (!strcmp(name, "stat")) gen_stat(&s, ps, np);
        else if (!strcmp(name, "uptime")) gen_uptime(&s);
        else if (!strcmp(name, "loadavg")) gen_loadavg(&s, ps, np);
        else if (!strcmp(name, "version"))
            sb_printf(&s, "Banana OS version 0.6 (banana@banana-os) (build %s, %s) #1 SMP %s\n", BANANA_BUILD, BANANA_COMMIT,
#ifdef __x86_64__
                      "x86_64"
#else
                      "i686"
#endif
            );
        else if (!strcmp(name, "mounts")) gen_mounts(&s);
        else if (!strcmp(name, "filesystems")) sb_puts(&s, "nodev\tproc\nnodev\tramfs\n\text4\n\tvfat\n\texfat\n");
        else if (!strcmp(name, "cmdline")) { sb_puts(&s, sysinfo_get()->cmdline); sb_puts(&s, "\n"); }
        else if (!strcmp(name, "swaps")) sb_puts(&s, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
        else ok = 0;
    } else if (n == 3 && pl[0] == 3 && !strncmp(part[0], "sys", 3)) {
        kstrlcpy(name, part[2], (size_t)(pl[2] + 1 < (int)sizeof(name) ? pl[2] + 1 : (int)sizeof(name)));
        if (!strcmp(name, "hostname")) { sb_puts(&s, hostname_get()); sb_puts(&s, "\n"); }
        else if (!strcmp(name, "ostype")) sb_puts(&s, "Banana OS\n");
        else if (!strcmp(name, "osrelease")) sb_puts(&s, "0.6\n");
        else if (!strcmp(name, "pid_max")) sb_puts(&s, "32768\n");
        else ok = 0;
    } else if (n == 2) {
        const procfs_proc_t* p = find_proc(ps, np, pid_of(part[0], pl[0]));
        kstrlcpy(name, part[1], (size_t)(pl[1] + 1 < (int)sizeof(name) ? pl[1] + 1 : (int)sizeof(name)));
        if (!p) ok = 0;
        else if (!strcmp(name, "stat")) gen_pid_stat(&s, p);
        else if (!strcmp(name, "status")) gen_pid_status(&s, p);
        else if (!strcmp(name, "comm")) { sb_puts(&s, p->i.name); sb_puts(&s, "\n"); }
        else if (!strcmp(name, "statm"))
            sb_printf(&s, "%llu %llu 0 0 0 %llu 0\n", p->i.mem / 4096, p->i.mem / 4096, p->i.mem / 4096);
        else if (!strcmp(name, "mounts")) gen_mounts(&s);
        else if (!strcmp(name, "cmdline")) {
            /* its arguments, each ended by a NUL (as the kernel keeps them: with spaces between) */
            const char* c = p->i.cmdline;
            uint32_t at = 0;
            char z = 0;
            for (; c[at]; at++) sb_add(&s, c[at] == ' ' ? &z : &c[at], 1);
            sb_add(&s, &z, 1);
        } else ok = 0;
    } else ok = 0;
    kfree(ps);
    if (!ok || s.oom) { kfree(s.b); return NULL; }
    if (!s.b) { s.b = (char*)kmalloc(1); if (!s.b) return NULL; s.b[0] = 0; }
    *len = s.n;
    return s.b;
}

int procfs_entry(const char* apath, int index, char* name, int cap, int* is_dir) {
    const char* part[3];
    int pl[3];
    int n = split(apath, part, pl);
    if (index < 0) return 0;
    if (n == 0) {
        int k = 0;
        for (; g_root_files[k]; k++) if (index == k) { kstrlcpy(name, g_root_files[k], (size_t)cap); *is_dir = 0; return 1; }
        if (index == k) { kstrlcpy(name, "sys", (size_t)cap); *is_dir = 1; return 1; }
        if (index == k + 1) { kstrlcpy(name, "self", (size_t)cap); *is_dir = 1; return app_current_pid() > 0; }
        index -= k + 2;
        procfs_proc_t* ps = (procfs_proc_t*)kmalloc(sizeof(procfs_proc_t) * PROC_MAX);
        if (!ps) return 0;
        int np = app_proc_snapshot(ps, PROC_MAX), r = 0;
        if (index < np) { ksnprintf(name, (size_t)cap, "%d", ps[index].i.pid); *is_dir = 1; r = 1; }
        kfree(ps);
        return r;
    }
    if (n == 1 && pl[0] == 3 && !strncmp(part[0], "sys", 3)) {
        if (index) return 0;
        kstrlcpy(name, "kernel", (size_t)cap);
        *is_dir = 1;
        return 1;
    }
    if (n == 2 && pl[0] == 3 && !strncmp(part[0], "sys", 3)) {
        for (int k = 0; g_kernel_files[k]; k++) if (index == k) { kstrlcpy(name, g_kernel_files[k], (size_t)cap); *is_dir = 0; return 1; }
        return 0;
    }
    if (n == 1 && pid_of(part[0], pl[0]) > 0) {
        int k = 0;
        for (; g_pid_files[k]; k++) if (index == k) { kstrlcpy(name, g_pid_files[k], (size_t)cap); *is_dir = 0; return 1; }
        if (index == k) { kstrlcpy(name, "exe", (size_t)cap); *is_dir = 0; return 1; }
        return 0;
    }
    return 0;
}

int procfs_link(const char* apath, char* out, int cap) {
    const char* part[3];
    int pl[3];
    int n = split(apath, part, pl);
    if (n == 1 && ((pl[0] == 4 && !strncmp(part[0], "self", 4)) || (pl[0] == 11 && !strncmp(part[0], "thread-self", 11)))) {
        int pid = app_current_pid();
        if (pid <= 0) return -1;
        return ksnprintf(out, (size_t)cap, "%d", pid);
    }
    if (n == 2 && pl[1] == 3 && (!strncmp(part[1], "exe", 3) || !strncmp(part[1], "cwd", 3))) {
        int pid = pid_of(part[0], pl[0]);
        if (!strncmp(part[1], "cwd", 3)) {
            if (pid != app_current_pid()) return -1;
            fs_cwd_path(out, cap);
            return (int)strlen(out);
        }
        procfs_proc_t* ps = (procfs_proc_t*)kmalloc(sizeof(procfs_proc_t) * PROC_MAX);
        if (!ps) return -1;
        int np = app_proc_snapshot(ps, PROC_MAX), r = -1;
        const procfs_proc_t* p = find_proc(ps, np, pid);
        if (p && p->exe[0]) { kstrlcpy(out, p->exe, (size_t)cap); r = (int)strlen(out); }
        kfree(ps);
        return r;
    }
    return -1;
}

/* ── at the start: the processor's flags and speed, /sys/devices/system/cpu ── */

/* the text, written only if it is not there already (an installed disk is
 * not written at every start) */
static void put(const char* path, const char* text) {
    uint32_t n = (uint32_t)strlen(text);
    int idx = fs_find_file(path);
    if (idx >= 0) {
        const fs_file_t* f = fs_file_info(idx);
        if (f && f->size == n) {
            char* old = (char*)kmalloc(n + 1);
            int same = old && fs_read(idx, 0, old, n) == (int)n && memcmp(old, text, n) == 0;
            kfree(old);
            if (same) return;
        }
    }
    fs_write_path_sys(path, text, n);
}

void procfs_boot(void) {
    uint32_t r[4], max_ext, regs[7] = { 0 };
    cpuid2(0, 0, r);
    g_max_basic = r[0];
    cpuid2(0x80000000u, 0, r);
    max_ext = r[0];
    if (g_max_basic >= 1) { cpuid2(1, 0, r); regs[L1D] = r[3]; regs[L1C] = r[2]; }
    if (g_max_basic >= 7) { cpuid2(7, 0, r); regs[L7B] = r[1]; regs[L7C] = r[2]; regs[L7D] = r[3]; }
    if (max_ext >= 0x80000001u) { cpuid2(0x80000001u, 0, r); regs[X1D] = r[3]; regs[X1C] = r[2]; }
    g_cpu_flags[0] = 0;
    for (unsigned i = 0; i < sizeof(g_flags) / sizeof(g_flags[0]); i++) {
        if (!(regs[g_flags[i].reg] & (1u << g_flags[i].bit))) continue;
        if (g_cpu_flags[0]) kstrlcat(g_cpu_flags, " ", sizeof(g_cpu_flags));
        kstrlcat(g_cpu_flags, g_flags[i].name, sizeof(g_cpu_flags));
    }
    g_mhz = cpu_mhz(cpu_name());

    /* /proc: the folder only (what a build before wrote in it goes) */
    const fs_cred_t* oc = fs_cred_swap(NULL);
    if (fs_find_dir("/proc") >= 0 && (fs_list_files("/proc", NULL, 0) > 0 || fs_list_dirs("/proc", NULL, 0) > 0)) fs_delete("/proc", 1);
    if (fs_find_dir("/proc") < 0) fs_mkdir_p("/proc");
    fs_chmod("/proc", 0555);
    fs_cred_swap(oc);

    /* /sys/devices/system/cpu: which cores there are, each one's place */
    int n = ncpu();
    char path[96], line[256], p2[128];
    ksnprintf(line, sizeof(line), n > 1 ? "0-%d\n" : "0\n", n - 1);
    fs_mkdir_p_sys("/sys/devices/system/cpu");
    put("/sys/devices/system/cpu/online", line);
    put("/sys/devices/system/cpu/possible", line);
    put("/sys/devices/system/cpu/present", line);
    for (int i = 0; i < n; i++) {
        const cpu_topo_t* t = cpu_topo(i);
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology", i);
        fs_mkdir_p_sys(path);
        char core[128], pkg[128];
        cpu_list(i, 0, core, sizeof(core));
        cpu_list(i, 1, pkg, sizeof(pkg));
        kstrlcat(core, "\n", sizeof(core));
        kstrlcat(pkg, "\n", sizeof(pkg));
        ksnprintf(line, sizeof(line), "%u\n", t->pkg);
        ksnprintf(p2, sizeof(p2), "%s/physical_package_id", path); put(p2, line);
        ksnprintf(p2, sizeof(p2), "%s/die_id", path); put(p2, "0\n");
        ksnprintf(line, sizeof(line), "%u\n", t->core);
        ksnprintf(p2, sizeof(p2), "%s/core_id", path); put(p2, line);
        ksnprintf(p2, sizeof(p2), "%s/thread_siblings_list", path); put(p2, core);
        ksnprintf(p2, sizeof(p2), "%s/core_cpus_list", path); put(p2, core);
        ksnprintf(p2, sizeof(p2), "%s/core_siblings_list", path); put(p2, pkg);
        ksnprintf(p2, sizeof(p2), "%s/package_cpus_list", path); put(p2, pkg);
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq", i);
        fs_mkdir_p_sys(path);
        ksnprintf(line, sizeof(line), "%u\n", g_mhz * 1000u);
        ksnprintf(p2, sizeof(p2), "%s/cpuinfo_max_freq", path); put(p2, line);
        ksnprintf(p2, sizeof(p2), "%s/scaling_cur_freq", path); put(p2, line);
    }
    /* (a start with more cores before: theirs go, or they would be counted) */
    for (int i = n; i < SMP_MAX_CPUS; i++) {
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", i);
        if (fs_find_dir(path) < 0) break;
        oc = fs_cred_swap(NULL);
        fs_delete(path, 1);
        fs_cred_swap(oc);
    }
}
