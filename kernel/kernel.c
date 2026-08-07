#include "terminal.h"
#include "keyboard.h"
#include "sysinfo.h"
#include "task.h"
#include "timer.h"
#include "rtc.h"
#include "usb.h"
#include "daemon.h"
#include "types.h"
#include "gui.h"
#include "fb.h"
#include "gfx.h"
#include "idt.h"
#include "../shell/shell.h"

#define MULTIBOOT2_MAGIC 0x36D76289

void kernel_main(uint32_t magic, uint32_t mb_info) {
    /* Install exception handlers before anything else: a fault before
     * this point is unrecoverable anyway, but once the IDT is live, any
     * CPU exception paints a readable panic screen instead of silently
     * triple-faulting into a VM reset. */
    idt_init();

    /* Parse Multiboot2 info first so the console can choose framebuffer mode. */
    if (magic == MULTIBOOT2_MAGIC) {
        fb_init_multiboot2(mb_info);
        gfx_init();
        sysinfo_init_mb2(mb_info);
    }

    terminal_init();
    timer_init();
    rtc_init();
    task_init("banana-sh");   /* boot stack becomes the shell's real thread */
    task_start_sysmon();      /* real background stats-sampling thread */
    usb_init();       /* xHCI legacy handoff → USB keyboards work via PS/2 */
    mouse_init();     /* enable PS/2 AUX port for USB/PS2 mice */
    keyboard_init();  /* drain buffer after USB init */
    daemon_init();
    gui_init();

    if (magic != MULTIBOOT2_MAGIC) {
        terminal_write_color(
            "ERROR: Not loaded by a Multiboot2 bootloader!\n",
            VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        while (1) __asm__("hlt");
    }

    shell_run();
}
