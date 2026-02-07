[bits 16]

; Public symbol for Watcom C compiler intrinsic
; __U4M: Unsigned 4-byte Multiplication (UINT32 * UINT32 -> UINT32)
; Inputs: DX:AX (op1), CX:BX (op2)
; Output: DX:AX (result)
; Clobbers: Can clobber registers as per Watcom internal calling convention

global __U4M

segment _TEXT class=CODE use16

; Re-implementation from scratch calling convention safe
; Watcom: DX:AX * CX:BX -> DX:AX
__U4M:
    push bx
    push cx
    push si
    
    ; SI = H1 (DX)
    mov si, dx
    
    ; 1. L1 * H2 (AX * CX) -> contribute to High result
    push ax
    mul cx
    mov cx, ax ; CX = partial High
    pop ax
    
    ; 2. H1 * L2 (DX * BX) -> contribute to High result
    push ax
    mov ax, si
    mul bx
    add cx, ax ; CX += partial High
    pop ax
    
    ; 3. L1 * L2 (AX * BX) -> Low result + carry to High
    mul bx
    add dx, cx ; DX (high of L1*L2) + partials
    
    ; Result in DX:AX
    
    pop si
    pop cx
    pop bx
    ret
