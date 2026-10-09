/* Banana Boot (UEFI): from the firmware's 64-bit long mode to the Multiboot2
 * machine state the kernels start in - 32-bit protected mode, paging off,
 * EAX = 0x36D76289, EBX = the information. efi.c copies this code into a
 * page below 4 GiB (identity-mapped, like all of memory under UEFI), fills
 * in tramp_gdtr_base, and calls it with RDI = the kernel's entry, RSI = the
 * information (both below 4 GiB).
 *
 * Also the PE file's .reloc section: firmware wants one, and this program
 * has no relocations (it is position-independent), so it holds one empty
 * block. */

    .section .text
    .globl tramp_start, tramp_end, tramp_gdt, tramp_gdtr_base
    .code64
tramp_start:
    cli
    lgdt tramp_gdtr(%rip)
    lea compat(%rip), %rax
    pushq $0x08                         /* 32-bit code */
    pushq %rax
    lretq                               /* -> compatibility mode */
    .code32
compat:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov %cr0, %eax                      /* paging off: out of long mode */
    and $0x7FFFFFFF, %eax
    mov %eax, %cr0
    mov $0xC0000080, %ecx               /* EFER.LME off */
    rdmsr
    and $0xFFFFFEFF, %eax
    wrmsr
    xor %eax, %eax                      /* CR4 as a BIOS leaves it */
    mov %eax, %cr4
    mov %esi, %ebx
    mov $0x36D76289, %eax
    jmp *%edi

    .balign 8
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08 32-bit code */
    .quad 0x00CF92000000FFFF            /* 0x10 data */
tramp_gdtr:
    .word 23
tramp_gdtr_base:
    .quad 0
tramp_end:

    .section .peloc, "a"
    .long 0                             /* page RVA */
    .long 10                            /* block size */
    .word 0                             /* IMAGE_REL_BASED_ABSOLUTE: nothing */
