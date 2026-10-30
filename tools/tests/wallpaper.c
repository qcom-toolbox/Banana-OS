/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../kernel/wallpaper.h"
#include "../../kernel/wallpaper_data.h"
#include "../../kernel/fs.h"
#include "../../kernel/fb.h"
#define PIXEL(name) const uint8_t wallpaper_##name[WALLPAPER_IMG_W*WALLPAPER_IMG_H*3]={0};
PIXEL(midnight_blue) PIXEL(forest_night) PIXEL(graphite) PIXEL(royal_purple) PIXEL(sunset_grid)
PIXEL(ocean_wave) PIXEL(cyber_mint) PIXEL(amber_mesh) PIXEL(azure_flow) PIXEL(plain_black)
static fs_file_t files[2];
static int pins[2],read_fail,evict_config,evictions,checks,decodes,reenter;
static fb_info_t framebuffer={.width=800,.height=600};
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void* kmalloc(size_t n) {
 if(evict_config&&files[1].content&&files[1].cap>=4096&&!pins[1]){free(files[1].content);files[1].content=NULL;files[1].loaded=0;evictions++;}
 CHECK(n<=2560u*1600u*4u);return malloc(n?n:1);
}
void kfree(void* p){free(p);}
void klog(const char* f,...){(void)f;}
int fs_find_file(const char* p){return !strcmp(p,WALLPAPER_CONFIG)?1:!strcmp(p,"/pic.jpg")?0:-1;}
fs_file_t* fs_get_file(int i){if(i<0||i>1||read_fail)return NULL;return &files[i];}
void fs_pin(int i){CHECK(i>=0&&i<2);pins[i]++;}
void fs_unpin(int i){CHECK(i>=0&&i<2&&pins[i]>0);pins[i]--;}
int fs_write_path(const char* p,const void* b,uint32_t n){(void)p;(void)b;return (int)n;}
void fs_file_path(int i,char* p,int cap){(void)i;snprintf(p,(size_t)cap,"/pic.jpg");}
const fb_info_t* fb_info(void){return &framebuffer;}
int fb_available(void){return 1;}
const char* image_mode_name(image_mode_t m){(void)m;return "fill";}
int image_mode_parse(const char* s,image_mode_t* m){(void)s;*m=IMAGE_FILL;return 1;}
int image_decode(const uint8_t* p,uint32_t n,image_t* out,char* err,uint32_t cap){
 (void)err;(void)cap;CHECK(p&&n);decodes++;
 if(reenter){reenter=0;uint32_t nested[16*9];wallpaper_render(nested,16,9,16);}
 if(p==(uint8_t*)files[0].content) { CHECK(pins[0]>0); }
 out->rgb=kmalloc(48);CHECK(out->rgb);memset(out->rgb,7,48);out->w=out->h=4;return 0;
}
void image_free(image_t* p){free(p->rgb);memset(p,0,sizeof(*p));}
void image_render(const image_t* p,uint32_t* dst,int w,int h,image_mode_t mode,uint32_t bg){(void)p;(void)mode;(void)bg;for(int i=0;i<w*h;i++)dst[i]=0x123456;}
#include "wallpaper_impl.inc"
int main(int argc,char** argv){
 CHECK(argc==2);files[0]=(fs_file_t){.content=calloc(1,8192),.size=4096,.cap=8192,.used=1,.loaded=1};
 char err[128];
 if(!strcmp(argv[1],"io")){
  read_fail=1;CHECK(wallpaper_set_file("/pic.jpg",IMAGE_FILL,err,sizeof(err))<0);CHECK(pins[0]==0&&decodes==0);
  read_fail=0;files[0].loaded=0;CHECK(wallpaper_set_file("/pic.jpg",IMAGE_FILL,err,sizeof(err))<0);CHECK(pins[0]==0&&decodes==0);
 }else if(!strcmp(argv[1],"config")){
  char* text=calloc(1,8192);strcpy(text,"file fill /pic.jpg\n");memset(text+strlen(text),'#',8000);text[8019]=0;
  files[1]=(fs_file_t){.content=text,.size=8019,.cap=8192,.used=1,.loaded=1};evict_config=1;
  wallpaper_load_config();CHECK(decodes==1&&evictions==0&&pins[0]==0&&pins[1]==0);CHECK(wallpaper_current_preset()==-1);
 }else if(!strcmp(argv[1],"preset")){
  uint32_t screen[16*9];wallpaper_set_preset(10);reenter=1;
  wallpaper_render(screen,16,9,16);CHECK(decodes==(sizeof(void*)==8?1:0)&&screen[0]==0x123456);
  wallpaper_render(screen,16,9,16);CHECK(decodes==(sizeof(void*)==8?1:0));
  wallpaper_set_preset(11);wallpaper_render(screen,16,9,16);CHECK(decodes==(sizeof(void*)==8?2:0));
  CHECK(wallpaper_current_preset()==11&&!wallpaper_current_file()[0]);drop_photo();
 }else{
  CHECK(set_file_at("/pic.jpg",IMAGE_FILL,-1,600,err,sizeof(err),0)<0);
  CHECK(set_file_at("/pic.jpg",IMAGE_FILL,2561,1600,err,sizeof(err),0)<0);
  CHECK(wallpaper_recent_thumb(0,-1,20)==NULL);
 }
 free(files[0].content);free(files[1].content);drop_custom();
 printf("Wallpaper %s: %d checks passed\n",argv[1],checks);return 0;
}
