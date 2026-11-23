/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZSWAP_H
#define ZSWAP_H

#include "types.h"

/*
 * Compressed memory, a swap in RAM (64-bit kernel). When memory runs low,
 * pages of apps' mmap regions (a Java heap...) that were not used lately
 * are compressed into the kernel heap and their page goes back to the
 * system (kernel/upage.c); touching one again decompresses it in the page
 * fault (kernel/uspace.c), on any core. An all-zero page takes no room.
 * Windows 10 and Linux (zram, zswap) do the same before - or instead of -
 * writing pages to a disk.
 *
 * A compressed page is an entry of a table: its page table entry holds the
 * entry's number. While a page is being compressed the entry still holds
 * the page itself (PENDING): a touch then simply puts it back.
 */

#define ZS_FREE      0
#define ZS_PENDING   1      /* taken out of the app's tables, being compressed: frame is the page */
#define ZS_STORED    2      /* data / len hold it (len 0: a page of zeros) */
#define ZS_CANCELLED 3      /* the page went back (touched) or away (unmapped) while pending */
#define ZS_WRITING   4      /* being written to the swap area (its data still here) */
#define ZS_DISK      5      /* in the swap area: slot, len (its RAM freed) */
#define ZS_READING   6      /* being read back from the swap area */
#define ZS_DEAD      7      /* unmapped while being written or read: the one doing it frees it */
#define ZS_TAKEN     8      /* touched while being written: decompressed already, the writer frees it */

typedef struct {                        /* (32 bytes: 128 to a page) */
    uint8_t*  data;
    uintptr_t frame;
    uint32_t  len : 16;
    uint32_t  state : 8;
    uint32_t  in_lru : 8;
    uint32_t  slot;                     /* ZS_DISK: its first sector in the swap area + 1 (as many as len needs) */
    uint32_t  prev, next;               /* STORED with data: the oldest first (what goes to the disk) */
} zent_t;

/* a new entry (PENDING, holding frame): its number (>= 1), or 0 */
uint32_t zswap_new(uintptr_t frame);
zent_t*  zswap_ent(uint32_t idx);
void     zswap_free(uint32_t idx);        /* the entry and its data */
/* its page unmapped or its app gone: the entry goes, whatever it is in */
void     zswap_release(uint32_t idx);
/* a PENDING entry gets its compressed data (kmalloc'd, owned from now on;
 * NULL and 0 for a page of zeros): STORED */
void     zswap_store(uint32_t idx, uint8_t* data, uint32_t len);
/* an entry back into page, and the entry freed: 0; -1 (damaged); 2 if it
 * is on the disk (zswap_disk_in first: it may wait) */
int      zswap_take(uint32_t idx, uint8_t* page);
/* the swap area on the installed disk (64 MiB at least, the RAM's size at
 * most): 0, or -1 if there is none to be had */
int      zswap_disk_init(void);
uint64_t zswap_disk_bytes(void);
/* the oldest compressed pages to the disk, up to max: how many went */
uint32_t zswap_writeback(uint32_t max);
/* a page on the disk back into RAM (compressed): on the boot core,
 * interrupts on - it waits for the disk. 0, or -1 */
int      zswap_disk_in(uint32_t idx);

/* 4096 bytes -> out (cap bytes): the compressed size, or 0 if it did not
 * fit (a page that does not compress); zero pages: *zero = 1, returns 0.
 * ht: 4096 uint16_t of work space */
uint32_t zswap_pack(const uint8_t* page, uint8_t* out, uint32_t cap, int* zero, uint16_t* ht);
/* back into a 4096-byte page: 0, or -1 if the data is damaged */
int      zswap_unpack(const uint8_t* in, uint32_t len, uint8_t* page);

/* the background task: compresses when RAM runs low */
void     zswap_start(void);
/* every app page not in use right now compressed (the `swap now` command,
 * selftest): how many were */
uint32_t zswap_compress_now(void);
/* page-out with no heap for a compressed copy (kernel/uspace.c): len bytes
 * written to a free slot of the swap area (its number, or 0); an entry made
 * DISK with it; a slot given back unused */
uint32_t zswap_disk_put(const uint8_t* src, uint32_t len);
void     zswap_store_disk(uint32_t idx, uint32_t slot, uint32_t len);
void     zswap_slot_free(uint32_t slot, uint32_t len);
/* a page fault found no memory: some made now (compressed, written out), or
 * a moment waited - on the boot core, interrupts on; the pages freed */
uint32_t zswap_reclaim(void);
/* on (1, the default) or off: when off, nothing new is compressed */
void     zswap_enable(int on);
int      zswap_enabled(void);

/* what is compressed: pages, the bytes they take, and how often a page was
 * brought back */
typedef struct {
    uint32_t pages, zero_pages;
    uint64_t bytes;                       /* the compressed data */
    uint64_t pages_out, pages_in;         /* since boot */
    uint32_t disk_pages;                  /* in the swap area on the disk */
    uint64_t disk_out, disk_in;           /* since boot */
    uint64_t disk_used;                   /* bytes of the swap area taken (by the sector) */
    uint32_t entries;                     /* entries made (each page compressed or on the disk takes one) */
} zswap_stats_t;
void zswap_stats(zswap_stats_t* s);
/* the entries counted one by one: those on the disk (*disk_ents) against the
 * stats' disk_pages (*disk_counted), the swap area's bytes their slots hold
 * (*held) against those taken (*used) - equal unless an entry leaked its slot
 * or the count drifted (selftest) */
void zswap_audit(uint32_t* disk_ents, uint32_t* disk_counted, uint64_t* held, uint64_t* used);

#endif
