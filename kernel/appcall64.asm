; kernel/appcall64.asm
;
; int  app_enter(int (*fn)(void*), void* arg, void* stack_top, uintptr_t* saved_sp);
; void app_leave(uintptr_t saved_sp, int code);
;
; app_enter() saves the callee-saved registers on the caller's stack,
; stores that stack pointer through saved_sp, and calls fn(arg) on the
; app's own stack. If fn returns, app_enter returns its result. An app
; that calls exit() lands in app_leave(), which goes straight back to the
; saved stack - app_enter() then returns `code` - wherever on its own
; stack the app was.

bits 64
global app_enter
global app_leave

section .text
app_enter:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rcx], rsp
    mov r12, rcx            ; callee-saved: survives the call
    mov rsp, rdx            ; the app's stack (16-byte aligned)
    mov rax, rdi
    mov rdi, rsi
    call rax
    mov rsp, [r12]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

app_leave:
    mov rsp, rdi
    mov eax, esi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
