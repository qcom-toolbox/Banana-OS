/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char* u32_to_str(uint32_t,char*,int);
#include "../../kernel/fs.c"
static int checks,source=-1,force_evict,pressure,evictions,read_fail,write_fail,alloc_fail;
static uint32_t now=1000;
static char pattern[8192],disk_bytes[4*1024*1024];static uint32_t disk_size;
static void* source_cache;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reclaim_now(void){if(force_evict){force_evict=0;now+=4000;pressure=1;fs_evict(16384,3000);pressure=0;}}
void* kmalloc(size_t n){reclaim_now();if(alloc_fail)return NULL;CHECK(n<8u*1024*1024);return malloc(n?n:1);}
void* kzalloc(size_t n){void* p=kmalloc(n);if(p)memset(p,0,n);return p;}
void* krealloc(void* p,size_t n){reclaim_now();if(alloc_fail)return NULL;CHECK(n<8u*1024*1024);return realloc(p,n?n:1);}
void kfree(void* p){if(p&&p==source_cache)evictions++;free(p);}
uint32_t kheap_largest_free(void){return pressure?0:16u*1024*1024;}
uint32_t timer_ms(void){return now;}
uint32_t rtc_now(void){return 1700000000;}
int* task_cwd_slot(void){static int slot;return &slot;}
void task_sleep_ms(uint32_t n){now+=n;}
const char* hostname_get(void){return "test";}
void klog(const char* fmt,...){(void)fmt;}
void terminal_write(const char* s){(void)s;}
void terminal_writeln(const char* s){(void)s;}
void terminal_write_color(const char* s,uint8_t a,uint8_t b){(void)s;(void)a;(void)b;}
void terminal_putchar(char c){(void)c;}
static int disk_read_mock(int idx,uint8_t* p,uint32_t n){CHECK(idx==source&&n==sizeof(pattern));if(read_fail)return -1;memcpy(p,pattern,n);return 0;}
static int clean_mock(int idx,uint32_t gen){(void)gen;return idx==source;}
static int mount_create(void* c,uint32_t p,const char* name,int dir,uint32_t* node){(void)c;(void)p;(void)name;(void)dir;*node=1;return 0;}
static int mount_read(void* c,uint32_t n,uint8_t* p,uint32_t len){(void)c;(void)n;if(read_fail)return -1;CHECK(len<=disk_size);memcpy(p,disk_bytes,len);return 0;}
static int mount_write(void* c,uint32_t n,const void* p,uint32_t len){(void)c;(void)n;if(write_fail)return -1;reclaim_now();CHECK(len<=sizeof(disk_bytes));memcpy(disk_bytes,p,len);disk_size=len;return 0;}
static int mount_append(void* c,uint32_t n,uint32_t off,const void* p,uint32_t len){(void)c;(void)n;reclaim_now();CHECK(off+len<=sizeof(disk_bytes));memcpy(disk_bytes+off,p,len);disk_size=off+len;return 0;}
static int mount_write_at(void* c,uint32_t n,uint32_t off,const void* p,uint32_t len){return mount_append(c,n,off,p,len);}
static int mount_truncate(void* c,uint32_t n,uint32_t size){(void)c;(void)n;reclaim_now();CHECK(size<=sizeof(disk_bytes));if(size>disk_size)memset(disk_bytes+disk_size,0,size-disk_size);disk_size=size;return 0;}
static int mount_remove(void* c,uint32_t n,int dir){(void)c;(void)n;(void)dir;return 0;}
static const fs_mount_ops_t ops={.read=mount_read,.write=mount_write,.append=mount_append,.create=mount_create,.remove=mount_remove};
static void setup(void){
 force_evict=pressure=alloc_fail=read_fail=write_fail=evictions=0;source=-1;source_cache=NULL;
 fs_init();
 for(unsigned i=0;i<sizeof(pattern);i++)pattern[i]=(char)((i*29u+17u)&255u);
 source=fs_create("/home/banana/source");CHECK(source>=0&&fs_write(source,pattern,sizeof(pattern))==0);
 source_cache=F(source).content;fs_set_disk_ops(disk_read_mock,NULL,clean_mock);
 CHECK(fs_mount("/mnt/usb",&ops,NULL,1)>0);
}
static void check_copy(int idx){CHECK(idx>=0&&F(idx).size==sizeof(pattern));CHECK(!memcmp(F(idx).content,pattern,sizeof(pattern)));CHECK(F(idx).content[sizeof(pattern)]==0);}
int main(int argc,char** argv){
 CHECK(argc==2);setup();
 if(!strcmp(argv[1],"copy")){
  int target=fs_create("/home/banana/dest");CHECK(target>=0);force_evict=1;
  CHECK(fs_copy("/home/banana/source","/home/banana/dest")==target);check_copy(target);
  CHECK(F(source).pins==0&&evictions==0);
  CHECK(fs_copy("/home/banana/source","/home/banana/source")==source);
  force_evict=1;int created=fs_copy("/home/banana/source","/mnt/usb/new-copy");
  check_copy(created);CHECK(F(source).pins==0&&evictions==0);
  CHECK(disk_size==sizeof(pattern)&&!memcmp(disk_bytes,pattern,sizeof(pattern)));
 }else if(!strcmp(argv[1],"move")){
  force_evict=1;int target=fs_move("/home/banana/source","/mnt/usb/moved");check_copy(target);
  CHECK(fs_find_file("/home/banana/source")<0&&disk_size==sizeof(pattern));
  CHECK(!memcmp(disk_bytes,pattern,sizeof(pattern)));
 }else if(!strcmp(argv[1],"overflow")){
  uint32_t old=F(source).size;char x='x';
  CHECK(fs_append(source,&x,0xffffffffu)<0);CHECK(F(source).size==old);
  CHECK(fs_write_at(source,old,&x,0xffffffffu)<0);CHECK(F(source).size==old);
 }else if(!strcmp(argv[1],"gap")){
  int f=fs_create("/home/banana/gap");CHECK(f>=0);
  CHECK(fs_write_at(f,4096,"abc",3)==0);CHECK(F(f).size==4099);
  for(int i=0;i<4096;i++)CHECK(F(f).content[i]==0);
  CHECK(!memcmp(F(f).content+4096,"abc",3));
  CHECK(fs_write_at(f,1,"Z",1)==0&&F(f).size==4099);
 }else if(!strcmp(argv[1],"alias")){
  fs_set_system_backend(&ops,NULL,1);F(source).node=2;
  memcpy(disk_bytes,pattern,sizeof(pattern));disk_size=sizeof(pattern);
  force_evict=1;CHECK(fs_write(source,F(source).content+3,sizeof(pattern)-3)==0);
  CHECK(F(source).size==sizeof(pattern)-3&&!memcmp(F(source).content,pattern+3,sizeof(pattern)-3));
  CHECK(fs_write(source,pattern,sizeof(pattern))==0);
  force_evict=1;CHECK(fs_append(source,F(source).content,F(source).size)==0);
  CHECK(F(source).size==2*sizeof(pattern)&&F(source).pins==0&&evictions==0);
  CHECK(!memcmp(F(source).content,pattern,sizeof(pattern)));
  CHECK(!memcmp(F(source).content+sizeof(pattern),pattern,sizeof(pattern)));
  char expected[8192];memcpy(expected,pattern,sizeof(expected));memmove(expected+4,expected,32);
  CHECK(fs_write_at(source,4,F(source).content,32)==0);
  CHECK(!memcmp(F(source).content,expected,sizeof(expected)));
  /* Force reserve to relocate an aliased source in the offset-write path. */
  fs_set_system_backend(NULL,NULL,0);
  CHECK(fs_write_at(source,32768,F(source).content+1,32)==0);
  CHECK(F(source).size==32800&&!memcmp(F(source).content+32768,expected+1,32));
  for(int i=16384;i<32768;i++)CHECK(F(source).content[i]==0);
 }else if(!strcmp(argv[1],"links")){
  int link=fs_link("/home/banana/source","/home/banana/hardlink");CHECK(link>=0);
  fs_set_system_backend(&ops,NULL,1);F(source).node=2;links_sync(source);
  memcpy(disk_bytes,pattern,sizeof(pattern));disk_size=sizeof(pattern);
  force_evict=1;CHECK(fs_append(link,F(source).content,sizeof(pattern))==0);
  CHECK(F(source).content==F(link).content&&F(source).size==2*sizeof(pattern));
  CHECK(!memcmp(F(source).content,pattern,sizeof(pattern)));
  CHECK(!memcmp(F(source).content+sizeof(pattern),pattern,sizeof(pattern)));
  CHECK(F(source).pins==0&&F(link).pins==0&&evictions==0);
  source_cache=F(source).content;force_evict=1;
  CHECK(fs_truncate(link,4096)==0);
  CHECK(F(source).size==4096&&F(source).content==F(link).content);
  CHECK(F(source).pins==0&&F(link).pins==0&&evictions==0);
 }else if(!strcmp(argv[1],"offset")){
  fs_mount_ops_t direct=ops;direct.write_at=mount_write_at;direct.truncate=mount_truncate;
  fs_set_system_backend(&direct,NULL,1);F(source).node=2;
  memcpy(disk_bytes,pattern,sizeof(pattern));disk_size=sizeof(pattern);
  force_evict=1;CHECK(fs_write_at(source,2,"XY",2)==0);
  CHECK(F(source).pins==0&&evictions==0&&F(source).loaded);
  CHECK(F(source).content[2]=='X'&&disk_bytes[3]=='Y');
  char expected[32];memcpy(expected,F(source).content+1,sizeof(expected));
  CHECK(fs_write_at(source,32768,F(source).content+1,sizeof(expected))==0);
  CHECK(F(source).size==32800&&disk_size==32800);
  CHECK(!memcmp(F(source).content+32768,expected,sizeof(expected)));
  CHECK(!memcmp(disk_bytes+32768,expected,sizeof(expected)));
  source_cache=F(source).content;evictions=0;
  force_evict=1;CHECK(fs_truncate(source,4096)==0);
  CHECK(F(source).pins==0&&evictions==0&&F(source).loaded&&F(source).size==4096&&disk_size==4096);
 }else if(!strcmp(argv[1],"stream")){
  fs_mount_ops_t streaming=ops;streaming.write_at=mount_write_at;
  fs_set_system_backend(&streaming,NULL,1);F(source).node=2;
  char* big=malloc(APPEND_STREAM_MIN);CHECK(big!=NULL);memset(big,'S',APPEND_STREAM_MIN);
  CHECK(fs_write(source,big,APPEND_STREAM_MIN)==0);
  CHECK(fs_write_at(source,APPEND_STREAM_MIN,"tail",4)==0);
  CHECK(!F(source).loaded&&F(source).pins==0&&F(source).size==APPEND_STREAM_MIN+4);
  CHECK(disk_size==APPEND_STREAM_MIN+4&&!memcmp(disk_bytes+APPEND_STREAM_MIN,"tail",4));
  CHECK(fs_get_file(source)!=NULL&&F(source).loaded);
  force_evict=1;CHECK(fs_append(source,F(source).content,128)==0);
  CHECK(F(source).loaded&&F(source).pins==0&&F(source).size==APPEND_STREAM_MIN+132);
  for(unsigned i=0;i<128;i++)CHECK(F(source).content[APPEND_STREAM_MIN+4+i]=='S');
  CHECK(!memcmp(disk_bytes,F(source).content,F(source).size));
  free(big);
 }else if(!strcmp(argv[1],"write")){
  fs_set_system_backend(&ops,NULL,1);F(source).node=2;
  memcpy(disk_bytes,pattern,sizeof(pattern));disk_size=sizeof(pattern);
  force_evict=1;CHECK(fs_write(source,pattern,sizeof(pattern))==0);CHECK(F(source).pins==0&&evictions==0);check_copy(source);
  force_evict=1;CHECK(fs_write_at(source,2,"YZ",2)==0);CHECK(F(source).pins==0&&evictions==0);CHECK(F(source).content[2]=='Y');
  force_evict=1;CHECK(fs_truncate(source,4096)==0);CHECK(F(source).pins==0&&evictions==0&&F(source).size==4096);
 }else{
  alloc_fail=1;CHECK(fs_copy("/home/banana/source","/home/banana/noalloc")<0);alloc_fail=0;CHECK(F(source).pins==0);
  write_fail=1;CHECK(fs_move("/home/banana/source","/mnt/usb/bad")<0);write_fail=0;CHECK(F(source).used&&F(source).pins==0);
  drop(source);read_fail=1;CHECK(fs_copy("/home/banana/source","/home/banana/readfail")<0);
  CHECK(F(source).pins==0);read_fail=0;
  drop(source);alloc_fail=1;CHECK(fs_append(source,"x",1)<0);CHECK(F(source).pins==0);
  CHECK(fs_write_at(source,1,"x",1)<0);CHECK(F(source).pins==0);
  CHECK(fs_truncate(source,9000)<0);CHECK(F(source).pins==0);alloc_fail=0;
 }
 force_evict=pressure=0;source_cache=NULL;clear_tree();
 printf("Filesystem mutation %s: %d checks passed\n",argv[1],checks);return 0;
}
