#ifndef PKG_H
#define PKG_H

#include "types.h"

/*
 * App packages (.bpk), made by the SDK's `bpkg` tool:
 *
 *   "BPK1"  uint32 count  count x { char name[56]; uint32 offset; uint32 size; }  data...
 *
 * (little endian). Every package has a "manifest" (key=value lines: name,
 * title, version, type = console | gui | driver, description, author) and the
 * program for one or both CPUs: "app-i686", "app-x86_64". Any other file
 * is app data. `pkg install` unpacks it all into /apps/<name>/.
 */

#define PKG_DIR      "/apps"
#define PKG_NAME_MAX 25

typedef struct {
    char name[PKG_NAME_MAX];
    char title[48];
    char version[16];
    char type[8];            /* "console" or "gui" */
    char description[96];
    char author[48];
    int  has_i686, has_x86_64;
    uint32_t files, bytes;
} pkg_info_t;

/* 0 on success; msg says what happened (or why not) */
int pkg_install(const char* bpk_path, char* msg, int mcap);
int pkg_install_mem(const uint8_t* data, uint32_t size, char* msg, int mcap);   /* a package in memory */
/* what a package in memory holds (0, or -1 with err) */
int pkg_inspect_mem(const uint8_t* data, uint32_t size, pkg_info_t* out, char* err, int ecap);
int pkg_remove(const char* name, char* msg, int mcap);
/* what a .bpk file holds, without installing it */
int pkg_inspect(const char* bpk_path, pkg_info_t* out, char* err, int ecap);
/* installed apps: how many (fills out[] up to max) */
int pkg_list(pkg_info_t* out, int max);
int pkg_list_drivers(pkg_info_t* out, int max);  /* the installed driver packages (type=driver) */
int pkg_get(const char* name, pkg_info_t* out);   /* 0 if installed */

/* starts installed app `name`: a console app runs in the calling shell
 * (or, from the desktop, in a new terminal window), a desktop app in a
 * task of its own. Returns the exit code (console, from a shell), 0, or
 * -1 with err. */
int pkg_run(const char* name, int argc, char** argv, int from_desktop, char* err, int ecap);

#endif
