#include "smp.h"
#include "task.h"
#include "timer.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "sysinfo.h"
#include "idt.h"

/*
 * Starting the other processor cores (64-bit kernel).
 *
 * The ACPI MADT lists the cores (by local APIC id). Each one is started
 * with INIT + STARTUP IPIs: it begins in 16-bit real mode at 0x8000, where
 * kernel/smp_tramp.asm was copied, goes through protected mode into long
 * mode with the boot core's page tables, and lands in smp_ap_entry() on a
 * stack of its own. There it loads the kernel's GDT (with a TSS of its own,
 * so cpu_id() can tell the cores apart), the IDT, and turns its local APIC
 * on for IPIs only - the 8259's device interrupts keep going to the boot
 * core alone. Then it waits in task_ap_loop() for app code to run.
 *
 * Sending IPIs needs the boot core's local APIC, which kernel/idt.c turned
 * off (UEFI leaves its LINT0 masked): it is turned back on here in
 * "virtual wire" mode - LINT0 passes the 8259's interrupts through, as a
 * BIOS sets it up - and checked: if the timer stops ticking, it goes off
 * again and Banana OS stays on one core.
 */

static volatile int g_cpus = 1;
static int          g_found = 1;

int cpu_count(void) { return g_cpus; }
int smp_cores_found(void) { return g_found; }

#ifndef __x86_64__

/* the 32-bit kernel uses one core */
void smp_init(void) {}
void smp_send_ipi(int cpu, int vector) { (void)cpu; (void)vector; }
void smp_eoi(void) {}

#else

#define LAPIC_ID        0x020
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LVT_MASKED      0x10000u

#define TRAMP_ADDR      0x8000u

static volatile uint32_t* g_lapic;
static uint8_t            g_apic_id[SMP_MAX_CPUS];
static volatile int       g_ap_up;

static uint32_t lapic_rd(uint32_t r) { return g_lapic[r / 4]; }
static void     lapic_wr(uint32_t r, uint32_t v) { g_lapic[r / 4] = v; }

static void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    __asm__ volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}
static void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

void smp_eoi(void) {
    if (g_lapic) g_lapic[LAPIC_EOI / 4] = 0;
}

static void icr_send(uint32_t apic, uint32_t lo) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    while (lapic_rd(LAPIC_ICR_LO) & (1u << 12)) __asm__ volatile("pause");
    lapic_wr(LAPIC_ICR_HI, apic << 24);
    lapic_wr(LAPIC_ICR_LO, lo);
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void smp_send_ipi(int cpu, int vector) {
    if (!g_lapic || cpu < 0 || cpu >= g_cpus) return;
    icr_send(g_apic_id[cpu], (uint32_t)vector | (1u << 14));      /* fixed, assert */
}

/* ── ACPI: where the cores are ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     sig[8];
    uint8_t  sum;
    char     oem[6];
    uint8_t  rev;
    uint32_t rsdt;
    uint32_t len;
    uint64_t xsdt;
    uint8_t  xsum;
    uint8_t  res[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char     sig[4];
    uint32_t len;
    uint8_t  rev, sum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} sdt_t;

static int sum_ok(const uint8_t* p, uint32_t n) {
    uint8_t s = 0;
    while (n--) s += *p++;
    return s == 0;
}

static const rsdp_t* scan_rsdp(uintptr_t a, uintptr_t end) {
    for (; a + 20 <= end; a += 16)
        if (memcmp((const void*)a, "RSD PTR ", 8) == 0 && sum_ok((const uint8_t*)a, 20)) return (const rsdp_t*)a;
    return NULL;
}

static const rsdp_t* find_rsdp(void) {
    const sysinfo_t* si = sysinfo_get();
    if (si->has_rsdp) return (const rsdp_t*)si->rsdp;           /* GRUB's copy (UEFI too) */
    const rsdp_t* r = scan_rsdp(0x9FC00, 0xA0000);              /* the usual EBDA */
    return r ? r : scan_rsdp(0xE0000, 0x100000);                /* the BIOS area */
}

static const sdt_t* table_at(uint64_t a, const char* sig) {
    if (!a || a + sizeof(sdt_t) > (4ull << 30)) return NULL;    /* (the low 4 GiB are mapped) */
    const sdt_t* t = (const sdt_t*)(uintptr_t)a;
    return memcmp(t->sig, sig, 4) == 0 ? t : NULL;
}

static const sdt_t* find_madt(const rsdp_t* r) {
    const sdt_t* x = r->rev >= 2 ? table_at(r->xsdt, "XSDT") : NULL;
    if (x) {
        for (uint32_t o = sizeof(sdt_t); o + 8 <= x->len; o += 8) {
            uint64_t a;
            memcpy(&a, (const uint8_t*)x + o, 8);
            const sdt_t* t = table_at(a, "APIC");
            if (t) return t;
        }
    }
    const sdt_t* rs = table_at(r->rsdt, "RSDT");
    if (rs) {
        for (uint32_t o = sizeof(sdt_t); o + 4 <= rs->len; o += 4) {
            uint32_t a;
            memcpy(&a, (const uint8_t*)rs + o, 4);
            const sdt_t* t = table_at(a, "APIC");
            if (t) return t;
        }
    }
    return NULL;
}

/* the I/O APIC is not used (the 8259 is): none of its inputs may get through */
static void ioapic_mask_all(uint32_t base) {
    if (!base) return;
    volatile uint32_t* io = (volatile uint32_t*)(uintptr_t)base;
    io[0] = 1;
    uint32_t n = ((io[4] >> 16) & 0xFF) + 1;
    for (uint32_t i = 0; i < n; i++) {
        io[0] = 0x10 + 2 * i;
        uint32_t v = io[4];
        io[0] = 0x10 + 2 * i;
        io[4] = v | LVT_MASKED;
    }
}

/* ── another core's first C code ─────────────────────────────────── */

extern uint8_t smp_tramp_start[], smp_tramp_end[], smp_tramp_data[];

typedef struct __attribute__((packed)) {
    uint32_t cr3, cr4, cr0, efer;
    uint64_t stack, entry;
    uint32_t cpu, pad;
} tramp_data_t;

static void smp_ap_entry(uint32_t cpu) {
    idt_ap_init((int)cpu);                  /* GDT with its own TSS, the IDT */
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    uint32_t lo, hi;
    rdmsr(0x1B, &lo, &hi);
    if (!(lo & (1u << 11))) wrmsr(0x1B, lo | (1u << 11), hi);
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_LVT_LINT0, LVT_MASKED);  /* device interrupts: the boot core's */
    lapic_wr(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_wr(LAPIC_LVT_ERR, LVT_MASKED);
    lapic_wr(LAPIC_SVR, 0x100u | SMP_VEC_SPURIOUS);
    g_ap_up = 1;
    task_ap_loop((int)cpu);
}

static void wait_ms(uint32_t ms) {
    uint32_t t0 = timer_ms();
    while (timer_ms() - t0 < ms) timer_idle();
}

/* the boot core's local APIC on, passing the 8259's interrupts through; 0 if that breaks them */
static int lapic_on(uint32_t madt_addr) {
    uint32_t lo, hi;
    rdmsr(0x1B, &lo, &hi);
    uint32_t base = lo & 0xFFFFF000u;
    if (hi || !base) base = madt_addr;
    if (!base) return 0;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    wrmsr(0x1B, (lo & ~(1u << 10)) | (1u << 11), hi);     /* xAPIC mode, on */
    g_lapic = (volatile uint32_t*)(uintptr_t)base;
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_LVT_ERR, LVT_MASKED);
    lapic_wr(LAPIC_LVT_LINT0, 0x700);                     /* ExtINT: the 8259 */
    lapic_wr(LAPIC_LVT_LINT1, 0x400);                     /* NMI */
    lapic_wr(LAPIC_SVR, 0x100u | SMP_VEC_SPURIOUS);
    __asm__ volatile("sti");
    /* does the timer still tick? (bounded: no hlt, it may not) */
    uint32_t t0 = timer_irq_count();         /* (the interrupts themselves, not the clock) */
    for (uint32_t i = 0; i < 400000000u && timer_irq_count() - t0 < 3; i++) __asm__ volatile("pause");
    int ok = timer_irq_count() - t0 >= 3;
    if (!ok) {
        __asm__ volatile("cli");
        wrmsr(0x1B, lo & ~((1u << 11) | (1u << 10)), hi);
        g_lapic = NULL;
    }
    if (f & 0x200) __asm__ volatile("sti");
    else __asm__ volatile("cli");
    return ok;
}

/* The boot core's 1 kHz tick from its local APIC timer instead of the PIT.
 * With the local APIC on, the PIT's interrupt reaches the core through the
 * old "virtual wire" path (LINT0, ExtINT), which virtual machines handle
 * poorly - VirtualBox delivered those ticks late while the guest idled and
 * then in bursts, and the sound, the pointer and the clock stuttered with
 * them. The local APIC timer is what current systems use: counted against
 * the TSC clock (timer_ms) for 20 ms, then periodic. If it does not tick,
 * the PIT stays the timer. */
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

static int lapic_timer_start(void) {
    lapic_wr(LAPIC_TIMER_DIV, 0x3);                       /* the bus clock / 16 */
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | SMP_VEC_TIMER);
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFFu);              /* one shot, counting down: how fast? */
    uint32_t t0 = timer_ms();
    for (uint32_t i = 0; i < 400000000u && timer_ms() - t0 < 20; i++) __asm__ volatile("pause");
    uint32_t ms = timer_ms() - t0;
    uint32_t left = lapic_rd(LAPIC_TIMER_CUR);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    if (ms < 20) return 0;
    uint32_t per_ms = (0xFFFFFFFFu - left) / ms;
    if (per_ms < 100) return 0;

    __asm__ volatile("cli");
    irq_timer_from_lapic(1);                             /* the PIT's IRQ 0 masked */
    lapic_wr(LAPIC_LVT_TIMER, SMP_VEC_TIMER | (1u << 17)); /* periodic */
    lapic_wr(LAPIC_TIMER_INIT, per_ms);
    __asm__ volatile("sti");
    uint32_t c0 = timer_irq_count();
    for (uint32_t i = 0; i < 400000000u && timer_irq_count() - c0 < 5; i++) __asm__ volatile("pause");
    if (timer_irq_count() - c0 >= 5) return 1;
    __asm__ volatile("cli");                             /* it does not tick: back to the PIT */
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    irq_timer_from_lapic(0);
    __asm__ volatile("sti");
    return 0;
}

void smp_init(void) {
    /* "nosmp" on the kernel command line (GRUB): one core, the local APIC left off */
    if (strstr(sysinfo_get()->cmdline, "nosmp")) { klog("smp: nosmp - using one core\n"); return; }
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    /* (CPUID's APIC bit is clear now: kernel/idt.c switched the APIC off.
     * Every 64-bit processor has one; the MADT below says how many cores.) */
    if (!(d & (1u << 5))) { klog("smp: no MSRs - using one core\n"); return; }
    const rsdp_t* r = find_rsdp();
    const sdt_t* madt = r ? find_madt(r) : NULL;
    if (!madt) { klog("smp: no ACPI MADT - using one core\n"); return; }

    uint8_t ids[64];
    int n = 0;
    uint32_t ioapic[4] = { 0, 0, 0, 0 };
    int nio = 0;
    uint32_t lapic_addr = *(const uint32_t*)((const uint8_t*)madt + 36);
    for (uint32_t o = 44; o + 2 <= madt->len;) {
        const uint8_t* e = (const uint8_t*)madt + o;
        if (e[1] < 2) break;
        if (e[0] == 0 && e[1] >= 8) {                     /* a processor's local APIC */
            uint32_t flags;
            memcpy(&flags, e + 4, 4);
            if ((flags & 1) && n < 64) ids[n++] = e[3];
        } else if (e[0] == 1 && e[1] >= 12 && nio < 4) {  /* an I/O APIC */
            memcpy(&ioapic[nio++], e + 4, 4);
        }
        o += e[1];
    }
    g_found = n;
    if (n < 2) { klog("smp: one processor core\n"); return; }

    if (!lapic_on(lapic_addr)) {
        klog("smp: the local APIC would stop the timer - using one core\n");
        return;
    }
    if (lapic_timer_start()) klog("smp: the 1 kHz tick comes from the local APIC timer\n");
    else klog("smp: the local APIC timer does not tick - the PIT stays the timer\n");
    for (int i = 0; i < nio; i++) ioapic_mask_all(ioapic[i]);
    uint8_t bsp = (uint8_t)(lapic_rd(LAPIC_ID) >> 24);
    g_apic_id[0] = bsp;

    /* the start code, at 0x8000 */
    uint32_t len = (uint32_t)(smp_tramp_end - smp_tramp_start);
    if (len > 4096) { klog("smp: start code too big\n"); return; }
    memcpy((void*)(uintptr_t)TRAMP_ADDR, smp_tramp_start, len);
    tramp_data_t* td = (tramp_data_t*)(uintptr_t)(TRAMP_ADDR + (uint32_t)(smp_tramp_data - smp_tramp_start));
    uintptr_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    uint32_t elo, ehi;
    rdmsr(0xC0000080u, &elo, &ehi);
    td->cr0 = (uint32_t)cr0;
    td->cr3 = (uint32_t)cr3;
    td->cr4 = (uint32_t)cr4;
    td->efer = elo & ((1u << 0) | (1u << 8) | (1u << 11));    /* SCE, LME, NXE */
    td->entry = (uint64_t)(uintptr_t)smp_ap_entry;

    for (int i = 0; i < n && g_cpus < SMP_MAX_CPUS; i++) {
        if (ids[i] == bsp) continue;
        int cpu = g_cpus;
        uint8_t* stack = (uint8_t*)kmalloc(16384);
        if (!stack) break;
        td->stack = ((uint64_t)(uintptr_t)stack + 16384) & ~15ull;
        td->cpu = (uint32_t)cpu;
        g_apic_id[cpu] = ids[i];
        g_ap_up = 0;
        __sync_synchronize();
        icr_send(ids[i], 0x4500);                          /* INIT */
        wait_ms(10);
        for (int tries = 0; tries < 2 && !g_ap_up; tries++) {
            icr_send(ids[i], 0x4600 | (TRAMP_ADDR >> 12)); /* STARTUP at 0x8000 */
            uint32_t t0 = timer_ms();
            while (!g_ap_up && timer_ms() - t0 < (tries ? 200u : 2u)) timer_idle();
        }
        if (!g_ap_up) {
            klog("smp: core (APIC id %u) did not start\n", ids[i]);
            kfree(stack);
            continue;
        }
        g_cpus = cpu + 1;
    }
    klog("smp: %d of %d processor cores running\n", g_cpus, n);
}

#endif
