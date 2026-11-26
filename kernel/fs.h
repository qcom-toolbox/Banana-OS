/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FS_H
#define FS_H

#include "types.h"

/* The tables grow as they fill (256 entries at a time), up to these. */
#define FS_MAX_FILES     1048576
#define FS_MAX_DIRS      262144
/* most entries one folder listing hands out (fs_list_files / fs_list_dirs callers) */
#define FS_LIST_MAX      65536
/* names up to 255 bytes, like ext4 (the + 1 is the NUL) */
#define FS_NAME_LEN      256
#define FS_PATH_LEN      512
/* Largest single file (downloads, images, videos). The data of a file in
 * use lives on the kernel heap, so the practical limit is free RAM; files
 * on the installed disk bigger than the RAM are read in pieces (fs_read). */
#define FS_MAX_FILE_SIZE 0x7FFFF000u

/* mode bits, as in Unix and ext4: the type, then rwxrwxrwx */
#define FS_S_IFMT   0170000u
#define FS_S_IFDIR  0040000u
#define FS_S_IFREG  0100000u
#define FS_S_IFLNK  0120000u
#define FS_MODE_FILE (FS_S_IFREG | 0644u)
#define FS_MODE_DIR  (FS_S_IFDIR | 0755u)
#define FS_MODE_LINK (FS_S_IFLNK | 0777u)
#define FS_UID_ROOT   0u
#define FS_UID_BANANA 1000u

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
    uint32_t atime;      /* last read (relatime: at most once a day, or after a change) */
    uint32_t mode;       /* FS_S_IF* | permissions; 0 = not set yet (FS_MODE_FILE) */
    uint32_t uid, gid;
    int32_t  lnext;      /* hard links: the next name of the same file (a ring), -1 none */
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
    uint8_t  unread;     /* its entries not read from the disk yet (kernel/fsdisk.c reads them when used) */
    uint32_t last_use;   /* timer_ms() it was last looked in */
    uint32_t mtime;      /* last time something in it was added, removed or renamed; ctime: made */
    uint32_t ctime;
    uint32_t atime;
    uint32_t mode;       /* FS_S_IFDIR | permissions; 0 = not set yet (FS_MODE_DIR) */
    uint32_t uid, gid;
} fs_dir_t;

void fs_init(void);

/* directory ops - all paths may be absolute ("/a/b"), relative ("a/b"),
 * use "." / ".." / "~" (home), same as a real Unix shell. */
int  fs_mkdir(const char* path);           /* returns dir index or -1 */
/* the logged-in user's home folder ("/home/banana" until someone else
 * logs in): what ~ means for the kernel's commands and windows */
void        fs_set_home(const char* path);
const char* fs_home_path(void);
/* the home + rel ("/Downloads"): a buffer of 8 reused in turn (several in one expression) */
const char* fs_home_join(const char* rel);
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
#define FS_MAX_MOUNTS 16

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
    /* ── optional (the installed disk's ext4 has them; FAT leaves them NULL) ──
     * remove / rename by the folder and the name, as Unix filesystems do */
    int (*remove_at)(void* ctx, uint32_t parent, const char* name, uint32_t node, int is_dir);
    int (*rename_at)(void* ctx, uint32_t parent, const char* name, uint32_t node, int is_dir,
                     uint32_t new_parent, const char* new_name);
    /* a part of a file: read (the bytes read, -1), written (0, -1; the file
     * grows as needed), the file cut or grown to size (0, -1) */
    int (*read_at)(void* ctx, uint32_t node, uint32_t off, void* buf, uint32_t len);
    int (*write_at)(void* ctx, uint32_t node, uint32_t off, const void* data, uint32_t len);
    int (*truncate)(void* ctx, uint32_t node, uint32_t size);
    /* mode (permission bits), owner, group, the access and modification
     * dates (local time); ~0u: as it is */
    int (*setattr)(void* ctx, uint32_t node, int is_dir, uint32_t perms, uint32_t uid, uint32_t gid,
                   uint32_t atime, uint32_t mtime);
    /* a symbolic link named name in parent, to target: *node */
    int (*symlink)(void* ctx, uint32_t parent, const char* name, const char* target, uint32_t* node);
    /* a hard link: name in parent, another name of file node */
    int (*link)(void* ctx, uint32_t parent, const char* name, uint32_t node);
} fs_mount_ops_t;

/* The installed disk as the tree's own backing (kernel/fsdisk.c, ext4):
 * every entry not on a mount is written through to it as it changes, and
 * files are read from it by the part - RAM holds only what is in use. The
 * root folder is the volume's root_node. (ops NULL: none - the live CD.) */
void fs_set_system_backend(const fs_mount_ops_t* ops, void* ctx, uint32_t root_node);
int  fs_has_system_backend(void);

/* writes len bytes at off into file idx (it grows as needed, a gap reads as
 * zeros), without reading the rest of it in: 0, or -1 */
int  fs_write_at(int idx, uint32_t off, const void* data, uint32_t len);
/* the file cut to (or grown to, with zeros) size bytes: 0, or -1 */
int  fs_truncate(int idx, uint32_t size);
/* loading the tree from the ext4 disk, or copying it there: the inode of
 * folder (is_dir) or file idx */
void fs_set_node(int is_dir, int idx, uint32_t node);

/* Folders read when used (the ext4 disk): load(dir, node) adds folder dir's
 * entries (fs_mount_add_dir / _file, its folders marked unread with
 * fs_set_unread) - called the first time a lookup, a listing or a walk looks
 * in it. fs_populate does it now (copying a whole tree). */
void fs_set_dir_loader(int (*load)(int dir, uint32_t node));
void fs_set_unread(int dir);
void fs_populate(int dir);
/* folders nobody looked in for idle_ms, with nothing in them in use (open,
 * pinned, read in), give their entries back - read again when used: the
 * entries freed */
uint32_t fs_unload_idle(uint32_t idle_ms);
/* entries in the tables now (folders and files) */
uint32_t fs_entries(void);

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

/* ── Unix metadata, symbolic links, checking (ext4-style) ───────────
 * Every file and folder has a mode (type + rwxrwxrwx), an owner and a group
 * and three dates (atime / mtime / ctime). Permissions are checked with the
 * owner's bits (everything runs as the one user): writing a file needs its
 * w bit, making / deleting / renaming in a folder needs the folder's.
 * Symbolic links are files of type FS_S_IFLNK holding the target path; path
 * lookups follow them (up to FS_MAX_LINKS deep), rm / mv act on the link. */
#define FS_MAX_LINKS 8

typedef struct {
    int      is_dir, is_link;
    uint32_t mode, uid, gid;
    uint32_t size;
    uint32_t atime, mtime, ctime;
    uint16_t mnt;
    int      index;          /* fs_get_file / fs_get_dir index */
    uint32_t nlink;          /* a file's names (hard links) */
    int      ino;            /* the same for every name of a file (its lowest index) */
} fs_stat_t;

/* follow: 1 like stat(), 0 like lstat(); 0 or -1 */
/* Users (kernel/users.h): a file operation's rights. A task's current
 * credential (NULL: the kernel's - as before users, only the owner's w bit
 * counts) is set for the length of an app's file system call or a user's
 * command; fs_cred_swap returns the one it replaces. */
#define FS_CRED_GROUPS 16
typedef struct {
    uint32_t uid, gid;
    int      ngroups;
    uint32_t groups[FS_CRED_GROUPS];
} fs_cred_t;
const fs_cred_t* fs_cred_swap(const fs_cred_t* c);
const fs_cred_t* fs_cred_current(void);
/* the current credential may read (4) / write (2) / search or run (1) a
 * file (is_dir 0) or folder, every folder above it searchable; 1 or 0
 * (fs_perm_error then) */
int  fs_access_entry(int is_dir, int idx, uint32_t want);
int  fs_stat(const char* path, int follow, fs_stat_t* st);
int  fs_chmod(const char* path, uint32_t perms);          /* the low 12 bits; 0 or -1 */
int  fs_chown(const char* path, int uid, int gid);        /* -1 keeps that one; 0 or -1 */
/* makes linkpath, pointing at target (stored as given): file index, or -1 */
int  fs_symlink(const char* target, const char* linkpath);
/* a link's target into out: its length, or -1 (not a link) */
int  fs_readlink(const char* path, char* out, int cap);
/* its last access's and last change's times, ~0u: as it is (follow:
 * through a link); 0 or -1 */
int  fs_utime(const char* path, int follow, uint32_t atime, uint32_t mtime);
/* an open file's (index): its times; chmod / chown (~0u: kept); 0 or -1 */
void fs_file_utime(int idx, uint32_t atime, uint32_t mtime);
int  fs_file_setattr(int idx, uint32_t perms, uint32_t uid, uint32_t gid);
int  fs_is_symlink(int idx);
/* Hard links: a file may have several names (in any folders of the same
 * volume - the RAM tree and the ext4 disk, not FAT / exFAT): every name is
 * an entry of its own, sharing the data, mode, owner and dates with the
 * others; the data goes with the last name. fs_link makes newpath another
 * name of the file oldpath names (a link there is not followed): its index,
 * or -ENOENT (-2), -EEXIST (-17), -EPERM (-1: a folder, FAT), -EXDEV (-18:
 * another volume), -EACCES (-13), -EIO (-5) */
int  fs_link(const char* oldpath, const char* newpath);
/* the names file idx has (1 without hard links) */
uint32_t fs_nlink(int idx);
/* loading a folder of the ext4 disk: file idx is another name of a file
 * whose entry may already be in the tables (same node) - joins it */
void fs_join_links(int idx);
/* the mode of a file / folder (the default when not set yet) */
uint32_t fs_file_mode(int idx);
uint32_t fs_dir_mode(int idx);
/* 1 if the last failed call failed for want of permission (EACCES) */
int  fs_perm_error(void);
/* restoring a saved tree: everything but the name and the dates */
void fs_set_meta(int is_dir, int idx, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t atime);
/* checks the tree's own consistency (links, hash chains, parents, sizes);
 * fix: repairs what it can. Writes a line per problem with out(); the
 * number of problems found. */
int  fs_check(int fix, void (*out)(const char* line));

#endif
