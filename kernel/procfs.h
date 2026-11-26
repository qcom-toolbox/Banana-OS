/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROCFS_H
#define PROCFS_H
/* /proc/cpuinfo and /sys/devices/system/cpu, written at the start (once the
 * processor cores run and the files are loaded): kernel/procfs.c */
void procfs_boot(void);
#endif
