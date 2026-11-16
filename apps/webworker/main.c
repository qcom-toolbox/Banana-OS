/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <banana.h>
#include <banana_web_worker.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../web/page.h"
#include "../../web/render.h"
#include "../../web/image_decode.h"
#include "../../kernel/image.h"
#include "../../kernel/font.h"
#include "../../kernel/kstring.h"

char* worker_github_releases(page_env_t*,const char*,const char*,uint32_t,uint32_t*);

static page_t* page;
static page_env_t env;
static ww_command_t cmd;
static unsigned char packet[WW_CHUNK+sizeof(ww_io_t)];
static int result;
static unsigned char* result_data;
static unsigned int result_length;
static unsigned char* response_data;
static ww_response_t response;

static int ipc(int op,const void* in,unsigned int n,void* out,unsigned int cap) {
    return __banana->web_ipc(op,in,n,out,cap);
}
static void output(unsigned int kind,const void* data,unsigned int n) {
    unsigned int off=0;
    while(off<n) {
        unsigned int part=n-off<WW_CHUNK?n-off:WW_CHUNK;
        ww_io_t h={off,kind}; memcpy(packet,&h,sizeof(h));
        memcpy(packet+sizeof(h),(const unsigned char*)data+off,part);
        if(ipc(WW_OUTPUT,packet,sizeof(h)+part,NULL,0)<0)__banana->exit(121);
        off+=part;
    }
}
static void* input(unsigned int n) {
    if(n>WW_DOCUMENT)__banana->exit(122);
    unsigned char* b=malloc((size_t)n+1);
    if(!b)__banana->exit(120);
    unsigned int off=0;
    while(off<n) {
        ww_io_t h={off,0};unsigned int part=n-off<WW_CHUNK?n-off:WW_CHUNK;
        int got=ipc(WW_INPUT,&h,sizeof(h),b+off,part);
        if(got!=(int)part)__banana->exit(121);
        off+=part;
    }
    b[n]=0;return b;
}
static int resource(unsigned int kind,const char* url,const char* method,
                    const void* body,unsigned int length,const char* type,unsigned int arg) {
    if(length>WW_REQUEST_BODY)return -1;
    ww_resource_t* r=calloc(1,sizeof(*r));
    if(!r)__banana->exit(120);
    r->kind=kind;r->arg=arg;r->length=length;
    kstrlcpy(r->url,url?url:"",sizeof(r->url));
    kstrlcpy(r->method,method?method:"GET",sizeof(r->method));
    kstrlcpy(r->type,type?type:"",sizeof(r->type));
    if(length)memcpy(r->body,body,length);
    int ok=ipc(WW_RESOURCE,r,sizeof(*r),NULL,0);
    free(r);free(response_data);response_data=NULL;
    if(ok<0)return -1;
    while((ok=ipc(WW_RESPONSE,NULL,0,&response,sizeof(response)))==0)__banana->sleep_ms(2);
    if(ok<0 || response.length>WW_DOCUMENT || response.result<0)return -1;
    response_data=malloc((size_t)response.length+1);
    if(!response_data)__banana->exit(120);
    unsigned int off=0;
    while(off<response.length) {
        ww_io_t h={off,1};unsigned int part=response.length-off<WW_CHUNK?response.length-off:WW_CHUNK;
        int got=ipc(WW_INPUT,&h,sizeof(h),response_data+off,part);
        if(got!=(int)part)__banana->exit(121);
        off+=part;
    }
    response_data[response.length]=0;
    return 0;
}
static int fetch(void* c,const char* u,char** data,uint32_t* n,char* type,int tc,char* final,int fc,char* err,int ec) {
    (void)c;
    if(resource(WW_FETCH,u,NULL,NULL,0,NULL,0)) {kstrlcpy(err,response.error,ec);return -1;}
    *data=(char*)response_data;response_data=NULL;*n=response.length;
    kstrlcpy(type,response.type,tc);kstrlcpy(final,response.url,fc);return 0;
}
static int request(void* c,const char* u,const char* method,const char* body,uint32_t blen,const char* ct,
                   char** data,uint32_t* n,char* rt,int rc,char* err,int ec) {
    (void)c;
    if(resource(WW_REQUEST,u,method,body,blen,ct,0)){kstrlcpy(err,response.error,ec);return -1;}
    *data=(char*)response_data;response_data=NULL;*n=response.length;kstrlcpy(rt,response.type,rc);return 0;
}
static void cookie_get(void* c,const char* url,char* out,int cap) {
    (void)c;out[0]=0;if(!resource(WW_COOKIE_GET,url,NULL,NULL,0,NULL,0))kstrlcpy(out,(char*)response_data,cap);
}
static void cookie_set(void* c,const char* url,const char* text) {
    (void)c;resource(WW_COOKIE_SET,url,NULL,text,strlen(text),NULL,0);
}
static int read_file(void* c,const char* path,char** data,uint32_t* n) {
    (void)c;if(resource(WW_FILE,path,NULL,NULL,0,NULL,0))return -1;
    *data=(char*)response_data;response_data=NULL;*n=response.length;return 0;
}
static void message(void* c,const char* text) { (void)c;resource(WW_MESSAGE,"",NULL,text,strlen(text),NULL,0); }
/* <audio> / <video>: streams the kernel runs (kernel/media.c), each decoded
 * by a worker of its own; this page only gets their status and pictures */
static int media_open(void* c,void* owner,const char* url) {
    (void)c;(void)owner;int id;
    if(resource(WW_MEDIA,url,NULL,NULL,0,NULL,WWM_OPEN)||response.length!=sizeof(id))return -1;
    memcpy(&id,response_data,sizeof(id));return id;
}
static void media_cmd(void* c,int id,int op,int a,int b,int d) {
    (void)c;int body[5]={id,op,a,b,d};resource(WW_MEDIA,"",NULL,body,sizeof(body),NULL,WWM_CMD);
}
static int media_status(void* c,int id,page_media_status_t* out) {
    (void)c;ww_media_status_t st;
    if(resource(WW_MEDIA,"",NULL,&id,sizeof(id),NULL,WWM_STATUS)||response.length!=sizeof(st))return -1;
    memcpy(&st,response_data,sizeof(st));st.error[sizeof(st.error)-1]=0;
    memset(out,0,sizeof(*out));out->state=st.state;out->playing=st.playing;out->ended=st.ended;
    out->pos_ms=st.pos_ms;out->dur_ms=st.dur_ms;out->seq=st.seq;kstrlcpy(out->error,st.error,sizeof(out->error));
    return 0;
}
/* WebGL: contexts on the GPU worker (kernel/remote_page.c, apps/webgl) */
static int gl_open(void* c,int w,int h) {
    (void)c;int body[2]={w,h},id;
    if(resource(WW_GL,"",NULL,body,sizeof(body),NULL,WWG_OPEN)||response.length!=sizeof(id))return -1;
    memcpy(&id,response_data,sizeof(id));return id;
}
static int gl_exec(void* c,int id,const uint8_t* cmds,uint32_t n,int read,uint8_t** reply,uint32_t* len) {
    (void)c;
    unsigned char* chunk=malloc(WW_REQUEST_BODY);
    if(!chunk)return -1;
    for(uint32_t off=0;off<n;){                      /* the commands, a request body at a time */
        uint32_t part=n-off<WW_REQUEST_BODY-4?n-off:WW_REQUEST_BODY-4;
        memcpy(chunk,&id,4);memcpy(chunk+4,cmds+off,part);
        if(resource(WW_GL,"",NULL,chunk,part+4,NULL,WWG_APPEND)){free(chunk);return -1;}
        off+=part;
    }
    free(chunk);
    int body[2]={id,read};
    if(resource(WW_GL,"",NULL,body,sizeof(body),NULL,WWG_EXEC)||response.length<sizeof(ww_gl_reply_t))return -1;
    *reply=response_data;response_data=NULL;*len=response.length;return 0;
}
static void gl_close(void* c,int id){(void)c;resource(WW_GL,"",NULL,&id,sizeof(id),NULL,WWG_CLOSE);}
static int media_frame(void* c,int id,uint32_t* seq,uint32_t** px,int* w,int* h) {
    (void)c;int body[2]={id,(int)*seq};ww_media_frame_t f;
    if(resource(WW_MEDIA,"",NULL,body,sizeof(body),NULL,WWM_FRAME)||response.length<sizeof(f))return -1;
    memcpy(&f,response_data,sizeof(f));
    if(f.width<=0||f.height<=0||f.width>4096||f.height>4096||
       response.length!=sizeof(f)+(unsigned)f.width*(unsigned)f.height*4u)return -1;
    uint32_t* pixels=malloc((size_t)f.width*f.height*4);
    if(!pixels)return -1;
    memcpy(pixels,response_data+sizeof(f),(size_t)f.width*f.height*4);
    *px=pixels;*w=f.width;*h=f.height;*seq=f.seq;return 0;
}
static uint32_t now_ms(void) { return __banana->ticks_ms(); }
static void clock_now(script_tm_t* tm) {
    banana_time_t t;memset(&t,0,sizeof(t));__banana->localtime(&t);
    tm->year=t.year;tm->month=t.month;tm->day=t.day;
    tm->hour=t.hour;tm->minute=t.minute;tm->second=t.second;tm->wday=t.weekday;
}
static void yield(void) { __banana->yield(); }
static void fonts(void) {
#define FONT(n) extern const unsigned char font_##n[],font_##n##_end[];
    FONT(sans) FONT(sans_bold) FONT(mono) FONT(mono_bold) FONT(emoji) FONT(cjk) FONT(hangul)
#undef FONT
    const unsigned char* start[]={font_sans,font_sans_bold,font_mono,font_mono_bold,font_emoji,font_cjk,font_hangul};
    const unsigned char* end[]={font_sans_end,font_sans_bold_end,font_mono_end,font_mono_bold_end,font_emoji_end,font_cjk_end,font_hangul_end};
    for(int i=0;i<FONT_FACES;i++)font_register(i,start[i],(uint32_t)(end[i]-start[i]));
}
static void value_text(interp_t* I,value_t r,char* out,int cap) {
    if(r.t==V_OBJ||r.t==V_FUNC) {
        value_t json=script_get_global(I,"JSON");
        if(json.t==V_OBJ){value_t fn=obj_get(I,json.o,"stringify"),s;
            if(v_isfunc(fn)&&!script_call(I,fn,json,1,&r,&s)&&s.t==V_STR){kstrlcpy(out,v_cstr(I,s),cap);return;}}
    }
    kstrlcpy(out,r.t==V_UNDEF?"":v_cstr(I,r),cap);
}
static void snapshot(void) {
    ww_state_t s;memset(&s,0,sizeof(s));s.version=WW_VERSION;s.result=result;
    if(page){
        s.height=page->layout?page->layout->height:0;s.dirty=page->dirty;
        s.focus=page->focus!=NULL;s.textarea=page->focus&&!strcmp(page->focus->tag,"textarea");
        s.js=page->js!=NULL;s.live_keys=page->live_keys;s.oom=page->A.oom;
        s.alert_pending=page->alert_pending;s.nav_pending=page->nav_pending;s.nav_newtab=page->nav_newtab;
        s.file_pick_pending=page->file_pick_pending;s.scroll_req=page->scroll_req;
        kstrlcpy(s.title,page->title,sizeof(s.title));kstrlcpy(s.status,page->status,sizeof(s.status));
        kstrlcpy(s.alert,page->alert,sizeof(s.alert));kstrlcpy(s.nav,page->nav,sizeof(s.nav));
        kstrlcpy(s.nav_post_type,page->nav_post_type,sizeof(s.nav_post_type));
        if(page->nav_pending && page->nav_post && page->nav_post_len<=WW_POST){
            s.nav_post_len=page->nav_post_len;output(WW_NAVBODY,page->nav_post,s.nav_post_len);
        }

    }
    if(result_length)output(WW_RESULT,result_data,result_length);
    ipc(WW_FINISH,&s,sizeof(s),NULL,0);
}
static void execute(void) {
    result=0;result_data=NULL;result_length=0;
    char* data=cmd.length?input(cmd.length):NULL;
    if(page){
        page->view_h=cmd.a[10]>0?cmd.a[10]:600;
        if(!(cmd.a[11]&1)){page->nav_pending=page->nav_newtab=0;page->nav_post=NULL;page->nav_post_len=0;}
        if(!(cmd.a[11]&2))page->alert_pending=0;
        if(!(cmd.a[11]&4))page->file_pick_pending=0;
        if(!(cmd.a[11]&8))page->status[0]=0;
        if(!(cmd.a[11]&16))page->scroll_req=-1;
        if(cmd.a[11]&32)page->dirty=1;
    }
    switch(cmd.op) {
    case WW_INIT: fonts();page=page_new(&env,64u<<20);if(!page)result=-1;break;
    case WW_LOAD:
        if(page){
            uint32_t length=0;
            char* reader=worker_github_releases(&env,cmd.url,data?data:"",cmd.length,&length);
            if(reader){free(data);data=reader;}
            page_load(page,cmd.url,data?data:"",reader?length:cmd.length,cmd.a[0]);
        }
        break;
    case WW_UPDATE: if(page)page_update(page,cmd.a[0]);break;
    case WW_CLICK: if(page)page_click(page,cmd.a[0],cmd.a[1]);break;
    case WW_KEY: if(page)result=page_key(page,(char)cmd.a[0]);break;
    case WW_LIVE:
        if(page&&data&&cmd.length>=128)result=page_key_live(page,cmd.a[0],data,data+64,cmd.a[1],cmd.a[2]);
        break;
    case WW_TICK: if(page)result=page_tick(page);break;
    case WW_LINK:
        result_data=calloc(1,8192);if(!result_data)__banana->exit(120);
        if(page)result=page_link_at(page,cmd.a[0],cmd.a[1],(char*)result_data,8192);
        result_length=(unsigned int)strlen((char*)result_data)+1;break;
    case WW_PICK: if(page)page_set_files(page,page->file_pick,data&&*data?data:NULL);break;
    case WW_RENDER: {
        int w=cmd.a[0],h=cmd.a[1];
        if(w<=0||h<=0||(unsigned long long)w*h>WW_PIXELS){result=-1;break;}
        result_length=(unsigned int)w*h*4u;result_data=malloc(result_length);
        if(!result_data)__banana->exit(120);
        layout_t* l=page?page->layout:NULL;
        if(l){l->sel_on=cmd.a[3];l->sel_i0=cmd.a[4];l->sel_o0=cmd.a[5];l->sel_i1=cmd.a[6];l->sel_o1=cmd.a[7];}
        render_page(l,(uint32_t*)result_data,w,w,h,0,0,w,h,cmd.a[2]);break;
    }
    case WW_TEXTPOS: {
        int pos[2]={0,0};
        if(page&&page->layout)result=layout_text_pos(page->layout,cmd.a[0],cmd.a[1],pos,pos+1);
        output(WW_RESULT,pos,sizeof(pos));break;
    }
    case WW_TEXTRANGE:
        result_data=calloc(1,65536);if(!result_data)__banana->exit(120);
        if(page&&page->layout)result=(int)layout_text_range(page->layout,cmd.a[0],cmd.a[1],cmd.a[2],cmd.a[3],(char*)result_data,65536);
        result_length=result>=0?(unsigned int)result+1:0;break;
    case WW_EVAL:
        result=-1;result_data=calloc(1,65536);if(!result_data)__banana->exit(120);
        if(page&&page->js&&data) {
            interp_t* I=page->js;value_t fn=script_get_global(I,"eval"),arg=v_str(I,data),r;
            if(v_isfunc(fn)&&!script_call(I,fn,v_undef(),1,&arg,&r)){value_text(I,r,(char*)result_data,65536);result=0;}
            else kstrlcpy((char*)result_data,script_error(I),65536);
        }
        result_length=(unsigned int)strlen((char*)result_data)+1;break;
    case WW_POSTMSG:
        if(page&&page->js&&data){interp_t* I=page->js;value_t fn=script_get_global(I,"__banana_deliver"),arg=v_str(I,data),r;
            if(v_isfunc(fn))script_call(I,fn,v_undef(),1,&arg,&r);}break;
    case WW_DECODE: {
        image_t img;char err[128];
        result=image_decode((unsigned char*)data,cmd.length,&img,err,sizeof(err));
        if(!result){
            if(img.w<=0||img.h<=0||(unsigned long long)img.w*img.h>WW_PIXELS){image_free(&img);result=-1;break;}
            int dims[2]={img.w,img.h};output(WW_NAVBODY,dims,sizeof(dims));
            result_length=(unsigned int)img.w*img.h*3u;result_data=img.rgb;
        } else {
            result_data=malloc(128);
            kstrlcpy((char*)result_data,err,128);
            result_length=(unsigned int)strlen((char*)result_data)+1;
        }break;
    }
    case WW_TEST:
#ifdef BANANA_LIBJS_WORKER
        if(cmd.a[0]==6){
            result_data=calloc(1,4096);if(!result_data)__banana->exit(120);
            unsigned used=snprintf((char*)result_data,4096,"frames=%d",page?page->nframes:0);
            if(page)for(int i=0;i<page->nframes&&used<3500;i++){
                page_t* f=page->frames[i];
                value_t fn=f->js?script_get_global(f->js,"__bananaDiagnostics"):v_undef(),detail=v_undef();
                const char* diagnostic="";
                if(v_isfunc(fn)&&!script_call(f->js,fn,v_undef(),0,NULL,&detail))diagnostic=v_cstr(f->js,detail);
                char url[160];kstrlcpy(url,f->url,sizeof(url));char* query=strchr(url,'?');if(query)*query=0;
                used+=snprintf((char*)result_data+used,4096-used,
                    "\nframe %d items=%u nested=%d\nstatus=%.100s\ndetail=%.256s\ntitle=%.60s\nurl=%.160s",
                    i,f->layout?f->layout->n:0,f->nframes,f->status,diagnostic,f->title,url);
            }
            result_length=(unsigned)strlen((char*)result_data)+1;
        }
#endif
        if(cmd.a[0]==5){
            for(int round=0;round<20;round++){
                volatile char* map=__banana->vm_map(NULL,8u<<20,BANANA_PROT_READ|BANANA_PROT_WRITE,0);
                if(!map){result=-1;break;}
                for(unsigned i=0;i<(8u<<20);i+=4096)map[i]=(char)round;
                if(__banana->vm_unmap((void*)map,8u<<20)<0){result=-1;break;}
            }
        }
        if(cmd.a[0]==1){*(volatile int*)(uintptr_t)1=1;}
        if(cmd.a[0]==2){for(;;)__asm__ volatile("":::"memory");}
        if(cmd.a[0]==3){for(;;){volatile char* m=malloc(2u<<20);if(!m)__banana->exit(120);for(unsigned int i=0;i<(2u<<20);i+=4096)m[i]=1;}}
        if(cmd.a[0]==4){
            int denied=__banana->open("/etc/hostname",1)==-1;
            denied&=__banana->kcmd("touch /home/banana/worker-escaped")==-1;
            denied&=__banana->thread_create(NULL,NULL)==-1;
            unsigned long open_slot=(unsigned long)((const char*)&__banana->open-(const char*)__banana)/sizeof(void*);
            long raw;
            const char* path="/etc/hostname";
            __asm__ volatile("int $0x80":"=a"(raw):"a"(open_slot),"D"(path),"S"(1):"memory","cc");
            denied&=raw==-1;
            void* readonly=__banana->vm_map(NULL,16384,BANANA_PROT_READ,0);
            if(readonly){
                denied&=ipc(WW_WAIT,NULL,0,readonly,sizeof(ww_command_t))==-1;
                __banana->vm_unmap(readonly,16384);
            }else denied=0;
            result=denied?0:-1;
        }break;
    default:result=-1;break;
    }
#ifdef BANANA_LIBJS_WORKER
    if(page&&page->js)script_checkpoint(page->js);
#endif
    free(data);snapshot();free(result_data);
}
#ifdef BANANA_LIBJS_WORKER
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
#endif
int main(int argc,char** argv) {
#ifdef BANANA_LIBJS_WORKER
    for(void (**f)(void)=__init_array_start;f<__init_array_end;++f)if(*f)(*f)();
#endif
    (void)argc;(void)argv;
    if(!BANANA_HAS(__banana,web_ipc))return 125;
    memset(&env,0,sizeof(env));env.fetch=fetch;env.request=request;
    env.decode_image=web_decode_image;env.now_ms=now_ms;env.yield=yield;
    env.cookie_get=cookie_get;env.cookie_set=cookie_set;env.read_file=read_file;env.message=message;
    env.media_open=media_open;env.media_cmd=media_cmd;env.media_status=media_status;env.media_frame=media_frame;
    env.gl_open=gl_open;env.gl_exec=gl_exec;env.gl_close=gl_close;
    web_yield_hook=yield;script_clock=clock_now;
    for(;;){
        int n=ipc(WW_WAIT,NULL,0,&cmd,sizeof(cmd));
        if(n<0)return 121;
        if(!n){__banana->sleep_ms(10);continue;}
        if(n!=(int)sizeof(cmd)||cmd.version!=WW_VERSION)return 121;
        execute();
    }
}
