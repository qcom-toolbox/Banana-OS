#include "sysinfo.h"
#include "serial.h"
#include "types.h"

/* Multiboot info structure (partial) */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;   /* KiB below 1MB  */
    uint32_t mem_upper;   /* KiB above 1MB  */
    /* ... rest unused */
} __attribute__((packed)) mb_info_t;

static sysinfo_t info;

static void cpuid(uint32_t leaf,
                  uint32_t* eax, uint32_t* ebx,
                  uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
        : 
    );
}

static void cpuid_ex(uint32_t leaf, uint32_t subleaf,
                     uint32_t* eax, uint32_t* ebx,
                     uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
        :
    );
}

static void store_str(char* dst, uint32_t val) {
    dst[0] = (val)       & 0xFF;
    dst[1] = (val >>  8) & 0xFF;
    dst[2] = (val >> 16) & 0xFF;
    dst[3] = (val >> 24) & 0xFF;
}

void sysinfo_init(uint32_t mb_info_addr) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_basic = 0;
    uint32_t max_ext = 0;

    /* ── CPU vendor ─────────────────────────────────────────────── */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    max_basic = eax;
    store_str(info.cpu_vendor + 0, ebx);
    store_str(info.cpu_vendor + 4, edx);
    store_str(info.cpu_vendor + 8, ecx);
    info.cpu_vendor[12] = '\0';

    /* ── CPU brand string (leaves 0x80000002-4) ─────────────────── */
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    max_ext = eax;
    if (max_ext >= 0x80000004) {
        uint32_t* p = (uint32_t*)info.cpu_brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &eax, &ebx, &ecx, &edx);
            *p++ = eax; *p++ = ebx; *p++ = ecx; *p++ = edx;
        }
        info.cpu_brand[48] = '\0';
        /* trim leading spaces */
        char* s = info.cpu_brand;
        while (*s == ' ') s++;
        if (s != info.cpu_brand) {
            char tmp[49];
            int i = 0;
            while (s[i]) { tmp[i] = s[i]; i++; }
            tmp[i] = '\0';
            for (i = 0; tmp[i]; i++) info.cpu_brand[i] = tmp[i];
            info.cpu_brand[i] = '\0';
        }
    } else {
        /* fallback */
        const char* fb = "Whatever your hypervisor gives you";
        int i = 0;
        while (fb[i]) { info.cpu_brand[i] = fb[i]; i++; }
        info.cpu_brand[i] = '\0';
    }

    /* ── CPU topology / id ───────────────────────────────────────── */
    info.cpu_cores = 1;
    info.cpu_threads = 1;
    info.cpu_family = 0;
    info.cpu_model = 0;
    info.cpu_stepping = 0;
    info.cpu_has_ht = 0;

    if (max_basic >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        uint32_t stepping = eax & 0xFu;
        uint32_t model = (eax >> 4) & 0xFu;
        uint32_t family = (eax >> 8) & 0xFu;
        uint32_t ext_model = (eax >> 16) & 0xFu;
        uint32_t ext_family = (eax >> 20) & 0xFFu;
        if (family == 0xFu) family += ext_family;
        if (family == 0x6u || family == 0xFu) model += (ext_model << 4);

        info.cpu_family = family;
        info.cpu_model = model;
        info.cpu_stepping = stepping;
        info.cpu_has_ht = (edx & (1u << 28)) ? 1u : 0u;

        uint32_t logical = (ebx >> 16) & 0xFFu;
        if (logical > 0) info.cpu_threads = logical;
    }

    if (max_basic >= 4) {
        cpuid_ex(4, 0, &eax, &ebx, &ecx, &edx);
        uint32_t cores = ((eax >> 26) & 0x3Fu) + 1u;
        if (cores > 0) info.cpu_cores = cores;
    } else if (max_ext >= 0x80000008) {
        cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
        uint32_t cores = (ecx & 0xFFu) + 1u;
        if (cores > 0) info.cpu_cores = cores;
    }

    if (info.cpu_threads < info.cpu_cores) info.cpu_threads = info.cpu_cores;

    /* ── Memory from Multiboot ──────────────────────────────────── */
    if (mb_info_addr) {
        mb_info_t* mb = (mb_info_t*)(uintptr_t)mb_info_addr;
        if (mb->flags & 0x1) {
            /* mem_upper is KiB above 1MB; add 1024 for the first MB */
            info.mem_kb = mb->mem_upper + 1024;
        } else {
            info.mem_kb = 0;
        }
    } else {
        info.mem_kb = 0;
    }
}

/* ── Multiboot2 memory info ─────────────────────────────────────── */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) mb2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint32_t type;   /* 4 */
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed)) mb2_tag_basic_mem_t;

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;      /* 1 = available RAM */
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

typedef struct {
    uint32_t type;       /* 6 */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries[] */
} __attribute__((packed)) mb2_tag_mmap_t;

static uint32_t align_up8(uint32_t v) {
    return (v + 7u) & ~7u;
}

void sysinfo_init_mb2(uint32_t mb2_info_addr) {
    /* Keep CPU fields from cpuid() logic */
    sysinfo_init(0);

    info.mem_kb = 0;
    if (!mb2_info_addr) return;

    mb2_info_t* mb2 = (mb2_info_t*)(uintptr_t)mb2_info_addr;
    uint32_t total = mb2->total_size;
    uint32_t off = 8;

    /* GRUB passes the EFI system table (tag 11/12) only when it was
     * started by UEFI firmware */
    for (uint32_t o = 8; o + 8 <= total;) {
        mb2_tag_t* t = (mb2_tag_t*)(uintptr_t)(mb2_info_addr + o);
        if (t->type == 0 || t->size < 8) break;
        if (t->type == 11 || t->type == 12) info.uefi = 1;
        if (t->type == 1) {                      /* the command line */
            const char* s = (const char*)t + 8;
            uint32_t n = t->size - 8, i = 0;
            for (; i < n && i < sizeof(info.cmdline) - 1 && s[i]; i++) info.cmdline[i] = s[i];
            info.cmdline[i] = 0;
        }
        if (t->type == 5 && t->size >= 12) {     /* the BIOS boot device */
            info.biosdev = *(const uint32_t*)((const uint8_t*)t + 8);
            info.has_biosdev = 1;
        }
        o += (t->size + 7u) & ~7u;
    }

    while (off + 8 <= total) {
        mb2_tag_t* tag = (mb2_tag_t*)(uintptr_t)(mb2_info_addr + off);
        if (tag->type == 0) break;

        if (tag->type == 6 && tag->size >= sizeof(mb2_tag_mmap_t)) {
            mb2_tag_mmap_t* mm = (mb2_tag_mmap_t*)tag;
            if (mm->entry_size >= sizeof(mb2_mmap_entry_t)) {
                uint64_t max_end = 0;
                uint32_t pos = sizeof(mb2_tag_mmap_t);
                while (pos + mm->entry_size <= mm->size) {
                    mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)((uintptr_t)mm + pos);
                    if (e->type == 1) {
                        uint64_t end = e->base_addr + e->length;
                        if (end > max_end) max_end = end;
                    }
                    pos += mm->entry_size;
                }
                if (max_end > 0) {
                    info.mem_kb = (uint32_t)(max_end / 1024u);
                    return;
                }
            }
        }

        if (tag->type == 4 && tag->size >= sizeof(mb2_tag_basic_mem_t)) {
            mb2_tag_basic_mem_t* mem = (mb2_tag_basic_mem_t*)tag;
            /* mem_upper is KiB above 1MB; add 1024 for the first MB */
            info.mem_kb = mem->mem_upper + 1024u;
            return;
        }

        /* Every valid tag is at least 8 bytes (its own type+size header).
         * Guard against a malformed/zero-size tag stalling the scan forever. */
        uint32_t adv = align_up8(tag->size);
        if (adv < 8) adv = 8;
        off += adv;
    }
}

/* (called once at boot, for the log) */
void sysinfo_log_boot(void) {
    klog("boot: cmdline [%s], bios drive %s0x%x\n", info.cmdline, info.has_biosdev ? "" : "(none) ", info.biosdev);
}

int sysinfo_live_boot(void) {
    /* GRUB's $root (grub.cfg): "cdN" for a CD/DVD under UEFI; under BIOS
     * "hdN" with N = drive - 0x80, so a CD is hd96 (0xE0) while hard disks
     * and USB sticks are hd0..hd15 */
    for (const char* p = info.cmdline; *p; p++) {
        if (!(p[0] == 'r' && p[1] == 'o' && p[2] == 'o' && p[3] == 't' && p[4] == '=')) continue;
        const char* r = p + 5;
        if (r[0] == '(') r++;
        if (r[0] == 'c' && r[1] == 'd') return 1;
        if (r[0] == 'h' && r[1] == 'd' && r[2] >= '0' && r[2] <= '9') {
            int n = 0;
            for (const char* q = r + 2; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
            return n >= 16;
        }
        break;
    }
    /* an older grub.cfg: the BIOS drive (hard disks are 0x80..0x8F) */
    if (info.has_biosdev) return !(info.biosdev >= 0x80 && info.biosdev <= 0x8F);
    return 0;
}

const sysinfo_t* sysinfo_get(void) {
    return &info;
}
