/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../kernel/remote_page.c"
extern int printf(const char*,...);
extern void exit(int);
extern void* malloc(size_t);
extern void free(void*);
static int checks,sleeps;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void* kmalloc(size_t n){return malloc(n);}
void kfree(void* p){free(p);}
void task_sleep_ms(uint32_t n){(void)n;sleeps++;}
static remote_page_t context;
static unsigned char input_data[64],out_data[64],wire[65536],reply[65536];
static void reset(void){
    if(context.navbody)kfree(context.navbody);
    if(context.request)kfree(context.request);
    memset(&context,0,sizeof(context));
    context.app=11;context.generation=17;context.stage=2;context.input=input_data;context.cmd.length=sizeof(input_data);
    context.output=out_data;context.out_cap=sizeof(out_data);
    workers[0]=&context;
}
int main(void){
    reset();
    CHECK(web_worker_ipc(11,18,WW_WAIT,NULL,0,reply,sizeof(ww_command_t))<0);
    CHECK(web_worker_ipc(12,17,WW_WAIT,NULL,0,reply,sizeof(ww_command_t))<0);
    CHECK(web_worker_ipc(11,17,WW_WAIT,NULL,0,reply,1)<0);
    context.stage=1;context.cmd.version=WW_VERSION;
    CHECK(web_worker_ipc(11,17,WW_WAIT,NULL,0,reply,sizeof(ww_command_t))==(int)sizeof(ww_command_t));
    CHECK(context.stage==2);
    CHECK(!web_worker_ipc(11,17,WW_WAIT,NULL,0,reply,sizeof(ww_command_t))&&sleeps==1);
    ww_io_t io={0,0};
    CHECK(web_worker_ipc(11,17,WW_INPUT,&io,sizeof(io),reply,64)==64);
    io.offset=63;CHECK(web_worker_ipc(11,17,WW_INPUT,&io,sizeof(io),reply,2)<0);
    io.offset=~0u;CHECK(web_worker_ipc(11,17,WW_INPUT,&io,sizeof(io),reply,1)<0);
    io.offset=0;memcpy(wire,&io,sizeof(io));memset(wire+sizeof(io),0xab,64);
    CHECK(web_worker_ipc(11,17,WW_OUTPUT,wire,sizeof(io)+64,NULL,0)==64);
    CHECK(out_data[0]==0xab&&out_data[63]==0xab);
    CHECK(web_worker_ipc(11,17,WW_OUTPUT,wire,sizeof(io)+1,NULL,0)<0);
    ww_state_t state;memset(&state,0,sizeof(state));state.version=WW_VERSION;
    state.height=500001;CHECK(web_worker_ipc(11,17,WW_FINISH,&state,sizeof(state),NULL,0)<0);
    state.height=100;memset(state.title,'x',sizeof(state.title));
    CHECK(web_worker_ipc(11,17,WW_FINISH,&state,sizeof(state),NULL,0)<0);
    state.title[127]=0;state.nav_post_len=1;
    CHECK(web_worker_ipc(11,17,WW_FINISH,&state,sizeof(state),NULL,0)<0);
    state.nav_post_len=0;state.nav_post_type[0]='\n';
    CHECK(web_worker_ipc(11,17,WW_FINISH,&state,sizeof(state),NULL,0)<0);
    state.nav_post_type[0]=0;
    CHECK(!web_worker_ipc(11,17,WW_FINISH,&state,sizeof(state),NULL,0)&&context.stage==3);
    CHECK(web_worker_ipc(11,17,WW_INPUT,&io,sizeof(io),reply,1)<0);
    reset();
    ww_resource_t* q=(ww_resource_t*)wire;memset(q,0,sizeof(*q));q->kind=WW_FETCH;
    q->length=WW_REQUEST_BODY+1;
    CHECK(web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)<0);
    q->length=0;memset(q->url,'x',sizeof(q->url));
    CHECK(web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)<0);
    q->url[0]=0;
    CHECK(!web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)&&context.resource_ready==1);
    CHECK(web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)<0);
    CHECK(!web_worker_ipc(11,17,WW_RESPONSE,NULL,0,reply,sizeof(ww_response_t)));
    context.resource_ready=2;
    CHECK(web_worker_ipc(11,17,WW_RESPONSE,NULL,0,reply,sizeof(ww_response_t))==(int)sizeof(ww_response_t));
    context.requests=512;
    CHECK(web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)<0);
    /* a playing video's pictures and commands are not downloads: not capped */
    q->kind=WW_MEDIA;
    CHECK(!web_worker_ipc(11,17,WW_RESOURCE,q,sizeof(*q),NULL,0)&&context.requests==512);
    q->kind=WW_FETCH;
    /* Fuzz op/length/offset/state combinations against the real endpoint. */
    unsigned random=17;
    for(int i=0;i<12000;i++){
        reset();
        for(unsigned k=0;k<sizeof(wire);k+=4){random=random*1664525u+1013904223u;memcpy(wire+k,&random,4);}
        unsigned len=(random>>8)%65536u,cap=(random>>16)%65536u;
        int op=(random%9u);
        web_worker_ipc(11,17,op,wire,len,reply,cap);
        CHECK(context.out_used<=context.out_cap&&context.nav_used<=WW_POST);
    }
    reset();
    printf("PASS: %d browser IPC boundary and malformed-message checks\n",checks);
    return 0;
}
