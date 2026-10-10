/* Graphics cards Banana OS has no driver for - AMD Radeon, NVIDIA GeForce,
 * Intel Arc and Xe: the screen stays the one the firmware set up (UEFI GOP
 * or VESA), at the size Banana Boot chose (Settings > Screen sets it for
 * the next start). What Banana OS does with them:
 *   - names them (lsgpu, Settings > About): the vendor and the family,
 *     from the PCI device id
 *   - makes the framebuffer write-combining (paging_set_wc): firmware
 *     leaves video memory uncached, which makes every pixel written a
 *     separate trip over PCIe - drawing gets many times faster
 * Mode setting, hardware cursors and acceleration need a driver per
 * generation of these chips; none is here. */
#include "gpu.h"
#include "fb.h"
#include "paging.h"
#include "kstring.h"
#include "kheap.h"
#include "serial.h"

typedef struct { uint16_t vendor, lo, hi; const char* name; } family_t;

/* by device id range: what is in it is (mostly) one family */
static const family_t FAMILIES[] = {
    /* NVIDIA */
    { 0x10DE, 0x2900, 0x2FFF, "NVIDIA GeForce RTX 50 series (Blackwell)" },
    { 0x10DE, 0x2680, 0x28FF, "NVIDIA GeForce RTX 40 series (Ada Lovelace)" },
    { 0x10DE, 0x2200, 0x25FF, "NVIDIA GeForce RTX 30 series (Ampere)" },
    { 0x10DE, 0x2180, 0x21FF, "NVIDIA GeForce GTX 16 series (Turing)" },
    { 0x10DE, 0x1E00, 0x1FFF, "NVIDIA GeForce RTX 20 / GTX 16 series (Turing)" },
    { 0x10DE, 0x1B00, 0x1DFF, "NVIDIA GeForce GTX 10 series (Pascal)" },
    { 0x10DE, 0x1380, 0x17FF, "NVIDIA GeForce GTX 900 / 750 series (Maxwell)" },
    { 0x10DE, 0x0FC0, 0x137F, "NVIDIA GeForce GTX 600 / 700 series (Kepler)" },
    /* AMD */
    { 0x1002, 0x7550, 0x75FF, "AMD Radeon RX 9000 series (RDNA 4)" },
    { 0x1002, 0x7440, 0x74FF, "AMD Radeon RX 7000 series (RDNA 3)" },
    { 0x1002, 0x73A0, 0x73FF, "AMD Radeon RX 6000 series (RDNA 2)" },
    { 0x1002, 0x7310, 0x739F, "AMD Radeon RX 5000 series (RDNA)" },
    { 0x1002, 0x6860, 0x687F, "AMD Radeon RX Vega" },
    { 0x1002, 0x66A0, 0x66AF, "AMD Radeon VII / Instinct (Vega 20)" },
    { 0x1002, 0x67C0, 0x67FF, "AMD Radeon RX 400 / 500 series (Polaris)" },
    { 0x1002, 0x6980, 0x699F, "AMD Radeon RX 500 series (Polaris 12)" },
    { 0x1002, 0x1500, 0x17FF, "AMD Radeon Graphics (Ryzen processor)" },
    { 0x1002, 0x13C0, 0x13FF, "AMD Radeon Graphics (Ryzen processor)" },
    /* Intel */
    { 0x8086, 0xE200, 0xE2FF, "Intel Arc B-series (Battlemage)" },
    { 0x8086, 0x5690, 0x56BF, "Intel Arc A-series (Alchemist)" },
    { 0x8086, 0x7D40, 0x7DFF, "Intel Arc Graphics (Meteor / Arrow Lake)" },
    { 0x8086, 0x6420, 0x64FF, "Intel Arc Graphics (Lunar Lake)" },
    { 0x8086, 0xB080, 0xB0FF, "Intel Arc Graphics (Panther Lake)" },
    { 0x8086, 0xA780, 0xA7FF, "Intel Iris Xe / UHD Graphics (Raptor Lake)" },
    { 0x8086, 0x4680, 0x46FF, "Intel Iris Xe / UHD Graphics (Alder Lake)" },
    { 0x8086, 0x4600, 0x467F, "Intel UHD Graphics (Alder Lake-N)" },
    { 0x8086, 0x9A40, 0x9AFF, "Intel Iris Xe Graphics (Tiger Lake)" },
    { 0x8086, 0x4C80, 0x4CFF, "Intel UHD Graphics (Rocket Lake)" },
    { 0x8086, 0x8A50, 0x8A5F, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8086, 0x3E90, 0x3EFF, "Intel UHD Graphics 600 series (Coffee Lake)" },
    { 0x8086, 0x9B40, 0x9BFF, "Intel UHD Graphics (Comet Lake)" },
    { 0x8086, 0x5900, 0x59FF, "Intel HD / UHD Graphics 600 series (Kaby Lake)" },
    { 0x8086, 0x1900, 0x193F, "Intel HD Graphics 500 series (Skylake)" },
    { 0x8086, 0x1600, 0x163F, "Intel HD Graphics (Broadwell)" },
    { 0x8086, 0x0400, 0x0D2F, "Intel HD Graphics (Haswell)" },
};

const char* gpu_family_name(uint16_t vendor, uint16_t device) {
    for (unsigned i = 0; i < sizeof(FAMILIES) / sizeof(FAMILIES[0]); i++)
        if (FAMILIES[i].vendor == vendor && device >= FAMILIES[i].lo && device <= FAMILIES[i].hi) return FAMILIES[i].name;
    return NULL;
}

static int fw_set_mode(gpu_t* g, int w, int h) { (void)g; (void)w; (void)h; return -1; }

static void fw_info(gpu_t* g, char* buf, int cap) {
    const fb_info_t* fi = fb_info();
    ksnprintf(buf, (size_t)cap, "the firmware's framebuffer, %ux%u%s\n"
                                "screen size: Settings > Screen or `resolution` (from the next start)",
              fi ? fi->width : 0, fi ? fi->height : 0, g->priv ? ", write-combining" : "");
}

/* how far the address is above the nearest memory BAR of the card (the
 * framebuffer lives in the card's video memory BAR); ~0 if below them all.
 * (Only the bases are read: sizing a BAR would stop the card decoding for
 * a moment, under the screen it shows.) */
static uint64_t bar_distance(const pci_dev_t* d, uintptr_t addr) {
    uint64_t best = ~0ull;
    for (int b = 0; b < 6; b++) {
        int io = 0;
        uintptr_t base = pci_bar(d, b, &io);
        if (io || !base || base > addr) continue;
        if ((uint64_t)(addr - base) < best) best = addr - base;
    }
    return best;
}

static gpu_t* g_fw;

static int fw_cb(const pci_dev_t* d, void* ctx) {
    uint64_t* best = ctx;
    if (d->class_code != 0x03) return 0;
    if (d->vendor != 0x1002 && d->vendor != 0x10DE && d->vendor != 0x8086) return 0;
    const fb_info_t* fi = fb_info();
    /* the card whose memory the screen is in (iGPU + graphics card: the one showing it) */
    uint64_t dist = fi && fi->addr ? bar_distance(d, fi->addr) : ~0ull - 1;
    if (g_fw && dist >= *best) return 0;
    *best = dist;
    if (!g_fw) g_fw = (gpu_t*)kzalloc(sizeof(gpu_t));
    if (!g_fw) return 0;
    const char* fam = gpu_family_name(d->vendor, d->device);
    const char* ven = d->vendor == 0x1002 ? "AMD Radeon" : d->vendor == 0x10DE ? "NVIDIA" : "Intel";
    if (fam) kstrlcpy(g_fw->name, fam, sizeof(g_fw->name));
    else ksnprintf(g_fw->name, sizeof(g_fw->name), "%s graphics (%04x)", ven, d->device);
    g_fw->driver = "firmware";
    g_fw->pci = *d;
    g_fw->set_mode = fw_set_mode;
    g_fw->info = fw_info;
    return 0;
}

void gpu_firmware_init(void) {
    if (!fb_available()) return;
    uint64_t best = ~0ull;
    pci_scan(fw_cb, &best);
    const fb_info_t* fi = fb_info();
    if (g_fw && !gpu_active()) {
        if (fi && fi->addr && paging_set_wc(fi->addr, (uint64_t)fi->pitch * fi->height) == 0) g_fw->priv = (void*)1;
        gpu_register(g_fw);
        klog("gpu: %s - the firmware's framebuffer%s\n", g_fw->name, g_fw->priv ? ", write-combining" : "");
    }
}
