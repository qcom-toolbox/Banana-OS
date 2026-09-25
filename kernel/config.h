#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

/*
 * Tiny "key=value" configuration files in /etc (one setting per line,
 * '#' starts a comment). They live in the filesystem like any other
 * file, so on an installed disk they survive reboots.
 */

#define CFG_NETWORK  "/etc/network.conf"   /* ifconfig/dhcp: interface + addresses */
#define CFG_SERVICES "/etc/rc.conf"        /* services started at boot */

/* 1 and the value in out if `key` is set, 0 otherwise */
int  cfg_get(const char* path, const char* key, char* out, int cap);
/* sets (value != NULL) or removes (value == NULL) `key`, keeping the
 * rest of the file; `header` is written at the top of a new file.
 * Returns 0, or -1 if the file could not be written. */
int  cfg_set(const char* path, const char* key, const char* value, const char* header);
/* writes the filesystem to the installed disk now, if there is one, so a
 * saved setting survives even a power cut; 1 if it was written */
int  cfg_persist(void);

#endif
