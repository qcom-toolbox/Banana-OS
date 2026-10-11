#ifndef REPO_H
#define REPO_H

#include "types.h"
#include "pkg.h"

/*
 * Package repositories, apt style: `apt update` fetches each source's
 * package index, `apt install <name>` downloads a package (and what it
 * depends on) and installs it with pkg. The App Store uses the same calls.
 *
 * Sources: /etc/pkg/sources.list, one per line -
 *
 *     repo https://packages.example.org/banana key=<64 hex digits>
 *     repo file:///mnt/usb/repo trusted
 *     repo file:///home/banana/Examples trusted
 *
 * A repository is a folder served over http(s) (or a local one) with
 *
 *     Packages        the index: one stanza per package ("Key: value"
 *                     lines, a blank line between them - see below)
 *     Packages.sig    its Ed25519 signature, 128 hex digits
 *     pool/...        the .bpk files
 *     icons/...       pictures for the App Store (optional)
 *
 * made by tools/repo/banana-repo on Linux. A source with key= must have
 * a good signature; one marked trusted needs none (local folders). A
 * local folder without an index is read as a flat repository: its .bpk
 * files are the packages.
 *
 *     Package: paint            Version: 1.2           Arch: i686 x86_64
 *     Title: Paint              Type: gui              Category: Graphics
 *     Description: Draw with the mouse
 *     Author: ...               Depends: libfoo (>= 1.0), bar | baz
 *     Filename: pool/paint_1.2.bpk   Size: 41234   SHA256: <hex>
 *     Icon: icons/paint.png
 */

#define REPO_SOURCES "/etc/pkg/sources.list"
#define REPO_LISTS   "/var/lib/pkg/lists"
#define REPO_ICONS   "/var/cache/pkg/icons"

typedef struct {
    char     name[PKG_NAME_MAX];
    char     version[24];
    char     title[48];
    char     type[8];
    char     category[24];
    char     description[160];
    char     author[48];
    char     depends[128];
    char     arch[32];
    char     filename[160];      /* relative to the source's URL (or a full URL) */
    char     icon[96];
    char     sha256[65];
    uint32_t size;
    int      source;             /* index in the sources */
} repo_pkg_t;

typedef struct {
    char    url[200];
    uint8_t key[32];
    int     has_key, trusted;
} repo_source_t;

/* lines of what is going on (apt prints them), may be NULL */
typedef void (*repo_log_t)(void* ctx, const char* line);

/* the sources (re-read from sources.list each time) */
int  repo_sources(repo_source_t* out, int max);
/* adds a line to sources.list: 0, or -1 with msg */
int  repo_add_source(const char* url, const char* key_hex, int trusted, char* msg, int mcap);
/* removes the source with this URL (or number, 1-based): 0, or -1 */
int  repo_remove_source(const char* which, char* msg, int mcap);

/* fetches every source's index: how many packages are known, or -1 */
int  repo_update(repo_log_t log, void* ctx, char* msg, int mcap);
/* the known packages (from the last update): count, and each by index */
int  repo_count(void);
const repo_pkg_t* repo_at(int i);
/* the version to install for this computer (newest, right CPU), or NULL */
const repo_pkg_t* repo_candidate(const char* name);
/* 1 if the lists were never fetched */
int  repo_never_updated(void);

/* installs name and what it needs (or upgrades it): 0, or -1 with msg */
int  repo_install(const char* name, int reinstall, repo_log_t log, void* ctx, char* msg, int mcap);
/* every installed package with a newer version: how many were upgraded, -1 */
int  repo_upgrade(repo_log_t log, void* ctx, char* msg, int mcap);
/* how many installed packages have a newer version known */
int  repo_upgradable(void);
/* removes a package (and keeps nothing): 0, or -1 */
int  repo_remove(const char* name, char* msg, int mcap);
/* removes what was only installed for others and nothing needs any more */
int  repo_autoremove(repo_log_t log, void* ctx, char* msg, int mcap);
/* 1 if name was installed only as something another package needs */
int  repo_is_auto(const char* name);

/* a picture for the package (installed app's icon.png, or the repository's,
 * downloaded once): 0 with its path, or -1 */
int  repo_icon(const char* name, char* path, int cap);

/* what an update / install is doing: percent (0-100), or -1 when idle */
int  repo_status(char* msg, int mcap);

/* Debian-style version order: <0, 0, >0 */
int  repo_vercmp(const char* a, const char* b);

#endif
