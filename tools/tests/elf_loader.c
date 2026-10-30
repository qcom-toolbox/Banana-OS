/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define APP_MAX_IMAGE (64u<<20)
#ifdef __x86_64__
#define BANANA_ARCH "x86_64"
#else
#define BANANA_ARCH "i686"
#endif
typedef void (*banana_entry_t)(void);
typedef struct { int unused; } umem_t;
static unsigned checks,live;
static int fail_allocation;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void* kmalloc(size_t n) { CHECK(n<=APP_MAX_IMAGE+4096u);if(fail_allocation)return NULL;void* p=malloc(n?n:1);if(p){live++;memset(p,0x5a,n);}return p; }
static void kfree(void* p) { if(p){CHECK(live>0);live--;free(p);} }
static void* umem_alloc_pages(umem_t* m,size_t n,size_t guard) {
    (void)m;(void)guard;void* p=NULL;CHECK(n<=APP_MAX_IMAGE);if(fail_allocation)return NULL;
    if(posix_memalign(&p,4096,(n+4095)&~(size_t)4095))return NULL;
    live++;return p;
}
static void umem_free_pages(umem_t* m,void* p,size_t guard) { (void)m;(void)guard;kfree(p); }
static size_t kstrlcpy(char* d,const char* s,size_t n) {
    size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;
}
static int ksnprintf(char* d,size_t n,const char* fmt,...) {
    va_list ap;va_start(ap,fmt);int rc=vsnprintf(d,n,fmt,ap);va_end(ap);return rc;
}
#include "elf_loader.inc"
static uint8_t* seed(void) {
    uint8_t* b=calloc(1,4096);CHECK(b);
    ehdr_t e={0};memcpy(e.ident,"\177ELF",4);e.ident[4]=ELF_CLASS;e.ident[5]=e.ident[6]=1;
    e.type=3;e.machine=EM_HOST;e.version=1;e.ehsize=sizeof(e);e.phoff=sizeof(e);e.phentsize=sizeof(phdr_t);e.phnum=2;e.entry=0x1300;
    memcpy(b,&e,sizeof(e));
    phdr_t p={.type=PT_LOAD,.flags=7,.offset=0,.vaddr=0x1000,.filesz=4096,.memsz=4096,.align=4096};
    memcpy(b+e.phoff,&p,sizeof(p));
    p=(phdr_t){.type=PT_DYNAMIC,.flags=6,.offset=0x400,.vaddr=0x1400,.filesz=0x100,.memsz=0x100,.align=sizeof(uintptr_t)};
    memcpy(b+e.phoff+sizeof(p),&p,sizeof(p));
#ifdef __x86_64__
    dyn_t d[]={{DT_RELA,0x1600},{DT_RELASZ,24},{DT_RELAENT,24},{DT_SYMTAB,0x1700},{DT_SYMENT,24},{DT_STRTAB,0x1800},{DT_NULL,0}};
    uint64_t rel[]={0x1900,8,0x123};memcpy(b+0x600,rel,sizeof(rel));
#else
    dyn_t d[]={{DT_REL,0x1600},{DT_RELSZ,8},{DT_RELENT,8},{DT_SYMTAB,0x1700},{DT_SYMENT,16},{DT_STRTAB,0x1800},{DT_NULL,0}};
    uint32_t rel[]={0x1900,8},v=0x123;memcpy(b+0x600,rel,sizeof(rel));memcpy(b+0x900,&v,4);
#endif
    memcpy(b+0x400,d,sizeof(d));return b;
}
static int load(uint8_t* b,uint32_t n,int expect) {
    uint8_t* raw=NULL;banana_entry_t entry=NULL;uintptr_t base=0,lo=0,hi=0;char err[96];
    int rc=load_elf(NULL,b,n,&raw,&entry,&base,&lo,&hi,0,err,sizeof(err));
    if(expect>=0)CHECK((rc==0)==expect);
    if(expect==0)CHECK(rc==-8);
    if(!rc){CHECK(raw && (uintptr_t)entry>=lo && (uintptr_t)entry<hi);kfree(raw);}
    CHECK(live==0);return rc;
}
static void malformed_headers(void) {
    uint8_t* b=seed();ehdr_t* e=(ehdr_t*)b;
    e->phoff=(__typeof__(e->phoff))-(int)sizeof(phdr_t);e->phnum=2;
    CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
    for(unsigned n=0;n<sizeof(ehdr_t);n++){b=seed();CHECK(elf_span(b,n)==0);load(b,n,0);free(b);}
    b=seed();e=(ehdr_t*)b;e->ident[5]=2;CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
    b=seed();e=(ehdr_t*)b;e->entry=0x9000;CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
}
static void malformed_segments(void) {
    uint8_t* b=seed();ehdr_t* e=(ehdr_t*)b;phdr_t* p=(phdr_t*)(b+e->phoff);
    p->offset=(__typeof__(p->offset))-32;p->filesz=64;p->memsz=64;
    CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
    b=seed();e=(ehdr_t*)b;p=(phdr_t*)(b+e->phoff);
    p->vaddr=(__typeof__(p->vaddr))-16;p->memsz=64;p->filesz=64;
    CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
    b=seed();e=(ehdr_t*)b;p=(phdr_t*)(b+e->phoff);p[1].vaddr=0x40000000;
    CHECK(elf_span(b,4096)==0);load(b,4096,0);free(b);
}
static void malformed_dynamic(void) {
    uint8_t* b=seed();dyn_t* d=(dyn_t*)(b+0x400);
    for(int i=0;i<0x100/(int)sizeof(*d);i++)d[i]=(dyn_t){0x123,0};
    load(b,4096,0);free(b);
    b=seed();d=(dyn_t*)(b+0x400);d[0].val=(__typeof__(d[0].val))-8;d[1].val=24;load(b,4096,0);free(b);
    b=seed();d=(dyn_t*)(b+0x400);d[1].val=(__typeof__(d[1].val))-1;load(b,4096,0);free(b);
    b=seed();d=(dyn_t*)(b+0x400);d[4].val=~(uintptr_t)0;load(b,4096,0);free(b);
}
static void relocation_contracts(void) {
    for(int staged=0;staged<2;staged++){
        uint8_t* b=seed(),*raw=NULL;banana_entry_t entry;
        uintptr_t base,lo,hi;char err[96];umem_t m={0};
        fail_allocation=1;
        CHECK(load_elf(staged?&m:NULL,b,4096,&raw,&entry,&base,&lo,&hi,0,err,sizeof(err))==-12);
        CHECK(live==0);fail_allocation=0;
        CHECK(!load_elf(staged?&m:NULL,b,4096,&raw,&entry,&base,&lo,&hi,
                       staged?(uintptr_t)0x50000000:0,err,sizeof(err)));
        uint8_t* image=(uint8_t*)(((uintptr_t)raw+4095)&~(uintptr_t)4095);
#ifdef __x86_64__
        uint64_t value;memcpy(&value,image+0x900,8);
        CHECK(value==(uint64_t)base+0x123);
#else
        uint32_t value;memcpy(&value,image+0x900,4);
        CHECK(value==(uint32_t)base+0x123);
#endif
        CHECK((uintptr_t)entry==base+0x1300 && hi-lo==4096);
        if(staged)umem_free_pages(&m,raw,0);else kfree(raw);free(b);

        /* A four-byte PC-relative relocation may legally end at the last
         * four bytes of the image and may be unaligned on x86. */
        b=seed();raw=NULL;
#ifdef __x86_64__
        uint64_t rel[]={0x1ffc,(1ull<<32)|2,(uint64_t)-4};
        uint16_t sh=1;uint64_t val=0x1200;
        memcpy(b+0x600,rel,sizeof(rel));memcpy(b+0x700+24+6,&sh,2);memcpy(b+0x700+24+8,&val,8);
#else
        uint32_t rel[]={0x1ffc,(1u<<8)|2},val=0x1200,add=(uint32_t)-4;uint16_t sh=1;
        memcpy(b+0x600,rel,sizeof(rel));memcpy(b+0x700+16+14,&sh,2);memcpy(b+0x700+16+4,&val,4);memcpy(b+0xffc,&add,4);
#endif
        CHECK(!load_elf(staged?&m:NULL,b,4096,&raw,&entry,&base,&lo,&hi,
                       staged?(uintptr_t)0x50000000:0,err,sizeof(err)));
        image=(uint8_t*)(((uintptr_t)raw+4095)&~(uintptr_t)4095);
        uint32_t pc;memcpy(&pc,image+0xffc,4);
        CHECK(pc==(uint32_t)(0x1200u-4u-0x1ffcu));
        if(staged)umem_free_pages(&m,raw,0);else kfree(raw);free(b);

        b=seed();dyn_t* d=(dyn_t*)(b+0x400);d[5].val=0x1ffe;b[0xffe]='X';b[0xfff]='Y';
#ifdef __x86_64__
        uint64_t missing[]={0x1900,(1ull<<32)|6,0};memcpy(b+0x600,missing,sizeof(missing));
#else
        uint32_t missing[]={0x1900,(1u<<8)|6};memcpy(b+0x600,missing,sizeof(missing));
#endif
        CHECK(load_elf(staged?&m:NULL,b,4096,&raw,&entry,&base,&lo,&hi,0,err,sizeof(err))<0);
        CHECK(strstr(err,"needs XY,") && live==0);free(b);
    }
}

static uint32_t random_state=0x12345678;
static uint32_t random32(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static void fuzz(void) {
    for(int i=0;i<20000;i++){
        uint8_t* b=seed();
        for(int j=0;j<1+(int)(random32()%8);j++){
            unsigned off=random32()%2048;unsigned n=1+random32()%8;if(off+n>4096)n=4096-off;
            for(unsigned k=0;k<n;k++)b[off+k]=(uint8_t)random32();
        }
        uint32_t n=random32()%2?4096:random32()%4096;
        (void)elf_span(b,n);load(b,n,-1);free(b);
    }
}
int main(int argc,char** argv) {
    const char* mode=argc>1?argv[1]:"all";
    if(!strcmp(mode,"file")){
        CHECK(argc==3);FILE* f=fopen(argv[2],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));
        long n=ftell(f);CHECK(n>0 && n<128L*1024*1024);rewind(f);
        uint8_t* b=malloc((size_t)n);CHECK(b && fread(b,1,(size_t)n,f)==(size_t)n);fclose(f);
        CHECK(elf_span(b,(uint32_t)n)>0);load(b,(uint32_t)n,1);free(b);
        printf("ELF valid: %s\n",argv[2]);return 0;
    }
    if(!strcmp(mode,"all")||!strcmp(mode,"valid")){uint8_t* b=seed();CHECK(elf_span(b,4096)==4096);load(b,4096,1);free(b);}
    if(!strcmp(mode,"all")||!strcmp(mode,"relocations"))relocation_contracts();
    if(!strcmp(mode,"all")||!strcmp(mode,"headers"))malformed_headers();
    if(!strcmp(mode,"all")||!strcmp(mode,"segments"))malformed_segments();
    if(!strcmp(mode,"all")||!strcmp(mode,"dynamic"))malformed_dynamic();
    if(!strcmp(mode,"all")||!strcmp(mode,"fuzz"))fuzz();
    printf("ELF loader: %u checks passed\n",checks);return 0;
}
