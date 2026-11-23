/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "latency.h"
#include "latency_stats.h"
#include "timer.h"
#include "smp.h"
#include "serial.h"
#include "meminfo.h"
#include "zswap.h"
#include "task.h"
#include "kstring.h"

volatile int latency_enabled;
static struct {
    latency_hist_t input, irq, poll, service, timer;
    uint64_t since, irq_start, last_poll, last_timer;
    uint32_t drops;
} g;
static uint32_t g_last_report;

#ifndef LATENCY_HOST_TEST
static uint64_t cycles(void) { return timer_counter(); }

/* Raw flag helpers deliberately avoid instrumenting the profiler itself. */
static uintptr_t pause_irqs(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void resume_irqs(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

#endif

uint64_t latency_stamp(void) {
    return latency_enabled && cpu_id() == 0 && timer_has_hardware_clock() ? cycles() : 0;
}

void latency_irq_begin(uintptr_t f) {
    if (cpu_id() == 0 && (f & 0x200) && !g.irq_start) g.irq_start = latency_stamp();
}

void latency_irq_end(uintptr_t f) {
    if (!(f & 0x200) || cpu_id() != 0 || !g.irq_start) return;
    uint64_t end = cycles();
    latency_hist_add(&g.irq, latency_elapsed_us(g.irq_start, end, timer_counter_per_ms()));
    g.irq_start = 0;
}

void latency_input(uint64_t stamp) {
    if (!stamp || !latency_enabled || cpu_id() != 0) return;
    uintptr_t f = pause_irqs();
    uint64_t now = cycles();
    /* Discard queued events from an earlier session, or a backwards counter. */
    if (stamp >= g.since && now >= stamp)
        latency_hist_add(&g.input, latency_elapsed_us(stamp, now, timer_counter_per_ms()));
    resume_irqs(f);
}

void latency_service(uint64_t stamp) {
    uint64_t now = latency_stamp();
    if (stamp && stamp >= g.since && now >= stamp)
        latency_hist_add(&g.service, latency_elapsed_us(stamp, now, timer_counter_per_ms()));
}

void latency_timer(void) {
    uint64_t now = latency_stamp();
    if (!now) return;
    if (g.last_timer && now >= g.last_timer)
        latency_hist_add(&g.timer, latency_elapsed_us(g.last_timer, now, timer_counter_per_ms()));
    g.last_timer = now;
}

void latency_drop(void) {
    if (!latency_enabled || cpu_id() != 0) return;
    if (g.drops != LATENCY_MAX) g.drops++;
}

void latency_input_poll(void) {
    uint64_t now = latency_stamp();
    if (!now) return;
    if (g.last_poll && now >= g.last_poll)
        latency_hist_add(&g.poll, latency_elapsed_us(g.last_poll, now, timer_counter_per_ms()));
    g.last_poll = now;
}

void latency_set(int on) {
    if (cpu_id() != 0) return;
    if (!on) {
        latency_enabled = 0;
        g.irq_start = 0;
        latency_report();
        return;
    }
    uintptr_t f = pause_irqs();
    latency_enabled = 0;
    memset(&g, 0, sizeof(g));
    g.since = cycles();
    g_last_report = timer_ms();
    latency_enabled = 1;
    resume_irqs(f);
    klog("latency: started boot-core session shared_counter_per_ms=%llu\n", (unsigned long long)timer_counter_per_ms());
}

void latency_report(void) {
    if (cpu_id() != 0) return;
    latency_hist_t input, irq, poll, service, timer;
    uint32_t drops;
    uintptr_t f = pause_irqs();
    input = g.input; irq = g.irq; poll = g.poll;
    service = g.service; timer = g.timer; drops = g.drops;
    resume_irqs(f);
    meminfo_t m;
    zswap_stats_t z;
    meminfo_get(&m);
    zswap_stats(&z);
    klog("latency: ms=%u enabled=%d timing=%d input_n=%u input_p95_le_us=%u input_max_us=%u drops=%u\n",
         timer_ms(), latency_enabled, timer_has_hardware_clock() != 0, input.count,
         latency_hist_p95(&input), input.max_us, drops);
    klog("latency: irq_n=%u irq_p95_le_us=%u irq_max_us=%u poll_n=%u poll_p95_le_us=%u poll_max_us=%u\n",
         irq.count, latency_hist_p95(&irq), irq.max_us,
         poll.count, latency_hist_p95(&poll), poll.max_us);
    klog("latency: handler_n=%u handler_max_us=%u timer_n=%u timer_p95_le_us=%u timer_max_us=%u\n",
         service.count, service.max_us, timer.count, latency_hist_p95(&timer), timer.max_us);
    klog("latency: ram_total_kb=%u ram_free_kb=%u ram_avail_kb=%u ram_largest_kb=%u\n",
         m.total_kb, m.free_kb, m.avail_kb, m.largest_kb);
    klog("latency: swap_pages=%u swap_bytes=%llu swap_disk_pages=%u swap_disk_used=%llu\n",
         z.pages, (unsigned long long)z.bytes, z.disk_pages, (unsigned long long)z.disk_used);
    klog("latency: swap_out=%llu swap_in=%llu disk_out=%llu disk_in=%llu\n",
         (unsigned long long)z.pages_out, (unsigned long long)z.pages_in,
         (unsigned long long)z.disk_out, (unsigned long long)z.disk_in);
    for (int c = 0; c < cpu_count(); c++)
        klog("latency: cpu=%d busy_pct=%u\n", c, task_core_pct(c));
}

void latency_poll(void) {
    if (!latency_enabled || (uint32_t)(timer_ms() - g_last_report) < 1000) return;
    g_last_report = timer_ms();
    latency_report();
}
