%ifndef __READ__
%define __READ__

; load_img(start_lba, count_sectors, dest_seg)
;
; Stack (cdecl-like, pushed right-to-left):
;   [bp+4]  dest_seg
;   [bp+6]  count_sectors
;   [bp+8]  start_lba (word, absolute LBA; supports hidden sectors for partition VBR)
load_img:
	push bp
	mov bp, sp

	mov ax, word [bp+8]
	mov word [start_lba], ax
	mov bx, word [bp+6]
	mov word [total_sector_count], bx

    ; Try to enable EDD (LBA) reads when available (required for HDD/partition boots).
    mov byte [edd_ok], 0
    mov dl, byte [boot_drive]
    mov bx, 0x55AA
    mov ah, 0x41
    int 0x13
    jc reset_disk
    cmp bx, 0xAA55
    jne reset_disk
    test cx, 0x0001
    jz reset_disk
    mov byte [edd_ok], 1
reset_disk:
    mov ax, 0
    mov dl, byte [boot_drive]
    int 0x13
    jc loading_error

    mov ah, 0x08
    mov dl, byte [boot_drive]
    int 0x13
    jc loading_error

    mov al, cl
    and al, 0x3f  ; 하위 6비트만 남겨서 순수한 섹터 수를 얻음
    xor ah, ah
    mov word [disk_spt], ax
    mov byte [disk_max_head], dh
    mov bl, dh
    xor bh, bh
    inc bx
    mov word [disk_heads], bx

    ; LBA -> CHS (start position)
    ; sector = (lba % spt) + 1
    ; tmp    =  lba / spt
    ; head   = (tmp % heads)
    ; track  =  tmp / heads
    mov ax, word [start_lba]
    mov word [cur_lba_lo], ax
    mov word [cur_lba_hi], 0
    xor dx, dx
    div word [disk_spt]          ; AX=tmp, DX=sec_index
    mov cl, dl
    inc cl
    mov byte [sector_number], cl

    xor dx, dx
    div word [disk_heads]        ; AX=track, DX=head
    mov byte [track_number], al
    mov byte [head_number], dl

    mov si, word [bp+4]
    mov es, si
    mov bx, 0x0000
    mov di, word [total_sector_count]

read_data:
    cmp di, 0
    je read_end
    sub di, 0x1

    cmp byte [edd_ok], 0
    jne read_data_edd

    ; ES:BX가 데이터를 쓸 버퍼 주소이므로, 루프마다 ES를 바꿀 필요 없이 BX를 증가시키는 것이 더 효율적입니다.
    ; 여기서는 원본 코드의 로직을 존중하여 ES를 변경하는 방식을 유지하되, 버그를 수정합니다.
    mov ah, 0x02
    mov al, 0x1
    mov ch, byte [track_number]
    mov cl, byte [sector_number]
    mov dh, byte [head_number]
    mov dl, byte [boot_drive]
    int 0x13
    jc loading_error
    jmp read_data_after

read_data_edd:
    ; EDD read: AH=42h with a Disk Address Packet (DAP)
    push si
    mov byte [edd_dap+0], 0x10
    mov byte [edd_dap+1], 0x00
    mov word [edd_dap+2], 0x0001
    mov word [edd_dap+4], bx
    mov word [edd_dap+6], es
    mov ax, word [cur_lba_lo]
    mov dx, word [cur_lba_hi]
    mov word [edd_dap+8], ax
    mov word [edd_dap+10], dx
    mov dword [edd_dap+12], 0

    mov si, edd_dap
    mov dl, byte [boot_drive]
    mov ah, 0x42
    int 0x13
    pop si
    jc loading_error

    inc word [cur_lba_lo]

read_data_after:

    ; 다음 섹터를 저장할 위치로 ES 세그먼트 주소 갱신
    ; 512바이트 = 0x200바이트. 세그먼트 주소는 16(0x10)배 되므로 0x200 / 0x10 = 0x20을 더함
    add si, 0x0020
    mov es, si

    cmp byte [edd_ok], 0
    jne read_data

    mov al, byte [sector_number]
    add al, 0x1
    mov byte [sector_number], al

    cmp al, byte [disk_spt]
    jbe read_data

    mov byte [sector_number], 0x01
    mov al, byte [head_number]
    inc al
    mov byte [head_number], al
    cmp al, byte [disk_max_head]
    jbe read_data

    mov byte [head_number], 0x00
    add byte [track_number], 0x01
    jmp read_data

read_end:

	pop bp
	ret

loading_error:
    jmp $
start_lba: dw 0x0000
sector_number: db 0x02
head_number: db 0x00
track_number: db 0x00
total_sector_count: dw 0x00
disk_spt: dw 0x00
disk_heads: dw 0x00
disk_max_head: db 0x00
edd_ok: db 0
cur_lba_lo: dw 0
cur_lba_hi: dw 0
edd_dap: times 16 db 0

%endif
