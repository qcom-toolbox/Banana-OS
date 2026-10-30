/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define DESK_MAX 128
#define DESK_X 4
#define DESK_Y 8
#define BAR_H 28
#define CELL_W 80
#define CELL_H 72
enum { DK_APP,DK_TRASH,DK_FILE,DK_DIR };
typedef struct { int kind;char label[48],path[512];int col,row; } desk_item_t;
typedef struct { uint32_t width,height; } fb_info_t;
typedef struct { char* content; } fs_file_t;
static desk_item_t g_desk[DESK_MAX];static int g_ndesk,checks;
static fb_info_t screen={800,600};static fs_file_t file;
static int fs_find_file(const char* p) { (void)p;return 0; }
static fs_file_t* fs_get_file(int i) { (void)i;return &file; }
static const fb_info_t* fb_info(void) { return &screen; }
static size_t kstrlcpy(char* d,const char* s,size_t n) { size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l; }
static int ksnprintf(char* d,size_t n,const char* fmt,...) { va_list a;va_start(a,fmt);int k=vsnprintf(d,n,fmt,a);va_end(a);return k; }
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include "layout.inc"
static void reset(const char* text) { file.content=(char*)text;g_nlay=-1;g_ndesk=2;memset(g_desk,0,sizeof(g_desk));g_desk[0].kind=DK_APP;strcpy(g_desk[0].label,"one");g_desk[1].kind=DK_APP;strcpy(g_desk[1].label,"two"); }
int main(void) {
 /* Loaded unsigned UINT_MAX becomes signed -1 in the old implementation. */
 reset("app:one\t4294967295\t0\n");desk_place();
 CHECK(g_desk[0].col>=0 && g_desk[0].row>=0);
 const char* bad[]={"4294967295","2147483648","-1","64","4294967296","9999999999999999999999999","1junk",""};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
  char text[180];snprintf(text,sizeof(text),"app:one\t%s\t0\napp:two\t1\t2\n",bad[i]);reset(text);desk_place();
  CHECK(g_nlay==1);CHECK(g_desk[1].col==1&&g_desk[1].row==2);
 }
 reset("app:one\t2\t4294967295\napp:two\t1\t2\n");desk_place();CHECK(g_nlay==1);
 reset("app:one\t2\t32\n");desk_place();CHECK(g_nlay==0);
 reset("app:one\t2\t3\napp:two\t2\t3\n");desk_place();
 CHECK(g_desk[0].col==2&&g_desk[0].row==3);
 CHECK(g_desk[1].col!=2||g_desk[1].row!=3);
 /* Defense in depth if an invalid in-memory entry bypasses the parser. */
 reset("");g_nlay=1;strcpy(g_lay[0].key,"app:one");g_lay[0].col=-1;g_lay[0].row=-1;desk_place();CHECK(g_desk[0].col>=0&&g_desk[0].row>=0);
 reset("app:one\t63\t31\n");screen=(fb_info_t){8000,4000};desk_place();CHECK(g_desk[0].col==63&&g_desk[0].row==31);
 for(unsigned w=1;w<=8192;w=w*2+1)for(unsigned h=1;h<=4320;h=h*2+1){
  reset("app:one\t63\t31\n");screen=(fb_info_t){w,h};g_ndesk=DESK_MAX;desk_place();
  for(int i=0;i<g_ndesk;i++)CHECK(g_desk[i].col>=0&&g_desk[i].row>=0);
 }
 reset("app:one\t00002\t00003\r\n");screen=(fb_info_t){800,600};desk_place();CHECK(g_nlay==1&&g_desk[0].col==2&&g_desk[0].row==3);
 char longline[200];memset(longline,'0',sizeof(longline));memcpy(longline,"app:one\t1\t",10);longline[150]='\n';strcpy(longline+151,"app:two\t1\t2\n");reset(longline);desk_place();CHECK(g_nlay==1&&g_desk[1].col==1&&g_desk[1].row==2);
 printf("Desktop layout: %d checks passed\n",checks);return 0;
}
