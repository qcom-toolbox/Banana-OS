/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROCFS_H
#define PROCFS_H
/* /proc, as Linux has it, made when it is read (kernel/procfs.c): cpuinfo,
 * meminfo, stat, uptime, loadavg, version, mounts, filesystems, cmdline,
 * swaps, sys/kernel/..., self and one folder a process (stat, status,
 * cmdline, comm, statm, exe). Apps open it through kernel/app.c; nothing of
 * it is a file of kernel/fs.c (only the empty folder /proc, so that cd and
 * Files find it). /sys/devices/system/cpu is real files, written at the
 * start (procfs_boot). */
#include "types.h"
#include "../sdk/include/banana_api.h"

void procfs_boot(void);

/* a process, as /proc/<pid> shows it (kernel/app.c fills them) */
typedef struct {
    banana_procinfo_t i;
    uint32_t ruid, euid, rgid, egid;
    char     exe[128];
} procfs_proc_t;
int  app_proc_snapshot(procfs_proc_t* out, int max);   /* how many */
int  app_current_pid(void);                            /* the calling process's (0: none) */

/* path (absolute or from the current folder) is in /proc: its absolute,
 * clean form in out (1), else 0 */
int  procfs_path(const char* path, char* out, int cap);
/* what is there (an absolute /proc path): 0 nothing (ENOENT), 1 a file,
 * 2 a folder, 3 a link (self, exe) */
int  procfs_kind(const char* apath);
/* a file's text, made now (kmalloc'd, *len bytes); NULL if none */
char* procfs_read(const char* apath, uint32_t* len);
/* a folder's index-th entry: 1 (name, is_dir), 0 past the end */
int  procfs_entry(const char* apath, int index, char* name, int cap, int* is_dir);
/* a link's target (self -> 42, exe -> the program): its length, -1 */
int  procfs_link(const char* apath, char* out, int cap);

#endif
