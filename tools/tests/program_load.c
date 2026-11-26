/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual prepare()/pin_program(): force reclaim during every allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { int used,loaded,pins; char* content; uint32_t size; } fs_file_t;
typedef struct { int used,id,seq,pgid,sid; void* us; void* mem; char name[64]; char exe[128]; } app_proc_t;
#define APP_MAX 2
static app_proc_t g_procs[APP_MAX];
static int g_seq,checks,fail_at,allocations,io_error,missing,load_calls;
static fs_file_t file;
static char bytes[]="\177ELF-native-program";
#define CHECK(c) do { checks++; if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);} }while(0)
static void api_init(void) {}
/* (users: who starts it - here no app, and it may run the program unless told otherwise) */
static int no_exec;
static app_proc_t* cur(void) { return NULL; }
static void env_for_session(void) {}
static int fs_access_entry(int is_dir, int idx, unsigned want) { (void)is_dir; (void)idx; (void)want; return !no_exec; }
static void creds_inherit(app_proc_t* p, const app_proc_t* parent) { (void)p; (void)parent; }
static int fs_find_file(const char* p) { CHECK(!strcmp(p,"/bin/probe"));return missing?-1:0; }
static void fs_file_path(int i,char* b,int n) { CHECK(i==0);snprintf(b,(size_t)n,"/bin/probe"); }
static void fs_pin(int i) { if(i==0)file.pins++; }
static void fs_unpin(int i) { if(i==0){CHECK(file.pins>0);file.pins--;} }
static fs_file_t* fs_get_file(int i) {
    CHECK(i==0);
    file.content=bytes;
    file.loaded=!io_error;
    return &file; /* Mirrors fs_get_file's unloaded entry on a disk I/O error. */
}
static size_t kstrlcpy(char* d,const char* s,size_t n) {
    size_t len=strlen(s);
    if(n){size_t k=len<n-1?len:n-1;memcpy(d,s,k);d[k]=0;}
    return len;
}
static void reclaim(void) {
    if(!file.pins){file.content=NULL;file.loaded=0;}
}
static void* allocation(void) {
    reclaim();
    return ++allocations==fail_at?NULL:malloc(1);
}
static void* uspace_create(void) { return allocation(); }
static void* umem_create(void* us) { CHECK(us!=NULL);return allocation(); }
static int fd_std(app_proc_t* p) { CHECK(p->us && p->mem);reclaim();return ++allocations==fail_at?-1:0; }
static void discard(app_proc_t* p) { free(p->us);free(p->mem);p->us=p->mem=NULL; }
static int load_program(app_proc_t* p,const uint8_t* b,uint32_t n,int argc,char** argv,char** envp,char* err,int cap) {
    (void)p;(void)argc;(void)argv;(void)envp;(void)err;(void)cap;
    load_calls++;
    CHECK(file.pins==1 && file.loaded && b==(uint8_t*)bytes && n==sizeof(bytes));
    CHECK(!memcmp(b,"\177ELF",4));
    reclaim();
    return ++allocations==fail_at?-1:0;
}
#include "program_pin.inc"
#include "program_prepare.inc"
int main(void) {
    char err[128],*argv[]={"probe",NULL};
    for(int fault=0;fault<=4;fault++) {
        memset(g_procs,0,sizeof(g_procs));
        file=(fs_file_t){.used=1,.size=sizeof(bytes)};
        fail_at=fault;allocations=0;load_calls=0;
        app_proc_t* p=prepare("/bin/probe",1,argv,NULL,err,sizeof(err));
        CHECK((p!=NULL)==(fault==0));CHECK(file.pins==0);
        if(fault==0){                          /* a program the caller may not run: refused, nothing claimed */
            no_exec=1;
            app_proc_t* q=prepare("/bin/probe",1,argv,NULL,err,sizeof(err));
            CHECK(q==NULL && !strcmp(err,"permission denied") && file.pins==0);
            no_exec=0;
        }
        if(p){CHECK(p->used && !strcmp(p->name,"probe"));discard(p);}
        else CHECK(!g_procs[0].us && !g_procs[0].mem);
    }
    memset(g_procs,0,sizeof(g_procs));
    for(int kind=0;kind<3;kind++) {
        file=(fs_file_t){.used=kind!=1,.size=sizeof(bytes)};
        io_error=kind==0;missing=kind==2;allocations=load_calls=0;fail_at=0;
        CHECK(!prepare("/bin/probe",1,argv,NULL,err,sizeof(err)));
        CHECK(file.pins==0 && allocations==0 && load_calls==0);
    }
    printf("Program loading: %d checks passed\n",checks);
}
