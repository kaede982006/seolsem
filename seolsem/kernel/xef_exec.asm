[bits 16]

segment _TEXT class=CODE use16

global _xef_call_entry

; UINT16 xef_call_entry(UINT16 entry_off, const char *arg, UINT16 stack_top)
; cdecl stack:
;   [bp+4] = entry_off
;   [bp+6] = arg (near pointer)
;   [bp+8] = stack_top (new SP while running entry)
; Returns AX from called entry function.
_xef_call_entry:
    push bp
    mov  bp, sp

    ; Capture args on the original stack before switching (the new stack may
    ; overlap this call frame when stack_top is close to the current SP).
    mov  cx, [bp+4]             ; entry_off
    mov  ax, [bp+6]             ; arg (near pointer)
    mov  dx, [bp+8]             ; stack_top (0 = no switch)

    ; Switch to requested stack for program execution.
    test dx, dx
    jz   .no_stack_switch
    mov  sp, dx
.no_stack_switch:

    ; Save old stack pointer (after push bp). This is BP's current value.
    push bp

    ; Preserve callee-saved regs and flags for the C caller.
    pushf
    cld
    push bx
    push si
    push di
    push ds
    push es

    mov  bx, cx                 ; entry_off
    push ax                     ; arg
    call bx
    add  sp, 2                  ; pop arg (cdecl)

    ; Restore callee-saved regs/flags, then switch back to the original stack.
    pop  es
    pop  ds
    pop  di
    pop  si
    pop  bx
    popf

    pop  dx                     ; saved old SP
    mov  sp, dx

    pop  bp
    ret
