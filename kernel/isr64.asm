; kernel/isr64.asm - interrupt entry points of the x86_64 kernel
;
; Same scheme as isr.asm: each stub pushes (error_code, int_no) - a dummy
; 0 error code where the CPU pushes none - and jumps to a common handler
; that saves the general registers and calls the C side with a
; registers_t* (see kernel/idt.c). The kernel is built without SSE, so
; there is no FPU/SSE state to save. In long mode the CPU aligns the
; stack to 16 bytes before pushing its frame, so after the 2 + 15 pushes
; here rsp is 16-byte aligned again for the call.

bits 64

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
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

%macro IRQ 1
global irq%1
irq%1:
    push qword 0
    push qword (32 + %1)
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

; push order chosen so that registers_t (idt.c) reads r15 first
%macro PUSH_ALL 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_ALL 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

extern isr_handler
extern irq_handler

section .text
isr_common_stub:
    PUSH_ALL
    cld
    mov rdi, rsp                    ; registers_t*
    call isr_handler
    POP_ALL
    add rsp, 16                     ; int_no + error code
    iretq

irq_common_stub:
    PUSH_ALL
    cld
    mov rdi, rsp
    call irq_handler
    POP_ALL
    add rsp, 16
    iretq

; ── local-APIC interrupts (kernel/smp.c) ──────────────────────────────
; ipi_stub: SMP_VEC_KICK, another core asking this one to look at its
; work; stray_stub: any other vector that should not arrive (the I/O APIC
; is masked) - acknowledged and ignored; spurious_stub: the local APIC's
; spurious vector, which must not be acknowledged at all.
global ipi_stub
global stray_stub
global spurious_stub
extern ipi_handler

ipi_stub:
    push qword 0
    push qword 0xF0
    jmp ipi_common_stub

stray_stub:
    push qword 0
    push qword 0x30
    jmp ipi_common_stub

spurious_stub:
    iretq

ipi_common_stub:
    PUSH_ALL
    cld
    mov rdi, rsp
    call ipi_handler
    POP_ALL
    add rsp, 16
    iretq
