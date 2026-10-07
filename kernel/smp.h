#ifndef SMP_H
#define SMP_H

#include "types.h"

/*
 * Processor cores. The boot core runs the kernel; the others (once
 * started by smp_init(), 64-bit kernel) run app code - see kernel/task.h.
 */

#define SMP_MAX_CPUS 16
#define SMP_VEC_KICK 0xF0          /* IPI: look at your work (kernel/task.c, task_ipi) */
#define SMP_VEC_TIMER 0xF1         /* the boot core's local APIC timer: the 1 kHz tick */
#define SMP_VEC_SPURIOUS 0xFF

void smp_init(void);
/* cores in use (1 until the others are started) */
int  cpu_count(void);
/* the core this runs on: 0 = the boot core */
static inline int cpu_id(void) {
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    return tr <= 0x10 ? 0 : (int)(tr - 0x10) / 16;   /* each core's TSS (kernel/idt.c) */
}
/* an interrupt to another core (vector SMP_VEC_KICK) */
void smp_send_ipi(int cpu, int vector);
/* end of a local-APIC interrupt (IPIs) */
void smp_eoi(void);
/* the cores found in the ACPI tables (including ones not started) */
int  smp_cores_found(void);

#endif
