[org 0x00]
[bits 16]

jmp short boot_start
nop

; FAT12 BIOS Parameter Block (patched by image builder)
OEMName           db 'SEOLSEM '
bpbBytesPerSec    dw 512
bpbSecPerClust    db 1
bpbResSectors     dw 0
bpbFATs           db 2
bpbRootEnts       dw 224
bpbSectors        dw 2880
bpbMedia          db 0xF0
bpbFATsecs        dw 9
bpbSecPerTrack    dw 18
bpbHeads          dw 2
bpbHiddenSecs     dd 0
bpbHugeSectors    dd 0

; Extended boot record (FAT12/FAT16)
bsDriveNum        db 0
bsReserved1       db 0
bsBootSig         db 0x29
bsVolID           dd 0x534D4C53
bsVolLabel        db 'SEOLSEM    '
bsFileSys         db 'FAT12   '

boot_start:
    cli
    mov ax, 0x07c0
    mov ds, ax
    mov [boot_drive], dl

    xor ax, ax
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, sp

    ; Load stage2 from reserved sectors.
    ; Stage2 is stored at LBA (HiddenSectors + 1), for (bpbResSectors - 1) sectors.
    mov ax, [bpbResSectors]
    dec ax
    mov bx, ax                    ; count

    mov ax, word [bpbHiddenSecs]  ; start_lba = hidden + 1
    inc ax
    mov cx, 0x1000
    push ax
    push bx
    push cx

    call load_img
    add sp, 6

    mov dl, [boot_drive]
    jmp 0x1000:0x0000

boot_drive: db 0x00

%include "read.asm"

times 510 - ($-$$) db 0x00

dw 0xAA55
