[org 0x00]
[bits 16]

section .text

jmp 0x1000:start

start:
	mov ax, 0x8e00
	mov es, ax

    mov ax, 0x0000
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, 0xFFFE
	
	mov ax, cs
	mov ds, ax

    call clear_screen
	
	mov ax, 0x03
	mov bx, 0x01
	mov cx, 0x1080

	push ax
	push bx
	push cx

	call load_img
	add sp, 6

	jmp 0x1080:0x0000
	
system_halt:
    jmp $

%include "read.asm"

times 512-($-$$) db 0x00
