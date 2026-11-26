/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef USERCMDS_H
#define USERCMDS_H

/* passwd, adduser / useradd, deluser / userdel, usermod, lsusers
 * (shell/usercmds.c): 1 if line was one of them (and ran it) */
int usercmd_dispatch(const char* line);

#endif
