%ifndef __SIMA_HEAP__
%define __SIMA_HEAP__
[bits 16]

; ===== Public symbols (Watcom C cdecl 호환) =====
global _sima_heap_init,  sima_heap_init
global _sima_heap_set,   sima_heap_set
global _sima_malloc,     sima_malloc
global _sima_free_all,   sima_free_all
global _sima_heap_remaining, sima_heap_remaining

; ----- 설정 -----
%ifndef SIMA_HEAP_SIZE
%define SIMA_HEAP_SIZE  (16*1024)    ; 필요시 Makefile에서 -DSIMA_HEAP_SIZE=… 로 조절
%endif

; ----- 상태 -----
section .bss
align 2
sima_heap_buf:    resb  SIMA_HEAP_SIZE    ; 내부 정적 힙 버퍼

section .data
align 2
heap_base_seg:    dw 0
heap_base_off:    dw 0
heap_top_off:     dw 0                    ; 사용 중 오프셋
heap_cap:         dw SIMA_HEAP_SIZE

section .text

; 내부: 2바이트 정렬 보정
align2:
    ; in:  AX = size
    inc ax
    and ax, 0xFFFE
    ret

; void sima_heap_init(void)
; 기본값: 내부 버퍼를 힙으로 사용
_sima_heap_init:
sima_heap_init:
    push bp
    mov  bp, sp
    push ax
    push ds

    ; 커널 진입부에서 DS=CS 가 맞춰져 있다는 전제(아래 "통합 팁" 참고)
    mov  ax, cs
    mov  ds, ax

    mov  ax, sima_heap_buf
    mov  [heap_base_off], ax
    mov  ax, ds
    mov  [heap_base_seg], ax

    mov  word [heap_top_off], 0
    mov  ax, SIMA_HEAP_SIZE
    mov  [heap_cap], ax

    pop  ds
    pop  ax
    pop  bp
    ret

; void sima_heap_set(void far* base, UINT16 size)
; cdecl 스택 레이아웃: [bp+4]=size, [bp+6]=base.off, [bp+8]=base.seg
_sima_heap_set:
sima_heap_set:
    push bp
    mov  bp, sp

    mov  ax, [bp+6]               ; offset
    mov  [heap_base_off], ax
    mov  ax, [bp+8]               ; segment
    mov  [heap_base_seg], ax
    mov  ax, [bp+4]               ; size
    mov  [heap_cap], ax
    mov  word [heap_top_off], 0

    pop  bp
    ret

; void far* sima_malloc(UINT16 size)
; 반환: DX:AX = far pointer (실패 시 0:0)
_sima_malloc:
sima_malloc:
    push bp
    mov  bp, sp
    push bx
    push si

    mov  ax, [bp+4]               ; size
    call align2                   ; AX = need (2-byte aligned)
    mov  si, ax                   ; need -> SI

    mov  ax, [heap_cap]
    sub  ax, [heap_top_off]
    cmp  ax, si
    jb   .fail                    ; 남은 용량 부족

    ; p = base_off + top
    mov  dx, [heap_base_seg]
    mov  ax, [heap_base_off]
    mov  bx, [heap_top_off]
    add  ax, bx                   ; AX = offset 반환값

    ; top += need
    add  bx, si
    mov  [heap_top_off], bx

    jmp  .done

.fail:
    xor  dx, dx
    xor  ax, ax
.done:
    pop  si
    pop  bx
    pop  bp
    ret

; void sima_free_all(void)  -- 개별 free 없음, 전체 리셋
_sima_free_all:
sima_free_all:
    push bp
    mov  bp, sp
    mov  word [heap_top_off], 0
    pop  bp
    ret

; UINT16 sima_heap_remaining(void)
_sima_heap_remaining:
sima_heap_remaining:
    push bp
    mov  bp, sp
    mov  ax, [heap_cap]
    sub  ax, [heap_top_off]
    pop  bp
    ret

%endif

