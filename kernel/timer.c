/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "timer.h"
#include "latency.h"
#include "types.h"
#include "idt.h"
#include "io.h"
#include "terminal.h"
#include "serial.h"
#include "task.h"
#include "gui.h"
#include "audio.h"
#include "splash.h"
#include "gpu.h"
#include "prof.h"
#include "smp.h"
#include "kstring.h"
#include "timer_math.h"

/* IRQ0 still wakes sleepers at 1 kHz. Elapsed time comes from a shared
 * hardware counter, not the CPU's TSC: even a hypervisor advertising an
 * invariant TSC can inherit a broken counter from its host. HPET is the
 * first choice, then the ACPI PM timer; IRQ counting is the last fallback.
 * HPET comparators/routing are untouched: the PIT/local APIC delivers IRQ0. */
#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_HZ 1000
#define PIT_BASE 1193182u
static volatile uint32_t g_ms;
static volatile uint32_t* g_hpet;
static uint32_t g_period_fs, g_mask;
static uint16_t g_pm_port;
static int g_hpet64;
static uint64_t g_origin, g_ns_base;
/* Only IRQ0 writes the extension. Readers on every core use a sequence
 * counter, so a 24/32-bit rollover never exposes a torn 64-bit epoch. */
static volatile uint32_t g_seq, g_last;
static volatile uint64_t g_elapsed, g_irq_ns;

static uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t)u32(p+4)<<32; }
static int table_ok(const uint8_t* p, unsigned minimum) {
    if (!p || (uintptr_t)p<4096 || (uint64_t)(uintptr_t)p>(1ull<<32)-36) return 0;
    unsigned n=u32(p+4);
    if(n<minimum || n>4096 || n>(1ull<<32)-(uintptr_t)p) return 0;
    uint8_t sum=0;for(unsigned i=0;i<n;i++)sum+=p[i];return !sum;
}
static uint64_t hpet_read(void) {
    if(!g_hpet64)return g_hpet[0xf0/4];
#ifdef __x86_64__
    return *(volatile uint64_t*)(g_hpet+0xf0/4);
#else
    uint32_t hi,lo,again;
    do { hi=g_hpet[0xf4/4];lo=g_hpet[0xf0/4];again=g_hpet[0xf4/4]; } while(hi!=again);
    return (uint64_t)hi<<32 | lo;
#endif
}
static uint32_t narrow_read(void) { return g_hpet?(uint32_t)hpet_read():inl(g_pm_port); }
static int counter_moves(void) {
    uint32_t first=narrow_read();
    for(unsigned i=0;i<100000;i++)if(narrow_read()!=first)return 1;
    return 0;
}
static void choose_clock(void) {
    const uint8_t* h=acpi_find_table("HPET");
    if(table_ok(h,56) && h[40]==0 && !h[42]) {
        uint64_t address=u64(h+44);
        if(address>=4096 && address<=(1ull<<32)-0x400 && !(address&7)) {
            g_hpet=(volatile uint32_t*)(uintptr_t)address;
            uint32_t cap=g_hpet[0],old=g_hpet[0x10/4];g_period_fs=g_hpet[1];
            if((cap&0xff) && g_period_fs && g_period_fs<=100000000u) {
                g_hpet64=!!(cap&(1u<<13));g_hpet[0x10/4]=old|1;
                if(counter_moves()) { g_mask=0xffffffffu;return; }
                g_hpet[0x10/4]=old;
            }
            g_hpet=NULL;g_hpet64=0;
        }
    }
    const uint8_t* f=acpi_find_table("FACP");
    if(table_ok(f,116) && !(u32(f+112)&(1u<<20))) {
        uint64_t port=u32(f+76);
        if(u32(f+4)>=220 && f[208]==1 && f[209]>=32 && !f[210] && u64(f+212))port=u64(f+212);
        if(port && port<=65532 && f[91]>=4) {
            g_pm_port=(uint16_t)port;g_mask=u32(f+112)&(1u<<8)?0xffffffffu:0xffffffu;
            if(counter_moves())return;
            g_pm_port=0;
        }
    }
}
int timer_has_hardware_clock(void) { return g_hpet || g_pm_port; }
const char* timer_clock_source(void) {
    return g_hpet?(g_hpet64?"HPET 64-bit":"HPET 32-bit"):g_pm_port?"ACPI PM timer":"timer interrupts";
}
static void clock_reset(uint64_t ns) {
    g_ns_base=ns;
    g_origin=g_hpet64?hpet_read():0;
    g_last=(g_hpet || g_pm_port)?narrow_read():0;
    g_elapsed=0;g_irq_ns=0;g_seq=0;
}
uint64_t timer_counter(void) {
    if(g_hpet64)return g_ns_base+timer_hpet_ns(hpet_read()-g_origin,g_period_fs);
    uint32_t seq,last,raw;uint64_t elapsed;
    do {
        seq=__atomic_load_n(&g_seq,__ATOMIC_ACQUIRE);
        if(seq&1)continue;
        last=g_last;elapsed=(g_hpet || g_pm_port)?g_elapsed:g_irq_ns;
        raw=(g_hpet || g_pm_port)?narrow_read():last;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(seq==__atomic_load_n(&g_seq,__ATOMIC_RELAXED))break;
    } while(1);
    if(!g_hpet && !g_pm_port)return g_ns_base+elapsed;
    elapsed+=timer_wrap_delta(raw,last,g_mask);
    return g_ns_base+(g_hpet?timer_hpet_ns(elapsed,g_period_fs):timer_pm_ns(elapsed));
}
uint64_t timer_counter_per_ms(void) { return 1000000ull; } /* nanoseconds */
uint32_t timer_irq_count(void) { return g_ms; }

static void timer_irq(void) {
    if (latency_enabled) latency_timer();
    __atomic_add_fetch(&g_seq,1,__ATOMIC_ACQ_REL);
    if(!g_hpet64 && (g_hpet || g_pm_port)) {
        uint32_t now=narrow_read();g_elapsed+=timer_wrap_delta(now,g_last,g_mask);g_last=now;
    }
    g_irq_ns+=1000000ull;
    __atomic_add_fetch(&g_seq,1,__ATOMIC_RELEASE);
    g_ms++;
    serial_kick();      /* drain the serial console's output queue */
    task_smp_tick();    /* app code on the other cores takes turns (kernel/task.c) */
    gui_cursor_tick();  /* the mouse pointer moves even while the desktop's task is busy */
    audio_tick();       /* the sound card's ring stays fed, whatever the tasks do */
    gpu_load_tick(g_ms);/* the GPU's busy bit sampled (only while Task Manager looks) */
    prof_tick(g_ms);    /* prof: the other cores looked at on this tick too */
    splash_tick();      /* the boot screen's dots */
}

void timer_init(void) {
    choose_clock();clock_reset(0);
    klog("clock: %s (shared across cores)\n",timer_clock_source());
    uint32_t divisor = PIT_BASE / PIT_HZ;
    outb(PIT_CMD, 0x34);                 /* channel 0, lo/hi byte, mode 2 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
    g_ms = 0;
    irq_install(0, timer_irq);
}

uint32_t timer_ms(void) {
    return (uint32_t)(timer_counter()/1000000ull);
}

void timer_s3_resume(uint64_t before_ns) {
    if(g_hpet)g_hpet[0x10/4]|=1;
    clock_reset(before_ns);
    uint32_t divisor=PIT_BASE/PIT_HZ;
    outb(PIT_CMD,0x34);outb(PIT_CH0,(uint8_t)divisor);outb(PIT_CH0,(uint8_t)(divisor>>8));
}

uint32_t timer_ticks(void) {
    return timer_ms() / 10u;
}

void timer_poll(void) {
}

void timer_idle(void) {
    /* sti's one-instruction shadow makes "sti; hlt" atomic: an interrupt
     * can't sneak in between and leave us halted with nothing to wake us */
    __asm__ volatile("sti; hlt" ::: "memory");
}

void timer_sleep_ms(uint32_t ms) {
    terminal_flush();   /* whatever was printed before a blocking wait shows up */
    if (ms == 0) return;
    uint32_t start = timer_ms();
    while ((uint32_t)(timer_ms() - start) < ms) timer_idle();
}
