/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/config.c"
static fs_file_t file;
static int exists,evict,io_error,fail_alloc,fail_write,writes,flushes,installed=1;
static unsigned checks,live;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void reset(const char* text) {
    free(file.content);memset(&file,0,sizeof(file));exists=text!=NULL;
    if(text){file.content=strdup(text);file.size=(uint32_t)strlen(text);file.cap=file.size+1;file.loaded=file.used=1;}
    evict=io_error=fail_alloc=fail_write=writes=flushes=0;
}
int fs_find_file(const char* path) { CHECK(!strcmp(path,"/etc/test.conf"));return exists?0:-1; }
fs_file_t* fs_get_file(int idx) {
    CHECK(idx==0 && exists);
    if(io_error==2)return NULL;
    if(io_error)file.loaded=0;
    return &file;
}
void fs_pin(int idx) { CHECK(idx==0);file.pins++; }
void fs_unpin(int idx) { CHECK(idx==0 && file.pins);file.pins--; }
void* kmalloc(size_t n) {
    if(fail_alloc)return NULL;
    if(evict && !file.pins && file.loaded && file.cap>=4096){free(file.content);file.content=NULL;file.loaded=0;}
    void* p=malloc(n);if(p)live++;return p;
}
void kfree(void* p) { if(p){CHECK(live>0);live--;free(p);} }
size_t kstrlcpy(char* dst,const char* src,size_t n) {
    size_t k=strlen(src);if(n){size_t c=k<n-1?k:n-1;memcpy(dst,src,c);dst[c]=0;}return k;
}
size_t kstrlcat(char* dst,const char* src,size_t n) {
    size_t d=strlen(dst),s=strlen(src);
    if(d<n){size_t c=s<n-d-1?s:n-d-1;memcpy(dst+d,src,c);dst[d+c]=0;}
    return d+s;
}
int fs_write_path(const char* p,const void* text,uint32_t n) {
    CHECK(!strcmp(p,"/etc/test.conf"));writes++;
    if(fail_write)return -1;
    char* next=malloc(n+1);CHECK(next);memcpy(next,text,n);next[n]=0;
    free(file.content);file.content=next;file.size=n;file.loaded=file.used=1;exists=1;return 0;
}
int fsdisk_is_installed(void) { return installed; }
int fsdisk_sync(void) { flushes++;return 0; }
static void reclaim_test(void) {
    char text[8192];
    memset(text,' ',sizeof(text));
    memcpy(text,"gpu3d=off\nother=keep\n",strlen("gpu3d=off\nother=keep\n"));
    text[sizeof(text)-2]='\n';text[sizeof(text)-1]=0;
    reset(text);evict=1;
    CHECK(cfg_set("/etc/test.conf","gpu3d","on","# header\n")==0);
    CHECK(!strncmp(file.content,"gpu3d=on\nother=keep\n",strlen("gpu3d=on\nother=keep\n")));
    CHECK(strlen(file.content)==strlen(text)-1 && !file.pins && live==0);
}
static void noop_test(void) {
    reset("gpu3d=off\nother=keep\n");
    for(int i=0;i<100;i++)CHECK(!cfg_set("/etc/test.conf","gpu3d","off",NULL));
    CHECK(writes==0 && !file.pins && live==0);
    CHECK(!cfg_set("/etc/test.conf","absent",NULL,NULL) && writes==0);
}
static void io_test(void) {
    for(int err=1;err<=2;err++){
        reset("gpu3d=off\nother=keep\n");io_error=err;
        CHECK(cfg_set("/etc/test.conf","gpu3d","on",NULL)==-1);
        CHECK(writes==0 && file.pins==0 && live==0);
    }
    reset("gpu3d=off\n");fail_alloc=1;
    CHECK(cfg_set("/etc/test.conf","gpu3d","on",NULL)==-1 && writes==0 && !file.pins);
    reset("gpu3d=off\n");fail_write=1;
    CHECK(cfg_set("/etc/test.conf","gpu3d","on",NULL)==-1 && !file.pins && live==0);
    CHECK(!strcmp(file.content,"gpu3d=off\n"));
}
int main(int argc,char** argv) {
    const char* mode=argc>1?argv[1]:"all";
    if(!strcmp(mode,"all")||!strcmp(mode,"reclaim"))reclaim_test();
    if(!strcmp(mode,"all")||!strcmp(mode,"noop"))noop_test();
    if(!strcmp(mode,"all")||!strcmp(mode,"io"))io_test();
    if(!strcmp(mode,"all")){
        reset(NULL);CHECK(!cfg_set("/etc/test.conf","missing",NULL,NULL)&&writes==0);
        CHECK(!cfg_set("/etc/test.conf","theme","dark","# test\n"));
        CHECK(!strcmp(file.content,"# test\ntheme=dark\n"));
        char out[64];CHECK(cfg_get("/etc/test.conf","theme",out,sizeof(out))&&!strcmp(out,"dark"));
        CHECK(!cfg_set("/etc/test.conf","theme",NULL,NULL)&&!strcmp(file.content,"# test\n"));
        reset(" # preserved\n  key=old\nnext=ok");
        CHECK(!cfg_set("/etc/test.conf","key","new",NULL));
        CHECK(!strcmp(file.content," # preserved\nkey=new\nnext=ok"));
        CHECK(cfg_persist()==1 && flushes==1);installed=0;CHECK(cfg_persist()==0 && flushes==1);
    }
    reset(NULL);CHECK(live==0);printf("Configuration: %u checks passed\n",checks);
}
