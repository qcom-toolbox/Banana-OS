/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/fileops.h"
#define MAX 512
static struct { char path[FS_PATH_LEN];fs_dir_t info; } dirs[MAX];
static struct { char path[FS_PATH_LEN];fs_file_t info;unsigned data; } files[MAX];
static int nd,nf,checks,live,allocs,fail_alloc=-1,fail_copy,fail_list,oversized,deleted,occupied;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void* kmalloc(size_t n) { if(allocs++==fail_alloc)return NULL;void* p=malloc(n?n:1);if(p)live++;return p; }
void kfree(void* p) { if(p){CHECK(live>0);live--;free(p);} }
int fs_find_dir(const char* p) { for(int i=0;i<nd;i++)if(dirs[i].info.used&&!strcmp(p,dirs[i].path))return i;return -1; }
int fs_find_file(const char* p) { if(occupied&&!strncmp(p,"/block/",7))return 123;for(int i=0;i<nf;i++)if(files[i].info.used&&!strcmp(p,files[i].path))return i;return -1; }
int fs_mkdir(const char* p) { if(fs_find_dir(p)>=0||nd>=MAX)return -1;int i=nd++;snprintf(dirs[i].path,sizeof(dirs[i].path),"%s",p);snprintf(dirs[i].info.name,FS_NAME_LEN,"%s",fileops_base(p));dirs[i].info.used=1;return i; }
static int add_file(const char* p,unsigned data) { CHECK(nf<MAX);int i=nf++;snprintf(files[i].path,FS_PATH_LEN,"%s",p);snprintf(files[i].info.name,FS_NAME_LEN,"%s",fileops_base(p));files[i].info.used=1;files[i].data=data;return i; }
const fs_dir_t* fs_get_dir(int i) { return i>=0&&i<nd&&dirs[i].info.used?&dirs[i].info:NULL; }
fs_file_t* fs_file_info(int i) { return i>=0&&i<nf&&files[i].info.used?&files[i].info:NULL; }
static int child(const char* root,const char* p) { size_t n=strlen(root);return !strncmp(root,p,n)&&p[n]=='/'&&p[n+1]&&!strchr(p+n+1,'/'); }
int fs_list_files(const char* p,int* out,int max) { if(fail_list==1)return -1;if(oversized==1)return FS_LIST_MAX+1;int n=0;for(int i=0;i<nf;i++)if(files[i].info.used&&child(p,files[i].path)){if(n<max)out[n]=i;n++;}return n; }
int fs_list_dirs(const char* p,int* out,int max) { if(fail_list==2)return -1;if(oversized==2)return FS_LIST_MAX+1;int n=0;for(int i=0;i<nd;i++)if(dirs[i].info.used&&child(p,dirs[i].path)){if(n<max)out[n]=i;n++;}return n; }
int fs_copy(const char* s,const char* d) { int i=fs_find_file(s);if(i<0||fail_copy||fs_find_file(d)>=0)return -1;return add_file(d,files[i].data); }
int fs_move(const char* s,const char* d) { (void)s;(void)d;return -1; }
int fs_path_mount(const char* p) { return !strncmp(p,"/usb",4); }
int fs_io_error(void) { return 0; }
void fs_delete(const char* p,int recursive) { deleted++;for(int i=0;i<nd;i++)if(!strcmp(p,dirs[i].path)||(recursive&&!strncmp(p,dirs[i].path,strlen(p))&&dirs[i].path[strlen(p)]=='/'))dirs[i].info.used=0;for(int i=0;i<nf;i++)if(!strcmp(p,files[i].path)||(recursive&&!strncmp(p,files[i].path,strlen(p))&&files[i].path[strlen(p)]=='/'))files[i].info.used=0; }
static void setup(int children) {
 CHECK(live==0);memset(dirs,0,sizeof(dirs));memset(files,0,sizeof(files));nd=nf=0;allocs=0;fail_alloc=-1;fail_copy=fail_list=oversized=deleted=occupied=0;
 CHECK(fs_mkdir("/src")>=0);CHECK(fs_mkdir("/usb")>=0);add_file("/src/root",123);
 for(int i=0;i<children;i++){char p[128];snprintf(p,sizeof(p),"/src/d%03d",i);CHECK(fs_mkdir(p)>=0);snprintf(p,sizeof(p),"/src/d%03d/data",i);add_file(p,1000u+(unsigned)i);}
}
static int transfer(void) { char out[512],err[128];return fileops_transfer("/src","/usb",0,out,sizeof(out),err,sizeof(err)); }
int main(void) {
 setup(33);CHECK(transfer()==0);CHECK(fs_find_dir("/src")<0&&deleted==1);
 for(int i=0;i<33;i++){char p[128];snprintf(p,sizeof(p),"/usb/src/d%03d/data",i);int f=fs_find_file(p);CHECK(f>=0&&files[f].data==1000u+(unsigned)i);}
 CHECK(fs_find_dir("/src")<0&&deleted==1&&live==0);
 for(int count=0;count<=64;count++){setup(count);CHECK(transfer()==0);CHECK(fs_find_dir("/src")<0&&live==0);for(int i=0;i<count;i++){char p[128];snprintf(p,sizeof(p),"/usb/src/d%03d/data",i);CHECK(fs_find_file(p)>=0);}}
 for(int fail=0;fail<5;fail++){
  setup(33);if(fail==0)fail_copy=1;else if(fail<3)fail_list=fail;else oversized=fail-2;
  CHECK(transfer()<0);CHECK(fs_find_dir("/src")>=0&&fs_find_file("/src/d032/data")>=0&&deleted==0&&live==0);
 }
 setup(3);CHECK(transfer()==0);int total=allocs;
 for(int at=0;at<total;at++){setup(3);fail_alloc=at;CHECK(transfer()<0);CHECK(fs_find_dir("/src")>=0&&deleted==0&&live==0);}
 setup(0);CHECK(fileops_copy_tree("/src","/usb/deep",13)<0);CHECK(deleted==0&&live==0);
 /* Exhausted candidate names and path truncation must never return a
  * destination that callers could overwrite. */
 setup(0);char out[512];occupied=1;
 CHECK(fileops_unique("/block","file.txt",out,sizeof(out))<0&&out[0]==0);
 CHECK(fileops_unique("/block","file.txt",out,5)<0&&out[0]==0);
 CHECK(fileops_unique("/usb","file.txt",NULL,0)<0);
 occupied=0;CHECK(fileops_unique("/usb","file.txt",out,sizeof(out))==0&&!strcmp(out,"/usb/file.txt"));
 add_file("/usb/file.txt",777);CHECK(fileops_unique("/usb","file.txt",out,sizeof(out))==0&&!strcmp(out,"/usb/file (2).txt"));
 CHECK(fileops_unique("/usb","file.txt",out,0)<0);
 CHECK(files[fs_find_file("/usb/file.txt")].data==777);
 printf("File operations: %d checks passed\n",checks);return 0;
}
