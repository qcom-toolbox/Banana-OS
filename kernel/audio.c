#include "audio.h"
#include "pci.h"
#include "io.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "serial.h"
#include "fs.h"

/*
 * Sound: one output stream, 48 kHz, 16-bit, stereo.
 *
 * The card plays a 64 KiB ring (DMA) over and over. The part it has
 * already played is refilled - from the queue audio_play() fills, or with
 * silence - from the timer interrupt every 2 ms (audio_tick): it reads the
 * card's play position and copies. No task has to get the CPU for that, so
 * nothing the rest of the system does (a big save, opening the browser, a
 * busy desktop) can let the ring run dry; only an empty queue is silence.
 * (This used to be the "audiod" task: anything holding the CPU for more
 * than the ring's ~340 ms made the music stutter.)
 *
 * The queue: audio_play() (tasks) adds at g_qhead, the interrupt takes at
 * g_qtail; g_qcount changes with atomic adds on both sides.
 */

#define RATE        48000
#define FRAME       4u                      /* bytes: 2 channels x 16 bits */
/* The ring is much bigger than what is written ahead of the card: the
 * card's position is read as an offset in the ring, so a refill late by
 * more than a whole ring would miscount how far it got - and write over
 * sound not heard yet (it skips: "speeds up"). With 1.36 s of ring that
 * takes a 1 s stall. AHEAD alone sets the latency (~300 ms). */
#define RING        262144u                 /* the card's DMA ring (~1.36 s) */
#define QUEUE       (RATE * FRAME * 4u)     /* 4 s queued at most */
#define AHEAD       57344u                  /* how far ahead of the card we write (~300 ms) */
#define CLEAR_AHEAD 32768u                  /* played sound turned to silence past our writes */

typedef struct {
    const char* name;
    int      (*start)(void);                /* ring set up: go */
    uint32_t (*position)(void);             /* byte the card plays now, 0..RING-1 */
    void     (*tick)(void);                 /* keep the stream running (AC'97: LVI) */
} card_t;

static const card_t* g_card;
static char          g_card_name[64] = "none";
static uint8_t*      g_ring;                /* RING bytes, DMA-able */
static uint8_t*      g_queue;               /* QUEUE bytes of frames waiting */
static volatile uint32_t g_qhead, g_qtail, g_qcount;
static uint32_t      g_wp;                  /* next ring byte we write */
static uint32_t      g_last_hw;
static uint64_t      g_hw_abs, g_wr_abs, g_data_end;   /* absolute byte counts */
static uint64_t      g_zero_abs;            /* the ring is silence up to here (past g_wr_abs) */
static int           g_volume = 80;
static volatile int  g_running;             /* the card plays: audio_tick() feeds it */

static inline uintptr_t irq_off(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_on(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* 4 KiB-aligned zeroed memory for DMA (the heap is identity-mapped) */
static void* dma_alloc(uint32_t size, uint32_t align) {
    uint8_t* raw = (uint8_t*)kmalloc(size + align);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    memset(p, 0, size);
    return p;
}

/* ══ the feeder ═══════════════════════════════════════════════════════ */

static void feed(void) {
    if (!g_card) return;
    if (g_card->tick) g_card->tick();
    uint32_t hw = g_card->position() % RING;
    uint32_t moved = (hw - g_last_hw + RING) % RING;
    g_last_hw = hw;
    g_hw_abs += moved;
    /* we are never more than AHEAD in front of the card */
    if (g_wr_abs < g_hw_abs) { g_wr_abs = g_hw_abs; g_wp = hw; }   /* fell behind (a long stall) */
    while (g_wr_abs - g_hw_abs < AHEAD) {
        uint32_t room = AHEAD - (uint32_t)(g_wr_abs - g_hw_abs);
        uint32_t chunk = RING - g_wp;           /* up to the end of the ring */
        if (chunk > room) chunk = room;
        if (chunk > 4096) chunk = 4096;
        uint32_t take = g_qcount < chunk ? g_qcount : chunk;
        take &= ~(FRAME - 1);
        for (uint32_t i = 0; i < take; i++) {
            g_ring[g_wp + i] = g_queue[g_qtail];
            g_qtail = (g_qtail + 1) % QUEUE;
        }
        __sync_fetch_and_sub(&g_qcount, take);
        if (take) g_data_end = g_wr_abs + take;
        if (take < chunk) memset(g_ring + g_wp + take, 0, chunk - take);
        g_wp = (g_wp + chunk) % RING;
        g_wr_abs += chunk;
    }
    /* Past what we wrote is sound the card played a lap ago: it becomes
     * silence, so a refill that comes too late gives a short gap - not old
     * sound played again. (AHEAD + CLEAR_AHEAD < RING: only played sound.) */
    if (g_zero_abs < g_wr_abs) g_zero_abs = g_wr_abs;
    while (g_zero_abs < g_wr_abs + CLEAR_AHEAD) {
        uint32_t zp = (g_wp + (uint32_t)(g_zero_abs - g_wr_abs)) % RING;
        uint32_t n = RING - zp;
        if (n > 4096) n = 4096;
        memset(g_ring + zp, 0, n);
        g_zero_abs += n;
    }
}

/* the timer interrupt, every millisecond (kernel/timer.c) */
void audio_tick(void) {
    static uint32_t n;
    if (!g_running || ++n % 2) return;
    feed();
}

/* ══ PC speaker ═══════════════════════════════════════════════════════ */

static void speaker_on(int hz) {
    if (hz < 20) hz = 20;
    uint32_t div = 1193182u / (uint32_t)hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, inb(0x61) | 3);
}

static void speaker_off(void) {
    outb(0x61, inb(0x61) & ~3);
}

/* ══ Intel AC'97 (ICH) ════════════════════════════════════════════════ */

#define AC_SEG    (RING / 32u)          /* bytes per buffer descriptor */

static uint16_t g_nam, g_nabm;          /* mixer / bus master I/O bases */
static uint32_t* g_ac_bdl;

static uint32_t ac97_position(void) {
    uint8_t civ = inb(g_nabm + 0x14) & 31;
    uint16_t picb = inw(g_nabm + 0x18);       /* samples left in the current buffer */
    uint32_t left = (uint32_t)picb * 2;
    if (left > AC_SEG) left = AC_SEG;
    return civ * AC_SEG + (AC_SEG - left);
}

static void ac97_tick(void) {
    uint8_t civ = inb(g_nabm + 0x14) & 31;
    outb(g_nabm + 0x15, (uint8_t)((civ + 30) & 31));   /* the last valid buffer stays ahead */
    uint16_t sr = inw(g_nabm + 0x16);
    if (sr & 0x1C) outw(g_nabm + 0x16, sr & 0x1C);     /* clear the status bits */
    if (sr & 1) outb(g_nabm + 0x1B, 0x01);              /* halted: run again */
}

static int ac97_start(void) {
    outb(g_nabm + 0x1B, 0x02);                           /* reset the PCM out registers */
    for (int i = 0; i < 100 && (inb(g_nabm + 0x1B) & 0x02); i++) timer_sleep_ms(1);
    for (int i = 0; i < 32; i++) {
        g_ac_bdl[i * 2] = (uint32_t)(uintptr_t)(g_ring + i * AC_SEG);
        g_ac_bdl[i * 2 + 1] = (AC_SEG / 2);              /* samples, no interrupts */
    }
    outl(g_nabm + 0x10, (uint32_t)(uintptr_t)g_ac_bdl);
    outb(g_nabm + 0x15, 30);
    outb(g_nabm + 0x1B, 0x01);                           /* run */
    return 0;
}

static const card_t g_ac97 = { "AC'97", ac97_start, ac97_position, ac97_tick };

static int ac97_init(const pci_dev_t* pd) {
    int io0, io1;
    uintptr_t b0 = pci_bar(pd, 0, &io0), b1 = pci_bar(pd, 1, &io1);
    if (!io0 || !io1 || !b0 || !b1) return -1;
    pci_enable(pd);
    g_nam = (uint16_t)b0;
    g_nabm = (uint16_t)b1;
    /* a cold reset, then wait for the codec */
    outl(g_nabm + 0x2C, 0x00000002);
    for (int i = 0; i < 100 && !(inl(g_nabm + 0x30) & 0x100); i++) timer_sleep_ms(2);
    if (!(inl(g_nabm + 0x30) & 0x100)) { klog("ac97: no codec\n"); return -1; }
    outw(g_nam + 0x00, 0);                               /* codec reset */
    timer_sleep_ms(5);
    outw(g_nam + 0x02, 0x0000);                          /* master: 0 dB, unmuted */
    outw(g_nam + 0x04, 0x0000);                          /* headphones */
    outw(g_nam + 0x18, 0x0808);                          /* PCM out: 0 dB */
    if (inw(g_nam + 0x28) & 1) {                         /* variable rate: ask for 48 kHz */
        outw(g_nam + 0x2A, inw(g_nam + 0x2A) | 1);
        outw(g_nam + 0x2C, RATE);
    }
    g_ac_bdl = (uint32_t*)dma_alloc(32 * 8, 4096);
    if (!g_ac_bdl) return -1;
    ksnprintf(g_card_name, sizeof(g_card_name), "Intel AC'97 (%04x:%04x)", pd->vendor, pd->device);
    g_card = &g_ac97;
    return 0;
}

/* ══ Intel High Definition Audio ══════════════════════════════════════ */

static volatile uint8_t* g_hda;
static uint32_t* g_corb;
static uint64_t* g_rirb;
static uint16_t  g_corb_wp, g_rirb_rp;
static uint32_t  g_sd;                  /* the output stream descriptor's offset */
static uint64_t* g_hda_bdl;
static int       g_cad;                 /* codec address */

static uint32_t hr32(uint32_t o) { return *(volatile uint32_t*)(g_hda + o); }
static uint16_t hr16(uint32_t o) { return *(volatile uint16_t*)(g_hda + o); }
static uint8_t  hr8(uint32_t o)  { return *(volatile uint8_t*)(g_hda + o); }
static void hw32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(g_hda + o) = v; }
static void hw16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(g_hda + o) = v; }
static void hw8(uint32_t o, uint8_t v)   { *(volatile uint8_t*)(g_hda + o) = v; }

/* one verb to the codec, its answer (0xFFFFFFFF on a timeout) */
static uint32_t hda_cmd(int nid, uint32_t verb_payload) {
    uint32_t v = ((uint32_t)g_cad << 28) | ((uint32_t)nid << 20) | verb_payload;
    g_corb_wp = (uint16_t)((g_corb_wp + 1) & 0xFF);
    g_corb[g_corb_wp] = v;
    hw16(0x48, g_corb_wp);
    uint32_t start = timer_ms();
    while ((hr16(0x58) & 0xFF) == g_rirb_rp) {
        if (timer_ms() - start > 50) return 0xFFFFFFFFu;
        timer_idle();
    }
    g_rirb_rp = (uint16_t)((g_rirb_rp + 1) & 0xFF);
    hw8(0x5D, 0x05);                                     /* clear RIRB status */
    return (uint32_t)g_rirb[g_rirb_rp];
}

static uint32_t hda_param(int nid, int param) { return hda_cmd(nid, 0xF0000u | (uint32_t)param); }
/* the "12-bit" verbs: verb << 8 | 8-bit payload; the "4-bit" ones: verb << 16 | 16-bit payload */
static uint32_t verb12(int nid, uint32_t verb, uint32_t payload) { return hda_cmd(nid, (verb << 8) | (payload & 0xFF)); }
static uint32_t verb4(int nid, uint32_t verb, uint32_t payload) { return hda_cmd(nid, (verb << 16) | (payload & 0xFFFF)); }

static int widget_type(int nid) { return (int)((hda_param(nid, 0x09) >> 20) & 0xF); }

/* nid's connection list (up to max) */
static int connections(int nid, int* out, int max) {
    uint32_t len = hda_param(nid, 0x0E);
    int n = (int)(len & 0x7F), lng = (len >> 7) & 1, got = 0;
    for (int i = 0; i < n && got < max; i += lng ? 2 : 4) {
        uint32_t e = verb12(nid, 0xF02, (uint32_t)i);
        for (int k = 0; k < (lng ? 2 : 4) && i + k < n && got < max; k++)
            out[got++] = lng ? (int)((e >> (16 * k)) & 0xFFFF) : (int)((e >> (8 * k)) & 0xFF);
    }
    return got;
}

/* unmutes nid's output (and input) amplifiers at 0 dB */
static void amp_on(int nid) {
    uint32_t caps = hda_param(nid, 0x09);
    if (caps & (1u << 2)) {                      /* output amp */
        uint32_t ac = hda_param(nid, 0x12);
        uint32_t gain = ac & 0x7F;               /* the 0 dB step */
        verb4(nid, 0x3, 0xB000 | gain);          /* output, left + right */
    }
    if (caps & (1u << 1)) {                      /* input amps: unmute index 0 */
        uint32_t ac = hda_param(nid, 0x0D);
        uint32_t gain = ac & 0x7F;
        verb4(nid, 0x3, 0x7000 | gain);
    }
}

/* depth-first: a path from widget nid to a DAC; fills path[], its length */
static int find_dac(int nid, int* path, int depth) {
    if (depth >= 6) return 0;
    path[depth] = nid;
    int t = widget_type(nid);
    if (t == 0) return depth + 1;                /* audio output (DAC) */
    if (t == 1 || t > 4) return 0;               /* input, power, volume knob, ... */
    int conn[16];
    int n = connections(nid, conn, 16);
    for (int i = 0; i < n; i++) {
        int got = find_dac(conn[i], path, depth + 1);
        if (got) {
            if (n > 1) verb12(nid, 0x701, (uint32_t)i);   /* select that input */
            return got;
        }
    }
    return 0;
}

static uint32_t hda_position(void) { return hr32(g_sd + 0x04) % RING; }

static int hda_start(void) {
    /* stream reset, then: ring, 2 descriptors, format, stream tag 1, run */
    hw8(g_sd + 0x00, hr8(g_sd) | 1);
    for (int i = 0; i < 100 && !(hr8(g_sd) & 1); i++) timer_sleep_ms(1);
    hw8(g_sd + 0x00, hr8(g_sd) & ~1);
    for (int i = 0; i < 100 && (hr8(g_sd) & 1); i++) timer_sleep_ms(1);
    for (int i = 0; i < 2; i++) {
        g_hda_bdl[i * 2] = (uint64_t)(uintptr_t)(g_ring + i * (RING / 2));
        g_hda_bdl[i * 2 + 1] = RING / 2;          /* length, no interrupt */
    }
    hw32(g_sd + 0x18, (uint32_t)(uintptr_t)g_hda_bdl);
    hw32(g_sd + 0x1C, 0);
    hw32(g_sd + 0x08, RING);
    hw16(g_sd + 0x0C, 1);
    hw16(g_sd + 0x12, 0x0011);                   /* 48 kHz, 16-bit, 2 channels */
    hw8(g_sd + 0x02, 1 << 4);                    /* stream tag 1 */
    hw8(g_sd + 0x00, hr8(g_sd) | 2);             /* run */
    return 0;
}

static const card_t g_hdac = { "HDA", hda_start, hda_position, NULL };

static int hda_init(const pci_dev_t* pd) {
    int io;
    uintptr_t bar = pci_bar(pd, 0, &io);
    if (io || !bar) return -1;
    pci_enable(pd);
    g_hda = (volatile uint8_t*)(uintptr_t)bar;
    /* controller reset */
    hw32(0x08, hr32(0x08) & ~1u);
    for (int i = 0; i < 100 && (hr32(0x08) & 1); i++) timer_sleep_ms(1);
    hw32(0x08, hr32(0x08) | 1);
    for (int i = 0; i < 100 && !(hr32(0x08) & 1); i++) timer_sleep_ms(1);
    if (!(hr32(0x08) & 1)) { klog("hda: controller does not come out of reset\n"); return -1; }
    timer_sleep_ms(2);
    uint16_t codecs = hr16(0x0E);
    if (!codecs) { klog("hda: no codec\n"); return -1; }
    g_cad = 0;
    while (!(codecs & (1u << g_cad))) g_cad++;
    uint16_t gcap = hr16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { klog("hda: no output stream\n"); return -1; }
    g_sd = 0x80 + (uint32_t)iss * 0x20;

    /* CORB / RIRB: 256 entries each */
    g_corb = (uint32_t*)dma_alloc(1024, 128);
    g_rirb = (uint64_t*)dma_alloc(2048, 128);
    g_hda_bdl = (uint64_t*)dma_alloc(64, 128);
    if (!g_corb || !g_rirb || !g_hda_bdl) return -1;
    hw8(0x4C, 0);                                     /* stop the CORB */
    hw8(0x5C, 0);
    timer_sleep_ms(1);
    hw32(0x40, (uint32_t)(uintptr_t)g_corb); hw32(0x44, 0);
    hw8(0x4E, 0x02);                                  /* 256 entries */
    hw16(0x4A, 0x8000);                               /* read pointer reset */
    for (int i = 0; i < 50 && !(hr16(0x4A) & 0x8000); i++) timer_sleep_ms(1);
    hw16(0x4A, 0);
    for (int i = 0; i < 50 && (hr16(0x4A) & 0x8000); i++) timer_sleep_ms(1);
    hw16(0x48, 0);
    g_corb_wp = 0;
    hw32(0x50, (uint32_t)(uintptr_t)g_rirb); hw32(0x54, 0);
    hw8(0x5E, 0x02);
    hw16(0x58, 0x8000);                               /* write pointer reset */
    hw16(0x5A, 0xFF);                                 /* responses before the status bit (cleared after each) */
    g_rirb_rp = 0;
    hw8(0x4C, 0x02);                                  /* run CORB */
    hw8(0x5C, 0x03);                                  /* run RIRB; the "interrupt" only sets RIRBSTS (INTCTL stays off) */
    timer_sleep_ms(1);

    /* the codec: its audio function group, a pin that can output, a DAC behind it */
    uint32_t sub = hda_param(0, 0x04);
    if (sub == 0xFFFFFFFFu) { klog("hda: codec does not answer\n"); return -1; }
    int fg0 = (int)((sub >> 16) & 0xFF), nfg = (int)(sub & 0xFF), afg = -1;
    klog("hda: root %08x vendor %08x fg %08x\n", sub, hda_param(0, 0x00), hda_param(fg0, 0x05));
    for (int i = 0; i < nfg; i++)
        if ((hda_param(fg0 + i, 0x05) & 0xFF) == 1) { afg = fg0 + i; break; }
    if (afg < 0) { klog("hda: no audio function group\n"); return -1; }
    verb12(afg, 0x705, 0);                            /* power: D0 */
    uint32_t ws = hda_param(afg, 0x04);
    int w0 = (int)((ws >> 16) & 0xFF), nw = (int)(ws & 0xFF);
    int best = -1, best_score = -1, path[8], plen = 0;
    for (int nid = w0; nid < w0 + nw; nid++) {
        if (widget_type(nid) != 4) continue;          /* pin complex */
        if (!(hda_param(nid, 0x0C) & (1u << 4))) continue;   /* not output capable */
        uint32_t cfg = verb12(nid, 0xF1C, 0);
        int conn = (int)(cfg >> 30), dev = (int)((cfg >> 20) & 0xF);
        if (conn == 1) continue;                      /* nothing plugged there, ever */
        int score = dev == 0 ? 3 : dev == 1 ? 2 : dev == 2 ? 1 : 0;   /* line out, speaker, headphones */
        int tmp[8];
        if (score > best_score && find_dac(nid, tmp, 0)) { best = nid; best_score = score; }
    }
    if (best < 0) { klog("hda: no output pin\n"); return -1; }
    plen = find_dac(best, path, 0);
    for (int i = 0; i < plen; i++) {
        verb12(path[i], 0x705, 0);                    /* D0 */
        amp_on(path[i]);
    }
    uint32_t pcap = hda_param(best, 0x0C);
    verb12(best, 0x707, 0x40 | (best_score == 1 ? 0x80 : 0));   /* output enable (+ headphone amp) */
    if (pcap & (1u << 16)) verb12(best, 0x70C, 0x02);           /* external amplifier on */
    int dac = path[plen - 1];
    verb12(dac, 0x706, 0x10);                         /* stream tag 1, channel 0 */
    verb4(dac, 0x2, 0x0011);                          /* 48 kHz, 16-bit, stereo */
    ksnprintf(g_card_name, sizeof(g_card_name), "Intel HD Audio (%04x:%04x, codec %d)", pd->vendor, pd->device, g_cad);
    klog("hda: output pin %d -> DAC %d (%d widgets on the way)\n", best, dac, plen);
    g_card = &g_hdac;
    return 0;
}

/* ══ probe ════════════════════════════════════════════════════════════ */

typedef struct { pci_dev_t hda, ac97; int have_hda, have_ac97; } probe_t;

static int probe_cb(const pci_dev_t* d, void* ctx) {
    probe_t* p = (probe_t*)ctx;
    if (d->class_code != 0x04) return 0;
    if (d->subclass == 0x03 && !p->have_hda) { p->hda = *d; p->have_hda = 1; }
    if (d->subclass == 0x01 && !p->have_ac97) { p->ac97 = *d; p->have_ac97 = 1; }
    return 0;
}

void audio_init(void) {
    probe_t p;
    memset(&p, 0, sizeof(p));
    pci_scan(probe_cb, &p);
    g_ring = (uint8_t*)dma_alloc(RING, 4096);
    g_queue = (uint8_t*)kmalloc(QUEUE);
    if (!g_ring || !g_queue) return;
    if (p.have_hda && hda_init(&p.hda) != 0) g_card = NULL;
    if (!g_card && p.have_ac97 && ac97_init(&p.ac97) != 0) g_card = NULL;
    if (!g_card) { kstrlcpy(g_card_name, "none (PC speaker only)", sizeof(g_card_name)); return; }
    g_card->start();
    g_last_hw = g_card->position();
    g_wp = g_last_hw;
    klog("audio: %s\n", g_card_name);
    g_running = 1;                          /* audio_tick() keeps it fed from now on */
}

int audio_available(void) { return g_card != NULL; }
const char* audio_device_name(void) { return g_card_name; }

/* ══ playing ══════════════════════════════════════════════════════════ */

static void queue_frame(int16_t l, int16_t r) {
    uint8_t* q = g_queue;
    q[g_qhead] = (uint8_t)l; q[(g_qhead + 1) % QUEUE] = (uint8_t)((uint16_t)l >> 8);
    q[(g_qhead + 2) % QUEUE] = (uint8_t)r; q[(g_qhead + 3) % QUEUE] = (uint8_t)((uint16_t)r >> 8);
    g_qhead = (g_qhead + 4) % QUEUE;
    __sync_fetch_and_add(&g_qcount, 4);     /* (the interrupt takes from the other end) */
}

static int16_t sample_at(const uint8_t* p, int bits) {
    if (bits == 8) return (int16_t)(((int)p[0] - 128) << 8);
    return (int16_t)(p[0] | (p[1] << 8));
}

int audio_play(const void* pcm, uint32_t bytes, int rate, int channels, int bits) {
    if (!g_card) return -1;
    if ((bits != 8 && bits != 16) || channels < 1 || channels > 2 || rate < 1000 || rate > 192000) return -1;
    const uint8_t* src = (const uint8_t*)pcm;
    uint32_t fsz = (uint32_t)(channels * bits / 8);
    uint32_t frames = bytes / fsz;
    if (!frames) return 0;
    /* resample with linear interpolation: pos advances rate/48000 per output frame (16.16) */
    uint64_t step = ((uint64_t)rate << 16) / RATE;
    uint64_t pos = 0, end = (uint64_t)frames << 16;
    int vol = g_volume;
    while (pos < end) {
        while (QUEUE - g_qcount < 4096) task_sleep_ms(10);   /* full: let it play */
        for (int n = 0; n < 1024 && pos < end; n++, pos += step) {
            uint32_t i = (uint32_t)(pos >> 16), fr = (uint32_t)(pos & 0xFFFF);
            uint32_t j = i + 1 < frames ? i + 1 : i;
            const uint8_t* a = src + i * fsz;
            const uint8_t* b = src + j * fsz;
            int l0 = sample_at(a, bits), l1 = sample_at(b, bits);
            int r0 = channels == 2 ? sample_at(a + bits / 8, bits) : l0;
            int r1 = channels == 2 ? sample_at(b + bits / 8, bits) : l1;
            int l = l0 + (int)(((int64_t)(l1 - l0) * fr) >> 16);
            int r = r0 + (int)(((int64_t)(r1 - r0) * fr) >> 16);
            queue_frame((int16_t)(l * vol / 100), (int16_t)(r * vol / 100));
        }
    }
    return 0;
}

/* how long until what is queued (and in the card's ring) has played */
uint32_t audio_queued_ms(void) {
    if (!g_card) return 0;
    uintptr_t f = irq_off();                /* (64-bit counts the interrupt updates) */
    uint64_t pending = g_qcount + (g_data_end > g_hw_abs ? g_data_end - g_hw_abs : 0);
    irq_on(f);
    return (uint32_t)(pending / FRAME * 1000 / RATE);
}

int audio_busy(void) {
    if (!g_card) return 0;
    uintptr_t f = irq_off();
    int busy = g_qcount > 0 || g_hw_abs < g_data_end;
    irq_on(f);
    return busy;
}

void audio_stop(void) {
    uintptr_t f = irq_off();
    g_qtail = g_qhead;
    g_qcount = 0;
    g_data_end = 0;
    irq_on(f);
}

void audio_wait(void) {
    while (audio_busy()) task_sleep_ms(20);
}

void audio_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;
}

int audio_get_volume(void) { return g_volume; }

void audio_beep(int hz, int ms) {
    if (ms <= 0) return;
    if (ms > 5000) ms = 5000;
    if (!g_card) {
        speaker_on(hz);
        task_sleep_ms((uint32_t)ms);
        speaker_off();
        return;
    }
    /* a square wave through the card */
    const int rate = 24000;
    int n = rate * ms / 1000;
    int16_t* buf = (int16_t*)kmalloc((uint32_t)n * 2);
    if (!buf) return;
    int period = hz > 0 ? rate / hz : rate;
    if (period < 2) period = 2;
    for (int i = 0; i < n; i++) {
        int a = (i % period) < period / 2 ? 9000 : -9000;
        int edge = rate / 200;
        if (i < edge) a = a * i / edge;
        if (n - i < edge) a = a * (n - i) / edge;
        buf[i] = (int16_t)a;
    }
    audio_play(buf, (uint32_t)n * 2, rate, 1, 16);
    kfree(buf);
    audio_wait();
}

/* ══ .wav files ═══════════════════════════════════════════════════════ */

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int audio_play_wav(const char* path, char* err, int ecap) {
    if (!g_card) { kstrlcpy(err, "no sound card (try QEMU -device intel-hda -device hda-output, or -device AC97)", (size_t)ecap); return -1; }
    int fi = fs_find_file(path);
    if (fi < 0) { kstrlcpy(err, "no such file", (size_t)ecap); return -1; }
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(err, "cannot read it", (size_t)ecap); return -1; }
    const uint8_t* d = (const uint8_t*)f->content;
    uint32_t size = f->size;
    if (size < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) { kstrlcpy(err, "not a .wav file", (size_t)ecap); return -1; }
    int ch = 0, bits = 0, rate = 0, fmt = 0;
    const uint8_t* data = NULL;
    uint32_t dlen = 0;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t clen = rd32(d + off + 4);
        if (memcmp(d + off, "fmt ", 4) == 0 && clen >= 16 && off + 8 + 16 <= size) {
            fmt = rd16(d + off + 8);
            ch = rd16(d + off + 10);
            rate = (int)rd32(d + off + 12);
            bits = rd16(d + off + 22);
        } else if (memcmp(d + off, "data", 4) == 0) {
            data = d + off + 8;
            dlen = clen > size - off - 8 ? size - off - 8 : clen;
            break;
        }
        off += 8 + clen + (clen & 1);
    }
    if (fmt != 1) { kstrlcpy(err, "only uncompressed PCM .wav files", (size_t)ecap); return -1; }
    if (!data || (bits != 8 && bits != 16) || ch < 1 || ch > 2) { kstrlcpy(err, "8/16-bit mono/stereo PCM only", (size_t)ecap); return -1; }
    /* the file may change while it plays (it is queued in pieces): copy it */
    uint8_t* copy = (uint8_t*)kmalloc(dlen);
    if (!copy) { kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
    memcpy(copy, data, dlen);
    int rc = audio_play(copy, dlen, rate, ch, bits);
    kfree(copy);
    return rc;
}

void audio_list(void) {
    char line[96];
    ksnprintf(line, sizeof(line), "sound card: %s", g_card_name);
    terminal_writeln(line);
    if (g_card) {
        ksnprintf(line, sizeof(line), "output:     48000 Hz, 16-bit stereo, volume %d%%%s", g_volume,
                  audio_busy() ? ", playing" : "");
        terminal_writeln(line);
    }
    terminal_writeln("PC speaker: yes (beep)");
}
