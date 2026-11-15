/* SPDX-License-Identifier: GPL-3.0-or-later */
#define REMOTE_PAGE_IMPLEMENTATION
#include "remote_page.h"
#include "web_worker.h"
#include "web_policy.h"
#include "app.h"
#include "fs.h"
#include "image.h"
#include "browser.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "task.h"
#include "serial.h"
#include "media.h"
#include "../sdk/include/banana_web_worker.h"
#define WORKERS 8                   /* pages, and media streams' decoders */
#define TRANSFER_BUDGET (64u << 20)
typedef struct remote_page {
    page_t p;                       /* kernel-owned compatibility proxy */
    layout_t layout;
    dom_node_t focus;
    int app,slot,dead,busy,network_enabled;
    uint32_t generation;
    volatile int stage,resource_ready;
    ww_command_t cmd;
    ww_state_t state;
    const uint8_t* input;
    uint8_t* output;
    uint32_t out_cap,out_used;
    uint8_t* navbody;
    uint32_t nav_used;
    ww_resource_t* request;
    ww_response_t response;
    char* resource_data;
    uint32_t transferred,requests,peak_memory;
    page_env_t environment;
    char principal[8192];
    char grants[16][FS_PATH_LEN];
    int ngrants;
} remote_page_t;
static remote_page_t* workers[WORKERS];
static int terminated(const char* s,uint32_t n) { for(uint32_t i=0;i<n;i++)if(!s[i])return 1;return 0; }
static remote_page_t* channel(int app,uint32_t generation) {
    for(int i=0;i<WORKERS;i++)if(workers[i]&&workers[i]->app==app&&workers[i]->generation==generation&&!workers[i]->dead)return workers[i];
    return NULL;
}
int web_worker_ipc(int app,uint32_t generation,int op,const void* in,uint32_t len,void* out,uint32_t cap) {
    remote_page_t* r=channel(app,generation);
    if(!r)return -1;
    switch(op){
    case WW_WAIT:
        if(len||cap!=sizeof(ww_command_t))return -1;
        if(r->stage!=1){task_sleep_ms(10);return 0;}
        memcpy(out,&r->cmd,sizeof(r->cmd));r->stage=2;return sizeof(r->cmd);
    case WW_INPUT: {
        if(r->stage!=2||len!=sizeof(ww_io_t))return -1;
        ww_io_t h;memcpy(&h,in,sizeof(h));
        const void* data=NULL;uint32_t size=0;
        if(!h.kind){data=r->input;size=r->cmd.length;}
        else if(h.kind==1&&r->resource_ready==2){data=r->resource_data;size=r->response.length;}
        else return -1;
        if(h.offset>size||cap>size-h.offset||(!data&&cap))return -1;
        if(cap)memcpy(out,(const uint8_t*)data+h.offset,cap);
        return cap;
    }
    case WW_OUTPUT: {
        if(r->stage!=2||len<sizeof(ww_io_t)||len>WW_CHUNK+sizeof(ww_io_t))return -1;
        ww_io_t h;memcpy(&h,in,sizeof(h));uint32_t n=len-sizeof(h);
        if(h.kind==WW_RESULT){
            if(h.offset!=r->out_used||n>r->out_cap-r->out_used||(!r->output&&n))return -1;
            if(n)memcpy(r->output+r->out_used,(const uint8_t*)in+sizeof(h),n);r->out_used+=n;
        }else if(h.kind==WW_NAVBODY){
            if(h.offset!=r->nav_used||n>WW_POST-r->nav_used)return -1;
            if(!r->navbody){r->navbody=kmalloc(WW_POST+1);if(!r->navbody)return -1;}
            memcpy(r->navbody+r->nav_used,(const uint8_t*)in+sizeof(h),n);r->nav_used+=n;r->navbody[r->nav_used]=0;
        }else return -1;
        return n;
    }
    case WW_FINISH: {
        if(r->stage!=2||len!=sizeof(ww_state_t))return -1;
        ww_state_t s;memcpy(&s,in,sizeof(s));
        if(s.version!=WW_VERSION||s.height<0||s.height>500000||s.nav_post_len>r->nav_used||
           !terminated(s.title,sizeof(s.title))||!terminated(s.status,sizeof(s.status))||
           !terminated(s.alert,sizeof(s.alert))||!terminated(s.nav,sizeof(s.nav))||
           !terminated(s.nav_post_type,sizeof(s.nav_post_type)))return -1;
        for(const char* t=s.nav_post_type;*t;t++)if((uint8_t)*t<32||*t==127)return -1;
        r->state=s;__sync_synchronize();r->stage=3;return 0;
    }
    case WW_RESOURCE: {
        if(r->stage!=2||len!=sizeof(ww_resource_t)||r->resource_ready==1)return -1;
        ww_resource_t* q=kmalloc(sizeof(*q));if(!q)return -1;memcpy(q,in,sizeof(*q));
        if(q->length>WW_REQUEST_BODY||!terminated(q->url,sizeof(q->url))||
           !terminated(q->method,sizeof(q->method))||!terminated(q->type,sizeof(q->type))){kfree(q);return -1;}
        /* 512 downloads, files and messages a page; its media streams' pictures
         * and commands (no network, no files) are asked for many times a second */
        if(q->kind!=WW_MEDIA&&r->requests>=512){kfree(q);return -1;}
        if(r->request)kfree(r->request);if(r->resource_data)kfree(r->resource_data);
        r->resource_data=NULL;r->request=q;memset(&r->response,0,sizeof(r->response));
        if(q->kind!=WW_MEDIA)r->requests++;
        __sync_synchronize();r->resource_ready=1;return 0;
    }
    case WW_RESPONSE:
        if(r->stage!=2||len||cap!=sizeof(ww_response_t))return -1;
        if(r->resource_ready!=2)return 0;
        memcpy(out,&r->response,sizeof(r->response));return sizeof(r->response);
    default:return -1;
    }
}
static int granted(remote_page_t* r,const char* path) {
    for(int i=0;i<r->ngrants;i++)if(!strcmp(path,r->grants[i]))return 1;
    return 0;
}
static void serve(remote_page_t* r) {
    ww_resource_t* q=r->request;ww_response_t* a=&r->response;
    a->result=-1;
    kstrlcpy(a->error,"Browser permission or resource quota denied",sizeof(a->error));
    if(r->transferred>=TRANSFER_BUDGET)goto done;
    switch(q->kind){
    case WW_FETCH: case WW_REQUEST:
        if(r->network_enabled&&web_network_url(q->url)){
            if(q->kind==WW_REQUEST&&!web_same_origin(r->principal,q->url))break;
            if(strcmp(q->method,"GET")&&strcmp(q->method,"POST"))break;
            int valid=1;for(const char* t=q->type;*t;t++)if((uint8_t)*t<32||*t==127)valid=0;
            if(!valid)break;
            a->result=browser_worker_fetch(r->principal,q->kind==WW_REQUEST,q->url,q->method,
                (char*)q->body,q->length,q->type,&r->resource_data,&a->length,a->type,sizeof(a->type),
                a->url,sizeof(a->url),a->error,sizeof(a->error));
        }else if(!strncmp(q->url,"file://",7)&&granted(r,q->url+7)&&r->environment.read_file){
            a->result=r->environment.read_file(r->environment.ctx,q->url+7,&r->resource_data,&a->length);
            kstrlcpy(a->type,page_mime_of(q->url+7),sizeof(a->type));kstrlcpy(a->url,q->url,sizeof(a->url));
        }else if(!strncmp(r->principal,"about:",6)&&!strncmp(q->url,"about:",6)&&r->environment.fetch){
            a->result=r->environment.fetch(r->environment.ctx,q->url,&r->resource_data,&a->length,
                a->type,sizeof(a->type),a->url,sizeof(a->url),a->error,sizeof(a->error));
        }
        break;
    case WW_FILE:
        if(granted(r,q->url)&&r->environment.read_file)
            a->result=r->environment.read_file(r->environment.ctx,q->url,&r->resource_data,&a->length);
        break;
    case WW_COOKIE_GET:
        if(web_same_origin(r->principal,q->url)&&r->environment.cookie_get){
            r->resource_data=kzalloc(8192);
            if(r->resource_data){r->environment.cookie_get(r->environment.ctx,q->url,r->resource_data,8192);
                a->length=strlen(r->resource_data);a->result=0;}
        }break;
    case WW_COOKIE_SET:
        if(web_same_origin(r->principal,q->url)&&q->length<2048&&r->environment.cookie_set){
            char text[2048];memcpy(text,q->body,q->length);text[q->length]=0;
            r->environment.cookie_set(r->environment.ctx,q->url,text);a->result=0;
        }break;
    case WW_MEDIA: {
        /* <audio> / <video>: streams owned by this page (kernel/media.c) */
        int b[5]={0};memcpy(b,q->body,q->length<sizeof(b)?q->length:sizeof(b));
        if(q->arg==WWM_OPEN){
            if(!r->network_enabled||!web_network_url(q->url))break;
            int id=media_open(r,q->url);
            if(id>=0&&(r->resource_data=kmalloc(sizeof(id)))){memcpy(r->resource_data,&id,sizeof(id));a->length=sizeof(id);a->result=0;}
        }else if(q->arg==WWM_CMD){
            if(!media_owned(b[0],r))break;
            switch(b[1]){
            case PAGE_MEDIA_PLAY:media_play(b[0]);break;
            case PAGE_MEDIA_PAUSE:media_pause(b[0]);break;
            case PAGE_MEDIA_SEEK:media_seek(b[0],b[2]<0?0u:(uint32_t)b[2]);break;
            case PAGE_MEDIA_SET:media_set(b[0],b[2],b[3],b[4]);break;
            case PAGE_MEDIA_CLOSE:media_close(b[0]);break;
            default:break;
            }
            a->result=0;
        }else if(q->arg==WWM_STATUS){
            media_status_t m;ww_media_status_t* o;
            if(!media_owned(b[0],r)||media_status(b[0],&m)||!(o=kzalloc(sizeof(*o))))break;
            o->state=m.state;o->playing=m.playing;o->ended=m.ended;o->pos_ms=m.pos_ms;o->dur_ms=m.dur_ms;o->seq=m.seq;
            kstrlcpy(o->error,m.error,sizeof(o->error));
            r->resource_data=(char*)o;a->length=sizeof(*o);a->result=0;
        }else if(q->arg==WWM_FRAME){
            uint32_t bytes=0;
            if(media_owned(b[0],r)&&(r->resource_data=media_frame(b[0],(uint32_t)b[1],&bytes))){a->length=bytes;a->result=0;}
        }
        break;
    }
    case WW_MESSAGE:
        if(q->length<8192&&r->environment.message){
            char* text=kmalloc(q->length+1);
            if(text){memcpy(text,q->body,q->length);text[q->length]=0;
                r->environment.message(r->environment.ctx,text);kfree(text);a->result=0;}
        }break;
    default:break;
    }
    if(a->length>WW_DOCUMENT||(q->kind!=WW_MEDIA&&a->length>TRANSFER_BUDGET-r->transferred)){
        if(r->resource_data)kfree(r->resource_data);r->resource_data=NULL;a->length=0;a->result=-1;
    }else if(q->kind!=WW_MEDIA)r->transferred+=a->length;   /* pictures are not downloads */
done:
    __sync_synchronize();r->resource_ready=2;
}
static void crashed(remote_page_t* r,const char* why) {
    if(!r->dead)app_web_kill(r->app,r->generation);
    media_close_owner(r);
    if(r->slot>=0&&workers[r->slot]==r)workers[r->slot]=NULL;
    r->slot=-1;r->dead=1;r->stage=0;r->p.js=NULL;r->p.focus=NULL;r->p.dirty=0;
    r->p.nav_pending=r->p.alert_pending=r->p.file_pick_pending=0;
    kstrlcpy(r->p.status,why,sizeof(r->p.status));r->layout.height=0;
    klog("browser worker: %s (app %d; op %u; peak %u KiB)\n",
         why,r->app,r->cmd.op,r->peak_memory/1024);
}
static void apply(remote_page_t* r) {
    page_t* p=&r->p;ww_state_t* s=&r->state;
    r->layout.height=s->height;p->dirty=!!s->dirty;p->js=s->js?(interp_t*)p:NULL;
    p->focus=s->focus?&r->focus:NULL;r->focus.tag=s->textarea?"textarea":"input";
    p->live_keys=!!s->live_keys;p->A.oom=!!s->oom;
    p->alert_pending=!!s->alert_pending;p->nav_pending=!!s->nav_pending;p->nav_newtab=!!s->nav_newtab;
    p->file_pick_pending=!!s->file_pick_pending;p->file_pick=s->file_pick_pending?&r->focus:NULL;
    p->scroll_req=s->scroll_req>=-1&&s->scroll_req<500000?s->scroll_req:-1;
    kstrlcpy(p->title,s->title,sizeof(p->title));kstrlcpy(p->status,s->status,sizeof(p->status));
    kstrlcpy(p->alert,s->alert,sizeof(p->alert));kstrlcpy(p->nav,s->nav,sizeof(p->nav));
    kstrlcpy(p->nav_post_type,s->nav_post_type,sizeof(p->nav_post_type));
    p->nav_post_len=s->nav_post_len;p->nav_post=s->nav_post_len?(char*)r->navbody:NULL;
    if(p->nav_pending&&web_network_url(r->principal)&&!web_network_url(p->nav)&&strcmp(p->nav,"about:blank")){
        p->nav_pending=0;kstrlcpy(p->status,"Blocked navigation to a local or privileged URL",sizeof(p->status));
    }
}
static int call(remote_page_t* r,unsigned int op,const int* args,const char* url,const void* data,
                uint32_t n,void* out,uint32_t cap) {
    if(!r||r->dead||r->busy||n>(op==WW_MEDIA_OPEN?WW_MEDIA_BYTES:WW_DOCUMENT))return -1;
    r->busy=1;
    if(r->navbody)kfree(r->navbody);r->navbody=NULL;r->nav_used=0;r->p.nav_post=NULL;r->p.nav_post_len=0;
    memset(&r->cmd,0,sizeof(r->cmd));r->cmd.version=WW_VERSION;r->cmd.op=op;r->cmd.length=n;
    if(args)memcpy(r->cmd.a,args,10*sizeof(int));r->cmd.a[10]=r->p.view_h;
    r->cmd.a[11]=(r->p.nav_pending?1:0)|(r->p.alert_pending?2:0)|(r->p.file_pick_pending?4:0)|
        (r->p.status[0]?8:0)|(r->p.scroll_req>=0?16:0)|(r->p.dirty?32:0);
    if(url)kstrlcpy(r->cmd.url,url,sizeof(r->cmd.url));
    r->input=data;r->output=out;r->out_cap=cap;r->out_used=0;
    /* Updating layout can load new iframe documents and decode resources,
     * exactly as the initial document load does. Timers, eval and input keep
     * the shorter interaction budget; startup has its own initialization budget.
     * All commands remain bounded and throttled. */
    int loading=op==WW_LOAD||op==WW_DECODE||op==WW_UPDATE;
    uint32_t cpu_ms=loading?120000u:op==WW_INIT?30000u:10000u;
    uint32_t wall_ms=loading?300000u:op==WW_INIT?90000u:30000u;
    app_web_budget(r->app,r->generation,wall_ms,cpu_ms);
    __sync_synchronize();r->stage=1;uint32_t start=timer_ms();
    for(;;){
        uint32_t memory=app_web_memory(r->app,r->generation);
        if(memory>r->peak_memory)r->peak_memory=memory;
        if(r->resource_ready==1)serve(r);
        if(r->stage==3)break;
        int timed_out=timer_ms()-start>wall_ms+5000u;
        if(!app_web_alive(r->app,r->generation)||timed_out){
            int reason=app_web_stop_reason(r->app,r->generation);
            char why[128];
            if(reason==1)ksnprintf(why,sizeof(why),"Page stopped: CPU limit exceeded (%u seconds)",cpu_ms/1000u);
            else if(reason==2||timed_out)ksnprintf(why,sizeof(why),"Page stopped: wall time limit exceeded (%u seconds)",wall_ms/1000u);
            else kstrlcpy(why,"Page stopped: renderer terminated unexpectedly (possible memory exhaustion)",sizeof(why));
            crashed(r,why);r->busy=0;return -1;
        }
        task_sleep_ms(1);
    }
    __sync_synchronize();apply(r);r->stage=0;r->input=NULL;r->output=NULL;r->busy=0;
    app_web_budget(r->app,r->generation,0,0);
    return r->state.result;
}
static page_t* remote_page_new_worker(page_env_t* env,uint32_t limit,const char* path) {
    (void)limit;int slot=-1;
    for(int i=0;i<WORKERS;i++)if(!workers[i]){slot=i;break;}
    if(slot<0)return NULL;
    remote_page_t* r=kzalloc(sizeof(*r));if(!r)return NULL;
    r->environment=*env;r->slot=slot;r->app=-1;r->p.env=&r->environment;r->p.layout=&r->layout;
    r->layout.A=(arena_t*)r;r->p.scroll_req=-1;
    char err[128];
    r->app=app_spawn_web_worker(path,err,sizeof(err));
    if(r->app<0){klog("browser worker: %s\n",err);kfree(r);return NULL;}
    r->generation=app_web_generation(r->app);
    workers[slot]=r;
    if(call(r,WW_INIT,NULL,NULL,NULL,0,NULL,0)<0){remote_page_free(&r->p);return NULL;}
    return &r->p;
}
page_t* remote_page_new_libjs(page_env_t* env,uint32_t limit) {
    return remote_page_new_worker(env,limit,"/apps/libjsworker/app-x86_64");
}
page_t* remote_page_new(page_env_t* env,uint32_t limit) {
    return remote_page_new_worker(env,limit,"/apps/webworker/app-x86_64");
}
/* a media stream's decoder (apps/webmedia): restricted like the renderers */
void* media_worker_start(void) {
    int slot=-1;
    for(int i=0;i<WORKERS;i++)if(!workers[i]){slot=i;break;}
    if(slot<0)return NULL;
    remote_page_t* r=kzalloc(sizeof(*r));if(!r)return NULL;
    r->environment=*browser_env();r->slot=slot;r->app=-1;r->p.env=&r->environment;r->p.layout=&r->layout;
    r->layout.A=(arena_t*)r;r->p.scroll_req=-1;
    char err[128];
    r->app=app_spawn_web_worker("/apps/webmedia/app-x86_64",err,sizeof(err));
    if(r->app<0){klog("media worker: %s\n",err);kfree(r);return NULL;}
    r->generation=app_web_generation(r->app);
    workers[slot]=r;
    return r;
}
int media_worker_call(void* w,unsigned int op,const int* args,const void* in,uint32_t n,void* out,uint32_t cap,uint32_t* out_used) {
    remote_page_t* r=w;
    if(!r||r->dead)return -1;
    int a[10]={0};if(args)memcpy(a,args,sizeof(a));
    int rc=call(r,op,a,NULL,in,n,out,cap);
    if(out_used)*out_used=r->out_used;
    return r->dead?-1:rc;
}
void media_worker_stop(void* w){if(w)remote_page_free(&((remote_page_t*)w)->p);}
void remote_page_free(page_t* p) {
    if(!p)return;remote_page_t* r=(remote_page_t*)p;
    r->dead=1;app_web_kill(r->app,r->generation);
    media_close_owner(r);
    if(r->slot>=0&&workers[r->slot]==r)workers[r->slot]=NULL;
    /* Reclaim the old process before another page consumes its pool share.
     * The generation prevents a reused app slot from being killed here. */
    uint32_t start=timer_ms();
    while(app_web_alive(r->app,r->generation)&&timer_ms()-start<2000u)task_sleep_ms(1);
    if(r->navbody)kfree(r->navbody);if(r->request)kfree(r->request);if(r->resource_data)kfree(r->resource_data);
    kfree(r);
}
void remote_page_load(page_t* p,const char* url,const char* html,uint32_t len,int width) {
    remote_page_t* r=(remote_page_t*)p;
    r->network_enabled=1;
    kstrlcpy(r->principal,url,sizeof(r->principal));kstrlcpy(p->url,url,sizeof(p->url));
    if(!strncmp(url,"file://",7)&&strlen(url+7)<FS_PATH_LEN){kstrlcpy(r->grants[0],url+7,FS_PATH_LEN);r->ngrants=1;}
    int a[10]={width};call(r,WW_LOAD,a,url,html,len,NULL,0);
}
void remote_page_update(page_t* p,int width){int a[10]={width};call((remote_page_t*)p,WW_UPDATE,a,NULL,NULL,0,NULL,0);}
void remote_page_click(page_t* p,int x,int y){int a[10]={x,y};call((remote_page_t*)p,WW_CLICK,a,NULL,NULL,0,NULL,0);}
int remote_page_key(page_t* p,char c){int a[10]={(unsigned char)c};return call((remote_page_t*)p,WW_KEY,a,NULL,NULL,0,NULL,0)>0;}
int remote_page_key_live(page_t* p,int down,const char* key,const char* code,int kc,int mods){
    int a[10]={down,kc,mods};char text[128]={0};kstrlcpy(text,key,64);kstrlcpy(text+64,code,64);
    return call((remote_page_t*)p,WW_LIVE,a,NULL,text,sizeof(text),NULL,0)>0;
}
int remote_page_tick(page_t* p){return call((remote_page_t*)p,WW_TICK,NULL,NULL,NULL,0,NULL,0)>0;}
int remote_page_link_at(page_t* p,int x,int y,char* out,int cap){
    int a[10]={x,y};char text[8192]={0};int rc=call((remote_page_t*)p,WW_LINK,a,NULL,NULL,0,text,sizeof(text));
    text[sizeof(text)-1]=0;if(cap>0)kstrlcpy(out,text,cap);return rc>0;
}
void remote_page_set_files(page_t* p,dom_node_t* node,const char* path){
    (void)node;remote_page_t* r=(remote_page_t*)p;
    if(path&&strlen(path)<FS_PATH_LEN&&r->ngrants<16)kstrlcpy(r->grants[r->ngrants++],path,FS_PATH_LEN);
    call(r,WW_PICK,NULL,NULL,path?path:"",path?strlen(path)+1:1,NULL,0);
}
int remote_page_eval(page_t* p,const char* code,char* out,int cap) {
    char* text=kzalloc(65536);if(!text)return -1;
    int rc=call((remote_page_t*)p,WW_EVAL,NULL,NULL,code,strlen(code),text,65536);
    if(out&&cap>0){text[65535]=0;kstrlcpy(out,text,cap);}kfree(text);return rc;
}
void remote_page_post(page_t* p,const char* text){call((remote_page_t*)p,WW_POSTMSG,NULL,NULL,text,strlen(text),NULL,0);}
static remote_page_t* from_layout(const layout_t* l){return l?(remote_page_t*)l->A:NULL;}
void remote_render(const layout_t* l,uint32_t* pixels,int stride,int bw,int bh,int vx,int vy,int vw,int vh,int scroll) {
    if(!pixels||stride<bw||vw<=0||vh<=0||vx<0||vy<0||vx>bw-vw||vy>bh-vh||(uint64_t)vw*vh>WW_PIXELS)return;
    remote_page_t* r=from_layout(l);
    for(int y=vy;y<vy+vh;y++)memset32(pixels+(size_t)y*stride+vx,0xffffff,vw);
    if(!r||r->dead)return;
    int a[10]={vw,vh,scroll,l->sel_on,l->sel_i0,l->sel_o0,l->sel_i1,l->sel_o1};
    uint32_t bytes=(uint32_t)vw*vh*4u;
    uint32_t* temp=(vx||vy||stride!=vw)?kmalloc(bytes):pixels;
    if(!temp)return;
    if(call(r,WW_RENDER,a,NULL,NULL,0,temp,bytes)==0&&r->out_used==bytes&&temp!=pixels)
        for(int y=0;y<vh;y++)memcpy(pixels+(size_t)(vy+y)*stride+vx,temp+(size_t)y*vw,(size_t)vw*4);
    if(temp!=pixels)kfree(temp);
}
int remote_text_pos(layout_t* l,int x,int y,int* item,int* off){
    int a[10]={x,y},pos[2]={0,0};int rc=call(from_layout(l),WW_TEXTPOS,a,NULL,NULL,0,pos,sizeof(pos));
    if(rc>0){*item=pos[0];*off=pos[1];}return rc>0;
}
uint32_t remote_text_range(layout_t* l,int i0,int o0,int i1,int o1,char* out,uint32_t cap){
    int a[10]={i0,o0,i1,o1};char* text=kzalloc(65536);if(!text)return 0;
    int n=call(from_layout(l),WW_TEXTRANGE,a,NULL,NULL,0,text,65536);text[65535]=0;
    if(n<0)n=0;if(cap)kstrlcpy(out,text,cap);kfree(text);return cap?(uint32_t)n<cap?(uint32_t)n:cap-1:0;
}

/* All nontrivial image parsing/decoding is user code, including SDK images
 * and wallpapers. The kernel validates only dimensions and byte counts. */
int web_worker_decode(const uint8_t* data,uint32_t len,image_t* out,char* err,uint32_t errcap) {
    memset(out,0,sizeof(*out));
    if(!len||len>WW_DOCUMENT){kstrlcpy(err,"Image input exceeds the 8 MiB quota",errcap);return -1;}
    page_t* p=remote_page_new(browser_env(),0);
    if(!p){kstrlcpy(err,"No isolated image worker is available (64-bit kernel required)",errcap);return -1;}
    remote_page_t* r=(remote_page_t*)p;
    uint8_t* pixels=kmalloc(WW_PIXELS*3u);
    if(!pixels){remote_page_free(p);kstrlcpy(err,"Not enough memory for image output",errcap);return -1;}
    int rc=call(r,WW_DECODE,NULL,NULL,data,len,pixels,WW_PIXELS*3u);
    int dims[2]={0,0};
    if(r->nav_used==sizeof(dims))memcpy(dims,r->navbody,sizeof(dims));
    uint64_t count=(uint64_t)(uint32_t)dims[0]*(uint32_t)dims[1];
    if(rc||dims[0]<=0||dims[1]<=0||dims[0]>8192||dims[1]>8192||
       count>WW_PIXELS||count*3u!=r->out_used){
        if(rc && r->out_used && r->out_used<=128 && !pixels[r->out_used-1])
            kstrlcpy(err,(const char*)pixels,errcap);
        else kstrlcpy(err,"Image decoder rejected data or exceeded its quota",errcap);
        kfree(pixels);remote_page_free(p);return -1;
    }
    uint8_t* smaller=krealloc(pixels,(size_t)count*3u);
    out->rgb=smaller?smaller:pixels;out->w=dims[0];out->h=dims[1];
    remote_page_free(p);return 0;
}

void web_isolation_tests(void (*report)(const char*,int,const char*)) {
    page_env_t* env=browser_env();
    page_t* canary=remote_page_new(env,0);
    report("isolated renderer starts",canary!=NULL,"256 MiB per worker, shared pool <= half free RAM");
    if(!canary)return;
    const char* doc="<html><head><title>Canary</title></head><body><form method=post action='/submit'><input name=q value=manual><button id=go type=submit>Go</button></form><p id=x>alive</p><script>var counter=7;</script></body></html>";
    remote_page_load(canary,"https://example.com/",doc,strlen(doc),640);
    report("HTML and JS run outside the kernel",!strcmp(canary->title,"Canary"),canary->status);
    char text[256]={0};
    int rc=remote_page_eval(canary,"counter+5",text,sizeof(text));
    report("JavaScript evaluation crosses IPC",!rc&&!strcmp(text,"12"),text);
    rc=remote_page_eval(canary,"document.title='Changed'; document.getElementById('x').textContent='updated'; 'ok'",text,sizeof(text));
    remote_page_update(canary,640);
    report("DOM updates cross IPC",!rc&&!strcmp(canary->title,"Changed"),canary->status);
    rc=remote_page_eval(canary,"document.getElementById('go').click(); 'submitted'",text,sizeof(text));
    report("scripted form clicks preserve POST bodies",!rc&&canary->nav_pending&&canary->nav_post_len&&
           strstr(canary->nav_post,"q=manual"),canary->nav_post_type);
    canary->nav_pending=0;
    remote_page_eval(canary,"location.href='file:///etc/hostname'; 'queued'",text,sizeof(text));
    report("remote page cannot navigate into local files",!canary->nav_pending,canary->status);
    rc=remote_page_eval(canary,"var secret=''; try { var x=new XMLHttpRequest(); x.open('GET','file:///etc/hostname',false); x.send(); secret=x.responseText; } catch(e){}; secret",text,sizeof(text));
    report("remote page cannot read arbitrary local files",rc==0&&text[0]==0,text);
    rc=remote_page_eval(canary,
        "var inbox=[]; onmessage=function(e){inbox.push(e.data.n+'|'+(e.data.self===e.data)+'|'+e.origin+'|'+(e.source===window)+'|'+(e.target===window)+'|'+(this===window));};"
        "var parcel={n:1};parcel.self=parcel;postMessage(parcel,'https://example.com:443/path');parcel.n=9;inbox.length",
        text,sizeof(text));
    if(!rc)rc=remote_page_eval(canary,"inbox.length",text,sizeof(text));
    report("postMessage delivery is asynchronous",!rc&&!strcmp(text,"0"),text);
    remote_page_tick(canary);
    rc=remote_page_eval(canary,"inbox.join(',')",text,sizeof(text));
    report("messages clone cycles and preserve window event identity",!rc&&
           !strcmp(text,"1|true|https://example.com|true|true|true"),text);
    remote_page_eval(canary,"postMessage('blocked','https://elsewhere.example');",text,sizeof(text));
    remote_page_tick(canary);
    rc=remote_page_eval(canary,"inbox.length",text,sizeof(text));
    report("message target origin is checked",!rc&&!strcmp(text,"1"),text);
    rc=remote_page_eval(canary,
        "var cloneError='';try{postMessage(function(){},'*');}catch(e){cloneError=e.name;}cloneError",
        text,sizeof(text));
    if(!rc)rc=remote_page_eval(canary,"cloneError",text,sizeof(text));
    report("uncloneable message fails explicitly",!rc&&!strcmp(text,"DataCloneError"),text);
    rc=remote_page_eval(canary,
        "var quota='';try{for(var mi=0;mi<65;mi++)postMessage(mi,'*');}catch(e){quota=String(e);}quota.indexOf('QuotaExceededError')>=0",
        text,sizeof(text));
    if(!rc)rc=remote_page_eval(canary,"quota.indexOf('QuotaExceededError')>=0",text,sizeof(text));
    report("asynchronous message queue is bounded",!rc&&!strcmp(text,"true"),text);
    remote_page_eval(canary,"onmessage=null",text,sizeof(text));
    remote_page_tick(canary);
    int a[10]={4};
    rc=call((remote_page_t*)canary,WW_TEST,a,NULL,NULL,0,NULL,0);
    report("file, command and process syscalls denied",rc==0,canary->status);
    a[0]=5;
    rc=call((remote_page_t*)canary,WW_TEST,a,NULL,NULL,0,NULL,0);
    report("unmapping anonymous pages returns their memory quota",rc==0,canary->status);
    for(int test=1;test<=3;test++){
        page_t* victim=remote_page_new(env,0);
        if(!victim){report("hostile worker starts",0,"no worker slot");break;}
        a[0]=test;
        rc=call((remote_page_t*)victim,WW_TEST,a,NULL,NULL,0,NULL,0);
        report(test==1?"worker CPU fault stays isolated":test==2?"infinite loop hits CPU/time quota":"memory exhaustion hits physical quota",
               rc<0&&((remote_page_t*)victim)->dead,victim->status);
        uint32_t ended=timer_ms();
        remote_page_t* stale=(remote_page_t*)victim;
        while(app_web_alive(stale->app,stale->generation)&&timer_ms()-ended<2000u)task_sleep_ms(1);
        page_t* replacement=remote_page_new(env,0);
        remote_page_free(victim);
        if(replacement){
            remote_page_load(replacement,"https://example.com/","<title>Replacement</title>",26,640);
            report("new renderer survives freeing a stale process proxy",
                   !strcmp(replacement->title,"Replacement"),replacement->status);
            remote_page_free(replacement);
        }else report("worker memory is reclaimed after termination",0,"cannot start replacement");
        rc=remote_page_eval(canary,"counter+1",text,sizeof(text));
        report("independent renderer survives hostile worker",!rc&&!strcmp(text,"8"),text);
    }
    static const uint8_t broken_png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,255,255,255,255};
    image_t image;char err[160];
    rc=web_worker_decode(broken_png,sizeof(broken_png),&image,err,sizeof(err));
    report("malformed decoder input stays isolated",rc<0&&!strstr(err,"available")&&!strstr(err,"memory"),err);
    rc=remote_page_eval(canary,"counter",text,sizeof(text));
    report("renderer survives a rejected image",!rc&&!strcmp(text,"7"),text);
    uint32_t* pixels=kmalloc(640u*200u*4u);
    if(pixels){remote_render(canary->layout,pixels,640,640,200,0,0,640,200,0);
        int painted=0;for(int i=0;i<640*200;i++)if(pixels[i]!=0xffffff){painted=1;break;}
        report("validated pixel rendering crosses IPC",painted,canary->status);kfree(pixels);}
    remote_page_free(canary);
}

/* Optional port probe. No Ladybird code is linked into the kernel or browser. */
void web_libjs_tests(void (*report)(const char*,int,const char*)) {
    page_t* p=remote_page_new_worker(browser_env(),0,"/apps/libjsprobe/app-x86_64");
    report("isolated LibJS prototype starts",p!=NULL,"requires the optional libjsprobe.bpk package");
    if(!p)return;
    remote_page_t* r=(remote_page_t*)p;
    char* results=kzalloc(8192);
    if(!results){remote_page_free(p);report("LibJS result buffer",0,"out of memory");return;}
    int rc=call(r,WW_LOAD,NULL,NULL,NULL,0,results,8192);
    results[8191]=0;
    int complete=!rc&&strstr(results,"ok LibJS suite passed\n")!=NULL;
    for(char* line=results;*line;){
        char* end=strchr(line,'\n');if(end)*end=0;
        if(!strncmp(line,"ok ",3))report(line+3,1,"LibJS user process");
        else if(!strncmp(line,"FAIL ",5))report(line+5,0,"LibJS user process");
        if(!end)break;
        line=end+1;
    }
    report("LibJS engine suite completed",complete,p->status);
    char note[160],text[256];
    ksnprintf(note,sizeof(note),"peak charged memory: %u KiB (sampled)",r->peak_memory/1024);
    report("LibJS memory footprint measured",r->peak_memory>0,note);
    kfree(results);
    rc=remote_page_eval(p,"1+2",text,sizeof(text));
    report("LibJS evaluation crosses bounded IPC",!rc&&!strcmp(text,"3"),text);
    rc=remote_page_eval(p,"new Set([2]).has(2)",text,sizeof(text));
    report("LibJS Set.has works through IPC",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"const = ",text,sizeof(text));
    report("LibJS syntax errors stay inside the worker",rc<0&&!r->dead,text);
    rc=remote_page_eval(p,"throw new Error('probe')",text,sizeof(text));
    report("LibJS thrown exceptions stay inside the worker",rc<0&&!r->dead,text);
    int a[10]={4};
    rc=call(r,WW_TEST,a,NULL,NULL,0,NULL,0);
    report("LibJS direct syscalls and broker file/network permissions enforced",rc==0,p->status);
    page_t* canary=remote_page_new(browser_env(),0);
    if(canary){
        const char* doc="<script>var canary=41;</script>";
        remote_page_load(canary,"https://example.com/",doc,strlen(doc),640);
        int alive_before=!r->dead;
        rc=remote_page_eval(p,"for(;;){}",text,sizeof(text));
        report("endless LibJS code hits the execution quota",alive_before&&rc<0&&r->dead,p->status);
        rc=remote_page_eval(canary,"canary+1",text,sizeof(text));
        report("another renderer survives hostile LibJS",!rc&&!strcmp(text,"42"),text);
        remote_page_free(canary);
    }else report("independent renderer starts alongside LibJS",0,"worker pool full");
    remote_page_free(p);
}

/* Host-requested selftest diagnostics, never exported to page JavaScript. */
int remote_page_frame_diagnostics(page_t* p,char* out,unsigned cap){
    int args[10]={6};if(!out||!cap)return -1;out[0]=0;
    int rc=call((remote_page_t*)p,WW_TEST,args,NULL,NULL,0,out,cap);
    out[cap-1]=0;return rc;
}
void web_libjs_dom_tests(void (*report)(const char*,int,const char*)) {
    page_t* p=remote_page_new_libjs(browser_env(),0);
    report("LibJS DOM renderer starts",p!=NULL,"optional libjsworker package");
    if(!p)return;
    const char* html="<html><head><title>LibJS DOM</title></head><body>"
        "<button id='go'>Go</button><div id='out'>before</div><script>"
        "globalThis.count=0;const values=new Set([3,3]);"
        "document.getElementById('out').textContent='ready '+values.size;"
        "document.getElementById('go').addEventListener('click',()=>{"
        "count++;document.getElementById('out').textContent='clicked '+count;});"
        "</script><script type='module'>throw new Error('module loader unavailable')</script>"
        "<script nomodule>globalThis.classicFallback=true;</script></body></html>";
    remote_page_load(p,"https://example.com/",html,strlen(html),640);
    char text[512];
    int rc=remote_page_eval(p,"document.getElementById('out').textContent",text,sizeof(text));
    report("LibJS script changes the rendered DOM",!rc&&!strcmp(text,"ready 1"),text);
    rc=remote_page_eval(p,"innerWidth===640&&innerHeight>0&&parent===window&&top===window",text,sizeof(text));
    report("live Window properties cross the DOM bridge",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"classicFallback",text,sizeof(text));
    report("classic fallback matches module-loader capability",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"window===globalThis&&document.getElementById('out')===document.querySelector('#out')",text,sizeof(text));
    report("DOM wrappers preserve node and Window identity",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"document.getElementById('go').click();document.getElementById('out').textContent",text,sizeof(text));
    report("LibJS event callback keeps its realm and closure",!rc&&!strcmp(text,"clicked 1"),text);
    rc=remote_page_eval(p,"(()=>{const n=document.createElement('span');n.id='added';n.textContent='new';document.body.appendChild(n);return document.getElementById('added').textContent})()",text,sizeof(text));
    report("LibJS creates and attaches actual DOM nodes",!rc&&!strcmp(text,"new"),text);
    rc=remote_page_eval(p,"globalThis.job=0;Promise.resolve(9).then(x=>{job=x;document.getElementById('out').textContent='job '+x});true",text,sizeof(text));
    remote_page_tick(p);
    rc=remote_page_eval(p,"job+'|'+document.getElementById('out').textContent",text,sizeof(text));
    report("LibJS promise jobs update the DOM",!rc&&!strcmp(text,"9|job 9"),text);
    rc=remote_page_eval(p,"globalThis.timerDone=0;setTimeout(()=>{timerDone=7;document.getElementById('out').setAttribute('data-timer','yes')},0);true",text,sizeof(text));
    remote_page_tick(p);
    rc=remote_page_eval(p,"timerDone+'|'+document.getElementById('out').getAttribute('data-timer')",text,sizeof(text));
    report("LibJS timers retain callbacks and modify attributes",!rc&&!strcmp(text,"7|yes"),text);
    rc=remote_page_eval(p,"(()=>{let n=0;const f=()=>n++;const e=document.getElementById('go');e.addEventListener('click',f);e.removeEventListener('click',f);e.click();return n===0})()",text,sizeof(text));
    report("event removal preserves LibJS callback identity",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{try{MutationObserver(()=>{});return false}catch(e){return String(e).includes('requires new')}})()",text,sizeof(text));
    report("DOM constructors reject calls without new",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"globalThis.observed='';globalThis.observer=new MutationObserver(records=>{observed=records[0].type+'|'+records[0].attributeName});observer.observe(document.getElementById('out'),{attributes:true});document.getElementById('out').setAttribute('data-observed','yes');true",text,sizeof(text));
    report("LibJS constructs native MutationObserver",!rc&&!strcmp(text,"true"),text);
    remote_page_tick(p);
    rc=remote_page_eval(p,"observed",text,sizeof(text));
    report("native mutation delivery calls back into LibJS",!rc&&!strcmp(text,"attributes|data-observed"),text);
    rc=remote_page_eval(p,"observer.disconnect();document.getElementById('out').setAttribute('data-observed','no');observed='disconnected';true",text,sizeof(text));
    remote_page_tick(p);
    rc=remote_page_eval(p,"observed",text,sizeof(text));
    report("constructed observers disconnect correctly",!rc&&!strcmp(text,"disconnected"),text);
    rc=remote_page_eval(p,"(()=>{const bytes=Array.from({length:256},(_,i)=>String.fromCharCode(i)).join('');return atob(btoa(bytes))===bytes&&atob('AP+A').charCodeAt(1)===255})()",text,sizeof(text));
    report("browser base64 preserves every byte including NUL",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"btoa(String.fromCharCode(233))==='6Q=='&&atob(' Y Q = = ')==='a'&&atob('YWI')==='ab'",text,sizeof(text));
    report("browser base64 uses Latin-1 and forgiving whitespace",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{let n=0;for(const f of [()=>atob('a'),()=>atob('YQ===') ,()=>btoa(String.fromCharCode(256))]){try{f()}catch(e){if(e.name==='InvalidCharacterError')n++}}return n===3})()",text,sizeof(text));
    report("browser base64 rejects malformed input and non Latin-1",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{const e=new TextEncoder,d=new TextDecoder,s='A'+String.fromCharCode(233,0)+String.fromCodePoint(0x1f34c);return d.decode(e.encode(s))===s})()",text,sizeof(text));
    report("LibJS text codecs preserve Unicode and embedded NUL",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{const a=new Uint8Array(5),r=new TextEncoder().encodeInto('A'+String.fromCodePoint(0x1f34c)+'B',a);const d=new TextDecoder;return r.read===3&&r.written===5&&d.decode(new Uint8Array([240,159]),{stream:true})===''&&d.decode(new Uint8Array([141,140]))===String.fromCodePoint(0x1f34c)})()",text,sizeof(text));
    report("LibJS UTF-8 streaming and encodeInto preserve character boundaries",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{const d=new TextDecoder('utf-8',{fatal:true});let failed=false;try{d.decode(new Uint8Array([192,128]))}catch(e){failed=e instanceof TypeError}return failed&&new TextDecoder().decode(new Uint8Array([239,187,191,65]))==='A'})()",text,sizeof(text));
    report("LibJS UTF-8 decoding handles BOM and fatal errors",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"globalThis.blocked=false;fetch('file:///etc/hostname').catch(()=>blocked=true);true",text,sizeof(text));
    remote_page_tick(p);
    rc=remote_page_eval(p,"blocked",text,sizeof(text));
    report("LibJS fetch promises preserve broker file permissions",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"(()=>{try{__bananaGet(NaN,'body');return false}catch(e){return e instanceof TypeError}})()",text,sizeof(text));
    report("forged DOM bridge indices are rejected",!rc&&!strcmp(text,"true"),text);
    rc=remote_page_eval(p,"document.getElementById('out').textContent='job 9';throw new Error('contained DOM exception')",text,sizeof(text));
    report("DOM script exceptions remain in the renderer",rc<0,text);
    rc=remote_page_eval(p,"document.getElementById('out').textContent",text,sizeof(text));
    report("DOM remains usable after an exception",!rc&&!strcmp(text,"job 9"),text);
    /* Elapsed-work checks need the guest monotonic clock. Date.now follows the
     * emulated RTC, which can drift/re-anchor independently under slow TCG. */
    rc=remote_page_eval(p,"(()=>{const end=performance.now()+6500;while(performance.now()<end){};return 42})()",text,sizeof(text));
    report("finite LibJS work survives the former short interaction quota",!rc&&!strcmp(text,"42"),rc?p->status:text);
    remote_page_update(p,640);
    uint32_t* pixels=kmalloc(640*160*4);
    if(pixels){
        remote_render(p->layout,pixels,640,640,160,0,0,640,160,0);
        int painted=0;for(int i=0;i<640*160;i++)if(pixels[i]!=0xffffff){painted=1;break;}
        report("LibJS DOM uses validated renderer pixel IPC",painted,p->status);kfree(pixels);
    }else report("LibJS render buffer",0,"out of memory");
    rc=remote_page_eval(p,
        "(()=>{const f=document.createElement('iframe');f.setAttribute('width','80');f.setAttribute('height','40');"
        "f.setAttribute('srcdoc','<body><script>const colors=new Map([[1,\"#1122ee\"]]);document.body.style.background=new TextDecoder().decode(new TextEncoder().encode(colors.get(1)))</script></body>');"
        "document.body.appendChild(f);return true})()",text,sizeof(text));
    remote_page_update(p,640);
    pixels=kmalloc(640*240*4);
    int frame_painted=0;
    if(pixels){
        remote_render(p->layout,pixels,640,640,240,0,0,640,240,0);
        for(int i=0;i<640*240;i++)if(pixels[i]==0x1122ee){frame_painted=1;break;}
        kfree(pixels);
    }
    report("layout updates execute new iframe documents in LibJS",!rc&&frame_painted,p->status);
    char memory_note[96];
    ksnprintf(memory_note,sizeof(memory_note),"%u KiB charged",((remote_page_t*)p)->peak_memory/1024);
    report("LibJS DOM memory accounting",((remote_page_t*)p)->peak_memory>0,memory_note);
    page_t* other=remote_page_new(browser_env(),0);
    if(other){
        const char* canary="<script>var answer=42;</script>";
        remote_page_load(other,"https://example.com/",canary,strlen(canary),640);
        rc=remote_page_eval(p,"for(;;){}",text,sizeof(text));
        report("hostile DOM JavaScript hits the CPU quota",rc<0&&((remote_page_t*)p)->dead&&strstr(p->status,"CPU limit exceeded"),p->status);
        rc=remote_page_eval(other,"answer",text,sizeof(text));
        report("independent renderer survives hostile LibJS DOM",!rc&&!strcmp(text,"42"),rc?other->status:text);
        remote_page_free(other);
    }else report("independent renderer available",0,"worker memory pool");
    remote_page_free(p);
}
