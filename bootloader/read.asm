%ifndef __READ__
%define __READ__

load_img:
	push bp
	mov bp, sp

	mov ax, word [ebp+8]
	mov byte [sector_number], al
	mov bx, word [ebp+6]
	mov word [total_sector_count], bx
reset_disk:
    mov ax, 0
    mov dl, 0
    int 0x13
    jc loading_error

    mov ah, 0x08
    mov dl, 0x80
    int 0x13
    jc loading_error
    
	inc dh

    mov al, cl
    and al, 0x3f  ; 하위 6비트만 남겨서 순수한 섹터 수를 얻음
    mov byte [disk_sector_per_track], al

    mov si, word [ebp+4]
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
    mov dl, 0x80
    int 0x13
    jc loading_error

    ; 다음 섹터를 저장할 위치로 ES 세그먼트 주소 갱신
    ; 512바이트 = 0x200바이트. 세그먼트 주소는 16(0x10)배 되므로 0x200 / 0x10 = 0x20을 더함
    add si, 0x0020
    mov es, si

    mov al, byte [sector_number]
    add al, 0x1
    mov byte [sector_number], al

    cmp al, byte [disk_sector_per_track]
    jle read_data

    xor byte [head_number], 0x1
    mov byte [sector_number], 0x01

    cmp byte [head_number], 0x00
    jne read_data

    add byte [track_number], 0x01
    jmp read_data

read_end:

	pop bp
	ret

loading_error:
    push disk_err_message
    call print_message

    jmp $

disk_err_message: db "Disk Read Error: System Halted", 0
sector_number: db 0x02
head_number: db 0x00
track_number: db 0x00
total_sector_count: dw 0x00
disk_sector_per_track: dw 0x00

extern print_message
extern line

%include "api.asm"

%endif
