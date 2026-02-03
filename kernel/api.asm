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

_clear_screen:
    pusha
    push es
    mov  ax, 0xB800
    mov  es, ax
    mov  di, 0
    mov  ax, 0x0700         ; 0x00 문자 + 0x07 속성
    mov  cx, 80*25
    cld
.cls_loop:
    stosw
    loop .cls_loop
    mov  word [ss:line], 0
    mov  word [ss:di_pos], 0
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
    pusha               ; AX,CX,DX,BX,SP,BP,SI,DI 저장 (세그먼트는 아님)
    push es
    mov  ax, ss         ; SS=DGROUP 전제, DS를 확정
    mov  ds, ax

    ; --- 비디오 메모리 세그먼트 설정 ---
    mov  ax, 0xB800
    mov  es, ax

    ; --- 현재 줄 검사(+스크롤) ---
    mov  ax, [ss:line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  word [ss:line], 24
.line_ok:

    ; --- 이번 줄 시작 DI 계산: di = line * 160 ---
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx                 ; unsigned: DX:AX = AX * BX
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]

    ; --- 메시지 포인터 가져오기 (호출자가 푸시한 오프셋) ---
    mov  si, [bp+4]

.print_loop:
    mov  cl, [ss:si]
    cmp  cl, 0
    je   .start_poll              ; 널 종료면 입력 대기 단계로
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    jmp  .print_loop

.start_poll:
    mov  bx, di
    mov  ax, [ss:line]
    mov  [ss:line_start], ax
    mov  dx, [ss:di_pos]
    add  dx, 160
    mov  [ss:line_end], dx

    mov  si, [bp+6]         ; buf
    mov  cx, si
    mov  byte [ss:si], 0    ; ★ 버퍼 시작을 항상 NUL로
.update_cursor:
    mov  ax, di
    sub  ax, [ss:di_pos]
    shr  ax, 1
    mov  dl, al
    mov  dh, [ss:line]
    call set_cursor_hw
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
    int  16h                ; AL=ASCII, AH=scancode
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
    int  16h                ; AL=ASCII, AH=scancode
    pop  es
    pop  ds
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx
	
    ; 1) 엔터면 줄 종료
    cmp  al, 13
    je   .end_line

    ; 2) 백스페이스 처리
    cmp  al, 8
    je   .backspace

    ; 3) 비인쇄 키(ASCII=0) 무시 (F1 등)
    test al, al
    jz   .poll

    ; 4) 최대 입력 길이 제한
    mov  dl, al
    mov  ax, si
    sub  ax, cx
    cmp  ax, INPUT_MAX
    jae  .poll

    ; 5) 일반 문자 출력
    mov  [es:di], dl
    mov  byte [es:di+1], 0x07
    add  di, 2

	; 일반 문자 입력
    mov  [ss:si], dl   ; 버퍼에 기록
    inc  si
    mov  byte [ss:si], 0  ; 널 유지

    ; 줄 끝에 도달하면 다음 줄로 이동 (입력 래핑)
    cmp  di, [ss:line_end]
    jb   .update_cursor
    inc  word [ss:line]
    cmp  word [ss:line], 25
    jb   .wrap_set_pos
    call scroll_screen
    mov  word [ss:line], 24
.wrap_set_pos:
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, ax
    mov  dx, ax
    add  dx, 160
    mov  [ss:line_end], dx
    jmp  .update_cursor

.backspace:
    ; di가 입력 시작 이전/같으면 지우지 않음
    cmp  di, bx
    jne  .backspace_ok
    mov  ax, [line]
    cmp  ax, [line_start]
    jbe  .poll
.backspace_ok:
    cmp  di, [ss:di_pos]
    jne  .backspace_same_line
    dec  word [ss:line]
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, ax
    mov  dx, ax
    add  dx, 160
    mov  [ss:line_end], dx
    add  di, 158
    jmp  .backspace_clear
.backspace_same_line:
    sub  di, 2
.backspace_clear:
    mov  byte [es:di], 0x20      ; 화면에서 지울 땐 공백(0x20)이 자연스러움
    mov  byte [es:di+1], 0x07

	; 백스페이스
    cmp  si, cx        ; buf 시작 이전은 금지
    jbe  .poll
    dec  si
    mov  byte [ss:si], 0
    jmp  .update_cursor

.end_line:
    inc  word [ss:line]
    cmp  word [ss:line], 25
    jb   .set_prompt_cursor
    call scroll_screen
    mov  word [ss:line], 24
.set_prompt_cursor:
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, ax
    mov  dx, ax
    add  dx, 160
    mov  [ss:line_end], dx
    mov  ax, [ss:line]
    mov  dh, al
    xor  dl, dl
    call set_cursor_hw
    ; (복원/ret는 그대로)
    mov  ax, ss
    mov  ds, ax
	pop es
	popa
	pop bp
    ret ; (호출자가 add sp,2 로 정리)

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
    push es
    mov  ax, ss         ; SS=DGROUP 전제, DS를 확정
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
    ; 스크롤이 DS를 건드리므로, 다시 DS=CS 세팅

    mov  word [ss:line], 24
.line_ok:

    ; di = line * 160
    mov  ax, [ss:line]
    mov  bx, 160
    mul  bx
    mov  [ss:di_pos], ax
    mov  di, [ss:di_pos]

    ; 인자: [bp+4] = msg 오프셋 (COM/단일 세그먼트 가정)
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
    sub  ax, [di_pos]
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
    mov  ax, ss
    mov  ds, ax

    pop  es
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
    pop  bx
    pop  ax
    ret

; 맨 아래에
segment _DATA class=DATA use16
global _api_data_anchor
_api_data_anchor db 0
line   dw 0
di_pos dw 0
line_end dw 0
line_start dw 0

%endif
