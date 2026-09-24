; kernel/isr.asm
;
; Stub entry points for the 32 CPU exception vectors (0-31). Each pushes a
; uniform (error_code, int_no) pair - vectors that don't have a CPU-pushed
; error code get a dummy 0 - then falls into a common handler that saves
; the full register state and calls the C-side isr_handler(registers_t*).

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common_stub
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs 0-15, remapped by idt.c's PIC setup to vectors 32-47.
%macro IRQ 1
global irq%1
irq%1:
    push dword 0
    push dword (32 + %1)
    jmp irq_common_stub
%endmacro

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

extern isr_handler
extern irq_handler

section .text
isr_common_stub:
    pushad                  ; edi,esi,ebp,esp,ebx,edx,ecx,eax
    push ds
    push es
    push fs
    push gs

    ; No segment reload here: this kernel never runs anything outside
    ; ring 0, so ds/es/fs/gs are already whatever valid flat kernel data
    ; segment the CPU was already using when the fault happened - forcing
    ; a hardcoded selector value here would just be guessing at GRUB's
    ; GDT layout, the same mistake idt_init() avoids for the code segment.

    push esp                 ; registers_t* argument
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8                ; drop error_code + int_no
    iret

; Same frame layout as isr_common_stub, so irq_handler() gets a
; registers_t* too. The direction flag is cleared for the C code (the
; interrupted code may have been in the middle of a backwards memmove).
irq_common_stub:
    pushad
    push ds
    push es
    push fs
    push gs
    cld

    push esp
    call irq_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8
    iret
