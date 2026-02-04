%ifndef __API__
%define __API__

segment _TEXT class=CODE use16

%define INPUT_MAX 64

global _clear_screen
global _print_message
global _wait_prompt
global _read_key
global _set_cursor
global _write_char
global _sync_ds
global _enable_irq
global _disable_irq
global _dgroup_seg

_clear_screen:
    pusha
    push es
    mov  ax, 0xB800
    mov  es, ax
    mov  di, 0
    mov  ax, 0x0720         ; 공백 문자 + 0x07 속성
    mov  cx, 80*25
    cld
.cls_loop:
    stosw
    loop .cls_loop
    mov  word [ss:line], 0
    mov  word [ss:di_pos], 0
    xor  dx, dx
    call set_cursor_hw
    pop  es
    popa
    ret

_sync_ds:
    mov  ax, ss
    mov  ds, ax
    ret

_enable_irq:
    sti
    ret

_disable_irq:
    cli
    ret

_wait_prompt:
    push bp
    mov  bp, sp
    pusha               ; AX,CX,DX,BX,SP,BP,SI,DI 저장
    push ds
    push es

    ; DS may be clobbered by IRQ handlers; use SS (DGROUP) for data.
    mov  ax, ss
    mov  ds, ax

    ; --- 비디오 메모리 세그먼트 설정 ---
    mov  ax, 0xB800
    mov  es, ax
    cld

    ; --- 현재 줄 검사(+스크롤) ---
    mov  ax, [ss:line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, 0xB800
    mov  es, ax
    mov  word [ss:line], 24
.line_ok:

    ; --- 이번 줄 시작 DI 계산: di = line * 160 ---
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]

    ; --- 메시지 포인터 가져오기 ---
    mov  si, [bp+4]
.print_loop:
    mov  cl, [ss:si]
    cmp  cl, 0
    je   .start_poll
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    jmp  .print_loop

.start_poll:
    mov  bx, di
    mov  dx, [ss:di_pos]
    add  dx, 160
    mov  [ss:line_end], dx

    mov  si, [bp+6]         ; buf
    mov  cx, si
    mov  byte [ss:si], 0    ; 버퍼 시작을 항상 NUL로

.update_cursor:
    push bx
    mov  ax, di
    cmp  ax, [ss:line_end]
    jne  .cursor_calc
    sub  ax, 2
.cursor_calc:
    xor  dx, dx
    mov  bx, 160
    div  bx                 ; AX=ROW, DX=OFFSET
    mov  bx, dx             ; save remainder (offset)
    mov  dh, al             ; row
    mov  ax, bx
    shr  ax, 1
    mov  dl, al
    call set_cursor_hw
    pop  bx
.poll:
.wait_key:
    mov  ah, 01h
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    int  16h
    pop  es
    pop  ds
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx
    jz   .wait_key

    xor  ah, ah
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    int  16h
    pop  es
    pop  ds
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx

    cmp  al, 13
    je   .end_line

    cmp  al, 8
    je   .backspace

    test al, al
    jz   .poll

    mov  dx, [ss:line_end]
    cmp  di, dx
    jae  .poll

    mov  [es:di], al
    mov  byte [es:di+1], 0x07
    add  di, 2

    mov  [ss:si], al
    inc  si
    mov  byte [ss:si], 0
    jmp  .update_cursor

.backspace:
    cmp  di, bx
    jbe  .poll
    sub  di, 2
    mov  byte [es:di], 0x20
    mov  byte [es:di+1], 0x07

    cmp  si, cx
    jbe  .poll
    dec  si
    mov  byte [ss:si], 0
    jmp  .update_cursor

.end_line:
    inc  word [ss:line]
    pop  es
    pop  ds
    popa
    pop  bp
    ret

_read_key:
    push ds
    push es
    xor ah, ah
    int 16h
    pop es
    pop ds
    ret

_set_cursor:
    push bp
    mov  bp, sp
    mov  dh, [bp+4]
    mov  dl, [bp+6]
    call set_cursor_hw
    pop  bp
    ret

_write_char:
    push bp
    mov  bp, sp
    push ax
    push bx
    push dx
    push es

    mov  ax, 0xB800
    mov  es, ax

    mov  ax, [bp+4]         ; row
    mov  bx, 160
    mul  bx                 ; AX = row * 160
    mov  bx, [bp+6]         ; col
    shl  bx, 1
    add  ax, bx
    mov  di, ax

    mov  al, [bp+8]         ; ch
    mov  ah, [bp+10]        ; attr
    mov  [es:di], ax

    pop  es
    pop  dx
    pop  bx
    pop  ax
    pop  bp
    ret
_print_message:
    push bp
    mov  bp, sp
    pusha
    push ds
    push es
    mov  ax, ss
    mov  ds, ax

    ; 비디오 세그먼트
    mov  ax, 0xB800
    mov  es, ax
    mov  bl, 0

    ; 줄 검사 및 스크롤
    mov  ax, [ss:line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, 0xB800
    mov  es, ax
    mov  word [ss:line], 24
.line_ok:

    ; di = line * 160
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]

    ; 인자: [bp+4] = msg 오프셋
    mov  si, [bp+4]

.print_loop:
    mov  cl, [ss:si]
    test cl, cl
    jz   .end
    cmp  cl, 10
    je   .newline
    cmp  cl, 13
    je   .carriage_return
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    mov  bl, 0
    mov  ax, di
    sub  ax, [ss:di_pos]
    cmp  ax, 160
    jb   .print_loop
    jmp  .wrap
.newline:
    call .advance_line
    inc  si
    jmp  .print_loop
.carriage_return:
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]
    mov  bl, 1
    inc  si
    jmp  .print_loop
.wrap:
    call .advance_line
    jmp  .print_loop
.advance_line:
    inc  word [ss:line]
    mov  ax, [ss:line]
    cmp  ax, 25
    jb   .advance_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, 0xB800
    mov  es, ax
    mov  word [ss:line], 24
.advance_ok:
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]
    mov  bl, 1
    ret
.end:
    cmp  bl, 1
    je   .done
    inc  word [ss:line]
.done:
    pop  es
    pop  ds
    popa
    pop  bp
    ret

scroll_screen:
    pusha
    push ds
    push es

    mov  ax, 0xB800
    mov  es, ax
    mov  ds, ax
    cld                     ; 문자열 명령은 정방향으로!

    ; 위로 한 줄 스크롤
    mov  si, 160            ; src = 1번째 줄
    mov  di, 0              ; dst = 0번째 줄
    mov  cx, 24*80          ; 워드 개수(= 문자 수)
    rep  movsw

    ; 마지막 줄 클리어 (공백/회색)
    mov  di, 24*160
    mov  cx, 80
    mov  ax, 0x0720
    rep  stosw

    pop  es
    pop  ds
    popa
    ret

set_cursor_hw:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov  ah, 0x02            ; BIOS: set cursor position
    mov  bh, 0x00            ; page 0
    int  0x10

    pop  es
    pop  ds
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

bios_teletype:
    pusha
    push ds
    push es
    mov  ah, 0x0E
    mov  bh, 0x00
    mov  bl, 0x07
    int  0x10
    pop  es
    pop  ds
    popa
    ret

; DGROUP segment cached in code segment for reliable DS restore
global _dgroup_seg_cs
_dgroup_seg_cs dw 0

; 맨 아래에
segment _DATA class=DATA use16
global _api_data_anchor
_api_data_anchor db 0
_dgroup_seg dw 0
line        dw 0
di_pos      dw 0
line_end    dw 0

%endif
