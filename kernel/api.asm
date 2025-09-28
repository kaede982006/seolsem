%ifndef __API__
%define __API__

segment _TEXT class=CODE use16

global _clear_screen
global _print_message
global _wait_prompt

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
    pop  es
    popa
    ret

_wait_prompt:
    push bp
    mov  bp, sp
    pusha               ; AX,CX,DX,BX,SP,BP,SI,DI 저장 (세그먼트는 아님)
    push ds             ; 세그먼트 레지스터는 따로 보존
    push es

    ; --- 비디오 메모리 세그먼트 설정 ---
    mov  ax, 0xB800
    mov  es, ax

    ; --- 현재 줄 검사(+스크롤) ---
    mov  ax, [line]         ; DS:line (이제 DS가 맞음)
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  word [line], 24
.line_ok:

    ; --- 이번 줄 시작 DI 계산: di = line * 160 ---
    mov  ax, [line]
    mov  bx, 160
    mul  bx                 ; unsigned: DX:AX = AX * BX
    mov  [di_pos], ax
    mov  di, [di_pos]

    ; --- 메시지 포인터 가져오기 (호출자가 푸시한 오프셋) ---
    mov  si, [bp+4]         ; DS는 code/data와 동일하다고 가정(COM/TINY)

.print_loop:
    mov  cl, [si]
    cmp  cl, 0
    je   .start_poll              ; 널 종료면 입력 대기 단계로
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    jmp  .print_loop

.start_poll:
    mov  bx, di
    mov  dx, [di_pos]
    add  dx, 160

    mov  si, [bp+6]         ; buf
    mov  cx, si
    mov  byte [si], 0       ; ★ 버퍼 시작을 항상 NUL로
.poll:
.wait_key:
    mov  ah, 01h
	push si
    push di
    int  16h                ; AL=ASCII, AH=scancode
    pop  di
    pop  si

    jz   .wait_key

    xor  ah, ah
	push si
    push di
    int  16h                ; AL=ASCII, AH=scancode
    pop  di
    pop  si
	
    ; 1) 엔터면 줄 종료
    cmp  al, 13
    je   .end_line

    ; 2) 백스페이스 처리
    cmp  al, 8
    je   .backspace

    ; 3) 비인쇄 키(ASCII=0) 무시 (F1 등)
    test al, al
    jz   .poll

    ; 4) 줄 끝이면 더 못 씀 (경계 = di_pos+160)
    cmp  di, dx
    jae  .poll

    ; 5) 일반 문자 출력
    mov  [es:di], al
    mov  byte [es:di+1], 0x07
    add  di, 2

	; 일반 문자 입력
	mov  [si], al      ; 버퍼에 기록
	inc  si
	mov  byte [si], 0  ; 널 유지

    jmp  .poll

.backspace:
    ; di가 입력 시작 이전/같으면 지우지 않음
    cmp  di, bx
    jbe  .poll
    sub  di, 2
    mov  byte [es:di], 0x20      ; 화면에서 지울 땐 공백(0x20)이 자연스러움
    mov  byte [es:di+1], 0x07

	; 백스페이스
	cmp  si, cx        ; buf 시작 이전은 금지
	jbe  .poll
	dec  si
	mov  byte [si], 0
	jmp  .poll

.end_line:
    inc  word [line]
    ; (복원/ret는 그대로)
	pop es
	pop ds
	popa
	pop bp
	ret ; (호출자가 add sp,2 로 정리)
_print_message:
    push bp
    mov  bp, sp
    pusha
    push ds
    push es

    ; 비디오 세그먼트
    mov  ax, 0xB800
    mov  es, ax

    ; 줄 검사 및 스크롤
    mov  ax, [line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    ; 스크롤이 DS를 건드리므로, 다시 DS=CS 세팅

    mov  word [line], 24
.line_ok:

    ; di = line * 160
    mov  ax, [line]
    mov  bx, 160
    mul  bx
    mov  [di_pos], ax
    mov  di, [di_pos]

    ; 인자: [bp+4] = msg 오프셋 (COM/단일 세그먼트 가정)
    mov  si, [bp+4]

.print_loop:
    mov  cl, [si]
    test cl, cl
    jz   .end
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    jmp  .print_loop
.end:
    inc  word [line]

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

; 맨 아래에
segment _DATA class=DATA use16
line   dw 0
di_pos dw 0

%endif
