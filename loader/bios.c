/* Banana Boot - the legacy BIOS loader (32-bit protected mode; the BIOS is
 * called through bios_int, which drops back to real mode for each call).
 *
 * From a CD, a USB stick or a hard disk alike it shows the menu, reads the
 * chosen kernel from the image's ISO9660 file system (/boot/kernel.bin or
 * /boot/kernel64.bin), loads it, sets the graphics mode the kernel asks for
 * (VBE), and starts it with the Multiboot2 information: the command line,
 * the memory map (E820), the framebuffer, the boot drive and the ACPI RSDP. */
#include "common.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u16 ds, es;
    u32 eflags;
} __attribute__((packed)) rm_regs_t;

extern void bios_int(int n, rm_regs_t* r);
extern void boot_kernel(u32 entry, u32 info) __attribute__((noreturn));

#define CF(r) ((r).eflags & 1u)

#define BUF_SEG    0x1000u                 /* 0x10000: VBE info, mode info, DAP, E820 */
#define DISK_BUF   ((u8*)0x20000)          /* 32 KiB */
#define DISK_SEG   0x2000u
#define MB2_INFO   ((u8*)0x30000)
#define BOUNCE_BLOCKS 16u                  /* 2048-byte blocks per read */

void* memcpy(void* d, const void* s, unsigned n) { u8* a = d; const u8* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned n) { u8* a = d; while (n--) *a++ = (u8)c; return d; }
static int c_memeq(const void* a, const void* b, u32 n) { const u8* x = a; const u8* y = b; while (n--) if (*x++ != *y++) return 0; return 1; }

/* ── the screen (VGA text mode, written directly) ─────────────────────── */
static volatile u16* const VGA = (volatile u16*)0xB8000;
static void put_at(int row, int col, const char* s, u8 attr) {
    for (; *s && col < 80; s++, col++) VGA[row * 80 + col] = (u16)((u8)*s | attr << 8);
}
static void fill_row(int row, u8 attr) { for (int c = 0; c < 80; c++) VGA[row * 80 + c] = (u16)(' ' | attr << 8); }
static void cls(void) { for (int r = 0; r < 25; r++) fill_row(r, 0x07); }

static int g_msg_row = 20;
static void message(const char* s) { fill_row(g_msg_row, 0x07); put_at(g_msg_row, 2, s, 0x0F); if (g_msg_row < 24) g_msg_row++; }

static void u32_str(u32 v, char* out) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = 0;
}

static void hide_cursor(void) {
    rm_regs_t r = {0};
    r.eax = 0x0100;
    r.ecx = 0x2000;
    bios_int(0x10, &r);
}

/* ── the keyboard and time ─────────────────────────────────────────────── */
static int key_ready(void) {
    rm_regs_t r = {0};
    r.eax = 0x0100;
    bios_int(0x16, &r);
    return !(r.eflags & 0x40);                 /* ZF clear: a key waits */
}
static u16 key_get(void) {
    rm_regs_t r = {0};
    bios_int(0x16, &r);
    return (u16)r.eax;                         /* AH scan code, AL character */
}
/* waits ms milliseconds (INT 15h, AH=86h; the timer runs meanwhile) */
static void sleep_ms(u32 ms) {
    rm_regs_t r = {0};
    u32 us = ms * 1000u;
    r.eax = 0x8600;
    r.ecx = us >> 16;
    r.edx = us & 0xFFFF;
    bios_int(0x15, &r);
    if (!CF(r)) return;
    /* no such BIOS call: the refresh bit of port 0x61 flips every 15.085 us */
    u8 last;
    __asm__ volatile("inb $0x61, %0" : "=a"(last));
    for (u32 flips = 0; flips < ms * 66u;) {
        u8 now;
        __asm__ volatile("inb $0x61, %0" : "=a"(now));
        if ((now ^ last) & 0x10) { flips++; last = now; }
    }
}

static void kbc_wait(void) { for (int i = 0; i < 100000; i++) { u8 s; __asm__ volatile("inb $0x64, %0" : "=a"(s)); if (!(s & 2)) return; } }

static void reboot(void) __attribute__((noreturn));
static void reboot(void) {
    message("Press a key to restart the computer.");
    key_get();
    kbc_wait();
    __asm__ volatile("outb %0, $0x64" : : "a"((u8)0xFE));   /* the keyboard controller's reset line */
    for (;;) __asm__ volatile("hlt");
}

static void fail(const char* what) __attribute__((noreturn));
static void fail(const char* what) {
    message(what);
    reboot();
}

/* ── the A20 line: checked, the keyboard controller as the last resort ─ */
static int a20_on(void) {
    volatile u32* lo = (volatile u32*)0x007E00, *hi = (volatile u32*)0x107E00;
    u32 keep = *lo;
    *lo = 0x12345678;
    *hi = 0x87654321;
    int on = *lo == 0x12345678;
    *lo = keep;
    return on;
}
static void enable_a20(void) {
    if (a20_on()) return;
    kbc_wait(); __asm__ volatile("outb %0, $0x64" : : "a"((u8)0xD1));
    kbc_wait(); __asm__ volatile("outb %0, $0x60" : : "a"((u8)0xDF));
    kbc_wait();
    if (!a20_on()) fail("Banana Boot: the A20 line cannot be enabled");
}

/* ── the boot drive ──────────────────────────────────────────────────── */
static u8  g_drive;
static int g_cd;                               /* 2048-byte sectors */

/* n 2048-byte blocks of the image from block blk into DISK_BUF */
static int read_raw(u32 blk, u32 n) {
    u8* dap = (u8*)(BUF_SEG * 16 + 0xF00);
    for (int tries = 0; tries < 3; tries++) {
        memset(dap, 0, 16);
        dap[0] = 0x10;
        *(u16*)(dap + 2) = (u16)(g_cd ? n : n * 4);
        *(u16*)(dap + 4) = 0;
        *(u16*)(dap + 6) = DISK_SEG;
        *(u32*)(dap + 8) = g_cd ? blk : blk * 4;
        rm_regs_t r = {0};
        r.eax = 0x4200;
        r.edx = g_drive;
        r.ds = BUF_SEG;
        r.esi = 0xF00;
        bios_int(0x13, &r);
        if (!CF(r)) return 0;
        rm_regs_t z = {0};                     /* reset the drive, try again */
        z.edx = g_drive;
        bios_int(0x13, &z);
    }
    return -1;
}

/* len bytes at byte offset off of the file starting at block lba, to dst */
static int read_file(u32 lba, u32 off, u32 len, u8* dst) {
    while (len) {
        u32 blk = lba + off / 2048, skip = off % 2048;
        u32 n = (skip + len + 2047) / 2048;
        if (n > BOUNCE_BLOCKS) n = BOUNCE_BLOCKS;
        if (read_raw(blk, n) != 0) return -1;
        u32 got = n * 2048 - skip;
        if (got > len) got = len;
        memcpy(dst, DISK_BUF + skip, got);
        dst += got;
        off += got;
        len -= got;
    }
    return 0;
}

/* ── ISO9660: a file by its path ("boot/kernel64.bin") ───────────────── */
static u8 g_sec[2048];

/* an ISO9660 name ("KERNEL64.BIN;1") against a path part, any case */
static int name_is(const u8* id, u32 idlen, const char* name, u32 nlen) {
    for (u32 i = 0; i < idlen; i++) if (id[i] == ';') { idlen = i; break; }
    while (idlen && id[idlen - 1] == '.') idlen--;
    if (idlen != nlen) return 0;
    for (u32 i = 0; i < nlen; i++) {
        char a = (char)id[i], b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

static int iso_find(const char* path, u32* lba, u32* size) {
    if (read_file(16, 0, 2048, g_sec) != 0) return -1;
    if (g_sec[0] != 1 || !c_memeq(g_sec + 1, "CD001", 5)) return -2;
    u32 dir = rd32(g_sec + 156 + 2), dsize = rd32(g_sec + 156 + 10);
    while (*path) {
        const char* e = path;
        while (*e && *e != '/') e++;
        u32 nlen = (u32)(e - path), found = 0;
        for (u32 off = 0; off < dsize && !found; off += 2048) {
            if (read_file(dir, off, 2048, g_sec) != 0) return -1;
            for (u32 p = 0; p < 2048 && g_sec[p];) {
                const u8* r = g_sec + p;
                if (name_is(r + 33, r[32], path, nlen)) {
                    dir = rd32(r + 2);
                    dsize = rd32(r + 10);
                    found = 1;
                    break;
                }
                p += r[0];
            }
        }
        if (!found) return -3;
        path = *e ? e + 1 : e;
    }
    *lba = dir;
    *size = dsize;
    return 0;
}

/* ── VBE: a linear 32-bit mode, the size the kernel asks for if there is one ─ */
typedef struct { u32 addr, pitch, w, h, bpp; u8 rp, rs, gp, gs, bp, bs; int ok; } fbinfo_t;

static vmode_t g_modes[MAX_VMODES];
static int     g_nmodes, g_vsel = -1;          /* the size chosen (-1: automatic) */

static int vbe_mode_info(u16 mode, u8* mi) {
    rm_regs_t r = {0};
    r.eax = 0x4F01;
    r.ecx = mode;
    r.es = BUF_SEG;
    r.edi = 0x200;
    bios_int(0x10, &r);
    if ((r.eax & 0xFFFF) != 0x004F) return -1;
    memcpy(mi, (u8*)(BUF_SEG * 16 + 0x200), 256);
    return 0;
}

/* the controller's mode list (VBE 2 info), 0xFFFF-terminated; 0 without VBE */
static const u16* vbe_list(u16* version) {
    u8* vi = (u8*)(BUF_SEG * 16);
    memset(vi, 0, 512);
    memcpy(vi, "VBE2", 4);
    rm_regs_t r = {0};
    r.eax = 0x4F00;
    r.es = BUF_SEG;
    r.edi = 0;
    bios_int(0x10, &r);
    if ((r.eax & 0xFFFF) != 0x004F || !c_memeq(vi, "VESA", 4)) return 0;
    *version = rd16(vi + 4);
    return (const u16*)((u32)rd16(vi + 16) * 16 + rd16(vi + 14));
}

/* a mode the kernel can use: graphics, linear, 32-bit direct colour */
static int vbe_usable(const u8* mi) {
    return (rd16(mi) & 0x91) == 0x91 && mi[25] == 32 && mi[27] == 6 && rd32(mi + 40);
}

/* the sizes for the menu (and the kernel's Settings) */
static void vbe_modes(void) {
    u16 version, modes[256];
    const u16* list = vbe_list(&version);
    if (!list) return;
    int n = 0;
    for (; n < 256 && list[n] != 0xFFFF; n++) modes[n] = list[n];   /* the next calls reuse the buffer */
    u8 mi[256];
    for (int i = 0; i < n; i++)
        if (vbe_mode_info(modes[i], mi) == 0 && vbe_usable(mi))
            vmode_add(g_modes, &g_nmodes, rd16(mi + 18), rd16(mi + 20), modes[i]);
}

static void set_video(u32 want_w, u32 want_h, fbinfo_t* fb) {
    fb->ok = 0;
    u16 version;
    const u16* list = vbe_list(&version);
    if (!list) return;
    u16 modes[256];
    int n = 0;
    for (; n < 256 && list[n] != 0xFFFF; n++) modes[n] = list[n];
    u8 mi[256];
    rm_regs_t r;
    int best = -1;
    u32 best_score = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        if (vbe_mode_info(modes[i], mi) != 0) continue;
        if (!vbe_usable(mi)) continue;
        u32 w = rd16(mi + 18), h = rd16(mi + 20);
        /* the size asked for; else the nearest (by area, a bigger one costs more) */
        u32 want = want_w * want_h, have = w * h;
        u32 score = have >= want ? (have - want) * 2 : want - have;
        if (w == want_w && h == want_h) score = 0;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best < 0 || vbe_mode_info(modes[best], mi) != 0) return;
    memset(&r, 0, sizeof(r));
    r.eax = 0x4F02;
    r.ebx = (u32)modes[best] | 0x4000;         /* linear framebuffer */
    bios_int(0x10, &r);
    if ((r.eax & 0xFFFF) != 0x004F) return;
    fb->addr = rd32(mi + 40);
    fb->w = rd16(mi + 18);
    fb->h = rd16(mi + 20);
    fb->bpp = 32;
    fb->pitch = version >= 0x300 && rd16(mi + 50) ? rd16(mi + 50) : rd16(mi + 16);
    int v3 = version >= 0x300 && mi[54];
    fb->rs = v3 ? mi[54] : mi[31]; fb->rp = v3 ? mi[55] : mi[32];
    fb->gs = v3 ? mi[56] : mi[33]; fb->gp = v3 ? mi[57] : mi[34];
    fb->bs = v3 ? mi[58] : mi[35]; fb->bp = v3 ? mi[59] : mi[36];
    fb->ok = 1;
}

/* ── the memory map (E820; E801 on old machines) ─────────────────────── */
static u32 memory_map(mb2_t* m, u32* upper_kb) {
    u8* t = mb2_tag(m, 6, 16);                  /* grows as entries come */
    *(u32*)(t + 8) = 24;                        /* entry size */
    u32 n = 0, cont = 0;
    *upper_kb = 0;
    do {
        u8* e = (u8*)(BUF_SEG * 16 + 0x400);
        memset(e, 0, 24);
        *(u32*)(e + 20) = 1;
        rm_regs_t r = {0};
        r.eax = 0xE820;
        r.edx = 0x534D4150;                     /* "SMAP" */
        r.ecx = 24;
        r.ebx = cont;
        r.es = BUF_SEG;
        r.edi = 0x400;
        bios_int(0x15, &r);
        if (CF(r) || r.eax != 0x534D4150) break;
        if (r.ecx < 24 || (*(u32*)(e + 20) & 1)) {   /* (ACPI 3 "ignore this entry" bit) */
            memcpy(m->base + m->off, e, 20);    /* the entries follow the tag's header */
            *(u32*)(m->base + m->off + 20) = 0;
            m->off += 24;
            n++;
            u64 base = *(u64*)e, len = *(u64*)(e + 8);
            if (*(u32*)(e + 16) == 1 && base <= 0x100000 && base + len > 0x100000) *upper_kb = (u32)((base + len - 0x100000) >> 10);
        }
        cont = r.ebx;
    } while (cont && n < 128);
    if (!n) {
        rm_regs_t r = {0};
        r.eax = 0xE801;
        bios_int(0x15, &r);
        if (CF(r)) fail("Banana Boot: the BIOS gives no memory map");
        u32 kb1 = (r.eax & 0xFFFF) ? (r.eax & 0xFFFF) : (r.ecx & 0xFFFF);
        u32 b64 = (r.ebx & 0xFFFF) ? (r.ebx & 0xFFFF) : (r.edx & 0xFFFF);
        u64 ents[3][3] = { { 0, 0x9F000, 1 }, { 0x100000, (u64)kb1 * 1024, 1 }, { 0x1000000, (u64)b64 * 65536, 1 } };
        for (int i = 0; i < 3; i++) {
            u8* d = m->base + m->off;
            *(u64*)d = ents[i][0];
            *(u64*)(d + 8) = ents[i][1];
            *(u32*)(d + 16) = (u32)ents[i][2];
            *(u32*)(d + 20) = 0;
            m->off += 24;
        }
        *upper_kb = kb1 + b64 * 64;
        n = 3;
    }
    *(u32*)(t + 4) = 16 + n * 24;               /* the tag's size, entries included */
    return n;
}

/* ── the ACPI RSDP: the EBDA's first KiB, then 0xE0000-0xFFFFF ─────────── */
static const u8* find_rsdp(void) {
    u32 ebda = (u32)*(volatile u16*)0x40E << 4;
    u32 ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int k = 0; k < 2; k++) {
        if (k == 0 && (ebda < 0x80000 || ebda > 0x9FC00)) continue;
        for (u32 a = ranges[k][0]; a < ranges[k][1]; a += 16) {
            const u8* p = (const u8*)a;
            if (!c_memeq(p, "RSD PTR ", 8)) continue;
            u8 sum = 0;
            for (int i = 0; i < 20; i++) sum = (u8)(sum + p[i]);
            if (!sum) return p;
        }
    }
    return 0;
}

/* The line `install` rewrites in the disk's copy of this file (see common.h). */
static const char MEDIUM_LINE[] = "set banana_medium=live-cd";
/* ... and the screen size (common.h): the value is padded to 12 characters */
static const char VIDEO_LINE[] = "set banana_video=auto        ";

/* ── the menu: the kernels, then what else the BIOS can do ─────────────── */
#define ENTRY_NEXT    (KERNEL_ENTRIES)       /* the next boot device (INT 18h) */
#define ENTRY_RESTART (KERNEL_ENTRIES + 1)
#define ENTRIES       (KERNEL_ENTRIES + 2)

static const char* label(int i) {
    if (i == ENTRY_NEXT) return "Boot from the next device";
    if (i == ENTRY_RESTART) return "Restart the computer";
    return entry_label(i);
}
static int row_of(int i) { return i < KERNEL_ENTRIES ? 4 + i : 5 + i; }   /* a gap before the extras */

static void draw_menu(int sel, int secs) {
    fill_row(1, 0x1E);
    put_at(1, 2, "Banana Boot", 0x1E);
    put_at(1, 62, g_cd ? "(CD/DVD)" : "(disk / USB)", 0x1B);
    for (int i = 0; i < ENTRIES; i++) {
        int r = row_of(i);
        fill_row(r, 0x07);
        put_at(r, 4, label(i), i == sel ? 0x70 : 0x07);
        if (i == sel) { VGA[r * 80 + 3] = (u16)(' ' | 0x70 << 8); for (int c = 4 + (int)c_strlen(label(i)); c < 60; c++) VGA[r * 80 + c] = (u16)(' ' | 0x70 << 8); }
    }
    char vl[80];
    vmode_label(vl, g_modes, g_nmodes, g_vsel);
    fill_row(13, 0x07);
    put_at(13, 4, vl, 0x07);
    fill_row(15, 0x07);
    put_at(15, 4, g_nmodes ? "Up / Down choose, Left / Right: screen size, Enter starts."
                           : "Up / Down choose, Enter starts.", 0x08);
    fill_row(16, 0x07);
    if (secs >= 0) {
        char line[64] = "Starting the highlighted entry in ";
        char n[12];
        u32_str((u32)secs, n);
        c_strcat(line, n);
        c_strcat(line, " s.");
        put_at(16, 4, line, 0x08);
    }
}

static int menu(int def) {
    int sel = def, ticks = 30;                  /* 3 s, by 100 ms */
    draw_menu(sel, 3);
    for (;;) {
        if (key_ready()) {
            u16 k = key_get();
            ticks = -1;                         /* a key: no more countdown */
            u8 scan = (u8)(k >> 8), ch = (u8)k;
            if (scan == 0x48) sel = (sel + ENTRIES - 1) % ENTRIES;
            else if (scan == 0x50) sel = (sel + 1) % ENTRIES;
            else if (scan == 0x4B) g_vsel = vmode_step(g_vsel, g_nmodes, -1);
            else if (scan == 0x4D) g_vsel = vmode_step(g_vsel, g_nmodes, 1);
            else if (ch == '\r') return sel;
            draw_menu(sel, -1);
            continue;
        }
        if (ticks >= 0) {
            if (ticks == 0) return sel;
            sleep_ms(100);
            ticks--;
            if (ticks % 10 == 0) draw_menu(sel, ticks / 10);
        } else {
            sleep_ms(20);
        }
    }
}

/* ── loading the kernel ───────────────────────────────────────────────── */
static u8 g_head[8192];

void loader_main(u32 drive, u32 cd) {
    g_drive = (u8)drive;
    g_cd = (int)cd;
    hide_cursor();
    cls();
    enable_a20();
    if (!g_cd) {
        /* a drive with 2048-byte sectors though started from its MBR (some CD BIOSes) */
        u8* dp = (u8*)(BUF_SEG * 16 + 0xE00);
        memset(dp, 0, 0x1E);
        *(u16*)dp = 0x1E;
        rm_regs_t r = {0};
        r.eax = 0x4800;
        r.edx = g_drive;
        r.ds = BUF_SEG;
        r.esi = 0xE00;
        bios_int(0x13, &r);
        if (!CF(r) && rd16(dp + 24) == 2048) g_cd = 1;
    }

    /* the screen sizes, and the one this disk is set to (its line may have
     * been rewritten: read it through an opaque pointer, never folded) */
    vbe_modes();
    {
        const char* vl = VIDEO_LINE;
        __asm__ volatile("" : "+r"(vl));
        u32 vw, vh;
        video_value(vl, sizeof(VIDEO_LINE) - 1, &vw, &vh);
        g_vsel = vw ? vmode_find(g_modes, g_nmodes, vw, vh) : -1;
    }

    int entry = menu(has_long_mode() ? 0 : 2);
    cls();
    if (entry == ENTRY_NEXT) {
        /* the BIOS boots the next device in its boot order (or says there is none) */
        rm_regs_t r = {0};
        bios_int(0x18, &r);
        fail("Banana Boot: the BIOS has no other device to boot");
    }
    if (entry == ENTRY_RESTART) {
        kbc_wait();
        __asm__ volatile("outb %0, $0x64" : : "a"((u8)0xFE));
        for (;;) __asm__ volatile("hlt");
    }
    g_msg_row = 2;
    message(entry_is64(entry) ? "Loading Banana OS (64-bit)..." : "Loading Banana OS (32-bit)...");

    u32 lba, size;
    int rc = iso_find(entry_is64(entry) ? "boot/kernel64.bin" : "boot/kernel.bin", &lba, &size);
    if (rc == -1) fail("Banana Boot: cannot read the boot drive");
    if (rc != 0) fail("Banana Boot: the kernel is not on this drive (/boot/kernel*.bin)");
    u32 hlen = size < sizeof(g_head) ? size : sizeof(g_head);
    if (read_file(lba, 0, hlen, g_head) != 0) fail("Banana Boot: cannot read the kernel");
    seg_t segs[16];
    u64 entry_addr;
    int ns = elf_segments(g_head, hlen, segs, 16, &entry_addr);
    if (ns <= 0) fail("Banana Boot: the kernel file is not an ELF program");
    for (int i = 0; i < ns; i++) {
        seg_t* s = &segs[i];
        if (s->paddr < 0x100000 || s->paddr + s->memsz > 0xF0000000ull || s->offset + s->filesz > size)
            fail("Banana Boot: the kernel wants memory it cannot have");
        if (s->filesz && read_file(lba, (u32)s->offset, (u32)s->filesz, (u8*)(u32)s->paddr) != 0)
            fail("Banana Boot: cannot read the kernel");
        memset((u8*)(u32)(s->paddr + s->filesz), 0, (u32)(s->memsz - s->filesz));
    }

    /* the Multiboot2 information */
    mb2_t m;
    mb2_begin(&m, MB2_INFO);
    char cmd[96];
    char medium[16];
    medium_value(MEDIUM_LINE, sizeof(MEDIUM_LINE) - 1, medium, sizeof(medium));
    build_cmdline(cmd, entry, medium);
    mb2_string(&m, 1, cmd);
    mb2_string(&m, 2, "Banana Boot");
    u8* bootdev = mb2_tag(&m, 5, 20);
    *(u32*)(bootdev + 8) = g_drive;
    *(u32*)(bootdev + 12) = 0xFFFFFFFFu;
    *(u32*)(bootdev + 16) = 0xFFFFFFFFu;
    u8* basic = mb2_tag(&m, 4, 16);
    u32 upper = 0;
    memory_map(&m, &upper);
    rm_regs_t r = {0};
    bios_int(0x12, &r);
    *(u32*)(basic + 8) = r.eax & 0xFFFF;
    *(u32*)(basic + 12) = upper;
    const u8* rsdp = find_rsdp();
    if (rsdp) mb2_rsdp(&m, rsdp);
    mb2_modes(&m, g_modes, g_nmodes);
    /* the graphics mode last: no more text after it */
    u32 fw = 800, fh = 600, fd = 32;
    mb2_fb_request(g_head, hlen, &fw, &fh, &fd);
    if (g_vsel >= 0) { fw = g_modes[g_vsel].w; fh = g_modes[g_vsel].h; }
    fbinfo_t fb;
    set_video(fw ? fw : 800, fh ? fh : 600, &fb);
    if (fb.ok) mb2_framebuffer(&m, fb.addr, fb.pitch, fb.w, fb.h, fb.bpp, fb.rp, fb.rs, fb.gp, fb.gs, fb.bp, fb.bs);
    mb2_end(&m);
    boot_kernel((u32)entry_addr, (u32)MB2_INFO);
}
