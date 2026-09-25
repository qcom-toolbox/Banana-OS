#ifndef SSHD_H
#define SSHD_H

#include "net.h"

/*
 * SSH server (SSH-2: RFC 4251-4254). Works with OpenSSH, PuTTY, Windows'
 * ssh.exe, ...:
 *   key exchange  curve25519-sha256 (RFC 8731), strict KEX (Terrapin fix)
 *   host key      ssh-ed25519, generated on first start
 *   ciphers       chacha20-poly1305@openssh.com, aes128-gcm@openssh.com
 *   login         password (`passwd`), user "banana"
 *   sessions      interactive shell or `ssh host <command>`, 2 at a time
 */

int  sshd_start(uint16_t port, char* err, int errcap);   /* 0 or -1 */
void sshd_stop(void);
int  sshd_running(void);
uint16_t sshd_port(void);
void sshd_print_status(void);
/* "SHA256:..." of the host key (generating the key if needed) */
int  sshd_fingerprint(char* out, int cap);

#endif
