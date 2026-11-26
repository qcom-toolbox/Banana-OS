/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The processor as Linux describes it, for programs that ask the Linux way:
 * /proc/cpuinfo and /sys/devices/system/cpu (online, possible, present and
 * each core's topology/). Java libraries read them - OSHI, through which
 * Minecraft's F3 screen says "CPU: 8x Intel(R) Core(TM) i7 ..." - and so do
 * neofetch and the like.
 *
 * They are files written at every start (by the system, read-only for the
 * others), rewritten only when what they say changed: an installed disk is
 * not written at every boot. */
#include "procfs.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "sysinfo.h"
#include "smp.h"
#include "timer.h"
#include "io.h"

static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

/* the text, written only if it is not there already */
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

/* ── the flags: cpuid's bits, with Linux's names ── */
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

/* the processor's speed in MHz: as its name says ("@ 3.20GHz"), as cpuid
 * leaf 0x16 says, or measured (to 100 MHz: the same at every start) */
static uint32_t cpu_mhz(const char* brand, uint32_t max_basic) {
    const char* at = strstr(brand, "@ ");
    if (at) {
        const char* s = at + 2;
        uint32_t whole = 0, frac = 0, div = 1;
        while (*s >= '0' && *s <= '9') whole = whole * 10 + (uint32_t)(*s++ - '0');
        if (*s == '.') for (s++; *s >= '0' && *s <= '9' && div < 1000; s++) { frac = frac * 10 + (uint32_t)(*s - '0'); div *= 10; }
        if (!strncmp(s, "GHz", 3) && whole < 20) return whole * 1000 + frac * 1000 / div;
        if (!strncmp(s, "MHz", 3) && whole) return whole;
    }
    if (max_basic >= 0x16) {
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

/* threads a core (Hyper-Threading / SMT): cpuid's topology leaves - Intel's
 * (and a virtual machine's) 0xB, AMD's 0x8000001E; 1 when neither says */
static int threads_per_core(const sysinfo_t* si, uint32_t max_basic, uint32_t max_ext) {
    uint32_t r[4];
    if (!si->cpu_has_ht) return 1;
    if (max_basic >= 0xB) {
        cpuid2(0xB, 0, r);
        if (((r[2] >> 8) & 0xFF) == 1 && (r[1] & 0xFFFF)) return (int)(r[1] & 0xFFFF);   /* (level 0: SMT) */
    }
    if (!strcmp(si->cpu_vendor, "AuthenticAMD") && max_ext >= 0x8000001Eu) {
        cpuid2(0x8000001Eu, 0, r);
        return (int)((r[1] >> 8) & 0xFF) + 1;
    }
    return 1;
}

void procfs_boot(void) {
    const sysinfo_t* si = sysinfo_get();
    uint32_t r[4], max_basic, max_ext;
    uint32_t regs[7] = { 0 };
    cpuid2(0, 0, r);
    max_basic = r[0];
    cpuid2(0x80000000u, 0, r);
    max_ext = r[0];
    if (max_basic >= 1) { cpuid2(1, 0, r); regs[L1D] = r[3]; regs[L1C] = r[2]; }
    if (max_basic >= 7) { cpuid2(7, 0, r); regs[L7B] = r[1]; regs[L7C] = r[2]; regs[L7D] = r[3]; }
    if (max_ext >= 0x80000001u) { cpuid2(0x80000001u, 0, r); regs[X1D] = r[3]; regs[X1C] = r[2]; }

    char flags[900];
    flags[0] = 0;
    for (unsigned i = 0; i < sizeof(g_flags) / sizeof(g_flags[0]); i++) {
        if (!(regs[g_flags[i].reg] & (1u << g_flags[i].bit))) continue;
        if (flags[0]) kstrlcat(flags, " ", sizeof(flags));
        kstrlcat(flags, g_flags[i].name, sizeof(flags));
    }

    /* the cores in use; two threads a core if the processor says so and
     * they divide evenly (a virtual machine's cpuid need not match) */
    int n = cpu_count();
    if (n < 1) n = 1;
    int tpc = threads_per_core(si, max_basic, max_ext);
    if (tpc < 1 || n % tpc) tpc = 1;
    int cores = n / tpc;
    const char* name = si->cpu_brand[0] ? si->cpu_brand : si->cpu_vendor;
    uint32_t mhz = cpu_mhz(name, max_basic);

    size_t cap = (size_t)n * 1400 + 64;
    char* t = (char*)kmalloc(cap);
    if (!t) return;
    size_t o = 0;
    for (int i = 0; i < n && o < cap; i++) {
        o += (size_t)ksnprintf(t + o, cap - o,
            "processor\t: %d\n"
            "vendor_id\t: %s\n"
            "cpu family\t: %u\n"
            "model\t\t: %u\n"
            "model name\t: %s\n"
            "stepping\t: %u\n"
            "cpu MHz\t\t: %u.000\n"
            "physical id\t: 0\n"
            "siblings\t: %d\n"
            "core id\t\t: %d\n"
            "cpu cores\t: %d\n"
            "apicid\t\t: %d\n"
            "fpu\t\t: yes\n"
            "fpu_exception\t: yes\n"
            "cpuid level\t: %u\n"
            "wp\t\t: yes\n"
            "flags\t\t: %s\n"
            "bogomips\t: %u.00\n"
            "clflush size\t: 64\n"
            "cache_alignment\t: 64\n"
            "address sizes\t: 39 bits physical, 48 bits virtual\n"
            "\n",
            i, si->cpu_vendor, si->cpu_family, si->cpu_model, name, si->cpu_stepping, mhz,
            n, i / tpc, cores, i, max_basic, flags, mhz * 2);
    }
    fs_mkdir_p_sys("/proc");
    put("/proc/cpuinfo", t);

    /* /sys/devices/system/cpu: which cores there are, and each one's place */
    char path[96], line[64];
    ksnprintf(line, sizeof(line), n > 1 ? "0-%d\n" : "0\n", n - 1);
    fs_mkdir_p_sys("/sys/devices/system/cpu");
    put("/sys/devices/system/cpu/online", line);
    put("/sys/devices/system/cpu/possible", line);
    put("/sys/devices/system/cpu/present", line);
    for (int i = 0; i < n; i++) {
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology", i);
        fs_mkdir_p_sys(path);
        int first = i / tpc * tpc;
        char sib[32];
        if (tpc > 1) ksnprintf(sib, sizeof(sib), "%d-%d\n", first, first + tpc - 1);
        else ksnprintf(sib, sizeof(sib), "%d\n", i);
        static const struct { const char* file; int what; } tf[] = {
            { "physical_package_id", 0 }, { "die_id", 0 }, { "core_id", 1 },
            { "thread_siblings_list", 2 }, { "core_cpus_list", 2 },
            { "core_siblings_list", 3 }, { "package_cpus_list", 3 },
        };
        for (unsigned k = 0; k < sizeof(tf) / sizeof(tf[0]); k++) {
            char p2[128];
            ksnprintf(p2, sizeof(p2), "%s/%s", path, tf[k].file);
            if (tf[k].what == 0) put(p2, "0\n");
            else if (tf[k].what == 1) { ksnprintf(line, sizeof(line), "%d\n", i / tpc); put(p2, line); }
            else if (tf[k].what == 2) put(p2, sib);
            else { ksnprintf(line, sizeof(line), n > 1 ? "0-%d\n" : "0\n", n - 1); put(p2, line); }
        }
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq", i);
        fs_mkdir_p_sys(path);
        char p2[128];
        ksnprintf(line, sizeof(line), "%u\n", mhz * 1000u);
        ksnprintf(p2, sizeof(p2), "%s/cpuinfo_max_freq", path); put(p2, line);
        ksnprintf(p2, sizeof(p2), "%s/scaling_cur_freq", path); put(p2, line);
    }
    /* (a start with more cores before: theirs go, or they would be counted) */
    for (int i = n; i < SMP_MAX_CPUS; i++) {
        ksnprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", i);
        if (fs_find_dir(path) < 0) break;
        const fs_cred_t* oc = fs_cred_swap(NULL);
        fs_delete(path, 1);
        fs_cred_swap(oc);
    }
    kfree(t);
}
