bits 16
extern _kernel_main
extern _sima_heap_init
global _start

section .text
_start:
    cli

    ; 세그먼트 전부 커널 세그먼트(CS)로 통일
    mov  ax, cs
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0xFFFE
    mov  bp, sp

    cld                     ; 문자열 방향 플래그 정방향

    ; (선택) BSS 0클리어 루틴이 있으면 여기서 호출

    ; 내부 아레나 힙 초기화 (freestanding)
    call _sima_heap_init

    ; 실모드 BIOS 키보드 입력은 IRQ1이 필요하므로 인터럽트 활성화
    sti

    ; C 커널 진입
    call _kernel_main

.halt:
    cli
    hlt
    jmp .halt

