%ifndef __API__
%define __API__

global _clear_screen
global _print_message
global _print_number

_clear_screen:
    pusha
    mov ax, 0xB800
    mov es, ax
    mov di, 0
.cls_loop:
    mov word [es:di], 0x0700
    add di, 2
    cmp di, 80*25*2
    jl .cls_loop
    popa
    ret

_print_message:
	push bp
    mov bp, sp
    pusha

mov ax, word [line]
    cmp ax, 25
    jne .print_now
    call scroll_screen

    mov ax, cs
    mov ds, ax

    mov word [line], 24

.print_now:
    mov ax, 0xB800
    mov es, ax
	
	mov ax, word [line]
    mov bx, 160
    mul bx
    mov word [di_pos], ax ; 이번 줄에서 사용할 시작 di 위치를 저장

    mov di, word [di_pos]  ; 저장된 di 위치에서 출력 시작
    mov si, [bp+4]         ; 스택에서 메시지 주소 가져오기

.loop:
    mov cl, byte [si]
    cmp cl, 0
    je .end
    mov byte [es:di], cl
    mov byte [es:di+1], 0x07
    add si, 1
    add di, 2
    jmp .loop
.end:
	inc word [line]
    popa
    pop bp
    ret

scroll_screen:
    pusha
    mov ax, 0xB800
    mov es, ax
    mov ds, ax
    mov si, 160      ; 소스: 1번 줄 시작 주소
    mov di, 0        ; 목적지: 0번 줄 시작 주소
    mov cx, 24 * 80  ; 복사할 워드(2바이트) 개수
    rep movsw        ; 24줄의 내용을 위로 복사

    mov di, 24 * 160 ; 목적지: 마지막 24번 줄 시작 주소
    mov cx, 80       ; 채울 워드 개수 (80컬럼)
    mov ax, 0x0720   ; <-- 수정: ax를 공백 문자(0x20)와 속성(0x07)으로 설정
    rep stosw        ; 마지막 줄을 공백으로 채움
    
    popa
    ret

line: dw 0
di_pos: dw 0               ; 현재 출력 위치(di)를 저장할 변수

%endif
