/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_decode.h"
#include "../kernel/image.h"
int web_decode_image(void* ctx,const uint8_t* data,uint32_t len,img_data_t* out,arena_t* A) {
    (void)ctx;
    image_t img; char err[128];
#ifdef BANANA_USER_WORKER
    const int ch=4;                     /* the isolated worker keeps transparency */
    if(image_decode_rgba(data,len,&img,err,sizeof(err)))return -1;
#else
    const int ch=3;
    if(image_decode(data,len,&img,err,sizeof(err)))return -1;
#endif
    /* Retina screenshots (e.g. 2880x1800) exceed the old four-million-pixel
     * ceiling. Keep a finite cap and charge the pixels to the page arena. */
    if((uint64_t)img.w*img.h>8000000u) { image_free(&img);return -1; }
    uint32_t n=(uint32_t)img.w*(uint32_t)img.h;
    uint32_t* px=arena_alloc(A,n*4);
    if(A->oom) { image_free(&img);return -1; }
    int see_through=0;
    for(uint32_t i=0;i<n;i++) {
        if(!(i&16383))WEB_TICK();
        const uint8_t* s=img.rgb+i*ch;
        px[i]=(uint32_t)s[0]<<16 | (uint32_t)s[1]<<8 | s[2];
        if(ch==4 && s[3]!=255)see_through=1;
    }
    /* transparent PNG / GIF / WebP: keep the coverage so the page shows through */
    uint8_t* alpha=NULL;
    if(see_through) {
        alpha=arena_alloc(A,n);
        if(A->oom) { image_free(&img);return -1; }
        for(uint32_t i=0;i<n;i++)alpha[i]=img.rgb[i*ch+3];
    }
    out->px=px;out->alpha=alpha;out->w=img.w;out->h=img.h;
    image_free(&img);return 0;
}
