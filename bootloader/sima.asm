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
	
	call enable_a20
	jc a20_support_error
load_kernel:
	mov ax, 0x04
	mov bx, KRN_SECT
	mov cx, 0x1080
	push ax
	push bx
	push cx

	call load_img
	add sp, 6

	jmp 0x1080:0x0000
	
system_halt:
    jmp $
a20_support_error:
	push a20_err_message
	call print_message
	jmp $
	
%include "read.asm"
%include "a20.asm"

a20_err_message: db "Turning on A20 failed: System Halted"
times 1024-($-$$) db 0x00
