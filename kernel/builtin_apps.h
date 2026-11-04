/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BUILTIN_APPS_H
#define BUILTIN_APPS_H

#include "types.h"

/* The apps that come with Banana OS (Media Player, Music, Amethyst Music: apps/), built
 * into the kernel. At boot each is installed into /apps - or upgraded when
 * this kernel has a newer version; one that was removed stays removed
 * until a new version comes, and one an install profile left out stays out
 * until it is installed again by hand. */
void builtin_apps_install(void);

/* ── the App Store's "builtin:" source (kernel/repo.c) ─────────────────
 * The built-in apps and the SDK's examples are packages of a source that
 * needs no network: `repo builtin: trusted` in /etc/pkg/sources.list. So an
 * app removed (or left out by the minimal and standard install profiles)
 * can always be installed again from the App Store or with apt. */
int         builtin_package_count(void);
const char* builtin_package_file(int i);              /* "mediaplayer.bpk", "paint.bpk" */
/* the package with that file name, in a fresh buffer (kfree): 0, or -1 */
int         builtin_package(const char* file, char** data, uint32_t* len);
uint32_t    builtin_packages_stamp(void);                  /* changes with this kernel's packages */
/* what the build found out about package file (tools/mkbuiltinmeta.py): its
 * unpacked size and SHA-256, and a skeleton of it - its file table and
 * manifest - for pkg_inspect_mem; 0, or -1 if there is none that matches */
int         builtin_package_meta(const char* file, uint32_t* size, uint8_t sha256[32],
                                 const uint8_t** skel, uint32_t* skel_len);
/* selftest: those facts against each package itself, unpacked and hashed -
 * how many agree, or -1 and which did not in msg */
int         builtin_meta_check(char* msg, int cap);
/* on an installed system: the packages saved as files on the disk
 * (/usr/share/builtin, read when wanted) and the kernel's copies of them
 * given to the heap - after builtin_apps_install(), at boot. The live CD
 * keeps them. What was given back, in bytes (meminfo) */
void        builtin_release(void);
uint32_t    builtin_released_bytes(void);

/* ── install profiles (the installer, `install --minimal`) ──────────── */
#define PROFILE_FULL     0     /* every built-in app and the examples */
#define PROFILE_STANDARD 1     /* the everyday apps: App Store, Media Player, Music, Photos */
#define PROFILE_MINIMAL  2     /* the App Store only: the rest can be installed from it */
#define PROFILE_COUNT    3
const char* builtin_profile_name(int profile);        /* "Full", "Standard", "Minimal" */
const char* builtin_profile_apps(int profile);        /* what it installs, in a line */
/* removes from this system what the profile leaves out (apps and ~/Examples):
 * they stay out at the next boots, and installable from the App Store;
 * how many apps were removed */
int         builtin_profile_apply(int profile);

/* an app installed again from the kernel's built-in copy: 0; 1 it will be
 * at the next start; -1 no such built-in app; -2 it failed (msg says why) */
int builtin_app_reinstall(const char* name, char* msg, int mcap);

#endif
