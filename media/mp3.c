/* MP3: MPEG-1, MPEG-2 and MPEG-2.5 Layer III. Frames, side information
 * and the bit reservoir; MPEG-1 and LSF scalefactors; Huffman decoding;
 * requantization; mid/side and intensity stereo; short block reordering,
 * alias reduction, IMDCT with overlap-add and the polyphase synthesis
 * filterbank. Xing/Info and LAME headers give the length and the encoder
 * delay / padding (gapless playback). Tables: mp3tab.h. */
#include "codec.h"
#include "mp3tab.h"

/* the kernel builds without SSE: x87 long doubles */
typedef long double real;

int mp3_gapless = 1;                /* (host tests can turn trimming off) */

/* ── the maths for the tables (no libm in the kernel) ── */
static const real PI = 3.14159265358979323846264338327950288L;

static real r_sin(real x) {
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    real term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 30; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
static real r_cos(real x) { return r_sin(x + PI / 2); }
static real r_sqrt(real x) {
    if (x <= 0) return 0;
    real r = x > 1 ? x : 1;
    for (int i = 0; i < 100; i++) r = (r + x / r) / 2;
    return r;
}
static real r_cbrt(real x) {
    if (x <= 0) return 0;
    real r = x > 1 ? x : 1;
    for (int i = 0; i < 200; i++) r = (2 * r + x / (r * r)) / 3;
    return r;
}

static int  g_ready;
static real g_pow43[8207];          /* n^(4/3) */
static real g_cos36[36][18], g_cos12[12][6];
static real g_win[4][36];           /* IMDCT windows: normal, start, short, stop */
static real g_N[64][32];            /* synthesis matrixing */
static real g_D[512];               /* synthesis window */
static real g_cs[8], g_ca[8];       /* alias reduction butterflies */
static real g_tan_l[7], g_tan_r[7]; /* MPEG-1 intensity stereo */
static real g_q4[4];                /* 2^(k/4) */

static void init_tables(void) {
    if (g_ready) return;
    for (int n = 0; n < 8207; n++) g_pow43[n] = (real)n * r_cbrt((real)n);
    for (int i = 0; i < 36; i++)
        for (int k = 0; k < 18; k++) g_cos36[i][k] = r_cos(PI / 72 * (2 * i + 1 + 18) * (2 * k + 1));
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++) g_cos12[i][k] = r_cos(PI / 24 * (2 * i + 1 + 6) * (2 * k + 1));
    for (int i = 0; i < 36; i++) {
        real s36 = r_sin(PI / 36 * (i + (real)0.5));
        g_win[0][i] = s36;
        g_win[1][i] = i < 18 ? s36 : i < 24 ? 1 : i < 30 ? r_sin(PI / 12 * (i - 18 + (real)0.5)) : 0;
        g_win[3][i] = i < 6 ? 0 : i < 12 ? r_sin(PI / 12 * (i - 6 + (real)0.5)) : i < 18 ? 1 : s36;
        g_win[2][i] = i < 12 ? r_sin(PI / 12 * (i + (real)0.5)) : 0;
    }
    for (int k = 0; k < 64; k++)
        for (int i = 0; i < 32; i++) g_N[k][i] = r_cos((16 + k) * (2 * i + 1) * PI / 64);
    for (int i = 0; i < 512; i++) g_D[i] = (real)mp3_window[i] / 65536;
    static const real c[8] = { -0.6L, -0.535L, -0.33L, -0.185L, -0.095L, -0.041L, -0.0142L, -0.0037L };
    for (int i = 0; i < 8; i++) {
        real sq = r_sqrt(1 + c[i] * c[i]);
        g_cs[i] = 1 / sq;
        g_ca[i] = c[i] / sq;
    }
    for (int i = 0; i < 7; i++) {                    /* tan(i pi / 12) */
        real t = i == 6 ? 0 : r_sin(i * PI / 12) / r_cos(i * PI / 12);
        if (i == 6) { g_tan_l[i] = 1; g_tan_r[i] = 0; continue; }   /* tan = infinity */
        g_tan_l[i] = t / (1 + t);
        g_tan_r[i] = 1 / (1 + t);
    }
    g_q4[0] = 1;
    g_q4[1] = r_sqrt(r_sqrt(2));
    g_q4[2] = r_sqrt(2);
    g_q4[3] = g_q4[1] * g_q4[2];
    g_ready = 1;
}

/* 2^(q/4) */
static real pow2q(int q) {
    int e = q >> 2;                                  /* floor */
    real v = g_q4[q & 3];
    while (e > 0) { v *= 2; e--; }
    while (e < 0) { v /= 2; e++; }
    return v;
}

/* ── bits, most significant first ── */
typedef struct { const uint8_t* d; uint32_t len; uint32_t pos; } bits_t;   /* pos in bits */

static uint32_t peek(const bits_t* b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint32_t p = b->pos + (uint32_t)i;
        uint32_t bit = (p >> 3) < b->len ? (b->d[p >> 3] >> (7 - (p & 7))) & 1 : 0;
        v = (v << 1) | bit;
    }
    return v;
}
static uint32_t get(bits_t* b, int n) { uint32_t v = peek(b, n); b->pos += (uint32_t)n; return v; }

/* ── frames ── */
typedef struct {
    int ver;                        /* 1 MPEG-1, 2 MPEG-2, 3 MPEG-2.5 */
    int crc, pad, mode, modext, nch, rate, len, spf, row, side;
} hdr_t;

static int parse_header(const uint8_t* p, uint32_t avail, hdr_t* h) {
    if (avail < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
    int vb = (p[1] >> 3) & 3;
    if (vb == 1) return -1;
    if (((p[1] >> 1) & 3) != 1) return -1;           /* Layer III */
    int bri = p[2] >> 4, sri = (p[2] >> 2) & 3;
    if (bri == 0 || bri == 15 || sri == 3) return -1;   /* (free format: not handled) */
    static const int br1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int br2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    static const int rates[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
    h->ver = vb == 3 ? 1 : vb == 2 ? 2 : 3;
    h->crc = !(p[1] & 1);
    h->pad = (p[2] >> 1) & 1;
    h->mode = p[3] >> 6;
    h->modext = (p[3] >> 4) & 3;
    h->nch = h->mode == 3 ? 1 : 2;
    h->rate = rates[h->ver - 1][sri];
    int br = (h->ver == 1 ? br1 : br2)[bri] * 1000;
    h->len = (h->ver == 1 ? 144 : 72) * br / h->rate + h->pad;
    h->spf = h->ver == 1 ? 1152 : 576;
    int idx = sri + (h->ver == 1 ? 6 : h->ver == 2 ? 3 : 0);    /* the band table row */
    h->row = idx - (idx != 0);
    h->side = h->ver == 1 ? (h->nch == 1 ? 17 : 32) : (h->nch == 1 ? 9 : 17);
    return h->len >= 4 + h->side ? 0 : -1;
}

/* granule side information */
typedef struct {
    int part23, big_values, global_gain, sfc, block_type, mixed;
    int table[3], subgain[3], region0, region1, preflag, sfscale, count1sel;
    int scfsi[4];
} gran_t;

typedef struct {
    codec_t  c;
    const uint8_t* d;
    uint32_t len, first, pos;
    hdr_t    h0;                     /* the first frame's (rate, channels) */
    uint8_t  res[4096];             /* the bit reservoir */
    int      res_len;
    real     xr[2][576];
    real     overlap[2][32][18];
    real     V[2][1024];
    int      voff[2];
    uint8_t  sfl[2][22], sfs[2][13][3];   /* scalefactors (the previous granule's: scfsi) */
    int16_t  pcm[1152 * 2];
    int      pcm_n, pcm_at;
    uint32_t skip;                  /* samples to drop (encoder delay, a seek) */
    uint64_t out_frames;            /* handed out so far */
    uint64_t frames_total;          /* (Xing) */
    uint8_t  toc[100];
    int      have_toc;
    int      delay, padding;
} mp3_t;

/* the next frame header at or after pos, checked against the one after it */
static int sync(mp3_t* m, uint32_t* pos, hdr_t* h) {
    for (uint32_t p = *pos; p + 4 <= m->len; p++) {
        if (m->d[p] != 0xFF) continue;
        if (parse_header(m->d + p, m->len - p, h) != 0) continue;
        if (m->h0.rate && (h->rate != m->h0.rate || h->ver != m->h0.ver)) continue;
        uint32_t next = p + (uint32_t)h->len;
        hdr_t h2;
        if (next + 4 <= m->len && (parse_header(m->d + next, m->len - next, &h2) != 0 || h2.rate != h->rate)) continue;
        *pos = p;
        return 0;
    }
    return -1;
}

static void side_info(bits_t* b, const hdr_t* h, int* main_begin, gran_t g[2][2]) {
    int nch = h->nch, ngr = h->ver == 1 ? 2 : 1;
    int scfsi[2][4] = { { 0 } };
    if (h->ver == 1) {
        *main_begin = (int)get(b, 9);
        get(b, nch == 1 ? 5 : 3);
        for (int ch = 0; ch < nch; ch++) for (int k = 0; k < 4; k++) scfsi[ch][k] = (int)get(b, 1);
    } else {
        *main_begin = (int)get(b, 8);
        get(b, nch == 1 ? 1 : 2);
    }
    for (int gr = 0; gr < ngr; gr++)
        for (int ch = 0; ch < nch; ch++) {
            gran_t* q = &g[gr][ch];
            memset(q, 0, sizeof(*q));
            for (int k = 0; k < 4; k++) q->scfsi[k] = scfsi[ch][k];
            q->part23 = (int)get(b, 12);
            q->big_values = (int)get(b, 9);
            if (q->big_values > 288) q->big_values = 288;
            q->global_gain = (int)get(b, 8);
            q->sfc = (int)get(b, h->ver == 1 ? 4 : 9);
            if (get(b, 1)) {                         /* window switching */
                q->block_type = (int)get(b, 2);
                q->mixed = (int)get(b, 1);
                for (int k = 0; k < 2; k++) q->table[k] = (int)get(b, 5);
                for (int k = 0; k < 3; k++) q->subgain[k] = (int)get(b, 3);
                q->region0 = q->block_type == 2 && !q->mixed ? 8 : 7;
                q->region1 = 36;                     /* (the rest) */
                if (q->block_type == 0) q->block_type = 1;   /* (invalid: treat as start) */
            } else {
                for (int k = 0; k < 3; k++) q->table[k] = (int)get(b, 5);
                q->region0 = (int)get(b, 4);
                q->region1 = (int)get(b, 3);
            }
            if (h->ver == 1) q->preflag = (int)get(b, 1);
            q->sfscale = (int)get(b, 1);
            q->count1sel = (int)get(b, 1);
        }
}

/* ── scalefactors ── */
static const uint8_t PRETAB[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0 };

static void scalefactors_mpeg1(mp3_t* m, bits_t* b, gran_t* g, int gr, int ch) {
    static const uint8_t slen[16][2] = { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 3, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 },
                                         { 2, 1 }, { 2, 2 }, { 2, 3 }, { 3, 1 }, { 3, 2 }, { 3, 3 }, { 4, 2 }, { 4, 3 } };
    int s1 = slen[g->sfc][0], s2 = slen[g->sfc][1];
    if (g->block_type == 2) {
        if (g->mixed) {
            for (int sfb = 0; sfb < 8; sfb++) m->sfl[ch][sfb] = (uint8_t)get(b, s1);
            for (int sfb = 3; sfb < 12; sfb++)
                for (int w = 0; w < 3; w++) m->sfs[ch][sfb][w] = (uint8_t)get(b, sfb < 6 ? s1 : s2);
        } else {
            for (int sfb = 0; sfb < 12; sfb++)
                for (int w = 0; w < 3; w++) m->sfs[ch][sfb][w] = (uint8_t)get(b, sfb < 6 ? s1 : s2);
        }
        for (int w = 0; w < 3; w++) m->sfs[ch][12][w] = 0;
        return;
    }
    static const int bands[5] = { 0, 6, 11, 16, 21 };
    for (int k = 0; k < 4; k++) {
        if (gr == 1 && g->scfsi[k]) continue;        /* the first granule's are kept */
        for (int sfb = bands[k]; sfb < bands[k + 1]; sfb++) m->sfl[ch][sfb] = (uint8_t)get(b, k < 2 ? s1 : s2);
    }
    m->sfl[ch][21] = 0;
}

/* MPEG-2 LSF; *is_max gets the illegal intensity position per band (right channel, intensity stereo) */
static void scalefactors_lsf(mp3_t* m, bits_t* b, gran_t* g, int ch, int intensity, int* is_max) {
    static const uint8_t nr[6][3][4] = {
        { { 6, 5, 5, 5 }, { 9, 9, 9, 9 }, { 6, 9, 9, 9 } },
        { { 6, 5, 7, 3 }, { 9, 9, 12, 6 }, { 6, 9, 12, 6 } },
        { { 11, 10, 0, 0 }, { 18, 18, 0, 0 }, { 15, 18, 0, 0 } },
        { { 7, 7, 7, 0 }, { 12, 12, 12, 0 }, { 6, 15, 12, 0 } },
        { { 6, 6, 6, 3 }, { 12, 9, 9, 6 }, { 6, 12, 9, 6 } },
        { { 8, 8, 5, 0 }, { 15, 12, 9, 0 }, { 6, 18, 9, 0 } },
    };
    int slen[4] = { 0 }, t;
    int sfc = g->sfc;
    g->preflag = 0;
    if (!intensity) {
        if (sfc < 400) { slen[0] = (sfc >> 4) / 5; slen[1] = (sfc >> 4) % 5; slen[2] = (sfc & 15) >> 2; slen[3] = sfc & 3; t = 0; }
        else if (sfc < 500) { sfc -= 400; slen[0] = (sfc >> 2) / 5; slen[1] = (sfc >> 2) % 5; slen[2] = sfc & 3; t = 1; }
        else { sfc -= 500; slen[0] = sfc / 3; slen[1] = sfc % 3; g->preflag = 1; t = 2; }
    } else {
        sfc >>= 1;
        if (sfc < 180) { slen[0] = sfc / 36; slen[1] = (sfc % 36) / 6; slen[2] = (sfc % 36) % 6; t = 3; }
        else if (sfc < 244) { sfc -= 180; slen[0] = (sfc & 63) >> 4; slen[1] = (sfc & 15) >> 2; slen[2] = sfc & 3; t = 4; }
        else { sfc -= 244; slen[0] = sfc / 3; slen[1] = sfc % 3; t = 5; }
    }
    int bt = g->block_type == 2 ? (g->mixed ? 2 : 1) : 0;
    uint8_t vals[40];
    int maxv[40];
    int n = 0;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < nr[t][bt][k]; i++) {
            vals[n] = (uint8_t)get(b, slen[k]);
            maxv[n] = (1 << slen[k]) - 1;
            n++;
        }
    int i = 0;
    if (bt == 0) {
        for (int sfb = 0; sfb < 21; sfb++, i++) { m->sfl[ch][sfb] = i < n ? vals[i] : 0; if (is_max) is_max[sfb] = i < n ? maxv[i] : 0; }
        m->sfl[ch][21] = 0;
        if (is_max) is_max[21] = is_max[20];
    } else {
        int start = 0;
        if (bt == 2) { for (int sfb = 0; sfb < 6; sfb++, i++) { m->sfl[ch][sfb] = i < n ? vals[i] : 0; if (is_max) is_max[22 + sfb] = i < n ? maxv[i] : 0; } start = 3; }
        for (int sfb = start; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++, i++) { m->sfs[ch][sfb][w] = i < n ? vals[i] : 0; if (is_max) is_max[sfb * 3 + w] = i < n ? maxv[i] : 0; }
        for (int w = 0; w < 3; w++) { m->sfs[ch][12][w] = 0; if (is_max) is_max[36 + w] = is_max[33 + w]; }
    }
}

/* ── Huffman ── */
static void huff_pair(bits_t* b, int tab, int* x, int* y) {
    const int16_t* cb = mp3_huff + mp3_huff_index[tab];
    int w = 5;
    int leaf = cb[peek(b, w)];
    while (leaf < 0) {
        b->pos += (uint32_t)w;
        w = leaf & 7;
        leaf = cb[(int)peek(b, w) - (leaf >> 3)];
    }
    b->pos += (uint32_t)(leaf >> 8);
    *x = leaf & 15;
    *y = (leaf >> 4) & 15;
}

static int huff_value(bits_t* b, int v, int linbits) {
    if (v == 15 && linbits) v += (int)get(b, linbits);
    if (v && get(b, 1)) v = -v;
    return v;
}

/* the 576 quantized values of a granule; how many lines may be nonzero */
static int huffman(bits_t* b, const gran_t* g, const hdr_t* h, uint32_t end, int* is) {
    int row = h->row;
    /* region boundaries in lines */
    int r1, r2;
    if (g->block_type == 2 && !g->mixed) {           /* short: region 1 from the 4th band (3 bands x 3 windows) */
        int s = 0;
        for (int k = 0; k < 9; k++) s += mp3_sfb_short[row][k];
        r1 = s;
        r2 = 576;
    } else if (g->block_type != 0) {                 /* start / stop / mixed: from the 9th long band */
        int s = 0;
        for (int k = 0; k < 8; k++) s += mp3_sfb_long[row][k];
        r1 = s;
        r2 = 576;
    } else {
        int s = 0, k = 0;
        for (; k <= g->region0 && mp3_sfb_long[row][k]; k++) s += mp3_sfb_long[row][k];
        r1 = s;
        for (; k <= g->region0 + g->region1 + 1 && mp3_sfb_long[row][k]; k++) s += mp3_sfb_long[row][k];
        r2 = s;
    }
    int bv = g->big_values * 2;
    if (r1 > bv) r1 = bv;
    if (r2 > bv) r2 = bv;
    int i = 0;
    for (; i < bv && i < 576; i += 2) {
        int tab = g->table[i < r1 ? 0 : i < r2 ? 1 : 2];
        int x = 0, y = 0;
        if (tab) {
            huff_pair(b, tab, &x, &y);
            int lb = mp3_linbits[tab];
            x = huff_value(b, x, lb);
            y = huff_value(b, y, lb);
        }
        is[i] = x;
        is[i + 1] = y;
    }
    /* count1: quadruples of -1 / 0 / 1 */
    const uint8_t* cb = g->count1sel ? mp3_count1_b : mp3_count1_a;
    while (i + 4 <= 576 && b->pos < end) {
        uint32_t start = b->pos;
        int leaf = cb[peek(b, 4)];
        if (!(leaf & 8)) {
            int extra = leaf & 3;
            leaf = cb[(leaf >> 3) + (int)(peek(b, 4 + extra) & ((1u << extra) - 1))];
        }
        b->pos += (uint32_t)(leaf & 7);
        int v[4];
        for (int s = 0; s < 4; s++) {
            v[s] = (leaf & (128 >> s)) ? 1 : 0;
            if (v[s] && get(b, 1)) v[s] = -1;
        }
        if (b->pos > end) { b->pos = start; break; }   /* ran past the granule: the last one is not real */
        for (int s = 0; s < 4; s++) is[i + s] = v[s];
        i += 4;
    }
    int nz = i;
    for (; i < 576; i++) is[i] = 0;
    return nz;
}

/* ── requantization ── */
static void requantize(mp3_t* m, const gran_t* g, const hdr_t* h, int ch, const int* is) {
    int row = h->row;
    real* xr = m->xr[ch];
    int sfmul = 2 * (1 + g->sfscale);                /* scalefactor steps, in quarter powers of two */
    int base = g->global_gain - 210;
    int line = 0;
    if (g->block_type != 2) {
        for (int sfb = 0; sfb < 22 && line < 576; sfb++) {
            int w = mp3_sfb_long[row][sfb];
            if (!w) break;
            int pre = g->preflag ? PRETAB[sfb] : 0;
            real gain = pow2q(base - sfmul * (m->sfl[ch][sfb] + pre));
            for (int k = 0; k < w && line < 576; k++, line++) {
                int v = is[line];
                xr[line] = v >= 0 ? g_pow43[v > 8206 ? 8206 : v] * gain : -g_pow43[-v > 8206 ? 8206 : -v] * gain;
            }
        }
        for (; line < 576; line++) xr[line] = 0;
        return;
    }
    const uint8_t* widths = g->mixed ? mp3_sfb_mixed[row] : mp3_sfb_short[row];
    int nlong = g->mixed ? (h->ver == 1 ? 8 : 6) : 0;
    int k = 0;
    for (int sfb = 0; sfb < nlong && widths[k]; sfb++, k++) {
        int pre = g->preflag ? PRETAB[sfb] : 0;
        real gain = pow2q(base - sfmul * (m->sfl[ch][sfb] + pre));
        for (int j = 0; j < widths[k] && line < 576; j++, line++) {
            int v = is[line];
            xr[line] = v >= 0 ? g_pow43[v > 8206 ? 8206 : v] * gain : -g_pow43[-v > 8206 ? 8206 : -v] * gain;
        }
    }
    for (int sfb = g->mixed ? 3 : 0; sfb < 13 && widths[k]; sfb++)
        for (int w = 0; w < 3 && widths[k]; w++, k++) {
            real gain = pow2q(base - 8 * g->subgain[w] - sfmul * m->sfs[ch][sfb][w]);
            for (int j = 0; j < widths[k] && line < 576; j++, line++) {
                int v = is[line];
                xr[line] = v >= 0 ? g_pow43[v > 8206 ? 8206 : v] * gain : -g_pow43[-v > 8206 ? 8206 : -v] * gain;
            }
        }
    for (; line < 576; line++) xr[line] = 0;
}

/* ── stereo ── */
/* is_scale: MPEG-2 intensity_scale (the right channel's scalefac_compress & 1) */
static void stereo(mp3_t* m, const gran_t* g, const hdr_t* h, const int* is_max, int is_scale) {
    int ms = h->mode == 1 && (h->modext & 2), intensity = h->mode == 1 && (h->modext & 1);
    real* l = m->xr[0], *r = m->xr[1];
    int is_line = 576;                               /* intensity from here on */
    int row = h->row;
    if (intensity && !(g->block_type == 2 && g->mixed)) {
        if (g->block_type != 2) {
            /* the first band above the right channel's last nonzero line */
            int last = -1;
            for (int i = 575; i >= 0; i--) if (r[i] != 0) { last = i; break; }
            int line = 0, sfb = 0;
            while (sfb < 22 && mp3_sfb_long[row][sfb] && line + mp3_sfb_long[row][sfb] <= last) line += mp3_sfb_long[row][sfb++];
            if (last >= 0) { line += mp3_sfb_long[row][sfb]; sfb++; }
            is_line = line;
            for (; sfb < 22 && mp3_sfb_long[row][sfb]; sfb++) {
                int w = mp3_sfb_long[row][sfb];
                int pos = m->sfl[1][sfb < 21 ? sfb : 20];
                real kl, kr;
                if (h->ver == 1) {
                    if (pos >= 7) { line += w; continue; }
                    kl = g_tan_l[pos];
                    kr = g_tan_r[pos];
                } else {
                    if (is_max && pos == is_max[sfb < 21 ? sfb : 20]) { line += w; continue; }
                    real io = is_scale ? g_q4[2] : g_q4[1];   /* (2^(1/2) or 2^(1/4)) */
                    real f = 1;
                    for (int k = 0; k < (pos + 1) / 2; k++) f /= io;
                    if (pos & 1) { kl = f; kr = 1; } else { kl = 1; kr = f; }
                }
                for (int k = 0; k < w && line < 576; k++, line++) {
                    real v = l[line];
                    l[line] = v * kl;
                    r[line] = v * kr;
                }
            }
        } else {
            /* short blocks: per window, above the right channel's last nonzero band */
            int min_line = 576;
            for (int w = 0; w < 3; w++) {
                int line = 0, last_sfb = -1;
                for (int sfb = 0; sfb < 13; sfb++) {
                    int wd = mp3_sfb_short[row][sfb * 3];
                    int start = line + w * wd;
                    for (int k = 0; k < wd; k++) if (r[start + k] != 0) last_sfb = sfb;
                    line += 3 * wd;
                }
                line = 0;
                for (int sfb = 0; sfb < 13; sfb++) {
                    int wd = mp3_sfb_short[row][sfb * 3];
                    if (sfb > last_sfb) {
                        if (line < min_line) min_line = line;
                        int pos = m->sfs[1][sfb < 12 ? sfb : 11][w];
                        real kl, kr;
                        int ok = 1;
                        if (h->ver == 1) { if (pos >= 7) ok = 0; else { kl = g_tan_l[pos]; kr = g_tan_r[pos]; } }
                        else {
                            if (is_max && pos == is_max[(sfb < 12 ? sfb : 11) * 3 + w]) ok = 0;
                            real io = is_scale ? g_q4[2] : g_q4[1], f = 1;
                            for (int k = 0; k < (pos + 1) / 2; k++) f /= io;
                            if (pos & 1) { kl = f; kr = 1; } else { kl = 1; kr = f; }
                        }
                        if (ok) {
                            int start = line + w * wd;
                            for (int k = 0; k < wd; k++) { real v = l[start + k]; l[start + k] = v * kl; r[start + k] = v * kr; }
                        } else if (ms) {
                            int start = line + w * wd;
                            for (int k = 0; k < wd; k++) {
                                real a = l[start + k], b = r[start + k];
                                l[start + k] = (a + b) * g_q4[2] / 2;
                                r[start + k] = (a - b) * g_q4[2] / 2;
                            }
                        }
                    } else if (ms) {
                        int start = line + w * wd;
                        for (int k = 0; k < wd; k++) {
                            real a = l[start + k], b = r[start + k];
                            l[start + k] = (a + b) * g_q4[2] / 2;
                            r[start + k] = (a - b) * g_q4[2] / 2;
                        }
                    }
                    line += 3 * wd;
                }
            }
            return;                                  /* (mid/side done band by band above) */
        }
    }
    if (ms) {
        real s = g_q4[2] / 2;                        /* 1 / sqrt(2) */
        for (int i = 0; i < is_line; i++) {
            real a = l[i], b = r[i];
            l[i] = (a + b) * s;
            r[i] = (a - b) * s;
        }
        /* bands above is_line whose intensity position was illegal: mid/side too */
        if (is_line < 576 && g->block_type != 2) {
            int line = 0;
            for (int sfb = 0; sfb < 22 && mp3_sfb_long[row][sfb]; sfb++) {
                int w = mp3_sfb_long[row][sfb];
                if (line >= is_line) {
                    int pos = m->sfl[1][sfb < 21 ? sfb : 20];
                    int illegal = h->ver == 1 ? pos >= 7 : (is_max && pos == is_max[sfb < 21 ? sfb : 20]);
                    if (illegal)
                        for (int k = 0; k < w && line + k < 576; k++) {
                            real a = l[line + k], b = r[line + k];
                            l[line + k] = (a + b) * s;
                            r[line + k] = (a - b) * s;
                        }
                }
                line += w;
            }
        }
    }
}

/* ── short block reordering, alias reduction, IMDCT, synthesis ── */
static void reorder(real* xr, const gran_t* g, const hdr_t* h) {
    real tmp[576];
    const uint8_t* widths = g->mixed ? mp3_sfb_mixed[h->row] : mp3_sfb_short[h->row];
    int k = 0, line = 0;
    if (g->mixed) for (int sfb = 0; sfb < (h->ver == 1 ? 8 : 6); sfb++) line += widths[k++];
    while (widths[k] && line < 576) {
        int wd = widths[k];
        for (int w = 0; w < 3; w++)
            for (int i = 0; i < wd; i++) tmp[3 * i + w] = xr[line + w * wd + i];
        for (int i = 0; i < 3 * wd && line + i < 576; i++) xr[line + i] = tmp[i];
        line += 3 * wd;
        k += 3;
    }
}

static void antialias(real* xr, const gran_t* g) {
    int sbmax = g->block_type == 2 ? (g->mixed ? 2 : 0) : 32;
    for (int sb = 1; sb < sbmax; sb++)
        for (int i = 0; i < 8; i++) {
            real bu = xr[18 * sb - 1 - i], bd = xr[18 * sb + i];
            xr[18 * sb - 1 - i] = bu * g_cs[i] - bd * g_ca[i];
            xr[18 * sb + i] = bd * g_cs[i] + bu * g_ca[i];
        }
}

/* the granule's 576 lines (one channel) to 576 samples */
static void filterbank(mp3_t* m, const gran_t* g, int ch, int16_t* out, int stride) {
    real* xr = m->xr[ch];
    real s[18][32];                                  /* [time slot][subband] */
    for (int sb = 0; sb < 32; sb++) {
        int bt = (g->block_type == 2 && g->mixed && sb < 2) ? 0 : g->block_type;
        real* X = xr + sb * 18;
        real y[36];
        if (bt != 2) {
            for (int i = 0; i < 36; i++) {
                real sum = 0;
                for (int k = 0; k < 18; k++) sum += X[k] * g_cos36[i][k];
                y[i] = sum * g_win[bt][i];
            }
        } else {
            for (int i = 0; i < 36; i++) y[i] = 0;
            for (int w = 0; w < 3; w++)
                for (int i = 0; i < 12; i++) {
                    real sum = 0;
                    for (int k = 0; k < 6; k++) sum += X[3 * k + w] * g_cos12[i][k];
                    y[6 + 6 * w + i] += sum * g_win[2][i];
                }
        }
        for (int i = 0; i < 18; i++) {
            real v = y[i] + m->overlap[ch][sb][i];
            m->overlap[ch][sb][i] = y[i + 18];
            if ((sb & 1) && (i & 1)) v = -v;         /* frequency inversion */
            s[i][sb] = v;
        }
    }
    /* polyphase synthesis, 32 samples per time slot */
    real* V = m->V[ch];
    for (int t = 0; t < 18; t++) {
        m->voff[ch] = (m->voff[ch] - 64) & 1023;
        int o = m->voff[ch];
        for (int k = 0; k < 64; k++) {
            real sum = 0;
            for (int i = 0; i < 32; i++) sum += g_N[k][i] * s[t][i];
            V[(o + k) & 1023] = sum;
        }
        for (int j = 0; j < 32; j++) {
            real sum = 0;
            for (int i = 0; i < 8; i++) {
                sum += V[(o + 128 * i + j) & 1023] * g_D[64 * i + j];
                sum += V[(o + 128 * i + 96 + j) & 1023] * g_D[64 * i + 32 + j];
            }
            real v = sum * 32768;
            int iv = v >= 0 ? (int)(v + (real)0.5) : -(int)(-v + (real)0.5);
            out[(t * 32 + j) * stride] = (int16_t)(iv > 32767 ? 32767 : iv < -32768 ? -32768 : iv);
        }
    }
}

/* one frame into m->pcm; 0, or -1 at the end */
static int decode_frame(mp3_t* m) {
    hdr_t h;
    uint32_t p = m->pos;
    if (sync(m, &p, &h) != 0) return -1;
    m->pos = p + (uint32_t)h.len;
    const uint8_t* f = m->d + p;
    uint32_t flen = (uint32_t)h.len;
    if (p + flen > m->len) flen = m->len - p;
    uint32_t hl = 4 + (h.crc ? 2 : 0);
    if (hl + (uint32_t)h.side > flen) return -1;
    bits_t sb = { f + hl, (uint32_t)h.side, 0 };
    gran_t g[2][2];
    int main_begin;
    side_info(&sb, &h, &main_begin, g);
    /* the main data: this frame's, after what earlier frames left in the reservoir */
    uint32_t md = flen - hl - (uint32_t)h.side;
    int start = m->res_len - main_begin;
    if (m->res_len + (int)md > (int)sizeof(m->res)) {
        int drop = m->res_len + (int)md - (int)sizeof(m->res);
        memmove(m->res, m->res + drop, (size_t)(m->res_len - drop));
        m->res_len -= drop;
        start -= drop;
    }
    memcpy(m->res + m->res_len, f + hl + h.side, md);
    m->res_len += (int)md;
    int nch = h.nch, ngr = h.ver == 1 ? 2 : 1, outch = m->c.channels;
    m->pcm_n = ngr * 576;
    m->pcm_at = 0;
    if (start < 0) {                                 /* (after a seek: the data it needs is gone) */
        memset(m->pcm, 0, sizeof(m->pcm));
        return 0;
    }
    bits_t b = { m->res, (uint32_t)m->res_len, (uint32_t)start * 8 };
    int is[576];
    for (int gr = 0; gr < ngr; gr++) {
        int is_max[40];
        for (int ch = 0; ch < nch; ch++) {
            gran_t* q = &g[gr][ch];
            uint32_t part2 = b.pos, end = part2 + (uint32_t)q->part23;
            if (h.ver == 1) scalefactors_mpeg1(m, &b, q, gr, ch);
            else scalefactors_lsf(m, &b, q, ch, ch == 1 && h.mode == 1 && (h.modext & 1), ch == 1 ? is_max : NULL);
            huffman(&b, q, &h, end, is);
            requantize(m, q, &h, ch, is);
            b.pos = end;
        }
        if (nch == 2) stereo(m, &g[gr][0], &h, h.ver == 1 ? NULL : is_max, g[gr][1].sfc & 1);
        for (int ch = 0; ch < nch; ch++) {
            gran_t* q = &g[gr][ch];
            if (q->block_type == 2) reorder(m->xr[ch], q, &h);
            antialias(m->xr[ch], q);
        }
        for (int ch = 0; ch < nch && ch < outch; ch++) filterbank(m, &g[gr][ch], ch, m->pcm + gr * 576 * outch + ch, outch);
        if (outch == 2 && nch == 1)
            for (int i = 0; i < 576; i++) m->pcm[(gr * 576 + i) * 2 + 1] = m->pcm[(gr * 576 + i) * 2];
    }
    return 0;
}

static int mp3_decode(codec_t* c, int16_t* out, int max) {
    mp3_t* m = (mp3_t*)c;
    int n = 0;
    while (n < max) {
        if (c->length && m->out_frames >= c->length) break;   /* (gapless: the encoder's padding) */
        if (m->pcm_at >= m->pcm_n) {
            if (decode_frame(m) != 0) break;
            if (m->skip) {
                int s = m->skip < (uint32_t)m->pcm_n ? (int)m->skip : m->pcm_n;
                m->pcm_at = s;
                m->skip -= (uint32_t)s;
                continue;
            }
        }
        int take = m->pcm_n - m->pcm_at;
        if (take > max - n) take = max - n;
        if (c->length && m->out_frames + (uint64_t)take > c->length) take = (int)(c->length - m->out_frames);
        memcpy(out + n * c->channels, m->pcm + m->pcm_at * c->channels, (size_t)take * (size_t)c->channels * 2);
        m->pcm_at += take;
        n += take;
        m->out_frames += (uint64_t)take;
    }
    return n;
}

static void reset_state(mp3_t* m) {
    m->res_len = 0;
    memset(m->overlap, 0, sizeof(m->overlap));
    memset(m->V, 0, sizeof(m->V));
    m->voff[0] = m->voff[1] = 0;
    m->pcm_n = m->pcm_at = 0;
}

static int mp3_seek(codec_t* c, uint64_t frame) {
    mp3_t* m = (mp3_t*)c;
    reset_state(m);
    uint64_t want = frame + (mp3_gapless ? (uint64_t)m->delay + 529 : 0);   /* in decoder samples */
    /* frame by frame from the start (headers only: fast) to the one holding that sample;
     * a few frames earlier still, decoded and dropped, so the bit reservoir, the IMDCT
     * overlap and the synthesis history are as they would have been */
    uint64_t target = want / (uint64_t)m->h0.spf;
    uint64_t need = m->h0.spf == 576 ? 6 : 4;          /* (the reservoir reaches back up to 511 bytes) */
    uint64_t lead = target < need ? target : need;
    uint32_t p = m->first;
    uint64_t idx = 0;
    hdr_t h;
    while (idx + lead < target) {
        uint32_t q = p;
        if (sync(m, &q, &h) != 0) break;
        p = q + (uint32_t)h.len;
        idx++;
    }
    m->pos = p;
    m->skip = (uint32_t)(want - idx * (uint64_t)m->h0.spf);   /* the lead frame (silent) and the start of the wanted one */
    m->out_frames = frame;
    return 0;
}

static void mp3_close(codec_t* c) { kfree(c); }

codec_t* mp3_open(const uint8_t* d, uint32_t len, char* err, int ecap) {
    init_tables();
    mp3_t* m = (mp3_t*)kzalloc(sizeof(mp3_t));
    if (!m) { codec_err(err, ecap, "out of memory"); return NULL; }
    m->d = d;
    m->len = len;
    uint32_t start = 0;
    if (len >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {   /* an ID3v2 tag */
        start = (uint32_t)(d[6] & 0x7F) << 21 | (uint32_t)(d[7] & 0x7F) << 14 | (uint32_t)(d[8] & 0x7F) << 7 | (d[9] & 0x7F);
        start += 10 + ((d[5] & 0x10) ? 10 : 0);
        if (start > len) start = len;
    }
    hdr_t h;
    uint32_t p = start;
    if (sync(m, &p, &h) != 0) { kfree(m); codec_err(err, ecap, "no MP3 frames found"); return NULL; }
    m->h0 = h;
    m->first = p;
    m->pos = p;
    /* a Xing / Info frame: frame count, seek table, LAME delay / padding; it is silent - skipped */
    const uint8_t* x = d + p + 4 + (h.crc ? 2 : 0) + h.side;
    if (x + 8 <= d + len && (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0)) {
        uint32_t flags = (uint32_t)x[4] << 24 | (uint32_t)x[5] << 16 | (uint32_t)x[6] << 8 | x[7];
        const uint8_t* q = x + 8;
        if (flags & 1) { m->frames_total = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; q += 4; }
        if (flags & 2) q += 4;
        if ((flags & 4) && q + 100 <= d + len) { memcpy(m->toc, q, 100); m->have_toc = 1; q += 100; }
        if (flags & 8) q += 4;
        if (q + 24 <= d + len && (memcmp(q, "LAME", 4) == 0 || memcmp(q, "Lavf", 4) == 0 || memcmp(q, "Lavc", 4) == 0)) {
            m->delay = q[21] << 4 | q[22] >> 4;
            m->padding = (q[22] & 15) << 8 | q[23];
        }
        m->first = m->pos = p + (uint32_t)h.len;     /* the audio starts after it */
    }
    m->c.rate = h.rate;
    m->c.channels = h.nch;
    m->c.name = "MP3";
    if (m->frames_total) {
        uint64_t total = m->frames_total * (uint64_t)h.spf;
        if (mp3_gapless && total > (uint64_t)(m->delay + m->padding)) total -= (uint64_t)(m->delay + m->padding);
        m->c.length = total;
    } else {
        m->c.length = (uint64_t)(len - m->first) / (uint64_t)h.len * (uint64_t)h.spf;   /* (CBR estimate) */
        if (!mp3_gapless) m->c.length = 0;
    }
    if (mp3_gapless && m->frames_total) m->skip = (uint32_t)m->delay + 529;
    m->c.decode = mp3_decode;
    m->c.seek = mp3_seek;
    m->c.close = mp3_close;
    return &m->c;
}
