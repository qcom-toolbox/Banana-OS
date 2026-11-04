/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ext4.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "rtc.h"
#include "serial.h"

/* ext4 in its own on-disk format (see ext4.h). Byte offsets below are the
 * ones of Linux's fs/ext4/ext4.h; everything on the disk is little-endian. */

#define BS          4096u                  /* block size */
#define SPB         8u                     /* sectors per block */
#define BPG         32768u                 /* blocks per group (one bitmap block) */
#define ISZ         256u                   /* inode size */
#define DESC        32u                    /* group descriptor size (no 64bit) */
#define EXT_MAX_LEN 32768u                 /* an initialized extent's length at most */

/* features */
#define COMPAT_SPARSE_SUPER2   0x0200u
#define INCOMPAT_FILETYPE      0x0002u
#define INCOMPAT_EXTENTS       0x0040u
#define INCOMPAT_FLEX_BG       0x0200u
#define RO_SPARSE_SUPER        0x0001u
#define RO_LARGE_FILE          0x0002u
#define RO_GDT_CSUM            0x0010u
#define RO_DIR_NLINK           0x0020u
#define RO_EXTRA_ISIZE         0x0040u
#define OUR_INCOMPAT  (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG)
#define OUR_RO        (RO_SPARSE_SUPER | RO_LARGE_FILE | RO_GDT_CSUM | RO_DIR_NLINK | RO_EXTRA_ISIZE)

#define BG_INODE_UNINIT 0x0001u
#define EXT4_EXTENTS_FL 0x00080000u
#define EH_MAGIC        0xF30Au

static uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void     p16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void     p32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ── the filesystem's state ───────────────────────────────────────── */

static ext4_dev_t g_dev;
static int        g_mounted;
static uint8_t    g_sb[1024];              /* the superblock as on the disk */
static uint8_t*   g_gdt;                   /* the group descriptors, DESC bytes each */
static uint32_t   g_groups, g_gdt_blocks, g_ipg, g_itb, g_nblocks;
static uint8_t*   g_gdt_dirty;             /* per descriptor block */
static int        g_sb_dirty;
static char       g_label[17];
static int        g_hidden;                /* made, but its magic not on the disk yet (ext4_activate) */

static volatile int g_lock;                /* one operation at a time (they may wait for the disk) */
static void lock(void)   { while (g_lock) task_sleep_ms(1); g_lock = 1; }
static void unlock(void) { g_lock = 0; }

static uint32_t now(void) { uint32_t t = rtc_now_utc(); return t ? t : 1700000000u; }

/* ── crc16 (the uninit_bg group descriptor checksum) ─────────────── */

static uint16_t crc16(uint16_t crc, const uint8_t* p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint8_t* desc(uint32_t g) { return g_gdt + g * DESC; }

static void desc_csum(uint32_t g) {
    uint8_t* d = desc(g);
    uint8_t le[4];
    p32(le, g);
    uint16_t c = crc16(0xFFFFu, g_sb + 0x68, 16);
    c = crc16(c, le, 4);
    c = crc16(c, d, 0x1E);
    p16(d + 0x1E, c);
}

static void desc_changed(uint32_t g) {
    desc_csum(g);
    g_gdt_dirty[(g * DESC) / BS] = 1;
    g_sb_dirty = 1;
}

/* ── the block cache ──────────────────────────────────────────────── */

typedef struct cblk {
    uint32_t     blk;
    uint8_t*     data;
    uint8_t      dirty;
    uint8_t      meta;                     /* the filesystem's own (bitmaps, inodes, folders, trees): journaled */
    uint16_t     pins;
    uint32_t     used;                     /* timer_ms() */
    struct cblk* hnext;
    struct cblk* prev, *next;              /* the list, most recently used first */
} cblk_t;

#define HBUCKETS 4096u
static cblk_t*  g_hash[HBUCKETS];
static cblk_t*  g_head, *g_tail;
static uint32_t g_ncache, g_ndirty;
#define CACHE_MIN 64u                       /* blocks always allowed */

static uint32_t hb(uint32_t blk) { return (blk * 2654435761u) >> 20 & (HBUCKETS - 1); }

static void lru_unlink(cblk_t* c) {
    if (c->prev) c->prev->next = c->next; else g_head = c->next;
    if (c->next) c->next->prev = c->prev; else g_tail = c->prev;
    c->prev = c->next = 0;
}
static void lru_front(cblk_t* c) {
    c->next = g_head;
    c->prev = 0;
    if (g_head) g_head->prev = c; else g_tail = c;
    g_head = c;
}

static cblk_t* cfind(uint32_t blk) {
    for (cblk_t* c = g_hash[hb(blk)]; c; c = c->hnext) if (c->blk == blk) return c;
    return 0;
}

static void cdrop(cblk_t* c) {
    cblk_t** pp = &g_hash[hb(c->blk)];
    while (*pp != c) pp = &(*pp)->hnext;
    *pp = c->hnext;
    lru_unlink(c);
    if (c->dirty) g_ndirty--;
    kfree(c->data);
    kfree(c);
    g_ncache--;
}

static int dev_read(uint32_t blk, uint32_t n, void* buf) { return g_dev.read(g_dev.ctx, blk * SPB, n * SPB, buf); }
static int dev_write(uint32_t blk, uint32_t n, const void* buf) { return g_dev.write(g_dev.ctx, blk * SPB, n * SPB, buf); }

static int write_back(cblk_t* c) {
    if (!c->dirty) return 0;
    if (dev_write(c->blk, 1, c->data) != 0) return -1;
    c->dirty = 0;
    g_ndirty--;
    return 0;
}

/* clean blocks not pinned, the least recently used first, until `want` bytes went */
static uint32_t evict(uint32_t want) {
    uint32_t got = 0;
    for (cblk_t* c = g_tail; c && got < want && g_ncache > CACHE_MIN; ) {
        cblk_t* p = c->prev;
        if (!c->pins && !c->dirty) { cdrop(c); got += BS; }
        c = p;
    }
    return got;
}

/* block blk in the cache, pinned (cput when done): read from the disk
 * unless fresh (a block about to be written whole, or newly given out:
 * zeros). NULL: no memory, or the disk failed */
static cblk_t* cget(uint32_t blk, int fresh) {
    cblk_t* c = cfind(blk);
    if (c) {
        c->pins++;
        c->used = timer_ms();
        lru_unlink(c);
        lru_front(c);
        if (fresh) memset(c->data, 0, BS);
        return c;
    }
    /* RAM short: the cache gives back first */
    uint32_t total = kheap_total_bytes();
    if (g_ncache > CACHE_MIN && total - kheap_used_bytes() < total / 16u) evict(64u * BS);
    c = (cblk_t*)kzalloc(sizeof(cblk_t));
    uint8_t* d = c ? (uint8_t*)kmalloc(BS) : 0;
    if (!d) { kfree(c); evict(16u * BS); return 0; }
    c->blk = blk;
    c->data = d;
    c->pins = 1;
    c->used = timer_ms();
    if (fresh) memset(d, 0, BS);
    else if (dev_read(blk, 1, d) != 0) { kfree(d); kfree(c); return 0; }
    c->hnext = g_hash[hb(blk)];
    g_hash[hb(blk)] = c;
    lru_front(c);
    g_ncache++;
    return c;
}

/* a block of the filesystem's own structures (what the journal carries) */
static cblk_t* cmeta(uint32_t blk, int fresh) {
    cblk_t* c = cget(blk, fresh);
    if (c) c->meta = 1;
    return c;
}

static void cput(cblk_t* c) { if (c && c->pins) c->pins--; }
static void cdirty(cblk_t* c) { if (!c->dirty) { c->dirty = 1; g_ndirty++; } }

/* a freed block: its cached copy (written or not) means nothing any more */
static void cforget(uint32_t blk) {
    cblk_t* c = cfind(blk);
    if (c && !c->pins) cdrop(c);
    else if (c && c->dirty) { c->dirty = 0; g_ndirty--; }
}

/* ── superblock fields ────────────────────────────────────────────── */

static uint32_t sb32(uint32_t off) { return g32(g_sb + off); }

static uint64_t free_blocks_total(void) {
    uint64_t n = 0;
    for (uint32_t g = 0; g < g_groups; g++) n += g16(desc(g) + 0x0C);
    return n;
}
static uint32_t free_inodes_total(void) {
    uint32_t n = 0;
    for (uint32_t g = 0; g < g_groups; g++) n += g16(desc(g) + 0x0E);
    return n;
}

/* the superblock and the changed descriptor blocks, into the cache */
static int meta_out(void) {
    if (!g_sb_dirty) return 0;
    p32(g_sb + 0x0C, (uint32_t)free_blocks_total());
    p32(g_sb + 0x10, free_inodes_total());
    p32(g_sb + 0x30, now());                                   /* s_wtime */
    cblk_t* b0 = cmeta(0, 0);
    if (!b0) return -1;
    memcpy(b0->data + 1024, g_sb, 1024);
    if (g_hidden) p16(b0->data + 1024 + 0x38, 0);              /* (not ext4 to anyone yet) */
    cdirty(b0);
    cput(b0);
    for (uint32_t i = 0; i < g_gdt_blocks; i++) {
        if (!g_gdt_dirty[i]) continue;
        cblk_t* c = cmeta(1u + i, 1);
        if (!c) return -1;
        uint32_t n = g_groups * DESC - i * BS;
        memcpy(c->data, g_gdt + i * BS, n < BS ? n : BS);
        cdirty(c);
        cput(c);
        g_gdt_dirty[i] = 0;
    }
    g_sb_dirty = 0;
    return 0;
}

/* ── the bitmaps: giving out and taking back blocks and inodes ─────── */

static uint32_t bb_of(uint32_t g) { return g32(desc(g) + 0x00); }
static uint32_t ib_of(uint32_t g) { return g32(desc(g) + 0x04); }
static uint32_t it_of(uint32_t g) { return g32(desc(g) + 0x08); }
static uint32_t gblocks(uint32_t g) { return g + 1 < g_groups ? BPG : g_nblocks - g * BPG; }

static void add16(uint8_t* p, int d) { p16(p, (uint32_t)((int)g16(p) + d)); }

/* up to want free blocks in a row, from goal on (or anywhere): the first, *got
 * how many (they are in use from now on); 0: the disk is full */
static uint32_t balloc(uint32_t goal, uint32_t want, uint32_t* got) {
    if (goal >= g_nblocks) goal = 0;
    uint32_t g0 = goal / BPG;
    for (uint32_t k = 0; k <= g_groups; k++) {
        uint32_t g = (g0 + k) % g_groups;
        if (!g16(desc(g) + 0x0C)) continue;
        cblk_t* c = cmeta(bb_of(g), 0);
        if (!c) return 0;
        uint32_t n = gblocks(g), start = (k == 0) ? goal % BPG : 0;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t from = pass ? 0 : start, to = pass ? start : n;
            for (uint32_t i = from; i < to; ) {
                if ((i & 7) == 0 && c->data[i >> 3] == 0xFF && i + 8 <= to) { i += 8; continue; }
                if (c->data[i >> 3] & (1u << (i & 7))) { i++; continue; }
                uint32_t r = 0;
                while (i + r < n && r < want && !(c->data[(i + r) >> 3] & (1u << ((i + r) & 7)))) r++;
                for (uint32_t j = i; j < i + r; j++) c->data[j >> 3] |= (uint8_t)(1u << (j & 7));
                cdirty(c);
                cput(c);
                add16(desc(g) + 0x0C, -(int)r);
                desc_changed(g);
                *got = r;
                return g * BPG + i;
            }
        }
        cput(c);
    }
    *got = 0;
    return 0;
}

/* want free blocks in a row anywhere (across groups too), else the longest
 * run there is: the first, *got how many - in use from now on */
static uint32_t balloc_contig(uint32_t want, uint32_t* got) {
    uint32_t best = 0, best_len = 0, cur = 0, cur_len = 0;
    for (uint32_t g = 0; g < g_groups && best_len < want; g++) {
        if (!g16(desc(g) + 0x0C)) { cur_len = 0; continue; }
        cblk_t* c = cmeta(bb_of(g), 0);
        if (!c) { *got = 0; return 0; }
        uint32_t n = gblocks(g);
        for (uint32_t i = 0; i < n && best_len < want; i++) {
            if ((i & 7) == 0 && i + 8 <= n && c->data[i >> 3] == 0xFF) { cur_len = 0; i += 7; continue; }
            if (c->data[i >> 3] & (1u << (i & 7))) { cur_len = 0; continue; }
            if (!cur_len) cur = g * BPG + i;
            if (++cur_len > best_len) { best = cur; best_len = cur_len; }
        }
        cput(c);
    }
    if (!best_len) { *got = 0; return 0; }
    if (best_len > want) best_len = want;
    for (uint32_t b = best; b < best + best_len; ) {              /* taken, group by group */
        uint32_t g = b / BPG, i = b % BPG, k = BPG - i < best + best_len - b ? BPG - i : best + best_len - b;
        cblk_t* c = cmeta(bb_of(g), 0);
        if (!c) break;
        for (uint32_t j = i; j < i + k; j++) c->data[j >> 3] |= (uint8_t)(1u << (j & 7));
        cdirty(c);
        cput(c);
        add16(desc(g) + 0x0C, -(int)k);
        desc_changed(g);
        b += k;
    }
    *got = best_len;
    return best;
}

static void bfree(uint32_t blk, uint32_t n) {
    while (n) {
        uint32_t g = blk / BPG, i = blk % BPG, k = BPG - i < n ? BPG - i : n;
        cblk_t* c = cmeta(bb_of(g), 0);
        if (!c) return;
        uint32_t freed = 0;
        for (uint32_t j = i; j < i + k; j++) {
            if (c->data[j >> 3] & (1u << (j & 7))) { c->data[j >> 3] &= (uint8_t)~(1u << (j & 7)); freed++; }
            cforget(g * BPG + j);
        }
        cdirty(c);
        cput(c);
        add16(desc(g) + 0x0C, (int)freed);
        desc_changed(g);
        blk += k;
        n -= k;
    }
}

/* a free inode (a folder: in the group with the most free; a file: near
 * its folder's): its number, 0 if none */
static uint32_t ialloc(uint32_t near_ino, int is_dir) {
    /* near the folder it is made in (both files and folders, as ext2 did): what is
     * made together lies together, and the free space stays in long runs */
    uint32_t best = (near_ino ? (near_ino - 1) / g_ipg : 0);
    (void)is_dir;
    for (uint32_t k = 0; k < g_groups; k++) {
        uint32_t g = (best + k) % g_groups;
        uint8_t* d = desc(g);
        if (!g16(d + 0x0E)) continue;
        cblk_t* c;
        if (g16(d + 0x12) & BG_INODE_UNINIT) {
            /* first use of this group's inodes: its bitmap starts empty (the
             * bits past the group's inodes set, as ext4 has them) */
            c = cmeta(ib_of(g), 1);
            if (!c) return 0;
            for (uint32_t j = g_ipg; j < BS * 8; j++) c->data[j >> 3] |= (uint8_t)(1u << (j & 7));
            p16(d + 0x12, g16(d + 0x12) & ~BG_INODE_UNINIT);
        } else {
            c = cmeta(ib_of(g), 0);
            if (!c) return 0;
        }
        for (uint32_t i = 0; i < g_ipg; i++) {
            if (c->data[i >> 3] & (1u << (i & 7))) continue;
            c->data[i >> 3] |= (uint8_t)(1u << (i & 7));
            cdirty(c);
            cput(c);
            add16(d + 0x0E, -1);
            if (is_dir) add16(d + 0x10, 1);
            uint32_t unused = g16(d + 0x1C);
            if (g_ipg - unused <= i) p16(d + 0x1C, g_ipg - (i + 1));    /* bg_itable_unused */
            desc_changed(g);
            return g * g_ipg + i + 1;
        }
        cdirty(c);
        cput(c);
    }
    return 0;
}

static void ifree(uint32_t ino, int is_dir) {
    uint32_t g = (ino - 1) / g_ipg, i = (ino - 1) % g_ipg;
    cblk_t* c = cmeta(ib_of(g), 0);
    if (!c) return;
    c->data[i >> 3] &= (uint8_t)~(1u << (i & 7));
    cdirty(c);
    cput(c);
    add16(desc(g) + 0x0E, 1);
    if (is_dir) add16(desc(g) + 0x10, -1);
    desc_changed(g);
}

/* ── inodes ───────────────────────────────────────────────────────── */

typedef struct { uint8_t r[ISZ]; } raw_inode_t;

static int iread(uint32_t ino, raw_inode_t* in) {
    if (!ino || ino > g_groups * g_ipg) return -1;
    uint32_t g = (ino - 1) / g_ipg, i = (ino - 1) % g_ipg;
    cblk_t* c = cmeta(it_of(g) + (i * ISZ) / BS, 0);
    if (!c) return -1;
    memcpy(in->r, c->data + (i * ISZ) % BS, ISZ);
    cput(c);
    return 0;
}

static int iwrite(uint32_t ino, const raw_inode_t* in) {
    uint32_t g = (ino - 1) / g_ipg, i = (ino - 1) % g_ipg;
    cblk_t* c = cmeta(it_of(g) + (i * ISZ) / BS, 0);
    if (!c) return -1;
    memcpy(c->data + (i * ISZ) % BS, in->r, ISZ);
    cdirty(c);
    cput(c);
    return 0;
}

static uint32_t i_mode(const raw_inode_t* in) { return g16(in->r + 0x00); }
static uint64_t i_size(const raw_inode_t* in) { return (uint64_t)g32(in->r + 0x04) | (uint64_t)g32(in->r + 0x6C) << 32; }
static void     set_size(raw_inode_t* in, uint64_t s) { p32(in->r + 0x04, (uint32_t)s); p32(in->r + 0x6C, (uint32_t)(s >> 32)); }
static uint32_t i_links(const raw_inode_t* in) { return g16(in->r + 0x1A); }
static uint64_t i_blocks(const raw_inode_t* in) { return (uint64_t)g32(in->r + 0x1C) | (uint64_t)g16(in->r + 0x74) << 32; }
static void     set_blocks(raw_inode_t* in, uint64_t b) { p32(in->r + 0x1C, (uint32_t)b); p16(in->r + 0x74, (uint32_t)(b >> 32)); }
static int      is_dir_i(const raw_inode_t* in) { return (i_mode(in) & EXT4_S_IFMT) == EXT4_S_IFDIR; }
static int      fast_link(const raw_inode_t* in) {
    return (i_mode(in) & EXT4_S_IFMT) == EXT4_S_IFLNK && !(g32(in->r + 0x20) & EXT4_EXTENTS_FL);
}

/* a fresh inode: its fields, the extent tree empty */
static void init_inode(raw_inode_t* in, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t links) {
    memset(in, 0, sizeof(*in));
    uint32_t t = now();
    p16(in->r + 0x00, mode);
    p16(in->r + 0x02, uid);
    p16(in->r + 0x78, uid >> 16);
    p16(in->r + 0x18, gid);
    p16(in->r + 0x7A, gid >> 16);
    p32(in->r + 0x08, t);
    p32(in->r + 0x0C, t);
    p32(in->r + 0x10, t);
    p32(in->r + 0x90, t);                                     /* i_crtime */
    p16(in->r + 0x1A, links);
    p16(in->r + 0x80, 32);                                    /* i_extra_isize */
    p32(in->r + 0x64, t ^ 0x5A5A5A5Au);                       /* i_generation */
    if ((mode & EXT4_S_IFMT) != EXT4_S_IFLNK) {
        p32(in->r + 0x20, EXT4_EXTENTS_FL);
        uint8_t* h = in->r + 0x28;
        p16(h, EH_MAGIC);
        p16(h + 2, 0);
        p16(h + 4, 4);                                        /* eh_max: 4 in the inode */
        p16(h + 6, 0);
    }
}

/* ── extent maps ───────────────────────────────────────────────────
 * A file's extents are read into a list, changed there, and written back
 * as a new tree: in the inode (up to 4), or leaves of 340 under it (depth
 * 1), or one more level (depth 2: 462400 extents) - the old tree's blocks
 * go back. Files are written mostly at their end, so the list stays short. */

typedef struct { uint32_t lblk, pblk, len; uint8_t unw; } ext_t;
typedef struct {
    ext_t*    e;
    uint32_t  n, cap;
    uint32_t* tree;                         /* the tree's own blocks */
    uint32_t  ntree, tcap;
} emap_t;

#define LEAF_MAX ((BS - 12u) / 12u)         /* 340 */

static void emap_free(emap_t* m) { kfree(m->e); kfree(m->tree); memset(m, 0, sizeof(*m)); }

static int emap_push(emap_t* m, uint32_t lblk, uint32_t pblk, uint32_t len, int unw) {
    if (m->n && m->e[m->n - 1].lblk + m->e[m->n - 1].len == lblk && m->e[m->n - 1].pblk + m->e[m->n - 1].len == pblk &&
        m->e[m->n - 1].unw == (uint8_t)unw && m->e[m->n - 1].len + len <= (unw ? EXT_MAX_LEN - 1u : EXT_MAX_LEN)) {
        m->e[m->n - 1].len += len;
        return 0;
    }
    if (m->n == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 8;
        ext_t* e = (ext_t*)krealloc(m->e, cap * sizeof(ext_t));
        if (!e) return -1;
        m->e = e;
        m->cap = cap;
    }
    m->e[m->n++] = (ext_t){ lblk, pblk, len, (uint8_t)unw };
    return 0;
}

static int tree_push(emap_t* m, uint32_t blk) {
    if (m->ntree == m->tcap) {
        uint32_t cap = m->tcap ? m->tcap * 2 : 4;
        uint32_t* t = (uint32_t*)krealloc(m->tree, cap * sizeof(uint32_t));
        if (!t) return -1;
        m->tree = t;
        m->tcap = cap;
    }
    m->tree[m->ntree++] = blk;
    return 0;
}

/* a node of the tree (12-byte header, then entries): its extents into m */
static int load_node(emap_t* m, const uint8_t* h, uint32_t room, int depth_left) {
    if (g16(h) != EH_MAGIC) return -1;
    uint32_t n = g16(h + 2), depth = g16(h + 6);
    if (n > room || (int)depth != depth_left) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = h + 12 + i * 12;
        if (!depth) {
            uint32_t len = g16(e + 4), unw = len > EXT_MAX_LEN;
            if (unw) len -= EXT_MAX_LEN;
            uint32_t pblk = g32(e + 8);
            if (g16(e + 6) || !len || pblk + len > g_nblocks) return -1;        /* (no 48-bit block numbers) */
            if (emap_push(m, g32(e), pblk, len, (int)unw) != 0) return -1;
        } else {
            uint32_t leaf = g32(e + 4);
            if (g16(e + 8) || leaf >= g_nblocks || tree_push(m, leaf) != 0) return -1;
            cblk_t* c = cmeta(leaf, 0);
            if (!c) return -1;
            int rc = load_node(m, c->data, LEAF_MAX, (int)depth - 1);
            cput(c);
            if (rc) return -1;
        }
    }
    return 0;
}

static int emap_load(const raw_inode_t* in, emap_t* m) {
    memset(m, 0, sizeof(*m));
    if (!(g32(in->r + 0x20) & EXT4_EXTENTS_FL)) return 0;     /* (a fast link: no blocks) */
    const uint8_t* h = in->r + 0x28;
    if (g16(h) != EH_MAGIC) return -1;
    if (load_node(m, h, 4, g16(h + 6)) != 0) { emap_free(m); return -1; }
    return 0;
}

static void put_leaf(uint8_t* e, const ext_t* x) {
    p32(e, x->lblk);
    p16(e + 4, x->len + (x->unw ? EXT_MAX_LEN : 0));
    p16(e + 6, 0);
    p32(e + 8, x->pblk);
}

static void put_hdr(uint8_t* h, uint32_t n, uint32_t max, uint32_t depth) {
    p16(h, EH_MAGIC);
    p16(h + 2, n);
    p16(h + 4, max);
    p16(h + 6, depth);
    p32(h + 8, 0);
}

static void put_idx(uint8_t* e, uint32_t lblk, uint32_t blk) {
    p32(e, lblk);
    p32(e + 4, blk);
    p16(e + 8, 0);
    p16(e + 10, 0);
}

/* the list back as a tree in the inode; the old tree's blocks freed and
 * i_blocks counted again. 0, or -1 (the disk is full) */
static int emap_store(raw_inode_t* in, emap_t* m) {
    uint32_t goal = m->n ? m->e[0].pblk : 0;
    uint32_t* old = m->tree;
    uint32_t nold = m->ntree;
    m->tree = 0;
    m->ntree = m->tcap = 0;
    uint8_t* root = in->r + 0x28;
    memset(root, 0, 60);
    uint64_t data = 0;
    for (uint32_t i = 0; i < m->n; i++) data += m->e[i].len;
    int rc = 0;
    if (m->n <= 4) {
        put_hdr(root, m->n, 4, 0);
        for (uint32_t i = 0; i < m->n; i++) put_leaf(root + 12 + i * 12, &m->e[i]);
    } else {
        uint32_t nleaf = (m->n + LEAF_MAX - 1) / LEAF_MAX;
        uint32_t nidx = nleaf <= 4 ? 0 : (nleaf + LEAF_MAX - 1) / LEAF_MAX;
        uint32_t* leaf = nidx > 4 ? 0 : (uint32_t*)kmalloc((nleaf + nidx) * sizeof(uint32_t));
        if (!leaf) { rc = -1; goto out; }
        for (uint32_t k = 0; k < nleaf + nidx; k++) {
            uint32_t got;
            leaf[k] = balloc(goal, 1, &got);
            if (!got) { for (uint32_t j = 0; j < k; j++) bfree(leaf[j], 1); kfree(leaf); m->ntree = 0; rc = -1; goto out; }
            tree_push(m, leaf[k]);
        }
        for (uint32_t k = 0; k < nleaf; k++) {
            cblk_t* c = cmeta(leaf[k], 1);
            if (!c) { rc = -1; break; }
            uint32_t from = k * LEAF_MAX, cnt = m->n - from < LEAF_MAX ? m->n - from : LEAF_MAX;
            put_hdr(c->data, cnt, LEAF_MAX, 0);
            for (uint32_t i = 0; i < cnt; i++) put_leaf(c->data + 12 + i * 12, &m->e[from + i]);
            cdirty(c);
            cput(c);
        }
        if (!nidx) {
            put_hdr(root, nleaf, 4, 1);
            for (uint32_t k = 0; k < nleaf; k++) put_idx(root + 12 + k * 12, m->e[k * LEAF_MAX].lblk, leaf[k]);
        } else {
            for (uint32_t k = 0; k < nidx; k++) {
                cblk_t* c = cmeta(leaf[nleaf + k], 1);
                if (!c) { rc = -1; break; }
                uint32_t from = k * LEAF_MAX, cnt = nleaf - from < LEAF_MAX ? nleaf - from : LEAF_MAX;
                put_hdr(c->data, cnt, LEAF_MAX, 1);
                for (uint32_t i = 0; i < cnt; i++) put_idx(c->data + 12 + i * 12, m->e[(from + i) * LEAF_MAX].lblk, leaf[from + i]);
                cdirty(c);
                cput(c);
            }
            put_hdr(root, nidx, 4, 2);
            for (uint32_t k = 0; k < nidx; k++) put_idx(root + 12 + k * 12, m->e[k * LEAF_MAX * LEAF_MAX].lblk, leaf[nleaf + k]);
        }
        kfree(leaf);
    }
out:
    for (uint32_t i = 0; i < nold; i++) bfree(old[i], 1);
    kfree(old);
    set_blocks(in, (data + m->ntree) * SPB);
    return rc;
}

/* the physical block of logical block lblk (0: a hole), *unw: not written;
 * *run: how many blocks from it on are alike (mapped in a row, or the hole) */
static uint32_t emap_find(const emap_t* m, uint32_t lblk, int* unw, uint32_t* run) {
    uint32_t lo = 0, hi = m->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->e[mid].lblk + m->e[mid].len <= lblk) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->n && m->e[lo].lblk <= lblk) {
        if (unw) *unw = m->e[lo].unw;
        if (run) *run = m->e[lo].lblk + m->e[lo].len - lblk;
        return m->e[lo].pblk + (lblk - m->e[lo].lblk);
    }
    if (unw) *unw = 0;
    if (run) *run = lo < m->n ? m->e[lo].lblk - lblk : 0xFFFFFFFFu;
    return 0;
}

/* [lblk, lblk + len) out of the list (its blocks freed if asked) */
static int emap_cut(emap_t* m, uint32_t lblk, uint32_t len, int free_blocks) {
    uint32_t end = lblk + len;
    emap_t r;
    memset(&r, 0, sizeof(r));
    for (uint32_t i = 0; i < m->n; i++) {
        ext_t x = m->e[i];
        uint32_t xe = x.lblk + x.len;
        if (xe <= lblk || x.lblk >= end) { if (emap_push(&r, x.lblk, x.pblk, x.len, x.unw)) goto fail; continue; }
        if (x.lblk < lblk && emap_push(&r, x.lblk, x.pblk, lblk - x.lblk, x.unw)) goto fail;
        uint32_t a = x.lblk > lblk ? x.lblk : lblk, b = xe < end ? xe : end;
        if (free_blocks) bfree(x.pblk + (a - x.lblk), b - a);
        if (xe > end && emap_push(&r, end, x.pblk + (end - x.lblk), xe - end, x.unw)) goto fail;
    }
    kfree(m->e);
    m->e = r.e;
    m->n = r.n;
    m->cap = r.cap;
    return 0;
fail:
    kfree(r.e);
    return -1;
}

/* adds a run where the list has a hole, keeping it sorted */
static int emap_add(emap_t* m, uint32_t lblk, uint32_t pblk, uint32_t len, int unw) {
    if (!m->n || m->e[m->n - 1].lblk + m->e[m->n - 1].len <= lblk) return emap_push(m, lblk, pblk, len, unw);
    emap_t r;
    memset(&r, 0, sizeof(r));
    int done = 0;
    for (uint32_t i = 0; i < m->n; i++) {
        if (!done && m->e[i].lblk > lblk) { if (emap_push(&r, lblk, pblk, len, unw)) goto fail; done = 1; }
        if (emap_push(&r, m->e[i].lblk, m->e[i].pblk, m->e[i].len, m->e[i].unw)) goto fail;
    }
    if (!done && emap_push(&r, lblk, pblk, len, unw)) goto fail;
    kfree(m->e);
    m->e = r.e;
    m->n = r.n;
    m->cap = r.cap;
    return 0;
fail:
    kfree(r.e);
    return -1;
}

/* ── file data ────────────────────────────────────────────────────── */

#define DIRECT_MIN 16u                       /* runs this long bypass the cache */

/* read-ahead, per file as Linux has it: a file read where its last read ended
 * gets a window twice as big each time (up to 1 MiB), one read at random
 * places gets none - the block it asked for alone */
#define RA_MAX 256u
typedef struct { uint32_t ino; uint64_t next; uint32_t win; } ra_t;
static ra_t     g_ra[8];
static uint32_t g_ra_turn;
static uint8_t  g_ra_buf[RA_MAX * BS];

static uint32_t ra_window(uint32_t ino, uint64_t off, uint32_t len) {
    ra_t* r = 0;
    for (int i = 0; i < 8; i++) if (g_ra[i].ino == ino) r = &g_ra[i];
    if (!r) { r = &g_ra[g_ra_turn++ % 8]; r->ino = ino; r->win = off ? 1 : 4; r->next = ~0ull; }
    if (off == r->next) r->win = r->win * 2 > RA_MAX ? RA_MAX : (r->win ? r->win * 2 : 4);
    else if (off) r->win = 1;
    r->next = off + len;
    return r->win;
}

static long do_read(const raw_inode_t* in, const emap_t* m, uint64_t off, uint8_t* buf, uint32_t len, uint32_t window) {
    uint64_t size = i_size(in);
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    if (fast_link(in)) { memcpy(buf, in->r + 0x28 + off, len); return (long)len; }
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lblk = (uint32_t)(pos / BS), boff = (uint32_t)(pos % BS), run = 0;
        int unw = 0;
        uint32_t pblk = emap_find(m, lblk, &unw, &run);
        uint32_t want = len - done;
        if (!pblk || unw) {                                       /* a hole: zeros */
            uint64_t span = (uint64_t)run * BS - boff;
            uint32_t n = span < want ? (uint32_t)span : want;
            memset(buf + done, 0, n);
            done += n;
            continue;
        }
        /* whole blocks in one go, straight from the disk (cached ones over them) */
        uint32_t whole = boff ? 0 : want / BS;
        if (whole > run) whole = run;
        if (whole >= DIRECT_MIN) {
            if (whole > 256) whole = 256;
            if (dev_read(pblk, whole, buf + done) != 0) return -1;
            for (uint32_t k = 0; k < whole; k++) {
                cblk_t* c = cfind(pblk + k);
                if (c) memcpy(buf + done + k * BS, c->data, BS);
            }
            done += whole * BS;
            continue;
        }
        /* a part: through the cache, reading ahead what follows in the run */
        if (!cfind(pblk) && run > 1 && window > 1) {
            uint32_t ra = run < window ? run : window, k = 1;
            while (k < ra && !cfind(pblk + k)) k++;
            if (dev_read(pblk, k, g_ra_buf) == 0)
                for (uint32_t j = 0; j < k; j++) {
                    cblk_t* c = cget(pblk + j, 1);
                    if (c) { memcpy(c->data, g_ra_buf + j * BS, BS); cput(c); }
                }
        }
        cblk_t* c = cget(pblk, 0);
        if (!c) return -1;
        uint32_t n = BS - boff < want ? BS - boff : want;
        memcpy(buf + done, c->data + boff, n);
        cput(c);
        done += n;
    }
    return (long)len;
}

/* writes into the file's blocks, giving out the ones missing (next to the
 * blocks before them when free); unwritten ones become written */
static int do_write(raw_inode_t* in, emap_t* m, uint32_t ino, uint64_t off, const uint8_t* buf, uint32_t len, int* changed, int meta) {
    uint64_t old_size = i_size(in);
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lblk = (uint32_t)(pos / BS), boff = (uint32_t)(pos % BS), run = 0;
        int unw = 0;
        uint32_t pblk = emap_find(m, lblk, &unw, &run);
        uint32_t want = len - done;
        uint32_t span = (uint32_t)(((uint64_t)boff + want + BS - 1) / BS);   /* blocks this write still covers */
        int fresh_blk = 0;
        if (!pblk) {
            uint32_t need = span < run ? span : run;
            uint32_t goal = 0;
            if (lblk) {
                int u2;
                uint32_t prev = emap_find(m, lblk - 1, &u2, 0);
                if (prev) goal = prev + 1;
            }
            if (!goal) goal = ((ino - 1) / g_ipg) * BPG;
            uint32_t got;
            pblk = balloc(goal, need, &got);
            if (!got) {
                if (done && off + done > old_size) set_size(in, off + done);
                return -1;
            }
            if (emap_add(m, lblk, pblk, got, 0) != 0) {
                bfree(pblk, got);
                if (done && off + done > old_size) set_size(in, off + done);
                return -1;
            }
            *changed = 1;
            run = got;
            fresh_blk = 1;
        } else if (unw) {
            uint32_t n = span < run ? span : run;
            if (emap_cut(m, lblk, n, 0) != 0 || emap_add(m, lblk, pblk, n, 0) != 0) return -1;
            *changed = 1;
            run = n;
            fresh_blk = 1;
        }
        uint32_t whole = boff ? 0 : want / BS;
        if (whole > run) whole = run;
        if (whole >= DIRECT_MIN) {
            if (whole > 256) whole = 256;
            if (dev_write(pblk, whole, buf + done) != 0) return -1;
            for (uint32_t k = 0; k < whole; k++) {          /* (a cached copy is older now) */
                cblk_t* c = cfind(pblk + k);
                if (c) {
                    memcpy(c->data, buf + done + k * BS, BS);
                    if (c->dirty) { c->dirty = 0; g_ndirty--; }
                }
            }
            done += whole * BS;
            continue;
        }
        /* a part of a block: a new or unwritten one starts as zeros, and so
         * does one wholly past the old end (none of it was the file's) */
        uint32_t n = BS - boff < want ? BS - boff : want;
        int zero = fresh_blk || (boff == 0 && n == BS) || (uint64_t)lblk * BS >= old_size;
        cblk_t* c = meta ? cmeta(pblk, zero) : cget(pblk, zero);
        if (!c) return -1;
        memcpy(c->data + boff, buf + done, n);
        cdirty(c);
        cput(c);
        done += n;
        /* (whole blocks still to come of this run: the next turn) */
        (void)run;
    }
    if (off + len > old_size) set_size(in, off + len);
    return 0;
}

/* zeros from size to the end of its block (a later growth reads them) */
static void zero_tail(const emap_t* m, uint64_t size) {
    uint32_t boff = (uint32_t)(size % BS);
    if (!boff) return;
    int unw = 0;
    uint32_t pblk = emap_find(m, (uint32_t)(size / BS), &unw, 0);
    if (!pblk || unw) return;
    cblk_t* c = cget(pblk, 0);
    if (!c) return;
    memset(c->data + boff, 0, BS - boff);
    cdirty(c);
    cput(c);
}

static int do_truncate(raw_inode_t* in, emap_t* m, uint64_t size, int* changed) {
    uint64_t old = i_size(in);
    if (size < old) {
        uint32_t keep = (uint32_t)((size + BS - 1) / BS);
        if (m->n) {
            ext_t* last = &m->e[m->n - 1];
            uint32_t end = last->lblk + last->len;
            if (end > keep) {
                if (emap_cut(m, keep, end - keep, 1) != 0) return -1;
                *changed = 1;
            }
        }
        zero_tail(m, size);
    } else if (size > old) {
        zero_tail(m, old);
    }
    set_size(in, size);
    return 0;
}

static uint32_t lookup_locked(uint32_t dir, const char* name);

static void set_times(raw_inode_t* in, int a, int m, int c) {
    uint32_t t = now();
    if (a) p32(in->r + 0x08, t);
    if (c) p32(in->r + 0x0C, t);
    if (m) p32(in->r + 0x10, t);
}

/* ── directories ───────────────────────────────────────────────────
 * A folder's blocks hold entries: inode, rec_len (to the next entry, the
 * last one to the block's end), name_len, file type, the name. */

#define DE_LEN(nl) ((8u + (nl) + 3u) & ~3u)

typedef struct {
    raw_inode_t in;
    emap_t      m;
    uint32_t    nblk;
} dir_t;

static int dir_open(uint32_t ino, dir_t* d) {
    if (iread(ino, &d->in) != 0 || !is_dir_i(&d->in)) return -1;
    if (emap_load(&d->in, &d->m) != 0) return -1;
    d->nblk = (uint32_t)(i_size(&d->in) / BS);
    return 0;
}

static void dir_close(dir_t* d) { emap_free(&d->m); }

/* block k of the folder in the cache (pinned), or NULL */
static cblk_t* dir_block(dir_t* d, uint32_t k) {
    int unw;
    uint32_t p = emap_find(&d->m, k, &unw, 0);
    return p ? cmeta(p, 0) : 0;
}

static int de_ok(const uint8_t* b, uint32_t off) {
    uint32_t rl = g16(b + off + 4), nl = b[off + 6];
    return rl >= 12 && !(rl & 3) && off + rl <= BS && DE_LEN(nl) <= rl;
}

/* the entry name: 1 and where (block k, offset, the entry before it or ~0) */
static int dir_find(dir_t* d, const char* name, uint32_t* ino, uint32_t* type, uint32_t* bk, uint32_t* boff, uint32_t* bprev) {
    uint32_t nl = (uint32_t)strlen(name);
    for (uint32_t k = 0; k < d->nblk; k++) {
        cblk_t* c = dir_block(d, k);
        if (!c) continue;
        uint32_t prev = 0xFFFFFFFFu;
        for (uint32_t off = 0; off < BS; ) {
            if (!de_ok(c->data, off)) break;
            uint8_t* e = c->data + off;
            if (g32(e) && e[6] == nl && !memcmp(e + 8, name, nl)) {
                if (ino) *ino = g32(e);
                if (type) *type = e[7];
                if (bk) *bk = k;
                if (boff) *boff = off;
                if (bprev) *bprev = prev;
                cput(c);
                return 1;
            }
            prev = off;
            off += g16(e + 4);
        }
        cput(c);
    }
    return 0;
}

static void put_de(uint8_t* e, uint32_t ino, uint32_t rec, const char* name, uint32_t nl, uint32_t type) {
    p32(e, ino);
    p16(e + 4, rec);
    e[6] = (uint8_t)nl;
    e[7] = (uint8_t)type;
    memcpy(e + 8, name, nl);
    for (uint32_t i = 8 + nl; i < DE_LEN(nl) && i < rec; i++) e[i] = 0;
}

/* name -> ino in folder dino: 0, or -1 (no room, the name too long) */
static int dir_add(uint32_t dino, const char* name, uint32_t ino, uint32_t type) {
    uint32_t nl = (uint32_t)strlen(name);
    if (!nl || nl > 255) return -1;
    uint32_t need = DE_LEN(nl);
    dir_t d;
    if (dir_open(dino, &d) != 0) return -1;
    for (uint32_t k = 0; k < d.nblk; k++) {
        cblk_t* c = dir_block(&d, k);
        if (!c) continue;
        for (uint32_t off = 0; off < BS; ) {
            if (!de_ok(c->data, off)) break;
            uint8_t* e = c->data + off;
            uint32_t rl = g16(e + 4), used = g32(e) ? DE_LEN(e[6]) : 0;
            if (rl - used >= need) {
                if (used) {
                    p16(e + 4, used);
                    put_de(e + used, ino, rl - used, name, nl, type);
                } else {
                    put_de(e, ino, rl, name, nl, type);
                }
                cdirty(c);
                cput(c);
                set_times(&d.in, 0, 1, 1);
                iwrite(dino, &d.in);
                dir_close(&d);
                return 0;
            }
            off += rl;
        }
        cput(c);
    }
    /* full: one more block */
    int changed = 0;
    uint8_t* blk = (uint8_t*)kzalloc(BS);
    int rc = -1;
    if (blk) {
        put_de(blk, ino, BS, name, nl, type);
        rc = do_write(&d.in, &d.m, dino, (uint64_t)d.nblk * BS, blk, BS, &changed, 1);
        kfree(blk);
    }
    if (rc == 0 && changed) rc = emap_store(&d.in, &d.m);
    if (rc == 0) {
        set_times(&d.in, 0, 1, 1);
        iwrite(dino, &d.in);
    }
    dir_close(&d);
    return rc;
}

static int dir_remove(uint32_t dino, const char* name) {
    dir_t d;
    if (dir_open(dino, &d) != 0) return -1;
    uint32_t k, off, prev;
    if (!dir_find(&d, name, 0, 0, &k, &off, &prev)) { dir_close(&d); return -1; }
    cblk_t* c = dir_block(&d, k);
    if (!c) { dir_close(&d); return -1; }
    if (prev != 0xFFFFFFFFu) p16(c->data + prev + 4, g16(c->data + prev + 4) + g16(c->data + off + 4));
    else p32(c->data + off, 0);                               /* (the block's first entry: emptied) */
    cdirty(c);
    cput(c);
    set_times(&d.in, 0, 1, 1);
    iwrite(dino, &d.in);
    dir_close(&d);
    return 0;
}

static int dir_empty(uint32_t dino) {
    dir_t d;
    if (dir_open(dino, &d) != 0) return 0;
    int empty = 1;
    for (uint32_t k = 0; k < d.nblk && empty; k++) {
        cblk_t* c = dir_block(&d, k);
        if (!c) continue;
        for (uint32_t off = 0; off < BS && empty; ) {
            if (!de_ok(c->data, off)) break;
            uint8_t* e = c->data + off;
            if (g32(e) && !(e[6] == 1 && e[8] == '.') && !(e[6] == 2 && e[8] == '.' && e[9] == '.')) empty = 0;
            off += g16(e + 4);
        }
        cput(c);
    }
    dir_close(&d);
    return empty;
}

/* ── inodes made and dropped ──────────────────────────────────────── */

/* the inode's blocks back, then the inode itself */
static void drop_inode(uint32_t ino, raw_inode_t* in) {
    int dir = is_dir_i(in);
    emap_t m;
    if (emap_load(in, &m) == 0) {
        for (uint32_t i = 0; i < m.n; i++) bfree(m.e[i].pblk, m.e[i].len);
        m.n = 0;
        emap_store(in, &m);                                     /* (frees the tree's blocks) */
        emap_free(&m);
    }
    set_size(in, 0);
    p16(in->r + 0x1A, 0);
    p32(in->r + 0x14, now());                                 /* i_dtime */
    iwrite(ino, in);
    ifree(ino, dir);
}

static void links_add(uint32_t ino, int d) {
    raw_inode_t in;
    if (iread(ino, &in) != 0) return;
    uint32_t l = i_links(&in);
    if (d > 0 && l < 65000) l++;
    else if (d < 0 && l > 1 && l < 65000) l--;
    p16(in.r + 0x1A, l);
    set_times(&in, 0, 0, 1);
    iwrite(ino, &in);
}

/* ── the operations (each holds the lock) ──────────────────────────── */

int ext4_mounted(void) { return g_mounted; }
const char* ext4_label(void) { return g_label; }

uint32_t ext4_lookup(uint32_t dir, const char* name, uint32_t* type) {
    if (!g_mounted) return 0;
    lock();
    dir_t d;
    uint32_t ino = 0;
    if (dir_open(dir, &d) == 0) {
        if (!dir_find(&d, name, &ino, type, 0, 0, 0)) ino = 0;
        dir_close(&d);
    }
    unlock();
    return ino;
}

int ext4_readdir(uint32_t dir, int (*fn)(void*, const char*, uint32_t, uint32_t), void* arg) {
    if (!g_mounted) return -1;
    lock();
    dir_t d;
    if (dir_open(dir, &d) != 0) { unlock(); return -1; }
    char name[256];
    int stop = 0;
    for (uint32_t k = 0; k < d.nblk && !stop; k++) {
        cblk_t* c = dir_block(&d, k);
        if (!c) continue;
        /* the block copied out: fn may take its time (the lock stays held) */
        for (uint32_t off = 0; off < BS && !stop; ) {
            if (!de_ok(c->data, off)) break;
            uint8_t* e = c->data + off;
            uint32_t nl = e[6];
            if (g32(e) && !(nl == 1 && e[8] == '.') && !(nl == 2 && e[8] == '.' && e[9] == '.')) {
                memcpy(name, e + 8, nl);
                name[nl] = 0;
                stop = fn(arg, name, g32(e), e[7]);
            }
            off += g16(e + 4);
        }
        cput(c);
    }
    dir_close(&d);
    unlock();
    return 0;
}

int ext4_stat(uint32_t ino, ext4_stat_t* st) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    int rc = iread(ino, &in);
    unlock();
    if (rc) return -1;
    st->ino = ino;
    st->mode = i_mode(&in);
    st->uid = g16(in.r + 0x02) | (uint32_t)g16(in.r + 0x78) << 16;
    st->gid = g16(in.r + 0x18) | (uint32_t)g16(in.r + 0x7A) << 16;
    st->size = i_size(&in);
    st->atime = g32(in.r + 0x08);
    st->ctime = g32(in.r + 0x0C);
    st->mtime = g32(in.r + 0x10);
    st->crtime = g32(in.r + 0x90);
    st->links = i_links(&in);
    st->blocks = i_blocks(&in);
    return 0;
}

static uint32_t make_node(uint32_t dir, const char* name, uint32_t mode, uint32_t uid, uint32_t gid, const char* link) {
    if (!g_mounted || !name || !*name || strlen(name) > 255) return 0;
    lock();
    if (lookup_locked(dir, name)) { unlock(); return 0; }
    int isdir = (mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
    uint32_t ino = ialloc(dir, isdir);
    if (!ino) { unlock(); return 0; }
    raw_inode_t in;
    init_inode(&in, mode, uid, gid, isdir ? 2 : 1);
    int rc = 0;
    if (isdir) {
        uint32_t got, blk = balloc(((ino - 1) / g_ipg) * BPG, 1, &got);
        if (!got) rc = -1;
        else {
            cblk_t* c = cmeta(blk, 1);
            if (!c) rc = -1;
            else {
                put_de(c->data, ino, 12, ".", 1, EXT4_FT_DIR);
                put_de(c->data + 12, dir, BS - 12, "..", 2, EXT4_FT_DIR);
                cdirty(c);
                cput(c);
                emap_t m;
                memset(&m, 0, sizeof(m));
                emap_push(&m, 0, blk, 1, 0);
                emap_store(&in, &m);
                emap_free(&m);
                set_size(&in, BS);
            }
        }
    } else if (link) {
        uint32_t n = (uint32_t)strlen(link);
        if (n < 60) {
            memcpy(in.r + 0x28, link, n);
            set_size(&in, n);
        } else {
            /* a long one: its target in a block, an extent tree like a file's */
            p32(in.r + 0x20, EXT4_EXTENTS_FL);
            put_hdr(in.r + 0x28, 0, 4, 0);
            emap_t m;
            memset(&m, 0, sizeof(m));
            int changed = 0;
            rc = do_write(&in, &m, ino, 0, (const uint8_t*)link, n, &changed, 0);
            if (rc == 0) rc = emap_store(&in, &m);
            emap_free(&m);
        }
    }
    if (rc == 0) rc = iwrite(ino, &in);
    if (rc == 0) rc = dir_add(dir, name, ino, isdir ? EXT4_FT_DIR : link ? EXT4_FT_LNK : EXT4_FT_REG);
    if (rc != 0) { drop_inode(ino, &in); unlock(); return 0; }
    if (isdir) links_add(dir, 1);
    unlock();
    return ino;
}

uint32_t ext4_create(uint32_t dir, const char* name, uint32_t mode, uint32_t uid, uint32_t gid) {
    return make_node(dir, name, EXT4_S_IFREG | (mode & 07777u), uid, gid, 0);
}
uint32_t ext4_mkdir(uint32_t dir, const char* name, uint32_t mode, uint32_t uid, uint32_t gid) {
    return make_node(dir, name, EXT4_S_IFDIR | (mode & 07777u), uid, gid, 0);
}
uint32_t ext4_symlink(uint32_t dir, const char* name, const char* target, uint32_t uid, uint32_t gid) {
    return make_node(dir, name, EXT4_S_IFLNK | 0777u, uid, gid, target ? target : "");
}

/* name in dir, another link to file ino (not a folder): 0, or -1 */
int ext4_link(uint32_t dir, const char* name, uint32_t ino) {
    if (!g_mounted || !name || !*name || strlen(name) > 255) return -1;
    lock();
    raw_inode_t in;
    int rc = -1;
    if (!lookup_locked(dir, name) && iread(ino, &in) == 0 && !is_dir_i(&in) && i_links(&in) < 65000) {
        uint32_t t = (i_mode(&in) & EXT4_S_IFMT) == EXT4_S_IFLNK ? EXT4_FT_LNK : EXT4_FT_REG;
        rc = dir_add(dir, name, ino, t);
        if (rc == 0) links_add(ino, 1);
    }
    unlock();
    return rc;
}

/* name out of dir; its inode loses a link (and goes with the last) */
static int unlink_locked(uint32_t dir, const char* name, int want_dir) {
    dir_t d;
    if (dir_open(dir, &d) != 0) return -1;
    uint32_t ino, type;
    int found = dir_find(&d, name, &ino, &type, 0, 0, 0);
    dir_close(&d);
    if (!found) return -1;
    raw_inode_t in;
    if (iread(ino, &in) != 0) return -1;
    if (is_dir_i(&in) != want_dir) return -1;
    if (want_dir && !dir_empty(ino)) return -1;
    if (dir_remove(dir, name) != 0) return -1;
    if (want_dir) {
        links_add(dir, -1);
        drop_inode(ino, &in);
        return 0;
    }
    uint32_t l = i_links(&in);
    if (l > 1) { p16(in.r + 0x1A, l - 1); set_times(&in, 0, 0, 1); iwrite(ino, &in); }
    else drop_inode(ino, &in);
    return 0;
}

int ext4_unlink(uint32_t dir, const char* name) {
    if (!g_mounted) return -1;
    lock();
    int rc = unlink_locked(dir, name, 0);
    unlock();
    return rc;
}

int ext4_rmdir(uint32_t dir, const char* name) {
    if (!g_mounted) return -1;
    lock();
    int rc = unlink_locked(dir, name, 1);
    unlock();
    return rc;
}

int ext4_rename(uint32_t odir, const char* oname, uint32_t ndir, const char* nname) {
    if (!g_mounted || !nname || !*nname || strlen(nname) > 255) return -1;
    lock();
    dir_t d;
    uint32_t ino = 0, type = 0, oino = 0, otype = 0;
    int rc = -1;
    if (dir_open(odir, &d) == 0) {
        if (dir_find(&d, oname, &ino, &type, 0, 0, 0)) rc = 0;
        dir_close(&d);
    }
    if (rc == 0 && odir == ndir && !strcmp(oname, nname)) { unlock(); return 0; }
    if (rc == 0 && dir_open(ndir, &d) == 0) {
        int exists = dir_find(&d, nname, &oino, &otype, 0, 0, 0);
        dir_close(&d);
        if (exists && oino == ino) { unlock(); return 0; }
        if (exists) rc = unlink_locked(ndir, nname, otype == EXT4_FT_DIR);    /* replaced */
    }
    if (rc == 0) rc = dir_remove(odir, oname);
    if (rc == 0) rc = dir_add(ndir, nname, ino, type);
    if (rc == 0 && type == EXT4_FT_DIR && odir != ndir) {
        /* its ".." points at the new folder; the link counts follow */
        dir_t m;
        if (dir_open(ino, &m) == 0) {
            cblk_t* c = dir_block(&m, 0);
            if (c && de_ok(c->data, 0)) {
                uint32_t second = g16(c->data + 4);
                if (second < BS && de_ok(c->data, second)) { p32(c->data + second, ndir); cdirty(c); }
            }
            cput(c);
            dir_close(&m);
        }
        links_add(odir, -1);
        links_add(ndir, 1);
    }
    if (rc == 0) {
        raw_inode_t in;
        if (iread(ino, &in) == 0) { set_times(&in, 0, 0, 1); iwrite(ino, &in); }
    }
    unlock();
    return rc;
}

long ext4_read(uint32_t ino, uint64_t off, void* buf, uint32_t len) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    long r = -1;
    if (iread(ino, &in) == 0 && emap_load(&in, &m) == 0) {
        r = do_read(&in, &m, off, (uint8_t*)buf, len, ra_window(ino, off, len));
        emap_free(&m);
    }
    unlock();
    return r;
}

int ext4_write(uint32_t ino, uint64_t off, const void* buf, uint32_t len) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    int rc = -1;
    if (iread(ino, &in) == 0 && !is_dir_i(&in) && emap_load(&in, &m) == 0) {
        int changed = 0;
        rc = do_write(&in, &m, ino, off, (const uint8_t*)buf, len, &changed, 0);
        if (changed && emap_store(&in, &m) != 0) rc = -1;
        set_times(&in, 0, 1, 1);
        iwrite(ino, &in);
        emap_free(&m);
    }
    unlock();
    return rc;
}

int ext4_truncate(uint32_t ino, uint64_t size) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    int rc = -1;
    if (iread(ino, &in) == 0 && !is_dir_i(&in) && emap_load(&in, &m) == 0) {
        int changed = 0;
        rc = do_truncate(&in, &m, size, &changed);
        if (changed && emap_store(&in, &m) != 0) rc = -1;
        set_times(&in, 0, 1, 1);
        iwrite(ino, &in);
        emap_free(&m);
    }
    unlock();
    return rc;
}

int ext4_setattr(uint32_t ino, uint32_t perms, uint32_t uid, uint32_t gid, uint32_t atime, uint32_t mtime, uint32_t ctime) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    int rc = iread(ino, &in);
    if (rc == 0) {
        if (perms != ~0u) p16(in.r + 0x00, (i_mode(&in) & EXT4_S_IFMT) | (perms & 07777u));
        if (uid != ~0u) { p16(in.r + 0x02, uid); p16(in.r + 0x78, uid >> 16); }
        if (gid != ~0u) { p16(in.r + 0x18, gid); p16(in.r + 0x7A, gid >> 16); }
        if (atime != ~0u) p32(in.r + 0x08, atime);
        if (mtime != ~0u) p32(in.r + 0x10, mtime);
        if (ctime != ~0u) p32(in.r + 0x0C, ctime);
        else set_times(&in, 0, 0, 1);
        rc = iwrite(ino, &in);
    }
    unlock();
    return rc;
}

/* (lookup inside an operation that holds the lock already) */
static uint32_t lookup_locked(uint32_t dir, const char* name) {
    dir_t d;
    uint32_t ino = 0;
    if (dir_open(dir, &d) == 0) {
        if (!dir_find(&d, name, &ino, 0, 0, 0, 0)) ino = 0;
        dir_close(&d);
    }
    return ino;
}

int ext4_fallocate(uint32_t ino, uint64_t size) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    int rc = -1;
    if (iread(ino, &in) == 0 && !is_dir_i(&in) && emap_load(&in, &m) == 0) {
        rc = 0;
        uint32_t nblk = (uint32_t)((size + BS - 1) / BS);
        for (uint32_t l = 0; l < nblk && rc == 0; ) {
            uint32_t run;
            int unw;
            if (emap_find(&m, l, &unw, &run)) { l += run; continue; }
            /* the whole hole in one run if there is one (a swap file wants it so) */
            uint32_t need = nblk - l < run ? nblk - l : run;
            uint32_t got, p = balloc_contig(need, &got);
            if (!got) { rc = -1; break; }
            for (uint32_t k = 0; k < got && rc == 0; ) {
                uint32_t n = got - k < EXT_MAX_LEN - 1u ? got - k : EXT_MAX_LEN - 1u;
                if (emap_add(&m, l + k, p + k, n, 1) != 0) rc = -1;
                k += n;
            }
            l += got;
            if (got < need) { if (i_size(&in) < (uint64_t)l * BS) set_size(&in, (uint64_t)l * BS); break; }   /* (one run: no more pieces) */
        }
        if (emap_store(&in, &m) != 0) rc = -1;
        uint64_t have = m.n ? (uint64_t)(m.e[m.n - 1].lblk + m.e[m.n - 1].len) * BS : 0;
        if (i_size(&in) < size && have >= size) set_size(&in, size);
        iwrite(ino, &in);
        emap_free(&m);
    }
    unlock();
    return rc;
}

int ext4_first_run(uint32_t ino, uint32_t* sector, uint32_t* nsectors) {
    if (!g_mounted) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    int rc = -1;
    if (iread(ino, &in) == 0 && emap_load(&in, &m) == 0) {
        if (m.n) {
            /* the first extent and the ones right after it on the disk */
            uint32_t p = m.e[0].pblk, len = m.e[0].len;
            for (uint32_t i = 1; i < m.n && m.e[i].pblk == p + len && m.e[i].lblk == m.e[0].lblk + len; i++) len += m.e[i].len;
            *sector = p * SPB;
            *nsectors = len * SPB;
            rc = 0;
        }
        emap_free(&m);
    }
    unlock();
    return rc;
}

int ext4_adopt(uint32_t ino, uint64_t size, uint32_t pblk, uint32_t nblocks) {
    if (!g_mounted || pblk + nblocks > g_nblocks) return -1;
    lock();
    raw_inode_t in;
    emap_t m;
    int rc = -1;
    if (iread(ino, &in) == 0 && emap_load(&in, &m) == 0) {
        rc = 0;
        for (uint32_t l = 0; l < nblocks && rc == 0; ) {
            uint32_t n = nblocks - l < EXT_MAX_LEN ? nblocks - l : EXT_MAX_LEN;
            rc = emap_push(&m, l, pblk + l, n, 0);
            l += n;
        }
        if (rc == 0) rc = emap_store(&in, &m);
        if (rc == 0) { set_size(&in, size); rc = iwrite(ino, &in); }
        emap_free(&m);
    }
    unlock();
    return rc;
}

/* ── to the disk ──────────────────────────────────────────────────── */

uint32_t ext4_dirty_blocks(void) { return g_ndirty + (g_sb_dirty ? 1u : 0u); }
uint32_t ext4_cache_bytes(void) { return g_ncache * BS; }

uint32_t ext4_cache_trim(uint32_t want) {
    if (g_lock) return 0;                       /* (in the middle of an operation) */
    lock();
    uint32_t got = evict(want);
    unlock();
    return got;
}

static void sort_blocks(cblk_t** a, uint32_t n) {
    /* shell sort by block number (a few thousand at most) */
    for (uint32_t gap = n / 2; gap; gap /= 2)
        for (uint32_t i = gap; i < n; i++) {
            cblk_t* t = a[i];
            uint32_t j = i;
            while (j >= gap && a[j - gap]->blk > t->blk) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
}

#define RUN_MAX 16u
static uint8_t g_run[RUN_MAX * BS];             /* runs of changed blocks, written in one command */


/* ── the journal (JBD2, ext4's own: inode 8) ─────────────────────────
 * Ordered mode, as ext4 has it by default. A flush is a transaction:
 *   1. the files' changed data, in place;
 *   2. the changed metadata blocks into the journal - descriptor blocks
 *      listing where each goes, the copies - then a commit block;
 *   3. the metadata in place; the journal marked empty again.
 * A cut before the commit block is on the disk: the transaction never
 * happened (the metadata in place is still the old one); after it: the
 * mount replays it. Everything in the journal is big-endian. */

#define JBD2_MAGIC       0xC03B3998u
#define JBD2_DESCRIPTOR  1u
#define JBD2_COMMIT      2u
#define JBD2_SB_V2       4u
#define JBD2_REVOKE      5u
#define JF_ESCAPE        1u
#define JF_SAME_UUID     2u
#define JF_DELETED       4u
#define JF_LAST_TAG      8u
#define TAGS_PER_DESC    ((BS - 12u - 16u) / 8u)   /* 508: the first tag carries the UUID */
#define COMPAT_HAS_JOURNAL   0x0004u
#define INCOMPAT_RECOVER     0x0004u
#define JOURNAL_INO          8u

static uint32_t gb32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void     pb32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t gb16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void     pb16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static emap_t   g_jmap;                     /* the journal file's blocks */
static uint32_t g_jlen, g_jfirst, g_jseq;   /* its length (blocks), first log block, the next transaction */
static int      g_journal;                  /* journaling (else: written in place, as ext2) */
static uint8_t  g_jsb[BS];                  /* its superblock */

static uint32_t jblk(uint32_t i) {          /* journal block i on the disk (0: none) */
    int unw;
    return emap_find(&g_jmap, i, &unw, 0);
}

static int jsb_write(uint32_t start, uint32_t seq) {
    pb32(g_jsb + 0x18, seq);
    pb32(g_jsb + 0x1C, start);
    uint32_t p = jblk(0);
    return p ? dev_write(p, 1, g_jsb) : -1;
}

/* journal blocks written in runs: what is put here goes out together */
static uint32_t g_jw_first, g_jw_n;
static int jw_flush(void) {
    if (!g_jw_n) return 0;
    int rc = dev_write(g_jw_first, g_jw_n, g_run);
    g_jw_n = 0;
    return rc;
}
static int jw_put(uint32_t jidx, const uint8_t* blk) {
    uint32_t p = jblk(jidx);
    if (!p) return -1;
    if (g_jw_n && (g_jw_n == RUN_MAX || p != g_jw_first + g_jw_n) && jw_flush() != 0) return -1;
    if (!g_jw_n) g_jw_first = p;
    memcpy(g_run + g_jw_n * BS, blk, BS);
    g_jw_n++;
    return 0;
}

static int write_runs(cblk_t** a, uint32_t n);

/* the superblock on the disk with needs_recovery set or not: set while a
 * transaction is in the journal and not yet in place, so that Linux (and
 * its e2fsck) replay it after a cut too */
static int sb_recover_flag(int on, uint8_t* tmp) {
    /* the superblock as the disk has it (the transaction's own copy goes in
     * with the transaction): only the flag changes */
    if (dev_read(0, 1, tmp) != 0) return -1;
    uint32_t inc = g32(tmp + 1024 + 0x60);
    p32(tmp + 1024 + 0x60, on ? inc | INCOMPAT_RECOVER : inc & ~INCOMPAT_RECOVER);
    return dev_write(0, 1, tmp);
}

/* one transaction: these metadata blocks (sorted), as above */
static int jcommit(cblk_t** a, uint32_t n) {
    uint8_t* d = (uint8_t*)kmalloc(BS), *copy = (uint8_t*)kmalloc(BS);
    if (!d || !copy) { kfree(d); kfree(copy); return -1; }
    uint32_t tid = g_jseq, j = g_jfirst;
    int rc = jsb_write(g_jfirst, tid);
    if (rc == 0) rc = sb_recover_flag(1, copy);
    for (uint32_t i = 0; i < n && rc == 0; ) {
        uint32_t k = n - i < TAGS_PER_DESC ? n - i : TAGS_PER_DESC;
        memset(d, 0, BS);
        pb32(d, JBD2_MAGIC);
        pb32(d + 4, JBD2_DESCRIPTOR);
        pb32(d + 8, tid);
        uint32_t off = 12;
        for (uint32_t t = 0; t < k; t++) {
            const uint8_t* src = a[i + t]->data;
            uint32_t flags = t ? JF_SAME_UUID : 0;
            if (gb32(src) == JBD2_MAGIC) flags |= JF_ESCAPE;         /* (it would look like a journal block) */
            if (i + t + 1 == n || t + 1 == k) flags |= JF_LAST_TAG;
            pb32(d + off, a[i + t]->blk);
            pb16(d + off + 4, 0);
            pb16(d + off + 6, flags);
            off += 8;
            if (!t) { memcpy(d + off, g_sb + 0x68, 16); off += 16; }
        }
        if (jw_put(j++, d) != 0) { rc = -1; break; }
        for (uint32_t t = 0; t < k && rc == 0; t++) {
            memcpy(copy, a[i + t]->data, BS);
            if (gb32(copy) == JBD2_MAGIC) memset(copy, 0, 4);
            rc = jw_put(j++, copy);
        }
        i += k;
    }
    /* the copies on the disk before the commit block that makes them count */
    if (rc == 0) rc = jw_flush();
    if (rc == 0) rc = g_dev.flush(g_dev.ctx);
    if (rc == 0) {
        memset(d, 0, BS);
        pb32(d, JBD2_MAGIC);
        pb32(d + 4, JBD2_COMMIT);
        pb32(d + 8, tid);
        pb32(d + 0x34, now());                                  /* h_commit_sec (low half) */
        rc = jw_put(j, d);
        if (rc == 0) rc = jw_flush();
        if (rc == 0) rc = g_dev.flush(g_dev.ctx);
    }
    /* committed: in place now, then the journal empty again */
    if (rc == 0) rc = write_runs(a, n);
    if (rc == 0) rc = g_dev.flush(g_dev.ctx);
    if (rc == 0) {
        g_jseq = tid + 1;
        rc = jsb_write(0, g_jseq);
        if (rc == 0) rc = sb_recover_flag(0, copy);
        if (rc == 0) rc = g_dev.flush(g_dev.ctx);
    }
    g_jw_n = 0;
    kfree(d);
    kfree(copy);
    return rc;
}

/* blocks a transaction may hold (the descriptors and the commit block besides) */
static uint32_t jroom(void) {
    uint32_t avail = g_jlen - g_jfirst - 1u;
    return avail - (avail + TAGS_PER_DESC) / (TAGS_PER_DESC + 1u) - 1u;
}

/* ── replaying it at mount ── */

typedef struct { uint32_t blk, seq; } jrev_t;

static uint32_t jnext(uint32_t b) { return b + 1 >= g_jlen ? g_jfirst : b + 1; }

/* the tags of a descriptor block: fn(arg, fs block, journal block, flags) */
static uint32_t jtags(const uint8_t* d, uint32_t b, void (*fn)(void*, uint32_t, uint32_t, uint32_t), void* arg) {
    uint32_t off = 12, n = 0;
    while (off + 8 <= BS) {
        uint32_t blk = gb32(d + off), flags = gb16(d + off + 6);
        off += 8;
        if (!(flags & JF_SAME_UUID)) off += 16;
        b = jnext(b);
        if (fn) fn(arg, blk, b, flags);
        n++;
        if (flags & JF_LAST_TAG) break;
    }
    return n;
}

typedef struct { jrev_t* r; uint32_t nr; uint32_t seq; uint8_t* buf; int rc; } jreplay_t;

static void replay_one(void* arg, uint32_t fsblk, uint32_t jb, uint32_t flags) {
    jreplay_t* s = (jreplay_t*)arg;
    if (s->rc || fsblk >= g_nblocks) return;
    for (uint32_t i = 0; i < s->nr; i++) if (s->r[i].blk == fsblk && s->r[i].seq >= s->seq) return;   /* revoked */
    uint32_t p = jblk(jb);
    if (!p || dev_read(p, 1, s->buf) != 0) { s->rc = -1; return; }
    if (flags & JF_ESCAPE) pb32(s->buf, JBD2_MAGIC);
    if (dev_write(fsblk, 1, s->buf) != 0) s->rc = -1;
}

static int jrecover(void) {
    uint32_t start = gb32(g_jsb + 0x1C), seq0 = gb32(g_jsb + 0x18);
    if (!start) return 0;
    uint8_t* b = (uint8_t*)kmalloc(BS), *data = (uint8_t*)kmalloc(BS);
    jrev_t* rev = 0;
    uint32_t nrev = 0, cap = 0, end_seq = seq0;
    int rc = (b && data) ? 0 : -1;
    /* passes: 0 how far the committed transactions go, 1 the revoked blocks, 2 the replay */
    for (int pass = 0; pass < 3 && rc == 0; pass++) {
        uint32_t blk = start, seq = seq0;
        for (uint32_t guard = 0; guard < g_jlen * 2u; guard++) {
            if (pass && seq >= end_seq) break;
            uint32_t p = jblk(blk);
            if (!p || dev_read(p, 1, b) != 0) break;
            if (gb32(b) != JBD2_MAGIC || gb32(b + 8) != seq) break;
            uint32_t type = gb32(b + 4);
            if (type == JBD2_DESCRIPTOR) {
                if (pass == 2) {
                    jreplay_t s = { rev, nrev, seq, data, 0 };
                    uint32_t n = jtags(b, blk, replay_one, &s);
                    if (s.rc) { rc = -1; break; }
                    for (uint32_t k = 0; k <= n; k++) blk = jnext(blk);
                } else {
                    uint32_t n = jtags(b, blk, 0, 0);
                    for (uint32_t k = 0; k <= n; k++) blk = jnext(blk);
                }
            } else if (type == JBD2_COMMIT) {
                seq++;
                blk = jnext(blk);
                if (pass == 0) end_seq = seq;
            } else if (type == JBD2_REVOKE) {
                if (pass == 1) {
                    uint32_t used = gb32(b + 12);
                    for (uint32_t off = 16; off + 4 <= used && off + 4 <= BS; off += 4) {
                        if (nrev == cap) {
                            cap = cap ? cap * 2 : 64;
                            jrev_t* r = (jrev_t*)krealloc(rev, cap * sizeof(jrev_t));
                            if (!r) { rc = -1; break; }
                            rev = r;
                        }
                        rev[nrev++] = (jrev_t){ gb32(b + off), seq };
                    }
                }
                blk = jnext(blk);
            } else break;
        }
    }
    if (rc == 0) rc = g_dev.flush(g_dev.ctx);
    if (rc == 0) {
        g_jseq = end_seq;
        rc = jsb_write(0, g_jseq);
        if (rc == 0) rc = g_dev.flush(g_dev.ctx);
        if (end_seq != seq0) klog("ext4: the journal replayed %u transaction(s)\n", end_seq - seq0);
    }
    kfree(rev);
    kfree(b);
    kfree(data);
    return rc;
}

/* the journal found at mount: its blocks, its superblock; replayed if needed */
static int journal_open(void) {
    g_journal = 0;
    emap_free(&g_jmap);
    if (!(sb32(0x5C) & COMPAT_HAS_JOURNAL)) return 0;
    if (sb32(0xE0) != JOURNAL_INO) { klog("ext4: an external journal - not used\n"); return 0; }
    raw_inode_t in;
    if (iread(JOURNAL_INO, &in) != 0 || emap_load(&in, &g_jmap) != 0) return -1;
    uint32_t p = jblk(0);
    if (!p || dev_read(p, 1, g_jsb) != 0) return -1;
    if (gb32(g_jsb) != JBD2_MAGIC || (gb32(g_jsb + 4) != JBD2_SB_V2 && gb32(g_jsb + 4) != 3u) || gb32(g_jsb + 0xC) != BS)
        return -1;
    g_jlen = gb32(g_jsb + 0x10);
    g_jfirst = gb32(g_jsb + 0x14);
    g_jseq = gb32(g_jsb + 0x18);
    if (g_jfirst < 1 || g_jlen < g_jfirst + 16u) return -1;
    if (gb32(g_jsb + 4) == JBD2_SB_V2 && (gb32(g_jsb + 0x24) || gb32(g_jsb + 0x28) || gb32(g_jsb + 0x2C))) {
        /* checksums, 64-bit tags...: not written by this driver - the journal
         * is replayed only if it holds nothing, and not used after */
        if (gb32(g_jsb + 0x1C)) { klog("ext4: the journal needs a replay this driver cannot do\n"); return -1; }
        klog("ext4: the journal has features this driver does not write - not journaling\n");
        return 0;
    }
    uint32_t before = g_jseq;
    if (jrecover() != 0) return -1;
    if (g_jseq != before) {
        /* replayed: what was read before it (the superblock, the descriptors,
         * cached blocks) is older than the disk now - read again */
        while (g_head) {
            cblk_t* c = g_head;
            if (c->pins || c->dirty) return -1;
            cdrop(c);
        }
        uint8_t* b = (uint8_t*)kmalloc(BS);
        if (!b) return -1;
        int rc = dev_read(0, 1, b);
        if (rc == 0) memcpy(g_sb, b + 1024, 1024);
        kfree(b);
        if (rc != 0 || g16(g_sb + 0x38) != 0xEF53 || dev_read(1, g_gdt_blocks, g_gdt) != 0) return -1;
    }
    if (sb32(0x60) & INCOMPAT_RECOVER) { p32(g_sb + 0x60, sb32(0x60) & ~INCOMPAT_RECOVER); g_sb_dirty = 1; }
    g_journal = 1;
    return 0;
}
/* changed blocks (sorted) to their places, in runs */
static int write_runs(cblk_t** a, uint32_t n) {
    int rc = 0;
    for (uint32_t i = 0; i < n && rc == 0; ) {
        uint32_t k = 1;
        while (i + k < n && k < RUN_MAX && a[i + k]->blk == a[i]->blk + k) k++;
        if (k == 1) rc = write_back(a[i]);
        else {
            for (uint32_t j = 0; j < k; j++) memcpy(g_run + j * BS, a[i + j]->data, BS);
            rc = dev_write(a[i]->blk, k, g_run);
            if (rc == 0) for (uint32_t j = 0; j < k; j++) if (a[i + j]->dirty) { a[i + j]->dirty = 0; g_ndirty--; }
        }
        i += k;
    }
    return rc;
}

static int flush_locked(void) {
    if (meta_out() != 0) return -1;
    if (!g_ndirty) return g_dev.flush(g_dev.ctx);
    cblk_t** a = (cblk_t**)kmalloc(g_ndirty * sizeof(cblk_t*));
    int rc = 0;
    if (!a) {
        /* (no memory for the lists: written in place, one at a time - as ext2) */
        for (cblk_t* c = g_head; c && rc == 0; c = c->next) rc = write_back(c);
        return rc == 0 ? g_dev.flush(g_dev.ctx) : rc;
    }
    /* the files' data first (ordered mode), then the metadata */
    uint32_t nd = 0, nm = 0, total = g_ndirty;
    for (cblk_t* c = g_head; c; c = c->next) if (c->dirty && !c->meta) a[nd++] = c;
    cblk_t** m = a + nd;
    for (cblk_t* c = g_head; c; c = c->next) if (c->dirty && c->meta && nd + nm < total) m[nm++] = c;
    sort_blocks(a, nd);
    sort_blocks(m, nm);
    rc = write_runs(a, nd);
    if (rc == 0 && nd) rc = g_dev.flush(g_dev.ctx);
    if (rc == 0 && nm) {
        if (g_journal && !g_hidden) {
            uint32_t room = jroom();
            for (uint32_t i = 0; i < nm && rc == 0; i += room) rc = jcommit(m + i, nm - i < room ? nm - i : room);
        } else {
            rc = write_runs(m, nm);
            if (rc == 0) rc = g_dev.flush(g_dev.ctx);
        }
    }
    kfree(a);
    return rc;
}

int ext4_flush(void) {
    if (!g_mounted) return -1;
    lock();
    int rc = flush_locked();
    unlock();
    return rc;
}

void ext4_statfs(uint64_t* blocks, uint64_t* free_blocks, uint32_t* inodes, uint32_t* free_inodes) {
    if (!g_mounted) { *blocks = *free_blocks = 0; *inodes = *free_inodes = 0; return; }
    *blocks = g_nblocks;
    *free_blocks = free_blocks_total();
    *inodes = g_groups * g_ipg;
    *free_inodes = free_inodes_total();
}

/* ── mounting ─────────────────────────────────────────────────────── */

static void unmount_state(void) {
    while (g_head) cdrop(g_head);
    kfree(g_gdt);
    kfree(g_gdt_dirty);
    g_gdt = g_gdt_dirty = 0;
    g_mounted = 0;
}

static int mount_sb(const ext4_dev_t* dev);

/* written out and let go (the live system's repair: another disk's ext4) */
int ext4_unmount(void) {
    if (!g_mounted) return 0;
    int rc = ext4_flush();
    lock();
    unmount_state();
    unlock();
    return rc;
}

int ext4_mount(const ext4_dev_t* dev) {
    if (g_mounted) unmount_state();
    g_dev = *dev;
    g_hidden = 0;
    uint8_t* b = (uint8_t*)kmalloc(BS);
    if (!b) return -1;
    if (g_dev.read(g_dev.ctx, 0, SPB, b) != 0) { kfree(b); return -1; }
    memcpy(g_sb, b + 1024, 1024);
    kfree(b);
    return mount_sb(dev);
}

/* the superblock in g_sb (read, or just made): checked, the descriptors read */
static int mount_sb(const ext4_dev_t* dev) {
    g_dev = *dev;
    if (g16(g_sb + 0x38) != 0xEF53 || sb32(0x18) != 2 || sb32(0x4C) != 1 || g16(g_sb + 0x58) != ISZ ||
        sb32(0x20) != BPG || sb32(0x14) != 0)
        return -1;
    if ((sb32(0x60) & ~(OUR_INCOMPAT | INCOMPAT_RECOVER)) || (sb32(0x64) & ~OUR_RO) || !(sb32(0x60) & INCOMPAT_EXTENTS)) {
        klog("ext4: features this driver does not have (incompat %x, ro_compat %x)\n", sb32(0x60), sb32(0x64));
        return -1;
    }
    if (sb32(0x150) || (g16(g_sb + 0xFE) && g16(g_sb + 0xFE) != DESC)) return -1;   /* (no 64bit) */
    g_nblocks = sb32(0x04);
    if (g_nblocks > dev->sectors / SPB) return -1;
    g_groups = (g_nblocks + BPG - 1) / BPG;
    g_ipg = sb32(0x28);
    if (!g_ipg || g_ipg > BS * 8 || g_ipg % (BS / ISZ)) return -1;
    g_itb = g_ipg * ISZ / BS;
    g_gdt_blocks = (g_groups * DESC + BS - 1) / BS;
    g_gdt = (uint8_t*)kmalloc(g_gdt_blocks * BS);
    g_gdt_dirty = (uint8_t*)kzalloc(g_gdt_blocks);
    if (!g_gdt || !g_gdt_dirty || dev_read(1, g_gdt_blocks, g_gdt) != 0) { unmount_state(); return -1; }
    memcpy(g_label, g_sb + 0x78, 16);
    g_label[16] = 0;
    g_sb_dirty = 0;
    g_ncache = g_ndirty = 0;
    memset(g_hash, 0, sizeof(g_hash));
    g_head = g_tail = 0;
    g_mounted = 1;
    if (journal_open() != 0) { klog("ext4: the journal could not be read or replayed\n"); unmount_state(); return -1; }
    /* mounted: counted and dated (as Linux does) */
    p16(g_sb + 0x34, g16(g_sb + 0x34) + 1);                  /* s_mnt_count */
    p32(g_sb + 0x2C, now());                                  /* s_mtime */
    g_sb_dirty = 1;
    return 0;
}

/* ── making one ───────────────────────────────────────────────────── */

uint32_t ext4_fixed_blocks(uint32_t sectors) {
    uint32_t nb = sectors / SPB, groups = (nb + BPG - 1) / BPG;
    return 1u + (groups * DESC + BS - 1) / BS;
}

/* runs of blocks in use while making the filesystem, sorted */
typedef struct { uint32_t a, n; } run_t;
static run_t*   g_runs;
static uint32_t g_nruns, g_runcap;

static int runs_add(uint32_t a, uint32_t n) {
    if (!n) return 0;
    if (g_nruns == g_runcap) {
        uint32_t cap = g_runcap ? g_runcap * 2 : 256;
        run_t* r = (run_t*)krealloc(g_runs, cap * sizeof(run_t));
        if (!r) return -1;
        g_runs = r;
        g_runcap = cap;
    }
    uint32_t i = g_nruns;
    while (i > 0 && g_runs[i - 1].a > a) { g_runs[i] = g_runs[i - 1]; i--; }
    g_runs[i] = (run_t){ a, n };
    g_nruns++;
    return 0;
}

/* n free blocks in a row (not in any run), from `from` on: the first, 0 if none */
static uint32_t runs_take(uint32_t from, uint32_t n, uint32_t limit) {
    uint32_t cur = from;
    for (uint32_t i = 0; i <= g_nruns; i++) {
        uint32_t next = i < g_nruns ? g_runs[i].a : limit;
        if (next > cur && next - cur >= n) { if (runs_add(cur, n)) return 0; return cur; }
        if (i < g_nruns && g_runs[i].a + g_runs[i].n > cur) cur = g_runs[i].a + g_runs[i].n;
    }
    return 0;
}

int ext4_mkfs(const ext4_dev_t* dev, const char* label, const uint32_t (*keep)[2], int nkeep) {
    if (g_mounted) unmount_state();
    uint32_t nb = dev->sectors / SPB;
    uint32_t groups = (nb + BPG - 1) / BPG;
    if (groups > 1 && nb - (groups - 1) * BPG < 512u) { groups--; nb = groups * BPG; }   /* (a tiny last group: left out) */
    if (nb < 2048u) return -1;
    uint32_t ipg = 4096u;
    if (groups == 1 && nb / 8u < ipg) ipg = (nb / 8u) & ~15u;
    uint32_t itb = ipg * ISZ / BS, gdtb = (groups * DESC + BS - 1) / BS, fixed = 1 + gdtb;
    g_nruns = 0;
    int rc = -1;
    uint32_t* bb = (uint32_t*)kmalloc(groups * 3u * sizeof(uint32_t));
    uint8_t* gdt = (uint8_t*)kzalloc(gdtb * BS);
    uint8_t* blk = (uint8_t*)kmalloc(BS);
    if (!bb || !gdt || !blk || runs_add(0, fixed)) goto out;
    for (int i = 0; i < nkeep; i++) {
        if (keep[i][0] < fixed || keep[i][0] + keep[i][1] > nb) goto out;
        if (runs_add(keep[i][0], keep[i][1])) goto out;
    }
    uint32_t* ib = bb + groups, *it = bb + 2 * groups;
    /* flex_bg: the block bitmaps together, the inode bitmaps, the inode tables */
    uint32_t a = runs_take(fixed, groups, nb);
    for (uint32_t g = 0; g < groups; g++) bb[g] = a ? a + g : runs_take(fixed, 1, nb);
    a = runs_take(fixed, groups, nb);
    for (uint32_t g = 0; g < groups; g++) ib[g] = a ? a + g : runs_take(fixed, 1, nb);
    a = runs_take(fixed, groups * itb, nb);
    for (uint32_t g = 0; g < groups; g++) it[g] = a ? a + g * itb : runs_take(fixed, itb, nb);
    uint32_t rootb = runs_take(fixed, 1, nb), lpfb = runs_take(fixed, 1, nb);
    /* the journal: 32 MiB (smaller on a small disk), in one run */
    uint32_t jlen = nb >= 262144u ? 8192u : (nb / 32u < 1024u ? 1024u : nb / 32u);
    uint32_t jstart = runs_take(fixed, jlen, nb);
    if (!jstart) goto out;
    for (uint32_t g = 0; g < groups; g++) if (!bb[g] || !ib[g] || !it[g]) goto out;
    if (!rootb || !lpfb) goto out;

    /* the block bitmaps, from the runs (a sweep: both are in order) */
    uint32_t r0 = 0;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t lo = g * BPG, n = g + 1 < groups ? BPG : nb - lo, hi = lo + n, used = 0;
        memset(blk, 0, BS);
        while (r0 < g_nruns && g_runs[r0].a + g_runs[r0].n <= lo) r0++;
        for (uint32_t r = r0; r < g_nruns && g_runs[r].a < hi; r++) {
            uint32_t s = g_runs[r].a > lo ? g_runs[r].a : lo, e = g_runs[r].a + g_runs[r].n < hi ? g_runs[r].a + g_runs[r].n : hi;
            for (uint32_t j = s - lo; j < e - lo; j++) if (!(blk[j >> 3] & (1u << (j & 7)))) { blk[j >> 3] |= (uint8_t)(1u << (j & 7)); used++; }
        }
        for (uint32_t j = n; j < BS * 8; j++) blk[j >> 3] |= (uint8_t)(1u << (j & 7));   /* (past the end) */
        if (dev->write(dev->ctx, bb[g] * SPB, SPB, blk) != 0) goto out;
        uint8_t* d = gdt + g * DESC;
        p32(d + 0x00, bb[g]);
        p32(d + 0x04, ib[g]);
        p32(d + 0x08, it[g]);
        p16(d + 0x0C, n - used);
        p16(d + 0x0E, g ? ipg : ipg - 11u);
        p16(d + 0x10, g ? 0 : 2);
        p16(d + 0x12, g ? BG_INODE_UNINIT : 0);
        p16(d + 0x1C, g ? ipg : ipg - 11u);                  /* bg_itable_unused */
    }
    /* group 0's inodes: 1-10 reserved, 2 the root, 11 lost+found */
    memset(blk, 0, BS);
    for (uint32_t i = 0; i < 11; i++) blk[i >> 3] |= (uint8_t)(1u << (i & 7));
    for (uint32_t j = ipg; j < BS * 8; j++) blk[j >> 3] |= (uint8_t)(1u << (j & 7));
    if (dev->write(dev->ctx, ib[0] * SPB, SPB, blk) != 0) goto out;

    /* the superblock */
    memset(g_sb, 0, sizeof(g_sb));
    uint32_t lo32, hi32;
    __asm__ volatile("rdtsc" : "=a"(lo32), "=d"(hi32));
    uint32_t t = now(), seed = lo32 ^ (hi32 * 2654435761u) ^ t;
    p32(g_sb + 0x00, groups * ipg);
    p32(g_sb + 0x04, nb);
    p32(g_sb + 0x14, 0);
    p32(g_sb + 0x18, 2);
    p32(g_sb + 0x1C, 2);
    p32(g_sb + 0x20, BPG);
    p32(g_sb + 0x24, BPG);
    p32(g_sb + 0x28, ipg);
    p32(g_sb + 0x30, t);
    p16(g_sb + 0x36, 0xFFFF);                                 /* s_max_mnt_count: -1 */
    p16(g_sb + 0x38, 0xEF53);
    p16(g_sb + 0x3A, 1);                                      /* clean */
    p16(g_sb + 0x3C, 1);                                      /* errors: continue */
    p32(g_sb + 0x40, t);
    p32(g_sb + 0x4C, 1);                                      /* dynamic revision */
    p32(g_sb + 0x54, 11);
    p16(g_sb + 0x58, ISZ);
    p32(g_sb + 0x5C, COMPAT_SPARSE_SUPER2);
    p32(g_sb + 0x60, OUR_INCOMPAT);
    p32(g_sb + 0x64, OUR_RO);
    for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u; g_sb[0x68 + i] = (uint8_t)(seed >> 16); }
    g_sb[0x68 + 6] = (uint8_t)((g_sb[0x68 + 6] & 0x0F) | 0x40);  /* (a version-4 UUID) */
    g_sb[0x68 + 8] = (uint8_t)((g_sb[0x68 + 8] & 0x3F) | 0x80);
    memset(g_sb + 0x78, 0, 16);
    if (label) memcpy(g_sb + 0x78, label, strlen(label) < 16 ? strlen(label) : 16);
    for (int i = 0; i < 4; i++) { seed = seed * 1103515245u + 12345u; p32(g_sb + 0xEC + i * 4, seed); }
    g_sb[0xFC] = 1;                                           /* half_md4 */
    p32(g_sb + 0x108, t);                                     /* s_mkfs_time */
    p16(g_sb + 0x15C, 32);
    p16(g_sb + 0x15E, 32);
    p32(g_sb + 0x160, 1);                                     /* signed directory hash */
    g_sb[0x174] = 4;                                          /* 16 groups to a flex group */
    p32(g_sb + 0x24C, 0);                                     /* sparse_super2: no backups */
    p32(g_sb + 0x250, 0);
    /* the descriptors' checksums (the UUID is in them) */
    for (uint32_t g = 0; g < groups; g++) {
        uint8_t* d = gdt + g * DESC;
        uint8_t le[4];
        p32(le, g);
        uint16_t c = crc16(0xFFFFu, g_sb + 0x68, 16);
        c = crc16(c, le, 4);
        c = crc16(c, d, 0x1E);
        p16(d + 0x1E, c);
    }
    if (dev->write(dev->ctx, 1 * SPB, gdtb * SPB, gdt) != 0) goto out;

    /* the inode table's first block: the root and lost+found */
    memset(blk, 0, BS);
    raw_inode_t in;
    init_inode(&in, EXT4_S_IFDIR | 0755u, 0, 0, 3);
    set_size(&in, BS);
    put_hdr(in.r + 0x28, 1, 4, 0);
    { ext_t x = { 0, rootb, 1, 0 }; put_leaf(in.r + 0x28 + 12, &x); }
    set_blocks(&in, SPB);
    memcpy(blk + 1 * ISZ, in.r, ISZ);                         /* inode 2 */
    init_inode(&in, EXT4_S_IFDIR | 0700u, 0, 0, 2);
    set_size(&in, BS);
    put_hdr(in.r + 0x28, 1, 4, 0);
    { ext_t x = { 0, lpfb, 1, 0 }; put_leaf(in.r + 0x28 + 12, &x); }
    set_blocks(&in, SPB);
    memcpy(blk + 10 * ISZ, in.r, ISZ);                        /* inode 11 */
    /* inode 8: the journal, one extent; the superblock keeps a copy of its
     * block map (s_jnl_blocks), as mke2fs does */
    init_inode(&in, EXT4_S_IFREG | 0600u, 0, 0, 1);
    set_size(&in, (uint64_t)jlen * BS);
    put_hdr(in.r + 0x28, 1, 4, 0);
    { ext_t x = { 0, jstart, jlen, 0 }; put_leaf(in.r + 0x28 + 12, &x); }
    set_blocks(&in, (uint64_t)jlen * SPB);
    memcpy(blk + 7 * ISZ, in.r, ISZ);
    p32(g_sb + 0x5C, sb32(0x5C) | COMPAT_HAS_JOURNAL);
    p32(g_sb + 0xE0, JOURNAL_INO);
    g_sb[0xFD] = 1;                                           /* s_jnl_backup_type: the blocks */
    memcpy(g_sb + 0x10C, in.r + 0x28, 60);
    p32(g_sb + 0x10C + 60, 0);
    p32(g_sb + 0x10C + 64, jlen * BS);
    if (dev->write(dev->ctx, it[0] * SPB, SPB, blk) != 0) goto out;
    {
        uint8_t* j = (uint8_t*)kzalloc(BS);
        if (!j) goto out;
        pb32(j + 0x00, JBD2_MAGIC);
        pb32(j + 0x04, JBD2_SB_V2);
        pb32(j + 0x0C, BS);
        pb32(j + 0x10, jlen);
        pb32(j + 0x14, 1);                                    /* s_first */
        pb32(j + 0x18, 1);                                    /* s_sequence */
        memcpy(j + 0x30, g_sb + 0x68, 16);                    /* s_uuid */
        pb32(j + 0x40, 1);                                    /* s_nr_users */
        int wr = dev->write(dev->ctx, jstart * SPB, SPB, j);
        kfree(j);
        if (wr != 0) goto out;
    }
    /* (the rest of the table: never used yet - bg_itable_unused - so not zeroed) */
    memset(blk, 0, BS);
    put_de(blk, EXT4_ROOT_INO, 12, ".", 1, EXT4_FT_DIR);
    put_de(blk + 12, EXT4_ROOT_INO, 12, "..", 2, EXT4_FT_DIR);
    put_de(blk + 24, EXT4_LPF_INO, BS - 24, "lost+found", 10, EXT4_FT_DIR);
    if (dev->write(dev->ctx, rootb * SPB, SPB, blk) != 0) goto out;
    memset(blk, 0, BS);
    put_de(blk, EXT4_LPF_INO, 12, ".", 1, EXT4_FT_DIR);
    put_de(blk + 12, EXT4_ROOT_INO, BS - 12, "..", 2, EXT4_FT_DIR);
    if (dev->write(dev->ctx, lpfb * SPB, SPB, blk) != 0) goto out;

    /* the free counts, then the superblock last (block 0: its first 1 KiB kept) */
    {
        uint64_t fb = 0;
        uint32_t fi = 0;
        for (uint32_t g = 0; g < groups; g++) { fb += g16(gdt + g * DESC + 0x0C); fi += g16(gdt + g * DESC + 0x0E); }
        p32(g_sb + 0x0C, (uint32_t)fb);
        p32(g_sb + 0x10, fi);
    }
    if (dev->read(dev->ctx, 0, SPB, blk) != 0) goto out;
    memcpy(blk + 1024, g_sb, 1024);
    if (g_hidden) p16(blk + 1024 + 0x38, 0);
    memset(blk + 2048, 0, BS - 2048);
    if (dev->write(dev->ctx, 0, SPB, blk) != 0 || dev->flush(dev->ctx) != 0) goto out;
    g_dev = *dev;
    rc = mount_sb(dev);
out:
    kfree(bb);
    kfree(gdt);
    kfree(blk);
    kfree(g_runs);
    g_runs = 0;
    g_nruns = g_runcap = 0;
    return rc;
}

/* ── checking ─────────────────────────────────────────────────────── */

int ext4_check(void (*out)(const char* line)) {
    if (!g_mounted) { out("ext4: not mounted"); return 1; }
    char line[160];
    int problems = 0;
    lock();
    flush_locked();
    for (uint32_t g = 0; g < g_groups; g++) {
        cblk_t* c = cmeta(bb_of(g), 0);
        if (!c) { problems++; continue; }
        uint32_t n = gblocks(g), used = 0;
        for (uint32_t j = 0; j < n; j++) if (c->data[j >> 3] & (1u << (j & 7))) used++;
        cput(c);
        if (n - used != g16(desc(g) + 0x0C)) {
            ksnprintf(line, sizeof(line), "group %u: %u blocks free in its bitmap, %u in its descriptor", g, n - used, g16(desc(g) + 0x0C));
            out(line);
            problems++;
        }
        if (g16(desc(g) + 0x12) & BG_INODE_UNINIT) continue;
        c = cmeta(ib_of(g), 0);
        if (!c) { problems++; continue; }
        used = 0;
        for (uint32_t j = 0; j < g_ipg; j++) if (c->data[j >> 3] & (1u << (j & 7))) used++;
        cput(c);
        if (g_ipg - used != g16(desc(g) + 0x0E)) {
            ksnprintf(line, sizeof(line), "group %u: %u inodes free in its bitmap, %u in its descriptor", g, g_ipg - used, g16(desc(g) + 0x0E));
            out(line);
            problems++;
        }
    }
    uint64_t fb = free_blocks_total();
    ksnprintf(line, sizeof(line), "ext4 '%s': %u groups, %u of %u blocks free, %u of %u inodes free, %u blocks cached",
              g_label, g_groups, (uint32_t)fb, g_nblocks, free_inodes_total(), g_groups * g_ipg, g_ncache);
    out(line);
    unlock();
    return problems;
}

void ext4_set_hidden(int on) { g_hidden = on; }

int ext4_activate(void) {
    if (!g_mounted) return -1;
    lock();
    g_hidden = 0;
    g_sb_dirty = 1;
    int rc = flush_locked();
    unlock();
    return rc;
}

void ext4_free_run(uint32_t blk, uint32_t n) {
    if (!g_mounted || !n || blk + n > g_nblocks) return;
    lock();
    bfree(blk, n);
    unlock();
}
