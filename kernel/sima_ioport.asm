%ifndef __SIMA_IOPORT__
%define __SIMA_IOPORT__
[bits 16]

global _io_inb
global _io_outb
global _io_inw
global _io_outw
global _io_insw
global _io_outsw

; UINT8 io_inb(UINT16 port)
_io_inb:
    push bp
    mov  bp, sp
    mov  dx, [bp+4]
    in   al, dx
    xor  ah, ah
    pop  bp
    ret

; void io_outb(UINT16 port, UINT8 value)
_io_outb:
    push bp
    mov  bp, sp
    mov  dx, [bp+4]
    mov  al, [bp+6]
    out  dx, al
    pop  bp
    ret

; UINT16 io_inw(UINT16 port)
_io_inw:
    push bp
    mov  bp, sp
    mov  dx, [bp+4]
    in   ax, dx
    pop  bp
    ret

; void io_outw(UINT16 port, UINT16 value)
_io_outw:
    push bp
    mov  bp, sp
    mov  dx, [bp+4]
    mov  ax, [bp+6]
    out  dx, ax
    pop  bp
    ret

; void io_insw(UINT16 port, void *buffer, UINT16 count)
; count = words
_io_insw:
    push bp
    mov  bp, sp
    push es
    push di

    mov  dx, [bp+4]
    mov  di, [bp+6]
    mov  cx, [bp+8]
    mov  ax, ds
    mov  es, ax
    rep  insw

    pop  di
    pop  es
    pop  bp
    ret

; void io_outsw(UINT16 port, const void *buffer, UINT16 count)
_io_outsw:
    push bp
    mov  bp, sp
    push si

    mov  dx, [bp+4]
    mov  si, [bp+6]
    mov  cx, [bp+8]
    rep  outsw

    pop  si
    pop  bp
    ret

%endif
