; kernel/smp_tramp.asm
;
; The first code another processor core runs (kernel/smp.c copies it to
; 0x8000 and sends the core a STARTUP IPI for that page). The core starts
; in 16-bit real mode with CS = 0x0800; this switches it to protected mode
; with a flat GDT, turns on PAE + long mode + paging with the boot core's
; page tables (CR3/CR4/CR0/EFER as smp.c filled them into the data at the
; end), then calls smp_ap_entry(cpu) in 64-bit mode on the stack smp.c
; allocated for it. Every address is TRAMP + (label - smp_tramp_start):
; the code runs from the copy, not from where it is linked.

TRAMP equ 0x8000

section .rodata
global smp_tramp_start
global smp_tramp_end
global smp_tramp_data

align 16
bits 16
smp_tramp_start:
    cli
    cld
    mov ax, cs
    mov ds, ax
    lgdt [gdtr32 - smp_tramp_start]
    mov eax, cr0
    or eax, 1                       ; protection on
    mov cr0, eax
    jmp dword 0x08:(TRAMP + pm32 - smp_tramp_start)

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [TRAMP + d_cr4 - smp_tramp_start]
    mov cr4, eax                    ; PAE (and OSFXSR...) like the boot core
    mov eax, [TRAMP + d_cr3 - smp_tramp_start]
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER: long mode enable
    mov eax, [TRAMP + d_efer - smp_tramp_start]
    xor edx, edx
    wrmsr
    mov eax, [TRAMP + d_cr0 - smp_tramp_start]
    mov cr0, eax                    ; paging on: long mode active
    lgdt [TRAMP + gdtr64 - smp_tramp_start]
    jmp 0x08:(TRAMP + lm64 - smp_tramp_start)

bits 64
lm64:
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, [TRAMP + d_stack - smp_tramp_start]
    mov edi, [TRAMP + d_cpu - smp_tramp_start]
    mov rax, [TRAMP + d_entry - smp_tramp_start]
    call rax
.hang:
    cli
    hlt
    jmp .hang

align 8
gdt32:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10: data
gdtr32:
    dw 23
    dd TRAMP + gdt32 - smp_tramp_start

align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF           ; 0x08: 64-bit code (the kernel's own)
    dq 0x00CF92000000FFFF           ; 0x10: data
gdtr64:
    dw 23
    dq TRAMP + gdt64 - smp_tramp_start

align 8
smp_tramp_data:                     ; tramp_data_t in kernel/smp.c
d_cr3:   dd 0
d_cr4:   dd 0
d_cr0:   dd 0
d_efer:  dd 0
d_stack: dq 0
d_entry: dq 0
d_cpu:   dd 0
         dd 0
smp_tramp_end:
