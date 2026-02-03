[org 0x00]
[bits 16]

section .text

%define KERNEL_LOAD_SEG 0x1080
; CODE_SEG must not overlap DGROUP's 64KB window (DS..DS+0xFFFF).
; DGROUP currently starts at KERNEL_LOAD_SEG, so pick a segment >= DS+0x1000.
%define CODE_SEG        0x3000

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
    mov [boot_drive], dl

    ; 디버그: stage2 진입 표시
    mov ax, 0xB800
    mov es, ax
    mov word [es:0x0002], 0x0732    ; '2'
	
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

    ; ---- 코드 세그먼트 복사 (데이터/BSS와 분리) ----
    ; source: KERNEL_LOAD_SEG:CODE_BASE
    ; dest:   CODE_SEG:0x0000
    mov ax, KERNEL_LOAD_SEG
    mov ds, ax
    mov ax, CODE_SEG
    mov es, ax
    xor di, di
    mov si, CODE_BASE
    mov cx, CODE_SIZE
    cld
    rep movsb

    ; 커널 DGROUP 세그먼트로 DS/ES 설정
    mov ax, KERNEL_LOAD_SEG
    add ax, DGROUP_DELTA
    mov ds, ax
    mov es, ax

	jmp CODE_SEG:ENTRY_REL
	
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
