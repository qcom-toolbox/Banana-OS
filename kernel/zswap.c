/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zswap.h"
#include "uvm.h"
#include "kheap.h"
#include "hmem.h"
#include "kstring.h"
#include "spinlock.h"
#include "task.h"
#include "timer.h"
#include "serial.h"
#include "fsdisk.h"
#include "upage.h"

/* ── the entries: blocks of 128 (a page) that never move (pointers stay good) ── */

#define EBLK   128u
#define NBLK   8192u                       /* up to 1M pages (4 GiB of app memory) */

static zent_t*    g_blk[NBLK];
static uint32_t   g_nblk;
static uint32_t   g_free_head;             /* a free entry (its frame holds the next), 0: none */
/* every change of an entry's state is made holding it (after the app's
 * vm_lock, when that is held too) */
static spinlock_t g_zlock = SPINLOCK_INIT;
static zswap_stats_t g_st;
static int        g_on = 1;
/* the STORED entries with data, the oldest first: what goes to the disk */
static uint32_t   g_lru_head, g_lru_tail;

/* the swap area on the installed disk, by the sector: a compressed page takes
 * as many as its bytes need (a slot: its first sector + 1) - not a whole 4 KiB,
 * or text that packs to 300 bytes would fill the area 13 times as fast */
static uint32_t   g_sw_lba, g_nslots, g_slots_used, g_sw_hint;
static uint8_t*   g_slot_map;

static uint32_t nsect(uint32_t len) { return len ? (len + 511u) / 512u : 1u; }

zent_t* zswap_ent(uint32_t idx) {
    if (!idx) return NULL;
    uint32_t b = idx / EBLK, i = idx % EBLK;
    return b < g_nblk ? &g_blk[b][i] : NULL;
}

static void lru_add(uint32_t idx, zent_t* e) {
    e->prev = g_lru_tail;
    e->next = 0;
    if (g_lru_tail) zswap_ent(g_lru_tail)->next = idx; else g_lru_head = idx;
    g_lru_tail = idx;
    e->in_lru = 1;
}

static void lru_del(uint32_t idx, zent_t* e) {
    (void)idx;
    if (!e->in_lru) return;
    if (e->prev) zswap_ent(e->prev)->next = e->next; else g_lru_head = e->next;
    if (e->next) zswap_ent(e->next)->prev = e->prev; else g_lru_tail = e->prev;
    e->prev = e->next = 0;
    e->in_lru = 0;
}

/* n free sectors in a row (the lock held), from where the last one ended */
static uint32_t slot_get(uint32_t n) {
    uint32_t total = g_nslots, run = 0, start = 0;
    if (!total || n > total) return 0;
    for (uint32_t k = 0; k < total + n; k++) {
        uint32_t i = (g_sw_hint + k) % total;
        if (!i) run = 0;                                     /* (a run does not wrap) */
        if (!(i & 7) && g_slot_map[i >> 3] == 0xFF) { run = 0; k += 7; continue; }
        if (g_slot_map[i >> 3] & (1u << (i & 7))) { run = 0; continue; }
        if (!run) start = i;
        if (++run < n) continue;
        for (uint32_t j = start; j < start + n; j++) g_slot_map[j >> 3] |= (uint8_t)(1u << (j & 7));
        g_slots_used += n;
        g_sw_hint = start + n;
        return start + 1;
    }
    return 0;
}

static void slot_put(uint32_t s, uint32_t n) {
    if (!s || s - 1u + n > g_nslots) return;
    for (uint32_t j = s - 1u; j < s - 1u + n; j++)
        if (g_slot_map[j >> 3] & (1u << (j & 7))) { g_slot_map[j >> 3] &= (uint8_t)~(1u << (j & 7)); g_slots_used--; }
}

/* a new block, its entries (but number 0) on the free list: 0, or -1 (no
 * memory). From the heap - or, short, a page of the pool's reserve (kernel/
 * upage.c), never given back: entries run out as pages go to the disk, and
 * without one nothing more could be compressed to make memory */
static int add_block(void) {
    if (g_nblk == NBLK) return -1;
    int pooled = 0;
    (void)pooled;                                  /* (32-bit: no pool) */
    zent_t* blk = (zent_t*)kzalloc(EBLK * sizeof(zent_t));
#ifdef __x86_64__
    if (!blk && (blk = (zent_t*)upage_alloc_reserve()) != NULL) {
        memset(blk, 0, 4096);
        pooled = 1;
    }
#endif
    if (!blk) return -1;
    uintptr_t f = spin_lock_irq(&g_zlock);
    if (g_nblk == NBLK) {
        spin_unlock_irq(&g_zlock, f);
#ifdef __x86_64__
        if (pooled) upage_free(blk); else
#endif
        kfree(blk);
        return -1;
    }
    uint32_t b = g_nblk++;
    g_blk[b] = blk;
    for (uint32_t i = EBLK; i-- > 0; ) {
        uint32_t idx = b * EBLK + i;
        if (!idx) continue;
        blk[i].state = ZS_FREE;
        blk[i].frame = g_free_head;
        g_free_head = idx;
    }
    spin_unlock_irq(&g_zlock, f);
    return 0;
}

uint32_t zswap_new(uintptr_t frame) {
    uintptr_t f = spin_lock_irq(&g_zlock);
    while (!g_free_head) {
        spin_unlock_irq(&g_zlock, f);
        if (add_block() != 0) return 0;
        f = spin_lock_irq(&g_zlock);
    }
    uint32_t idx = g_free_head;
    zent_t* e = zswap_ent(idx);
    g_free_head = (uint32_t)e->frame;
    memset(e, 0, sizeof(*e));
    e->state = ZS_PENDING;
    e->frame = frame;
    spin_unlock_irq(&g_zlock, f);
    return idx;
}

/* the entry back on the free list (the lock held); its data / slot: the caller's */
static void put_entry(uint32_t idx, zent_t* e) {
    lru_del(idx, e);
    e->state = ZS_FREE;
    e->data = NULL;
    e->len = 0;
    e->slot = 0;
    e->frame = g_free_head;
    g_free_head = idx;
}

void zswap_free(uint32_t idx) {
    zent_t* e = zswap_ent(idx);
    if (!e || e->state == ZS_FREE) return;
    uintptr_t f = spin_lock_irq(&g_zlock);
    uint8_t* data = e->data;
    if (e->state == ZS_STORED) {
        g_st.pages--;
        if (!e->len) g_st.zero_pages--;
        g_st.bytes -= e->len;
    } else if (e->state == ZS_DISK) {
        g_st.disk_pages--;
        slot_put(e->slot, nsect(e->len));
    }
    put_entry(idx, e);
    spin_unlock_irq(&g_zlock, f);
    kfree(data);
}

/* its page unmapped or the app gone: the entry goes - or, while it is being
 * written or read, it is marked so the one doing that frees it */
void zswap_release(uint32_t idx) {
    zent_t* e = zswap_ent(idx);
    if (!e) return;
    uint8_t* data = NULL;
    uintptr_t f = spin_lock_irq(&g_zlock);
    switch (e->state) {
    case ZS_WRITING: g_st.pages--; g_st.bytes -= e->len; e->state = ZS_DEAD; break;
    case ZS_READING: g_st.disk_pages--; e->state = ZS_DEAD; break;
    case ZS_STORED:
        data = e->data;
        g_st.pages--;
        if (!e->len) g_st.zero_pages--;
        g_st.bytes -= e->len;
        put_entry(idx, e);
        break;
    case ZS_DISK: g_st.disk_pages--; slot_put(e->slot, nsect(e->len)); put_entry(idx, e); break;
    default: break;
    }
    spin_unlock_irq(&g_zlock, f);
    kfree(data);
}

void zswap_store(uint32_t idx, uint8_t* data, uint32_t len) {
    zent_t* e = zswap_ent(idx);
    if (!e) { kfree(data); return; }
    uintptr_t f = spin_lock_irq(&g_zlock);
    e->data = data;
    e->len = data ? len : 0;
    e->state = ZS_STORED;
    e->frame = 0;
    g_st.pages++;
    if (!e->len) g_st.zero_pages++;
    g_st.bytes += e->len;
    g_st.pages_out++;
    if (e->len) lru_add(idx, e);                  /* (zeros never go to the disk) */
    spin_unlock_irq(&g_zlock, f);
}

static int unpack_into(const zent_t* e, uint8_t* page) {
    if (!e->len) { memset(page, 0, 4096); return 0; }
    if (e->len == 4096) { memcpy(page, e->data, 4096); return 0; }     /* (kept as it was: did not compress) */
    return zswap_unpack(e->data, e->len, page);
}

int zswap_take(uint32_t idx, uint8_t* page) {
    zent_t* e = zswap_ent(idx);
    if (!e) return -1;
    uintptr_t f = spin_lock_irq(&g_zlock);
    int st = (int)e->state, rc;
    if (st == ZS_STORED) {
        rc = unpack_into(e, page);
        uint8_t* data = e->data;                 /* (freed here, under the lock: the writer must not take it) */
        g_st.pages--;
        if (!e->len) g_st.zero_pages--;
        g_st.bytes -= e->len;
        g_st.pages_in++;
        put_entry(idx, e);
        spin_unlock_irq(&g_zlock, f);
        kfree(data);
        return rc;
    }
    if (st == ZS_WRITING) {
        /* being written to the disk: its data is still here - the writer frees it */
        rc = unpack_into(e, page);
        e->state = ZS_TAKEN;
        g_st.pages--;
        g_st.bytes -= e->len;
        g_st.pages_in++;
        spin_unlock_irq(&g_zlock, f);
        return rc;
    }
    spin_unlock_irq(&g_zlock, f);
    return st == ZS_DISK || st == ZS_READING ? 2 : -1;
}

/* ── the disk ──────────────────────────────────────────────────────────
 * The oldest compressed pages are written to the swap area and their RAM
 * freed; a page fault on one reads it back first (kernel/app.c: on the
 * boot core, interrupts on - the fault may wait for the disk). */

int zswap_disk_init(void) {
#ifdef __x86_64__
    if (g_nslots) return 0;
    /* as much as the RAM (4 GiB at most), 64 MiB at least */
    uint64_t ram = (uint64_t)kheap_total_bytes() + hmem_total_bytes();
    if (ram > (4ull << 30)) ram = 4ull << 30;
    uint32_t lba, sect;
    if (fsdisk_swap_area((uint32_t)(ram / 512u), (64u << 20) / 512u, &lba, &sect) != 0) return -1;
    uint32_t n = sect;
    uint8_t* map = (uint8_t*)kzalloc(n / 8u + 1u);
    if (!map) return -1;
    g_slot_map = map;
    g_sw_lba = lba;
    g_nslots = n;
    return 0;
#else
    return -1;
#endif
}

uint64_t zswap_disk_bytes(void) { return (uint64_t)g_nslots * 512u; }

/* the oldest compressed pages to the disk (up to max): how many went */
static uint8_t      g_wb_buf[4096];             /* (not from the heap: it runs when the heap is short) */
static volatile int g_wb_busy;

uint32_t zswap_writeback(uint32_t max) {
    if (!g_nslots || __sync_lock_test_and_set(&g_wb_busy, 1)) return 0;
    uint8_t* buf = g_wb_buf;
    uint32_t done = 0;
    while (done < max) {
        uintptr_t f = spin_lock_irq(&g_zlock);
        uint32_t idx = g_lru_head, slot = idx ? slot_get(nsect(zswap_ent(idx)->len)) : 0;
        if (!idx || !slot) { spin_unlock_irq(&g_zlock, f); break; }
        zent_t* e = zswap_ent(idx);
        lru_del(idx, e);
        e->state = ZS_WRITING;
        e->slot = slot;
        uint32_t len = e->len;
        memcpy(buf, e->data, len);
        spin_unlock_irq(&g_zlock, f);
        memset(buf + len, 0, (512u - len % 512u) % 512u);
        uint32_t ns = (len + 511u) / 512u;
        int rc = fsdisk_swap_io(1, g_sw_lba + (slot - 1u), ns, buf);
        uint8_t* drop = NULL;
        f = spin_lock_irq(&g_zlock);
        if (e->state == ZS_WRITING && rc == 0) {
            drop = e->data;                         /* on the disk: its RAM goes */
            e->data = NULL;
            e->state = ZS_DISK;
            g_st.pages--;
            g_st.bytes -= len;
            g_st.disk_pages++;
            g_st.disk_out++;
            done++;
        } else if (e->state == ZS_WRITING) {
            slot_put(slot, nsect(len));                         /* (not written: it stays in RAM) */
            e->slot = 0;
            e->state = ZS_STORED;
            lru_add(idx, e);
            spin_unlock_irq(&g_zlock, f);
            break;
        } else {
            /* touched (TAKEN) or unmapped (DEAD) meanwhile: nothing of it is needed */
            drop = e->data;
            slot_put(slot, nsect(len));
            put_entry(idx, e);
        }
        spin_unlock_irq(&g_zlock, f);
        kfree(drop);
        if ((done & 63u) == 0) task_yield();
    }
    __sync_lock_release(&g_wb_busy);
    return done;
}

int zswap_disk_in(uint32_t idx) {
    zent_t* e = zswap_ent(idx);
    if (!e) return -1;
    for (;;) {
        uintptr_t f = spin_lock_irq(&g_zlock);
        int st = (int)e->state;
        if (st == ZS_READING) { spin_unlock_irq(&g_zlock, f); task_sleep_ms(1); continue; }   /* (another thread reads it) */
        if (st != ZS_DISK) { spin_unlock_irq(&g_zlock, f); return 0; }                       /* (in RAM again, or gone) */
        e->state = ZS_READING;
        uint32_t len = e->len, slot = e->slot;
        spin_unlock_irq(&g_zlock, f);
        uint32_t ns = (len + 511u) / 512u;
        uint8_t* data = (uint8_t*)kmalloc(ns * 512u);
        if (!data) {                                /* (no memory to read it into: make some, then again) */
            f = spin_lock_irq(&g_zlock);
            if (e->state == ZS_READING) e->state = ZS_DISK;
            else { slot_put(slot, nsect(len)); put_entry(idx, e); }
            spin_unlock_irq(&g_zlock, f);
            return -2;
        }
        int rc = fsdisk_swap_io(0, g_sw_lba + (slot - 1u), ns, data);
        f = spin_lock_irq(&g_zlock);
        if (e->state == ZS_READING && rc == 0) {
            e->data = data;
            e->state = ZS_STORED;
            slot_put(slot, nsect(len));
            e->slot = 0;
            g_st.disk_pages--;
            g_st.pages++;
            g_st.bytes += len;
            g_st.disk_in++;
            lru_add(idx, e);
            spin_unlock_irq(&g_zlock, f);
            return 0;
        }
        if (e->state == ZS_READING) {               /* (the disk failed: it stays there) */
            e->state = ZS_DISK;
            spin_unlock_irq(&g_zlock, f);
            kfree(data);
            klog("zswap: a swapped page could not be read back from the disk\n");
            return -1;
        }
        slot_put(slot, nsect(len));                             /* (unmapped meanwhile: DEAD) */
        put_entry(idx, e);
        spin_unlock_irq(&g_zlock, f);
        kfree(data);
        return 0;
    }
}

void zswap_stats(zswap_stats_t* s) {
    uintptr_t f = spin_lock_irq(&g_zlock);
    *s = g_st;
    s->disk_used = (uint64_t)g_slots_used * 512u;
    s->entries = g_nblk * EBLK;
    spin_unlock_irq(&g_zlock, f);
}

/* the stats against the entries themselves: the pages on the disk (DISK, or
 * READING: still counted) and the sectors held by an entry's slot - what an
 * entry gone without its slot given back would leave (selftest). Slots that
 * page-out holds between zswap_disk_put and zswap_store_disk are in no entry
 * yet: *held may then be below *used for a moment */
void zswap_audit(uint32_t* disk_ents, uint32_t* disk_counted, uint64_t* held, uint64_t* used) {
    uint32_t n = 0;
    uint64_t sect = 0;
    uintptr_t f = spin_lock_irq(&g_zlock);
    for (uint32_t b = 0; b < g_nblk; b++)
        for (uint32_t i = 0; i < EBLK; i++) {
            const zent_t* e = &g_blk[b][i];
            if (e->state == ZS_FREE || !e->slot) continue;
            if (e->state == ZS_DISK || e->state == ZS_READING) n++;
            sect += nsect(e->len);
        }
    *disk_ents = n;
    *disk_counted = g_st.disk_pages;
    *held = sect * 512u;
    *used = (uint64_t)g_slots_used * 512u;
    spin_unlock_irq(&g_zlock, f);
}

void zswap_enable(int on) { g_on = on != 0; }
int  zswap_enabled(void) { return g_on; }

/* ── the compressor: LZ77 in the LZ4 manner, for one 4 KiB page ────────
 * Sequences of: a token (literals in its high 4 bits, match length - 4 in
 * its low 4; 15 = more in the bytes that follow, 255 each), the literals,
 * a 2-byte offset back, more match length. The input ends after the last
 * sequence's literals. */

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int put_len(uint8_t* out, uint32_t* o, uint32_t cap, uint32_t n) {
    while (n >= 255) { if (*o >= cap) return -1; out[(*o)++] = 255; n -= 255; }
    if (*o >= cap) return -1;
    out[(*o)++] = (uint8_t)n;
    return 0;
}

static int put_seq(uint8_t* out, uint32_t* o, uint32_t cap, const uint8_t* lit, uint32_t nlit, uint32_t off, uint32_t mlen) {
    uint32_t ml = mlen ? mlen - 4 : 0;
    if (*o >= cap) return -1;
    out[(*o)++] = (uint8_t)((nlit < 15 ? nlit : 15) << 4 | (ml < 15 ? ml : 15));
    if (nlit >= 15 && put_len(out, o, cap, nlit - 15) < 0) return -1;
    if (*o + nlit > cap) return -1;
    memcpy(out + *o, lit, nlit);
    *o += nlit;
    if (!mlen) return 0;
    if (*o + 2 > cap) return -1;
    out[(*o)++] = (uint8_t)off;
    out[(*o)++] = (uint8_t)(off >> 8);
    if (ml >= 15 && put_len(out, o, cap, ml - 15) < 0) return -1;
    return 0;
}

uint32_t zswap_pack(const uint8_t* page, uint8_t* out, uint32_t cap, int* zero, uint16_t* ht) {
    const uint64_t* w = (const uint64_t*)page;
    uint64_t any = 0;
    for (int i = 0; i < 512; i++) any |= w[i];
    *zero = any == 0;
    if (*zero) return 0;
    memset(ht, 0, 4096 * sizeof(uint16_t));   /* position + 1 of the last 4 bytes with each hash */
    uint32_t ip = 0, anchor = 0, o = 0;
    while (ip + 4 <= 4096) {
        uint32_t v = rd32(page + ip), h = (v * 2654435761u) >> 20;
        uint32_t ref = ht[h];
        ht[h] = (uint16_t)(ip + 1);
        if (ref && rd32(page + ref - 1) == v) {
            ref--;
            uint32_t m = 4;
            while (ip + m < 4096 && page[ref + m] == page[ip + m]) m++;
            if (put_seq(out, &o, cap, page + anchor, ip - anchor, ip - ref, m) < 0) return 0;
            ip += m;
            anchor = ip;
            continue;
        }
        ip++;
    }
    if (anchor < 4096 && put_seq(out, &o, cap, page + anchor, 4096 - anchor, 0, 0) < 0) return 0;
    return o;
}

static int get_len(const uint8_t* in, uint32_t len, uint32_t* i, uint32_t* n) {
    for (;;) {
        if (*i >= len) return -1;
        uint8_t b = in[(*i)++];
        *n += b;
        if (b != 255) return 0;
        if (*n > 8192) return -1;
    }
}

int zswap_unpack(const uint8_t* in, uint32_t len, uint8_t* page) {
    uint32_t i = 0, o = 0;
    while (i < len) {
        uint8_t t = in[i++];
        uint32_t nlit = t >> 4, ml = t & 15;
        if (nlit == 15 && get_len(in, len, &i, &nlit) < 0) return -1;
        if (nlit > len - i || nlit > 4096 - o) return -1;
        memcpy(page + o, in + i, nlit);
        i += nlit;
        o += nlit;
        if (i == len) break;                  /* (the last sequence: literals only) */
        if (len - i < 2) return -1;
        uint32_t off = (uint32_t)in[i] | (uint32_t)in[i + 1] << 8;
        i += 2;
        if (ml == 15 && get_len(in, len, &i, &ml) < 0) return -1;
        uint32_t m = ml + 4;
        if (!off || off > o || m > 4096 - o) return -1;
        for (uint32_t k = 0; k < m; k++, o++) page[o] = page[o - off];
    }
    return o == 4096 ? 0 : -1;
}

/* ── when to compress ──────────────────────────────────────────────────
 * The RAM the system can still give: the kernel heap's free bytes and,
 * 64-bit, the frames above 4 GiB. Below 1/8 of all of it free (after the
 * file cache had its turn: kernel/fs.c), apps' pages not used lately are
 * compressed until 1/5 is free again. */

#ifdef __x86_64__
static void ram(uint64_t* total, uint64_t* free) {
    *total = (uint64_t)kheap_total_bytes() + hmem_total_bytes();
    *free = (uint64_t)(kheap_total_bytes() - kheap_used_bytes()) + (hmem_total_bytes() - hmem_used_bytes());
}

static void zswap_task(void) {
    task_set_background();
    int busy = 0;
    for (;;) {
        task_sleep_ms(busy ? 10 : 100);                  /* (an app filling memory fast: kept up with) */
        busy = 0;
        if (!g_on) continue;
        uint64_t total, free;
        ram(&total, &free);
        int low = free < total / 8, big = g_nslots && g_st.bytes > total / 4;
        if (!low && !big) continue;
        busy = 1;
        uint32_t done = 0, wb = 0;
        if (low) {
            uint32_t pages = (uint32_t)((total / 5 - free) / 4096u);
            for (int pass = 0; pass < 3 && done < pages; pass++) {
                uint32_t n = uvm_page_out_all(0, pages - done);
                done += n;
                if (!n && pass) break;            /* (first pass: the accessed bits were cleared) */
            }
            ram(&total, &free);
        }
        if (g_nslots && g_st.pages && (free < total / 8 || g_st.bytes > total / 4)) {
            /* still short, or much compressed: the oldest compressed pages to the disk */
            uint64_t want = free < total / 5 ? total / 5 - free : 0;
            uint64_t over = g_st.bytes > total / 8 ? g_st.bytes - total / 8 : 0;
            uint64_t bytes = want > over ? want : over, per = g_st.bytes / g_st.pages + 1;
            uint64_t n = bytes / per + 1;
            wb = zswap_writeback(n > 1024 ? 1024 : (uint32_t)n);
        }
        static uint32_t said, since_said, since_wb;      /* (a line every 5 s at most) */
        since_said += done;
        since_wb += wb;
        if ((since_said || since_wb) && timer_ms() - said > 5000u) {
            klog("zswap: memory low - %u KiB of app memory compressed, %u KiB written to the disk\n",
                 since_said * 4u, since_wb * 4u);
            said = timer_ms();
            since_said = since_wb = 0;
        }
    }
}
#endif

#ifdef __x86_64__
static void reserve_entries(void);
#endif

void zswap_start(void) {
#ifdef __x86_64__
    reserve_entries();
    task_create("zswap", zswap_task);
#endif
}

uint32_t zswap_compress_now(void) {
    return uvm_page_out_all(1, 0xFFFFFFFFu);
}

/* a page fault found no memory (kernel/app.c): what can be compressed or
 * written out now goes - pages used lately too - or, nothing to give, a
 * moment is waited for the others; the pages freed - by it, or meanwhile by
 * the zswap task (which, busy with it already, keeps the others out) */
uint32_t zswap_reclaim(void) {
    uint64_t before = g_st.pages_out + g_st.disk_out;
    uint32_t n = uvm_page_out_all(0, 256);
    if (!n) n = uvm_page_out_all(0, 256);          /* (the first pass cleared the accessed bits) */
    if (g_nslots && g_st.pages) n += zswap_writeback(1024);   /* (small: a few sectors each) */
    if (!n) {
        task_sleep_ms(20);
        uint64_t after = g_st.pages_out + g_st.disk_out;
        if (after > before) n = (uint32_t)(after - before);
    }
    return n;
}

/* ── straight to the disk (kernel/uspace.c: page-out with the heap too
 * short for the compressed copy) ─────────────────────────────────────── */

static uint8_t g_put_buf[4096];                    /* (page-out's: one at a time, uspace.c) */

uint32_t zswap_disk_put(const uint8_t* src, uint32_t len) {
    if (!g_nslots || !len || len > 4096) return 0;
    uintptr_t f = spin_lock_irq(&g_zlock);
    uint32_t slot = slot_get(nsect(len));
    spin_unlock_irq(&g_zlock, f);
    if (!slot) return 0;
    memcpy(g_put_buf, src, len);
    memset(g_put_buf + len, 0, (512u - len % 512u) % 512u);
    if (fsdisk_swap_io(1, g_sw_lba + (slot - 1u), (len + 511u) / 512u, g_put_buf) != 0) {
        f = spin_lock_irq(&g_zlock);
        slot_put(slot, nsect(len));
        spin_unlock_irq(&g_zlock, f);
        return 0;
    }
    return slot;
}

void zswap_store_disk(uint32_t idx, uint32_t slot, uint32_t len) {
    zent_t* e = zswap_ent(idx);
    if (!e) return;
    uintptr_t f = spin_lock_irq(&g_zlock);
    e->state = ZS_DISK;
    e->slot = slot;
    e->len = len;
    e->data = NULL;
    e->frame = 0;
    g_st.disk_pages++;
    g_st.disk_out++;
    g_st.pages_out++;
    spin_unlock_irq(&g_zlock, f);
}

void zswap_slot_free(uint32_t slot, uint32_t len) {
    uintptr_t f = spin_lock_irq(&g_zlock);
    slot_put(slot, nsect(len));
    spin_unlock_irq(&g_zlock, f);
}

#ifdef __x86_64__
/* entries for 64 MiB of pages from the start, made while there is memory
 * (the first pages are compressed when there is none) */
static void reserve_entries(void) {
    for (uint32_t k = 0; k < 16384u / EBLK; k++)
        if (add_block() != 0) return;
}
#endif
