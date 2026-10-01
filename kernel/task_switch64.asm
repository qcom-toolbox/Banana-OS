; kernel/task_switch64.asm
;
; void task_switch(uintptr_t** old_sp_store, uintptr_t* new_sp);
;
; x86_64 version of task_switch.asm: saves the callee-saved registers of
; the System V AMD64 ABI (rbp, rbx, r12-r15) on the outgoing task's stack,
; stores rsp through old_sp_store (rdi), switches to new_sp (rsi) and
; restores the incoming task's registers - returning into it, or for a new
; task into task_trampoline via the frame task_create() built.

bits 64
global task_switch

section .text
task_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
