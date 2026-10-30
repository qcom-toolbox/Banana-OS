/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../kernel/image.h"
#include "image_decode.h"
#include "test/image_fixtures.h"
/* Hosted 32-bit tests have a decoder shim; the 32-bit kernel deliberately
 * cannot launch the isolated 64-bit image worker. Verify that safe refusal
 * instead of reporting missing worker support as seven decoder regressions. */
#if defined(__i386__) && !defined(FONT_HOST)
static void image_tests(checks_t* c) {
    image_t image; char err[128];
    int rc=image_decode(image_fixture_lossless,sizeof(image_fixture_lossless),&image,err,sizeof(err));
    check(c,"32-bit image decoding requires the isolated 64-bit worker",
          rc!=0 && !image.rgb && !image.w && !image.h && strstr(err,"64-bit"),err);
    if(!rc)image_free(&image);
}
#else
static int image_pixels(const uint8_t* data,uint32_t len,int tolerance) {
    image_t image;char err[128];
    if(image_decode(data,len,&image,err,sizeof(err)))return 0;
    int ok=image.w==4 && image.h==3;
    const int expected[]={37,103,211};
    if(ok)for(int i=0;i<36;i++) {
        int difference=(int)image.rgb[i]-expected[i%3];
        if(difference>tolerance || difference < -tolerance)ok=0;
    }
    image_free(&image);return ok && !image.rgb;
}
static void image_tests(checks_t* c) {
    check(c,"WebP lossy decoding and RGB pixels",image_pixels(image_fixture_lossy,sizeof(image_fixture_lossy),5),"");
    check(c,"WebP lossless decoding and exact RGB pixels",image_pixels(image_fixture_lossless,sizeof(image_fixture_lossless),0),"");
    check(c,"WebP alpha stream decodes to the browser's RGB format",image_pixels(image_fixture_alpha,sizeof(image_fixture_alpha),0),"");
    image_t image;char err[128];int ok=1;
    for(uint32_t n=0;n<sizeof(image_fixture_lossless)/2;n++) {
        int rc=image_decode(image_fixture_lossless,n,&image,err,sizeof(err));
        ok &= rc!=0 && !image.rgb && !image.w && !image.h;
        if(!rc)image_free(&image);
    }
    check(c,"truncated WebP is rejected without publishing pixels",ok,"");
    uint8_t oversized[sizeof(image_fixture_lossless)];memcpy(oversized,image_fixture_lossless,sizeof(oversized));
    /* VP8L's packed 14-bit width-minus-one starts just after its 0x2f signature. */
    oversized[21]=0;oversized[22]=(uint8_t)((oversized[22]&0xc0)|0x20);
    int too_big=image_decode(oversized,sizeof(oversized),&image,err,sizeof(err));
    check(c,"WebP dimensions are bounded before allocation",too_big!=0 && !image.rgb && strstr(err,"8192"),err);
    if(!too_big)image_free(&image);
    int rc=image_decode(image_fixture_animation,sizeof(image_fixture_animation),&image,err,sizeof(err));
    check(c,"animated WebP reports its unsupported format",rc!=0 && !image.rgb && strstr(err,"animated"),err);
    if(!rc)image_free(&image);
    arena_t A;arena_init(&A,32u<<20);img_data_t bitmap={0};
    int large=web_decode_image(NULL,image_fixture_retina,sizeof(image_fixture_retina),&bitmap,&A)==0;
    check(c,"browser decodes 2880x1800 WebP screenshots",large && bitmap.w==2880 && bitmap.h==1800 &&
        bitmap.px && bitmap.px[0]==0x2567d3 && bitmap.px[2880u*1800u-1]==0x2567d3,"");
    arena_free_all(&A);
    check(c,"PNG JPEG BMP and GIF still decode",image_pixels(image_fixture_png,sizeof(image_fixture_png),0) &&
        image_pixels(image_fixture_jpeg,sizeof(image_fixture_jpeg),5) && image_pixels(image_fixture_bmp,sizeof(image_fixture_bmp),0) &&
        image_pixels(image_fixture_gif,sizeof(image_fixture_gif),0),"");
}

#endif
