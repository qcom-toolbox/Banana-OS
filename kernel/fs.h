#ifndef FS_H
#define FS_H

#include "types.h"

/* The tables grow as they fill (256 entries at a time), up to these. */
#define FS_MAX_FILES     65536
#define FS_MAX_DIRS      16384
#define FS_NAME_LEN      32
#define FS_PATH_LEN      128
/* Largest single file (downloads, images, videos). The data of a file in
 * use lives on the kernel heap, so the practical limit is free RAM. */
#define FS_MAX_FILE_SIZE (128u * 1024u * 1024u)

typedef struct {
    char     name[FS_NAME_LEN];
    char*    content;    /* heap buffer, always NUL-terminated at [size] */
    uint32_t size;       /* bytes of data (files may be binary) */
    uint32_t cap;        /* allocated bytes (>= size + 1) */
    int      used;
    int      parent_dir; /* index into dirs[] */
    uint16_t mnt;        /* 0, or the mount (USB stick, ...) the file lives on */
    uint8_t  loaded;     /* content read in yet (files on a stick or the installed disk: on first use) */
    uint8_t  loading;    /* being read in right now */
    uint32_t node;       /* the mount driver's handle for it */
    uint32_t data_gen;   /* fs_generation() of the last change to its data (kernel/fsdisk.c) */
    int32_t  hnext;      /* next in its name-hash bucket (-2: not linked) */
    int32_t  sib;        /* next file in the same folder */
    uint32_t last_use;   /* timer_ms() it was last used (eviction takes the oldest first) */
    uint16_t pins;       /* held by someone for a while (fs_pin): never evicted */
    uint32_t mtime;      /* last change of its data, ctime: made (rtc_now(); 0 = not known) */
    uint32_t ctime;
} fs_file_t;

typedef struct {
    char     name[FS_NAME_LEN];
    int      used;
    int      parent_dir; /* -1 = root */
    uint16_t mnt;        /* 0, or the mount it belongs to (its root dir included) */
    uint32_t node;
    int32_t  hnext;      /* next in its name-hash bucket (-2: not linked) */
    int32_t  sib;        /* next folder in the same folder */
    int32_t  kid_dir, last_dir;     /* its subfolders, in creation order */
    int32_t  kid_file, last_file;   /* its files */
    uint32_t mtime;      /* last time something in it was added, removed or renamed; ctime: made */
    uint32_t ctime;
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
/* the file, its content read in (a file on a mounted USB stick is read
 * on first use) - NULL if idx is out of range or it cannot be read */
fs_file_t* fs_get_file(int idx);
/* the same without reading anything: name, size, parent (listings) */
fs_file_t* fs_file_info(int idx);
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
/* the same for subdirectories (fs_get_dir) */
int  fs_list_dirs(const char* path, int* out_idx, int max);
const fs_dir_t* fs_get_dir(int idx);
/* absolute path of a file, for display */
void fs_file_path(int idx, char* buf, int buflen);
/* absolute path of a directory */
void fs_dir_path(int idx, char* buf, int buflen);

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
/* file data held in RAM right now, and the tables */
uint32_t fs_ram_used_bytes(void);
/* table slots made so far: valid indexes are below these */
int fs_file_slots(void);
int fs_dir_slots(void);

/* ── on-disk persistence (see kernel/fsdisk.c) ──────────────────────
 * Serialize/restore the whole tree as an opaque blob - fsdisk.c writes/
 * reads that blob to/from disk with no knowledge of fs.c's layout.
 *
 * Version 3 (current): the dirs table as a count + name/used/parent
 * records, home_dir, then each used file as name + parent + size + data,
 * so only real file bytes take space. Mounted volumes are never saved.
 * Version 2 (Banana OS 0.5): the same with a raw 64-entry dirs table.
 * Version 1 (Banana OS 0.4): raw fixed-size dirs[32]/files[64] tables
 * with 2 KiB inline file contents. Both still load, for upgrades. */
/* changes on every write, create, delete, rename... */
uint32_t fs_generation(void);

#define FS_SNAPSHOT_V1 1u
#define FS_SNAPSHOT_V2 2u
#define FS_SNAPSHOT_V3 3u
uint32_t fs_snapshot_size(void);
void     fs_snapshot_save(uint8_t* buf);       /* buf must be >= fs_snapshot_size() bytes */
/* returns 0 on success; the current tree is only replaced on success */
int      fs_snapshot_load(const uint8_t* buf, uint32_t len, uint32_t version);

/* Layout-5 disks (kernel/fsdisk.c) keep every file in a place of its own
 * and write only the files that changed: the tree is rebuilt from their
 * records with these, each entry at the index it was saved from. */
int  fs_home_dir(void);
void fs_restore_begin(void);
void fs_restore_dir(int idx, const char* name, int parent);
int  fs_restore_file(int idx, const char* name, int parent, const void* data, uint32_t size);
/* the same with the data left on the disk: read in on first use */
int  fs_restore_file_lazy(int idx, const char* name, int parent, uint32_t size);
void fs_restore_end(int home);
/* the installed disk (kernel/fsdisk.c): read reads the saved data of file
 * idx (size bytes, checked), read_range a part of it, clean says whether
 * the saved copy is the file as it is now (data_gen) - 0 / -1, 1 / 0 */
void fs_set_disk_ops(int (*read)(int idx, uint8_t* buf, uint32_t size),
                     int (*read_range)(int idx, uint32_t off, uint8_t* buf, uint32_t len),
                     int (*clean)(int idx, uint32_t data_gen));

/* ── files bigger than the RAM, and a cache that gives memory back ─────
 * Like other systems, Banana OS keeps file data in RAM as a cache: a file
 * is read in when used, and when memory runs low the files used longest
 * ago are dropped from RAM again - only files whose copy on the disk or
 * the stick is up to date (a changed file is saved first), never one
 * pinned. A dropped file is read in again the next time it is used. */

/* len bytes at off into buf: the bytes read (0 at the end), or -1. A big
 * file on the installed disk is read in pieces, straight from the disk -
 * it never has to fit in RAM (apps, the music player). */
int  fs_read(int idx, uint32_t off, void* buf, uint32_t len);
/* while pinned, a file's data (fs_get_file()->content) is not dropped:
 * for those who hold it while other tasks run (decoding a picture, a
 * web server sending it) */
void fs_pin(int idx);
void fs_unpin(int idx);
/* drops files unused for min_idle_ms, oldest first, until want bytes are
 * free in one block (kheap_largest_free); returns the bytes dropped */
uint32_t fs_evict(uint32_t want, uint32_t min_idle_ms);
/* called now and then (the autosave task): keeps some RAM free */
void fs_trim(void);
/* file data in RAM that could be dropped right now (for "available") */
uint32_t fs_evictable_bytes(void);

/* ── mounts (kernel/fat32.c) ─────────────────────────────────────────
 * Another filesystem's tree is mirrored under a directory: its folders
 * and files are regular entries (so ls, cp, Files, the browser... all
 * work on it), every change is written through to the volume, and file
 * contents are only read when first used. */
#define FS_MAX_MOUNTS 4

typedef struct {
    /* fill buf with the file's size bytes */
    int (*read)(void* ctx, uint32_t node, uint8_t* buf, uint32_t size);
    /* replace the file's data */
    int (*write)(void* ctx, uint32_t node, const void* data, uint32_t len);
    /* add data at the end of a file of old_size bytes */
    int (*append)(void* ctx, uint32_t node, uint32_t old_size, const void* data, uint32_t len);
    /* new empty file / folder inside folder parent; *node gets its handle */
    int (*create)(void* ctx, uint32_t parent, const char* name, int is_dir, uint32_t* node);
    /* delete a file or an (already emptied) folder */
    int (*remove)(void* ctx, uint32_t node, int is_dir);
    /* move / rename within the volume */
    int (*rename)(void* ctx, uint32_t node, int is_dir, uint32_t new_parent, const char* new_name);
    /* the mount goes away: forget the volume */
    void (*unmount)(void* ctx);
} fs_mount_ops_t;

/* mounts at path (made with mkdir -p; it must be empty): the mount id
 * (> 0), or -1. The volume's root folder has the handle root_node. */
int  fs_mount(const char* path, const fs_mount_ops_t* ops, void* ctx, uint32_t root_node);
/* the mount's root directory index */
int  fs_mount_root(int mnt);
/* while scanning a volume: add an entry under directory index parent */
int  fs_mount_add_dir(int mnt, int parent, const char* name, uint32_t node);
int  fs_mount_add_file(int mnt, int parent, const char* name, uint32_t node, uint32_t size);
/* drops every entry of the mount (writes nothing); 0 if no such mount */
int  fs_unmount(int mnt);
/* the mount holding path (0 = none) and its mount point ("" if none) */
int  fs_path_mount(const char* path);
const char* fs_mount_point(int mnt);
/* 1 if the last failed fs_* call failed because the volume reported an
 * error (full, unplugged...) rather than a bad name or path */
int  fs_io_error(void);
/* the dates of file (is_dir 0) or folder idx: loading a saved tree, a volume */
void fs_set_times(int is_dir, int idx, uint32_t mtime, uint32_t ctime);
/* touch: makes the file if needed, else its modification date becomes now */
int  fs_touch(const char* path);
/* 1 once the tree exists (fs_init() ran, or a disk was loaded) */
int  fs_is_ready(void);

#endif
