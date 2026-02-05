bits 16
extern _kernel_main
extern _sima_heap_init
extern __bss_start
extern __bss_end
extern _dgroup_seg_cs
global _start

segment _TEXT class=CODE use16
_start:
    jmp short .real_start
    db "KENTRY_MARKER_v1", 0
.real_start:
    cli

    ; DS는 부트로더에서 DGROUP으로 설정됨
    ; Small memory model uses near pointers for locals; keep SS=DS.
    mov  ax, ds
    mov  ss, ax
    mov  sp, 0xFFFE
    mov  bp, sp
    mov  es, ax                  ; BSS clear uses ES:DI

    ; Save DGROUP segment for later DS recovery (code segment storage)
    mov  [cs:_dgroup_seg_cs], ax

    cld                     ; 문자열 방향 플래그 정방향

    ; BSS 0클리어
    mov  di, __bss_start
    mov  cx, __bss_end
    sub  cx, di
    xor  ax, ax
    rep  stosb

    ; 내부 아레나 힙 초기화 (freestanding)
    call _sima_heap_init

    ; 인터럽트는 커널 메인 루프에서 입력 처리 직전에만 켬.
    ; (BIOS IRQ 핸들러가 DS를 보존하지 않는 경우가 있어, 초기화 중에는 끄는 편이 안전)

    ; C 커널 진입
    call _kernel_main

.halt:
    cli
    hlt
    jmp .halt
