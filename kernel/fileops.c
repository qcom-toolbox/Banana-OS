/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Moving, copying, the shared clipboard and the Recycle Bin (fileops.h). */
#include "fileops.h"
#include "kstring.h"
#include "kheap.h"
#include "rtc.h"

const char* fileops_base(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void fileops_parent(const char* path, char* out, int cap) {
    kstrlcpy(out, path, (size_t)cap);
    char* s = strrchr(out, '/');
    if (!s) { kstrlcpy(out, "/", (size_t)cap); return; }
    if (s == out) s[1] = 0; else *s = 0;
}

int fileops_is_dir(const char* path) { return fs_find_dir(path) >= 0; }
int fileops_on_usb(const char* path) { return fs_path_mount(path) != 0; }

static int join(const char* dir, const char* name, char* out, int cap) {
    int n = !strcmp(dir, "/") ? ksnprintf(out, (size_t)cap, "/%s", name)
                              : ksnprintf(out, (size_t)cap, "%s/%s", dir, name);
    return n >= 0 && n < cap ? 0 : -1;
}

int fileops_unique(const char* dir, const char* base, char* out, int cap) {
    if (!out || cap <= 0) return -1;
    out[0] = 0;
    if (!base[0] || strlen(base) >= FS_NAME_LEN ||
        join(dir, base, out, cap) < 0) { out[0] = 0; return -1; }
    if (fs_find_file(out) < 0 && fs_find_dir(out) < 0) return 0;
    /* "photo (2).png": the number before the extension. Never hand back
     * an existing or truncated path when the candidate range is exhausted. */
    char stem[FS_NAME_LEN], ext[FS_NAME_LEN] = "";
    kstrlcpy(stem, base, sizeof(stem));
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) { kstrlcpy(ext, dot, sizeof(ext)); *dot = 0; }
    for (int n = 2; n < 1000; n++) {
        char nm[FS_NAME_LEN];
        int len = ksnprintf(nm, sizeof(nm), "%s (%d)%s", stem, n, ext);
        if (len < 0 || len >= (int)sizeof(nm) || join(dir, nm, out, cap) < 0) break;
        if (fs_find_file(out) < 0 && fs_find_dir(out) < 0) return 0;
    }
    out[0] = 0;
    return -1;
}

int fileops_copy_tree(const char* src, const char* dst, int depth) {
    if (depth > 12 || fs_mkdir(dst) < 0) return -1;
    /* Listing returns the total, even when its output array is smaller.
     * Never report a partial copy as complete: cross-volume moves delete
     * the source only after this function reports success. */
    int nf = fs_list_files(src, NULL, 0);
    int nd = fs_list_dirs(src, NULL, 0);
    if (nf < 0 || nd < 0 || nf > FS_LIST_MAX || nd > FS_LIST_MAX) return -1;
    int cap = nf > nd ? nf : nd;
    if (!cap) return 0;
    int* idx = kmalloc(sizeof(int) * (size_t)cap);
    if (!idx) return -1;
    int rc = 0;
    nf = fs_list_files(src, idx, cap);
    if (nf < 0 || nf > cap) { kfree(idx); return -1; }
    for (int i = 0; i < nf; i++) {
        char s[FS_PATH_LEN], d[FS_PATH_LEN];
        const fs_file_t* f = fs_file_info(idx[i]);
        if (!f) { rc = -1; continue; }
        if (join(src, f->name, s, sizeof(s)) < 0 ||
            join(dst, f->name, d, sizeof(d)) < 0) { rc = -1; continue; }
        if (fs_copy(s, d) < 0) rc = -1;
    }
    nd = fs_list_dirs(src, idx, cap);
    if (nd < 0 || nd > cap) { kfree(idx); return -1; }
    for (int i = 0; i < nd; i++) {
        char s[FS_PATH_LEN], d[FS_PATH_LEN];
        const fs_dir_t* child = fs_get_dir(idx[i]);
        if (!child) { rc = -1; continue; }
        if (join(src, child->name, s, sizeof(s)) < 0 ||
            join(dst, child->name, d, sizeof(d)) < 0) { rc = -1; continue; }
        if (fileops_copy_tree(s, d, depth + 1) < 0) rc = -1;
    }
    kfree(idx);
    return rc;
}

int fileops_delete(const char* path) {
    if (fs_find_dir(path) < 0 && fs_find_file(path) < 0) return -1;
    fs_delete(path, 1);
    return fs_find_dir(path) < 0 && fs_find_file(path) < 0 ? 0 : -1;
}

int fileops_transfer(const char* src, const char* dstdir, int copy, char* out, int cap, char* err, int ecap) {
    char e[8];
    if (!err) { err = e; ecap = sizeof(e); }
    int dir = fs_find_dir(src) >= 0;
    if (!dir && fs_find_file(src) < 0) { kstrlcpy(err, "it is gone", (size_t)ecap); return -1; }
    if (fs_find_dir(dstdir) < 0) { kstrlcpy(err, "no such folder", (size_t)ecap); return -1; }
    /* not into itself */
    size_t sl = strlen(src);
    if (dir && !strncmp(dstdir, src, sl) && (dstdir[sl] == 0 || dstdir[sl] == '/')) {
        kstrlcpy(err, "a folder cannot go into itself", (size_t)ecap);
        return -1;
    }
    char parent[FS_PATH_LEN];
    fileops_parent(src, parent, sizeof(parent));
    if (!copy && !strcmp(parent, dstdir)) { if (out) kstrlcpy(out, src, (size_t)cap); return 0; }   /* already there */
    char dst[FS_PATH_LEN];
    if (fileops_unique(dstdir, fileops_base(src), dst, sizeof(dst)) < 0) {
        kstrlcpy(err, "no available destination name (or path too long)", (size_t)ecap);
        return -1;
    }
    int ok;
    /* a move between the disk and a USB stick: copy, then delete */
    int across = fileops_on_usb(src) != fileops_on_usb(dstdir) ||
                 (fileops_on_usb(src) && fs_path_mount(src) != fs_path_mount(dstdir));
    if (copy || across) {
        ok = dir ? fileops_copy_tree(src, dst, 0) == 0 : fs_copy(src, dst) >= 0;
        if (ok && !copy) ok = fileops_delete(src) == 0;
    } else {
        ok = fs_move(src, dst) >= 0;
    }
    if (!ok) { kstrlcpy(err, fs_io_error() ? "the USB stick reported an error" : "it could not be written", (size_t)ecap); return -1; }
    if (out) kstrlcpy(out, dst, (size_t)cap);
    return 0;
}

/* ── the clipboard ── */
static char g_clip[CLIP_MAX][FS_PATH_LEN];
static int  g_nclip, g_clip_cut;

void fileops_clip_set(const char* const* paths, int n, int cut) {
    if (n > CLIP_MAX) n = CLIP_MAX;
    for (int i = 0; i < n; i++) kstrlcpy(g_clip[i], paths[i], FS_PATH_LEN);
    g_nclip = n;
    g_clip_cut = cut;
}
int fileops_clip_count(void) { return g_nclip; }
int fileops_clip_cut(void) { return g_clip_cut; }

int fileops_paste(const char* dir, char* status, int cap) {
    int done = 0, failed = 0;
    char why[64] = "";
    for (int i = 0; i < g_nclip; i++) {
        char err[64];
        if (fileops_transfer(g_clip[i], dir, !g_clip_cut, NULL, 0, err, sizeof(err)) == 0) done++;
        else { failed++; kstrlcpy(why, err, sizeof(why)); }
    }
    if (g_clip_cut && !failed) g_nclip = 0;            /* moved: nothing left to paste */
    if (status) {
        if (failed) ksnprintf(status, (size_t)cap, "%d of %d could not be %s: %s", failed, done + failed, g_clip_cut ? "moved" : "copied", why);
        else ksnprintf(status, (size_t)cap, "%s %d item%s", g_clip_cut ? "Moved" : "Pasted", done, done == 1 ? "" : "s");
    }
    return done;
}

/* ── the Recycle Bin ── */
int in_trash(const char* path) {
    size_t n = strlen(TRASH_FILES);
    return !strncmp(path, TRASH_FILES, n) && path[n] == '/';
}

static void info_path(const char* trashed, char* out, int cap) {
    char nm[FS_PATH_LEN];
    ksnprintf(nm, sizeof(nm), "%s.trashinfo", fileops_base(trashed));
    join(TRASH_INFO, nm, out, cap);
}

int trash_put(const char* path) {
    if (in_trash(path) || !strcmp(path, TRASH_FILES)) return -1;
    if (fs_find_dir(path) < 0 && fs_find_file(path) < 0) return -1;
    fs_mkdir_p(TRASH_FILES);
    fs_mkdir_p(TRASH_INFO);
    char dst[FS_PATH_LEN], err[64];
    if (fileops_transfer(path, TRASH_FILES, 0, dst, sizeof(dst), err, sizeof(err)) != 0) return -1;
    char ip[FS_PATH_LEN], info[FS_PATH_LEN + 64], when[24];
    info_path(dst, ip, sizeof(ip));
    rtc_format(rtc_now(), when, sizeof(when));
    ksnprintf(info, sizeof(info), "[Trash Info]\nPath=%s\nDeletionDate=%s\n", path, when);
    fs_write_path(ip, info, (uint32_t)strlen(info));
    return 0;
}

void trash_origin(const char* trashed, char* out, int cap) {
    out[0] = 0;
    char ip[FS_PATH_LEN];
    info_path(trashed, ip, sizeof(ip));
    int fi = fs_find_file(ip);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return;
    const char* p = strstr(f->content, "Path=");
    if (!p) return;
    p += 5;
    int n = 0;
    while (p[n] && p[n] != '\n' && n < cap - 1) { out[n] = p[n]; n++; }
    out[n] = 0;
}

int trash_restore(const char* trashed) {
    if (!in_trash(trashed)) return -1;
    char orig[FS_PATH_LEN], dir[FS_PATH_LEN], ip[FS_PATH_LEN];
    trash_origin(trashed, orig, sizeof(orig));
    if (!orig[0]) kstrlcpy(orig, "/home/banana/restored", sizeof(orig));
    fileops_parent(orig, dir, sizeof(dir));
    fs_mkdir_p(dir);
    /* back under its own name (if that is taken: a free one) */
    char dst[FS_PATH_LEN];
    if (fileops_unique(dir, fileops_base(orig), dst, sizeof(dst)) < 0) return -1;
    int ok = fs_move(trashed, dst) >= 0;
    if (!ok) {
        char err[64];
        ok = fileops_transfer(trashed, dir, 0, NULL, 0, err, sizeof(err)) == 0;
    }
    info_path(trashed, ip, sizeof(ip));
    if (ok) fs_delete(ip, 0);
    return ok ? 0 : -1;
}

int trash_count(void) {
    int idx[1], d[1];
    int nf = fs_list_files(TRASH_FILES, idx, 1), nd = fs_list_dirs(TRASH_FILES, d, 1);
    return (nf > 0 ? nf : 0) + (nd > 0 ? nd : 0);
}

int trash_empty(void) {
    int n = trash_count();
    if (fs_find_dir(TRASH_DIR) >= 0) fs_delete(TRASH_DIR, 1);
    fs_mkdir_p(TRASH_FILES);
    fs_mkdir_p(TRASH_INFO);
    return n;
}
