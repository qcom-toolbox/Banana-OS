/* edu - a Driver Kit example: QEMU's "edu" teaching device (qemu -device
 * edu, PCI 1234:11e8). It shows the parts of a PCI driver: matching the
 * device, its registers in memory (BAR0), an interrupt, a task.
 *
 * The device computes factorials: write n to register 0x08, it raises an
 * interrupt when n! is ready to be read back from the same register. */
#include "banana_driver.h"

#define EDU_ID        0x00     /* 0xRRrr00ed: version */
#define EDU_LIVENESS  0x04     /* reads back what was written, inverted */
#define EDU_FACT      0x08
#define EDU_STATUS    0x20     /* bit 0: computing, bit 7: interrupt when done */
#define EDU_IRQ_STAT  0x24
#define EDU_IRQ_ACK   0x64

static uint64_t g_regs;
static volatile int g_done;

static uint32_t rd(uint32_t r) { return *bdrv_reg32(g_regs, r); }
static void wr(uint32_t r, uint32_t v) { *bdrv_reg32(g_regs, r) = v; }

/* interrupts: the line may be shared, so only if it is ours */
static void edu_irq(void* ctx) {
    (void)ctx;
    uint32_t st = rd(EDU_IRQ_STAT);
    if (!st) return;
    wr(EDU_IRQ_ACK, st);
    g_done = 1;
}

static uint32_t factorial(uint32_t n) {
    g_done = 0;
    wr(EDU_STATUS, 0x80);                    /* an interrupt when it is done */
    wr(EDU_FACT, n);
    uint32_t t0 = bdrv->ticks_ms();
    while (!g_done && bdrv->ticks_ms() - t0 < 1000) bdrv->sleep_ms(1);
    if (!g_done) bdrv->log("no interrupt - polling the status instead");
    while (rd(EDU_STATUS) & 1) ;
    return rd(EDU_FACT);
}

/* a task of the driver: the device's results in the log, now and then */
static void worker(void* arg) {
    (void)arg;
    for (uint32_t n = 1; n <= 12; n++) {
        bdrv->log("%u! = %u", n, factorial(n));
        bdrv->sleep_ms(500);
    }
}

static int edu_probe(const bdrv_pci_t* d) {
    bdrv->pci_enable(d);
    int io = 0;
    g_regs = bdrv->pci_bar(d, 0, &io);
    if (!g_regs || io) return -1;
    uint32_t id = rd(EDU_ID);
    wr(EDU_LIVENESS, 0x12345678u);
    if (rd(EDU_LIVENESS) != ~0x12345678u) { bdrv->log("the liveness check failed"); return -1; }
    bdrv->log("QEMU edu device %u.%u at %02x:%02x.%u, irq %u", id >> 24, (id >> 16) & 0xFF, d->bus, d->dev, d->fn, d->irq_line);
    if (d->irq_line && d->irq_line < 16) bdrv->irq_install(d->irq_line, edu_irq, 0);
    bdrv->log("5! = %u", factorial(5));
    bdrv->task_create("edu", worker, 0);
    return 0;
}

static const bdrv_pci_id_t IDS[] = { { 0x1234, 0x11E8, 0xFF, 0xFF } };
static const bdrv_pci_driver_t DRV = { "edu", "QEMU edu teaching device", IDS, 1, edu_probe };

int banana_driver_main(const banana_driver_api_t* api) {
    api->log("edu driver %s, Banana OS %s (%s)", "1.0", api->os_version, api->arch);
    return api->register_pci_driver(&DRV) > 0 ? 0 : 1;
}
