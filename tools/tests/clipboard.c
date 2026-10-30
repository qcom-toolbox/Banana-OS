/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define SPINLOCK_H
typedef struct { int v; } spinlock_t;
#define SPINLOCK_INIT {0}
static uintptr_t spin_lock_irq(spinlock_t* l){(void)l;return 0;}
static void spin_unlock_irq(spinlock_t* l,uintptr_t f){(void)l;(void)f;}
#include "../../kernel/clipboard.c"
static fs_file_t file;
static unsigned checks,live;
static int evict,io,fail_alloc,replace_on_alloc;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void* kmalloc(size_t n){
 if(replace_on_alloc){replace_on_alloc=0;char big[512];memset(big,'R',sizeof(big));clipboard_set(big,sizeof(big));}
 if(evict&&file.loaded&&!file.pins&&file.cap>=4096){free(file.content);file.content=NULL;file.loaded=0;}
 if(fail_alloc)return NULL;
 void* p=malloc(n);if(p)live++;return p;
}
void kfree(void* p){if(p){CHECK(live>0);live--;free(p);}}
int ksnprintf(char* out,size_t cap,const char* fmt,...){va_list a;va_start(a,fmt);int n=vsnprintf(out,cap,fmt,a);va_end(a);return n;}
int fs_find_file(const char* p){CHECK(!strcmp(p,PINNED_FILE));return file.used?0:-1;}
fs_file_t* fs_get_file(int i){CHECK(i==0);if(io==2)return NULL;if(io)file.loaded=0;return &file;}
void fs_pin(int i){CHECK(i==0);file.pins++;}
void fs_unpin(int i){CHECK(i==0&&file.pins);file.pins--;}
int fs_mkdir_p(const char* p){CHECK(!strcmp(p,"/etc"));return 0;}
void fs_delete(const char* p,int recursive){CHECK(!strcmp(p,PINNED_FILE)&&!recursive);free(file.content);memset(&file,0,sizeof(file));}
int fs_write_path(const char* p,const void* b,uint32_t n){
 CHECK(!strcmp(p,PINNED_FILE));char* c=malloc(n+1);CHECK(c);memcpy(c,b,n);c[n]=0;
 free(file.content);file.content=c;file.size=n;file.cap=n+1;file.used=file.loaded=1;return 0;
}
static void input(const char* s){file.content=strdup(s);CHECK(file.content);file.size=(uint32_t)strlen(s);file.cap=file.size+1;file.used=file.loaded=1;}
static void entry(int index,const char* text,int pin){
 char b[CLIPBOARD_MAX+1];uint32_t n=0;int p=-1;
 CHECK(clipboard_hist_peek(index,b,sizeof(b),&n,&p)==0);
 CHECK(n==strlen(text)&&!memcmp(b,text,n)&&p==pin);
}
int main(int argc,char** argv){
 CHECK(argc==2);
 if(!strcmp(argv[1],"race")){
  clipboard_set("a",1);replace_on_alloc=1;
  CHECK(clipboard_hist_use(0)<0);uint32_t n;CHECK(clipboard_get(&n)&&n==512);
 }else if(!strcmp(argv[1],"pinned")){
  for(int i=0;i<CLIP_HIST_MAX;i++){char text[2]={(char)('A'+i),0};clipboard_set(text,1);CHECK(clipboard_hist_pin(0,1)==0);}
  CHECK(g_nhist==CLIP_HIST_MAX);clipboard_set("new",3);
  CHECK(g_nhist==CLIP_HIST_MAX);entry(CLIP_HIST_MAX-1,"A",1);
  uint32_t n=0;CHECK(!strcmp(clipboard_get(&n),"new")&&n==3);
  CHECK(clipboard_hist_clear()==0);clipboard_set("A",1);entry(0,"A",1);
  CHECK(clipboard_hist_pin(0,0)==0&&clipboard_hist_clear()==1);
  clipboard_set("new",3);CHECK(g_nhist==CLIP_HIST_MAX);entry(0,"new",0);
 }else if(!strcmp(argv[1],"normal")){
  input("3\none\n3\ntwo\n3\none\n");clipboard_hist_load();CHECK(g_nhist==2);entry(0,"one",1);entry(1,"two",1);
  CHECK(clipboard_hist_use(1)==0);entry(0,"two",1);
  CHECK(clipboard_hist_pin(0,0)==0);CHECK(clipboard_hist_clear()==1);entry(0,"one",1);
  CHECK(clipboard_hist_delete(0)==0&&g_nhist==0&&!file.used);
 }else if(!strcmp(argv[1],"evict")){
  char b[6010];memcpy(b,"6000\n",5);memset(b+5,'E',6000);b[6005]='\n';b[6006]=0;input(b);evict=1;
  clipboard_hist_load();CHECK(g_nhist==1&&g_hist[0].len==6000);CHECK(g_hist[0].text[5999]=='E');
 }else if(!strcmp(argv[1],"io")||!strcmp(argv[1],"null")){
  input("3\none\n");io=!strcmp(argv[1],"io")?1:2;clipboard_hist_load();CHECK(g_nhist==0);
 }else if(!strcmp(argv[1],"oom")){
  input("3\none\n");fail_alloc=1;clipboard_hist_load();CHECK(g_nhist==0);
 }else if(!strcmp(argv[1],"missing")){
  clipboard_hist_load();CHECK(g_nhist==0);
 }else if(!strcmp(argv[1],"limit")){
  char* b=malloc(CLIPBOARD_MAX+32);CHECK(b);int n=sprintf(b,"%u\n",CLIPBOARD_MAX);
  memset(b+n,'M',CLIPBOARD_MAX);b[n+CLIPBOARD_MAX]='\n';b[n+CLIPBOARD_MAX+1]=0;input(b);free(b);
  clipboard_hist_load();CHECK(g_nhist==1&&g_hist[0].len==CLIPBOARD_MAX&&g_hist[0].text[CLIPBOARD_MAX-1]=='M');
 }else{
  const char* text=NULL;
  if(!strcmp(argv[1],"overflow"))text="4294967299\nabc\n";
  if(!strcmp(argv[1],"huge"))text="9999999999999999999999999999999999999\nabc\n";
  if(!strcmp(argv[1],"nodigit"))text="\n";
  if(!strcmp(argv[1],"truncated"))text="3\nab";
  if(!strcmp(argv[1],"nonl"))text="3\nabc";
  if(!strcmp(argv[1],"badnl"))text="3\nabcX";
  if(!strcmp(argv[1],"oversize"))text="65537\nabc\n";
  CHECK(text);input(text);clipboard_hist_load();CHECK(g_nhist==0);
 }
 CHECK(file.pins==0);for(int i=0;i<g_nhist;i++)kfree(g_hist[i].text);kfree(g_text);
 CHECK(live==0);free(file.content);printf("Clipboard %s: %u checks passed\n",argv[1],checks);return 0;
}
