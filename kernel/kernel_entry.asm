bits 16

; Watcom C 컴파일러는 C 함수 이름 앞에 밑줄(_)을 붙입니다.
; 따라서 C의 main() 함수를 호출하려면 _main을 찾아야 합니다.
extern _kernel_main

; 이 파일의 _start 심볼을 링커가 찾을 수 있도록 외부에 공개합니다.
global _start

; 코드 섹션을 정의합니다.
section .text

_start:
	
	mov ax, 0x8e00
	mov es, ax

    mov ax, 0x0000
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, 0xFFFE

	mov ax, cs
	mov ds, ax
	
	    sti

    ; C 커널의 main 함수 호출
    call _kernel_main
    ; 커널 실행이 끝나면 무한 루프로 시스템을 멈춤
    cli
.halt:
    hlt
    jmp .halt
