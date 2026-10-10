/* Banana Boot - the UEFI loader: BOOTX64.EFI for 64-bit firmware, and the
 * same source as BOOTIA32.EFI for 32-bit UEFI firmware (tablets and early
 * UEFI PCs, often with a 64-bit processor).
 *
 * It shows the menu, reads the chosen kernel from its own FAT partition
 * (/boot/kernel.bin or /boot/kernel64.bin, next to it in the image's EFI
 * system partition), copies it where its ELF headers say, hands the
 * firmware's memory map, the GOP framebuffer and the ACPI and EFI tables
 * to it as Multiboot2 information, leaves the firmware (ExitBootServices),
 * and starts it as GRUB did - in 32-bit protected mode with paging off,
 * through tramp.S (down from 64-bit long mode) or tramp32.S.
 *
 * The menu also offers the next boot option, the firmware's settings
 * screen, and - in Secure Boot's setup mode - enrolling Banana OS's key.
 * Under Secure Boot it starts only the kernels it was built with (their
 * SHA-256 is in it: kernel_hashes.h), so the chain of trust reaches them.
 *
 * Built position-independent with no relocations (efi.lds / efi32.lds;
 * the Makefile checks): it runs wherever the firmware puts it. */
#include "efi.h"
#include "sha256.h"
#include "kernel_hashes.h"

static EFI_SYSTEM_TABLE*  ST;
static EFI_BOOT_SERVICES* BS;
static EFI_RUNTIME_SERVICES* RT;
static EFI_HANDLE g_image;
static EFI_FILE* g_root;                       /* the EFI partition we came from */

void* memcpy(void* d, const void* s, unsigned long n) { u8* a = d; const u8* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { u8* a = d; while (n--) *a++ = (u8)c; return d; }

static const EFI_GUID LOADED_IMAGE_GUID = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID SIMPLE_FS_GUID    = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID FILE_INFO_GUID    = { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID GOP_GUID          = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static const EFI_GUID ACPI20_GUID       = { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static const EFI_GUID ACPI10_GUID       = { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
static const EFI_GUID GLOBAL_VAR_GUID   = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static const EFI_GUID IMAGE_SECURITY_DB_GUID = { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } };

#ifdef __x86_64__
#define FIRMWARE_NAME "UEFI, 64-bit"
#else
#define FIRMWARE_NAME "UEFI, 32-bit"
#endif

static int guid_eq(const EFI_GUID* a, const EFI_GUID* b) {
    const u8* x = (const u8*)a; const u8* y = (const u8*)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* ── text ─────────────────────────────────────────────────────────────── */
static void wide(const char* s, CHAR16* out, int cap) {
    int n = 0;
    while (*s && n < cap - 1) out[n++] = (CHAR16)(u8)*s++;
    out[n] = 0;
}

static void print_at(int col, int row, const char* s, int attr) {
    CHAR16 buf[100];
    wide(s, buf, 100);
    if (col >= 0) ST->ConOut->SetCursorPosition(ST->ConOut, (UINTN)col, (UINTN)row);
    ST->ConOut->SetAttribute(ST->ConOut, (UINTN)attr);
    ST->ConOut->OutputString(ST->ConOut, buf);
}
static void print(const char* s) { print_at(-1, 0, s, 0x07); }

static void u32_str(u32 v, char* out) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = 0;
}

static EFI_INPUT_KEY wait_key(void) {
    EFI_INPUT_KEY k;
    while (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) != EFI_SUCCESS) BS->Stall(20000);
    return k;
}

static void halt(const char* why) __attribute__((noreturn));
static void halt(const char* why) {
    print("\r\nBanana Boot: ");
    print(why);
    print("\r\nPress a key to restart the computer.\r\n");
    wait_key();
    RT->ResetSystem(EfiResetCold, 0, 0, 0);
    for (;;) BS->Stall(1000000);
}

/* ── firmware variables ──────────────────────────────────────────────── */
static int var_u8(const char* name, const EFI_GUID* g, u8* out) {
    CHAR16 n[32];
    wide(name, n, 32);
    UINTN sz = 1;
    return RT->GetVariable(n, g, 0, &sz, out) == EFI_SUCCESS && sz == 1;
}
static u64 var_u64(const char* name) {
    CHAR16 n[32];
    wide(name, n, 32);
    u64 v = 0;
    UINTN sz = 8;
    if (RT->GetVariable(n, &GLOBAL_VAR_GUID, 0, &sz, &v) != EFI_SUCCESS) return 0;
    return v;
}

static int g_secure;                           /* Secure Boot is enforcing */
static int g_setup;                            /* no platform key yet: keys can be enrolled */
static int g_fw_ui;                            /* the firmware can be asked to open its settings */
static int g_enroll;                           /* setup mode, and Banana OS's signed keys are here */

/* the screen: GOP's modes, and the one chosen (-1: as the firmware set it) */
typedef EFI_STATUS (EFIAPI *gop_query_t)(EFI_GOP*, u32, UINTN*, EFI_GOP_MODE_INFO**);
typedef EFI_STATUS (EFIAPI *gop_set_t)(EFI_GOP*, u32);
static EFI_GOP* g_gop;
static vmode_t  g_modes[MAX_VMODES];
static int      g_nmodes, g_vsel = -1;

/* ── files on our partition ──────────────────────────────────────────── */
static u8* read_file(const char* path, UINTN* size, int must) {
    EFI_FILE* f;
    CHAR16 p[64];
    wide(path, p, 64);
    if (g_root->Open(g_root, &f, p, 1, 0) != EFI_SUCCESS) {
        if (must) halt("a file is missing from my partition");
        return 0;
    }
    u8 info[512];
    UINTN isz = sizeof(info);
    if (f->GetInfo(f, &FILE_INFO_GUID, &isz, info) != EFI_SUCCESS) { f->Close(f); if (must) halt("cannot read a file"); return 0; }
    *size = (UINTN)((EFI_FILE_INFO*)info)->FileSize;
    u8* buf;
    if (BS->AllocatePool(EfiLoaderData, *size + 1, (void**)&buf) != EFI_SUCCESS) halt("out of memory");
    UINTN got = *size;
    if (f->Read(f, &got, buf) != EFI_SUCCESS || got != *size) { f->Close(f); if (must) halt("cannot read a file"); return 0; }
    f->Close(f);
    buf[*size] = 0;
    return buf;
}

static void open_partition(void) {
    EFI_LOADED_IMAGE* li;
    EFI_SIMPLE_FILE_SYSTEM* fs;
    if (BS->HandleProtocol(g_image, &LOADED_IMAGE_GUID, (void**)&li) != EFI_SUCCESS) halt("no loaded-image protocol");
    if (BS->HandleProtocol(li->DeviceHandle, &SIMPLE_FS_GUID, (void**)&fs) != EFI_SUCCESS) halt("cannot read my partition");
    if (fs->OpenVolume(fs, &g_root) != EFI_SUCCESS) halt("cannot open my partition");
}

/* ── the menu: the kernels, then what else the firmware can do ───────── */
enum { X_NEXT, X_FIRMWARE, X_ENROLL, X_RESTART };
static int g_extra[4], g_nextra;               /* the extra entries shown, in order */

static const char* extra_label(int x) {
    switch (x) {
    case X_NEXT:     return "Boot the next boot option";
    case X_FIRMWARE: return "UEFI firmware settings";
    case X_ENROLL:   return "Enroll Banana OS's Secure Boot keys";
    default:         return "Restart the computer";
    }
}

static void draw_menu(int sel, int secs) {
    print_at(2, 1, "Banana Boot  (" FIRMWARE_NAME ")", 0x0E);
    print_at(48, 1, g_secure ? "Secure Boot: on   " : g_setup ? "Secure Boot: setup" : "Secure Boot: off  ", g_secure ? 0x0A : 0x08);
    int n = KERNEL_ENTRIES + g_nextra;
    for (int i = 0; i < n; i++) {
        char line[60] = " ";                    /* the label in a bar 56 wide */
        c_strcat(line, i < KERNEL_ENTRIES ? entry_label(i) : extra_label(g_extra[i - KERNEL_ENTRIES]));
        for (u32 k = c_strlen(line); k < 56; k++) line[k] = ' ';
        line[56] = 0;
        print_at(3, i < KERNEL_ENTRIES ? 3 + i : 4 + i, line, i == sel ? 0x70 : 0x07);
    }
    int r = 5 + n;
    char vl[80];
    vmode_label(vl, g_modes, g_nmodes, g_vsel);
    for (u32 k = c_strlen(vl); k < 64; k++) vl[k] = ' ';
    vl[64] = 0;
    print_at(4, r, vl, 0x07);
    r += 2;
    print_at(4, r, g_nmodes ? "Up / Down choose, Left / Right: screen size, Enter starts."
                            : "Up / Down choose, Enter starts.", 0x08);
    print_at(4, r + 1, "                                                ", 0x07);
    if (secs >= 0) {
        char line[64] = "Starting the highlighted entry in ";
        char num[12];
        u32_str((u32)secs, num);
        c_strcat(line, num);
        c_strcat(line, " s.");
        print_at(4, r + 1, line, 0x08);
    }
}

static int menu(int def) {
    int sel = def, ticks = 30, n = KERNEL_ENTRIES + g_nextra;   /* 3 s, by 100 ms */
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->EnableCursor(ST->ConOut, 0);
    draw_menu(sel, 3);
    for (;;) {
        EFI_INPUT_KEY k;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) == EFI_SUCCESS) {
            ticks = -1;
            if (k.ScanCode == 1) sel = (sel + n - 1) % n;
            else if (k.ScanCode == 2) sel = (sel + 1) % n;
            else if (k.ScanCode == 4) g_vsel = vmode_step(g_vsel, g_nmodes, -1);
            else if (k.ScanCode == 3) g_vsel = vmode_step(g_vsel, g_nmodes, 1);
            else if (k.UnicodeChar == '\r') return sel;
            draw_menu(sel, -1);
            continue;
        }
        if (ticks >= 0) {
            if (ticks == 0) return sel;
            BS->Stall(100000);
            ticks--;
            if (ticks % 10 == 0) draw_menu(sel, ticks / 10);
        } else {
            BS->Stall(20000);
        }
    }
}

/* the firmware opens its settings at the next start (OsIndications bit 0) */
static void firmware_settings(void) {
    u64 ind = var_u64("OsIndications") | 1;
    CHAR16 n[16];
    wide("OsIndications", n, 16);
    if (RT->SetVariable(n, &GLOBAL_VAR_GUID, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS, 8, &ind) != EFI_SUCCESS)
        halt("the firmware refused to open its settings");
    RT->ResetSystem(EfiResetCold, 0, 0, 0);
    halt("the firmware did not restart");
}

/* Setup mode (no platform key): Banana OS's keys become the computer's -
 * its certificate is added to db (what may boot) and KEK, then becomes the
 * platform key (PK), which ends setup mode. The files are signed at build
 * time (the Makefile, efitools); the firmware checks them. */
static void enroll_keys(void) {
    ST->ConOut->ClearScreen(ST->ConOut);
    print_at(2, 1, "Enroll Banana OS's Secure Boot keys", 0x0E);
    print_at(2, 3, "This computer's Secure Boot is in setup mode: it has no platform key.", 0x07);
    print_at(2, 4, "Enrolling makes Banana OS's certificate the platform key (PK), and adds it", 0x07);
    print_at(2, 5, "to KEK and to the list of what may start (db) - what db holds stays.", 0x07);
    print_at(2, 6, "Secure Boot then starts Banana OS, and whatever else db allows.", 0x07);
    print_at(2, 9, "Press Y to enroll, any other key to go back.", 0x0F);
    EFI_INPUT_KEY k = wait_key();
    if (k.UnicodeChar != 'y' && k.UnicodeChar != 'Y') return;
    for (int i = 0; i < 3; i++) {
        /* (no table of strings: it would need relocations) */
        const char* file = i == 0 ? "\\EFI\\BananaOS\\db.auth" : i == 1 ? "\\EFI\\BananaOS\\KEK.auth" : "\\EFI\\BananaOS\\PK.auth";
        const char* var = i == 0 ? "db" : i == 1 ? "KEK" : "PK";
        /* NV BS RT, time-authenticated; db and KEK appended to; PK last: it ends setup mode */
        u32 attr = i < 2 ? 0x67 : 0x27;
        UINTN sz;
        u8* data = read_file(file, &sz, 1);
        CHAR16 n[8];
        wide(var, n, 8);
        EFI_STATUS s = RT->SetVariable(n, i == 0 ? &IMAGE_SECURITY_DB_GUID : &GLOBAL_VAR_GUID, attr, sz, data);
        BS->FreePool(data);
        if (s != EFI_SUCCESS) {
            print_at(2, 11, "The firmware refused the key: ", 0x0C);
            print(var);
            halt("the keys were not enrolled");
        }
    }
    print_at(2, 11, "Done. Turn Secure Boot on in the firmware settings if it is not on by itself.", 0x0A);
    print_at(2, 12, "Press a key to restart.", 0x07);
    wait_key();
    RT->ResetSystem(EfiResetCold, 0, 0, 0);
}

/* ── the memory map: UEFI's, as Multiboot2 types, sorted and merged ──── */
static u32 mb2_type(u32 efi) {
    switch (efi) {
    case EfiLoaderCode: case EfiLoaderData: case EfiBootServicesCode: case EfiBootServicesData:
    case EfiConventionalMemory: return 1;             /* free once the firmware is left */
    case EfiACPIReclaimMemory: return 3;
    case EfiACPIMemoryNVS: return 4;
    case EfiUnusableMemory: return 5;
    default: return 2;                                /* runtime services, MMIO, reserved */
    }
}

/* the map from GetMemoryMap as the mmap tag (tag 6) and basic memory (tag 4) */
static void map_tags(mb2_t* m, u8* map, UINTN size, UINTN dsize) {
    UINTN n = size / dsize;
    /* sorted by address (insertion sort: a few hundred entries) */
    for (UINTN i = 1; i < n; i++)
        for (UINTN j = i; j > 0; j--) {
            EFI_MEMORY_DESCRIPTOR* a = (EFI_MEMORY_DESCRIPTOR*)(map + (j - 1) * dsize);
            EFI_MEMORY_DESCRIPTOR* b = (EFI_MEMORY_DESCRIPTOR*)(map + j * dsize);
            if (a->PhysicalStart <= b->PhysicalStart) break;
            EFI_MEMORY_DESCRIPTOR t = *a; *a = *b; *b = t;
        }
    u8* basic = mb2_tag(m, 4, 16);
    u8* t = mb2_tag(m, 6, 16);
    *(u32*)(t + 8) = 24;
    u32 count = 0;
    u8* last = 0;
    u64 upper = 0, lower = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(map + i * dsize);
        u64 base = d->PhysicalStart, len = d->NumberOfPages * 4096;
        u32 type = mb2_type(d->Type);
        if (last && *(u32*)(last + 16) == type && *(u64*)last + *(u64*)(last + 8) == base) {
            *(u64*)(last + 8) += len;                 /* continues the one before */
        } else {
            last = m->base + m->off;
            *(u64*)last = base;
            *(u64*)(last + 8) = len;
            *(u32*)(last + 16) = type;
            *(u32*)(last + 20) = 0;
            m->off += 24;
            count++;
        }
    }
    *(u32*)(t + 4) = 16 + count * 24;
    for (u8* e = t + 16; e < t + 16 + count * 24; e += 24) {
        u64 b = *(u64*)e, l = *(u64*)(e + 8);
        if (*(u32*)(e + 16) != 1) continue;
        if (b == 0) lower = l > 0xA0000 ? 0xA0000 : l;
        if (b <= 0x100000 && b + l > 0x100000) upper = b + l - 0x100000;
    }
    *(u32*)(basic + 8) = (u32)(lower >> 10);
    *(u32*)(basic + 12) = (u32)(upper >> 10);
}

/* hidden: reached relative to the code - through the GOT they would need relocations */
#define HIDDEN __attribute__((visibility("hidden")))
extern HIDDEN u8 tramp_start[], tramp_end[], tramp_gdtr_base[], tramp_gdt[];

#define PTR(x) ((void*)(UINTN)(x))                     /* a physical address as a pointer */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    ST = st;
    BS = st->BootServices;
    RT = st->RuntimeServices;
    g_image = image;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    open_partition();

    /* what the firmware can do, for the menu */
    u8 v;
    g_secure = var_u8("SecureBoot", &GLOBAL_VAR_GUID, &v) && v == 1;
    g_setup = var_u8("SetupMode", &GLOBAL_VAR_GUID, &v) && v == 1;
    g_fw_ui = (var_u64("OsIndicationsSupported") & 1) != 0;
    if (g_setup) {
        UINTN sz;
        u8* f = read_file("\\EFI\\BananaOS\\PK.auth", &sz, 0);
        if (f) { g_enroll = 1; BS->FreePool(f); }
    }
    /* the screen sizes (32-bit colour with a framebuffer), and the one set
     * in medium.cfg */
    UINTN msz = 0;
    u8* mfile = read_file("\\EFI\\BananaOS\\medium.cfg", &msz, 0);
    if (BS->LocateProtocol(&GOP_GUID, 0, (void**)&g_gop) == EFI_SUCCESS && g_gop->Mode) {
        for (u32 i = 0; i < g_gop->Mode->MaxMode && i < 256; i++) {
            UINTN isz;
            EFI_GOP_MODE_INFO* mi;
            if (((gop_query_t)g_gop->QueryMode)(g_gop, i, &isz, &mi) != EFI_SUCCESS) continue;
            if (mi->PixelFormat != 3) vmode_add(g_modes, &g_nmodes, mi->HorizontalResolution, mi->VerticalResolution, i);
            BS->FreePool(mi);
        }
        u32 vw, vh;
        video_value(mfile ? (const char*)mfile : "", mfile ? (u32)msz : 0, &vw, &vh);
        g_vsel = vw ? vmode_find(g_modes, g_nmodes, vw, vh) : -1;
    } else {
        g_gop = 0;
    }

    g_extra[g_nextra++] = X_NEXT;
    if (g_fw_ui) g_extra[g_nextra++] = X_FIRMWARE;
    if (g_enroll) g_extra[g_nextra++] = X_ENROLL;
    g_extra[g_nextra++] = X_RESTART;

    int entry;
    for (;;) {
        entry = menu(has_long_mode() ? 0 : 2);
        if (entry < KERNEL_ENTRIES) break;
        int x = g_extra[entry - KERNEL_ENTRIES];
        ST->ConOut->ClearScreen(ST->ConOut);
        if (x == X_NEXT) return EFI_ABORTED;          /* the firmware tries the next boot option */
        if (x == X_FIRMWARE) firmware_settings();
        if (x == X_ENROLL) enroll_keys();
        if (x == X_RESTART) RT->ResetSystem(EfiResetCold, 0, 0, 0);
    }
    if (g_gop && g_vsel >= 0 && g_modes[g_vsel].id != g_gop->Mode->Mode)
        ((gop_set_t)g_gop->SetMode)(g_gop, g_modes[g_vsel].id);    /* refused: the size stays */
    ST->ConOut->ClearScreen(ST->ConOut);
    int is64 = entry_is64(entry);
    print(is64 ? "Loading Banana OS (64-bit)...\r\n" : "Loading Banana OS (32-bit)...\r\n");

    UINTN fsize;
    u8* file = read_file(is64 ? "\\boot\\kernel64.bin" : "\\boot\\kernel.bin", &fsize, 1);
    /* Secure Boot vouched for this program; it vouches for the kernel */
    u8 sum[32];
    sha256(file, fsize, sum);
    const u8* want = is64 ? KERNEL64_SHA256 : KERNEL32_SHA256;
    int same = 1;
    for (int i = 0; i < 32; i++) if (sum[i] != want[i]) same = 0;
    if (!same && g_secure) halt("Secure Boot: the kernel is not the one this loader was built with");

    seg_t segs[16];
    u64 kentry;
    int ns = elf_segments(file, fsize > 65536 ? 65536 : (u32)fsize, segs, 16, &kentry);
    if (ns <= 0) halt("the kernel file is not an ELF program");
    u64 lo = ~0ull, hi = 0;
    for (int i = 0; i < ns; i++) {
        if (segs[i].offset + segs[i].filesz > fsize) halt("the kernel file is cut short");
        if (segs[i].paddr < lo) lo = segs[i].paddr;
        if (segs[i].paddr + segs[i].memsz > hi) hi = segs[i].paddr + segs[i].memsz;
    }
    if (lo < 0x100000 || hi > 0xF0000000ull || kentry >= 0x100000000ull) halt("the kernel wants memory it cannot have");

    /* The kernel's place: reserved now if the firmware lets us (then copied
     * right away); some firmware keeps its own data there until it is left -
     * then the kernel is copied after ExitBootServices, everything else of
     * ours being elsewhere (checked). */
    lo &= ~0xFFFull;
    EFI_PHYSICAL_ADDRESS kaddr = lo;
    UINTN kpages = (UINTN)((hi - lo + 4095) / 4096);
    int reserved = BS->AllocatePages(AllocateAddress, EfiLoaderData, kpages, &kaddr) == EFI_SUCCESS;

    /* the Multiboot2 information and the trampoline: below 4 GiB */
    EFI_PHYSICAL_ADDRESS info = 0xFFFFFFFFull, tramp = 0xFFFFFFFFull;
    if (BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 16, &info) != EFI_SUCCESS ||
        BS->AllocatePages(AllocateMaxAddress, EfiLoaderCode, 1, &tramp) != EFI_SUCCESS)
        halt("out of memory below 4 GiB");
    if (!reserved) {
        EFI_LOADED_IMAGE* li;
        BS->HandleProtocol(image, &LOADED_IMAGE_GUID, (void**)&li);
        u64 sp = (u64)(UINTN)&sp;
        u64 r[5][2] = { { (u64)(UINTN)file, fsize }, { info, 65536 }, { tramp, 4096 },
                        { (u64)(UINTN)li->ImageBase, li->ImageSize }, { sp - 65536, 65536 + 4096 } };
        for (int i = 0; i < 5; i++)
            if (r[i][0] < hi && r[i][0] + r[i][1] > lo) halt("the kernel's place is taken by the firmware");
    }
    memcpy(PTR(tramp), tramp_start, (UINTN)(tramp_end - tramp_start));
    *(u64*)PTR(tramp + (UINTN)(tramp_gdtr_base - tramp_start)) = tramp + (u64)(tramp_gdt - tramp_start);

    mb2_t m;
    mb2_begin(&m, (u8*)PTR(info));
    char medium[16], cmd[96];
    medium_value(mfile ? (const char*)mfile : "", mfile ? (u32)msz : 0, medium, sizeof(medium));
    build_cmdline(cmd, entry, medium);
    mb2_string(&m, 1, cmd);
    mb2_string(&m, 2, "Banana Boot (" FIRMWARE_NAME ")");
#ifdef __x86_64__
    u8* t = mb2_tag(&m, 12, 16);                     /* the EFI system table: the kernel knows it is UEFI */
    *(u64*)(t + 8) = (u64)(UINTN)st;
#else
    u8* t = mb2_tag(&m, 11, 12);
    *(u32*)(t + 8) = (u32)(UINTN)st;
#endif
    /* the ACPI RSDP: the ACPI 2 one, else the ACPI 1 one */
    const u8* rsdp = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* c = &st->ConfigurationTable[i];
        if (guid_eq(&c->VendorGuid, &ACPI20_GUID)) { rsdp = (const u8*)c->VendorTable; break; }
        if (guid_eq(&c->VendorGuid, &ACPI10_GUID)) rsdp = (const u8*)c->VendorTable;
    }
    if (rsdp) mb2_rsdp(&m, rsdp);
    mb2_modes(&m, g_modes, g_nmodes);
    EFI_GOP* gop;
    if (BS->LocateProtocol(&GOP_GUID, 0, (void**)&gop) == EFI_SUCCESS && gop->Mode && gop->Mode->Info &&
        gop->Mode->Info->PixelFormat != 3) {
        EFI_GOP_MODE_INFO* gi = gop->Mode->Info;
        u8 rp = 16, gp = 8, bp = 0;
        if (gi->PixelFormat == 0) { rp = 0; bp = 16; }
        if (gi->PixelFormat == 2) {
            u32 masks[3] = { gi->RedMask, gi->GreenMask, gi->BlueMask };
            u8 pos[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; k++) { u32 mv = masks[k]; while (mv && !(mv & 1)) { mv >>= 1; pos[k]++; } }
            rp = pos[0]; gp = pos[1]; bp = pos[2];
        }
        mb2_framebuffer(&m, gop->Mode->FrameBufferBase, gi->PixelsPerScanLine * 4, gi->HorizontalResolution,
                        gi->VerticalResolution, 32, rp, 8, gp, 8, bp, 8);
    }

    if (reserved) {
        for (int i = 0; i < ns; i++) {
            memcpy(PTR(segs[i].paddr), file + segs[i].offset, (UINTN)segs[i].filesz);
            memset(PTR(segs[i].paddr + segs[i].filesz), 0, (UINTN)(segs[i].memsz - segs[i].filesz));
        }
    }

    /* the memory map last, then leave the firmware (again if the map changed meanwhile) */
    UINTN size = 0, key, dsize;
    u32 dver;
    BS->GetMemoryMap(&size, 0, &key, &dsize, &dver);
    size += 64 * 64;
    u8* map;
    if (BS->AllocatePool(EfiLoaderData, size, (void**)&map) != EFI_SUCCESS) halt("out of memory");
    u32 before_map = m.off;
    for (int tries = 0;; tries++) {
        UINTN sz = size;
        if (BS->GetMemoryMap(&sz, (EFI_MEMORY_DESCRIPTOR*)map, &key, &dsize, &dver) != EFI_SUCCESS) halt("no memory map");
        m.off = before_map;
        map_tags(&m, map, sz, dsize);
        mb2_end(&m);
        if (BS->ExitBootServices(image, key) == EFI_SUCCESS) break;
        if (tries == 3) halt("cannot leave the firmware");
    }

    /* the firmware is gone: no more calls to it */
    if (!reserved) {
        for (int i = 0; i < ns; i++) {
            memcpy(PTR(segs[i].paddr), file + segs[i].offset, (UINTN)segs[i].filesz);
            memset(PTR(segs[i].paddr + segs[i].filesz), 0, (UINTN)(segs[i].memsz - segs[i].filesz));
        }
    }
    ((void (*)(UINTN, UINTN))PTR(tramp))((UINTN)kentry, (UINTN)info);
    for (;;) __asm__ volatile("hlt");
}
