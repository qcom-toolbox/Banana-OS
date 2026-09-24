/* Banana OS 0.5 linker script */
ENTRY(_start)

SECTIONS {
    . = 0x100000;       /* Load kernel at 1MB */

    .text BLOCK(4K) : ALIGN(4K) {
        *(.multiboot)
        *(.multiboot2)
        *(.text .text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata .rodata.*)
        *(.eh_frame*)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data .data.*)
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
