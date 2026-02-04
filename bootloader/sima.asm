[org 0x00]
[bits 16]

section .text

%define KERNEL_LOAD_SEG 0x1080
%define BOOTSEG         0x07C0
%define FAT_BUF_SEG     0x9000
%define ROOT_BUF_SEG    0x9200

%define ENTRY_OFF (CODE_BASE + ENTRY_REL)
%define ENTRY_SEG (KERNEL_LOAD_SEG + (ENTRY_OFF >> 4))
%define ENTRY_IP  (ENTRY_OFF & 0x0F)

jmp 0x1000:start

start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0xFFFE
    mov bp, sp

    mov ax, cs
    mov ds, ax
    cld
    mov [boot_drive], dl

    ; Read BIOS geometry for CHS conversion (more reliable than BPB values)
    push ds
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop ds
    jc disk_error
    mov al, cl
    and al, 0x3F
    mov [disk_spt], al
    mov byte [disk_spt+1], 0
    mov al, dh
    inc al
    mov [disk_heads], al
    mov byte [disk_heads+1], 0

    call enable_a20

    ; ---- Load BPB from boot sector ----
    mov ax, BOOTSEG
    mov es, ax

    mov ax, [es:0x0B]
    mov [bpb_bytes_per_sec], ax
    mov al, [es:0x0D]
    mov [bpb_sec_per_clus], al
    mov ax, [es:0x0E]
    mov [bpb_res_sectors], ax
    mov al, [es:0x10]
    mov [bpb_fats], al
    mov ax, [es:0x11]
    mov [bpb_root_ents], ax
    mov ax, [es:0x16]
    mov [bpb_fat_secs], ax
    mov ax, [es:0x18]
    mov [bpb_sec_per_track], ax
    mov ax, [es:0x1A]
    mov [bpb_heads], ax

    ; root_dir_sectors = (root_ents * 32 + bytes_per_sec - 1) / bytes_per_sec
    mov ax, [bpb_root_ents]
    mov bx, 32
    mul bx
    add ax, [bpb_bytes_per_sec]
    dec ax
    xor dx, dx
    div word [bpb_bytes_per_sec]
    mov [root_dir_sectors], ax

    ; first FAT LBA = reserved
    mov ax, [bpb_res_sectors]
    mov [fat_start_lba], ax

    ; root dir LBA = fat_start + fats * fat_secs
    xor bx, bx
    mov bl, [bpb_fats]
    mov ax, [bpb_fat_secs]
    mul bx
    add ax, [fat_start_lba]
    mov [root_start_lba], ax

    ; data LBA = root_start + root_dir_sectors
    mov ax, [root_start_lba]
    add ax, [root_dir_sectors]
    mov [data_start_lba], ax

    ; ---- Read FAT ----
    mov ax, FAT_BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [fat_start_lba]
    mov cx, [bpb_fat_secs]
    call read_sectors_lba

    ; ---- Read Root Directory ----
    mov ax, ROOT_BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [root_start_lba]
    mov cx, [root_dir_sectors]
    call read_sectors_lba

    ; ---- Find KERNEL.BIN ----
    mov ax, ROOT_BUF_SEG
    mov es, ax
    xor di, di
    mov cx, [bpb_root_ents]
.find_kernel:
    cmp cx, 0
    je kernel_not_found
    mov al, [es:di]
    cmp al, 0x00
    je kernel_not_found
    cmp al, 0xE5
    je .next_entry
    mov al, [es:di+11]
    cmp al, 0x0F
    je .next_entry

    push cx
    push di
    mov si, kernel_name
    mov bx, 11
.cmp_loop:
    mov al, [es:di]
    cmp al, [ds:si]
    jne .no_match
    inc di
    inc si
    dec bx
    jnz .cmp_loop
    pop di
    pop cx
    jmp kernel_found
.no_match:
    pop di
    pop cx
.next_entry:
    add di, 32
    dec cx
    jmp .find_kernel

kernel_not_found:
    mov si, msg_kernel_not_found
    call print_string
    jmp $

kernel_found:
    mov ax, [es:di+26]
    mov [cur_cluster], ax
    mov ax, [es:di+28]
    mov [kernel_size_low], ax
    mov ax, [es:di+30]
    mov [kernel_size_high], ax

    ; ---- Load kernel file ----
    mov ax, KERNEL_LOAD_SEG
    mov es, ax
    xor bx, bx
.load_loop:
    mov ax, [kernel_size_low]
    mov dx, [kernel_size_high]
    or ax, dx
    jz kernel_loaded

    mov ax, [cur_cluster]
    mov [cur_cluster_orig], ax
    cmp ax, 0x0FF8
    jae kernel_loaded

    ; lba = data_start + (cluster-2) * sec_per_clus
    mov ax, [cur_cluster]
    sub ax, 2
    xor cx, cx
    mov cl, [bpb_sec_per_clus]
    mul cx
    add ax, [data_start_lba]
    mov [cur_lba], ax

    mov ax, [cur_lba]
    xor cx, cx
    mov cl, [bpb_sec_per_clus]
    call read_sectors_lba

    ; kernel_size -= sec_per_clus * bytes_per_sec
    xor ax, ax
    mov al, [bpb_sec_per_clus]
    mul word [bpb_bytes_per_sec]
    sub [kernel_size_low], ax
    sbb [kernel_size_high], dx

    ; next cluster
    mov ax, [cur_cluster_orig]
    call fat12_next_cluster
    mov dx, [kernel_size_low]
    mov cx, [kernel_size_high]
    or dx, cx
    jz .size_ok
    cmp ax, 0x0FF8
    jb .size_ok
    mov si, msg_kernel_chain_error
    call print_string
    jmp $
.size_ok:
    mov [cur_cluster], ax
    jmp .load_loop

kernel_loaded:
    ; Verify entry opcode at loaded location (expect 0xEB)
    mov ax, ENTRY_SEG
    mov ds, ax
    mov si, ENTRY_IP
    mov al, [ds:si]
    cmp al, 0xEB
    je .entry_ok
    push cs
    pop ds
    mov si, msg_kernel_entry_error
    call print_string
    jmp $
.entry_ok:

    ; Set DS/ES to DGROUP and jump to kernel entry
    mov ax, KERNEL_LOAD_SEG
    add ax, DGROUP_DELTA
    mov ds, ax
    mov es, ax

    jmp ENTRY_SEG:ENTRY_IP

; --------------------
; FAT12 helpers
; --------------------

; AX = cluster, returns AX = next cluster
fat12_next_cluster:
    push ds
    push bx
    push si
    mov dx, ax            ; save cluster for odd/even
    mov bx, ax
    shr ax, 1
    add bx, ax            ; offset = cluster + cluster/2
    mov ax, FAT_BUF_SEG
    mov ds, ax
    mov si, bx
    mov ax, [ds:si]
    test dx, 1
    jz .even
    shr ax, 4
    and ax, 0x0FFF
    jmp .done
.even:
    and ax, 0x0FFF
.done:
    pop si
    pop bx
    pop ds
    ret

; Read CX sectors from LBA AX into ES:BX
read_sectors_lba:
    push ax
    push cx
    push dx
    push si
    push di

    mov di, ax            ; current LBA
.read_loop:
    cmp cx, 0
    je .read_done

    mov ax, di
    call read_sector_lba

    mov ax, [bpb_bytes_per_sec]
    add bx, ax
    jnc .no_wrap
    mov ax, es
    mov dx, [bpb_bytes_per_sec]
    shr dx, 4
    add ax, dx
    mov es, ax
.no_wrap:

    inc di
    dec cx
    jmp .read_loop

.read_done:
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    ret

; Read one sector at LBA AX into ES:BX
read_sector_lba:
    push ax
    push bx
    push cx
    push dx
    push si
    push es
    push ds
    push cs
    pop ds

    mov si, ax
    xor dx, dx
    mov ax, si
    div word [disk_spt]            ; AX = tmp, DX = sector index
    mov cl, dl                     ; sector index

    xor dx, dx
    div word [disk_heads]          ; AX = cylinder, DX = head
    mov dh, dl                     ; head
    mov ch, al                     ; cylinder low
    inc cl                         ; sector (1-based)
    and cl, 0x3F
    mov al, ah                     ; cylinder high bits
    and al, 0x03
    shl al, 6
    or cl, al                      ; add cylinder high bits

    mov dl, [boot_drive]
    mov ah, 0x02
    mov al, 0x01
    int 0x13
    jc disk_error

    pop ds
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

disk_error:
    push cs
    pop ds
    mov si, msg_disk_error
    call print_string
    jmp $

a20_support_error:
    jmp $

; --------------------
; Data
; --------------------

boot_drive        db 0
kernel_name       db 'KERNEL  BIN'

bpb_bytes_per_sec dw 0
bpb_sec_per_clus  db 0
bpb_res_sectors   dw 0
bpb_fats          db 0
bpb_root_ents     dw 0
bpb_fat_secs      dw 0
bpb_sec_per_track dw 0
bpb_heads         dw 0

disk_spt          dw 0
disk_heads        dw 0

root_dir_sectors  dw 0
fat_start_lba     dw 0
root_start_lba    dw 0
data_start_lba    dw 0

cur_cluster       dw 0
cur_cluster_orig  dw 0
kernel_size_low   dw 0
kernel_size_high  dw 0
cur_lba           dw 0

print_char:
    push bx
    push ds
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    pop ds
    pop bx
    ret

print_string:
    ; DS:SI = NUL-terminated string
    push ax
    push si
.loop:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .loop
.done:
    pop si
    pop ax
    ret

msg_disk_error          db 'Disk read error.', 13, 10, 0
msg_kernel_not_found    db 'KERNEL.BIN not found.', 13, 10, 0
msg_kernel_chain_error  db 'Kernel load error.', 13, 10, 0
msg_kernel_entry_error  db 'Kernel entry invalid.', 13, 10, 0

%include "a20.asm"
