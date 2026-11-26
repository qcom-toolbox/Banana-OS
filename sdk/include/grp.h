/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _GRP_H
#define _GRP_H
#ifdef __cplusplus
extern "C" {
#endif
#include <sys/types.h>
struct group { char* gr_name; char* gr_passwd; gid_t gr_gid; char** gr_mem; };
struct group* getgrgid(gid_t gid);
int setgroups(size_t n, const gid_t* list);
int initgroups(const char* user, gid_t gid);
struct group* getgrnam(const char* name);
int getgrgid_r(gid_t gid, struct group* g, char* buf, size_t len, struct group** out);
int getgrnam_r(const char* name, struct group* g, char* buf, size_t len, struct group** out);
void setgrent(void);
struct group* getgrent(void);
void endgrent(void);
#ifdef __cplusplus
}
#endif
#endif
