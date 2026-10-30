/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../kernel/textbuf.c"
#include "../../kernel/notepad.c"
static fs_file_t file;
static unsigned checks,live;
static int fail_at=-1,allocations,evict,io,binary;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void* kmalloc(size_t n){
 if(evict&&file.loaded&&!file.pins&&file.cap>=4096){free(file.content);file.content=NULL;file.loaded=0;}
 if(allocations++==fail_at)return NULL;
 void* p=malloc(n);if(p)live++;return p;
}
void* kzalloc(size_t n){void* p=kmalloc(n);if(p)memset(p,0,n);return p;}
void kfree(void* p){if(p){CHECK(live>0);live--;free(p);}}
size_t kstrlcpy(char* d,const char* s,size_t n){size_t z=strlen(s);if(n){size_t c=z<n-1?z:n-1;memcpy(d,s,c);d[c]=0;}return z;}
int ksnprintf(char* d,size_t n,const char* fmt,...){va_list a;va_start(a,fmt);int z=vsnprintf(d,n,fmt,a);va_end(a);return z;}
int fs_find_file(const char* p){(void)p;return file.used?0:-1;}
int fs_find_dir(const char* p){(void)p;return -1;}
int fs_is_binary(int i){CHECK(i==0);return binary;}
fs_file_t* fs_get_file(int i){CHECK(i==0);if(io==2)return NULL;if(io)file.loaded=0;return &file;}
void fs_pin(int i){CHECK(i==0);file.pins++;}
void fs_unpin(int i){CHECK(i==0&&file.pins);file.pins--;}
static void input(const char* text){free(file.content);memset(&file,0,sizeof(file));file.content=strdup(text);CHECK(file.content);file.size=(uint32_t)strlen(text);file.cap=file.size+1;file.used=file.loaded=1;}
static void expect(textbuf_t* t,const char* text){uint32_t n;char* b=tb_text(t,&n);CHECK(b);CHECK(n==strlen(text)&&!memcmp(b,text,n));kfree(b);}
int main(int argc,char** argv){
 CHECK(argc==2);
 if(!strcmp(argv[1],"oom")){
  char text[16384];size_t n=0;
  for(int i=0;i<100;i++)n+=(size_t)snprintf(text+n,sizeof(text)-n,"Line %d: abcdefghijklmnopqrstuvwxyz\r\n",i);
  textbuf_t t={0};tb_load(&t,"old unsaved",11);t.dirty=1;t.cx=3;
  int total;allocations=0;tb_load(&t,text,(uint32_t)n);total=allocations;tb_free(&t);
  for(int i=0;i<total;i++){
   tb_load(&t,"old unsaved",11);t.dirty=1;t.cx=3;
   textbuf_t before=t;allocations=0;fail_at=i;
   CHECK(tb_load(&t,text,(uint32_t)n)<0);fail_at=-1;
   CHECK(!memcmp(&t,&before,sizeof(t)));expect(&t,"old unsaved");tb_free(&t);
  }
 }else if(!strcmp(argv[1],"limits")){
  textbuf_t t={0};tb_load(&t,"keep",4);t.dirty=1;textbuf_t before=t;
  char* text=malloc(200001);CHECK(text);memset(text,'\n',200001);
  CHECK(tb_load(&t,text,200000)<0);CHECK(!memcmp(&t,&before,sizeof(t)));expect(&t,"keep");
  free(text);tb_free(&t);
 }else if(!strcmp(argv[1],"bounds")){
  textbuf_t t={0};CHECK(tb_load(&t,"keep",4)==0);t.dirty=1;textbuf_t before=t;
  CHECK(tb_load(&t,"x",0xffffffffu)<0);CHECK(!memcmp(&t,&before,sizeof(t)));
  CHECK(tb_load(&t,NULL,1)<0);CHECK(!memcmp(&t,&before,sizeof(t)));
  CHECK(tb_load(&t,NULL,0)==0);expect(&t,"");tb_free(&t);
 }else if(!strcmp(argv[1],"normal")){
  textbuf_t t={0};tb_load(&t,"one\r\ntwo\r\n",10);expect(&t,"one\ntwo\n");CHECK(t.n==3&&!t.dirty);
  tb_load(&t,t.lines[0].s,t.lines[0].len);expect(&t,"one");
  tb_load(&t,"",0);expect(&t,"");CHECK(t.n==1&&t.lines[0].s);tb_free(&t);
 }else{
  tb_load(&g_tb,"unsaved document",16);g_tb_ready=1;g_tb.dirty=1;
  strcpy(g_path,"/home/banana/old.txt");
  if(!strcmp(argv[1],"cache")){
   char text[8193];memset(text,'T',8192);text[8192]=0;input(text);evict=1;
   CHECK(load("/home/banana/new.txt")==0);expect(&g_tb,text);
  }else{
   input("new file contents");textbuf_t before=g_tb;
   if(!strcmp(argv[1],"io"))io=1;
   else if(!strcmp(argv[1],"null"))io=2;
   else if(!strcmp(argv[1],"binary"))binary=1;
   else if(!strcmp(argv[1],"load-oom")){allocations=0;fail_at=0;}
   else if(!strcmp(argv[1],"new-oom")){file.used=0;allocations=0;fail_at=0;}
   else CHECK(0);
   CHECK(load("/home/banana/new.txt")<0);fail_at=-1;
   CHECK(!memcmp(&g_tb,&before,sizeof(before)));expect(&g_tb,"unsaved document");
   CHECK(!strcmp(g_path,"/home/banana/old.txt")&&g_status_err);
  }
  tb_free(&g_tb);
 }
 CHECK(!file.pins&&live==0);free(file.content);
 printf("Text loading %s: %u checks passed\n",argv[1],checks);return 0;
}
