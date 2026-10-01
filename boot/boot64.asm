; Banana OS 0.5 - boot64.asm
; Multiboot2 header + entry point of the x86_64 kernel.
;
; GRUB starts a Multiboot2 kernel in 32-bit protected mode with paging off,
; on BIOS and UEFI machines alike. This code checks for a 64-bit CPU,
; identity-maps the first 4 GiB (RAM, PCI devices and the framebuffer all
; live there) with 2 MiB pages, enables PAE + long mode + paging, loads a
; 64-bit GDT and calls kernel_main(magic, info) in 64-bit mode.

MB2_MAGIC   equ 0xE85250D6
MB2_ARCH    equ 0                   ; i386 entry (protected mode) - also for 64-bit
MB2_HEADER_LEN equ (mb2_header_end - mb2_header_start)
MB2_CHECKSUM equ -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

section .multiboot2
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HEADER_LEN
    dd MB2_CHECKSUM

    ; Framebuffer request tag (type=5): on UEFI this is the GOP mode
    dw 5
    dw 0
    dd 24
    dd 800
    dd 600
    dd 32
    dd 0

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .bss
align 4096
pml4:   resb 4096
pdpt:   resb 4096
pd:     resb 4096 * 4               ; 4 x 512 entries x 2 MiB = 4 GiB
align 16
stack_bottom:
    resb 32768
stack_top:

section .rodata
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF           ; 0x08: 64-bit code (L=1), ring 0
    dq 0x00CF92000000FFFF           ; 0x10: data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

no_lm_msg: db "Banana OS (64-bit) needs a 64-bit CPU - pick the 32-bit entry in the boot menu.", 0

section .text
bits 32
global _start
extern kernel_main

_start:
    cli
    mov edi, eax                    ; Multiboot2 magic
    mov esi, ebx                    ; Multiboot2 information
    mov esp, stack_top

    ; long mode available? (CPUID 0x80000001, EDX bit 29)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz no_long_mode

    ; PML4[0] -> PDPT, PDPT[0..3] -> the four page directories
    mov eax, pdpt
    or eax, 3                       ; present + writable
    mov [pml4], eax
    xor ecx, ecx
.pdpt_loop:
    mov eax, ecx
    shl eax, 12
    add eax, pd
    or eax, 3
    mov [pdpt + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt_loop

    ; 2048 x 2 MiB pages: physical address == virtual address
    xor ecx, ecx
.pd_loop:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                    ; present + writable + 2 MiB page
    mov [pd + ecx * 8], eax
    mov dword [pd + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jne .pd_loop

    mov eax, pml4
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 1 << 8                  ; long mode enable
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | 1           ; paging + protection
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:long_mode

no_long_mode:
    ; say so on the text screen (and the serial port), then stop
    mov ebx, no_lm_msg
    mov edx, 0xB8000
.msg:
    mov al, [ebx]
    test al, al
    jz .halt
    mov ah, 0x4F
    mov [edx], ax
    push edx
    mov dx, 0x3F8
    out dx, al
    pop edx
    inc ebx
    add edx, 2
    jmp .msg
.halt:
    hlt
    jmp .halt

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, stack_top              ; 16-byte aligned; the call below makes it 8 mod 16
    mov edi, edi                    ; zero-extend: the upper halves are undefined
    mov esi, esi                    ; after the switch to 64-bit mode
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
