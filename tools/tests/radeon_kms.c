/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Radeon TeraScale 2 display path (kernel/radeon_atom.c, radeon_kms.c,
 * edid.c) against a model: a synthetic AtomBIOS image laid out with the
 * real atombios.h structures, whose command tables are AtomBIOS bytecode
 * run by the real interpreter (third_party/radeon/atom.c), on a register
 * file in memory. The DDC table answers with an EDID; AdjustDisplayPll
 * hands the clock back unchanged; every other table copies its parameters
 * into registers. Each table call is recorded with its parameters, and the
 * mode sets are checked: the sequence, the PLL dividers, the timing, the
 * scanout registers, DVI single and dual link, VGA, refusals. */
#define RADEON_MODEL
#define rad_atom_exec rad_atom_exec_real
#include "../../third_party/radeon/atom.c"
#include "../../kernel/radeon_atom.c"
#undef rad_atom_exec
int rad_atom_exec(radeon_t* r, int index, void* params, int size);
#include "../../kernel/radeon_kms.c"
#include "../../kernel/edid.c"

extern int printf(const char*, ...);
extern int vprintf(const char*, __builtin_va_list);
extern void* calloc(size_t, size_t);
extern void free(void*);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* ── the kernel's services ─────────────────────────────────────────── */
void* kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void* kzalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void* p) { free(p); }
static int g_verbose;
void klog(const char* fmt, ...) {
    if (!g_verbose) return;
    __builtin_va_list a;
    __builtin_va_start(a, fmt);
    vprintf(fmt, a);
    __builtin_va_end(a);
}
static uint64_t g_ns;
uint64_t timer_counter(void) { return g_ns += 1000; }
uint32_t timer_ms(void) { return (uint32_t)(g_ns / 1000000u); }
void timer_sleep_ms(uint32_t ms) { g_ns += (uint64_t)ms * 1000000u; }
static uint8_t g_vfct[0x200 + 65536];
const void* acpi_find_table(const char* sig) { return !strcmp(sig, "VFCT") ? g_vfct : NULL; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; (void)o; return 0; }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) { (void)b; (void)d; (void)f; (void)o; (void)v; }
int mmio_map(uint64_t p, uint64_t n) { (void)p; (void)n; return 1; }
volatile int latency_enabled;
void latency_irq_begin(uintptr_t f) { (void)f; }
void latency_irq_end(uintptr_t f) { (void)f; }

/* ── the calls ─────────────────────────────────────────────────────── */
typedef struct { int index; uint8_t ps[64]; } call_t;
static call_t g_calls[128];
static int g_ncalls;
int rad_atom_exec(radeon_t* r, int index, void* params, int size) {
    CHECK(size <= 64);
    int rc = rad_atom_exec_real(r, index, params, size);
    if (rc == 0 && g_ncalls < 128) { g_calls[g_ncalls].index = index; memcpy(g_calls[g_ncalls].ps, params, (size_t)size); g_ncalls++; }
    return rc;
}
static const call_t* nth(int index, int k) {           /* the k-th call of a table */
    for (int i = 0; i < g_ncalls; i++) if (g_calls[i].index == index && k-- == 0) return &g_calls[i];
    return NULL;
}
static int pos_of(int index, int k) {
    for (int i = 0; i < g_ncalls; i++) if (g_calls[i].index == index && k-- == 0) return i;
    return -1;
}

/* ── the video BIOS ────────────────────────────────────────────────── */
static uint8_t B[65536];
static uint32_t g_end;
static void p16(uint32_t o, uint16_t v) { B[o] = (uint8_t)v; B[o + 1] = (uint8_t)(v >> 8); }
static void p32(uint32_t o, uint32_t v) { p16(o, (uint16_t)v); p16(o + 2, (uint16_t)(v >> 16)); }

static uint8_t EDID[128];
static void make_edid(void) {
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(EDID, hdr, 8);
    EDID[8] = 0x08; EDID[9] = 0x2E;                    /* (a maker id) */
    EDID[18] = 1; EDID[19] = 4;                        /* EDID 1.4 */
    EDID[20] = 0x80;                                   /* a digital input */
    EDID[35] = 1 << 5;                                 /* 640x480@60 */
    EDID[36] = 1 << 3;                                 /* 1024x768@60 */
    for (int i = 38; i < 54; i++) EDID[i] = 1;
    EDID[38] = 129; EDID[39] = 0x80;                   /* 1280x1024@60 (5:4) */
    EDID[40] = 0xD1; EDID[41] = 0xC0;                  /* 1920x1080@60 (16:9) */
    uint8_t* d = EDID + 54;                            /* 1920x1080@60, CEA timing */
    uint8_t dtd[18] = { 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 88, 44, 0x45, 0x00, 0, 0, 0, 0, 0, 0x1E };
    memcpy(d, dtd, 18);
    uint8_t* n = EDID + 72;                            /* its name */
    n[3] = 0xFC;
    memcpy(n + 5, "BANANA TEST\n ", 13);
    uint8_t s = 0;
    for (int i = 0; i < 127; i++) s = (uint8_t)(s + EDID[i]);
    EDID[127] = (uint8_t)(0x100 - s);
}

/* AtomBIOS bytecode */
#define OP_MOVE_REG 1
#define OP_MOVE_PS  2
#define OP_MOVE_FB  4
#define OP_AND_PS   8
#define OP_EOT      91
static uint32_t at;
static void e8(uint8_t v) { B[at++] = v; }
static void e16(uint16_t v) { e8((uint8_t)v); e8((uint8_t)(v >> 8)); }
static void e32(uint32_t v) { e16((uint16_t)v); e16((uint16_t)(v >> 16)); }

static uint32_t g_cmd_master, g_data_master;
static void cmd_entry(int index, uint32_t table) { p16(g_cmd_master + 4 + 2u * (uint32_t)index, (uint16_t)table); }
static void data_entry(int index, uint32_t table) { p16(g_data_master + 4 + 2u * (uint32_t)index, (uint16_t)table); }

/* a command table: header, then code from body(); returns its offset */
static uint32_t table_begin(uint8_t frev, uint8_t crev) {
    uint32_t t = at;
    at += 6;
    B[t + 2] = frev; B[t + 3] = crev;
    B[t + 4] = 0;                                      /* (no work space) */
    B[t + 5] = 64;                                     /* (parameter space, bytes) */
    return t;
}
static void table_end(uint32_t t) { e8(OP_EOT); p16(t, (uint16_t)(at - t)); }

/* the usual body: PS[0..7] copied to registers (dword index 0x1000 + index * 8 + i) */
static void copy_table(int index, uint8_t frev, uint8_t crev) {
    uint32_t t = table_begin(frev, crev);
    for (int i = 0; i < 8; i++) { e8(OP_MOVE_REG); e8(ATOM_ARG_PS); e16((uint16_t)(0x1000 + index * 8 + i)); e8((uint8_t)i); }
    table_end(t);
    cmd_entry(index, t);
}

static void make_bios(int dp) {
    memset(B, 0, sizeof(B));
    B[0] = 0x55; B[1] = 0xAA; B[2] = 64;              /* (32 KiB) */
    p16(0x18, 0x80);                                   /* PCIR */
    memcpy(B + 0x80, "PCIR", 4);
    p16(0x84, 0x1002); p16(0x86, 0x6779);
    B[0x80 + 0x14] = 0;                                /* x86 code */
    memcpy(B + 0x30, " 761295520", 10);
    p16(0x48, 0x100);                                  /* the ATOM ROM header */
    memcpy(B + 0x104, "ATOM", 4);
    p16(0x100 + ATOM_ROM_MSG_PTR, 0x1C0);
    memcpy(B + 0x1C0, "BANANA.MODEL.CAICOS", 20);
    g_cmd_master = 0x200;
    g_data_master = 0x400;
    p16(0x100 + ATOM_ROM_CMD_PTR, (uint16_t)g_cmd_master);
    p16(0x100 + ATOM_ROM_DATA_PTR, (uint16_t)g_data_master);
    p16(g_cmd_master, (uint16_t)(4 + sizeof(ATOM_MASTER_LIST_OF_COMMAND_TABLES)));
    p16(g_data_master, (uint16_t)(4 + sizeof(ATOM_MASTER_LIST_OF_DATA_TABLES)));
    /* indirect I/O: none (an empty list) */
    data_entry(ATOM_DATA_IIO_PTR / 2 - 2, 0x600);
    p16(0x600, 8); B[0x604] = 0;
    /* FirmwareInfo 2.1 */
    uint32_t fi = 0x700;
    data_entry(GetIndexIntoMasterTable(DATA, FirmwareInfo), fi);
    ATOM_FIRMWARE_INFO_V2_1* f = (ATOM_FIRMWARE_INFO_V2_1*)(B + fi);
    f->sHeader.usStructureSize = sizeof(*f); f->sHeader.ucTableFormatRevision = 2; f->sHeader.ucTableContentRevision = 1;
    ((ATOM_FIRMWARE_INFO*)f)->usReferenceClock = 2700;            /* 27 MHz */
    f->ulMinPixelClockPLL_Output = 64800;                         /* 648 MHz */
    f->ulMaxPixelClockPLL_Output = 120000;                        /* 1.2 GHz */
    f->usMinPixelClockPLL_Input = 1000;
    f->usMaxPixelClockPLL_Input = 3000;
    f->usMaxPixelClock = 40000;                                   /* 400 MHz */
    f->ulDefaultDispEngineClkFreq = 60000;
    /* Object_Header 1.3: a dual-link DVI-I (UNIPHY link A + DAC1), and an HDMI or DisplayPort (UNIPHY1 link A) */
    uint32_t oh = 0x800;
    data_entry(GetIndexIntoMasterTable(DATA, Object_Header), oh);
    ATOM_OBJECT_HEADER* h = (ATOM_OBJECT_HEADER*)(B + oh);
    h->sHeader.usStructureSize = 0x200; h->sHeader.ucTableFormatRevision = 1; h->sHeader.ucTableContentRevision = 3;
    h->usDeviceSupport = ATOM_DEVICE_CRT1_SUPPORT | ATOM_DEVICE_DFP1_SUPPORT | ATOM_DEVICE_DFP2_SUPPORT;
    uint16_t dvi = (GRAPH_OBJECT_TYPE_CONNECTOR << OBJECT_TYPE_SHIFT) | (1 << ENUM_ID_SHIFT) | CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I;
    uint16_t two = (GRAPH_OBJECT_TYPE_CONNECTOR << OBJECT_TYPE_SHIFT) | (1 << ENUM_ID_SHIFT) |
                   (dp ? CONNECTOR_OBJECT_ID_DISPLAYPORT : CONNECTOR_OBJECT_ID_HDMI_TYPE_A);
    uint16_t uniphy = (GRAPH_OBJECT_TYPE_ENCODER << OBJECT_TYPE_SHIFT) | (1 << ENUM_ID_SHIFT) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY;
    uint16_t uniphy1 = (GRAPH_OBJECT_TYPE_ENCODER << OBJECT_TYPE_SHIFT) | (1 << ENUM_ID_SHIFT) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY1;
    uint16_t dac = (GRAPH_OBJECT_TYPE_ENCODER << OBJECT_TYPE_SHIFT) | (1 << ENUM_ID_SHIFT) | ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1;
    /* the paths */
    h->usDisplayPathTableOffset = 0x40;
    ATOM_DISPLAY_OBJECT_PATH_TABLE* pt = (ATOM_DISPLAY_OBJECT_PATH_TABLE*)(B + oh + 0x40);
    pt->ucNumOfDispPath = 3;
    uint32_t pp = oh + 0x40 + offsetof(ATOM_DISPLAY_OBJECT_PATH_TABLE, asDispPath);
    struct { uint16_t tag, conn, enc; } P[3] = {
        { ATOM_DEVICE_DFP1_SUPPORT, dvi, uniphy }, { ATOM_DEVICE_CRT1_SUPPORT, dvi, dac }, { ATOM_DEVICE_DFP2_SUPPORT, two, uniphy1 } };
    for (int i = 0; i < 3; i++) {
        ATOM_DISPLAY_OBJECT_PATH* p = (ATOM_DISPLAY_OBJECT_PATH*)(B + pp);
        p->usDeviceTag = P[i].tag; p->usSize = 10; p->usConnObjectId = P[i].conn; p->usGPUObjectId = 0x1100;
        p16(pp + offsetof(ATOM_DISPLAY_OBJECT_PATH, usGraphicObjIds), P[i].enc);
        pp += 10;
    }
    /* the connectors, each with its I2C record (DDC lines 0x90, 0x91) */
    h->usConnectorObjectTableOffset = 0x100;
    ATOM_OBJECT_TABLE* ct = (ATOM_OBJECT_TABLE*)(B + oh + 0x100);
    ct->ucNumberOfObjects = 2;
    uint16_t conns[2] = { dvi, two };
    for (int i = 0; i < 2; i++) {
        ATOM_OBJECT* o = (ATOM_OBJECT*)(B + oh + 0x100 + offsetof(ATOM_OBJECT_TABLE, asObjects) + sizeof(ATOM_OBJECT) * (unsigned)i);
        o->usObjectID = conns[i];
        o->usRecordOffset = (uint16_t)(0x180 + 0x10 * i);
        ATOM_I2C_RECORD* rec = (ATOM_I2C_RECORD*)(B + oh + 0x180 + 0x10 * i);
        rec->sheader.ucRecordType = ATOM_I2C_RECORD_TYPE;
        rec->sheader.ucRecordSize = sizeof(ATOM_I2C_RECORD);
        ((ATOM_I2C_ID_CONFIG_ACCESS*)&rec->sucI2cId)->ucAccess = (uint8_t)(0x90 + i);
        B[oh + 0x180 + 0x10 * i + sizeof(ATOM_I2C_RECORD)] = 0xFF;   /* (the end) */
    }
    /* the command tables */
    at = 0x1000;
    int copies[] = { GetIndexIntoMasterTable(COMMAND, UpdateCRTC_DoubleBufferRegisters), GetIndexIntoMasterTable(COMMAND, EnableCRTC),
                     GetIndexIntoMasterTable(COMMAND, EnableCRTCMemReq), GetIndexIntoMasterTable(COMMAND, BlankCRTC),
                     GetIndexIntoMasterTable(COMMAND, EnableSpreadSpectrumOnPPLL), GetIndexIntoMasterTable(COMMAND, SetCRTC_UsingDTDTiming),
                     GetIndexIntoMasterTable(COMMAND, SetCRTC_OverScan), GetIndexIntoMasterTable(COMMAND, DAC1EncoderControl) };
    for (unsigned i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) copy_table(copies[i], 1, 1);
    copy_table(GetIndexIntoMasterTable(COMMAND, SetPixelClock), 1, 5);
    copy_table(GetIndexIntoMasterTable(COMMAND, SelectCRTC_Source), 1, 2);
    copy_table(GetIndexIntoMasterTable(COMMAND, DIGxEncoderControl), 1, 4);
    copy_table(GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl), 1, 4);
    /* AdjustDisplayPll 1.3: the clock back as it is (ulDispPllFreq = usPixelClock), no dividers asked for */
    uint32_t t = table_begin(1, 3);
    e8(OP_AND_PS); e8(ATOM_ARG_IMM); e8(0); e32(0x0000FFFF);
    e8(OP_MOVE_PS); e8(ATOM_ARG_IMM); e8(1); e32(0);
    table_end(t);
    cmd_entry(GetIndexIntoMasterTable(COMMAND, AdjustDisplayPll), t);
    /* ProcessI2cChannelTransaction: the EDID into the scratch, success in ucStatus (PS byte 1) */
    t = table_begin(1, 1);
    for (int i = 0; i < 32; i++) {
        e8(OP_MOVE_FB); e8(ATOM_ARG_IMM); e8((uint8_t)i);
        e32((uint32_t)EDID[4 * i] | (uint32_t)EDID[4 * i + 1] << 8 | (uint32_t)EDID[4 * i + 2] << 16 | (uint32_t)EDID[4 * i + 3] << 24);
    }
    e8(OP_MOVE_PS); e8((uint8_t)(ATOM_ARG_IMM | ATOM_SRC_BYTE0 << 3 | 1 << 6)); e8(0); e8(HW_ASSISTED_I2C_STATUS_SUCCESS);
    table_end(t);
    cmd_entry(GetIndexIntoMasterTable(COMMAND, ProcessI2cChannelTransaction), t);
    g_end = at;
    CHECK(g_end < 32768);
    /* the ACPI VFCT table holding it, for bus 1, device 0 */
    memset(g_vfct, 0, sizeof(g_vfct));
    memcpy(g_vfct, "VFCT", 4);
    uint32_t len = 0x4C + 0x1C + 32768;
    g_vfct[4] = (uint8_t)len; g_vfct[5] = (uint8_t)(len >> 8); g_vfct[6] = (uint8_t)(len >> 16);
    g_vfct[0x34] = 0x4C;
    uint8_t* vh = g_vfct + 0x4C;
    vh[0] = 1; vh[4] = 0; vh[8] = 0;
    vh[12] = 0x02; vh[13] = 0x10; vh[14] = 0x79; vh[15] = 0x67;
    vh[24] = 0x00; vh[25] = 0x80;                      /* 32768 */
    memcpy(vh + 0x1C, B, 32768);
}

static uint32_t g_regs[(128u << 10) / 4];

static void card(radeon_t* r) {
    memset(r, 0, sizeof(*r));
    r->pci.bus = 1; r->pci.vendor = 0x1002; r->pci.device = 0x6779;
    r->family = rad_family(0x6779);
    r->mmio = (volatile uint8_t*)g_regs;
    r->mmio_size = sizeof(g_regs);
}

int main(int argc, char** argv) {
    g_verbose = argc > 1;
    (void)argv;
    make_edid();
    make_bios(0);
    radeon_t r;
    card(&r);
    CHECK(r.family == CHIP_CAICOS);
    CHECK(rad_family(0x68E1) == CHIP_CEDAR && rad_family(0x6720) == CHIP_BARTS && rad_family(0x1234) == -1);
    char why[128];
    CHECK(rad_atom_init(&r, why, sizeof(why)) == 0);
    CHECK(!strcmp(r.bios_from, "ACPI VFCT") && r.bios_size == 32768);
    /* what the BIOS describes */
    CHECK(r.clk.ref_freq == 2700 && r.clk.pll_out_min == 64800 && r.clk.pll_out_max == 120000 && r.clk.max_pixel_clock == 40000);
    CHECK(r.npaths == 3);
    CHECK(r.paths[0].device == ATOM_DEVICE_DFP1_SUPPORT && r.paths[0].conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I);
    CHECK(r.paths[0].i2c_id == 0x90 && r.paths[1].i2c_id == 0x90 && r.paths[2].i2c_id == 0x91);
    CHECK(r.paths[0].nenc == 1 && ((r.paths[0].enc_obj[0] & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT) == ENCODER_OBJECT_ID_INTERNAL_UNIPHY);
    CHECK(rad_kms_path_kind(&r.paths[0], 1) == RAD_OUT_TMDS && rad_kms_path_kind(&r.paths[1], 0) == RAD_OUT_VGA);
    CHECK(rad_kms_path_kind(&r.paths[2], 1) == RAD_OUT_TMDS);
    CHECK(rad_kms_max_clock(&r, &r.paths[0], RAD_OUT_TMDS) == 330000 && rad_kms_max_clock(&r, &r.paths[2], RAD_OUT_TMDS) == 165000);
    char info[1024];
    rad_atom_info(&r, info, sizeof(info));
    CHECK(strstr(info, "ACPI VFCT") && strstr(info, "DVI-I") && strstr(info, "HDMI"));
    /* the monitor through the BIOS's DDC */
    uint8_t edid[128];
    CHECK(rad_atom_read_edid(&r, 0x90, edid) == 0 && !memcmp(edid, EDID, 128));
    CHECK(edid_valid(edid));
    char name[24];
    edid_name(edid, name, sizeof(name));
    CHECK(!strcmp(name, "BANANA TEST"));
    edid_mode_t modes[16];
    int n = edid_modes(edid, modes, 16);
    CHECK(n == 4);
    CHECK(modes[0].hdisplay == 1920 && modes[0].vdisplay == 1080 && modes[0].clock == 148500 && modes[0].refresh == 60 &&
          (modes[0].flags & EDID_PREFERRED) && !(modes[0].flags & (EDID_NHSYNC | EDID_NVSYNC)));
    CHECK(modes[0].hsync_start == 2008 && modes[0].hsync_end == 2052 && modes[0].htotal == 2200);
    CHECK(modes[0].vsync_start == 1084 && modes[0].vsync_end == 1089 && modes[0].vtotal == 1125);
    int got640 = 0, got1024 = 0, got1280 = 0;
    for (int i = 0; i < n; i++) {
        if (modes[i].hdisplay == 640 && modes[i].clock == 25175) got640 = 1;
        if (modes[i].hdisplay == 1024 && modes[i].clock == 65000) got1024 = 1;
        if (modes[i].hdisplay == 1280 && modes[i].vdisplay == 1024 && modes[i].clock == 108000) got1280 = 1;
    }
    CHECK(got640 && got1024 && got1280);

    /* 1920x1080@60 on the DVI connector, CRTC 0 */
    uint64_t fb = 0xF04000000ull;
    g_ncalls = 0;
    CHECK(rad_kms_set(&r, 0, 0, 1, &modes[0], fb, 1920, 1080, 1920, why, sizeof(why)) == 0);
    int TX = IDX(UNIPHYTransmitterControl), DIG = IDX(DIGxEncoderControl), PIX = IDX(SetPixelClock), DTD = IDX(SetCRTC_UsingDTDTiming);
    int LOCK = IDX(UpdateCRTC_DoubleBufferRegisters), EN = IDX(EnableCRTC), BL = IDX(BlankCRTC), SRC = IDX(SelectCRTC_Source);
    /* the order: output off, source, lock, blank, off; PLL; timing; on, unblank, unlock; DIG setup, panel mode, transmitter on */
    CHECK(pos_of(TX, 0) == 0 && pos_of(SRC, 0) == 1 && pos_of(LOCK, 0) == 2 && pos_of(BL, 0) == 3 && pos_of(EN, 0) == 4);
    CHECK(pos_of(PIX, 0) > pos_of(EN, 0) && pos_of(DTD, 0) > pos_of(PIX, 0));
    CHECK(pos_of(EN, 1) > pos_of(DTD, 0) && pos_of(BL, 1) > pos_of(EN, 1) && pos_of(LOCK, 1) > pos_of(BL, 1));
    CHECK(pos_of(DIG, 0) > pos_of(LOCK, 1) && pos_of(DIG, 1) > pos_of(DIG, 0) && pos_of(TX, 1) > pos_of(DIG, 1));
    CHECK(pos_of(TX, 1) == g_ncalls - 1);
    const DIG_TRANSMITTER_CONTROL_PARAMETERS_V4* tx0 = (const void*)nth(TX, 0)->ps;
    CHECK(tx0->ucAction == ATOM_TRANSMITTER_ACTION_DISABLE);
    const ENABLE_CRTC_PS_ALLOCATION* l0 = (const void*)nth(LOCK, 0)->ps, *l1 = (const void*)nth(LOCK, 1)->ps;
    CHECK(l0->ucCRTC == 0 && l0->ucEnable == 1 && l1->ucEnable == 0);
    const BLANK_CRTC_PS_ALLOCATION* b0 = (const void*)nth(BL, 0)->ps;
    CHECK(b0->ucBlanking == 1 && ((const BLANK_CRTC_PS_ALLOCATION*)nth(BL, 1)->ps)->ucBlanking == 0);
    const SELECT_CRTC_SOURCE_PARAMETERS_V2* src = (const void*)nth(SRC, 0)->ps;
    CHECK(src->ucCRTC == 0 && src->ucEncoderID == ASIC_INT_DIG1_ENCODER_ID && src->ucEncodeMode == ATOM_ENCODER_MODE_DVI);
    /* the PLL: dividers within the limits that make 148.5 MHz */
    const PIXEL_CLOCK_PARAMETERS_V5* pc = (const void*)nth(PIX, 0)->ps;
    CHECK(pc->ucCRTC == 0 && pc->ucPpll == ATOM_PPLL1 && pc->usPixelClock == 14850);
    CHECK(pc->ucEncoderMode == ATOM_ENCODER_MODE_DVI && pc->ucTransmitterID == ENCODER_OBJECT_ID_INTERNAL_UNIPHY);
    CHECK(pc->ucRefDiv >= 2 && pc->usFbDiv >= 4 && pc->usFbDiv <= 0x7ff && pc->ucPostDiv >= 2);
    uint64_t vco10 = (uint64_t)2700 * ((uint64_t)pc->usFbDiv * 10 + pc->ulFbDivDecFrac / 100000) / pc->ucRefDiv;   /* x10 kHz x10 */
    uint64_t dot = vco10 / pc->ucPostDiv;                         /* (10 kHz x 10) */
    CHECK(dot > 148500 * 10 / 10 * 99 / 100 && dot < 148500 * 101 / 100);
    CHECK(vco10 / 10 >= 64800 && vco10 / 10 <= 120000);
    unsigned fbd = pc->usFbDiv, frd = pc->ulFbDivDecFrac / 100000, rfd = pc->ucRefDiv, psd = pc->ucPostDiv;
    /* the timing */
    const SET_CRTC_USING_DTD_TIMING_PARAMETERS* dt = (const void*)nth(DTD, 0)->ps;
    CHECK(dt->usH_Size == 1920 && dt->usH_Blanking_Time == 280 && dt->usH_SyncOffset == 88 && dt->usH_SyncWidth == 44);
    CHECK(dt->usV_Size == 1080 && dt->usV_Blanking_Time == 45 && dt->usV_SyncOffset == 4 && dt->usV_SyncWidth == 5);
    CHECK(dt->susModeMiscInfo.usAccess == 0 && dt->ucCRTC == 0);
    /* the scanout surface and the line buffer */
    CHECK(g_regs[GRPH_PRIMARY_SURFACE / 4] == (uint32_t)fb && g_regs[GRPH_PRIMARY_SURFACE_HIGH / 4] == (uint32_t)(fb >> 32));
    CHECK(g_regs[GRPH_PITCH / 4] == 1920 && g_regs[GRPH_X_END / 4] == 1920 && g_regs[GRPH_Y_END / 4] == 1080);
    CHECK(g_regs[VIEWPORT_SIZE / 4] == (1920u << 16 | 1080u) && g_regs[GRPH_ENABLE / 4] == 1);
    CHECK(g_regs[GRPH_CONTROL / 4] == GRPH_DEPTH_32BPP && !(g_regs[GRPH_UPDATE / 4] & GRPH_UPDATE_LOCK));
    CHECK(g_regs[DC_LB_MEMORY_SPLIT / 4] == 2 && g_regs[PIPE0_DMIF_BUFFER_CONTROL / 4] == 2);
    /* the encoder and transmitter: single link, DIG 0, coherent, PPLL1 */
    const DIG_ENCODER_CONTROL_PARAMETERS_V4* dg = (const void*)nth(DIG, 0)->ps;
    CHECK(dg->ucAction == ATOM_ENCODER_CMD_SETUP && dg->ucEncoderMode == ATOM_ENCODER_MODE_DVI && dg->ucLaneNum == 4);
    CHECK(dg->usPixelClock == 14850 && dg->acConfig.ucDigSel == 0);
    CHECK(((const DIG_ENCODER_CONTROL_PARAMETERS_V4*)nth(DIG, 1)->ps)->ucAction == ATOM_ENCODER_CMD_SETUP_PANEL_MODE);
    const DIG_TRANSMITTER_CONTROL_PARAMETERS_V4* tx = (const void*)nth(TX, 1)->ps;
    CHECK(tx->ucAction == ATOM_TRANSMITTER_ACTION_ENABLE && tx->usPixelClock == 14850 && tx->ucLaneNum == 4);
    CHECK(tx->acConfig.ucTransmitterSel == 0 && tx->acConfig.fCoherentMode && !tx->acConfig.fDualLinkConnector);
    CHECK(tx->acConfig.ucRefClkSource == ATOM_PPLL1 && tx->acConfig.ucLinkSel == 0);
    /* the interpreter ran the copying tables: SetPixelClock's parameters landed in its registers */
    CHECK(g_regs[0x1000 + PIX * 8] == ((const uint32_t*)nth(PIX, 0)->ps)[0]);

    /* 2560x1600@60 (268.5 MHz) on the dual-link DVI: 8 lanes, half the clock a link */
    edid_mode_t big = { 268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, EDID_NVSYNC, 60 };
    g_ncalls = 0;
    CHECK(rad_kms_set(&r, 0, 0, 1, &big, fb, 2560, 1600, 2560, why, sizeof(why)) == 0);
    tx = (const void*)nth(TX, 1)->ps;
    CHECK(tx->ucLaneNum == 8 && tx->usPixelClock == 13425 && tx->acConfig.fDualLinkConnector);
    CHECK(((const DIG_ENCODER_CONTROL_PARAMETERS_V4*)nth(DIG, 0)->ps)->ucLaneNum == 8);
    CHECK(((const SET_CRTC_USING_DTD_TIMING_PARAMETERS*)nth(DTD, 0)->ps)->susModeMiscInfo.usAccess == ATOM_VSYNC_POLARITY);
    /* ... too fast for the single-link HDMI */
    CHECK(rad_kms_set(&r, 1, 2, 1, &big, fb, 2560, 1600, 2560, why, sizeof(why)) == -1 && strstr(why, "too fast"));
    /* 1024x768 on HDMI, CRTC 1: UNIPHY1 link A is DIG 2, PPLL2, the other half of the line buffer */
    const edid_mode_t* m1024 = NULL;
    for (int i = 0; i < n; i++) if (modes[i].hdisplay == 1024) m1024 = &modes[i];
    g_ncalls = 0;
    CHECK(rad_kms_set(&r, 1, 2, 1, m1024, fb, 1024, 768, 1024, why, sizeof(why)) == 0);
    CHECK(((const SELECT_CRTC_SOURCE_PARAMETERS_V2*)nth(SRC, 0)->ps)->ucEncoderID == ASIC_INT_DIG3_ENCODER_ID);
    CHECK(((const PIXEL_CLOCK_PARAMETERS_V5*)nth(PIX, 0)->ps)->ucPpll == ATOM_PPLL2);
    CHECK(((const ENABLE_CRTC_PS_ALLOCATION*)nth(EN, 0)->ps)->ucCRTC == 1);
    CHECK(((const DIG_TRANSMITTER_CONTROL_PARAMETERS_V4*)nth(TX, 1)->ps)->acConfig.ucTransmitterSel == 1);
    CHECK(g_regs[(DC_LB_MEMORY_SPLIT + CRTC_OFF[1]) / 4] == 6);   /* (CRTC 0 is off in the model: all of it, second half) */
    CHECK(g_regs[(GRPH_PITCH + CRTC_OFF[1]) / 4] == 1024);
    /* VGA through the DVI-I's DAC */
    g_ncalls = 0;
    CHECK(rad_kms_set(&r, 0, 1, 0, m1024, fb, 1024, 768, 1024, why, sizeof(why)) == 0);
    int DAC = IDX(DAC1EncoderControl);
    CHECK(nth(TX, 0) == NULL && nth(DIG, 0) == NULL);
    const DAC_ENCODER_CONTROL_PS_ALLOCATION* d0 = (const void*)nth(DAC, 0)->ps;
    CHECK(d0->ucAction == ATOM_DISABLE && d0->ucDacStandard == ATOM_DAC1_PS2);
    CHECK(((const DAC_ENCODER_CONTROL_PS_ALLOCATION*)nth(DAC, 1)->ps)->ucAction == ATOM_ENABLE);
    CHECK(((const DAC_ENCODER_CONTROL_PS_ALLOCATION*)nth(DAC, 2)->ps)->usPixelClock == 6500);
    CHECK(((const SELECT_CRTC_SOURCE_PARAMETERS_V2*)nth(SRC, 0)->ps)->ucEncoderID == ASIC_INT_DAC1_ENCODER_ID);
    CHECK(((const PIXEL_CLOCK_PARAMETERS_V5*)nth(PIX, 0)->ps)->ucEncoderMode == ATOM_ENCODER_MODE_CRT);

    /* a DisplayPort connector: refused, the firmware's mode stays */
    make_bios(1);
    radeon_t r2;
    card(&r2);
    CHECK(rad_atom_init(&r2, why, sizeof(why)) == 0);
    CHECK(rad_kms_path_kind(&r2.paths[2], 1) == RAD_OUT_DP);
    g_ncalls = 0;
    CHECK(rad_kms_set(&r2, 0, 2, 1, &modes[0], fb, 1920, 1080, 1920, why, sizeof(why)) == -1 && strstr(why, "DisplayPort"));
    CHECK(g_ncalls == 0);
    printf("PASS: radeon TeraScale 2 mode setting model (%d paths, %d EDID modes, PLL %u.%u/%u/%u)\n",
           r.npaths, n, fbd, frd, rfd, psd);
    return 0;
}
