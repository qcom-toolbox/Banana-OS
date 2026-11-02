/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tlb.h"
#include "smp.h"
#include "paging.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "uvm.h"

#ifdef __x86_64__

#define CR4_PGE     (1ull << 7)
#define CR4_PCIDE   (1ull << 17)
#define CR3_NOFLUSH (1ull << 63)        /* loading CR3: keep what the TLB has under the PCID */
#define CR3_ADDR    0x000FFFFFFFFFF000ull
#define P_PRESENT   0x001ull
#define P_WRITE     0x002ull
#define P_LARGE     0x080ull
#define P_GLOBAL    0x100ull
#define NSLOT       63                  /* PCIDs 1..63 on each core (0: the kernel's tables) */

/* a PCID of one core: the address space it is for, at which generation */
typedef struct { uint64_t id; uint32_t gen; } slot_t;

typedef struct {
    slot_t   slot[NSLOT];
    uint32_t next;                      /* the slot given to a new space next */
    uint32_t kgen;                      /* the kernel tables' changes it forgot everything for */
    uint64_t id;                        /* without PCIDs: the space loaded, */
    uint32_t gen;                       /* ... at this generation */
    int      dirty0;                    /* PCID 0 held tables that were not the kernel's */
    tlb_stats_t st;
} __attribute__((aligned(64))) core_t;

static core_t g_core[SMP_MAX_CPUS];
static volatile uint32_t g_kgen;
static volatile uint64_t g_next_id = 1;
static int g_global, g_pcid = -1;       /* (-1: no core looked yet) */

static uintptr_t rd_cr3(void) { uintptr_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static void wr_cr3(uintptr_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static uintptr_t rd_cr4(void) { uintptr_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static void wr_cr4(uintptr_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

static uintptr_t irq_off(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void irq_restore(uintptr_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

void tlb_cpu_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uintptr_t cr4 = rd_cr4();
    if (d & (1u << 13)) {
        cr4 |= CR4_PGE;
        wr_cr4(cr4);
        if (cpu_id() == 0) g_global = 1;
    }
    /* PCIDs only if every core has them (and CR3 names PCID 0 now, as it
     * must when they are turned on) */
    int has = (c & (1u << 17)) && (cr4 & CR4_PGE);
    if (g_pcid < 0) g_pcid = has;
    else if (!has) g_pcid = 0;
    if (g_pcid && !(rd_cr3() & 0xFFF)) wr_cr4(cr4 | CR4_PCIDE);
    int cpu = cpu_id();
    if (cpu >= 0 && cpu < SMP_MAX_CPUS) {
        core_t* k = &g_core[cpu];
        /* (after a wake from sleep the TLB is empty: nothing kept counts) */
        for (int i = 0; i < NSLOT; i++) k->slot[i].id = 0;
        k->id = (uint64_t)-1;
        k->dirty0 = 1;                  /* (and CR3 may be an app's, under PCID 0) */
        k->kgen = g_kgen;
    }
}

int tlb_has_global(void) { return g_global; }
int tlb_has_pcid(void) { return g_pcid > 0 && (rd_cr4() & CR4_PCIDE); }

void tlb_tag_init(tlb_tag_t* t) {
    t->id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
    t->gen = 0;
}

void tlb_space_changed(tlb_tag_t* t) {
    /* (a locked add: also a full fence - the tables' change is out before
     * smp_cur_cr3 is looked at, kernel/smp.c) */
    __atomic_add_fetch(&t->gen, 1, __ATOMIC_SEQ_CST);
}

void tlb_flush_all(void) {
    uintptr_t cr4 = rd_cr4();
    if (cr4 & CR4_PGE) {
        wr_cr4(cr4 & ~CR4_PGE);         /* (turning PGE off and on: every PCID, global pages too) */
        wr_cr4(cr4);
    } else {
        wr_cr3(rd_cr3());
    }
}

void tlb_kernel_changed(int others_only) {
    uintptr_t f = irq_off();
    uint32_t old = __atomic_fetch_add(&g_kgen, 1, __ATOMIC_SEQ_CST);
    int cpu = cpu_id();
    if (!others_only) tlb_flush_all();
    /* this core is done - unless it still owed an older change */
    if (cpu >= 0 && cpu < SMP_MAX_CPUS && g_core[cpu].kgen == old) g_core[cpu].kgen = old + 1;
    irq_restore(f);
}

/* (interrupts off: a task switch in the middle could give the PCID chosen
 * here to another space) */
static void load(uintptr_t cr3, const tlb_tag_t* tag) {
    int cpu = cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) {
        if ((rd_cr3() & CR3_ADDR) != cr3) wr_cr3(cr3);
        return;
    }
    core_t* k = &g_core[cpu];
    smp_cur_cr3[cpu] = cr3;
    /* (before the generation is read: a core changing the space sees this
     * one on it and asks it to forget - or this one sees the change) */
    __asm__ volatile("mfence" ::: "memory");
    k->st.loads++;
    uint32_t kg = g_kgen;
    if (k->kgen != kg) {
        k->kgen = kg;
        tlb_flush_all();
        k->st.full++;
    }
    uintptr_t have = rd_cr3();
    if (!tlb_has_pcid()) {
        /* loading CR3 forgets all but the global pages: only if it is
         * another space, or this one changed */
        uint64_t id = tag ? tag->id : 0;
        uint32_t gen = tag ? tag->gen : 0;
        if ((have & CR3_ADDR) == cr3 && k->id == id && k->gen == gen) { k->st.kept++; return; }
        wr_cr3(cr3);
        k->id = id;
        k->gen = gen;
        k->st.forgot++;
        return;
    }
    if (!tag) {
        /* the kernel's tables: PCID 0 (only changes of the kernel's tables
         * make it forget, above) */
        if (k->dirty0 || cr3 != paging_kernel_cr3()) {
            k->dirty0 = cr3 != paging_kernel_cr3();     /* (tables of nobody's: kept under no PCID) */
            wr_cr3(cr3);
            k->st.forgot++;
            return;
        }
        if (have != cr3) wr_cr3(cr3 | CR3_NOFLUSH);
        k->st.kept++;
        return;
    }
    uint32_t gen = tag->gen;
    int i = 0;
    while (i < NSLOT && k->slot[i].id != tag->id) i++;
    int fresh = i < NSLOT && k->slot[i].gen == gen;
    if (i == NSLOT) {                   /* new to this core: the oldest one's PCID */
        i = (int)k->next;
        k->next = (k->next + 1) % NSLOT;
        k->slot[i].id = tag->id;
    }
    k->slot[i].gen = gen;
    uintptr_t want = cr3 | (uintptr_t)(i + 1);
    if (fresh) {
        if (have != want) wr_cr3(want | CR3_NOFLUSH);
        k->st.kept++;
    } else {
        wr_cr3(want);                   /* (without NOFLUSH: what the PCID held goes) */
        k->st.forgot++;
    }
}

void tlb_load(uintptr_t cr3, const tlb_tag_t* tag) {
    uintptr_t f = irq_off();
    load(cr3 & CR3_ADDR, tag);
    irq_restore(f);
}

void tlb_stats(tlb_stats_t* s) {
    memset(s, 0, sizeof(*s));
    for (int c = 0; c < cpu_count() && c < SMP_MAX_CPUS; c++) {
        s->loads += g_core[c].st.loads;
        s->kept += g_core[c].st.kept;
        s->forgot += g_core[c].st.forgot;
        s->full += g_core[c].st.full;
    }
}

/* ── selftest tlb ─────────────────────────────────────────────────────
 * A space of its own: the kernel's tables, plus in an empty PML4 slot one
 * page table of its own (not global, as an app's) - 64 pages to touch and
 * one test page whose frame is changed under it. */

#define TOUCH 64

static uint64_t* table(void) {
    uint64_t* t = (uint64_t*)kmalloc_aligned(4096, 4096);
    if (t) memset(t, 0, 4096);
    return t;
}

static uint32_t peek(uintptr_t va) { return *(volatile uint32_t*)va; }

static void touch(uintptr_t base) {
    for (int i = 0; i < TOUCH; i++) (void)peek(base + (uintptr_t)i * 4096);
}

/* the kernel's entry for va: its last level */
static uint64_t kernel_leaf(uintptr_t va) {
    const uint64_t* t = (const uint64_t*)(paging_kernel_cr3() & CR3_ADDR);
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = t[(va >> shift) & 511];
        if (!(e & P_PRESENT)) return 0;
        if (shift == 12 || (shift < 39 && (e & P_LARGE))) return e;
        t = (const uint64_t*)(uintptr_t)(e & CR3_ADDR);
    }
    return 0;
}

#define FAIL(...) do { ksnprintf(msg, (size_t)cap, __VA_ARGS__); rc = -1; goto out; } while (0)

int tlb_selftest(char* msg, int cap, uint32_t* old_ns, uint32_t* new_ns, int* kept_after) {
    int rc = 0, cpu = cpu_id();
    *old_ns = *new_ns = 0;
    *kept_after = -1;
    msg[0] = 0;
    if (g_global && !(kernel_leaf((uintptr_t)&tlb_selftest) & P_GLOBAL)) {
        ksnprintf(msg, (size_t)cap, "the kernel's code is not on a global page");
        return -1;
    }
    if (g_global && !(rd_cr4() & CR4_PGE)) {
        ksnprintf(msg, (size_t)cap, "CR4.PGE is off on this core");
        return -1;
    }
    const uint64_t* kpml4 = (const uint64_t*)(paging_kernel_cr3() & CR3_ADDR);
    int s = -1;
    for (int i = 255; i > 0 && s < 0; i--)
        if (!kpml4[i] && !(i >= (int)(UVM_BASE >> 39) && i < (int)(UVM_BASE >> 39) + UVM_SLOTS)) s = i;
    if (s < 0) { ksnprintf(msg, (size_t)cap, "no free PML4 slot"); return -1; }

    uint64_t *pml4 = table(), *pdpt = table(), *pd = table(), *pt = table();
    uint8_t* frames = (uint8_t*)kmalloc_aligned((TOUCH + 2) * 4096, 4096);
    uintptr_t f = 0, before = 0;
    int on = 0;
    if (!pml4 || !pdpt || !pd || !pt || !frames) { ksnprintf(msg, (size_t)cap, "out of memory"); rc = -1; goto done; }
    uint64_t A = (uint64_t)(uintptr_t)(frames + TOUCH * 4096), B = A + 4096;
    *(volatile uint32_t*)(uintptr_t)A = 0xA11CEu;
    *(volatile uint32_t*)(uintptr_t)B = 0xB0B0Bu;
    memcpy(pml4, kpml4, 4096);
    pml4[s] = (uint64_t)(uintptr_t)pdpt | P_PRESENT | P_WRITE;
    pdpt[0] = (uint64_t)(uintptr_t)pd | P_PRESENT | P_WRITE;
    pd[0] = (uint64_t)(uintptr_t)pt | P_PRESENT | P_WRITE;
    for (int i = 0; i < TOUCH; i++) pt[i] = ((uint64_t)(uintptr_t)frames + (uint64_t)i * 4096) | P_PRESENT | P_WRITE;
    pt[TOUCH] = A | P_PRESENT | P_WRITE;
    uintptr_t va = (uintptr_t)s << 39, test = va + TOUCH * 4096, kcr3 = paging_kernel_cr3();
    uintptr_t mine = (uintptr_t)pml4;
    tlb_tag_t tag;
    tlb_tag_init(&tag);

    f = irq_off();                      /* (no task switch while on these tables) */
    before = rd_cr3();
    on = 1;
    tlb_load(mine, &tag);
    if (peek(test) != 0xA11CEu) FAIL("the test page does not read what its frame holds");
    tlb_load(kcr3, NULL);
    pt[TOUCH] = B | P_PRESENT | P_WRITE;
    tlb_space_changed(&tag);
    tlb_load(mine, &tag);
    if (peek(test) != 0xB0B0Bu) FAIL("a changed space kept its old translation (generation %u)", tag.gen);
    tlb_stats_t* st = &g_core[cpu].st;  /* (this core's: the others go on switching) */
    tlb_stats_t s0 = *st;
    tlb_load(kcr3, NULL);
    tlb_load(mine, &tag);
    tlb_stats_t s1 = *st;
    if (peek(test) != 0xB0B0Bu) FAIL("the test page changed by itself");
    if (tlb_has_pcid() && s1.kept - s0.kept != 2) FAIL("going back to an unchanged space forgot it");
    /* changed WITHOUT saying so: with PCIDs the processor may well still
     * have the old translation (information only - it may also have lost it) */
    tlb_load(kcr3, NULL);
    pt[TOUCH] = A | P_PRESENT | P_WRITE;
    tlb_load(mine, &tag);
    *kept_after = peek(test) == 0xB0B0Bu;
    tlb_space_changed(&tag);            /* (and right again) */
    tlb_load(kcr3, NULL);
    tlb_load(mine, &tag);
    if (peek(test) != 0xA11CEu) FAIL("the space did not come right after a change");
    /* a change to the kernel's tables: everything goes, on this core now */
    tlb_load(kcr3, NULL);
    s0 = *st;
    tlb_kernel_changed(0);
    uint32_t kg = g_kgen;
    tlb_load(mine, &tag);
    s1 = *st;
    if (peek(test) != 0xA11CEu) FAIL("the space is wrong after a kernel change");
    if (s1.full != s0.full && g_kgen == kg) FAIL("this core forgot everything twice for one kernel change");

    /* timed: TOUCH pages here and TOUCH of the kernel's, then the same
     * again - as before (no global pages, every CR3 load under PCID 0: each
     * forgets everything), and now */
    /* (the two ways alternate, 5 rounds, and each keeps its fastest: one
     *  round slowed by the host - an emulator's or a hypervisor's pause -
     *  must not decide the comparison) */
    uint64_t hz = timer_counter_per_ms();
    if (hz) {
        const int R = 100, ROUNDS = 5;
        uint64_t best_old = ~0ull, best_new = ~0ull;
        uintptr_t cr4 = rd_cr4();
        for (int k = 0; k < ROUNDS; k++) {
            wr_cr4(cr4 & ~CR4_PGE);
            uint64_t t0 = timer_counter();
            for (int r = 0; r < R; r++) {
                wr_cr3(mine); touch(va);
                wr_cr3(kcr3); touch((uintptr_t)frames);
            }
            uint64_t t1 = timer_counter();
            wr_cr4(cr4);                /* (and everything forgotten again) */
            for (int r = 0; r < R; r++) {
                tlb_load(mine, &tag); touch(va);
                tlb_load(kcr3, NULL); touch((uintptr_t)frames);
            }
            uint64_t t2 = timer_counter();
            if (t1 - t0 < best_old) best_old = t1 - t0;
            if (t2 - t1 < best_new) best_new = t2 - t1;
        }
        *old_ns = (uint32_t)(best_old * 1000000ull / hz / (uint64_t)R);
        *new_ns = (uint32_t)(best_new * 1000000ull / hz / (uint64_t)R);
    }

out:
    if (on) {
        /* the tables it was on before (raw: whatever they were) */
        wr_cr3(before & ~CR3_NOFLUSH);
        smp_cur_cr3[cpu] = before & CR3_ADDR;
        if (cpu >= 0 && cpu < SMP_MAX_CPUS) { g_core[cpu].id = (uint64_t)-1; g_core[cpu].dirty0 = 1; }
        irq_restore(f);
    }
done:
    kfree(pml4); kfree(pdpt); kfree(pd); kfree(pt); kfree(frames);
    return rc;
}

#else

void tlb_cpu_init(void) {}
void tlb_tag_init(tlb_tag_t* t) { t->id = 0; t->gen = 0; }
void tlb_space_changed(tlb_tag_t* t) { (void)t; }
void tlb_kernel_changed(int others_only) { (void)others_only; }
void tlb_flush_all(void) {}
void tlb_load(uintptr_t cr3, const tlb_tag_t* tag) { (void)cr3; (void)tag; }
int  tlb_has_global(void) { return 0; }
int  tlb_has_pcid(void) { return 0; }
void tlb_stats(tlb_stats_t* s) { memset(s, 0, sizeof(*s)); }
int  tlb_selftest(char* msg, int cap, uint32_t* old_ns, uint32_t* new_ns, int* kept_after) {
    (void)cap;
    msg[0] = 0;
    *old_ns = *new_ns = 0;
    *kept_after = -1;
    return 0;
}

#endif
