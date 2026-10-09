/* Banana Boot (32-bit UEFI): the ELF layout objcopy turns into BOOTIA32.EFI
 * (PE32). Sections page-aligned from 0, as a PE image wants. */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
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
    .reloc : { *(.reloc) *(.peloc) }
    . = ALIGN(4096);
    .got : { *(.got .got.*) }           /* its own: tools/efi_relocs.py allows its one relocation */
    .data : {
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    . = ALIGN(4096);
    .rel : { *(.rel.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.interp) }
}
