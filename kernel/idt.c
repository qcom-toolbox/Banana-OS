#include "idt.h"
#include "types.h"
#include "fb.h"
#include "serial.h"
#include "app.h"
#include "kstring.h"
#include "smp.h"
#include "task.h"
#include "timer.h"

#define VGA_MEMORY ((volatile uint16_t*)0xB8000)

#ifdef __x86_64__
/* long mode: 16-byte gates with a 64-bit handler address */
struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* What kernel/isr64.asm pushes, low address -> high address: the 15
 * general registers, int_no/err_code from the stub, then the frame the
 * CPU pushes in long mode (always including rsp/ss). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;
#define REG_IP(r) ((r)->rip)
#else
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
#define REG_IP(r) ((r)->eip)
#endif

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

static void idt_set_gate(uint8_t num, uintptr_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].sel     = sel;
    idt[num].flags   = flags;
#ifdef __x86_64__
    idt[num].ist      = 0;
    idt[num].base_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].base_hi  = (uint32_t)((uint64_t)base >> 32);
    idt[num].reserved = 0;
#else
    idt[num].always0 = 0;
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
#endif
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

    /* on another core it is app code that faulted: the app is stopped on
     * the boot core (kernel/task.c) - this returns only for a kernel bug */
    if (n < 32 && cpu_id() != 0) {
        uintptr_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        task_ap_fault(n, (uint32_t)regs->err_code, (uintptr_t)REG_IP(regs), cr2);
        klog("*** PANIC on core %d: %s (vector %u) at ip %llx\n", cpu_id(), name, n,
             (unsigned long long)REG_IP(regs));
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* an app that crashed is stopped; only kernel faults are fatal */
    if (n < 32 && n != 2 && n != 8 && n != 18) {
        uintptr_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        app_fault(n, (uint32_t)regs->err_code, (uintptr_t)REG_IP(regs), cr2);
    }

    vga_clear_rows(6, 0x4F);
    vga_puts(0, 2, "*** BANANA OS PANIC ***", 0x4F);
    vga_puts(1, 2, "Exception:", 0x4F);
    vga_puts(1, 14, name, 0x4F);
    vga_puts(2, 2, "Vector:", 0x4F);
    vga_put_hex(2, 10, n, 0x4F);
    vga_puts(3, 2, "Error code:", 0x4F);
    vga_put_hex(3, 14, regs->err_code, 0x4F);
    vga_puts(4, 2, "IP:", 0x4F);
#ifdef __x86_64__
    vga_put_hex(4, 7, (uint32_t)(REG_IP(regs) >> 32), 0x4F);
    vga_put_hex(4, 15, (uint32_t)REG_IP(regs), 0x4F);
#else
    vga_put_hex(4, 7, REG_IP(regs), 0x4F);
#endif
    vga_puts(5, 2, "CS:", 0x4F);
    vga_put_hex(5, 6, (uint32_t)regs->cs, 0x4F);
    klog("*** PANIC: %s (vector %u, error %x) at ip %llx\n", name, n, (uint32_t)regs->err_code,
         (unsigned long long)REG_IP(regs));

    /* Solid red field + binary-encoded vector number (MSB..LSB, left to
     * right) on the real framebuffer, in case that's what's on screen. */
    fb_raw_rect(0, 0, 260, 200, 0x00990000u);
    fb_raw_binary(8, 8, n, 5);

    for (;;) __asm__ volatile("cli; hlt");
}

/* ── hardware IRQs (8259 PIC) ───────────────────────────────────────
 * The PICs are remapped to vectors 32-47 (out of the CPU exception
 * range) and every line starts masked: drivers opt in with
 * irq_install(). Keyboard and mouse stay polled as before - only the
 * timer (and the NIC, to wake the CPU from hlt) use interrupts. */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static inline void pic_outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t pic_inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void pic_wait(void) { pic_outb(0x80, 0); }

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void (*const irq_stubs[16])(void) = {
    irq0, irq1, irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15,
};

/* PCI interrupt lines are shared (e.g. the NIC and a USB controller on
 * IRQ 11), so each line keeps a short chain; every handler checks its own
 * device and returns quietly when it was not the source. */
#define IRQ_CHAIN 4
static void (*irq_handlers[16][IRQ_CHAIN])(void);
static uint16_t irq_mask = 0xFFFF;

static void pic_apply_mask(void) {
    uint16_t m = irq_mask;
    /* the cascade line (IRQ2) must be open for anything on the slave */
    if ((m & 0xFF00) != 0xFF00) m &= (uint16_t)~(1u << 2);
    pic_outb(PIC1_DATA, (uint8_t)(m & 0xFF));
    pic_outb(PIC2_DATA, (uint8_t)(m >> 8));
}

static void pic_remap(void) {
    pic_outb(PIC1_CMD, 0x11); pic_wait();   /* ICW1: init + ICW4 */
    pic_outb(PIC2_CMD, 0x11); pic_wait();
    pic_outb(PIC1_DATA, 32);  pic_wait();   /* ICW2: vector offsets */
    pic_outb(PIC2_DATA, 40);  pic_wait();
    pic_outb(PIC1_DATA, 4);   pic_wait();   /* ICW3: slave on IRQ2 */
    pic_outb(PIC2_DATA, 2);   pic_wait();
    pic_outb(PIC1_DATA, 0x01); pic_wait();  /* ICW4: 8086 mode */
    pic_outb(PIC2_DATA, 0x01); pic_wait();
    pic_apply_mask();
}

void irq_install(int irq, void (*handler)(void)) {
    if (irq < 0 || irq > 15) return;
    for (int i = 0; i < IRQ_CHAIN; i++) {
        if (irq_handlers[irq][i] == handler) break;
        if (!irq_handlers[irq][i]) { irq_handlers[irq][i] = handler; break; }
    }
    irq_mask &= (uint16_t)~(1u << irq);
    pic_apply_mask();
}

void irq_handler(registers_t* regs) {
    int irq = (int)regs->int_no - 32;
    if (irq < 0 || irq > 15) return;

    /* Spurious IRQ7/15: the PIC's in-service bit isn't set, so it must
     * not be acknowledged (IRQ15 still needs the master's EOI). */
    if (irq == 7 || irq == 15) {
        uint16_t cmd = (irq == 7) ? PIC1_CMD : PIC2_CMD;
        pic_outb(cmd, 0x0B);  /* read ISR */
        if (!(pic_inb(cmd) & 0x80)) {
            if (irq == 15) pic_outb(PIC1_CMD, 0x20);
            return;
        }
    }

    for (int i = 0; i < IRQ_CHAIN && irq_handlers[irq][i]; i++) irq_handlers[irq][i]();

    /* An interrupt storm: a line that fires without end (a device on a
     * shared line that no driver here quiets - the BIOS's USB 1.1
     * controllers next to the USB 2.0 one, on older Intel chipsets) leaves
     * the processor no time for anything else: the boot stood still. More
     * than 5000 in 100 ms is no real device's load: the line is masked
     * (its drivers poll too, they go on). */
    if (irq != 0) {
        static uint32_t win[16], cnt[16];
        uint32_t now = timer_irq_count();
        if (now - win[irq] >= 100) { win[irq] = now; cnt[irq] = 0; }
        if (++cnt[irq] > 5000) {
            irq_mask |= (uint16_t)(1u << irq);
            pic_apply_mask();
            klog("irq %d: interrupt storm (a device nobody answers) - the line is masked\n", irq);
        }
    }

    if (irq >= 8) pic_outb(PIC2_CMD, 0x20);
    pic_outb(PIC1_CMD, 0x20);
    /* app code is safe to switch away from (kernel code is not) */
    app_preempt((uintptr_t)REG_IP(regs));
}

/* UEFI firmware hands over with the local APIC enabled and its LINT0
 * input (where the 8259 PIC's interrupt line arrives) masked, so no
 * hardware interrupt would ever reach the CPU - a BIOS sets LINT0 to
 * "ExtINT" pass-through instead. Banana OS uses the 8259, so the local
 * APIC is switched off: the CPU then takes the PIC's INTR directly. */
static void lapic_off(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 9))) return;                       /* no APIC */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    if (!(lo & (1u << 11))) return;                     /* already off */
    lo &= ~((1u << 11) | (1u << 10));                   /* EN and x2APIC EXTD */
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1Bu));
}

/* Before a restart, in a virtual machine: the local APIC back on, its LINT0
 * passing the 8259's interrupts through (ExtINT), as a BIOS leaves it. QEMU
 * (and maybe others) keeps an APIC switched off by lapic_off() off across
 * the reset, with LINT0 masked: the BIOS then never got its timer interrupt
 * and Banana Boot's countdown stood still. A real PC's reset clears it all
 * (and some processors cannot switch a disabled APIC back on: not there). */
void lapic_before_reset(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 31)) || !(d & (1u << 5))) return;  /* not a VM / no MSRs */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    if (!(lo & (1u << 11))) {
        lo |= 1u << 11;
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1Bu));
    }
    uintptr_t base = (uintptr_t)(lo & 0xFFFFF000u);
    if (!base || hi) return;
    volatile uint32_t* apic = (volatile uint32_t*)base;
    apic[0xF0 / 4] = 0x1FF;                             /* SVR: on, spurious vector 0xFF */
    apic[0x350 / 4] = 0x700;                            /* LINT0: ExtINT */
    apic[0x360 / 4] = 0x400;                            /* LINT1: NMI */
}

#ifdef __x86_64__
/* A TSS for its interrupt stack table: page faults, stack faults and double
 * faults run on a stack of their own, so an app that ran into the guard
 * page under its stack (kernel/app.c) gets a clean "stack overflow"
 * instead of a fault while pushing the fault's frame (a triple fault). */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap;
} __attribute__((packed));

/* One TSS per processor core (kernel/smp.c), each with its own fault
 * stack; core n's is at selector 0x10 + 16 * n, which is also how a core
 * knows which one it is (cpu_id(), kernel/smp.h). */
static struct tss64 g_tss[SMP_MAX_CPUS];
static uint8_t g_fault_stack[SMP_MAX_CPUS][16384] __attribute__((aligned(16)));
static uint64_t g_gdt[2 + 2 * SMP_MAX_CPUS];   /* null, 64-bit code (0x08), a TSS (16 bytes) per core */

static void gdt_load_cpu(int cpu) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gp = { sizeof(g_gdt) - 1, (uint64_t)(uintptr_t)g_gdt };
    __asm__ volatile("lgdt %0" :: "m"(gp));
    /* long mode ignores the data segments, but iretq reloads SS from the
     * interrupt frame: the old selectors must not point at the TSS slot */
    __asm__ volatile("xor %%eax, %%eax; mov %%ax, %%ss; mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%fs; mov %%ax, %%gs"
                     ::: "rax", "memory");
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)(0x10 + 16 * cpu)));
}

static void tss_init(void) {
    memset(g_tss, 0, sizeof(g_tss));
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFull;      /* the same code segment as boot64.asm's */
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        struct tss64* t = &g_tss[c];
        t->ist[0] = (uint64_t)(uintptr_t)(g_fault_stack[c] + sizeof(g_fault_stack[c]));
        t->iomap = sizeof(*t);
        uint64_t base = (uint64_t)(uintptr_t)t, limit = sizeof(*t) - 1;
        g_gdt[2 + 2 * c] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
                           (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
        g_gdt[3 + 2 * c] = base >> 32;
    }
    gdt_load_cpu(0);
}

/* another core, starting (kernel/smp.c) */
void idt_ap_init(int cpu) {
    gdt_load_cpu(cpu);
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

extern void ipi_stub(void);
extern void stray_stub(void);
extern void spurious_stub(void);
extern void lapic_timer_stub(void);

/* local-APIC interrupts: kernel/isr64.asm */
void ipi_handler(registers_t* regs) {
    if (regs->int_no == SMP_VEC_TIMER) {
        /* the boot core's local APIC timer, standing in for the PIT (IRQ 0) */
        for (int i = 0; i < IRQ_CHAIN && irq_handlers[0][i]; i++) irq_handlers[0][i]();
        smp_eoi();
        app_preempt((uintptr_t)REG_IP(regs));
        return;
    }
    smp_eoi();
    if (regs->int_no == SMP_VEC_KICK) task_ipi();
}

/* kernel/smp.c: the timer's handlers (IRQ 0) run from the local APIC
 * timer from now on (1), or from the PIT again (0) */
void irq_timer_from_lapic(int on) {
    if (on) irq_mask |= 1u;
    else irq_mask &= (uint16_t)~1u;
    pic_apply_mask();
}
#endif

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uintptr_t)&idt;

    uint16_t code_sel = read_cs();

    for (int i = 0; i < 256; i++) idt_set_gate((uint8_t)i, 0, 0, 0);
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, (uintptr_t)isr_stubs[i], code_sel, 0x8E);
    }
#ifdef __x86_64__
    tss_init();
    idt[8].ist = 1;                        /* double fault */
    idt[12].ist = 1;                       /* stack fault */
    idt[14].ist = 1;                       /* page fault */
#endif
    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(32 + i), (uintptr_t)irq_stubs[i], code_sel, 0x8E);
    }
#ifdef __x86_64__
    for (int i = 48; i < 256; i++) idt_set_gate((uint8_t)i, (uintptr_t)stray_stub, code_sel, 0x8E);
    idt_set_gate(SMP_VEC_KICK, (uintptr_t)ipi_stub, code_sel, 0x8E);
    idt_set_gate(SMP_VEC_TIMER, (uintptr_t)lapic_timer_stub, code_sel, 0x8E);
    idt_set_gate(SMP_VEC_SPURIOUS, (uintptr_t)spurious_stub, code_sel, 0x8E);
#endif

    __asm__ volatile("lidt %0" : : "m"(idtp));
    lapic_off();
    pic_remap();
}
