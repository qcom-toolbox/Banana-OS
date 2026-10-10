/*
 * HID report descriptor parser (HID 1.11, section 6.2.2): short items,
 * global / local state, collections; each Input / Output / Feature main
 * item becomes fields (one per value, padding as one constant field).
 */
#include "hidparse.h"

#define MAX_USAGES 32
#define MAX_REPORTS 32

typedef struct {
    uint32_t page;
    int32_t  lmin, lmax, pmin, pmax;
    int      exp;
    uint32_t unit;
    uint32_t size, count;
    uint8_t  rid;
} glob_t;

static int32_t sext(uint32_t v, int bytes) {
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

/* physical extent of a length in 1/10 mm, from the physical range, unit and exponent */
static int32_t length_mm10(const glob_t* g) {
    int32_t span = g->pmax - g->pmin;
    if (span <= 0 || span > 1000000) return 0;
    int32_t v;                                   /* in 1/10 mm, before the exponent */
    uint32_t sys = g->unit & 0xF, len = (g->unit >> 4) & 0xF;
    if (len != 1) return 0;
    if (sys == 1) v = span * 100;       /* SI linear: centimetres */
    else if (sys == 3) v = span * 254;  /* English linear: inches */
    else return 0;
    int e = g->exp;
    while (e > 0 && v < 200000000) { v *= 10; e--; }
    while (e < 0) { v /= 10; e++; }
    return v;
}

int hid_parse(const uint8_t* d, int len, hid_field_t* out, int max) {
    glob_t g, stack[4];
    int sp = 0;
    uint32_t usages[MAX_USAGES];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    int have_range = 0;
    /* collections: the application's usage, and the finger number */
    uint32_t app = 0;
    int depth = 0, finger_depth = -1, finger = -1, nfingers = 0;
    /* bit offsets per (report ID, type) */
    struct { uint8_t rid, type; uint16_t bits; } off[MAX_REPORTS];
    int noff = 0;
    int n = 0;

    for (int i = 0; i < (int)sizeof(g); i++) ((uint8_t*)&g)[i] = 0;

    int p = 0;
    while (p < len) {
        uint8_t b = d[p++];
        if (b == 0xFE) {                         /* long item: skipped */
            if (p + 2 > len) return -1;
            p += 2 + d[p];
            continue;
        }
        int sz = b & 3;
        if (sz == 3) sz = 4;
        if (p + sz > len) return -1;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)d[p + k] << (8 * k);
        p += sz;
        int type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                         /* global */
            switch (tag) {
            case 0: g.page = v; break;
            case 1: g.lmin = sext(v, sz); break;
            case 2: g.lmax = sz ? sext(v, sz) : 0; break;
            case 3: g.pmin = sext(v, sz); break;
            case 4: g.pmax = sz ? sext(v, sz) : 0; break;
            case 5: g.exp = (v & 8) ? (int)(v & 0xF) - 16 : (int)(v & 0xF); break;
            case 6: g.unit = v; break;
            case 7: g.size = v; break;
            case 8: g.rid = (uint8_t)v; break;
            case 9: g.count = v; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
            continue;
        }
        if (type == 2) {                         /* local */
            uint32_t u = sz == 4 ? v : (g.page << 16 | v);
            if (tag == 0) { if (nusages < MAX_USAGES) usages[nusages++] = u; }
            else if (tag == 1) { umin = u; have_range |= 1; }
            else if (tag == 2) { umax = u; have_range |= 2; }
            continue;
        }
        if (type != 0) continue;

        /* main items */
        uint32_t cur_usage = nusages ? usages[0] : (have_range & 1 ? umin : 0);
        if (tag == 0xA) {                        /* Collection */
            depth++;
            if (v == 1) {                        /* application */
                app = cur_usage;
                nfingers = 0;
            } else if (cur_usage == HID_U(0x0D, 0x22) && finger_depth < 0) {
                finger_depth = depth;            /* a Finger */
                finger = nfingers++;
            }
        } else if (tag == 0xC) {                 /* End Collection */
            if (depth == finger_depth) { finger_depth = -1; finger = -1; }
            if (depth > 0) depth--;
        } else if (tag == 8 || tag == 9 || tag == 0xB) {
            uint8_t rtype = tag == 8 ? HID_INPUT : tag == 9 ? HID_OUTPUT : HID_FEATURE;
            int o;
            for (o = 0; o < noff; o++) if (off[o].rid == g.rid && off[o].type == rtype) break;
            if (o == noff) {
                if (noff == MAX_REPORTS) return -1;
                off[noff].rid = g.rid; off[noff].type = rtype; off[noff].bits = 0;
                noff++;
            }
            int32_t lmax = g.lmax;
            if (g.lmin >= 0 && lmax < g.lmin && g.size < 32) lmax = (int32_t)(((uint32_t)g.lmax) & ((1u << g.size) - 1));
            int32_t mm10 = length_mm10(&g);
            int constant = (v & 1) || (!nusages && !have_range);
            int variable = (v & 2) != 0;
            if (constant || !variable || g.count > 16) {
                /* padding, an array (key codes...) or a long blob (vendor
                 * data, firmware): one field */
                if (n < max) {
                    hid_field_t* f = &out[n++];
                    f->rid = g.rid; f->type = rtype; f->flags = (uint8_t)v;
                    f->finger = (int8_t)finger; f->app = app;
                    f->usage = constant ? 0 : cur_usage;
                    f->bit = off[o].bits; f->size = (uint16_t)(g.size * (constant || variable ? g.count : 1));
                    f->lmin = g.lmin; f->lmax = lmax; f->mm10 = 0;
                }
                off[o].bits += (uint16_t)(g.size * g.count);
            } else {
                for (uint32_t k = 0; k < g.count; k++) {
                    uint32_t u;
                    if ((int)k < nusages) u = usages[k];
                    else if (have_range == 3 && umin + k - (uint32_t)nusages <= umax) u = umin + k - (uint32_t)nusages;
                    else u = nusages ? usages[nusages - 1] : umax;
                    if (n < max) {
                        hid_field_t* f = &out[n++];
                        f->rid = g.rid; f->type = rtype; f->flags = (uint8_t)v;
                        f->finger = (int8_t)finger; f->app = app;
                        f->usage = u;
                        f->bit = off[o].bits; f->size = (uint16_t)g.size;
                        f->lmin = g.lmin; f->lmax = lmax; f->mm10 = mm10;
                    }
                    off[o].bits += (uint16_t)g.size;
                }
            }
        }
        /* local state ends with each main item */
        nusages = 0;
        have_range = 0;
        umin = umax = 0;
    }
    return n;
}

int32_t hid_value(const uint8_t* r, int rlen, const hid_field_t* f) {
    uint32_t v = 0;
    int size = f->size > 32 ? 32 : f->size;
    for (int i = 0; i < size; i++) {
        int bit = f->bit + i;
        if (bit / 8 >= rlen) break;
        if (r[bit / 8] & (1 << (bit % 8))) v |= 1u << i;
    }
    if (f->lmin < 0 && size > 0 && size < 32 && (v & (1u << (size - 1)))) v |= ~((1u << size) - 1);
    return (int32_t)v;
}

void hid_set_value(uint8_t* r, int rlen, const hid_field_t* f, uint32_t v) {
    int size = f->size > 32 ? 32 : f->size;
    for (int i = 0; i < size; i++) {
        int bit = f->bit + i;
        if (bit / 8 >= rlen) break;
        if (v & (1u << i)) r[bit / 8] |= (uint8_t)(1 << (bit % 8));
        else r[bit / 8] &= (uint8_t)~(1 << (bit % 8));
    }
}

int hid_report_len(const hid_field_t* f, int n, uint8_t rid, uint8_t type) {
    int bits = 0;
    for (int i = 0; i < n; i++)
        if (f[i].rid == rid && f[i].type == type && f[i].bit + f[i].size > bits) bits = f[i].bit + f[i].size;
    return (bits + 7) / 8;
}
