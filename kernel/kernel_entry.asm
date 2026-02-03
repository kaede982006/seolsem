bits 16
extern _kernel_main
extern _sima_heap_init
extern __bss_start
extern __bss_end
global _start

segment _TEXT class=CODE use16
_start:
    jmp short .real_start
    db "KENTRY_MARKER_v1", 0
.real_start:
    cli

    ; 디버그: 커널 진입 표시
    mov  ax, 0xB800
    mov  es, ax
    mov  word [es:0x0004], 0x074B   ; 'K'

    ; DS는 부트로더에서 DGROUP으로 설정됨
    mov  ax, ds
    mov  es, ax

    ; Small memory model uses near pointers for locals; keep SS=DS.
    mov  ss, ax
    mov  sp, 0xFFFE
    mov  bp, sp

    cld                     ; 문자열 방향 플래그 정방향

    ; BSS 0클리어
    mov  di, __bss_start
    mov  cx, __bss_end
    sub  cx, di
    xor  ax, ax
    rep  stosb

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
