; Banana Boot - the master boot record (legacy BIOS, USB sticks and disks)
;
; The image is a "hybrid": the same bytes are a CD (El Torito) and a disk
; (this MBR at sector 0). The BIOS runs these 440 bytes at 0x7C00; they
; load the second stage - the file /boot/bios.bin of the ISO, whose place
; tools/mkimage.py writes into stage2_lba / stage2_sectors below - to
; 0x8000 and jump into it (0x8800: its first 2 KiB are the CD entry).

bits 16
org 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl

    ; the BIOS disk extensions (LBA reads) are needed
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc no_ext
    cmp bx, 0xAA55
    jne no_ext

    ; read the second stage: up to 64 sectors (32 KiB) at 0x0000:0x8000
    mov si, dap
    mov ax, [stage2_sectors]
    mov [dap_count], ax
    mov eax, [stage2_lba]
    mov [dap_lba], eax
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc read_err

    mov dl, [drive]
    xor si, si                      ; booted from a disk (not a CD)
    jmp 0x0000:0x8800

no_ext:
    mov si, msg_ext
    jmp fail
read_err:
    mov si, msg_read
fail:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp fail
.halt:
    hlt
    jmp .halt

msg_ext:  db "Banana Boot: this BIOS cannot read disks by LBA", 0
msg_read: db "Banana Boot: cannot read the loader", 0

align 4
dap:
    db 0x10, 0
dap_count:
    dw 0
    dw 0x8000, 0x0000               ; buffer offset, segment
dap_lba:
    dd 0, 0
drive: db 0

; patched by tools/mkimage.py
times 0x1B0 - ($ - $$) db 0
stage2_lba:     dd 0                ; 512-byte sectors
stage2_sectors: dw 0
times 440 - ($ - $$) db 0
; 440..511: disk signature and partition table (tools/mkimage.py), then 55 AA
