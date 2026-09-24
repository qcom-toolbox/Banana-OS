#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAX_FILES     256
#define FS_MAX_DIRS      64
#define FS_NAME_LEN      32
#define FS_PATH_LEN      128
/* Largest single file (downloads, images). File data lives on the
 * kernel heap, so the practical limit is free RAM. */
#define FS_MAX_FILE_SIZE (32u * 1024u * 1024u)

typedef struct {
    char     name[FS_NAME_LEN];
    char*    content;    /* heap buffer, always NUL-terminated at [size] */
    uint32_t size;       /* bytes of data (files may be binary) */
    uint32_t cap;        /* allocated bytes (>= size + 1) */
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

/* file data: replace / append. Return 0, or -1 (out of memory/too big). */
int  fs_write(int idx, const void* data, uint32_t len);
int  fs_append(int idx, const void* data, uint32_t len);
int  fs_set_text(int idx, const char* text);
/* create-or-truncate `path` and write data; returns file index or -1 */
int  fs_write_path(const char* path, const void* data, uint32_t len);
/* 1 if the file's data contains NUL bytes (i.e. isn't text) */
int  fs_is_binary(int idx);
/* file indexes (fs_get_file) directly inside directory `path`, up to max;
 * returns how many exist in total (may exceed max), or -1 if no such dir */
int  fs_list_files(const char* path, int* out_idx, int max);
/* absolute path of a file, for display */
void fs_file_path(int idx, char* buf, int buflen);

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
 * Serialize/restore the whole tree as an opaque blob - fsdisk.c writes/
 * reads that blob to/from disk with no knowledge of fs.c's layout.
 *
 * Version 2 (current): dirs[] table, home_dir, then each used file as
 * name + parent + size + data, so only real file bytes take space.
 * Version 1 (Banana OS 0.4): raw fixed-size dirs[32]/files[64] tables
 * with 2 KiB inline file contents - still loadable, for upgrades. */
#define FS_SNAPSHOT_V1 1u
#define FS_SNAPSHOT_V2 2u
uint32_t fs_snapshot_size(void);
void     fs_snapshot_save(uint8_t* buf);       /* buf must be >= fs_snapshot_size() bytes */
/* returns 0 on success; the current tree is only replaced on success */
int      fs_snapshot_load(const uint8_t* buf, uint32_t len, uint32_t version);

#endif
