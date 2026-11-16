/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../kernel/types.h"
/* Host replacement for IRQ masking only; exercise the actual quota ledger. */
#define SPINLOCK_H
typedef struct { int v; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spin_lock(spinlock_t* l){while(__sync_lock_test_and_set(&l->v,1)){}}
static void spin_unlock(spinlock_t* l){__sync_lock_release(&l->v);}
static uintptr_t spin_lock_irq(spinlock_t* l){while(__sync_lock_test_and_set(&l->v,1)){}return 0;}
static void spin_unlock_irq(spinlock_t* l,uintptr_t f){(void)f;__sync_lock_release(&l->v);}
#include "../../kernel/umem.c"
extern int printf(const char*,...);
extern void exit(int);
static uint32_t mock_heap_used;
static uint32_t mock_heap_total=128u<<20;
uint32_t kheap_total_bytes(void){return mock_heap_total;}
uint32_t kheap_used_bytes(void){return mock_heap_used;}
static uint32_t mock_evictable;   /* file data cached from the disk: the heap evicts it when short */
uint32_t fs_evictable_bytes(void){return mock_evictable;}
static int checks;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
    umem_t a={0},b={0},ordinary={0};
    umem_web_pool_init();
    CHECK(g_pool_limit==(64u<<20)&&g_pool_used==0);
    mock_heap_used=32u<<20; /* loader image consumes memory after the snapshot */
    umem_web_pool_init();
    CHECK(g_pool_limit==(64u<<20)&&g_pool_used==0);
    a.reserved=8u<<20;b.reserved=8u<<20;
    CHECK(!umem_limit(&a,32u<<20));
    CHECK(!umem_limit(&b,64u<<20));
    CHECK(g_pool_used==(16u<<20));
    CHECK(umem_vm_charge(&a,24u<<20));
    CHECK(a.reserved==(32u<<20));
    CHECK(!umem_vm_charge(&a,1));
    CHECK(g_pool_used==(40u<<20));
    CHECK(umem_vm_charge(&b,24u<<20));
    CHECK(g_pool_used==(64u<<20));
    CHECK(!umem_vm_charge(&b,4096));
    CHECK(b.reserved==(32u<<20));
    umem_vm_uncharge(&a,8u<<20);
    CHECK(umem_vm_charge(&b,8u<<20));
    CHECK(g_pool_used==(64u<<20));
    CHECK(umem_vm_charge(&ordinary,0xffffffffu));
    CHECK(!ordinary.reserved&&g_pool_used==(64u<<20));
    for(int i=0;i<10000;i++){
        umem_vm_uncharge(&a,4096);
        CHECK(umem_vm_charge(&a,4096));
    }
    umem_vm_uncharge(&a,16u<<20);
    umem_vm_uncharge(&b,32u<<20);
    CHECK(g_pool_used==(16u<<20));
    CHECK(a.reserved==(8u<<20)&&b.reserved==(8u<<20));
    /* Large-memory machines get the higher cap, while the half-heap rule
     * remains authoritative on smaller machines. Exercise the actual ledger. */
    /* cached file data counts as free: 96 MiB used, 64 MiB of it evictable -> a 48 MiB pool, not 16 */
    g_pool_limit=0;g_pool_used=0;mock_heap_used=96u<<20;mock_evictable=64u<<20;
    CHECK(pool_take(0)&&g_pool_limit==(48u<<20));
    mock_evictable=0;
    g_pool_limit=0;g_pool_used=0;mock_heap_used=0;
    mock_heap_total=3u<<30;
    umem_web_pool_init();
    CHECK(g_pool_limit==(1024u<<20));
    umem_t large={0},peer={0};
    CHECK(!umem_limit(&large,512u<<20));
    CHECK(!umem_limit(&peer,512u<<20));
    CHECK(umem_vm_charge(&large,512u<<20));
    CHECK(!umem_vm_charge(&large,4096));
    CHECK(umem_vm_charge(&peer,512u<<20));
    CHECK(g_pool_used==(1024u<<20));
    umem_vm_uncharge(&large,512u<<20);
    umem_vm_uncharge(&peer,512u<<20);
    CHECK(g_pool_used==0);
    printf("PASS: %d shared direct/mmap memory quota checks\n",checks);
    return 0;
}
