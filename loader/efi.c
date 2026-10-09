/* Banana Boot - the UEFI loader (BOOTX64.EFI, 64-bit).
 *
 * It shows the menu, reads the chosen kernel from its own FAT partition
 * (/boot/kernel.bin or /boot/kernel64.bin, next to it in the image's EFI
 * system partition), copies it where its ELF headers say, hands the
 * firmware's memory map, the GOP framebuffer and the ACPI and EFI tables
 * to it as Multiboot2 information, leaves the firmware (ExitBootServices),
 * and starts it as GRUB did: through tramp.S, from 64-bit long mode down
 * to 32-bit protected mode with paging off - for both kernels.
 *
 * Built position-independent with no relocations (efi.lds; the Makefile
 * checks): it runs wherever the firmware puts it. */
#include "efi.h"

static EFI_SYSTEM_TABLE*  ST;
static EFI_BOOT_SERVICES* BS;

void* memcpy(void* d, const void* s, unsigned long n) { u8* a = d; const u8* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { u8* a = d; while (n--) *a++ = (u8)c; return d; }

static const EFI_GUID LOADED_IMAGE_GUID = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID SIMPLE_FS_GUID    = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID FILE_INFO_GUID    = { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const EFI_GUID GOP_GUID          = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static const EFI_GUID ACPI20_GUID       = { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static const EFI_GUID ACPI10_GUID       = { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static int guid_eq(const EFI_GUID* a, const EFI_GUID* b) {
    const u8* x = (const u8*)a; const u8* y = (const u8*)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* ── text ─────────────────────────────────────────────────────────────── */
static void print_at(int col, int row, const char* s, int attr) {
    CHAR16 buf[100];
    int n = 0;
    while (*s && n < 99) buf[n++] = (CHAR16)(u8)*s++;
    buf[n] = 0;
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

static void halt(const char* why) __attribute__((noreturn));
static void halt(const char* why) {
    print("\r\nBanana Boot: ");
    print(why);
    print("\r\n");
    for (;;) BS->Stall(1000000);
}

/* ── the menu ─────────────────────────────────────────────────────────── */
static void draw_menu(int sel, int secs) {
    print_at(2, 1, "Banana Boot  (UEFI)", 0x0E);
    for (int i = 0; i < ENTRIES; i++) {
        char line[60] = " ";                    /* the label in a bar 56 wide */
        c_strcat(line, entry_label(i));
        for (u32 n = c_strlen(line); n < 56; n++) line[n] = ' ';
        line[56] = 0;
        print_at(3, 3 + i, line, i == sel ? 0x70 : 0x07);
    }
    print_at(4, 10, "Up / Down choose, Enter starts.", 0x08);
    print_at(4, 11, "                                                ", 0x07);
    if (secs >= 0) {
        char line[64] = "Starting the highlighted entry in ";
        char n[12];
        u32_str((u32)secs, n);
        c_strcat(line, n);
        c_strcat(line, " s.");
        print_at(4, 11, line, 0x08);
    }
}

static int menu(void) {
    int sel = 0, ticks = 30;                    /* 3 s, by 100 ms */
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->EnableCursor(ST->ConOut, 0);
    draw_menu(sel, 3);
    for (;;) {
        EFI_INPUT_KEY k;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) == EFI_SUCCESS) {
            ticks = -1;
            if (k.ScanCode == 1) sel = (sel + ENTRIES - 1) % ENTRIES;
            else if (k.ScanCode == 2) sel = (sel + 1) % ENTRIES;
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

/* ── the kernel file, from the partition this program came from ──────── */
static u8* read_kernel(EFI_HANDLE image, int is64, UINTN* size) {
    EFI_LOADED_IMAGE* li;
    EFI_SIMPLE_FILE_SYSTEM* fs;
    EFI_FILE *root, *f;
    if (BS->HandleProtocol(image, &LOADED_IMAGE_GUID, (void**)&li) != EFI_SUCCESS) halt("no loaded-image protocol");
    if (BS->HandleProtocol(li->DeviceHandle, &SIMPLE_FS_GUID, (void**)&fs) != EFI_SUCCESS) halt("cannot read my partition");
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS) halt("cannot open my partition");
    CHAR16 path[24];
    const char* p = is64 ? "\\boot\\kernel64.bin" : "\\boot\\kernel.bin";
    int n = 0;
    while (*p) path[n++] = (CHAR16)*p++;
    path[n] = 0;
    if (root->Open(root, &f, path, 1, 0) != EFI_SUCCESS) halt("the kernel is not in my partition (/boot)");
    u8 info[512];
    UINTN isz = sizeof(info);
    if (f->GetInfo(f, &FILE_INFO_GUID, &isz, info) != EFI_SUCCESS) halt("cannot read the kernel");
    *size = (UINTN)((EFI_FILE_INFO*)info)->FileSize;
    u8* buf;
    if (BS->AllocatePool(EfiLoaderData, *size, (void**)&buf) != EFI_SUCCESS) halt("out of memory");
    UINTN got = *size;
    if (f->Read(f, &got, buf) != EFI_SUCCESS || got != *size) halt("cannot read the kernel");
    f->Close(f);
    root->Close(root);
    return buf;
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

extern u8 tramp_start[], tramp_end[], tramp_gdtr_base[], tramp_gdt[];

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    ST = st;
    BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);

    int entry = menu();
    ST->ConOut->ClearScreen(ST->ConOut);
    print(entry_is64(entry) ? "Loading Banana OS (64-bit)...\r\n" : "Loading Banana OS (32-bit)...\r\n");

    UINTN fsize;
    u8* file = read_kernel(image, entry_is64(entry), &fsize);
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
        u64 sp = (u64)&sp;
        u64 r[5][2] = { { (u64)file, fsize }, { info, 65536 }, { tramp, 4096 },
                        { (u64)li->ImageBase, li->ImageSize }, { sp - 65536, 65536 + 4096 } };
        for (int i = 0; i < 5; i++)
            if (r[i][0] < hi && r[i][0] + r[i][1] > lo) halt("the kernel's place is taken by the firmware");
    }
    memcpy((void*)tramp, tramp_start, (UINTN)(tramp_end - tramp_start));
    *(u64*)(tramp + (UINTN)(tramp_gdtr_base - tramp_start)) = tramp + (u64)(tramp_gdt - tramp_start);

    mb2_t m;
    mb2_begin(&m, (u8*)info);
    char cmd[96];
    build_cmdline(cmd, entry);
    mb2_string(&m, 1, cmd);
    mb2_string(&m, 2, "Banana Boot (UEFI)");
    u8* t = mb2_tag(&m, 12, 16);                     /* the EFI system table: the kernel knows it is UEFI */
    *(u64*)(t + 8) = (u64)st;
    /* the ACPI RSDP: the ACPI 2 one, else the ACPI 1 one */
    const u8* rsdp = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* c = &st->ConfigurationTable[i];
        if (guid_eq(&c->VendorGuid, &ACPI20_GUID)) { rsdp = (const u8*)c->VendorTable; break; }
        if (guid_eq(&c->VendorGuid, &ACPI10_GUID)) rsdp = (const u8*)c->VendorTable;
    }
    if (rsdp) mb2_rsdp(&m, rsdp);
    EFI_GOP* gop;
    if (BS->LocateProtocol(&GOP_GUID, 0, (void**)&gop) == EFI_SUCCESS && gop->Mode && gop->Mode->Info &&
        gop->Mode->Info->PixelFormat != 3) {
        EFI_GOP_MODE_INFO* gi = gop->Mode->Info;
        u8 rp = 16, gp = 8, bp = 0;
        if (gi->PixelFormat == 0) { rp = 0; bp = 16; }
        if (gi->PixelFormat == 2) {
            u32 masks[3] = { gi->RedMask, gi->GreenMask, gi->BlueMask };
            u8 pos[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; k++) { u32 v = masks[k]; while (v && !(v & 1)) { v >>= 1; pos[k]++; } }
            rp = pos[0]; gp = pos[1]; bp = pos[2];
        }
        mb2_framebuffer(&m, gop->Mode->FrameBufferBase, gi->PixelsPerScanLine * 4, gi->HorizontalResolution,
                        gi->VerticalResolution, 32, rp, 8, gp, 8, bp, 8);
    }

    if (reserved) {
        for (int i = 0; i < ns; i++) {
            memcpy((void*)segs[i].paddr, file + segs[i].offset, (UINTN)segs[i].filesz);
            memset((void*)(segs[i].paddr + segs[i].filesz), 0, (UINTN)(segs[i].memsz - segs[i].filesz));
        }
    }

    /* the memory map last, then leave the firmware (again if the map changed meanwhile) */
    UINTN msize = 0, key, dsize;
    u32 dver;
    BS->GetMemoryMap(&msize, 0, &key, &dsize, &dver);
    msize += 64 * 64;
    u8* map;
    if (BS->AllocatePool(EfiLoaderData, msize, (void**)&map) != EFI_SUCCESS) halt("out of memory");
    u32 before_map = m.off;
    for (int tries = 0;; tries++) {
        UINTN sz = msize;
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
            memcpy((void*)segs[i].paddr, file + segs[i].offset, (UINTN)segs[i].filesz);
            memset((void*)(segs[i].paddr + segs[i].filesz), 0, (UINTN)(segs[i].memsz - segs[i].filesz));
        }
    }
    ((void (*)(u64, u64))tramp)(kentry, info);
    for (;;) __asm__ volatile("hlt");
}
