#ifndef SRVCMDS_H
#define SRVCMDS_H

/* Services: `httpd`, `sshd`, `passwd` (shell/srvcmds.c).
 * Returns 1 if `line` was one of them (and ran it). */
int srvcmd_dispatch(const char* line);

/* Once the filesystem is loaded at boot: applies /etc/network.conf and
 * starts the services enabled in /etc/rc.conf. */
void services_boot(void);

#endif
