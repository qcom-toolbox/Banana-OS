; Banana Boot - the BIOS loader's assembly: its two entries, the switch to
; protected mode, and bios_int(), which lets the C part (bios.c, 32-bit
; protected mode) call the BIOS by going back to real mode for the call.
;
; The file /boot/bios.bin is loaded at 0x8000 (its first 2 KiB, the CD
; entry, are linked for 0x7C00 - where El Torito puts them - and the rest
; for 0x8800). It must stay below 0x10000: real mode reaches it with
; segment 0.
;
; Memory below 1 MiB:  0x0600-0x4FFF real-mode stack, 0x5000-0x7BFF stack,
; 0x8000-0xFFFF this, 0x10000 BIOS call buffers, 0x20000 disk buffer,
; 0x30000 the Multiboot2 information.

; ── the CD entry: El Torito ("no emulation") loads 2 KiB of this file at
;    0x7C00; mkisofs's -boot-info-table writes where the file is into
;    bytes 8..63. It reads the whole file to 0x8000 and jumps on. ──────
section .cdstub
bits 16
global cd_start
cd_start:
    jmp short cd_main
    nop
    times 8 - ($ - cd_start) db 0
bi_pvd:   dd 0
bi_file:  dd 0                      ; this file's first 2048-byte block
bi_len:   dd 0                      ; its size in bytes
bi_csum:  dd 0
          times 40 db 0
cd_main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [cd_drive], dl
    mov eax, [bi_len]
    add eax, 2047
    shr eax, 11
    mov [cd_dap + 2], ax
    mov eax, [bi_file]
    mov [cd_dap + 8], eax
    mov si, cd_dap
    mov ah, 0x42
    mov dl, [cd_drive]
    int 0x13
    jc .fail
    mov dl, [cd_drive]
    mov si, 1                       ; booted from a CD
    jmp 0x0000:0x8800
.fail:
    mov si, cd_msg
.p: lodsb
    test al, al
    jz .h
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .p
.h: hlt
    jmp .h
cd_msg:   db "Banana Boot: cannot read the loader from the CD", 0
align 4
cd_dap:   db 0x10, 0
          dw 0                      ; 2048-byte blocks
          dw 0x8000, 0x0000
          dd 0, 0
cd_drive: db 0
    times 2048 - ($ - cd_start) db 0

; ── the common entry at 0x8800 (from the MBR or the CD entry):
;    DL = the BIOS drive, SI = 1 for a CD ───────────────────────────────
section .entry16
bits 16
global entry16
extern __bss_start, __bss_end
entry16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    mov [boot_drive], dl
    mov [boot_cd], si
    ; the A20 line: the BIOS, then the "fast A20" port (bios.c checks it)
    sti
    mov ax, 0x2401
    int 0x15
    cli
    in al, 0x92
    test al, 2
    jnz .a20
    or al, 2
    and al, 0xFE
    out 0x92, al
.a20:
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    movzx eax, word [boot_cd]
    push eax
    movzx eax, byte [boot_drive]
    push eax
    extern loader_main
    call loader_main
.halt:
    hlt
    jmp .halt

; ── void bios_int(int n, rm_regs_t* r): software interrupt n in real mode
;    with the registers in *r, which gets them back afterwards ───────────
global bios_int
bios_int:
    pushad
    mov eax, [esp + 36]
    mov [int_no], al
    mov esi, [esp + 40]
    mov [saved_rptr], esi
    mov edi, rm_regs
    mov ecx, 9
    rep movsd
    mov [saved_esp], esp
    jmp 0x18:.pm16
bits 16
.pm16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:.rm
.rm:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov sp, 0x5000
    lidt [rm_idtr]
    mov ax, [rm_regs + 30]
    mov es, ax
    mov eax, [rm_regs + 0]
    mov ebx, [rm_regs + 4]
    mov ecx, [rm_regs + 8]
    mov edx, [rm_regs + 12]
    mov esi, [rm_regs + 16]
    mov edi, [rm_regs + 20]
    mov ebp, [rm_regs + 24]
    push word [rm_regs + 28]
    pop ds
    sti
    db 0xCD                         ; int imm8
int_no: db 0
    pushfd                          ; the flags the BIOS returned (CF, ZF), before anything changes them
    cli
    push ds
    push eax
    xor ax, ax
    mov ds, ax
    pop eax
    mov [rm_regs + 0], eax
    mov [rm_regs + 4], ebx
    mov [rm_regs + 8], ecx
    mov [rm_regs + 12], edx
    mov [rm_regs + 16], esi
    mov [rm_regs + 20], edi
    mov [rm_regs + 24], ebp
    pop ax
    mov [rm_regs + 28], ax
    mov [rm_regs + 30], es
    pop eax
    mov [rm_regs + 32], eax
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:.back
bits 32
.back:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [saved_esp]
    mov esi, rm_regs
    mov edi, [saved_rptr]
    mov ecx, 9
    rep movsd
    popad
    ret

; ── void boot_kernel(u32 entry, u32 info): the Multiboot2 hand-over ─────
global boot_kernel
boot_kernel:
    cli
    mov ecx, [esp + 4]
    mov ebx, [esp + 8]
    mov eax, 0x36D76289
    jmp ecx

section .data
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10 32-bit data
    dq 0x00009A000000FFFF           ; 0x18 16-bit code (to go back to real mode)
    dq 0x000092000000FFFF           ; 0x20 16-bit data
gdtr:
    dw 5 * 8 - 1
    dd gdt
rm_idtr:
    dw 0x3FF
    dd 0
global boot_drive, boot_cd
boot_drive: db 0
align 2
boot_cd:    dw 0
align 4
rm_regs:    times 9 dd 0            ; eax ebx ecx edx esi edi ebp, ds:es, eflags
saved_rptr: dd 0
saved_esp:  dd 0
