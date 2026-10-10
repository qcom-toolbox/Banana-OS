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

/* ── ACPI tables: where the cores are, and the power-off registers ── */

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

/* the n-th ACPI table with this signature (0 = the first; there are
 * several SSDTs), through the XSDT or else the RSDT */
static const sdt_t* find_table(const rsdp_t* r, const char* sig, int n) {
    const sdt_t* x = r->rev >= 2 ? table_at(r->xsdt, "XSDT") : NULL;
    if (x) {
        for (uint32_t o = sizeof(sdt_t); o + 8 <= x->len; o += 8) {
            uint64_t a;
            memcpy(&a, (const uint8_t*)x + o, 8);
            const sdt_t* t = table_at(a, sig);
            if (t && n-- == 0) return t;
        }
        return NULL;
    }
    const sdt_t* rs = table_at(r->rsdt, "RSDT");
    if (rs) {
        for (uint32_t o = sizeof(sdt_t); o + 4 <= rs->len; o += 4) {
            uint32_t a;
            memcpy(&a, (const uint8_t*)rs + o, 4);
            const sdt_t* t = table_at(a, sig);
            if (t && n-- == 0) return t;
        }
    }
    return NULL;
}

const void* acpi_find_table(const char* sig) {
    const rsdp_t* r = find_rsdp();
    return r ? find_table(r, sig, 0) : NULL;
}

const void* acpi_find_table_n(const char* sig, int n) {
    const rsdp_t* r = find_rsdp();
    return r ? find_table(r, sig, n) : NULL;
}

const void* acpi_table_at(uint64_t addr, const char* sig) { return table_at(addr, sig); }

#ifndef __x86_64__

/* the 32-bit kernel uses one core */
static cpu_topo_t g_topo0;
const cpu_topo_t* cpu_topo(int cpu) { (void)cpu; return &g_topo0; }
int smp_phys_cores(void) { return 1; }
int smp_threads_per_core(void) { return 1; }
int smp_x2apic(void) { return 0; }
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

/* The local APIC: memory-mapped registers (xAPIC), or MSRs 0x800 +
 * offset / 16 (x2APIC: 32-bit ids, needed past 255 processors and where the
 * firmware locks it in that mode). */
static volatile uint32_t* g_lapic;
static volatile int       g_apic_on;
static int                g_x2;
static uint32_t           g_apic_id[SMP_MAX_CPUS];
static volatile int       g_ap_up;

static void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    __asm__ volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}
static void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

static uint32_t lapic_rd(uint32_t r) {
    if (g_x2) { uint32_t lo, hi; rdmsr(0x800 + r / 16, &lo, &hi); return lo; }
    return g_lapic[r / 4];
}
static void lapic_wr(uint32_t r, uint32_t v) {
    if (g_x2) wrmsr(0x800 + r / 16, v, 0);
    else g_lapic[r / 4] = v;
}
static uint32_t lapic_id(void) {
    return g_x2 ? lapic_rd(LAPIC_ID) : lapic_rd(LAPIC_ID) >> 24;
}

int smp_x2apic(void) { return g_x2; }

void smp_eoi(void) {
    if (g_apic_on) lapic_wr(LAPIC_EOI, 0);
}

/* ── topology ──────────────────────────────────────────────────────── */

static cpu_topo_t g_topo[SMP_MAX_CPUS];
static int        g_nphys = 1, g_tpc = 1;

const cpu_topo_t* cpu_topo(int cpu) { return &g_topo[cpu >= 0 && cpu < SMP_MAX_CPUS ? cpu : 0]; }
int smp_phys_cores(void) { return g_nphys; }
int smp_threads_per_core(void) { return g_tpc; }

static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static int bits_for(uint32_t n) { int b = 0; while ((1u << b) < n) b++; return b; }

/* this processor's place, asked of itself */
static void topo_self(cpu_topo_t* t) {
    uint32_t r[4], max, ext;
    cpuid2(0, 0, r);
    max = r[0];
    int amd = r[1] == 0x68747541u;                      /* "Auth"enticAMD */
    cpuid2(0x80000000u, 0, r);
    ext = r[0];
    cpuid2(1, 0, r);
    uint32_t apic = r[1] >> 24;
    int htt = (r[3] >> 28) & 1;
    uint32_t logical = htt ? ((r[1] >> 16) & 0xFF) : 1;
    int smt_shift = -1, pkg_shift = -1;
    uint32_t leaf = max >= 0x1F ? 0x1F : max >= 0xB ? 0xB : 0;
    if (leaf) {
        cpuid2(leaf, 0, r);
        if (!r[1] && leaf == 0x1F && max >= 0xB) { leaf = 0xB; cpuid2(leaf, 0, r); }
        if (r[1]) {
            for (uint32_t sub = 0; sub < 8; sub++) {
                cpuid2(leaf, sub, r);
                uint32_t type = (r[2] >> 8) & 0xFF;
                if (!type) break;
                if (type == 1) smt_shift = (int)(r[0] & 0x1F);
                pkg_shift = (int)(r[0] & 0x1F);         /* the last level: the package above it */
                apic = r[3];                            /* the x2APIC id */
            }
            if (smt_shift < 0) smt_shift = 0;
        }
    }
    if (pkg_shift < 0) {
        /* older processors: logical processors per package (leaf 1), cores per package (leaf 4 / AMD) */
        uint32_t cores = 1;
        if (amd && ext >= 0x80000008u) { cpuid2(0x80000008u, 0, r); cores = (r[2] & 0xFF) + 1; }
        else if (!amd && max >= 4) { cpuid2(4, 0, r); cores = (r[0] >> 26) + 1; }
        if (logical < cores) logical = cores;
        uint32_t tpc = logical / cores;
        if (amd && ext >= 0x8000001Eu && max >= 1) {   /* Zen: threads per core */
            cpuid2(0x8000001Eu, 0, r);
            tpc = ((r[1] >> 8) & 0xFF) + 1;
        }
        smt_shift = bits_for(tpc ? tpc : 1);
        pkg_shift = bits_for(logical ? logical : 1);
        if (pkg_shift < smt_shift) pkg_shift = smt_shift;
    }
    t->apic = apic;
    t->smt = (uint8_t)(apic & ((1u << smt_shift) - 1));
    t->core = (uint16_t)((apic >> smt_shift) & ((1u << (pkg_shift - smt_shift)) - 1));
    t->pkg = (uint16_t)(apic >> pkg_shift);
    t->type = CPU_TYPE_PLAIN;
    if (max >= 0x1A) {
        cpuid2(7, 0, r);
        if (r[3] & (1u << 15)) {                        /* a hybrid processor */
            cpuid2(0x1A, 0, r);
            uint32_t ct = r[0] >> 24;
            t->type = ct == 0x40 ? CPU_TYPE_PERF : ct == 0x20 ? CPU_TYPE_EFF : CPU_TYPE_PLAIN;
        }
    }
}

/* which logical processors share a physical core */
static void topo_link(int n) {
    int phys = 0, tpc = 1;
    for (int i = 0; i < n; i++) {
        g_topo[i].phys = -1;
        for (int j = 0; j < i; j++)
            if (g_topo[j].pkg == g_topo[i].pkg && g_topo[j].core == g_topo[i].core) { g_topo[i].phys = g_topo[j].phys; break; }
        if (g_topo[i].phys < 0) g_topo[i].phys = phys++;
    }
    for (int p = 0; p < phys; p++) {
        int k = 0;
        for (int i = 0; i < n; i++) if (g_topo[i].phys == p) k++;
        if (k > tpc) tpc = k;
    }
    g_nphys = phys ? phys : 1;
    g_tpc = tpc;
}

static void icr_send(uint32_t apic, uint32_t lo) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    if (g_x2) {
        wrmsr(0x830, lo, apic);                           /* x2APIC: one 64-bit ICR, no busy bit */
    } else {
        while (lapic_rd(LAPIC_ICR_LO) & (1u << 12)) __asm__ volatile("pause");
        lapic_wr(LAPIC_ICR_HI, apic << 24);
        lapic_wr(LAPIC_ICR_LO, lo);
    }
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void smp_send_ipi(int cpu, int vector) {
    if (!g_apic_on || cpu < 0 || cpu >= g_cpus) return;
    icr_send(g_apic_id[cpu], (uint32_t)vector | (1u << 14));      /* fixed, assert */
}


/* ── the I/O APICs ───────────────────────────────────────────────────
 * Not used for devices (the 8259 is), so their inputs are masked once the
 * local APIC is on - except an input in ExtINT mode: on many real boards
 * that is how the 8259 reaches the processor ("virtual wire" through the
 * I/O APIC rather than LINT0), and masking it silenced the timer: every
 * wait on it waited forever (stuck on the boot screen). What the firmware
 * set is saved first, and put back if the local APIC goes off again. */
#define IOAPIC_PINS 120

typedef struct {
    volatile uint32_t* io;
    uint32_t pins;
    uint32_t lo[IOAPIC_PINS];        /* the low half of each redirection entry */
} ioapic_t;

static ioapic_t g_ioa[4];
static int      g_nioa;
static int      g_extint_ioapic;     /* the 8259 reaches the core through an I/O APIC input */

static uint32_t ioa_rd(ioapic_t* a, uint32_t reg) { a->io[0] = reg; return a->io[4]; }
static void     ioa_wr(ioapic_t* a, uint32_t reg, uint32_t v) { a->io[0] = reg; a->io[4] = v; }

static void ioapic_save(uint32_t base) {
    if (!base || g_nioa >= 4) return;
    ioapic_t* a = &g_ioa[g_nioa++];
    a->io = (volatile uint32_t*)(uintptr_t)base;
    a->pins = ((ioa_rd(a, 1) >> 16) & 0xFF) + 1;
    if (a->pins > IOAPIC_PINS) a->pins = IOAPIC_PINS;
    for (uint32_t i = 0; i < a->pins; i++) {
        a->lo[i] = ioa_rd(a, 0x10 + 2 * i);
        if (!(a->lo[i] & LVT_MASKED) && ((a->lo[i] >> 8) & 7) == 7) g_extint_ioapic = 1;
    }
}

static void ioapic_mask_devices(void) {
    for (int k = 0; k < g_nioa; k++)
        for (uint32_t i = 0; i < g_ioa[k].pins; i++)
            if (((g_ioa[k].lo[i] >> 8) & 7) != 7) ioa_wr(&g_ioa[k], 0x10 + 2 * i, g_ioa[k].lo[i] | LVT_MASKED);
}

static void ioapic_restore(void) {
    for (int k = 0; k < g_nioa; k++)
        for (uint32_t i = 0; i < g_ioa[k].pins; i++) ioa_wr(&g_ioa[k], 0x10 + 2 * i, g_ioa[k].lo[i]);
}

static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* n timer interrupts within max_ms (busy: no hlt, they may not come) - bounded by the TSC */
static int ticks_arrive(uint32_t n, uint32_t max_ms) {
    uint32_t c0 = timer_irq_count();
    uint64_t per = timer_tsc_per_ms(), t0 = tsc();
    for (uint32_t i = 0;; i++) {
        if (timer_irq_count() - c0 >= n) return 1;
        if (per ? tsc() - t0 > per * max_ms : i > 50000000u) return 0;
        __asm__ volatile("pause");
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
    if (!(lo & (1u << 11))) { lo |= 1u << 11; wrmsr(0x1B, lo, hi); }           /* off -> xAPIC */
    if (g_x2 && !(lo & (1u << 10))) wrmsr(0x1B, lo | (1u << 10), hi);         /* xAPIC -> x2APIC */
    topo_self(&g_topo[cpu]);
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
    if (!base && !g_x2) return 0;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    if (g_x2) {
        /* off -> xAPIC -> x2APIC (off -> x2APIC directly is not allowed) */
        if (!(lo & (1u << 11))) { lo = (lo & ~(1u << 10)) | (1u << 11); wrmsr(0x1B, lo, hi); }
        if (!(lo & (1u << 10))) wrmsr(0x1B, lo | (1u << 10), hi);
    } else {
        wrmsr(0x1B, (lo & ~(1u << 10)) | (1u << 11), hi);     /* xAPIC mode, on */
        g_lapic = (volatile uint32_t*)(uintptr_t)base;
    }
    g_apic_on = 1;
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_LVT_ERR, LVT_MASKED);
    /* the 8259 through LINT0 (ExtINT) - unless it comes through an I/O APIC input */
    lapic_wr(LAPIC_LVT_LINT0, g_extint_ioapic ? LVT_MASKED : 0x700);
    lapic_wr(LAPIC_LVT_LINT1, 0x400);                     /* NMI */
    lapic_wr(LAPIC_SVR, 0x100u | SMP_VEC_SPURIOUS);
    __asm__ volatile("sti");
    /* does the timer still tick? (bounded: no hlt, it may not) */
    int ok = ticks_arrive(3, 100);
    if (!ok) {
        __asm__ volatile("cli");
        if (!lapic_x2apic_locked()) wrmsr(0x1B, lo & ~((1u << 11) | (1u << 10)), hi);
        g_lapic = NULL;
        g_apic_on = 0;
        g_x2 = 0;
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

static void lapic_timer_stop(void) {
    __asm__ volatile("cli");
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    irq_timer_from_lapic(0);                             /* the PIT's IRQ 0 again */
    __asm__ volatile("sti");
}

/* Only where it keeps counting while the core is halted: in a virtual
 * machine, or with CPUID's "always running APIC timer". On older real
 * processors it stops in power-saving states - the hlt of every idle wait
 * would then never end. */
static int lapic_timer_safe(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1u << 31)) return 1;                        /* a hypervisor */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 6) return 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(6), "c"(0));
    return (a >> 2) & 1;                                 /* ARAT */
}

static int lapic_timer_start(void) {
    if (!lapic_timer_safe()) return 0;
    lapic_wr(LAPIC_TIMER_DIV, 0x3);                       /* the bus clock / 16 */
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | SMP_VEC_TIMER);
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFFu);              /* one shot, counting down: how fast? */
    uint32_t c0 = timer_irq_count();                     /* 20 PIT ticks: 20 ms */
    if (!ticks_arrive(20, 200)) { lapic_wr(LAPIC_TIMER_INIT, 0); return 0; }
    uint32_t ms = timer_irq_count() - c0;
    uint32_t left = lapic_rd(LAPIC_TIMER_CUR);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    uint32_t per_ms = (0xFFFFFFFFu - left) / ms;
    if (per_ms < 100) return 0;

    __asm__ volatile("cli");
    irq_timer_from_lapic(1);                             /* the PIT's IRQ 0 masked */
    lapic_wr(LAPIC_LVT_TIMER, SMP_VEC_TIMER | (1u << 17)); /* periodic */
    lapic_wr(LAPIC_TIMER_INIT, per_ms);
    __asm__ volatile("sti");
    if (ticks_arrive(5, 100)) return 1;
    lapic_timer_stop();                                  /* it does not tick: back to the PIT */
    return 0;
}

/* everything as it was before smp_init: the local APIC off, the firmware's I/O APIC setup */
static void lapic_revert(void) {
    __asm__ volatile("cli");
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    irq_timer_from_lapic(0);
    ioapic_restore();
    uint32_t lo, hi;
    rdmsr(0x1B, &lo, &hi);
    if (!lapic_x2apic_locked()) wrmsr(0x1B, lo & ~((1u << 11) | (1u << 10)), hi);
    g_lapic = NULL;
    g_apic_on = 0;
    g_x2 = 0;
    __asm__ volatile("sti");
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
    const sdt_t* madt = r ? find_table(r, "APIC", 0) : NULL;
    if (!madt) { klog("smp: no ACPI MADT - using one core\n"); return; }

    static uint32_t ids[256];
    int n = 0, big = 0;
    topo_self(&g_topo[0]);
    uint32_t ioapic[4] = { 0, 0, 0, 0 };
    int nio = 0;
    uint32_t lapic_addr = *(const uint32_t*)((const uint8_t*)madt + 36);
    for (uint32_t o = 44; o + 2 <= madt->len;) {
        const uint8_t* e = (const uint8_t*)madt + o;
        if (e[1] < 2) break;
        if (e[0] == 0 && e[1] >= 8) {                     /* a processor's local APIC */
            uint32_t flags;
            memcpy(&flags, e + 4, 4);
            int dup = 0;
            for (int k = 0; k < n; k++) if (ids[k] == e[3]) dup = 1;
            if ((flags & 1) && n < 256 && !dup) ids[n++] = e[3];
        } else if (e[0] == 9 && e[1] >= 16) {             /* a processor's local x2APIC */
            uint32_t id, flags;
            memcpy(&id, e + 4, 4);
            memcpy(&flags, e + 8, 4);
            int dup = 0;
            for (int k = 0; k < n; k++) if (ids[k] == id) dup = 1;
            if ((flags & 1) && n < 256 && !dup && id != 0xFFFFFFFFu) { ids[n++] = id; if (id > 254) big = 1; }
        } else if (e[0] == 1 && e[1] >= 12 && nio < 4) {  /* an I/O APIC */
            memcpy(&ioapic[nio++], e + 4, 4);
        }
        o += e[1];
    }
    g_found = n;
    if (n < 2) { klog("smp: one processor core\n"); return; }

    /* x2APIC when the firmware used it (or locked it), or ids need it */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int x2_ok = (c >> 21) & 1;
    g_x2 = x2_ok && (lapic_boot_x2apic() || lapic_x2apic_locked() || big);
    if (lapic_x2apic_locked()) g_x2 = 1;
    for (int i = 0; i < nio; i++) ioapic_save(ioapic[i]);
    if (!lapic_on(lapic_addr)) {
        klog("smp: the local APIC would stop the timer - using one core\n");
        return;
    }
    ioapic_mask_devices();
    if (lapic_timer_start()) klog("smp: the 1 kHz tick comes from the local APIC timer\n");
    else klog("smp: the PIT stays the timer\n");
    /* the last word: if the timer no longer ticks now, nothing of this stays */
    if (!ticks_arrive(5, 200)) {
        lapic_revert();
        klog("smp: the timer stopped with the local APIC on - it is off again, using one core\n");
        return;
    }
    uint32_t bsp = lapic_id();
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
        if (!g_x2 && ids[i] > 254) { klog("smp: core (APIC id %u) needs x2APIC - left off\n", ids[i]); continue; }
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
    topo_link(g_cpus);
    klog("smp: %d of %d logical processors running (%s): %d physical cores, %d thread%s per core\n", g_cpus, n,
         g_x2 ? "x2APIC" : "xAPIC", g_nphys, g_tpc, g_tpc == 1 ? "" : "s");
    for (int i = 0; i < g_cpus; i++)
        klog("smp:   cpu %d: APIC id %u, package %u, core %u, thread %u%s\n", i, g_topo[i].apic, g_topo[i].pkg,
             g_topo[i].core, g_topo[i].smt, g_topo[i].type == CPU_TYPE_PERF ? ", performance core" :
             g_topo[i].type == CPU_TYPE_EFF ? ", efficient core" : "");
}

#endif
