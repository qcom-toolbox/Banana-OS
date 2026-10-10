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

#define DESKTOP_DIR  "/home/banana/Desktop"
#define TRASH_DIR    "/home/banana/.Trash"
#define TRASH_FILES  TRASH_DIR "/files"
#define TRASH_INFO   TRASH_DIR "/info"

/* a name like base, free in dir: "name", then "name (2)"... */
void fileops_unique(const char* dir, const char* base, char* out, int cap);
int  fileops_copy_tree(const char* src, const char* dst, int depth);
/* src into the folder dstdir (copy, or move): 0, or -1 with why in err.
 * out (if given) gets where it went. */
int  fileops_transfer(const char* src, const char* dstdir, int copy, char* out, int cap, char* err, int ecap);
int  fileops_delete(const char* path);                   /* for good */
const char* fileops_base(const char* path);
void fileops_parent(const char* path, char* out, int cap);
int  fileops_is_dir(const char* path);
int  fileops_on_usb(const char* path);                    /* on a mounted stick (no Recycle Bin there) */

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
