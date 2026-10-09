/* Banana Boot (UEFI): the ELF layout objcopy turns into BOOTX64.EFI (PE32+).
 * Sections page-aligned from 0, as a PE image wants. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)
SECTIONS
{
    . = 0;
    ImageBase = .;
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    . = ALIGN(4096);
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .reloc : { *(.reloc) }
    . = ALIGN(4096);
    .data : {
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    . = ALIGN(4096);
    .rela : { *(.rela.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.interp) }
}
