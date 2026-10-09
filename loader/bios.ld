/* Banana Boot, the BIOS loader: one flat file, loaded at 0x8000. Its first
 * 2 KiB (the CD entry) run at 0x7C00, where El Torito puts them. */
OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386)
ENTRY(cd_start)
SECTIONS
{
    .cdstub 0x7C00 : AT(0) { *(.cdstub) }
    .main 0x8800 : AT(0x800) {
        *(.entry16)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
    }
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
/* real mode reaches it with segment 0, and the MBR loads at most 32 KiB */
ASSERT(__bss_end <= 0x10000, "the BIOS loader is too big (it must end below 0x10000)")
ASSERT(SIZEOF(.cdstub) == 0x800, "the CD entry must be 2 KiB")
