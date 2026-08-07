#include "idt.h"
#include "types.h"
#include "fb.h"

#define VGA_MEMORY ((volatile uint16_t*)0xB8000)

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Layout matches, low address -> high address, exactly what isr.asm pushes:
 * pushad (eax,ecx,edx,ebx,esp,ebp,esi,edi in *push* order -> edi ends up at
 * the lowest address), then ds/es/fs/gs, then int_no/err_code from the ISR
 * stub, then eip/cs/eflags pushed by the CPU itself (ring0 -> ring0, so no
 * user esp/ss). Declared here low-to-high to mirror memory order. */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} registers_t;

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* The Multiboot2 spec guarantees a flat 32-bit code/data GDT is loaded,
 * but does NOT guarantee which selector values GRUB used for it - 0x08
 * is a common convention, not a promise. Read the CS we're actually
 * running with instead of assuming, so a fault handler doesn't itself
 * fault trying to load a guessed-wrong segment into a gate. */
static uint16_t read_cs(void) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

static void (*const isr_stubs[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static const char* const exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point Exception", "Alignment Check",
    "Machine Check", "SIMD Floating-Point Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor Injection", "VMM Communication Exception",
    "Security Exception", "Reserved",
};

/* ── Raw, dependency-free panic output ──────────────────────────────────
 * Deliberately does NOT call into terminal.c/gfx.c/fb.c's drawing paths:
 * if one of *those* is what faulted, reusing them here could fault again
 * and turn a diagnosable panic back into a silent triple-fault reset.
 * Writes straight to the legacy VGA text buffer (always safe to write,
 * whether or not it's the active display) and, if a linear framebuffer
 * was detected, pokes raw pixels directly via its already-stored geometry
 * instead of going through the gfx.c/fb.c drawing helpers. */

static void vga_puts(int row, int col, const char* s, uint8_t color) {
    volatile uint16_t* buf = VGA_MEMORY;
    for (int i = 0; s[i] && col + i < 80; i++) {
        buf[row * 80 + col + i] = (uint16_t)(uint8_t)s[i] | ((uint16_t)color << 8);
    }
}

static void vga_clear_rows(int rows, uint8_t color) {
    volatile uint16_t* buf = VGA_MEMORY;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < 80; c++)
            buf[r * 80 + c] = (uint16_t)' ' | ((uint16_t)color << 8);
}

static void vga_put_hex(int row, int col, uint32_t val, uint8_t color) {
    volatile uint16_t* buf = VGA_MEMORY;
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (uint8_t)((val >> ((7 - i) * 4)) & 0xFu);
        buf[row * 80 + col + i] = (uint16_t)(uint8_t)digits[nibble] | ((uint16_t)color << 8);
    }
}

static void fb_raw_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!fb_available()) return;
    const fb_info_t* fi = fb_info();
    if (!fi || !fi->addr || fi->bpp != 32) return;

    int x1 = x + w, y1 = y + h;
    if (x1 > (int)fi->width)  x1 = (int)fi->width;
    if (y1 > (int)fi->height) y1 = (int)fi->height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    for (int yy = y; yy < y1; yy++) {
        volatile uint32_t* row = (volatile uint32_t*)(fi->addr + (uintptr_t)((uint32_t)yy * fi->pitch));
        for (int xx = x; xx < x1; xx++) row[xx] = rgb;
    }
}

/* Draws the lowest `bits` bits of val as a row of black/white squares
 * (MSB first) so the exception vector is readable with no font rendering. */
static void fb_raw_binary(int x, int y, uint32_t val, int bits) {
    for (int i = 0; i < bits; i++) {
        uint32_t bit = (val >> (bits - 1 - i)) & 1u;
        uint32_t color = bit ? 0x00FFFFFFu : 0x00000000u;
        fb_raw_rect(x + i * 40, y, 32, 32, color);
    }
}

void isr_handler(registers_t* regs) {
    uint32_t n = regs->int_no;
    const char* name = (n < 32) ? exception_names[n] : "Unknown";

    vga_clear_rows(6, 0x4F);
    vga_puts(0, 2, "*** BANANA OS PANIC ***", 0x4F);
    vga_puts(1, 2, "Exception:", 0x4F);
    vga_puts(1, 14, name, 0x4F);
    vga_puts(2, 2, "Vector:", 0x4F);
    vga_put_hex(2, 10, n, 0x4F);
    vga_puts(3, 2, "Error code:", 0x4F);
    vga_put_hex(3, 14, regs->err_code, 0x4F);
    vga_puts(4, 2, "EIP:", 0x4F);
    vga_put_hex(4, 7, regs->eip, 0x4F);
    vga_puts(5, 2, "CS:", 0x4F);
    vga_put_hex(5, 6, regs->cs, 0x4F);

    /* Solid red field + binary-encoded vector number (MSB..LSB, left to
     * right) on the real framebuffer, in case that's what's on screen. */
    fb_raw_rect(0, 0, 260, 200, 0x00990000u);
    fb_raw_binary(8, 8, n, 5);

    for (;;) __asm__ volatile("cli; hlt");
}

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt;

    uint16_t code_sel = read_cs();

    for (int i = 0; i < 256; i++) idt_set_gate((uint8_t)i, 0, 0, 0);
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, (uint32_t)isr_stubs[i], code_sel, 0x8E);
    }

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
