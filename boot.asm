[org 0x00]
[bits 16]

section .text

jmp 0x07c0:start

total_sector_count: dw 4

start:
	mov ah, 0x02        ; AH=2: 커서 위치 설정 기능
    mov bh, 0           ; 페이지 번호 0
    mov dh, 25          ; DH=로우(Row): 화면 밖인 25번째 줄
    mov dl, 0           ; DL=컬럼(Column): 0번째 열
    int 0x10            ; 비디오 인터럽트 호출

    mov ah, 0x01        ; AH=1: 커서 모양 설정 기능
    mov ch, 0x20        ; CH의 5번 비트를 1로 설정하면 커서가 사라짐
    int 0x10            ; 비디오 인터럽트 호출

    mov ax, 0x07c0
    mov ds, ax
    mov ax, 0xB800
    mov es, ax

    mov ax, 0x0000
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, 0xFFFE

    mov si, 0
.clear_screen:
    mov byte [es:si], 0
    mov byte [es:si+1], 0x07

    add si, 2
    cmp si, 80*25*2
    jl .clear_screen

    ; 메시지 출력을 위해 line 변수 초기화
    mov word [line], 0

    push loading_message
    call print_message
    add sp, 2

reset_disk:
    mov ax, 0
    mov dl, 0
    int 0x13
    jc loading_error

    mov si, 0x1000
    ; <-- 수정된 부분: 루프 시작 전 ES를 목적지 메모리 세그먼트로 설정
    mov es, si
    mov bx, 0x0000
    mov di, word [total_sector_count]

read_data:
    cmp di, 0
    je read_end
    sub di, 0x1

    ; ES:BX가 데이터를 쓸 버퍼 주소이므로, 루프마다 ES를 바꿀 필요 없이 BX를 증가시키는 것이 더 효율적입니다.
    ; 여기서는 원본 코드의 로직을 존중하여 ES를 변경하는 방식을 유지하되, 버그를 수정합니다.
    mov ah, 0x02
    mov al, 0x1
    mov ch, byte [track_number]
    mov cl, byte [sector_number]
    mov dh, byte [head_number]
    mov dl, 0x00
    int 0x13
    jc loading_error

    ; 다음 섹터를 저장할 위치로 ES 세그먼트 주소 갱신
    ; 512바이트 = 0x200바이트. 세그먼트 주소는 16(0x10)배 되므로 0x200 / 0x10 = 0x20을 더함
    add si, 0x0020
    mov es, si

    mov al, byte [sector_number]
    add al, 0x1
    mov byte [sector_number], al
    cmp al, 19
    jl read_data

    xor byte [head_number], 0x1
    mov byte [sector_number], 0x01

    cmp byte [head_number], 0x00
    jne read_data

    add byte [track_number], 0x01
    jmp read_data

read_end:
    push complete_message
    call print_message
    add sp, 2

    jmp 0x1000:0x0000

loading_error:
    push error_message
    call print_message

    jmp $

print_message:
    push bp
    mov bp, sp

    push es
    push si
    push di
    push ax
    push cx
    push dx
    push bx ; <-- bx를 사용하므로 스택에 저장

    mov ax, 0xB800
    mov es, ax

    ; <-- 수정된 부분: di를 올바르게 계산
    mov ax, word [line]
    mov bx, 160             ; 80 * 2 = 160. 한 줄당 바이트 수
    mul bx                  ; ax = ax * bx (line * 160)
    mov di, ax              ; di를 계산된 시작 위치로 설정
    
    add word [line], 1

    mov si, word [bp+4]     ; 인자로 넘어온 메시지 주소
.loop_message:
    mov cl, byte [si]
    cmp cl, 0
    je .end_message

    mov byte [es:di], cl

    add si, 1
    add di, 2
    jmp .loop_message

.end_message:
    pop bx ; <-- 저장한 bx 복원
    pop dx
    pop cx
    pop ax
    pop di
    pop si
    pop es
    pop bp
    ret

    error_message: db "Error. System Halted.", 0
    loading_message: db "Loading The Image", 0
    complete_message: db "Loading Bootloader Completed.", 0

    sector_number: db 0x02
    head_number: db 0x00
    track_number: db 0x00
    line: dw 0x00

times 510 - ($-$$) db 0x00

dw 0xAA55
