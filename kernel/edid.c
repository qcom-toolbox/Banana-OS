/* SPDX-License-Identifier: GPL-3.0-or-later */
/* EDID base blocks: the modes a monitor takes (kernel/edid.h). The VESA DMT
 * timings below are the standard's (Display Monitor Timing 1.0 rev 13). */
#include "edid.h"
#include "kstring.h"

int edid_valid(const uint8_t* e) {
    static const uint8_t HDR[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (memcmp(e, HDR, 8)) return 0;
    uint8_t s = 0;
    for (int i = 0; i < 128; i++) s = (uint8_t)(s + e[i]);
    return s == 0;
}

#define NH EDID_NHSYNC
#define NV EDID_NVSYNC
static const struct { uint32_t clock; uint16_t h[4], v[4]; uint8_t flags, hz; } DMT[] = {
    {  25175, {  640,  656,  752,  800 }, {  480,  490,  492,  525 }, NH | NV, 60 },
    {  31500, {  640,  664,  704,  832 }, {  480,  489,  492,  520 }, NH | NV, 72 },
    {  31500, {  640,  656,  720,  840 }, {  480,  481,  484,  500 }, NH | NV, 75 },
    {  28322, {  720,  738,  846,  900 }, {  400,  412,  414,  449 }, NH, 70 },
    {  36000, {  800,  824,  896, 1024 }, {  600,  601,  603,  625 }, 0, 56 },
    {  40000, {  800,  840,  968, 1056 }, {  600,  601,  605,  628 }, 0, 60 },
    {  50000, {  800,  856,  976, 1040 }, {  600,  637,  643,  666 }, 0, 72 },
    {  49500, {  800,  816,  896, 1056 }, {  600,  601,  604,  625 }, 0, 75 },
    {  65000, { 1024, 1048, 1184, 1344 }, {  768,  771,  777,  806 }, NH | NV, 60 },
    {  75000, { 1024, 1048, 1184, 1328 }, {  768,  771,  777,  806 }, NH | NV, 70 },
    {  78750, { 1024, 1040, 1136, 1312 }, {  768,  769,  772,  800 }, 0, 75 },
    { 108000, { 1152, 1216, 1344, 1600 }, {  864,  865,  868,  900 }, 0, 75 },
    {  74250, { 1280, 1390, 1430, 1650 }, {  720,  725,  730,  750 }, 0, 60 },
    {  83500, { 1280, 1352, 1480, 1680 }, {  800,  803,  809,  831 }, NH, 60 },
    { 108000, { 1280, 1376, 1488, 1800 }, {  960,  961,  964, 1000 }, 0, 60 },
    { 108000, { 1280, 1328, 1440, 1688 }, { 1024, 1025, 1028, 1066 }, 0, 60 },
    { 135000, { 1280, 1296, 1440, 1688 }, { 1024, 1025, 1028, 1066 }, 0, 75 },
    {  85500, { 1360, 1424, 1536, 1792 }, {  768,  771,  777,  795 }, 0, 60 },
    {  85500, { 1366, 1436, 1579, 1792 }, {  768,  771,  774,  798 }, 0, 60 },
    { 121750, { 1400, 1488, 1632, 1864 }, { 1050, 1053, 1057, 1089 }, NH, 60 },
    { 106500, { 1440, 1520, 1672, 1904 }, {  900,  903,  909,  934 }, NH, 60 },
    { 108000, { 1600, 1624, 1704, 1800 }, {  900,  901,  904, 1000 }, 0, 60 },
    { 162000, { 1600, 1664, 1856, 2160 }, { 1200, 1201, 1204, 1250 }, 0, 60 },
    { 146250, { 1680, 1784, 1960, 2240 }, { 1050, 1053, 1059, 1089 }, NH, 60 },
    { 148500, { 1920, 2008, 2052, 2200 }, { 1080, 1084, 1089, 1125 }, 0, 60 },
    { 154000, { 1920, 1968, 2000, 2080 }, { 1200, 1203, 1209, 1235 }, NV, 60 },
};
#define NDMT (int)(sizeof(DMT) / sizeof(DMT[0]))

static void from_dmt(int i, edid_mode_t* m) {
    memset(m, 0, sizeof(*m));
    m->clock = DMT[i].clock;
    m->hdisplay = DMT[i].h[0]; m->hsync_start = DMT[i].h[1]; m->hsync_end = DMT[i].h[2]; m->htotal = DMT[i].h[3];
    m->vdisplay = DMT[i].v[0]; m->vsync_start = DMT[i].v[1]; m->vsync_end = DMT[i].v[2]; m->vtotal = DMT[i].v[3];
    m->flags = DMT[i].flags;
    m->refresh = DMT[i].hz;
}

int edid_dmt(int w, int h, int hz, edid_mode_t* out) {
    if (!hz) hz = 60;
    for (int i = 0; i < NDMT; i++)
        if (DMT[i].h[0] == w && DMT[i].v[0] == h && DMT[i].hz == hz) { from_dmt(i, out); return 0; }
    return -1;
}

static int add(edid_mode_t* out, int n, int max, const edid_mode_t* m) {
    if (n >= max || !m->clock || !m->hdisplay || !m->vdisplay) return n;
    for (int i = 0; i < n; i++)
        if (out[i].hdisplay == m->hdisplay && out[i].vdisplay == m->vdisplay && out[i].refresh == m->refresh) return n;
    out[n] = *m;
    return n + 1;
}

/* an 18-byte detailed timing descriptor */
static int detailed(const uint8_t* d, edid_mode_t* m) {
    uint32_t clk = (uint32_t)(d[0] | d[1] << 8) * 10u;
    if (!clk) return 0;
    uint32_t ha = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 15) << 8;
    uint32_t va = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 15) << 8;
    uint32_t hso = d[8] | ((d[11] >> 6) & 3) << 8, hsw = d[9] | ((d[11] >> 4) & 3) << 8;
    uint32_t vso = (d[10] >> 4) | ((d[11] >> 2) & 3) << 4, vsw = (d[10] & 15) | (d[11] & 3) << 4;
    if (!ha || !va || !hb || !vb) return 0;
    memset(m, 0, sizeof(*m));
    m->clock = clk;
    m->hdisplay = (uint16_t)ha; m->hsync_start = (uint16_t)(ha + hso); m->hsync_end = (uint16_t)(ha + hso + hsw); m->htotal = (uint16_t)(ha + hb);
    m->vdisplay = (uint16_t)va; m->vsync_start = (uint16_t)(va + vso); m->vsync_end = (uint16_t)(va + vso + vsw); m->vtotal = (uint16_t)(va + vb);
    if (m->hsync_end > m->htotal) m->hsync_end = m->htotal;
    if (m->vsync_end > m->vtotal) m->vsync_end = m->vtotal;
    if (d[17] & 0x80) m->flags |= EDID_INTERLACE;
    if (!(d[17] & 0x02)) m->flags |= EDID_NHSYNC;    /* (as Linux's drm_edid: the bits, whatever the sync type) */
    if (!(d[17] & 0x04)) m->flags |= EDID_NVSYNC;
    uint64_t px = (uint64_t)m->htotal * m->vtotal;
    m->refresh = px ? (uint8_t)(((uint64_t)clk * 1000u + px / 2) / px) : 0;
    return 1;
}

int edid_modes(const uint8_t* e, edid_mode_t* out, int max) {
    int n = 0;
    edid_mode_t m;
    /* the detailed timings: the first is the preferred one */
    for (int i = 0; i < 4; i++)
        if (detailed(e + 54 + 18 * i, &m) && !(m.flags & EDID_INTERLACE)) {
            if (n == 0) m.flags |= EDID_PREFERRED;
            n = add(out, n, max, &m);
        }
    /* the established timings */
    static const struct { uint8_t byte, bit; uint16_t w, h; uint8_t hz; } EST[] = {
        { 35, 5, 640, 480, 60 }, { 35, 3, 640, 480, 72 }, { 35, 2, 640, 480, 75 }, { 35, 7, 720, 400, 70 },
        { 35, 1, 800, 600, 56 }, { 35, 0, 800, 600, 60 }, { 36, 7, 800, 600, 72 }, { 36, 6, 800, 600, 75 },
        { 36, 3, 1024, 768, 60 }, { 36, 2, 1024, 768, 70 }, { 36, 1, 1024, 768, 75 }, { 36, 0, 1280, 1024, 75 },
    };
    for (unsigned i = 0; i < sizeof(EST) / sizeof(EST[0]); i++)
        if ((e[EST[i].byte] >> EST[i].bit) & 1 && edid_dmt(EST[i].w, EST[i].h, EST[i].hz, &m) == 0) n = add(out, n, max, &m);
    /* the standard timings */
    for (int i = 0; i < 8; i++) {
        uint8_t a = e[38 + 2 * i], b = e[39 + 2 * i];
        if ((a == 1 && b == 1) || !a) continue;
        int w = (a + 31) * 8, h;
        switch (b >> 6) {
        case 0: h = e[18] == 1 && e[19] < 3 ? w : w * 10 / 16; break;    /* (16:10; 1:1 before EDID 1.3) */
        case 1: h = w * 3 / 4; break;
        case 2: h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        if (edid_dmt(w, h, (b & 63) + 60, &m) == 0) n = add(out, n, max, &m);
    }
    return n;
}

void edid_name(const uint8_t* e, char* out, int cap) {
    for (int i = 0; i < 4; i++) {
        const uint8_t* d = e + 54 + 18 * i;
        if (d[0] || d[1] || d[3] != 0xFC) continue;
        int k = 0;
        for (int j = 5; j < 18 && d[j] != 0x0A && k < cap - 1; j++) out[k++] = (char)(d[j] >= 32 && d[j] < 127 ? d[j] : '?');
        while (k > 0 && out[k - 1] == ' ') k--;
        out[k] = 0;
        return;
    }
    uint16_t id = (uint16_t)(e[8] << 8 | e[9]);
    char maker[4] = { (char)('@' + ((id >> 10) & 31)), (char)('@' + ((id >> 5) & 31)), (char)('@' + (id & 31)), 0 };
    ksnprintf(out, (size_t)cap, "%s %04x", maker, (unsigned)(e[10] | e[11] << 8));
}
