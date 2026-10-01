/* Banana OS 0.5 linker script - x86_64 kernel (identity mapped, at 1 MiB
 * like the 32-bit one; boot64.asm maps the first 4 GiB 1:1) */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text BLOCK(4K) : ALIGN(4K) {
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
