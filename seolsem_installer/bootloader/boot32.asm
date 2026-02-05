[org 0x00]
[bits 16]

jmp short boot_start
nop

; FAT32 BPB (Matches what installer writes)
OEMName           db 'SEOLSEM '
bpbBytesPerSec    dw 512
bpbSecPerClust    db 0
bpbResSectors     dw 0
bpbFATs           db 0
bpbRootEnts       dw 0
bpbSectors        dw 0
bpbMedia          db 0
bpbFATsecs        dw 0
bpbSecPerTrack    dw 0
bpbHeads          dw 0
bpbHiddenSecs     dd 0
bpbTotalSecs32    dd 0
bpbFATSz32        dd 0
bpbExtFlags       dw 0
bpbFSVer          dw 0
bpbRootClus       dd 0
bpbFSInfo         dw 0
bpbBkBootSec      dw 0
bpbReserved       times 12 db 0
bpbDrvNum         db 0
bpbRes1           db 0
bpbBootSig        db 0
bpbVolID          dd 0
bpbVolLab         db '           '
bpbFSCols         db '        '

; Code starts around 0x5A
boot_start:
    cli
    mov ax, 0x07c0
    mov ds, ax
    mov [boot_drive], dl
    
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00
    mov bp, sp
    
    ; Load Stage2 from Hidden+2
    ; We assume HiddenSecs fits in 16-bit for start (LBA < 65535)
    ; because read.asm takes 16-bit LBA.
    
    mov ax, word [bpbHiddenSecs]
    add ax, 2

    ; Load stage2 from reserved area.
    ; NOTE: stage2 is loaded at 0x1000:0000 and must not overlap the kernel load
    ; segment used by sima.bin (KERNEL_LOAD_SEG=0x1080). Keep this small.
    mov bx, 4
    mov cx, 0x1000 ; Dest Segment
    
    push ax ; LBA
    push bx ; Count
    push cx ; Seg
    
    call load_img
    add sp, 6
    
    mov dl, [boot_drive]
    jmp 0x1000:0000

boot_drive: db 0x00

%include "read.asm"

times 510 - ($-$$) db 0x00
dw 0xAA55
