; Banana OS 0.3 - boot.asm
; Multiboot2 header + kernel entry point

MB2_MAGIC   equ 0xE85250D6
MB2_ARCH    equ 0
MB2_HEADER_LEN equ (mb2_header_end - mb2_header_start)
MB2_CHECKSUM equ -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

section .multiboot2
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HEADER_LEN
    dd MB2_CHECKSUM

    ; Framebuffer request tag (type=5)
    dw 5                ; type
    dw 0                ; flags (optional)
    dd 24               ; size (must be multiple of 8)
    dd 800              ; width  (0 = any)
    dd 600              ; height (0 = any)
    dd 32               ; bpp    (0 = any)
    dd 0                ; padding to 8-byte boundary

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 16384      ; 16 KiB stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top      ; set up stack
    and esp, -16            ; defensive: guarantee 16-byte alignment
    ; The i386 SysV ABI (which GCC/Clang both assume when emitting SSE
    ; instructions such as movaps for struct copies/vectorized loops)
    ; requires esp % 16 == 0 immediately before every `call`. We're about
    ; to push two 4-byte args, so pad by 8 first to land back on a
    ; 16-byte boundary once those pushes are done.
    sub esp, 8
    push ebx                ; multiboot info pointer (mb2)
    push eax                ; multiboot magic (mb2)
    call kernel_main        ; jump to C kernel - esp is 16-aligned here
    cli
.hang:
    hlt
    jmp .hang
