/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mode setting on Radeon DCE 4 / 5 (Evergreen, Northern Islands - kernel/
 * radeon.h): a mode on one CRTC and one output, through the video BIOS's
 * command tables, as Linux's atombios_crtc.c and atombios_encoders.c do it
 * (MIT): the output off, the CRTC locked and blanked, its pixel PLL
 * (AdjustDisplayPll, then the dividers, then SetPixelClock), the timing
 * (SetCRTC_UsingDTDTiming), the scanout surface and the line buffer, then
 * the encoder (DIG + UNIPHY transmitter for DVI / HDMI, the DAC for VGA)
 * and the CRTC back on.
 *
 * What is left out, on purpose: DisplayPort (link training - such outputs
 * keep the firmware's mode), spread spectrum (turned off), HDMI's own
 * signalling and audio (HDMI monitors get DVI signalling, which they all
 * take), deep colour (8 bits a component) and the second CRTC sharing a
 * PLL (one display). */
#include "radeon.h"
#include "../third_party/radeon/atom.h"
#include "edid.h"
#include "kstring.h"
#include "serial.h"

#define IDX(t) GetIndexIntoMasterTable(COMMAND, t)

/* the CRTCs' register blocks (bytes from CRTC 0's) */
static const uint32_t CRTC_OFF[6] = { 0, 0xC00, 0x9800, 0xA400, 0xB000, 0xBC00 };

#define GRPH_ENABLE                0x6800
#define GRPH_CONTROL               0x6804
#define GRPH_LUT_10BIT_BYPASS      0x6808
#define GRPH_SWAP_CONTROL          0x680c
#define GRPH_PRIMARY_SURFACE       0x6810
#define GRPH_SECONDARY_SURFACE     0x6814
#define GRPH_PITCH                 0x6818
#define GRPH_PRIMARY_SURFACE_HIGH  0x681c
#define GRPH_SECONDARY_SURFACE_HIGH 0x6820
#define GRPH_SURFACE_OFFSET_X      0x6824
#define GRPH_SURFACE_OFFSET_Y      0x6828
#define GRPH_X_START               0x682c
#define GRPH_Y_START               0x6830
#define GRPH_X_END                 0x6834
#define GRPH_Y_END                 0x6838
#define GRPH_UPDATE                0x6844
#define GRPH_UPDATE_LOCK           (1u << 16)
#define GRPH_FLIP_CONTROL          0x6848
#define DESKTOP_HEIGHT             0x6b04
#define DC_LB_MEMORY_SPLIT         0x6b0c
#define VIEWPORT_START             0x6d70
#define VIEWPORT_SIZE              0x6d74
#define CRTC_CONTROL               0x6e70
#define MASTER_UPDATE_MODE         0x6ef8
#define PIPE0_DMIF_BUFFER_CONTROL  0x0ca0
#define DMIF_BUFFERS_ALLOCATED(x)  ((uint32_t)(x) << 0)
#define DMIF_ALLOCATED_COMPLETED   (1u << 4)
static const uint32_t VGA_CONTROL[6] = { 0x330, 0x338, 0x3e0, 0x3e4, 0x3e8, 0x3ec };

#define GRPH_DEPTH_32BPP           (2u << 0)
#define GRPH_FORMAT_ARGB8888       (0u << 8)
#define GRPH_ARRAY_LINEAR_GENERAL  (0u << 20)

/* ── the PLL dividers (radeon_compute_pll_avivo) ───────────────────── */

#define PLL_USE_REF_DIV   (1u << 0)
#define PLL_USE_POST_DIV  (1u << 1)
#define PLL_USE_FRAC_FB   (1u << 2)

typedef struct {
    uint32_t flags, reference_div, post_div;
    uint32_t ref_freq, pll_out_min, pll_out_max;
    uint32_t min_post_div, max_post_div, min_ref_div, max_ref_div, min_fb_div, max_fb_div;
} pll_t;

static unsigned gcd_(unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a ? a : 1; }
static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned umax(unsigned a, unsigned b) { return a > b ? a : b; }
static unsigned div_up(unsigned a, unsigned b) { return (a + b - 1) / b; }

static void reduce_ratio(unsigned* nom, unsigned* den, unsigned nom_min, unsigned den_min) {
    unsigned t = gcd_(*nom, *den);
    *nom /= t;
    *den /= t;
    if (*nom < nom_min) { t = div_up(nom_min, *nom); *nom *= t; *den *= t; }
    if (*den < den_min) { t = div_up(den_min, *den); *nom *= t; *den *= t; }
}

static void fb_ref_div(unsigned nom, unsigned den, unsigned post_div, unsigned fb_div_max, unsigned ref_div_max,
                       unsigned* fb_div, unsigned* ref_div) {
    ref_div_max = umax(umin(100 / post_div, ref_div_max), 1u);
    *ref_div = umin(umax(den / post_div, 1u), ref_div_max);
    *fb_div = (nom * *ref_div * post_div + den / 2) / den;
    if (*fb_div > fb_div_max) {
        *ref_div = (*ref_div * fb_div_max) / *fb_div;
        *fb_div = fb_div_max;
    }
}

/* freq in kHz (the limits in 10 kHz); the dividers that make it (dot = ref * fb / (ref_div * post)) */
static void compute_pll(const pll_t* pll, uint32_t freq, uint32_t* fb_p, uint32_t* frac_p, uint32_t* ref_p, uint32_t* post_p) {
    unsigned frac = pll->flags & PLL_USE_FRAC_FB;
    unsigned target = frac ? freq : freq / 10;
    unsigned fb_min = pll->min_fb_div, fb_max = pll->max_fb_div;
    if (frac) { fb_min *= 10; fb_max *= 10; }
    unsigned ref_min = pll->flags & PLL_USE_REF_DIV ? pll->reference_div : pll->min_ref_div;
    unsigned ref_max = frac && (pll->flags & PLL_USE_REF_DIV) ? pll->reference_div : pll->max_ref_div;
    unsigned post_min, post_max;
    if (pll->flags & PLL_USE_POST_DIV) post_min = post_max = pll->post_div;
    else {
        unsigned vmin = pll->pll_out_min, vmax = pll->pll_out_max;
        if (frac) { vmin *= 10; vmax *= 10; }
        post_min = vmin / target;
        if (target * post_min < vmin) post_min++;
        if (post_min < pll->min_post_div) post_min = pll->min_post_div;
        post_max = vmax / target;
        if (target * post_max > vmax) post_max--;
        if (post_max > pll->max_post_div) post_max = pll->max_post_div;
    }
    unsigned nom = target, den = pll->ref_freq;
    reduce_ratio(&nom, &den, fb_min, post_min);
    unsigned best = post_max, diff_best = ~0u, fb, ref;
    for (unsigned post = post_min; post <= post_max; post++) {
        fb_ref_div(nom, den, post, fb_max, ref_max, &fb, &ref);
        unsigned got = (pll->ref_freq * fb) / (ref * post);
        unsigned diff = got > target ? got - target : target - got;
        if (diff <= diff_best) { best = post; diff_best = diff; }
    }
    fb_ref_div(nom, den, best, fb_max, ref_max, &fb, &ref);
    reduce_ratio(&fb, &ref, fb_min, ref_min);
    if (frac && (fb % 10)) {                         /* (high jitter with small fractional dividers) */
        fb_min = umax(fb_min, (9 - (fb % 10)) * 20 + 50);
        if (fb < fb_min) { unsigned t = div_up(fb_min, fb); fb *= t; ref *= t; }
    }
    *fb_p = frac ? fb / 10 : fb;
    *frac_p = frac ? fb % 10 : 0;
    *ref_p = ref;
    *post_p = best;
}

/* ── the pieces of a mode set ──────────────────────────────────────── */

typedef struct {
    radeon_t* r;
    int crtc, pll, dig;              /* dig: the DIG engine (0-5), -1 for the DAC */
    const rad_path_t* path;
    uint16_t enc;                    /* the encoder's object id */
    int enc_id;                      /* ENCODER_OBJECT_ID_* */
    int linkb, dual, mode;           /* the second link of the UNIPHY; dual-link DVI; ATOM_ENCODER_MODE_* */
    uint32_t clock;                  /* kHz */
} job_t;

static int exec(radeon_t* r, int index, void* ps) { return rad_atom_exec(r, index, ps, 64); }

static void crtc_lock(job_t* j, int lock) {
    uint32_t ps[16] = { 0 };
    ENABLE_CRTC_PS_ALLOCATION* a = (void*)ps;
    a->ucCRTC = (uint8_t)j->crtc;
    a->ucEnable = (uint8_t)lock;
    exec(j->r, IDX(UpdateCRTC_DoubleBufferRegisters), ps);
}

static void crtc_enable(job_t* j, int on) {
    uint32_t ps[16] = { 0 };
    ENABLE_CRTC_PS_ALLOCATION* a = (void*)ps;
    a->ucCRTC = (uint8_t)j->crtc;
    a->ucEnable = (uint8_t)on;
    exec(j->r, IDX(EnableCRTC), ps);
    memset(ps, 0, sizeof(ps));
    a->ucCRTC = (uint8_t)j->crtc;
    a->ucEnable = (uint8_t)on;
    exec(j->r, IDX(EnableCRTCMemReq), ps);           /* (DCE 3 - 5) */
}

static void crtc_blank(job_t* j, int blank) {
    uint32_t ps[16] = { 0 };
    BLANK_CRTC_PS_ALLOCATION* a = (void*)ps;
    a->ucCRTC = (uint8_t)j->crtc;
    a->ucBlanking = (uint8_t)blank;
    exec(j->r, IDX(BlankCRTC), ps);
}

/* spread spectrum on the PLL: off (atombios_crtc_program_ss, disable) */
static void ss_off(job_t* j) {
    uint32_t ps[16] = { 0 };
    if (j->r->family >= CHIP_BARTS) {                /* DCE 5 */
        ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3* a = (void*)ps;
        a->ucSpreadSpectrumType = j->pll == ATOM_PPLL1 ? ATOM_PPLL_SS_TYPE_V3_P1PLL : ATOM_PPLL_SS_TYPE_V3_P2PLL;
        a->ucEnable = ATOM_DISABLE;
    } else {
        ENABLE_SPREAD_SPECTRUM_ON_PPLL_V2* a = (void*)ps;
        a->ucSpreadSpectrumType = j->pll == ATOM_PPLL1 ? ATOM_PPLL_SS_TYPE_V2_P1PLL : ATOM_PPLL_SS_TYPE_V2_P2PLL;
        a->ucEnable = ATOM_DISABLE;
    }
    exec(j->r, IDX(EnableSpreadSpectrumOnPPLL), ps);
}

/* the pixel clock the PLL must make for this encoder (atombios_adjust_pll), and its flags */
static uint32_t adjust_pll(job_t* j, pll_t* pll) {
    uint32_t clock = j->clock;
    uint8_t frev, crev;
    if (rad_atom_cmd_rev(j->r, IDX(AdjustDisplayPll), &frev, &crev) != 0 || frev != 1) return clock;
    uint32_t ps[16] = { 0 };
    if (crev <= 2) {
        ADJUST_DISPLAY_PLL_PS_ALLOCATION* a = (void*)ps;
        a->usPixelClock = (uint16_t)(clock / 10);
        a->ucTransmitterID = (uint8_t)j->enc_id;
        a->ucEncodeMode = (uint8_t)j->mode;
        if (exec(j->r, IDX(AdjustDisplayPll), ps) == 0) clock = (uint32_t)a->usPixelClock * 10;
    } else if (crev == 3) {
        ADJUST_DISPLAY_PLL_PS_ALLOCATION_V3* a = (void*)ps;
        a->sInput.usPixelClock = (uint16_t)(clock / 10);
        a->sInput.ucTransmitterID = (uint8_t)j->enc_id;
        a->sInput.ucEncodeMode = (uint8_t)j->mode;
        if (j->dig >= 0) {
            a->sInput.ucDispPllConfig |= DISPPLL_CONFIG_COHERENT_MODE;
            if (j->dual) a->sInput.ucDispPllConfig |= DISPPLL_CONFIG_DUAL_LINK;
        }
        if (exec(j->r, IDX(AdjustDisplayPll), ps) == 0) {
            clock = a->sOutput.ulDispPllFreq * 10;
            if (a->sOutput.ucRefDiv) { pll->flags |= PLL_USE_FRAC_FB | PLL_USE_REF_DIV; pll->reference_div = a->sOutput.ucRefDiv; }
            if (a->sOutput.ucPostDiv) { pll->flags |= PLL_USE_FRAC_FB | PLL_USE_POST_DIV; pll->post_div = a->sOutput.ucPostDiv; }
        }
    }
    return clock;
}

static int set_pll(job_t* j, char* why, int cap) {
    const rad_clocks_t* c = &j->r->clk;
    pll_t pll;
    memset(&pll, 0, sizeof(pll));
    pll.ref_freq = c->ref_freq;
    pll.pll_out_min = c->pll_out_min;
    pll.pll_out_max = c->pll_out_max;
    pll.min_post_div = 2; pll.max_post_div = 0x7f;
    pll.min_ref_div = 2; pll.max_ref_div = 0x3ff;
    pll.min_fb_div = 4; pll.max_fb_div = 0x7ff;
    if (!pll.ref_freq || !pll.pll_out_max) { kstrlcpy(why, "the BIOS gives no PLL limits", (size_t)cap); return -1; }
    uint32_t adjusted = adjust_pll(j, &pll);
    uint32_t fb, frac, ref, post;
    compute_pll(&pll, adjusted, &fb, &frac, &ref, &post);
    ss_off(j);
    uint8_t frev, crev;
    if (rad_atom_cmd_rev(j->r, IDX(SetPixelClock), &frev, &crev) != 0 || frev != 1 || (crev != 5 && crev != 6)) {
        ksnprintf(why, (size_t)cap, "SetPixelClock %u.%u is not one of DCE 4/5's", frev, crev);
        return -1;
    }
    uint32_t ps[16] = { 0 };
    if (crev == 5) {
        PIXEL_CLOCK_PARAMETERS_V5* a = (void*)ps;
        a->ucCRTC = (uint8_t)j->crtc;
        a->usPixelClock = (uint16_t)(j->clock / 10);
        a->ucRefDiv = (uint8_t)ref;
        a->usFbDiv = (uint16_t)fb;
        a->ulFbDivDecFrac = frac * 100000u;
        a->ucPostDiv = (uint8_t)post;
        a->ucMiscInfo = 0;
        a->ucTransmitterID = (uint8_t)j->enc_id;
        a->ucEncoderMode = (uint8_t)j->mode;
        a->ucPpll = (uint8_t)j->pll;
    } else {
        PIXEL_CLOCK_PARAMETERS_V6* a = (void*)ps;
        a->ulDispEngClkFreq = (uint32_t)j->crtc << 24 | j->clock / 10;
        a->ucRefDiv = (uint8_t)ref;
        a->usFbDiv = (uint16_t)fb;
        a->ulFbDivDecFrac = frac * 100000u;
        a->ucPostDiv = (uint8_t)post;
        a->ucTransmitterID = (uint8_t)j->enc_id;
        a->ucEncoderMode = (uint8_t)j->mode;
        a->ucPpll = (uint8_t)j->pll;
    }
    if (exec(j->r, IDX(SetPixelClock), ps) != 0) { kstrlcpy(why, "SetPixelClock failed", (size_t)cap); return -1; }
    klog("radeon: CRTC %d pixel clock %u kHz (PLL %d: fb %u.%u ref %u post %u)\n", j->crtc, j->clock, j->pll, fb, frac, ref, post);
    return 0;
}

static int set_timing(job_t* j, const edid_mode_t* m) {
    uint32_t ps[16] = { 0 };
    SET_CRTC_USING_DTD_TIMING_PARAMETERS* a = (void*)ps;
    a->usH_Size = m->hdisplay;
    a->usH_Blanking_Time = (uint16_t)(m->htotal - m->hdisplay);
    a->usV_Size = m->vdisplay;
    a->usV_Blanking_Time = (uint16_t)(m->vtotal - m->vdisplay);
    a->usH_SyncOffset = (uint16_t)(m->hsync_start - m->hdisplay);
    a->usH_SyncWidth = (uint16_t)(m->hsync_end - m->hsync_start);
    a->usV_SyncOffset = (uint16_t)(m->vsync_start - m->vdisplay);
    a->usV_SyncWidth = (uint16_t)(m->vsync_end - m->vsync_start);
    uint16_t misc = 0;
    if (m->flags & EDID_NVSYNC) misc |= ATOM_VSYNC_POLARITY;
    if (m->flags & EDID_NHSYNC) misc |= ATOM_HSYNC_POLARITY;
    a->susModeMiscInfo.usAccess = misc;
    a->ucCRTC = (uint8_t)j->crtc;
    return exec(j->r, IDX(SetCRTC_UsingDTDTiming), ps);
}

/* the scanout surface: w x h XRGB8888, linear, at fb_mc (dce4_crtc_do_set_base) */
static void set_base(job_t* j, uint64_t fb_mc, int w, int h, int pitch_px) {
    radeon_t* r = j->r;
    uint32_t o = CRTC_OFF[j->crtc];
    for (int i = 0; i < 6; i++) rad_wr(r, VGA_CONTROL[i], 0);   /* (out of VGA mode) */
    rad_wr(r, GRPH_UPDATE + o, rad_rd(r, GRPH_UPDATE + o) | GRPH_UPDATE_LOCK);
    rad_wr(r, GRPH_FLIP_CONTROL + o, 0);
    rad_wr(r, GRPH_PRIMARY_SURFACE_HIGH + o, (uint32_t)(fb_mc >> 32));
    rad_wr(r, GRPH_SECONDARY_SURFACE_HIGH + o, (uint32_t)(fb_mc >> 32));
    rad_wr(r, GRPH_PRIMARY_SURFACE + o, (uint32_t)fb_mc & ~0xFFu);
    rad_wr(r, GRPH_SECONDARY_SURFACE + o, (uint32_t)fb_mc & ~0xFFu);
    rad_wr(r, GRPH_CONTROL + o, GRPH_DEPTH_32BPP | GRPH_FORMAT_ARGB8888 | GRPH_ARRAY_LINEAR_GENERAL);
    rad_wr(r, GRPH_SWAP_CONTROL + o, 0);
    rad_wr(r, GRPH_LUT_10BIT_BYPASS + o, 0);
    rad_wr(r, GRPH_SURFACE_OFFSET_X + o, 0);
    rad_wr(r, GRPH_SURFACE_OFFSET_Y + o, 0);
    rad_wr(r, GRPH_X_START + o, 0);
    rad_wr(r, GRPH_Y_START + o, 0);
    rad_wr(r, GRPH_X_END + o, (uint32_t)w);
    rad_wr(r, GRPH_Y_END + o, (uint32_t)h);
    rad_wr(r, GRPH_PITCH + o, (uint32_t)pitch_px);
    rad_wr(r, GRPH_ENABLE + o, 1);
    rad_wr(r, DESKTOP_HEIGHT + o, (uint32_t)h);
    rad_wr(r, VIEWPORT_START + o, 0);
    rad_wr(r, VIEWPORT_SIZE + o, (uint32_t)w << 16 | (uint32_t)h);
    rad_wr(r, MASTER_UPDATE_MODE + o, 0);
    rad_wr(r, GRPH_UPDATE + o, rad_rd(r, GRPH_UPDATE + o) & ~GRPH_UPDATE_LOCK);
}

/* the line buffer: all of the pair's when the other CRTC is off, else half
 * (evergreen_line_buffer_adjust; DCE 5 also has its DMIF buffers) */
static void line_buffer(job_t* j) {
    radeon_t* r = j->r;
    int other = j->crtc ^ 1;
    int other_on = (rad_rd(r, CRTC_CONTROL + CRTC_OFF[other]) & 1) != 0;
    uint32_t split = other_on ? 0 : 2, alloc = other_on ? 1 : 2;
    if (j->crtc & 1) split += 4;
    rad_wr(r, DC_LB_MEMORY_SPLIT + CRTC_OFF[j->crtc], split);
    if (r->family >= CHIP_BARTS) {
        uint32_t p = PIPE0_DMIF_BUFFER_CONTROL + (uint32_t)j->crtc * 0x20;
        rad_wr(r, p, DMIF_BUFFERS_ALLOCATED(alloc));
        for (int i = 0; i < 100000 && !(rad_rd(r, p) & DMIF_ALLOCATED_COMPLETED); i++) __asm__ volatile("pause");
    }
}

static void crtc_source(job_t* j) {
    uint32_t ps[16] = { 0 };
    SELECT_CRTC_SOURCE_PARAMETERS_V2* a = (void*)ps;
    a->ucCRTC = (uint8_t)j->crtc;
    a->ucEncodeMode = (uint8_t)j->mode;
    static const uint8_t DIG_IDS[6] = { ASIC_INT_DIG1_ENCODER_ID, ASIC_INT_DIG2_ENCODER_ID, ASIC_INT_DIG3_ENCODER_ID,
                                        ASIC_INT_DIG4_ENCODER_ID, ASIC_INT_DIG5_ENCODER_ID, ASIC_INT_DIG6_ENCODER_ID };
    a->ucEncoderID = j->dig >= 0 ? DIG_IDS[j->dig] : ASIC_INT_DAC1_ENCODER_ID;
    exec(j->r, IDX(SelectCRTC_Source), ps);
}

static void dac(job_t* j, int action) {
    uint32_t ps[16] = { 0 };
    DAC_ENCODER_CONTROL_PS_ALLOCATION* a = (void*)ps;
    a->ucAction = (uint8_t)action;
    a->ucDacStandard = ATOM_DAC1_PS2;
    a->usPixelClock = (uint16_t)(j->clock / 10);
    exec(j->r, j->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2 ? IDX(DAC2EncoderControl) : IDX(DAC1EncoderControl), ps);
}

static void dig_encoder(job_t* j, int action) {
    uint8_t frev, crev;
    if (rad_atom_cmd_rev(j->r, IDX(DIGxEncoderControl), &frev, &crev) != 0) return;
    uint32_t ps[16] = { 0 };
    int lanes = j->dual ? 8 : 4;
    if (crev >= 4) {
        DIG_ENCODER_CONTROL_PARAMETERS_V4* a = (void*)ps;
        a->ucAction = (uint8_t)action;
        a->usPixelClock = (uint16_t)(j->clock / 10);
        if (action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE) a->ucPanelMode = 0;
        else a->ucEncoderMode = (uint8_t)j->mode;
        a->ucLaneNum = (uint8_t)lanes;
        a->acConfig.ucDigSel = (uint8_t)j->dig;
        a->ucBitPerColor = PANEL_8BIT_PER_COLOR;
        a->ucHPD_ID = 0;
    } else {
        DIG_ENCODER_CONTROL_PARAMETERS_V3* a = (void*)ps;
        a->ucAction = (uint8_t)action;
        a->usPixelClock = (uint16_t)(j->clock / 10);
        if (action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE) a->ucPanelMode = 0;
        else a->ucEncoderMode = (uint8_t)j->mode;
        a->ucLaneNum = (uint8_t)lanes;
        a->acConfig.ucDigSel = (uint8_t)j->dig;
        a->ucBitPerColor = PANEL_8BIT_PER_COLOR;
    }
    exec(j->r, IDX(DIGxEncoderControl), ps);
}

static void transmitter(job_t* j, int action) {
    uint8_t frev, crev;
    if (rad_atom_cmd_rev(j->r, IDX(UNIPHYTransmitterControl), &frev, &crev) != 0) return;
    uint32_t ps[16] = { 0 };
    uint16_t clk = (uint16_t)((j->dual ? j->clock / 2 : j->clock) / 10);
    int sel = j->enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY ? 0 : j->enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 ? 1 : 2;
    if (crev >= 4) {
        DIG_TRANSMITTER_CONTROL_PARAMETERS_V4* a = (void*)ps;
        a->ucAction = (uint8_t)action;
        a->usPixelClock = clk;
        a->ucLaneNum = (uint8_t)(j->dual ? 8 : 4);
        a->acConfig.ucLinkSel = (uint8_t)j->linkb;
        a->acConfig.ucEncoderSel = (uint8_t)(j->dig & 1);
        a->acConfig.ucRefClkSource = (uint8_t)j->pll;
        a->acConfig.ucTransmitterSel = (uint8_t)sel;
        a->acConfig.fCoherentMode = 1;
        a->acConfig.fDualLinkConnector = (uint8_t)j->dual;
    } else {
        DIG_TRANSMITTER_CONTROL_PARAMETERS_V3* a = (void*)ps;
        a->ucAction = (uint8_t)action;
        a->usPixelClock = clk;
        a->ucLaneNum = (uint8_t)(j->dual ? 8 : 4);
        a->acConfig.ucLinkSel = (uint8_t)j->linkb;
        a->acConfig.ucEncoderSel = (uint8_t)(j->dig & 1);
        a->acConfig.ucRefClkSource = (uint8_t)j->pll;
        a->acConfig.ucTransmitterSel = (uint8_t)sel;
        a->acConfig.fCoherentMode = 1;
        a->acConfig.fDualLinkConnector = (uint8_t)j->dual;
    }
    exec(j->r, IDX(UNIPHYTransmitterControl), ps);
}

/* ── the output a path offers ──────────────────────────────────────── */

static int is_dig(int id) {
    return id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY || id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 ||
           id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY2;
}
static int is_dac(int id) {
    return id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1 || id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2;
}

int rad_kms_path_kind(const rad_path_t* p, int digital) {
    if (p->conn_type == CONNECTOR_OBJECT_ID_DISPLAYPORT || p->conn_type == CONNECTOR_OBJECT_ID_eDP) return RAD_OUT_DP;
    for (int i = 0; i < p->nenc; i++) {
        int id = (p->enc_obj[i] & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT;
        if (digital && is_dig(id)) return RAD_OUT_TMDS;
        if (!digital && is_dac(id)) return RAD_OUT_VGA;
    }
    for (int i = 0; i < p->nenc; i++) {
        int id = (p->enc_obj[i] & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT;
        if (is_dig(id)) return RAD_OUT_TMDS;
        if (is_dac(id)) return RAD_OUT_VGA;
    }
    return RAD_OUT_NONE;
}

/* the fastest pixel clock (kHz) the output takes */
uint32_t rad_kms_max_clock(radeon_t* r, const rad_path_t* p, int kind) {
    uint32_t bios = r->clk.max_pixel_clock * 10;
    uint32_t lim = kind == RAD_OUT_VGA ? 400000 :
                   (p->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I || p->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D) ? 330000 : 165000;
    return bios && bios < lim ? bios : lim;
}

int rad_kms_set(radeon_t* r, int crtc, int path, int digital, const edid_mode_t* m,
                uint64_t fb_mc, int w, int h, int pitch_px, char* why, int cap) {
    if (crtc < 0 || crtc > 5 || path < 0 || path >= r->npaths) { kstrlcpy(why, "no such CRTC or output", (size_t)cap); return -1; }
    job_t j;
    memset(&j, 0, sizeof(j));
    j.r = r;
    j.crtc = crtc;
    j.path = &r->paths[path];
    j.clock = m->clock;
    j.pll = (crtc & 1) ? ATOM_PPLL2 : ATOM_PPLL1;
    int kind = rad_kms_path_kind(j.path, digital);
    if (kind == RAD_OUT_DP) { kstrlcpy(why, "DisplayPort outputs keep the firmware's mode", (size_t)cap); return -1; }
    if (kind == RAD_OUT_NONE) { kstrlcpy(why, "the output has no encoder this driver drives", (size_t)cap); return -1; }
    if (m->clock > rad_kms_max_clock(r, j.path, kind)) { kstrlcpy(why, "the mode's pixel clock is too fast for the output", (size_t)cap); return -1; }
    for (int i = 0; i < j.path->nenc; i++) {
        int id = (j.path->enc_obj[i] & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT;
        if ((kind == RAD_OUT_TMDS && is_dig(id)) || (kind == RAD_OUT_VGA && is_dac(id))) { j.enc = j.path->enc_obj[i]; j.enc_id = id; }
    }
    if (kind == RAD_OUT_TMDS) {
        j.linkb = ((j.enc & ENUM_ID_MASK) >> ENUM_ID_SHIFT) == 2;
        int base = j.enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY ? 0 : j.enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 ? 2 : 4;
        j.dig = base + j.linkb;
        j.dual = m->clock > 165000;
        j.mode = ATOM_ENCODER_MODE_DVI;
    } else {
        j.dig = -1;
        j.mode = ATOM_ENCODER_MODE_CRT;
    }
    /* the output off; the CRTC locked and off */
    if (j.dig >= 0) transmitter(&j, ATOM_TRANSMITTER_ACTION_DISABLE);
    else dac(&j, ATOM_DISABLE);
    crtc_source(&j);
    crtc_lock(&j, ATOM_ENABLE);
    crtc_blank(&j, ATOM_ENABLE);
    crtc_enable(&j, ATOM_DISABLE);
    /* the clock, the timing, the surface */
    if (set_pll(&j, why, cap) != 0) { crtc_enable(&j, ATOM_ENABLE); crtc_blank(&j, ATOM_DISABLE); crtc_lock(&j, ATOM_DISABLE); return -1; }
    set_timing(&j, m);
    set_base(&j, fb_mc, w, h, pitch_px);
    {
        uint32_t ps[16] = { 0 };                    /* (no overscan: borders 0) */
        SET_CRTC_OVERSCAN_PS_ALLOCATION* a = (void*)ps;
        a->ucCRTC = (uint8_t)crtc;
        exec(r, GetIndexIntoMasterTable(COMMAND, SetCRTC_OverScan), ps);
    }
    line_buffer(&j);
    /* the encoder */
    if (j.dig < 0) dac(&j, ATOM_ENABLE);
    /* the CRTC on, unlocked */
    crtc_enable(&j, ATOM_ENABLE);
    crtc_blank(&j, ATOM_DISABLE);
    crtc_lock(&j, ATOM_DISABLE);
    /* the output on (DCE 4 / 5: the DIG set up, then its transmitter) */
    if (j.dig >= 0) {
        dig_encoder(&j, ATOM_ENCODER_CMD_SETUP);
        if (r->family >= CHIP_BARTS) dig_encoder(&j, ATOM_ENCODER_CMD_SETUP_PANEL_MODE);
        transmitter(&j, ATOM_TRANSMITTER_ACTION_ENABLE);
    } else dac(&j, ATOM_ENABLE);
    klog("radeon: %dx%d@%u on CRTC %d, %s (encoder %04x, DIG %d%s)\n", m->hdisplay, m->vdisplay, m->refresh, crtc,
         kind == RAD_OUT_TMDS ? "DVI/HDMI" : "VGA", j.enc, j.dig, j.dual ? ", dual link" : "");
    return 0;
}
