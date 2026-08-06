; kernel/task_switch.asm
;
; void task_switch(uint32_t** old_sp_store, uint32_t* new_sp);
;
; Cooperative context switch. Pushes the outgoing task's callee-saved
; registers onto its own stack, stashes the resulting esp through
; old_sp_store, then loads esp for the incoming task and pops its
; callee-saved registers back off before returning - which returns into
; whatever instruction the incoming task was switched away from (or, for a
; brand-new task, straight into task_trampoline via the fake frame that
; task_create() built).

global task_switch

section .text
task_switch:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi

    mov eax, [ebp+8]      ; old_sp_store
    mov [eax], esp

    mov eax, [ebp+12]     ; new_sp
    mov esp, eax

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
