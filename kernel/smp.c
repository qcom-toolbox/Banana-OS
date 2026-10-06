#include "smp.h"

static int g_cpus = 1;

void smp_init(void) {}

int cpu_count(void) { return g_cpus; }
