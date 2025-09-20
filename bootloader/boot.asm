[org 0x00]
[bits 16]

section .text

jmp 0x07c0:start

start:
    mov ax, 0x07c0
    mov ds, ax
    mov ax, 0xB800
    mov es, ax

    mov ax, 0x0000
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, 0xFFFE

    mov si, 0
.clear_screen:
    mov byte [es:si], 0
    mov byte [es:si+1], 0x07

    add si, 2
    cmp si, 80*25*2
    jl .clear_screen

    ; 메시지 출력을 위해 line 변수 초기화
    mov word [line], 0

	mov ax, 0x02
	mov bx, 0x01
	mov cx, 0x1000
	push ax
	push bx
	push cx
	
	call load_img
	add sp, 6

    jmp 0x1000:0x0000

%include "read.asm"

times 510 - ($-$$) db 0x00

dw 0xAA55
