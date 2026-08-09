#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAX_FILES     64
#define FS_MAX_DIRS      32
#define FS_NAME_LEN      32
#define FS_CONTENT_LEN   2048
#define FS_PATH_LEN      128

typedef struct {
    char     name[FS_NAME_LEN];
    char     content[FS_CONTENT_LEN];
    int      used;
    int      parent_dir; /* index into dirs[] */
} fs_file_t;

typedef struct {
    char     name[FS_NAME_LEN];
    int      used;
    int      parent_dir; /* -1 = root */
} fs_dir_t;

void fs_init(void);

/* directory ops - all paths may be absolute ("/a/b"), relative ("a/b"),
 * use "." / ".." / "~" (home), same as a real Unix shell. */
int  fs_mkdir(const char* path);           /* returns dir index or -1 */
int  fs_mkdir_p(const char* path);         /* mkdir -p: create intermediate dirs too */
int  fs_find_dir(const char* path);        /* returns dir index or -1 */
void fs_ls(const char* path);              /* NULL/"" lists cwd */
void fs_ls_long(const char* path);         /* ls -l style */

/* file ops */
int  fs_create(const char* path);          /* returns file index or -1 */
int  fs_find_file(const char* path);       /* returns index or -1 */
fs_file_t* fs_get_file(int idx);
void fs_delete(const char* path, int recursive); /* rm [-r] */
int  fs_copy(const char* src, const char* dst);  /* cp */
int  fs_move(const char* src, const char* dst);  /* mv */

/* navigation */
void fs_cd(const char* path);              /* NULL/"" or "~" goes home */
void fs_pwd(void);
const char* fs_cwd_name(void);             /* leaf name of cwd only */
void fs_cwd_path(char* buf, int buflen);   /* full absolute path of cwd */

/* recursively lists everything under path (NULL/"" = cwd); if name_filter
 * is non-empty, only entries whose name contains it are printed */
void fs_find(const char* path, const char* name_filter);

/* stats */
uint32_t fs_used_files(void);
uint32_t fs_used_dirs(void);
uint32_t fs_max_files(void);
uint32_t fs_max_dirs(void);
uint32_t fs_ram_used_bytes(void);

/* ── on-disk persistence (see kernel/fsdisk.c) ──────────────────────
 * Dump/restore the raw dirs[]/files[] tables plus home_dir as an opaque
 * blob - fsdisk.c writes/reads that blob to/from disk with no knowledge
 * of fs.c's internal layout. */
uint32_t fs_snapshot_size(void);
void     fs_snapshot_save(uint8_t* buf);       /* buf must be >= fs_snapshot_size() bytes */
int      fs_snapshot_load(const uint8_t* buf); /* returns 0 on success */

#endif
