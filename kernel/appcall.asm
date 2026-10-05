; kernel/appcall.asm
;
; int  app_enter(int (*fn)(void*), void* arg, void* stack_top, uint32_t* saved_sp);
; void app_leave(uint32_t saved_sp, int code);
;
; 32-bit version of appcall64.asm: fn(arg) runs on the app's own stack;
; app_leave() (an app's exit()) returns from app_enter() with `code`.

global app_enter
global app_leave

section .text
app_enter:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp + 20]     ; fn
    mov ecx, [esp + 24]     ; arg
    mov edx, [esp + 28]     ; stack_top
    mov esi, [esp + 32]     ; saved_sp
    mov [esi], esp
    mov esp, edx
    sub esp, 12             ; keep the call 16-byte aligned
    push ecx
    call eax
    mov esp, [esi]          ; esi is callee-saved
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

app_leave:
    mov ecx, [esp + 4]      ; saved_sp
    mov eax, [esp + 8]      ; code
    mov esp, ecx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
