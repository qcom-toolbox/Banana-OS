/* Banana Boot (32-bit UEFI): from the firmware's 32-bit protected mode to the
 * Multiboot2 machine state - our flat GDT, paging off, EAX = 0x36D76289,
 * EBX = the information. efi.c copies this into a page of its own, fills in
 * tramp_gdtr_base, and calls it (cdecl): tramp(entry, info).
 *
 * Also the PE file's .reloc section (one empty block): see tramp.S. */

    .section .text
    .globl tramp_start, tramp_end, tramp_gdt, tramp_gdtr_base
    .code32
tramp_start:
    cli
    mov 4(%esp), %edi                   /* the kernel's entry */
    mov 8(%esp), %esi                   /* the information */
    call 1f
1:  pop %ebp                            /* where this copy runs */
    lgdt (tramp_gdtr - 1b)(%ebp)
    lea (2f - 1b)(%ebp), %eax
    pushl $0x08                         /* our 32-bit code segment */
    pushl %eax
    lret
2:  mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov %cr0, %eax                      /* paging off (if the firmware had it on) */
    and $0x7FFFFFFF, %eax
    mov %eax, %cr0
    xor %eax, %eax
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
    .quad 0                             /* (efi.c writes 8 bytes; lgdt reads 4 here) */
tramp_end:

    .section .peloc, "a"
    .long 0
    .long 10
    .word 0
