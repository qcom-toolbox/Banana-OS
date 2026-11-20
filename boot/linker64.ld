/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Banana OS 0.5 linker script - x86_64 kernel (identity mapped, at 1 MiB
 * like the 32-bit one; boot64.asm maps the first 4 GiB 1:1) */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text BLOCK(4K) : ALIGN(4K) {
        *(.multiboot2)
        /* preemptible kernel code (kernel/task.h, task_kpreempt): compute
         * that only touches what its caller hands it */
        __kpreempt_start = .;
        web/svg.o64(.text .text.*)
        third_party/stb/stb_image_impl.o64(.text .text.*)
        __kpreempt_end = .;
        *(.text .text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata .rodata.*)
        *(.eh_frame*)
        *(.ksyms)          /* (the symbol table, last: prof - the second link pass) */
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data .data.*)
    }

    /* the packages built into the kernel (kernel/appbin.asm, exbin.asm): on an
     * installed system they are kept as files on the disk, and these pages
     * go to the heap after boot (kernel/builtin_apps.c) */
    .payload BLOCK(4K) : ALIGN(4K) {
        __payload_start = .;
        *(.payload)
        . = ALIGN(4K);
        __payload_end = .;
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* First free byte after the image: the kernel heap starts here
     * (kernel/kheap.c). */
    _kernel_end = .;

    /DISCARD/ : {
        *(.note*)
        *(.comment)
    }
}
