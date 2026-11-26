/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ext4move.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"

/* (see ext4move.h) */

#define BS_SECT 8u                               /* sectors per 4 KiB block */

typedef struct {
    uint32_t old_ino, new_ino;
    uint32_t mode, uid, gid, atime, mtime, ctime;
    uint64_t size;
    uint32_t run0, nrun;                         /* its data: runs[run0 ...] (new block numbers) */
    char*    link;                               /* a symbolic link's target */
} mnode_t;

typedef struct { uint32_t parent, node; char* name; } ment_t;

typedef struct {
    mnode_t*    node;   uint32_t nnode, cnode;
    ment_t*     ent;    uint32_t nent, cent;
    ext4_run_t* run;    uint32_t nrun, crun;
    uint32_t (*keep)[2]; uint32_t nkeep, ckeep;
    uint32_t*   hash;   uint32_t hcap;           /* old inode -> node + 1 */
    uint32_t    delta, lo, hi;                   /* (old block numbers) */
    int         bad;                             /* a used block where it may not be */
} mctx_t;

static int grow(void** p, uint32_t* cap, uint32_t need, uint32_t size) {
    if (need <= *cap) return 0;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    void* n = krealloc(*p, (size_t)c * size);
    if (!n) return -1;
    *p = n;
    *cap = c;
    return 0;
}

static uint32_t hslot(const mctx_t* c, uint32_t ino) { return (ino * 2654435761u) & (c->hcap - 1u); }

static uint32_t node_of(const mctx_t* c, uint32_t ino) {
    if (!c->hcap) return 0;
    for (uint32_t i = hslot(c, ino); c->hash[i]; i = (i + 1) & (c->hcap - 1u))
        if (c->node[c->hash[i] - 1].old_ino == ino) return c->hash[i];
    return 0;
}

static int hash_put(mctx_t* c, uint32_t ino, uint32_t idx) {
    if ((c->nnode + 1) * 2 > c->hcap) {                    /* (half full at most) */
        uint32_t cap = c->hcap ? c->hcap * 2 : 1024;
        uint32_t* h = (uint32_t*)kzalloc(cap * sizeof(uint32_t));
        if (!h) return -1;
        uint32_t* old = c->hash;
        uint32_t ocap = c->hcap;
        c->hash = h;
        c->hcap = cap;
        for (uint32_t i = 0; i < ocap; i++) {
            if (!old[i]) continue;
            uint32_t s = hslot(c, c->node[old[i] - 1].old_ino);
            while (h[s]) s = (s + 1) & (cap - 1u);
            h[s] = old[i];
        }
        kfree(old);
    }
    uint32_t s = hslot(c, ino);
    while (c->hash[s]) s = (s + 1) & (c->hcap - 1u);
    c->hash[s] = idx + 1;
    return 0;
}

static void ctx_free(mctx_t* c) {
    for (uint32_t i = 0; i < c->nnode; i++) kfree(c->node[i].link);
    for (uint32_t i = 0; i < c->nent; i++) kfree(c->ent[i].name);
    kfree(c->node);
    kfree(c->ent);
    kfree(c->run);
    kfree(c->keep);
    kfree(c->hash);
    memset(c, 0, sizeof(*c));
}

/* ── the room ── */

typedef struct { uint64_t used, used_in; uint32_t lo, hi; } count_t;

static int count_run(void* arg, uint32_t blk, uint32_t n) {
    count_t* k = (count_t*)arg;
    k->used += n;
    uint32_t a = blk > k->lo ? blk : k->lo, b = blk + n < k->hi ? blk + n : k->hi;
    if (b > a) k->used_in += b - a;
    return 0;
}

/* the new filesystem's own structures: bitmaps, inode tables, the journal (ext4_mkfs) */
static uint32_t new_meta_blocks(uint32_t nb) {
    uint32_t groups = (nb + 32767u) / 32768u;
    return groups * (2u + 256u) + 8192u + 64u;
}

static int room_ok(const ext4_dev_t* new_dev, uint32_t delta, uint64_t used, uint32_t* lo, uint32_t* hi) {
    uint32_t nb = ext4_mkfs_blocks(new_dev->sectors), fixed = ext4_fixed_blocks(new_dev->sectors);
    if (nb < 4096u || nb <= fixed) return 0;
    *lo = delta + fixed;
    *hi = delta + nb;
    /* every block the old one uses (its own structures too: a little more than
     * it will need), the new one's, and 2% (64 MiB at most, 8 MiB at least) */
    uint64_t spare = nb / 50u;
    if (spare > 16384u) spare = 16384u;
    if (spare < 2048u) spare = 2048u;
    return used + new_meta_blocks(nb) + spare <= (uint64_t)(nb - fixed);
}

int ext4move_check(const ext4_dev_t* old_dev, const ext4_dev_t* new_dev, uint32_t delta) {
    if (ext4_mount(old_dev) != 0) return EXT4MOVE_MOUNT;
    uint64_t blocks, free;
    uint32_t inodes, ifree;
    ext4_statfs(&blocks, &free, &inodes, &ifree);
    /* (the swap file goes: its blocks do not count) */
    uint32_t sw = ext4_lookup(EXT4_ROOT_INO, "swapfile", 0);
    ext4_stat_t st;
    uint64_t swap = sw && ext4_stat(sw, &st) == 0 && st.links == 1 ? st.blocks / BS_SECT : 0;
    uint32_t lo, hi;
    int ok = room_ok(new_dev, delta, blocks - free - swap, &lo, &hi);
    ext4_unmount();
    return ok ? EXT4MOVE_OK : EXT4MOVE_FULL;
}

/* ── what the old filesystem has ── */

static int keep_run(void* arg, uint32_t blk, uint32_t n) {
    mctx_t* c = (mctx_t*)arg;
    uint32_t end = blk + n;
    /* past the old start's boot area: in the new filesystem's superblock or
     * descriptors, or past its end, nothing may be left */
    if ((blk < c->lo && end > c->delta) || end > c->hi) c->bad = 1;
    uint32_t a = blk > c->lo ? blk : c->lo, b = end < c->hi ? end : c->hi;
    if (b <= a) return 0;
    if (grow((void**)&c->keep, &c->ckeep, c->nkeep + 1, sizeof(*c->keep))) return -1;
    c->keep[c->nkeep][0] = a - c->delta;
    c->keep[c->nkeep][1] = b - a;
    c->nkeep++;
    return 0;
}

static int add_node(mctx_t* c, uint32_t ino, const ext4_stat_t* st) {
    if (grow((void**)&c->node, &c->cnode, c->nnode + 1, sizeof(mnode_t))) return -1;
    mnode_t* n = &c->node[c->nnode];
    memset(n, 0, sizeof(*n));
    n->old_ino = ino;
    n->mode = st->mode;
    n->uid = st->uid;
    n->gid = st->gid;
    n->atime = st->atime;
    n->mtime = st->mtime;
    n->ctime = st->ctime;
    n->size = st->size;
    if (hash_put(c, ino, c->nnode)) return -1;
    c->nnode++;
    return 0;
}

typedef struct { char* name; uint32_t ino; } kid_t;
typedef struct { kid_t* k; uint32_t n, cap; int fail; } kids_t;

static int collect_kid(void* arg, const char* name, uint32_t ino, uint32_t type) {
    (void)type;
    kids_t* k = (kids_t*)arg;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (grow((void**)&k->k, &k->cap, k->n + 1, sizeof(kid_t))) { k->fail = 1; return 1; }
    size_t len = strlen(name);
    char* s = (char*)kmalloc(len + 1);
    if (!s) { k->fail = 1; return 1; }
    memcpy(s, name, len + 1);
    k->k[k->n++] = (kid_t){ s, ino };
    return 0;
}

/* the tree from the root, folder by folder (parents before their children) */
static int walk(mctx_t* c, ext4move_info_t* info) {
    ext4_stat_t st;
    if (ext4_stat(EXT4_ROOT_INO, &st) != 0 || add_node(c, EXT4_ROOT_INO, &st)) return EXT4MOVE_IO;
    c->node[0].new_ino = EXT4_ROOT_INO;
    for (uint32_t q = 0; q < c->nnode; q++) {
        if ((c->node[q].mode & EXT4_S_IFMT) != EXT4_S_IFDIR) continue;
        kids_t kids;
        memset(&kids, 0, sizeof(kids));
        int rr = ext4_readdir(c->node[q].old_ino, collect_kid, &kids);
        int rc = rr < 0 || kids.fail ? EXT4MOVE_IO : EXT4MOVE_OK;
        for (uint32_t i = 0; i < kids.n && rc == EXT4MOVE_OK; i++) {
            uint32_t ino = kids.k[i].ino, idx = node_of(c, ino);
            if (!idx) {
                if (ext4_stat(ino, &st) != 0) { rc = EXT4MOVE_IO; break; }
                uint32_t t = st.mode & EXT4_S_IFMT;
                if (t != EXT4_S_IFDIR && t != EXT4_S_IFREG && t != EXT4_S_IFLNK) {
                    klog("ext4move: %s: not a file, folder or link (mode %o) - left out\n", kids.k[i].name, st.mode);
                    info->skipped++;
                    continue;
                }
                if (add_node(c, ino, &st)) { rc = EXT4MOVE_IO; break; }
                idx = c->nnode;
                mnode_t* n = &c->node[idx - 1];
                if (ino == EXT4_LPF_INO && q == 0 && t == EXT4_S_IFDIR) n->new_ino = EXT4_LPF_INO;   /* (mkfs makes it) */
                if (t == EXT4_S_IFLNK) {
                    uint32_t len = st.size < 4095u ? (uint32_t)st.size : 4095u;
                    n->link = (char*)kzalloc(len + 1);
                    if (!n->link || ext4_read(ino, 0, n->link, len) != (long)len) { rc = EXT4MOVE_IO; break; }
                    info->symlinks++;
                } else if (t == EXT4_S_IFREG) {
                    ext4_run_t* r;
                    uint32_t nr;
                    if (ext4_runs(ino, &r, &nr) != 0) { rc = EXT4MOVE_IO; break; }
                    if (grow((void**)&c->run, &c->crun, c->nrun + nr, sizeof(ext4_run_t))) { kfree(r); rc = EXT4MOVE_IO; break; }
                    n->run0 = c->nrun;
                    n->nrun = nr;
                    for (uint32_t k = 0; k < nr; k++) {
                        if (r[k].pblk < c->lo || r[k].pblk + r[k].len > c->hi) c->bad = 1;   /* (moved in: never) */
                        r[k].pblk -= c->delta;
                        c->run[c->nrun++] = r[k];
                    }
                    kfree(r);
                    info->files++;
                } else {
                    info->dirs++;
                }
            } else if ((c->node[idx - 1].mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
                continue;                               /* (a folder linked twice: damaged - once) */
            } else {
                info->links++;
            }
            if (grow((void**)&c->ent, &c->cent, c->nent + 1, sizeof(ment_t))) { rc = EXT4MOVE_IO; break; }
            c->ent[c->nent++] = (ment_t){ q, idx - 1, kids.k[i].name };
            kids.k[i].name = 0;
        }
        for (uint32_t i = 0; i < kids.n; i++) kfree(kids.k[i].name);
        kfree(kids.k);
        if (rc != EXT4MOVE_OK) return rc;
    }
    return EXT4MOVE_OK;
}

/* ── the new filesystem ── */

static int make_tree(mctx_t* c) {
    for (uint32_t i = 0; i < c->nent; i++) {
        ment_t* e = &c->ent[i];
        mnode_t* n = &c->node[e->node];
        uint32_t p = c->node[e->parent].new_ino;
        if (!p) return EXT4MOVE_IO;
        if (n->new_ino) {
            if (n->old_ino == EXT4_LPF_INO && n->new_ino == EXT4_LPF_INO && (n->mode & EXT4_S_IFMT) == EXT4_S_IFDIR) continue;
            if (ext4_link(p, e->name, n->new_ino) != 0) return EXT4MOVE_FULL;
            continue;
        }
        uint32_t t = n->mode & EXT4_S_IFMT, perm = n->mode & 07777u, ino;
        if (t == EXT4_S_IFDIR) ino = ext4_mkdir(p, e->name, perm, n->uid, n->gid);
        else if (t == EXT4_S_IFLNK) ino = ext4_symlink(p, e->name, n->link, n->uid, n->gid);
        else {
            ino = ext4_create(p, e->name, perm, n->uid, n->gid);
            if (ino && ext4_adopt_runs(ino, n->size, c->run + n->run0, n->nrun) != 0) return EXT4MOVE_IO;
        }
        if (!ino) return EXT4MOVE_FULL;
        n->new_ino = ino;
    }
    /* the owners, permissions and dates last (making the children changed the folders') */
    for (uint32_t i = 0; i < c->nnode; i++) {
        mnode_t* n = &c->node[i];
        if (n->new_ino && ext4_setattr(n->new_ino, n->mode & 07777u, n->uid, n->gid, n->atime, n->mtime, n->ctime) != 0)
            return EXT4MOVE_IO;
    }
    return EXT4MOVE_OK;
}

static int cmp_run(const ext4_run_t* a, const ext4_run_t* b) { return a->pblk < b->pblk ? -1 : a->pblk > b->pblk; }

/* what was kept but is no file's data (the old filesystem's folders, trees,
 * journal...): free in the new one */
static uint32_t free_unowned(mctx_t* c) {
    uint32_t n = c->nrun;
    ext4_run_t* r = c->run;
    for (uint32_t gap = n / 2; gap; gap /= 2)              /* (by physical block) */
        for (uint32_t i = gap; i < n; i++) {
            ext4_run_t t = r[i];
            uint32_t j = i;
            while (j >= gap && cmp_run(&r[j - gap], &t) > 0) { r[j] = r[j - gap]; j -= gap; }
            r[j] = t;
        }
    uint32_t freed = 0, k = 0;
    for (uint32_t i = 0; i < c->nkeep; i++) {
        uint32_t a = c->keep[i][0], end = a + c->keep[i][1];
        while (k < n && r[k].pblk + r[k].len <= a) k++;
        for (uint32_t j = k; j < n && r[j].pblk < end && a < end; j++) {
            if (r[j].pblk > a) { ext4_free_run(a, r[j].pblk - a); freed += r[j].pblk - a; }
            if (r[j].pblk + r[j].len > a) a = r[j].pblk + r[j].len;
        }
        if (a < end) { ext4_free_run(a, end - a); freed += end - a; }
    }
    return freed;
}

int ext4move_run(const ext4_dev_t* old_dev, const ext4_dev_t* new_dev, uint32_t delta, ext4move_info_t* info) {
    ext4move_info_t dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof(*info));
    mctx_t c;
    memset(&c, 0, sizeof(c));
    c.delta = delta;
    if (ext4_mount(old_dev) != 0) return EXT4MOVE_MOUNT;
    int rc = EXT4MOVE_OK;
    /* 1. the swap file goes; the room is checked */
    uint32_t sw = ext4_lookup(EXT4_ROOT_INO, "swapfile", 0);
    if (sw && ext4_unlink(EXT4_ROOT_INO, "swapfile") != 0) rc = EXT4MOVE_IO;
    uint64_t blocks, free;
    uint32_t inodes, ifree;
    ext4_statfs(&blocks, &free, &inodes, &ifree);
    if (rc == EXT4MOVE_OK && !room_ok(new_dev, delta, blocks - free, &c.lo, &c.hi)) rc = EXT4MOVE_FULL;
    if (c.hi > blocks) c.hi = (uint32_t)blocks;            /* (the new one may end where the old did not use) */
    /* 2. every file's data into what becomes the new filesystem */
    if (rc == EXT4MOVE_OK) {
        int mv = ext4_move_into(c.lo, c.hi, &info->moved_blocks);
        if (mv != 0) rc = mv == -2 ? EXT4MOVE_FULL : EXT4MOVE_IO;
    }
    /* 3. what the old one uses there, kept; and its tree, read */
    if (rc == EXT4MOVE_OK && ext4_used_runs(keep_run, &c) != 0) rc = EXT4MOVE_IO;
    if (rc == EXT4MOVE_OK && c.bad) rc = EXT4MOVE_LAYOUT;
    if (rc == EXT4MOVE_OK) rc = walk(&c, info);
    if (rc == EXT4MOVE_OK && c.bad) rc = EXT4MOVE_LAYOUT;
    if (ext4_unmount() != 0 && rc == EXT4MOVE_OK) rc = EXT4MOVE_IO;
    if (rc != EXT4MOVE_OK) { ctx_free(&c); return rc; }
    for (uint32_t i = 0; i < c.nkeep; i++) info->kept_blocks += c.keep[i][1];
    klog("ext4move: %u files, %u folders, %u links; %u blocks moved in, %u kept\n",
         info->files, info->dirs, info->links + info->symlinks, info->moved_blocks, info->kept_blocks);

    /* 4. the new filesystem around them, without its magic until it is whole */
    ext4_set_hidden(1);
    if (ext4_mkfs(new_dev, "BANANA_OS", (const uint32_t (*)[2])c.keep, (int)c.nkeep) != 0) {
        ext4_set_hidden(0);
        ctx_free(&c);
        return EXT4MOVE_FULL;
    }
    rc = make_tree(&c);
    if (rc == EXT4MOVE_OK) {
        info->freed_blocks = free_unowned(&c);
        if (ext4_flush() != 0 || ext4_activate() != 0) rc = EXT4MOVE_IO;
    }
    if (rc != EXT4MOVE_OK) {
        /* (still without its magic: nothing points at it) */
        ext4_unmount();
        ext4_set_hidden(0);
        ctx_free(&c);
        return rc;
    }
    if (ext4_unmount() != 0) rc = EXT4MOVE_IO;
    ctx_free(&c);
    return rc;
}
