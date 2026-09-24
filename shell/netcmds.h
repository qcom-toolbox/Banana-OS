#ifndef NETCMDS_H
#define NETCMDS_H

/* Network shell commands (ifconfig, dhcp, ping, nslookup/host, netstat,
 * arp, curl, wget, cryptotest) plus `wallpaper` (shell/wpcmd.c).
 * Returns 1 if `line` was one of them (and ran it). */
int netcmd_dispatch(const char* line);

/* Splits a command line into argv (whitespace separated, "double" and
 * 'single' quotes group words). Modifies buf in place. Returns argc. */
int shell_split_args(char* buf, char** argv, int max_args);

#endif
