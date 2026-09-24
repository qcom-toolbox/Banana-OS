#include "image.h"
#include "kheap.h"
#include "kstring.h"

/* third_party/stb/stb_image_impl.c */
extern unsigned char* stbi_load_from_memory(const unsigned char* buf, int len, int* x, int* y,
                                            int* comp, int req_comp);
extern int  stbi_info_from_memory(const unsigned char* buf, int len, int* x, int* y, int* comp);
extern void stbi_image_free(void* p);
extern const char* stbi_failure_reason(void);

static const char* const g_mode_names[IMAGE_MODE_COUNT] = { "fill", "fit", "stretch", "center" };

const char* image_mode_name(image_mode_t m) {
    return (unsigned)m < IMAGE_MODE_COUNT ? g_mode_names[m] : "fill";
}

int image_mode_parse(const char* s, image_mode_t* out) {
    for (int i = 0; i < IMAGE_MODE_COUNT; i++) {
        if (strcasecmp(s, g_mode_names[i]) == 0) { *out = (image_mode_t)i; return 1; }
    }
    return 0;
}

const char* image_format(const uint8_t* d, uint32_t len) {
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "PNG";
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "JPEG";
    if (len >= 2 && d[0] == 'B' && d[1] == 'M') return "BMP";
    if (len >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return "GIF";
    return NULL;
}

int image_decode(const uint8_t* data, uint32_t len, image_t* out, char* err, uint32_t errlen) {
    memset(out, 0, sizeof(*out));
    const char* fmt = image_format(data, len);
    if (!fmt) {
        /* the classic mistake: saving an HTML error/redirect page */
        if (len > 0 && (data[0] == '<' || (len > 15 && strncasecmp((const char*)data, "<!doctype", 9) == 0)))
            ksnprintf(err, errlen, "not an image - looks like an HTML page (check the URL)");
        else
            ksnprintf(err, errlen, "unsupported file type (need PNG, JPEG, BMP or GIF)");
        return -1;
    }
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp)) {
        ksnprintf(err, errlen, "corrupt or unsupported %s (%s)", fmt, stbi_failure_reason());
        return -1;
    }
    /* decoding needs w*h*3 bytes at once: say so up front instead of
     * failing half way */
    uint64_t need = (uint64_t)w * (uint64_t)h * 3u;
    if (need > kheap_largest_free()) {
        ksnprintf(err, errlen, "%dx%d image needs %u MiB of RAM, only %u MiB free (use a smaller picture)",
                  w, h, (uint32_t)(need >> 20), kheap_largest_free() >> 20);
        return -1;
    }
    unsigned char* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
    if (!px) {
        ksnprintf(err, errlen, "could not decode %s: %s", fmt, stbi_failure_reason());
        return -1;
    }
    out->rgb = px;
    out->w = w;
    out->h = h;
    return 0;
}

void image_free(image_t* img) {
    if (img->rgb) stbi_image_free(img->rgb);
    img->rgb = NULL;
}

/* ── resampling ─────────────────────────────────────────────────────
 * Integer only (the kernel is built without SSE and never touches the
 * FPU). The source rectangle (sx, sy, sw, sh) of `src` is scaled into
 * the destination rectangle (dx, dy, tw, th) of the dw-wide dst buffer. */

/* shrinking: each output pixel is the average of the source block it
 * covers, so a 4000px photo doesn't turn into aliased noise at 800px */
static void scale_area(const image_t* s, int sx, int sy, int sw, int sh,
                       uint32_t* dst, int dw, int dx, int dy, int tw, int th) {
    int* x0 = (int*)kmalloc(sizeof(int) * (uint32_t)(tw + 1));
    if (!x0) return;
    for (int x = 0; x <= tw; x++) x0[x] = sx + (int)(((int64_t)x * sw) / tw);
    for (int y = 0; y < th; y++) {
        int ya = sy + (int)(((int64_t)y * sh) / th);
        int yb = sy + (int)(((int64_t)(y + 1) * sh) / th);
        if (yb <= ya) yb = ya + 1;
        uint32_t* out = dst + (uint32_t)(dy + y) * (uint32_t)dw + (uint32_t)dx;
        for (int x = 0; x < tw; x++) {
            int xa = x0[x], xb = x0[x + 1];
            if (xb <= xa) xb = xa + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = ya; yy < yb; yy++) {
                const uint8_t* p = s->rgb + ((uint32_t)yy * (uint32_t)s->w + (uint32_t)xa) * 3u;
                for (int xx = xa; xx < xb; xx++, p += 3) {
                    r += p[0]; g += p[1]; b += p[2];
                }
                n += (uint32_t)(xb - xa);
            }
            out[x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    kfree(x0);
}

/* enlarging: bilinear, 16.16 fixed point */
static void scale_bilinear(const image_t* s, int sx, int sy, int sw, int sh,
                           uint32_t* dst, int dw, int dx, int dy, int tw, int th) {
    int32_t step_x = tw > 1 ? (int32_t)(((int64_t)(sw - 1) << 16) / (tw - 1)) : 0;
    int32_t step_y = th > 1 ? (int32_t)(((int64_t)(sh - 1) << 16) / (th - 1)) : 0;
    for (int y = 0; y < th; y++) {
        int32_t fyf = y * step_y;
        int y0 = sy + (fyf >> 16), fy = fyf & 0xFFFF;
        int y1 = (y0 + 1 < sy + sh) ? y0 + 1 : y0;
        uint32_t* out = dst + (uint32_t)(dy + y) * (uint32_t)dw + (uint32_t)dx;
        const uint8_t* r0 = s->rgb + (uint32_t)y0 * (uint32_t)s->w * 3u;
        const uint8_t* r1 = s->rgb + (uint32_t)y1 * (uint32_t)s->w * 3u;
        for (int x = 0; x < tw; x++) {
            int32_t fxf = x * step_x;
            int x0 = sx + (fxf >> 16), fx = fxf & 0xFFFF;
            int x1 = (x0 + 1 < sx + sw) ? x0 + 1 : x0;
            const uint8_t *p00 = r0 + x0 * 3, *p10 = r0 + x1 * 3, *p01 = r1 + x0 * 3, *p11 = r1 + x1 * 3;
            uint32_t rgb = 0;
            for (int c = 0; c < 3; c++) {
                int top = p00[c] + (((p10[c] - p00[c]) * fx) >> 16);
                int bot = p01[c] + (((p11[c] - p01[c]) * fx) >> 16);
                rgb = (rgb << 8) | (uint32_t)((top + (((bot - top) * fy) >> 16)) & 0xFF);
            }
            out[x] = rgb;
        }
    }
}

static void scale_rect(const image_t* s, int sx, int sy, int sw, int sh,
                       uint32_t* dst, int dw, int dx, int dy, int tw, int th) {
    if (sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return;
    if (tw < sw && th < sh) scale_area(s, sx, sy, sw, sh, dst, dw, dx, dy, tw, th);
    else scale_bilinear(s, sx, sy, sw, sh, dst, dw, dx, dy, tw, th);
}

void image_render(const image_t* s, uint32_t* dst, int dw, int dh, image_mode_t mode, uint32_t bg) {
    int sw = s->w, sh = s->h;
    switch (mode) {
    case IMAGE_STRETCH:
        scale_rect(s, 0, 0, sw, sh, dst, dw, 0, 0, dw, dh);
        return;
    case IMAGE_FIT: {
        memset32(dst, bg, (uint32_t)dw * (uint32_t)dh);
        /* whichever side hits the screen edge first decides the scale */
        int tw = dw, th = (int)(((int64_t)sh * dw) / sw);
        if (th > dh) { th = dh; tw = (int)(((int64_t)sw * dh) / sh); }
        scale_rect(s, 0, 0, sw, sh, dst, dw, (dw - tw) / 2, (dh - th) / 2, tw, th);
        return;
    }
    case IMAGE_CENTER: {
        memset32(dst, bg, (uint32_t)dw * (uint32_t)dh);
        int cw = sw < dw ? sw : dw, ch = sh < dh ? sh : dh;
        int sx = (sw - cw) / 2, sy = (sh - ch) / 2;
        int dx = (dw - cw) / 2, dy = (dh - ch) / 2;
        for (int y = 0; y < ch; y++) {
            const uint8_t* p = s->rgb + ((uint32_t)(sy + y) * (uint32_t)sw + (uint32_t)sx) * 3u;
            uint32_t* out = dst + (uint32_t)(dy + y) * (uint32_t)dw + (uint32_t)dx;
            for (int x = 0; x < cw; x++, p += 3) out[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
        return;
    }
    case IMAGE_FILL:
    default: {
        /* crop the source to the screen's aspect ratio, centered */
        int cw = sw, ch = sh;
        if ((int64_t)sw * dh > (int64_t)sh * dw) cw = (int)(((int64_t)sh * dw) / dh);   /* too wide */
        else ch = (int)(((int64_t)sw * dh) / dw);                                      /* too tall */
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        scale_rect(s, (sw - cw) / 2, (sh - ch) / 2, cw, ch, dst, dw, 0, 0, dw, dh);
        return;
    }
    }
}
