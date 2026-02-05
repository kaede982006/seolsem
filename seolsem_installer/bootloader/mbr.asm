[org 0x7C00]
[bits 16]

; Seolsem MBR
; - Relocates itself to 0x0000:0x0600
; - Loads the active partition's VBR to 0x0000:0x7C00 and jumps
; Notes:
; - Uses CHS reads (int 13h AH=02h) with LBA->CHS conversion.
; - Installer patches partition table entry #1 and marks it active.

%define MBR_RELOC 0x0600
%define REL(lbl)  (MBR_RELOC + (lbl - $$))

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; Relocate 512 bytes from 0x7C00 -> 0x0600 to avoid overwriting ourselves.
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw

    jmp 0x0000:(0x0600 + after_reloc - $$)

after_reloc:
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [REL(boot_drive)], dl

    ; Get BIOS geometry (SPT, heads count)
    mov ah, 0x08
    mov dl, [REL(boot_drive)]
    int 0x13
    jc disk_error

    mov al, cl
    and al, 0x3F
    xor ah, ah
    mov [REL(disk_spt)], ax

    mov al, dh
    inc al
    xor ah, ah
    mov [REL(disk_heads)], ax

    ; Find active partition entry (status=0x80). If none, use entry #1.
    mov si, (MBR_RELOC + 0x1BE)
    mov cx, 4
.find_active:
    cmp byte [si], 0x80
    je .found
    add si, 16
    loop .find_active
    mov si, 0x1BE
.found:
    ; Start LBA (DWORD). We use DX:AX.
    mov ax, [si + 8]
    mov dx, [si + 10]
    mov [REL(part_lba_lo)], ax
    mov [REL(part_lba_hi)], dx

    ; Prefer EDD (LBA read) when available to avoid CHS translation issues.
    mov dl, [REL(boot_drive)]
    mov bx, 0x55AA
    mov ah, 0x41
    int 0x13
    jc .use_chs
    cmp bx, 0xAA55
    jne .use_chs
    test cx, 0x0001
    jz .use_chs

    ; Build a DAP and read VBR to 0x0000:0x7C00 (we're running from 0x0600).
    mov si, REL(edd_dap)
    mov byte [si+0], 0x10
    mov byte [si+1], 0x00
    mov word [si+2], 0x0001
    mov word [si+4], 0x7C00
    mov word [si+6], 0x0000
    mov ax, [REL(part_lba_lo)]
    mov dx, [REL(part_lba_hi)]
    mov word [si+8], ax
    mov word [si+10], dx
    mov dword [si+12], 0

    mov dl, [REL(boot_drive)]
    mov ah, 0x42
    int 0x13
    jc disk_error
    jmp .jump_vbr

.use_chs:
    mov ax, [REL(part_lba_lo)]
    mov dx, [REL(part_lba_hi)]

    ; LBA -> CHS conversion (CH=cyl, CL=sec|cyl_hi, DH=head)
    div word [REL(disk_spt)]     ; DX:AX / SPT => AX=tmp, DX=sec_index
    mov cl, dl                   ; sec_index (0-based)

    xor dx, dx
    div word [REL(disk_heads)]   ; AX/tmp / heads => AX=cyl, DX=head
    mov dh, dl                   ; head
    mov ch, al                   ; cyl low 8

    inc cl                       ; sector is 1-based
    and cl, 0x3F
    mov al, ah                   ; cylinder high bits
    and al, 0x03
    shl al, 6
    or  cl, al

    ; Read VBR to 0x0000:0x7C00
    mov ax, 0x0000
    mov es, ax
    mov bx, 0x7C00
    mov dl, [REL(boot_drive)]
    mov ah, 0x02
    mov al, 0x01
    int 0x13
    jc disk_error

.jump_vbr:
    mov dl, [REL(boot_drive)]
    jmp 0x0000:0x7C00

disk_error:
    mov si, REL(msg_disk_error)
    call print_string
    cli
.hang:
    hlt
    jmp .hang

print_string:
    ; DS:SI = NUL-terminated string
    push ax
    push bx
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

boot_drive  db 0
disk_spt    dw 0
disk_heads  dw 0
part_lba_lo dw 0
part_lba_hi dw 0
edd_dap     times 16 db 0
msg_disk_error db 'MBR: Disk read error.', 0

times 446 - ($ - $$) db 0

; Partition table (patched by installer)
times 64 db 0

dw 0xAA55
