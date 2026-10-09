#include "fat32.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "rtc.h"
#include "serial.h"
#include "task.h"

/*
 * FAT32, read and write, with long file names (VFAT).
 *
 * The directory tree is read once at mount time and mirrored into the
 * filesystem tree (fs_mount_add_dir/file); every node (file or folder)
 * remembers where its 8.3 directory entry lives - the byte offset in its
 * parent folder's cluster chain - so writes can update it in place.
 * File data is read on first use and written through on every change.
 * The FAT itself is cached a few sectors at a time and flushed (to every
 * FAT copy) at the end of each operation.
 */

#define SEC        512u
#define FAT_EOC    0x0FFFFFF8u
#define FAT_MASK   0x0FFFFFFFu
#define FATC       8               /* cached FAT sectors */
#define MAX_DEPTH  10

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS    0x04
#define ATTR_VOLID  0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

typedef struct {
    int      used, is_dir;
    uint32_t parent;               /* node of the folder holding the entry */
    uint32_t first;                /* first cluster, 0 = empty file */
    uint32_t size;
    uint32_t pos;                  /* byte offset of the 8.3 entry in the parent's chain */
    uint8_t  nlfn;                 /* long-name entries right before it */
} fnode_t;

typedef struct {
    blockdev_t* bd;
    uint32_t base;                 /* first sector of the volume */
    uint32_t spc, clus_bytes;
    uint32_t fat_lba, fat_sectors, nfats;
    uint32_t data_lba;             /* cluster 2 */
    uint32_t nclusters;            /* data clusters: valid numbers are 2 .. nclusters+1 */
    uint32_t root;
    uint32_t fsinfo;               /* absolute sector of FSInfo, 0 = none */
    int      fsinfo_done;
    uint32_t hint;                 /* where the next free-cluster search starts */
    uint32_t nfree;                /* free clusters, once known (FSInfo, or counted for df) */
    int      nfree_ok;
    char     label[12];
    uint32_t cache_idx[FATC];
    int      cache_ok[FATC], cache_dirty[FATC];
    uint8_t  cache[FATC][SEC];
    uint8_t  sec[SEC];             /* one-sector scratch */
    uint8_t* cbuf;                 /* one-cluster scratch */
    fnode_t* nodes;
    uint32_t nnodes, cap;
    int      mnt;
    int      skipped;              /* entries that did not fit the tree */
    int      io_err;
} fvol_t;

static fvol_t* g_vols[FS_MAX_MOUNTS + 1];

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ── sectors and clusters ──────────────────────────────────────────── */

static int dev_read(fvol_t* v, uint32_t lba, uint32_t n, void* buf) {
    if (!v->bd->present || v->bd->read(v->bd, lba, n, buf) != 0) { v->io_err = 1; return -1; }
    return 0;
}

static int dev_write(fvol_t* v, uint32_t lba, uint32_t n, const void* buf) {
    if (!v->bd->present || v->bd->write(v->bd, lba, n, buf) != 0) { v->io_err = 1; return -1; }
    return 0;
}

static uint32_t clus_lba(fvol_t* v, uint32_t c) {
    return v->data_lba + (c - 2) * v->spc;
}

static int valid_clus(fvol_t* v, uint32_t c) {
    return c >= 2 && c <= v->nclusters + 1;
}

/* ── the FAT ───────────────────────────────────────────────────────── */

static int cache_flush_slot(fvol_t* v, int s) {
    if (!v->cache_ok[s] || !v->cache_dirty[s]) return 0;
    for (uint32_t k = 0; k < v->nfats; k++)
        if (dev_write(v, v->fat_lba + k * v->fat_sectors + v->cache_idx[s], 1, v->cache[s]) != 0) return -1;
    v->cache_dirty[s] = 0;
    return 0;
}

/* the cached copy of FAT sector idx, NULL on a read error */
static uint8_t* fat_sector(fvol_t* v, uint32_t idx, int* slot) {
    int s = (int)(idx % FATC);
    if (!v->cache_ok[s] || v->cache_idx[s] != idx) {
        if (cache_flush_slot(v, s) != 0) return NULL;
        v->cache_ok[s] = 0;
        if (dev_read(v, v->fat_lba + idx, 1, v->cache[s]) != 0) return NULL;
        v->cache_ok[s] = 1;
        v->cache_idx[s] = idx;
        v->cache_dirty[s] = 0;
    }
    if (slot) *slot = s;
    return v->cache[s];
}

static uint32_t fat_get(fvol_t* v, uint32_t c) {
    uint8_t* s = fat_sector(v, c * 4 / SEC, NULL);
    if (!s) return FAT_MASK;            /* reads as end of chain */
    return rd32(s + (c * 4) % SEC) & FAT_MASK;
}

static int fat_set(fvol_t* v, uint32_t c, uint32_t val) {
    int slot;
    uint8_t* s = fat_sector(v, c * 4 / SEC, &slot);
    if (!s) return -1;
    uint8_t* p = s + (c * 4) % SEC;
    uint32_t old = rd32(p) & FAT_MASK;
    wr32(p, (rd32(p) & ~FAT_MASK) | (val & FAT_MASK));
    v->cache_dirty[slot] = 1;
    if (v->nfree_ok) {                   /* the free count follows (df) */
        if (!old && (val & FAT_MASK) && v->nfree) v->nfree--;
        else if (old && !(val & FAT_MASK)) v->nfree++;
    }
    return 0;
}

/* the FSInfo free count is no longer known once we change the FAT:
 * mark it "unknown" (0xFFFFFFFF) the first time, as the spec allows */
static void fsinfo_invalidate(fvol_t* v) {
    if (v->fsinfo_done || !v->fsinfo) return;
    v->fsinfo_done = 1;
    if (dev_read(v, v->fsinfo, 1, v->sec) != 0) return;
    if (rd32(v->sec) != 0x41615252u || rd32(v->sec + 484) != 0x61417272u) return;
    wr32(v->sec + 488, 0xFFFFFFFFu);
    wr32(v->sec + 492, v->hint);
    dev_write(v, v->fsinfo, 1, v->sec);
}

static int fat_flush(fvol_t* v) {
    int rc = 0;
    for (int s = 0; s < FATC; s++) if (cache_flush_slot(v, s) != 0) rc = -1;
    return rc;
}

static uint32_t next_clus(fvol_t* v, uint32_t c) {
    uint32_t n = fat_get(v, c);
    return valid_clus(v, n) ? n : 0;    /* 0: end of chain (or a broken one) */
}

static void free_chain(fvol_t* v, uint32_t c) {
    for (uint32_t guard = 0; valid_clus(v, c) && guard <= v->nclusters; guard++) {
        uint32_t n = fat_get(v, c);
        fat_set(v, c, 0);
        if (c < v->hint) v->hint = c;
        c = valid_clus(v, n) ? n : 0;
    }
}

/* a free cluster, marked end-of-chain and linked after prev (if any) */
static uint32_t alloc_clus(fvol_t* v, uint32_t prev) {
    fsinfo_invalidate(v);
    uint32_t start = valid_clus(v, v->hint) ? v->hint : 2;
    uint32_t c = start;
    for (uint32_t n = 0; n < v->nclusters; n++) {
        uint32_t val = fat_get(v, c);
        if (v->io_err) return 0;
        if (val == 0) {
            if (fat_set(v, c, FAT_MASK) != 0) return 0;
            if (prev && fat_set(v, prev, c) != 0) return 0;
            v->hint = c + 1;
            return c;
        }
        c = (c >= v->nclusters + 1) ? 2 : c + 1;
    }
    return 0;                            /* the stick is full */
}

/* n clusters chained after prev (0: a new chain); *first gets the first */
static int alloc_chain(fvol_t* v, uint32_t n, uint32_t prev, uint32_t* first) {
    uint32_t p = prev, f = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = alloc_clus(v, p);
        if (!c) {
            if (f) free_chain(v, f);
            if (prev) fat_set(v, prev, FAT_MASK);
            return -1;
        }
        if (!f) f = c;
        p = c;
    }
    *first = f;
    return 0;
}

static uint32_t chain_last(fvol_t* v, uint32_t c, uint32_t* count) {
    uint32_t n = 1;
    for (uint32_t guard = 0; guard <= v->nclusters; guard++) {
        uint32_t nx = next_clus(v, c);
        if (!nx) break;
        c = nx;
        n++;
    }
    if (count) *count = n;
    return c;
}

/* ── file data ─────────────────────────────────────────────────────── */

/* reads size bytes of the chain starting at c into out (runs of
 * consecutive clusters are read with one command) */
static int read_chain(fvol_t* v, uint32_t c, uint8_t* out, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        if (!valid_clus(v, c)) return -1;
        /* a run of consecutive clusters that are all wholly wanted */
        uint32_t run = 1, last = c;
        while ((run + 1) * v->clus_bytes <= size - done) {
            uint32_t nx = next_clus(v, last);
            if (nx != last + 1) break;
            last = nx;
            run++;
        }
        uint32_t left = size - done;
        if (run * v->clus_bytes <= left) {
            if (dev_read(v, clus_lba(v, c), run * v->spc, out + done) != 0) return -1;
            done += run * v->clus_bytes;
        } else {                         /* the tail of the last cluster */
            if (dev_read(v, clus_lba(v, c), v->spc, v->cbuf) != 0) return -1;
            memcpy(out + done, v->cbuf, left);
            done = size;
        }
        if (done < size) c = next_clus(v, last);
    }
    return 0;
}

/* writes len bytes over the chain starting at c (already long enough) */
static int write_chain(fvol_t* v, uint32_t c, const uint8_t* data, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        if (!valid_clus(v, c)) return -1;
        uint32_t run = 1, last = c;
        while ((run + 1) * v->clus_bytes <= len - done) {
            uint32_t nx = next_clus(v, last);
            if (nx != last + 1) break;
            last = nx;
            run++;
        }
        uint32_t left = len - done;
        if (run * v->clus_bytes <= left) {
            if (dev_write(v, clus_lba(v, c), run * v->spc, data + done) != 0) return -1;
            done += run * v->clus_bytes;
        } else {
            memset(v->cbuf, 0, v->clus_bytes);
            memcpy(v->cbuf, data + done, left);
            if (dev_write(v, clus_lba(v, c), v->spc, v->cbuf) != 0) return -1;
            done = len;
        }
        if (done < len) c = next_clus(v, last);
    }
    return 0;
}

/* ── directory entries ─────────────────────────────────────────────── */

static uint32_t node_first(fvol_t* v, uint32_t node) {
    return node == 0 ? v->root : v->nodes[node].first;
}

/* the 32-byte entry at byte offset pos of the folder chain starting at
 * dirc: read into ent, or (write) replaced by ent */
static int dir_entry(fvol_t* v, uint32_t dirc, uint32_t pos, uint8_t* ent, int write) {
    uint32_t c = dirc;
    for (uint32_t i = pos / v->clus_bytes; i > 0; i--) {
        c = next_clus(v, c);
        if (!c) return -1;
    }
    if (!valid_clus(v, c)) return -1;
    uint32_t off = pos % v->clus_bytes;
    uint32_t lba = clus_lba(v, c) + off / SEC;
    if (dev_read(v, lba, 1, v->sec) != 0) return -1;
    if (!write) { memcpy(ent, v->sec + off % SEC, 32); return 0; }
    memcpy(v->sec + off % SEC, ent, 32);
    return dev_write(v, lba, 1, v->sec);
}

/* a whole folder (kmalloc'd), *len bytes */
static uint8_t* dir_load(fvol_t* v, uint32_t dirc, uint32_t* len) {
    uint32_t n;
    chain_last(v, dirc, &n);
    if (n > 65536 / (v->clus_bytes / 32) + 1) n = 65536 / (v->clus_bytes / 32) + 1;   /* 65536 entries max */
    uint8_t* buf = (uint8_t*)kmalloc(n * v->clus_bytes);
    if (!buf) return NULL;
    if (read_chain(v, dirc, buf, n * v->clus_bytes) != 0) { kfree(buf); return NULL; }
    *len = n * v->clus_bytes;
    return buf;
}

static void now_stamp(uint16_t* time, uint16_t* date) {
    rtc_datetime_t dt;
    if (rtc_read_datetime(&dt) != 0 || dt.year < 1980) { *time = 0; *date = (uint16_t)((46 << 9) | (1 << 5) | 1); return; }
    *time = (uint16_t)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
    *date = (uint16_t)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
}

/* rewrites a node's entry: first cluster, size, modification time */
static int update_entry(fvol_t* v, uint32_t node) {
    fnode_t* n = &v->nodes[node];
    uint8_t e[32];
    uint32_t dirc = node_first(v, n->parent);
    if (dir_entry(v, dirc, n->pos, e, 0) != 0) return -1;
    wr16(e + 20, (uint16_t)(n->first >> 16));
    wr16(e + 26, (uint16_t)n->first);
    wr32(e + 28, n->is_dir ? 0 : n->size);
    uint16_t t, d;
    now_stamp(&t, &d);
    wr16(e + 22, t);
    wr16(e + 24, d);
    wr16(e + 18, d);                      /* last access */
    if (!n->is_dir) e[11] |= ATTR_ARCH;
    return dir_entry(v, dirc, n->pos, e, 1);
}

static uint8_t lfn_sum(const uint8_t* sfn) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
}

static int sfn_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c && strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

/* "README.TXT" style names need no long-name entries */
static int is_plain_83(const char* name, uint8_t* sfn) {
    int len = (int)strlen(name);
    if (len == 0 || len > 12) return 0;
    const char* dot = strchr(name, '.');
    if (dot && strchr(dot + 1, '.')) return 0;
    int bl = dot ? (int)(dot - name) : len;
    int el = dot ? len - bl - 1 : 0;
    if (bl < 1 || bl > 8 || el > 3 || (dot && el == 0)) return 0;
    memset(sfn, ' ', 11);
    for (int i = 0; i < bl; i++) { if (!sfn_char_ok(name[i])) return 0; sfn[i] = (uint8_t)name[i]; }
    for (int i = 0; i < el; i++) { if (!sfn_char_ok(dot[1 + i])) return 0; sfn[8 + i] = (uint8_t)dot[1 + i]; }
    return 1;
}

static int sfn_taken(const uint8_t* dir, uint32_t len, const uint8_t* sfn) {
    for (uint32_t p = 0; p + 32 <= len; p += 32) {
        const uint8_t* e = dir + p;
        if (e[0] == 0) break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN) continue;
        if (memcmp(e, sfn, 11) == 0) return 1;
    }
    return 0;
}

/* an 8.3 alias "BASE~N.EXT" for a long name, unique in the folder */
static void make_alias(const char* name, const uint8_t* dir, uint32_t len, uint8_t* sfn) {
    char base[9], ext[4];
    int bl = 0, el = 0;
    const char* dot = strrchr(name, '.');
    if (dot == name) dot = NULL;          /* ".profile": no extension */
    for (const char* s = name; *s && (!dot || s < dot) && bl < 8; s++) {
        char c = *s;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        base[bl++] = sfn_char_ok(c) ? c : '_';
    }
    if (dot) for (const char* s = dot + 1; *s && el < 3; s++) {
        char c = *s;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        ext[el++] = sfn_char_ok(c) ? c : '_';
    }
    if (bl == 0) base[bl++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = ksnprintf(tail, sizeof(tail), "~%u", n);
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(sfn, ' ', 11);
        memcpy(sfn, base, (size_t)keep);
        memcpy(sfn + keep, tail, (size_t)tl);
        memcpy(sfn + 8, ext, (size_t)el);
        if (!sfn_taken(dir, len, sfn)) return;
    }
}

/* Adds the entries for name to folder node parent: long-name entries (if
 * needed) and an 8.3 entry made from tmpl (attributes, times, cluster,
 * size) with its name replaced. *pos / *nlfn tell where it went. */
static int add_entries(fvol_t* v, uint32_t parent, const char* name, const uint8_t* tmpl,
                       uint32_t* pos, uint8_t* nlfn) {
    uint32_t dirc = node_first(v, parent);
    uint32_t len;
    uint8_t* dir = dir_load(v, dirc, &len);
    if (!dir) return -1;

    uint8_t sfn[11];
    int nl = 0;
    int nlen = (int)strlen(name);
    if (!is_plain_83(name, sfn)) {
        make_alias(name, dir, len, sfn);
        nl = (nlen + 12) / 13;
    } else if (sfn_taken(dir, len, sfn)) {
        kfree(dir);
        return -1;                        /* the name exists already */
    }
    uint32_t need = (uint32_t)nl + 1;

    /* a run of free slots (deleted ones, or everything after the end mark) */
    uint32_t slot = 0xFFFFFFFFu, run = 0;
    for (uint32_t p = 0; p + 32 <= len; p += 32) {
        uint8_t b = dir[p];
        if (b == 0x00) {                  /* end of folder: the rest is free */
            if (run == 0) slot = p / 32;
            run += (len - p) / 32;
            break;
        }
        if (b == 0xE5) { if (run == 0) slot = p / 32; run++; }
        else run = 0;
        if (run >= need) break;
    }
    uint32_t end_slots = len / 32;
    if (run < need) {
        /* grow the folder by a zeroed cluster */
        if (run == 0) slot = end_slots;
        uint32_t last = chain_last(v, dirc, NULL), c;
        uint32_t extra = (need - run + v->clus_bytes / 32 - 1) / (v->clus_bytes / 32);
        for (uint32_t i = 0; i < extra; i++) {
            c = alloc_clus(v, last);
            if (!c) { kfree(dir); return -1; }
            memset(v->cbuf, 0, v->clus_bytes);
            if (dev_write(v, clus_lba(v, c), v->spc, v->cbuf) != 0) { kfree(dir); return -1; }
            last = c;
        }
    }
    kfree(dir);

    uint8_t e[32];
    uint8_t sum = lfn_sum(sfn);
    for (int k = nl; k >= 1; k--) {       /* on disk: the last part first */
        memset(e, 0, 32);
        e[0] = (uint8_t)(k | (k == nl ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int i = 0; i < 13; i++) {
            int ci = (k - 1) * 13 + i;
            uint16_t ch = ci < nlen ? (uint8_t)name[ci] : ci == nlen ? 0x0000 : 0xFFFF;
            wr16(e + offs[i], ch);
        }
        if (dir_entry(v, dirc, (slot + (uint32_t)(nl - k)) * 32, e, 1) != 0) return -1;
    }
    memcpy(e, tmpl, 32);
    memcpy(e, sfn, 11);
    e[12] = 0;                            /* no case flags: the long name has the case */
    *pos = (slot + (uint32_t)nl) * 32;
    *nlfn = (uint8_t)nl;
    return dir_entry(v, dirc, *pos, e, 1);
}

static void mark_deleted(fvol_t* v, uint32_t node) {
    fnode_t* n = &v->nodes[node];
    uint32_t dirc = node_first(v, n->parent);
    uint8_t e[32];
    for (uint32_t k = 0; k <= n->nlfn && n->pos >= k * 32; k++) {
        if (dir_entry(v, dirc, n->pos - k * 32, e, 0) != 0) return;
        e[0] = 0xE5;
        dir_entry(v, dirc, n->pos - k * 32, e, 1);
    }
}

/* ── nodes ─────────────────────────────────────────────────────────── */

static int new_node(fvol_t* v) {
    for (uint32_t i = 1; i < v->nnodes; i++) if (!v->nodes[i].used) return (int)i;
    if (v->nnodes == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 64;
        fnode_t* nn = (fnode_t*)krealloc(v->nodes, cap * sizeof(fnode_t));
        if (!nn) return -1;
        v->nodes = nn;
        v->cap = cap;
    }
    memset(&v->nodes[v->nnodes], 0, sizeof(fnode_t));
    return (int)v->nnodes++;
}

/* ── fs mount ops ──────────────────────────────────────────────────── */

static int op_read(void* ctx, uint32_t node, uint8_t* buf, uint32_t size) {
    fvol_t* v = (fvol_t*)ctx;
    if (node >= v->nnodes || !v->nodes[node].used) return -1;
    v->io_err = 0;
    if (!size) return 0;
    return read_chain(v, v->nodes[node].first, buf, size);
}

static int op_write(void* ctx, uint32_t node, const void* data, uint32_t len) {
    fvol_t* v = (fvol_t*)ctx;
    if (node == 0 || node >= v->nnodes || !v->nodes[node].used) return -1;
    fnode_t* n = &v->nodes[node];
    v->io_err = 0;
    if (n->first) { free_chain(v, n->first); n->first = 0; }
    int rc = 0;
    if (len) {
        uint32_t first;
        uint32_t clusters = (len + v->clus_bytes - 1) / v->clus_bytes;
        if (alloc_chain(v, clusters, 0, &first) != 0) rc = -1;
        else {
            n = &v->nodes[node];
            n->first = first;
            if (write_chain(v, first, (const uint8_t*)data, len) != 0) rc = -1;
        }
    }
    n->size = rc == 0 ? len : 0;
    if (update_entry(v, node) != 0) rc = -1;
    if (fat_flush(v) != 0) rc = -1;
    return rc;
}

static int op_append(void* ctx, uint32_t node, uint32_t old_size, const void* data, uint32_t len) {
    fvol_t* v = (fvol_t*)ctx;
    if (node == 0 || node >= v->nnodes || !v->nodes[node].used) return -1;
    fnode_t* n = &v->nodes[node];
    if (!n->first || old_size == 0) {
        uint8_t* all = (uint8_t*)data;
        return op_write(ctx, node, all, len);
    }
    if (!len) return 0;
    v->io_err = 0;
    const uint8_t* d = (const uint8_t*)data;
    uint32_t count;
    uint32_t last = chain_last(v, n->first, &count);
    uint32_t used = old_size - (count - 1) * v->clus_bytes;    /* bytes in the last cluster */
    if (old_size > count * v->clus_bytes || used > v->clus_bytes) used = v->clus_bytes;
    uint32_t room = v->clus_bytes - used;
    int rc = 0;
    if (room) {                          /* fill the last cluster */
        uint32_t take = len < room ? len : room;
        if (dev_read(v, clus_lba(v, last), v->spc, v->cbuf) != 0) rc = -1;
        else {
            memcpy(v->cbuf + used, d, take);
            if (dev_write(v, clus_lba(v, last), v->spc, v->cbuf) != 0) rc = -1;
        }
        d += take;
        len -= take;
        old_size += take;
    }
    if (rc == 0 && len) {
        uint32_t first;
        if (alloc_chain(v, (len + v->clus_bytes - 1) / v->clus_bytes, last, &first) != 0) rc = -1;
        else if (write_chain(v, first, d, len) != 0) rc = -1;
        else old_size += len;
    }
    n = &v->nodes[node];
    n->size = old_size;
    if (update_entry(v, node) != 0) rc = -1;
    if (fat_flush(v) != 0) rc = -1;
    return rc;
}

static int op_create(void* ctx, uint32_t parent, const char* name, int is_dir, uint32_t* node) {
    fvol_t* v = (fvol_t*)ctx;
    if (parent >= v->nnodes || !v->nodes[parent].used || !v->nodes[parent].is_dir) return -1;
    v->io_err = 0;
    int ni = new_node(v);
    if (ni < 0) return -1;
    uint8_t tmpl[32];
    memset(tmpl, 0, 32);
    uint16_t t, d;
    now_stamp(&t, &d);
    wr16(tmpl + 14, t); wr16(tmpl + 16, d); wr16(tmpl + 18, d);
    wr16(tmpl + 22, t); wr16(tmpl + 24, d);
    uint32_t first = 0;
    if (is_dir) {
        tmpl[11] = ATTR_DIR;
        first = alloc_clus(v, 0);
        if (!first) { fat_flush(v); return -1; }
        /* "." and ".." (".." of a folder in the root points to cluster 0) */
        memset(v->cbuf, 0, v->clus_bytes);
        uint32_t up = parent == 0 ? 0 : v->nodes[parent].first;
        uint8_t* e = v->cbuf;
        memcpy(e, tmpl, 32);
        memcpy(e, ".          ", 11);
        wr16(e + 20, (uint16_t)(first >> 16)); wr16(e + 26, (uint16_t)first);
        e += 32;
        memcpy(e, tmpl, 32);
        memcpy(e, "..         ", 11);
        wr16(e + 20, (uint16_t)(up >> 16)); wr16(e + 26, (uint16_t)up);
        if (dev_write(v, clus_lba(v, first), v->spc, v->cbuf) != 0) { free_chain(v, first); fat_flush(v); return -1; }
        wr16(tmpl + 20, (uint16_t)(first >> 16));
        wr16(tmpl + 26, (uint16_t)first);
    } else {
        tmpl[11] = ATTR_ARCH;
    }
    uint32_t pos;
    uint8_t nlfn;
    if (add_entries(v, parent, name, tmpl, &pos, &nlfn) != 0) {
        if (first) free_chain(v, first);
        fat_flush(v);
        return -1;
    }
    fnode_t* n = &v->nodes[ni];
    n->used = 1;
    n->is_dir = is_dir;
    n->parent = parent;
    n->first = first;
    n->size = 0;
    n->pos = pos;
    n->nlfn = nlfn;
    *node = (uint32_t)ni;
    return fat_flush(v);
}

static int op_remove(void* ctx, uint32_t node, int is_dir) {
    fvol_t* v = (fvol_t*)ctx;
    (void)is_dir;
    if (node == 0 || node >= v->nnodes || !v->nodes[node].used) return -1;
    v->io_err = 0;
    mark_deleted(v, node);
    if (v->nodes[node].first) free_chain(v, v->nodes[node].first);
    v->nodes[node].used = 0;
    return (fat_flush(v) != 0 || v->io_err) ? -1 : 0;
}

static int op_rename(void* ctx, uint32_t node, int is_dir, uint32_t new_parent, const char* new_name) {
    fvol_t* v = (fvol_t*)ctx;
    if (node == 0 || node >= v->nnodes || !v->nodes[node].used) return -1;
    if (new_parent >= v->nnodes || !v->nodes[new_parent].used || !v->nodes[new_parent].is_dir) return -1;
    v->io_err = 0;
    fnode_t old = v->nodes[node];
    uint8_t e[32];
    if (dir_entry(v, node_first(v, old.parent), old.pos, e, 0) != 0) return -1;
    uint32_t pos;
    uint8_t nlfn;
    if (add_entries(v, new_parent, new_name, e, &pos, &nlfn) != 0) { fat_flush(v); return -1; }
    mark_deleted(v, node);                /* the old entries (positions unchanged) */
    fnode_t* n = &v->nodes[node];
    n->parent = new_parent;
    n->pos = pos;
    n->nlfn = nlfn;
    if (is_dir && old.parent != new_parent && n->first) {
        /* its ".." now points to the new parent */
        uint8_t dd[32];
        if (dir_entry(v, n->first, 32, dd, 0) == 0 && memcmp(dd, "..         ", 11) == 0) {
            uint32_t up = new_parent == 0 ? 0 : v->nodes[new_parent].first;
            wr16(dd + 20, (uint16_t)(up >> 16));
            wr16(dd + 26, (uint16_t)up);
            dir_entry(v, n->first, 32, dd, 1);
        }
    }
    return (fat_flush(v) != 0 || v->io_err) ? -1 : 0;
}

static void op_unmount(void* ctx) {
    fvol_t* v = (fvol_t*)ctx;
    fat_flush(v);
    if (v->mnt > 0 && v->mnt <= FS_MAX_MOUNTS && g_vols[v->mnt] == v) g_vols[v->mnt] = NULL;
    kfree(v->nodes);
    kfree(v->cbuf);
    kfree(v);
}

static const fs_mount_ops_t g_ops = {
    op_read, op_write, op_append, op_create, op_remove, op_rename, op_unmount,
};

/* ── mounting ──────────────────────────────────────────────────────── */

/* the 8.3 name of entry e as "name.ext" (honoring the lower-case flags) */
static void sfn_to_name(const uint8_t* e, char* out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32;
            out[n++] = c;
        }
    }
    out[n] = 0;
}

static void scan_dir(fvol_t* v, uint32_t dnode, int fsdir, int depth) {
    uint32_t len;
    uint8_t* dir = dir_load(v, node_first(v, dnode), &len);
    if (!dir) return;
    char lfn[256];
    int lfn_n = 0, lfn_have = 0;
    uint8_t lfn_chk = 0;
    for (uint32_t p = 0; p + 32 <= len; p += 32) {
        const uint8_t* e = dir + p;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfn_have = 0; continue; }
        if (e[11] == ATTR_LFN) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { lfn_have = seq; lfn_n = 0; lfn_chk = e[13]; memset(lfn, 0, sizeof(lfn)); }
            if (!lfn_have || seq < 1 || seq > 20 || e[13] != lfn_chk) { lfn_have = 0; continue; }
            static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13; i++) {
                uint16_t ch = rd16(e + offs[i]);
                int at = (seq - 1) * 13 + i;
                if (ch == 0 || ch == 0xFFFF || at >= 255) continue;
                lfn[at] = (char)(ch < 128 && ch >= 32 ? ch : '_');
            }
            lfn_n++;
            continue;
        }
        if (e[11] & ATTR_VOLID) {
            if (dnode == 0 && !(e[11] & ATTR_DIR)) {
                for (int i = 0; i < 11; i++) v->label[i] = (char)e[i];
                v->label[11] = 0;
                for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
            }
            lfn_have = 0;
            continue;
        }
        char name[256];
        int use_lfn = lfn_have && lfn_n == lfn_have && lfn_chk == lfn_sum(e) && lfn[0];
        if (use_lfn) kstrlcpy(name, lfn, sizeof(name));
        else sfn_to_name(e, name);
        int nl = use_lfn ? lfn_n : 0;
        lfn_have = 0;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        int ni = new_node(v);
        if (ni < 0) break;
        fnode_t* n = &v->nodes[ni];
        n->used = 1;
        n->is_dir = (e[11] & ATTR_DIR) != 0;
        n->parent = dnode;
        n->first = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        n->size = n->is_dir ? 0 : rd32(e + 28);
        n->pos = p;
        n->nlfn = (uint8_t)nl;
        if (strlen(name) >= FS_NAME_LEN) name[FS_NAME_LEN - 1] = 0;   /* the tree's limit */
        if (n->is_dir) {
            if (!valid_clus(v, n->first)) { n->used = 0; continue; }
            int fd = fs_mount_add_dir(v->mnt, fsdir, name, (uint32_t)ni);
            if (fd < 0) { v->nodes[ni].used = 0; v->skipped++; continue; }
            if (depth < MAX_DEPTH) scan_dir(v, (uint32_t)ni, fd, depth + 1);
        } else {
            if (fs_mount_add_file(v->mnt, fsdir, name, (uint32_t)ni, n->size) < 0) {
                v->nodes[ni].used = 0;
                v->skipped++;
            }
        }
    }
    kfree(dir);
}

/* a FAT32 boot sector? */
static int is_fat32_vbr(const uint8_t* b) {
    if (rd16(b + 510) != 0xAA55) return 0;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    if (rd16(b + 11) != SEC) return 0;
    uint8_t spc = b[13];
    if (!spc || (spc & (spc - 1))) return 0;
    if (rd16(b + 14) == 0 || b[16] == 0) return 0;
    return rd16(b + 22) == 0 && rd32(b + 36) != 0 && rd16(b + 17) == 0;
}

static int is_fat1x_vbr(const uint8_t* b) {
    return rd16(b + 510) == 0xAA55 && rd16(b + 11) == SEC && rd16(b + 22) != 0;
}

/* the first sector of the FAT32 volume: the whole device, or a partition */
static int find_volume(fvol_t* v, uint32_t* base, char* err, int errcap) {
    uint8_t* b = v->sec;
    if (dev_read(v, 0, 1, b) != 0) { kstrlcpy(err, "cannot read the stick", (size_t)errcap); return -1; }
    if (is_fat32_vbr(b)) { *base = 0; return 0; }
    if (rd16(b + 510) != 0xAA55) { kstrlcpy(err, "no partition table or FAT32 volume", (size_t)errcap); return -1; }
    uint32_t cand[4];
    int nc = 0, gpt = 0, fat1x = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* pe = b + 446 + i * 16;
        uint8_t type = pe[4];
        uint32_t lba = rd32(pe + 8);
        if (type == 0xEE) gpt = 1;
        else if (type && lba) cand[nc++] = lba;
    }
    if (gpt) {
        if (dev_read(v, 1, 1, b) != 0 || memcmp(b, "EFI PART", 8) != 0) { kstrlcpy(err, "bad GPT", (size_t)errcap); return -1; }
        uint32_t ent_lba = rd32(b + 72), nent = rd32(b + 80), esz = rd32(b + 84);
        if (esz < 128 || esz > SEC || nent > 128) nent = 0;
        nc = 0;
        for (uint32_t i = 0; i < nent && nc < 4; i++) {
            uint32_t off = i * esz;
            if (dev_read(v, ent_lba + off / SEC, 1, b) != 0) break;
            const uint8_t* ge = b + off % SEC;
            int empty = 1;
            for (int k = 0; k < 16; k++) if (ge[k]) empty = 0;
            if (!empty && rd32(ge + 32)) cand[nc++] = rd32(ge + 32);
        }
    }
    for (int i = 0; i < nc; i++) {
        if (dev_read(v, cand[i], 1, b) != 0) continue;
        if (is_fat32_vbr(b)) { *base = cand[i]; return 0; }
        if (is_fat1x_vbr(b)) fat1x = 1;
    }
    kstrlcpy(err, fat1x ? "FAT12/FAT16 volume - only FAT32 is supported" : "no FAT32 partition found",
             (size_t)errcap);
    return -1;
}

int fat32_mount(blockdev_t* bd, const char* path, char* err, int errcap) {
    err[0] = 0;
    fvol_t* v = (fvol_t*)kzalloc(sizeof(fvol_t));
    if (!v) { kstrlcpy(err, "out of memory", (size_t)errcap); return -1; }
    v->bd = bd;
    uint32_t base;
    if (find_volume(v, &base, err, errcap) != 0) { kfree(v); return -1; }
    if (dev_read(v, base, 1, v->sec) != 0) { kstrlcpy(err, "cannot read the boot sector", (size_t)errcap); kfree(v); return -1; }
    const uint8_t* b = v->sec;
    uint32_t reserved = rd16(b + 14);
    uint32_t total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    v->base = base;
    v->spc = b[13];
    v->clus_bytes = v->spc * SEC;
    v->nfats = b[16];
    v->fat_sectors = rd32(b + 36);
    v->fat_lba = base + reserved;
    v->data_lba = v->fat_lba + v->nfats * v->fat_sectors;
    uint32_t meta = reserved + v->nfats * v->fat_sectors;
    v->nclusters = total > meta ? (total - meta) / v->spc : 0;
    if (v->nclusters > v->fat_sectors * (SEC / 4) - 2) v->nclusters = v->fat_sectors * (SEC / 4) - 2;
    v->root = rd32(b + 44);
    v->fsinfo = rd16(b + 48) ? base + rd16(b + 48) : 0;
    v->hint = 2;
    memcpy(v->label, b + 71, 11);
    v->label[11] = 0;
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
    if (strcmp(v->label, "NO NAME") == 0) v->label[0] = 0;
    if (!v->nclusters || !valid_clus(v, v->root) || v->clus_bytes > 65536 ||
        base + total > bd->sectors + 1) {
        kstrlcpy(err, "the FAT32 boot sector does not make sense", (size_t)errcap);
        kfree(v);
        return -1;
    }
    /* start looking for free clusters where the last session stopped */
    if (v->fsinfo && dev_read(v, v->fsinfo, 1, v->sec) == 0 && rd32(v->sec) == 0x41615252u) {
        if (valid_clus(v, rd32(v->sec + 492))) v->hint = rd32(v->sec + 492);
        /* the free count the last system kept there, if it kept one */
        if (rd32(v->sec + 484) == 0x61417272u && rd32(v->sec + 488) <= v->nclusters) {
            v->nfree = rd32(v->sec + 488);
            v->nfree_ok = 1;
        }
    }
    v->cbuf = (uint8_t*)kmalloc(v->clus_bytes);
    int root = new_node(v);
    if (!v->cbuf || root != 0) { kstrlcpy(err, "out of memory", (size_t)errcap); kfree(v->cbuf); kfree(v->nodes); kfree(v); return -1; }
    v->nodes[0].used = 1;
    v->nodes[0].is_dir = 1;
    v->nodes[0].first = v->root;

    char where[FS_PATH_LEN];
    if (path) kstrlcpy(where, path, sizeof(where));
    else {
        for (int i = 1; i <= FS_MAX_MOUNTS; i++) {
            if (i == 1) ksnprintf(where, sizeof(where), "/mnt/%s", bd->kind[0] ? bd->kind : "usb");
            else ksnprintf(where, sizeof(where), "/mnt/%s%d", bd->kind[0] ? bd->kind : "usb", i);
            if (fs_find_dir(where) < 0) break;
        }
    }
    int mnt = fs_mount(where, &g_ops, v, 0);
    if (mnt < 0) {
        ksnprintf(err, (size_t)errcap, "cannot mount at %s", where);
        kfree(v->cbuf); kfree(v->nodes); kfree(v);
        return -1;
    }
    v->mnt = mnt;
    g_vols[mnt] = v;
    scan_dir(v, 0, fs_mount_root(mnt), 0);
    klog("fat32: %s (%s) mounted at %s - %u clusters of %u bytes%s\n", bd->name, v->label[0] ? v->label : "no label",
         where, v->nclusters, v->clus_bytes, v->skipped ? " (some entries did not fit)" : "");
    return mnt;
}

int fat32_space(int mnt, uint64_t* total, uint64_t* free_bytes) {
    if (mnt <= 0 || mnt > FS_MAX_MOUNTS || !g_vols[mnt]) return 0;
    fvol_t* v = g_vols[mnt];
    if (!v->nfree_ok) {
        /* count the free clusters once, 32 KiB of FAT at a time; kept up to date after that */
        if (fat_flush(v) != 0) return 0;
        uint8_t* buf = (uint8_t*)kmalloc(64 * SEC);
        if (!buf) return 0;
        uint32_t n = 0, last = v->nclusters + 1;
        for (uint32_t sec = 0; sec * (SEC / 4) <= last; sec += 64) {
            uint32_t cnt = v->fat_sectors - sec < 64 ? v->fat_sectors - sec : 64;
            if (!cnt || dev_read(v, v->fat_lba + sec, cnt, buf) != 0) { kfree(buf); return 0; }
            for (uint32_t k = 0; k < cnt * (SEC / 4); k++) {
                uint32_t c = sec * (SEC / 4) + k;
                if (c >= 2 && c <= last && (rd32(buf + k * 4) & FAT_MASK) == 0) n++;
            }
            task_maybe_yield();
        }
        kfree(buf);
        v->nfree = n;
        v->nfree_ok = 1;
    }
    *total = (uint64_t)v->nclusters * v->clus_bytes;
    *free_bytes = (uint64_t)v->nfree * v->clus_bytes;
    return 1;
}

int fat32_describe(int mnt, char* out, int cap) {
    if (mnt <= 0 || mnt > FS_MAX_MOUNTS || !g_vols[mnt]) return 0;
    fvol_t* v = g_vols[mnt];
    uint32_t mib = (uint32_t)(((uint64_t)v->nclusters * v->clus_bytes) >> 20);
    ksnprintf(out, (size_t)cap, "%-12s FAT32  %s%s%s on %s (%s), %u MiB%s", fs_mount_point(mnt),
              v->label[0] ? "\"" : "", v->label[0] ? v->label : "(no label)", v->label[0] ? "\"" : "",
              v->bd->name, v->bd->model, mib, v->skipped ? " - partly shown (too many files)" : "");
    return 1;
}
