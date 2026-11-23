/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Panic presentation uses only a static bitmap font and bounded integer
 * formatting. It never enters terminal/gfx/GUI code or allocates memory. */
#include "crash.h"
#include "font8x8.h"
#include "kstring.h"

#define BLUE 0x0759BCu
#define NAVY 0x091728u
#define PANEL 0x10243Au
#define WHITE 0xF4F8FFu
#define MUTED 0xB8CCE3u
#define YELLOW 0xFFD45Au

const char* crash_stop_code(const crash_report_t* r) {
    if (!r->exception)
        return "KERNEL_CHECK_FAILURE";
    switch (r->vector) {
    case 0:
        return "DIVIDE_ERROR";
    case 2:
        return "NON_MASKABLE_INTERRUPT";
    case 6:
        return "INVALID_KERNEL_INSTRUCTION";
    case 8:
        return "DOUBLE_FAULT";
    case 10:
        return "INVALID_TASK_STATE";
    case 11:
        return "SEGMENT_NOT_PRESENT";
    case 12:
        return "KERNEL_STACK_FAULT";
    case 13:
        return "GENERAL_PROTECTION_FAULT";
    case 14:
        return "PAGE_FAULT_IN_KERNEL";
    case 18:
        return "MACHINE_CHECK_EXCEPTION";
    default:
        return "UNHANDLED_CPU_EXCEPTION";
    }
}

static void line(crash_emit_t emit, void* user, int heading, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void line(crash_emit_t emit, void* user, int heading, const char* fmt, ...) {
    char b[160];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    emit(user, b, heading);
}

/* Width is explicit: a 64-bit address is never silently truncated. */
static void hex(char* out, uintptr_t n, int bits) {
    const char* digits = "0123456789ABCDEF";
    int width = bits == 32 ? 8 : 16;
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < width; i++)
        out[2 + i] = digits[((uint64_t)n >> ((width - i - 1) * 4)) & 15];
    out[2 + width] = 0;
}

void crash_format(const crash_report_t* r, crash_emit_t emit, void* user) {
    char a[20], b[20], c[20];
    line(emit, user, 1, "STOP CODE  %s", crash_stop_code(r));
    /* Wrap long check messages; never let one cover the next field. */
    for (int offset = 0; offset < 159 && r->reason[offset]; offset += 76) {
        char reason[77];
        int n = 0;
        while (n < 76 && offset + n < 159 && r->reason[offset + n]) {
            reason[n] = r->reason[offset + n];
            n++;
        }
        reason[n] = 0;
        emit(user, reason, 0);
    }
    line(emit, user, 1, "CONTEXT");
    line(emit, user, 0, "CPU %u/%u   PID %d   Task: %s", r->cpu, r->cpus, r->pid, r->task);
    line(emit, user, 0, "Banana OS 0.6 / %s / %s   Uptime: %u.%03u s", r->bits == 64 ? "x86_64" : "i686",
         r->uefi ? "UEFI" : "BIOS", r->uptime_ms / 1000, r->uptime_ms % 1000);
    if (r->preview)
        line(emit, user, 0, "PREVIEW: simulated fault/registers; the OS is still running.");
    line(emit, user, 1, "EXCEPTION DETAILS");
    if (r->exception)
        line(emit, user, 0, "Vector: %u (0x%02X)   Error: 0x%08X   Origin: ring %u", r->vector, r->vector,
             r->error, (unsigned)(r->cs & 3));
    else
        line(emit, user, 0, "Kernel check / explicit panic (no CPU exception frame).");
    if (r->exception && r->vector == 14) {
        hex(a, r->cr2, r->bits);
        line(emit, user, 0, "Fault address (CR2): %s", a);
        line(emit, user, 0, "%s / %s / %s%s%s%s%s",
             r->error & 1 ? "protection violation" : "page not present",
             r->error & 16  ? "instruction fetch"
             : r->error & 2 ? "write"
                            : "read",
             r->error & 4 ? "user" : "supervisor", r->error & 8 ? " / reserved-bit violation" : "",
             r->error & 32 ? " / protection key" : "", r->error & 64 ? " / shadow stack" : "",
             r->cr2 < 4096 ? " / near NULL" : "");
    } else if (r->exception && (r->vector == 10 || r->vector == 11 || r->vector == 12 || r->vector == 13)) {
        line(emit, user, 0, "Selector index: %u   Table: %s   External: %s", r->error >> 3,
             r->error & 2   ? "IDT"
             : r->error & 4 ? "LDT"
                            : "GDT",
             r->error & 1 ? "yes" : "no");
    }
    hex(a, r->ip, r->bits);
    hex(b, r->sp, r->bits);
    hex(c, r->bp, r->bits);
    line(emit, user, 0, "IP %s   SP %s", a, b);
    line(emit, user, 0, "BP %s   FLAGS 0x%08X   CS %04X SS %04X", c, (uint32_t)r->flags, (uint32_t)r->cs,
         (uint32_t)r->ss);
    line(emit, user, 1, r->exception ? "REGISTERS" : "CONTROL STATE (general registers unavailable)");
    static const char* const names64[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "R8",
                                          "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
    static const char* const names32[] = {"EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP"};
    int n = r->bits == 64 ? 15 : 7;
    if (r->exception)
        for (int i = 0; i < n; i += 3) {
            char row[104] = "", part[36];
            for (int j = i; j < i + 3 && j < n; j++) {
                hex(a, r->reg[j], r->bits);
                ksnprintf(part, sizeof(part), "%s %s", r->bits == 64 ? names64[j] : names32[j], a);
                if (j != i)
                    kstrlcat(row, "  ", sizeof(row));
                kstrlcat(row, part, sizeof(row));
            }
            emit(user, row, 0);
        }
    hex(a, r->cr0, r->bits);
    hex(b, r->cr3, r->bits);
    line(emit, user, 0, "CR0 %s   CR3 %s", a, b);
    hex(a, r->cr2, r->bits);
    hex(b, r->cr4, r->bits);
    line(emit, user, 0, "CR2 %s   CR4 %s", a, b);
    line(emit, user, 1, "STACK SNAPSHOT (raw words, not a call trace)");
    if (!r->stack_count)
        line(emit, user, 0, "Unavailable: SP is outside the known kernel stack.");
    for (int i = 0; i < r->stack_count && i < CRASH_STACK; i += 2) {
        hex(a, r->sp + (uintptr_t)i * (r->bits / 8), r->bits);
        hex(b, r->stack[i], r->bits);
        if (i + 1 < r->stack_count)
            hex(c, r->stack[i + 1], r->bits);
        else
            c[0] = 0;
        line(emit, user, 0, "%s: %s  %s", a, b, c);
    }
    line(emit, user, 1, "REPORTING");
    line(emit, user, 0, "Photograph the stop code, IP and fault address; keep the serial log.");
    line(emit, user, 0, "Host: addr2line -e %s -f -C <IP>", r->bits == 64 ? "kernel64.bin" : "kernel.bin");
}

/* Clip every write, including tiny screens and a framebuffer's row padding. */
static void rect(const crash_surface_t* s, int x, int y, int w, int h, uint32_t rgb) {
    if (!s || !s->pixels || s->pitch / 4 < s->width || w <= 0 || h <= 0)
        return;
    int x1 = x + w, y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int)s->width)
        x1 = (int)s->width;
    if (y1 > (int)s->height)
        y1 = (int)s->height;
    for (int yy = y; yy < y1; yy++) {
        volatile uint32_t* row = (volatile uint32_t*)((uintptr_t)s->pixels + (size_t)yy * s->pitch);
        for (int xx = x; xx < x1; xx++)
            row[xx] = rgb;
    }
}

static void text(const crash_surface_t* s, int x, int y, const char* str, int scale, uint32_t rgb) {
    int start = x;
    for (int n = 0; str[n] && n < 160; n++, x += 9 * scale) {
        if (x + 8 * scale > (int)s->width - start)
            break;
        unsigned ch = (uint8_t)str[n];
        if (ch >= 128)
            ch = '?';
        for (int py = 0; py < 12; py++)
            for (int px = 0; px < 8; px++)
                if (font8x8_basic[ch][py * 8 / 12] & (1u << px))
                    rect(s, x + px * scale, y + py * scale, scale, scale, rgb);
    }
}

typedef struct {
    const crash_surface_t* s;
    int x, y, scale, bottom, clipped;
} draw_t;
static void draw_line(void* user, const char* value, int heading) {
    draw_t* d = user;
    int cols = ((int)d->s->width - 2 * d->x) / (9 * d->scale);
    if (cols < 1) {
        d->clipped = 1;
        return;
    }
    const char* cursor = value;
    do {
        if (d->y + 12 * d->scale > d->bottom) {
            d->clipped = 1;
            return;
        }
        int n = 0;
        while (cursor[n] && n < cols && n < 159)
            n++;
        if (cursor[n]) {
            int split = 0;
            /* Keep register labels and their full addresses together. */
            for (int i = 1; i < n; i++)
                if (cursor[i] == ' ' && cursor[i - 1] == ' ')
                    split = i - 1;
            if (!split)
                for (int i = 1; i < n; i++)
                    if (cursor[i] == ' ')
                        split = i;
            if (split > 0)
                n = split;
        }
        char row[160];
        memcpy(row, cursor, n);
        row[n] = 0;
        text(d->s, d->x, d->y, row, d->scale, heading ? YELLOW : WHITE);
        d->y += 14 * d->scale;
        cursor += n;
        while (*cursor == ' ')
            cursor++;
    } while (*cursor);
}

void crash_render(const crash_report_t* r, const crash_surface_t* s) {
    if (!s || !s->pixels || !s->width || !s->height)
        return;
    int scale = s->width >= 1500 && s->height >= 950 ? 2 : 1;
    int margin = s->width >= 640 ? 24 * scale : 8;
    int header = 100 * scale, footer = 34 * scale;
    rect(s, 0, 0, s->width, s->height, NAVY);
    rect(s, 0, 0, s->width, header, BLUE);
    rect(s, margin, 16 * scale, 5 * scale, 64 * scale, YELLOW);
    text(s, margin + 18 * scale, 14 * scale, ":(", 3 * scale, WHITE);
    text(s, margin + 94 * scale, 16 * scale, "Banana OS stopped", 2 * scale, WHITE);
    text(s, margin + 94 * scale, 48 * scale,
         r->preview ? "CRASH SCREEN PREVIEW" : "A fatal kernel error stopped the system.", scale, WHITE);
    text(s, margin + 18 * scale, 76 * scale,
         r->preview ? "Simulated report. No crash has been triggered."
                    : "Diagnostics below may help identify what went wrong.",
         scale, WHITE);
    int panel_y = header + 12 * scale;
    rect(s, margin - 8, panel_y - 4, s->width - 2 * margin + 16, s->height - panel_y - footer, PANEL);
    draw_t d = {s, margin, panel_y, scale, (int)s->height - footer - 8, 0};
    crash_format(r, draw_line, &d);
    rect(s, 0, s->height - footer, s->width, footer, NAVY);
    text(s, margin, s->height - footer + 4 * scale,
         r->preview ? "Enter / Esc: return to the OS"
                    : "System halted. Restart using the power button or VM controls.",
         scale, WHITE);
    text(s, margin, s->height - footer + 18 * scale,
         d.clipped ? "Some rows do not fit. Full diagnostics are on serial (COM1)."
                   : "Full diagnostics are also sent to serial (COM1).",
         scale, MUTED);
}

void crash_sample(crash_report_t* r) {
    memset(r, 0, sizeof(*r));
    r->bits = sizeof(uintptr_t) * 8;
    r->preview = 1;
    r->exception = 1;
    r->vector = 14;
    r->error = 2;
    r->cs = 8;
    r->flags = 0x202;
    r->ip = 0x0012ABCD;
    r->sp = 0x0180FE00;
    r->bp = 0x0180FE80;
    r->cr2 = 0x28;
    r->cpus = 4;
    r->pid = 0;
    r->uptime_ms = 42819;
    r->cr0 = 0x80010033;
    r->cr3 = 0x103000;
    r->cr4 = 0x620;
    kstrlcpy(r->task, "banana-sh", sizeof(r->task));
    kstrlcpy(r->reason, "Page Fault: supervisor write to an unmapped address.", sizeof(r->reason));
    for (int i = 0; i < 15; i++)
        r->reg[i] = 0x12340000u + i * 0x111;
    r->stack_count = 8;
    for (int i = 0; i < 8; i++)
        r->stack[i] = 0x00101234u + i * 0x120;
}
