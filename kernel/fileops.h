/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FILEOPS_H
#define FILEOPS_H

#include "types.h"
#include "fs.h"

/*
 * What Files and the desktop do with files: moving and copying (a folder
 * with all it holds), the clipboard they share, and the Recycle Bin.
 *
 * The Recycle Bin is ~/.Trash: files/ holds what was deleted (under a name
 * of its own if two had the same), info/<name>.trashinfo where it came
 * from ("Path=..."), so Restore puts it back.
 */

/* (the logged-in user's: fs_home_join) */
#define DESKTOP_DIR  fs_home_join("/Desktop")
#define TRASH_DIR    fs_home_join("/.Trash")
#define TRASH_FILES  fs_home_join("/.Trash/files")
#define TRASH_INFO   fs_home_join("/.Trash/info")

/* A free name: 0 on success; -1 and empty out on exhaustion/truncation. */
int  fileops_unique(const char* dir, const char* base, char* out, int cap);
int  fileops_copy_tree(const char* src, const char* dst, int depth);
/* src into the folder dstdir (copy, or move): 0, or -1 with why in err.
 * out (if given) gets where it went. */
int  fileops_transfer(const char* src, const char* dstdir, int copy, char* out, int cap, char* err, int ecap);
int  fileops_delete(const char* path);                   /* for good */
const char* fileops_base(const char* path);
void fileops_parent(const char* path, char* out, int cap);
int  fileops_is_dir(const char* path);
int  fileops_on_usb(const char* path);                    /* on a mounted stick (no Recycle Bin there) */

/* Whose rights: the logged-in user's - an administrator's (the system's)
 * between fileops_as_admin(1) and (0), once the prompt allowed it. After a
 * failed transfer: stage 1 if it was copied but the source could not go
 * (else nothing changed: a partial copy is undone), perm 1 if the rights
 * refused it. A failed paste item is told to on_fail's function. */
void fileops_as_admin(int on);
int  fileops_failed_stage(void);
int  fileops_failed_perm(void);
typedef void (*fileops_fail_fn)(const char* src, const char* dstdir, int copy, int stage, int perm);
void fileops_on_fail(fileops_fail_fn fn);

/* the clipboard (Ctrl+C / X / V in Files and on the desktop) */
#define CLIP_MAX 64
void fileops_clip_set(const char* const* paths, int n, int cut);
int  fileops_clip_count(void);
int  fileops_clip_cut(void);
/* the clipboard's files into dir: how many went; status says what happened */
int  fileops_paste(const char* dir, char* status, int cap);

/* the Recycle Bin */
int  trash_put(const char* path);                         /* 0, -1 */
int  trash_restore(const char* trashed_path);             /* a path in TRASH_FILES: back where it was */
int  trash_empty(void);                                   /* how many went for good */
int  trash_count(void);
int  in_trash(const char* path);                          /* the path is in TRASH_FILES */
void trash_origin(const char* trashed_path, char* out, int cap);   /* where it came from ("" unknown) */

#endif
