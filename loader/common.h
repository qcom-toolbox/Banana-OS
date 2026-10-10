/* Banana Boot - what the BIOS loader (bios.c, 32-bit) and the UEFI loader
 * (efi.c, 64-bit) share: the menu, the kernel's command line, ELF loading
 * and the Multiboot2 information the kernels read (the same as GRUB gave
 * them: they are started exactly as before - 32-bit protected mode, paging
 * off, EAX = 0x36D76289, EBX = the information). */
#ifndef LOADER_COMMON_H
#define LOADER_COMMON_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define MB2_HEADER_MAGIC 0xE85250D6u
#define MB2_BOOT_MAGIC   0x36D76289u

/* `install` / `update` copy the image onto the disk with the line
 * "set banana_medium=live-cd" changed to "...=install" (kernel/fsdisk.c,
 * patch_marker): the copy the loader ran from tells the kernel whether it is
 * the live CD or the installed system. The BIOS loader holds that line
 * itself; the UEFI one reads it from EFI/BananaOS/medium.cfg - it must not
 * hold it, its Secure Boot signature covers all of its bytes. */

/* ── small string helpers (no C library here) ─────────────────────── */
static u32 c_strlen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static void c_strcat(char* d, const char* s) { d += c_strlen(d); while (*s) *d++ = *s++; *d = 0; }

/* ── the menu: the entries GRUB's menu had (each loader adds its own after
 *    these: the next boot device, the firmware's settings...) ──────── */
#define KERNEL_ENTRIES 5
static __attribute__((unused)) const char* entry_label(int i) {
    switch (i) {
    case 0: return "Banana OS 0.6 (64-bit)";
    case 1: return "Banana OS 0.6 (64-bit, one CPU core)";
    case 2: return "Banana OS 0.6 (32-bit)";
    case 3: return "Banana OS 0.6 (64-bit, boot messages)";
    default: return "Banana OS 0.6 (32-bit, boot messages)";
    }
}
static int entry_is64(int i) { return i == 0 || i == 1 || i == 3; }

/* the value of a "set banana_medium=..." line ("live-cd", "install") */
static __attribute__((unused)) void medium_value(const char* line, u32 len, char* out, u32 cap) {
    u32 i = 0, n = 0;
    while (i < len && line[i] != '=') i++;
    if (i < len) i++;
    while (i < len && n + 1 < cap && line[i] > ' ') out[n++] = line[i++];
    out[n] = 0;
    if (!n) { const char* d = "live-cd"; while (*d) out[n++] = *d++; out[n] = 0; }
}

/* ── the screen size: a line "set banana_video=1920x1080" ("auto": the
 *    loader's own choice), its value padded with spaces to a fixed width so
 *    that Settings can rewrite it in place on the installed disk
 *    (kernel/fsdisk.c). The BIOS loader holds the line; the UEFI one reads it
 *    from EFI/BananaOS/medium.cfg, after the medium line. The menu changes it
 *    for one start (Left / Right). ── */
static __attribute__((unused)) void video_value(const char* s, u32 len, u32* w, u32* h) {
    static const char key[] = "banana_video=";
    *w = *h = 0;
    for (u32 i = 0; i + 13 <= len; i++) {
        u32 k = 0;
        while (k < 13 && s[i + k] == key[k]) k++;
        if (k < 13) continue;
        u32 p = i + 13, a = 0, b = 0;
        while (p < len && s[p] >= '0' && s[p] <= '9' && a < 100000) a = a * 10 + (u32)(s[p++] - '0');
        if (p >= len || s[p] != 'x') return;
        p++;
        while (p < len && s[p] >= '0' && s[p] <= '9' && b < 100000) b = b * 10 + (u32)(s[p++] - '0');
        if (a >= 320 && b >= 200 && a <= 16384 && b <= 16384) { *w = a; *h = b; }
        return;
    }
}

/* the graphics modes the firmware offers (32-bit colour, a linear
 * framebuffer), sorted by size, each size once; id: the firmware's number */
typedef struct { u32 w, h, id; } vmode_t;
#define MAX_VMODES 48

static __attribute__((unused)) void vmode_add(vmode_t* v, int* n, u32 w, u32 h, u32 id) {
    if (w < 640 || h < 480 || w > 8192 || h > 8192) return;
    int i = 0;
    while (i < *n && (v[i].w * v[i].h < w * h || (v[i].w * v[i].h == w * h && v[i].w < w))) i++;
    if (i < *n && v[i].w == w && v[i].h == h) return;
    if (*n >= MAX_VMODES) return;
    for (int k = *n; k > i; k--) v[k] = v[k - 1];
    v[i].w = w; v[i].h = h; v[i].id = id;
    (*n)++;
}

/* w x h's place in the list; -1 (automatic) if it is not there or w is 0 */
static __attribute__((unused)) int vmode_find(const vmode_t* v, int n, u32 w, u32 h) {
    for (int i = 0; i < n; i++) if (v[i].w == w && v[i].h == h) return i;
    return -1;
}

static void c_utoa(u32 v, char* out) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = 0;
}

/* the menu's line: "Screen: 1920 x 1080  (Left / Right to change)" */
static __attribute__((unused)) void vmode_label(char* out, const vmode_t* v, int n, int sel) {
    char num[12];
    out[0] = 0;
    c_strcat(out, "Screen: ");
    if (sel < 0 || sel >= n) c_strcat(out, "automatic");
    else {
        c_utoa(v[sel].w, num); c_strcat(out, num);
        c_strcat(out, " x ");
        c_utoa(v[sel].h, num); c_strcat(out, num);
    }
    if (n) c_strcat(out, "   (Left / Right to change)");
}

/* Left (-1) / Right (+1): the next size, "automatic" between the ends */
static __attribute__((unused)) int vmode_step(int sel, int n, int dir) {
    if (!n) return -1;
    sel += dir;
    if (sel < -1) sel = n - 1;
    if (sel >= n) sel = -1;
    return sel;
}

/* "medium=live-cd nosmp" ... */
static __attribute__((unused)) void build_cmdline(char* out, int entry, const char* medium) {
    out[0] = 0;
    c_strcat(out, "medium=");
    c_strcat(out, medium);
    if (entry == 1) c_strcat(out, " nosmp");
    if (entry == 3 || entry == 4) c_strcat(out, " verbose");
}

static __attribute__((unused)) int has_long_mode(void) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a < 0x80000001u) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u));
    return (d >> 29) & 1;
}

/* ── ELF (32- or 64-bit): the PT_LOAD segments ─────────────────────── */
typedef struct { u64 paddr, offset, filesz, memsz; } seg_t;

static u32 rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
static u64 rd64(const u8* p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }

/* the segments of the ELF file whose start (headers included) is h[0..len);
 * returns how many (up to max), -1 if it is not an x86 ELF executable */
static __attribute__((unused)) int elf_segments(const u8* h, u32 len, seg_t* segs, int max, u64* entry) {
    if (len < 64 || h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return -1;
    int is64 = h[4] == 2;
    u64 phoff = is64 ? rd64(h + 32) : rd32(h + 28);
    u32 phentsize = rd16(h + (is64 ? 54 : 42)), phnum = rd16(h + (is64 ? 56 : 44));
    *entry = is64 ? rd64(h + 24) : rd32(h + 24);
    int n = 0;
    for (u32 i = 0; i < phnum && n < max; i++) {
        u64 o = phoff + (u64)i * phentsize;
        if (o + (is64 ? 56u : 32u) > len) return -1;
        const u8* p = h + o;
        if (rd32(p) != 1) continue;                       /* PT_LOAD */
        if (is64) {
            segs[n].offset = rd64(p + 8);  segs[n].paddr = rd64(p + 24);
            segs[n].filesz = rd64(p + 32); segs[n].memsz = rd64(p + 40);
        } else {
            segs[n].offset = rd32(p + 4);  segs[n].paddr = rd32(p + 12);
            segs[n].filesz = rd32(p + 16); segs[n].memsz = rd32(p + 20);
        }
        if (segs[n].memsz) n++;
    }
    return n;
}

/* the framebuffer size the kernel's Multiboot2 header asks for (tag 5) */
static __attribute__((unused)) void mb2_fb_request(const u8* h, u32 len, u32* w, u32* hgt, u32* depth) {
    for (u32 o = 0; o + 16 <= len && o < 32768; o += 8) {
        if (rd32(h + o) != MB2_HEADER_MAGIC) continue;
        u32 hlen = rd32(h + o + 8);
        if (rd32(h + o) + rd32(h + o + 4) + hlen + rd32(h + o + 12) != 0) continue;   /* checksum */
        for (u32 t = o + 16; t + 8 <= o + hlen && t + 8 <= len;) {
            u16 type = rd16(h + t);
            u32 size = rd32(h + t + 4);
            if (type == 0 || size < 8) break;
            if (type == 5 && size >= 20) { *w = rd32(h + t + 8); *hgt = rd32(h + t + 12); *depth = rd32(h + t + 16); }
            t += (size + 7) & ~7u;
        }
        return;
    }
}

/* ── building the Multiboot2 information ─────────────────────────────── */
typedef struct { u8* base; u32 off; } mb2_t;

static void mb2_begin(mb2_t* m, u8* base) { m->base = base; m->off = 8; }

/* a new tag of `size` bytes (header included), zeroed */
static __attribute__((unused)) u8* mb2_tag(mb2_t* m, u32 type, u32 size) {
    m->off = (m->off + 7) & ~7u;
    u8* t = m->base + m->off;
    for (u32 i = 0; i < size; i++) t[i] = 0;
    *(u32*)t = type;
    *(u32*)(t + 4) = size;
    m->off += size;
    return t;
}

static __attribute__((unused)) void mb2_string(mb2_t* m, u32 type, const char* s) {
    u32 n = c_strlen(s) + 1;
    u8* t = mb2_tag(m, type, 8 + n);
    for (u32 i = 0; i < n; i++) t[8 + i] = (u8)s[i];
}

static __attribute__((unused)) u32 mb2_end(mb2_t* m) {
    mb2_tag(m, 0, 8);
    *(u32*)m->base = m->off;
    *(u32*)(m->base + 4) = 0;
    return m->off;
}

/* tag 8, RGB direct colour */
static void mb2_framebuffer(mb2_t* m, u64 addr, u32 pitch, u32 w, u32 h, u32 bpp,
                            u8 rpos, u8 rsize, u8 gpos, u8 gsize, u8 bpos, u8 bsize) {
    u8* t = mb2_tag(m, 8, 38);
    *(u64*)(t + 8) = addr;
    *(u32*)(t + 16) = pitch;
    *(u32*)(t + 20) = w;
    *(u32*)(t + 24) = h;
    t[28] = (u8)bpp;
    t[29] = 1;                                            /* RGB */
    t[32] = rpos; t[33] = rsize; t[34] = gpos; t[35] = gsize; t[36] = bpos; t[37] = bsize;
}

/* Banana OS's own tag: the sizes the firmware offers (Settings > Screen
 * lists them on displays without a graphics driver): a count, then
 * 16-bit width and height pairs */
#define MB2_TAG_BANANA_MODES 0xBA00u
static __attribute__((unused)) void mb2_modes(mb2_t* m, const vmode_t* v, int n) {
    if (n <= 0) return;
    u8* t = mb2_tag(m, MB2_TAG_BANANA_MODES, 12 + 4 * (u32)n);
    *(u32*)(t + 8) = (u32)n;
    for (int i = 0; i < n; i++) {
        *(u16*)(t + 12 + 4 * i) = (u16)v[i].w;
        *(u16*)(t + 14 + 4 * i) = (u16)v[i].h;
    }
}

/* the ACPI RSDP: tag 14 (version 1, 20 bytes) or 15 (version 2+, 36) */
static __attribute__((unused)) void mb2_rsdp(mb2_t* m, const u8* rsdp) {
    int v2 = rsdp[15] >= 2;
    u32 n = v2 ? 36 : 20;
    u8* t = mb2_tag(m, v2 ? 15 : 14, 8 + n);
    for (u32 i = 0; i < n; i++) t[8 + i] = rsdp[i];
}

#endif
