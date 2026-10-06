#ifndef SMP_H
#define SMP_H

#include "types.h"

/*
 * Processor cores. The boot core runs the kernel; the others (once
 * started by smp_init()) run app threads.
 */

void smp_init(void);
/* cores in use (1 until the others are started) */
int  cpu_count(void);

#endif
