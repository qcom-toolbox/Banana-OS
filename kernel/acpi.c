/*
 * ACPI power-off: the PM1 control registers come from the FADT and the S5
 * sleep type from the \_S5_ package in the DSDT - what VMware, real PCs and
 * every other machine expect, instead of guessing well-known ports.
 */
#include "acpi.h"
#include "smp.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void delay_us(uint32_t us) { while (us--) inb(0x80); }   /* ~1 us each */

/* one AML integer (ZeroOp, OneOp, BytePrefix, WordPrefix): its length in bytes, 0 if none */
static int aml_int(const uint8_t* p, const uint8_t* end, uint32_t* v) {
    if (p >= end) return 0;
    switch (p[0]) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0x0A: if (p + 2 > end) return 0; *v = p[1]; return 2;
    case 0x0B: if (p + 3 > end) return 0; *v = p[1] | (p[2] << 8); return 3;
    default:   return 0;
    }
}

/* SLP_TYPa / SLP_TYPb from Name(\_S5_, Package(){a, b, ...}) */
static int find_s5(const uint8_t* aml, uint32_t len, uint32_t* a, uint32_t* b) {
    const uint8_t* end = aml + len;
    for (const uint8_t* p = aml; p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0) continue;
        /* NameOp before it (maybe with a '\' root prefix), PackageOp after */
        if (!(p[-1] == 0x08 || (p[-1] == 0x5C && p[-2] == 0x08))) continue;
        const uint8_t* q = p + 4;
        if (q >= end || *q != 0x12) continue;
        q++;
        if (q >= end) return 0;
        q += 1 + (*q >> 6);                    /* PkgLength: 1 to 4 bytes */
        q++;                                   /* NumElements */
        int n = aml_int(q, end, a);
        if (!n) return 0;
        q += n;
        if (!aml_int(q, end, b)) *b = 0;
        return 1;
    }
    return 0;
}

void acpi_s5_poweroff(void) {
    const uint8_t* f = acpi_find_table("FACP");
    if (!f) { klog("acpi: no FADT\n"); return; }
    uint32_t flen = rd32(f + 4);

    uint64_t dsdt_a = rd32(f + 40);
    if (flen >= 148 && rd64(f + 140)) dsdt_a = rd64(f + 140);
    const uint8_t* dsdt = acpi_table_at(dsdt_a, "DSDT");
    uint32_t typa, typb;
    if (!dsdt || !find_s5(dsdt + 36, rd32(dsdt + 4) - 36, &typa, &typb)) {
        klog("acpi: no _S5_ in the DSDT\n");
        return;
    }

    uint32_t smi_cmd = rd32(f + 48);
    uint8_t  enable  = f[52];
    uint32_t pm1a = rd32(f + 64), pm1b = rd32(f + 68);
    /* the extended (GAS) addresses, when only they are given in I/O space */
    if (!pm1a && flen >= 184 && f[172] == 1) pm1a = (uint32_t)rd64(f + 176);
    if (!pm1b && flen >= 196 && f[184] == 1) pm1b = (uint32_t)rd64(f + 188);
    if (!pm1a || pm1a > 0xFFFF) { klog("acpi: no PM1a control port\n"); return; }

    /* still in legacy mode (VMware's BIOS boots so): ask the firmware to
     * switch ACPI on, and wait for SCI_EN */
    if (!(inw(pm1a) & 1) && smi_cmd && smi_cmd <= 0xFFFF && enable) {
        outb((uint16_t)smi_cmd, enable);
        for (int i = 0; i < 300 && !(inw(pm1a) & 1); i++) delay_us(10000);
    }

    klog("acpi: S5 (SLP_TYP %u) on port %x\n", typa, pm1a);
    delay_us(20000);                           /* (the log out on the serial port) */
    __asm__ volatile("cli");
    uint16_t v = inw(pm1a) & ~(uint16_t)(7 << 10);
    outw((uint16_t)pm1a, v | (uint16_t)((typa & 7) << 10) | (1 << 13));
    if (pm1b && pm1b <= 0xFFFF) {
        v = inw(pm1b) & ~(uint16_t)(7 << 10);
        outw((uint16_t)pm1b, v | (uint16_t)((typb & 7) << 10) | (1 << 13));
    }
    delay_us(500000);                          /* (the machine goes off here) */
    klog("acpi: S5 did not power off\n");
}
