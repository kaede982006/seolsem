; Seolsem installer stage2
; - Ask confirmation (Y/N) and require typing "YES"
; - Read the Seolsem system disk from A: (swapped) or B: (optional second floppy)
; - Partition HDD with MBR + single FAT32 partition at LBA 2048
; - Format partition as FAT32
; - Copy KERNEL.BIN, XENV.ENV, and BIN/* into the new FAT32 volume
; - Halt on completion

[org 0x00]
[bits 16]

section .text

%define STAGE2_SEG      0x1000

%define BUF_SEG         0x8000
%define SRC_FAT_SEG     0x9000
%define SRC_ROOT_SEG    0x9200
%define DST_FAT_SEG     0x9400
%define DST_ROOT_SEG    0x9600

%define SECTOR_SIZE     512

%define INSTALL_DRIVE   0x00         ; A: (installer boot drive)
%define SRC_DRIVE_B     0x01         ; B: (optional second floppy for Seolsem disk)
%define DST_DRIVE       0x80         ; First HDD

%define PART_START_LBA  2048

%define FAT12_EOC       0x0FF8
%define FAT12_EOC_VALUE 0x0FFF

%define FAT32_EOC       0x0FFFFFF8
%define FAT32_EOC_VALUE 0x0FFFFFFF
%define ROOT_CLUSTER    2

jmp STAGE2_SEG:start

; --------------------------
; Entry
; --------------------------
start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0xFFFE
    sti

    push cs
    pop ds
    cld

    call init_serial

    mov si, msg_banner
    call print_string

    ; ---- Get HDD geometry ----
    mov dl, DST_DRIVE
    call get_geometry
    jc fatal_disk
    mov [dst_spt], ax
    mov [dst_heads], bx
    mov [dst_cyls], cx

    ; ---- Detect HDD total sectors (EDD preferred) ----
    mov dl, DST_DRIVE
    call get_total_sectors_32
    jc  .no_edd
    mov byte [dst_has_edd], 1
    jmp .got_sectors
.no_edd:
    mov byte [dst_has_edd], 0
    call calc_total_sectors_chs_32
.got_sectors:
    mov si, msg_hdd_sectors
    call print_string
    ; BIOS interrupts may clobber DS; we keep DS=CS for our data.
    push cs
    pop ds
    mov ax, [dst_secs_hi]
    mov bx, [dst_secs_lo]
    call print_hex32_hilo
    call print_crlf

    ; ---- Confirmation UI ----
    mov si, msg_confirm1
    call print_string
    call read_key
    call print_crlf

    cmp al, 'Y'
    je  .confirm2
    cmp al, 'y'
    je  .confirm2
    mov si, msg_cancel
    call print_string
    jmp halt_forever

.confirm2:
    mov si, msg_confirm2
    call print_string
    call read_line_upper
    call print_crlf

    ; Accept only "YES"
    mov si, line_buf
    cmp byte [si+0], 'Y'
    jne .bad_yes
    cmp byte [si+1], 'E'
    jne .bad_yes
    cmp byte [si+2], 'S'
    jne .bad_yes
    cmp byte [si+3], 0
    jne .bad_yes
    ; Two-floppy setup: if B: already has the Seolsem system disk, skip swap prompt.
    mov byte [src_drive], SRC_DRIVE_B
    call read_src_boot_sector
    jnc .src_ready
    jmp .swap_prompt
.bad_yes:
    mov si, msg_cancel
    call print_string
    jmp halt_forever

.swap_prompt:
    mov si, msg_swap
    call print_string
    call read_key
    call print_crlf

    ; One-floppy setup: assume the user swapped the system disk into A:.
    mov byte [src_drive], INSTALL_DRIVE
    ; Reset A: after swap
    push ds
    xor ax, ax
    mov dl, INSTALL_DRIVE
    int 0x13
    pop ds

    call read_src_boot_sector
    jc fatal_disk
.src_ready:

    ; Detect whether the swapped source drive supports INT 13h extensions.
    ; If it does, we can use EDD (LBA) reads which are often more reliable than CHS.
    mov dl, [src_drive]
    call check_edd
    jc  .no_src_edd
    mov byte [src_has_edd], 1
    jmp .src_edd_done
.no_src_edd:
    mov byte [src_has_edd], 0
.src_edd_done:

    ; ---- Get floppy geometry ----
    mov dl, [src_drive]
    call get_geometry
    jc fatal_disk
    mov [src_spt], ax
    mov [src_heads], bx
    mov [src_cyls], cx

    ; ---- Source boot sector is already in BUF_SEG ----
    mov ax, BUF_SEG
    mov es, ax

    call parse_src_bpb
    jc fatal_bpb
    call calc_src_layout
    jc fatal_bpb

    ; ---- Compute destination (partition) layout (FAT32) ----
    call calc_dst_layout_fat32
    jc fatal_layout

    ; ---- Write MBR (LBA0) ----
    call write_mbr_fat32
    jc fatal_disk

    ; ---- Create FAT32 VBR ----
    ; We construct a new FAT32 BPB in memory
    call create_fat32_vbr
    
    ; Write VBR to partition start
    mov dx, word (PART_START_LBA >> 16)
    mov ax, word (PART_START_LBA & 0xFFFF)
    mov si, DST_DRIVE
    call write_sector_lba32
    jc fatal_disk

    ; Write FS Info Sector (Sector 1 relative to part start)
    call create_fs_info
    mov dx, word (PART_START_LBA >> 16)
    mov ax, word (PART_START_LBA & 0xFFFF)
    add ax, 1
    adc dx, 0
    mov si, DST_DRIVE
    call write_sector_lba32
    jc fatal_disk

    ; ---- Copy reserved sectors (stage2) ----
    mov si, msg_copy_reserved
    call print_string
    call copy_stage2_to_hdd
    jc fatal_disk
    
    ; ---- Clear FAT tables ----
    call clear_fat32_tables
    jc fatal_disk

    ; ---- Initialize Root Directory Cluster (Cluster 2) ----
    call init_root_cluster
    jc fatal_disk

    ; ---- Load source FAT and root directory (for file copying) ----
    mov ax, SRC_FAT_SEG
    mov es, ax
    xor bx, bx
    xor ax, ax
    mov al, [src_drive]
    mov si, ax
    xor dx, dx
    mov ax, [src_fat_start_lba]
    mov cx, [src_fat_secs]
    call read_sectors_lba32
    jc fatal_disk

    mov ax, SRC_ROOT_SEG
    mov es, ax
    xor bx, bx
    xor ax, ax
    mov al, [src_drive]
    mov si, ax
    xor dx, dx
    mov ax, [src_root_start_lba]
    mov cx, [src_root_dir_sectors]
    call read_sectors_lba32
    jc fatal_disk

    ; ---- Install filesystem (Copy files) ----
    mov si, msg_install_fs
    call print_string
    call install_fs_fat32
    jc fatal_install
    call print_crlf

    mov si, msg_done
    call print_string
    jmp halt_forever

; --------------------------
; Fatal handlers
; --------------------------
fatal_disk:
    mov si, msg_disk_error
    call print_string
    call print_disk_error_detail
    jmp halt_forever

fatal_bpb:
    mov si, msg_bad_bpb
    call print_string
    jmp halt_forever

fatal_layout:
    mov si, msg_layout_fail
    call print_string
    jmp halt_forever

fatal_install:
    mov si, msg_install_fail
    call print_string
    jmp halt_forever

halt_forever:
    mov si, msg_reboot
    call print_string
    xor ax, ax
    int 0x16        ; Wait for key
    int 0x19        ; Warm boot (DL should be drive? 19h usually reloads form boot drive)
    jmp $

msg_reboot db 13, 10, 'Press any key to reboot...', 13, 10, 0


; --------------------------
; BIOS: keyboard + printing
; --------------------------
init_serial:
    push ax
    push dx
    mov dx, 0x3F8 + 1 ; IER
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3 ; LCR (DLAB=1)
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0 ; Divisor Lo (9600 baud = 115200 / 12)
    mov al, 12
    out dx, al
    mov dx, 0x3F8 + 1 ; Divisor Hi
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3 ; LCR (8N1)
    mov al, 0x03
    out dx, al
    pop dx
    pop ax
    ret

print_char:
    push bx
    push ds
    push dx
    push ax
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    pop ax

    mov bl, al
    mov dx, 0x3F8 + 5 ; LSR
.wait_tx:
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov dx, 0x3F8 + 0 ; THR
    mov al, bl
    out dx, al

    pop dx
    pop ds
    pop bx
    ret

print_string:
    ; DS:SI = NUL-terminated
    push ax
.loop:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .loop
.done:
    pop ax
    ret

print_crlf:
    push ax
    mov al, 13
    call print_char
    mov al, 10
    call print_char
    pop ax
    ret

read_key:
    xor ah, ah
    int 0x16
    ret

; Read a short line, uppercase A-Z. Ends on Enter. Backspace supported.
; Result stored to line_buf and NUL-terminated.
read_line_upper:
    push ax
    push bx
    push cx
    push di
    mov di, line_buf
    mov cx, 0
.rl_loop:
    xor ah, ah
    int 0x16
    ; Ignore extended keys (AL=0, scan code in AH)
    test al, al
    jz  .rl_loop
    cmp al, 13
    je  .rl_done
    cmp al, 8
    jne .rl_char
    cmp cx, 0
    je  .rl_loop
    dec cx
    dec di
    ; erase one char on screen: BS, space, BS
    mov al, 8
    call print_char
    mov al, ' '
    call print_char
    mov al, 8
    call print_char
    jmp .rl_loop
.rl_char:
    cmp cx, (LINE_BUF_MAX-1)
    jae .rl_loop
    ; uppercase
    cmp al, 'a'
    jb  .store
    cmp al, 'z'
    ja  .store
    sub al, 32
.store:
    mov [di], al
    inc di
    inc cx
    call print_char
    jmp .rl_loop
.rl_done:
    mov byte [di], 0
    pop di
    pop cx
    pop bx
    pop ax
    ret

; Print 32-bit value (AX=hi, BX=lo) as 0xHHHH:LLLL
print_hex32_hilo:
    push ax
    push bx
    push cx
    push dx
    push si

    mov cx, ax
    call print_hex16
    mov al, ':'
    call print_char
    mov ax, bx
    call print_hex16_noprefix

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_hex16:
    push ax
    mov al, '0'
    call print_char
    mov al, 'x'
    call print_char
    pop ax
    jmp print_hex16_noprefix

print_hex16_noprefix:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.h_loop:
    mov ax, bx
    and ax, 0xF000
    shr ax, 12
    call hex_nibble
    shl bx, 4
    loop .h_loop
    pop cx
    pop bx
    pop ax
    ret

hex_nibble:
    ; AX = 0..15
    cmp al, 9
    jbe .digit
    add al, ('A' - 10)
    jmp .out
.digit:
    add al, '0'
.out:
    call print_char
    ret

; --------------------------
; BIOS: disk geometry + LBA I/O (CHS)
; --------------------------

; DL=drive. Returns: AX=spt, BX=heads(count), CX=cyls(count). CF=1 on error.
get_geometry:
    push dx
    push ds
    mov ah, 0x08
    int 0x13
    jc .err

    ; AX=spt
    mov al, cl
    and al, 0x3F
    xor ah, ah
    push ax

    ; BX=heads count
    mov bl, dh
    inc bl
    xor bh, bh

    ; CX=cyls count
    mov al, ch
    mov ah, cl
    and ah, 0xC0
    shr ah, 6
    mov cx, ax
    inc cx

    pop ax
    clc
    pop ds
    pop dx
    ret
.err:
    stc
    pop ds
    pop dx
    ret

; DL=drive. On success sets dst_secs_hi:dst_secs_lo (low 32 bits). CF=1 on error.
get_total_sectors_32:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds

    push cs
    pop ds

    mov [tmp_drive], dl

    mov dl, [tmp_drive]
    mov bx, 0x55AA
    mov ah, 0x41
    int 0x13
    push cs
    pop ds
    jc .fail
    cmp bx, 0xAA55
    jne .fail
    test cx, 0x0001
    jz .fail

    mov dl, [tmp_drive]
    mov word [edd_params+0], 0x1A
    mov si, edd_params
    mov ah, 0x48
    int 0x13
    push cs
    pop ds
    jc .fail

    mov ax, [edd_params+0x10]
    mov [dst_secs_lo], ax
    mov ax, [edd_params+0x12]
    mov [dst_secs_hi], ax
    clc
    jmp .done
.fail:
    stc
.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; DL=drive. CF=0 if INT 13h extensions are supported, CF=1 otherwise.
check_edd:
    push ax
    push bx
    push cx
    push ds

    push cs
    pop ds

    mov bx, 0x55AA
    mov ah, 0x41
    int 0x13
    push cs
    pop ds
    jc .fail
    cmp bx, 0xAA55
    jne .fail
    test cx, 0x0001
    jz .fail
    clc
    jmp .done
.fail:
    stc
.done:
    pop ds
    pop cx
    pop bx
    pop ax
    ret

; CHS fallback: dst_secs_hi:dst_secs_lo = cyls*heads*spt (low 32 bits)
calc_total_sectors_chs_32:
    push ax
    push bx
    push dx

    mov ax, [dst_cyls]
    mov bx, [dst_heads]
    mul bx
    mov bx, [dst_spt]
    mul bx
    mov [dst_secs_lo], ax
    mov [dst_secs_hi], dx

    pop dx
    pop bx
    pop ax
    clc
    ret

; Convert LBA (DX:AX) to CHS in CH/CL/DH.
; Drive number is read from [tmp_drive] (set by caller). This avoids clobbering DX (LBA hi).
lba_to_chs32:
    ; Select geometry based on drive number
    push ax
    mov al, [tmp_drive]
    cmp al, 0x80
    pop ax
    jb  .use_src
    mov bx, [dst_spt]
    mov di, [dst_heads]
    jmp .geo_ok
.use_src:
    mov bx, [src_spt]
    mov di, [src_heads]
.geo_ok:
    ; tmp = LBA / SPT, sec = LBA % SPT
    div bx                ; DX:AX / spt
    mov cl, dl            ; sec_index (0-based)

    ; cyl = tmp / HEADS, head = tmp % HEADS
    mov bx, di
    xor dx, dx
    div bx

    mov dh, dl            ; head
    mov ch, al            ; cyl low
    inc cl
    and cl, 0x3F

    mov bl, ah            ; cyl high bits
    and bl, 0x03
    shl bl, 6
    or  cl, bl

    ret

; Read one sector at LBA DX:AX from drive SI into ES:BX. CF=1 on error.
read_sector_lba32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds
    ; Save drive to a temp byte (don't use DL because DX holds LBA high word).
    xchg ax, si
    mov [tmp_drive], al
    mov [last_drive], al
    xchg ax, si
    mov byte [last_op], 'R'
    mov [last_lba_lo], ax
    mov [last_lba_hi], dx
    ; Prefer EDD for HDD when available; for floppies we *try* EDD first (some BIOS/QEMU
    ; setups behave oddly with CHS reads after media change), then fall back to CHS.
    mov al, [tmp_drive]
    cmp al, 0x80
    jb  .edd_floppy
    cmp byte [dst_has_edd], 0
    je  .chs
    mov di, 3
    jmp .edd_retry
.edd_floppy:
    mov di, 1
.edd_retry:
    ; Reload LBA for each retry (INT 13h clobbers registers).
    mov ax, [last_lba_lo]
    mov dx, [last_lba_hi]
    ; Prepare DAP (uses DS:SI)
    push cs
    pop ds
    mov byte [last_method], 'E'
    mov word [edd_dap+2], 1          ; sectors
    mov word [edd_dap+4], bx         ; buffer offset
    mov word [edd_dap+6], es         ; buffer segment
    mov word [edd_dap+8], ax         ; LBA low word
    mov word [edd_dap+10], dx        ; LBA high word
    mov dword [edd_dap+12], 0        ; LBA upper dword
    mov si, edd_dap
    mov dl, [tmp_drive]
    mov ah, 0x42
    int 0x13
    push cs
    pop ds
    jnc .ok

    mov [last_ah], ah
    xor ax, ax
    mov dl, [tmp_drive]
    int 0x13
    push cs
    pop ds
    dec di
    jnz .edd_retry
    ; If this was a floppy, fall back to CHS. For HDD, fail.
    mov al, [tmp_drive]
    cmp al, 0x80
    jb  .chs
    stc
    jmp .done

.chs:
    mov di, 3
.retry:
    ; Reload LBA for each retry (INT 13h clobbers registers).
    mov ax, [last_lba_lo]
    mov dx, [last_lba_hi]
    ; lba_to_chs32 clobbers BX/DI, but BIOS uses ES:BX as the transfer buffer and
    ; DI is our retry counter. Preserve both across the conversion.
    push bx
    push di
    call lba_to_chs32
    pop di
    pop bx

    mov dl, [tmp_drive]
    mov ah, 0x02
    mov al, 0x01
    mov byte [last_method], 'C'
    mov [last_ch], ch
    mov [last_cl], cl
    mov [last_dh], dh
    int 0x13
    push cs
    pop ds
    jnc .ok

    mov [last_ah], ah
    xor ax, ax
    mov dl, [tmp_drive]
    int 0x13
    push cs
    pop ds
    dec di
    jnz .retry
    stc
    jmp .done
.ok:
    clc
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write one sector at LBA DX:AX to drive SI from ES:BX. CF=1 on error.
write_sector_lba32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds
    ; Save drive to a temp byte (don't use DL because DX holds LBA high word).
    xchg ax, si
    mov [tmp_drive], al
    mov [last_drive], al
    xchg ax, si
    mov byte [last_op], 'W'
    mov [last_lba_lo], ax
    mov [last_lba_hi], dx
    ; Prefer EDD for HDD when available; also use it for the swapped source floppy
    ; when supported. Otherwise fall back to legacy CHS.
    mov al, [tmp_drive]
    cmp al, 0x80
    jb  .maybe_src_edd
    cmp byte [dst_has_edd], 0
    je  .chs
    jmp .edd
.maybe_src_edd:
    cmp byte [src_has_edd], 0
    je  .chs
.edd:
    mov di, 3
.edd_retry:
    ; Reload LBA for each retry (INT 13h clobbers registers).
    mov ax, [last_lba_lo]
    mov dx, [last_lba_hi]
    ; Prepare DAP (uses DS:SI)
    push cs
    pop ds
    mov word [edd_dap+2], 1          ; sectors
    mov word [edd_dap+4], bx         ; buffer offset
    mov word [edd_dap+6], es         ; buffer segment
    mov word [edd_dap+8], ax         ; LBA low word
    mov word [edd_dap+10], dx        ; LBA high word
    mov dword [edd_dap+12], 0        ; LBA upper dword
    mov si, edd_dap
    mov dl, [tmp_drive]
    mov ah, 0x43
    xor al, al
    int 0x13
    push cs
    pop ds
    jnc .ok

    mov [last_ah], ah
    xor ax, ax
    mov dl, [tmp_drive]
    int 0x13
    push cs
    pop ds
    dec di
    jnz .edd_retry
    stc
    jmp .done

.chs:
    mov di, 3
.retry:
    ; Reload LBA for each retry (INT 13h clobbers registers).
    mov ax, [last_lba_lo]
    mov dx, [last_lba_hi]
    ; lba_to_chs32 clobbers BX/DI, but BIOS uses ES:BX as the transfer buffer and
    ; DI is our retry counter. Preserve both across the conversion.
    push bx
    push di
    call lba_to_chs32
    pop di
    pop bx

    mov dl, [tmp_drive]
    mov ah, 0x03
    mov al, 0x01
    mov [last_ch], ch
    mov [last_cl], cl
    mov [last_dh], dh
    int 0x13
    push cs
    pop ds
    jnc .ok

    mov [last_ah], ah
    xor ax, ax
    mov dl, [tmp_drive]
    int 0x13
    push cs
    pop ds
    dec di
    jnz .retry
    stc
    jmp .done
.ok:
    clc
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Read CX sectors from LBA DX:AX from drive SI into ES:BX (buffer advances by 512).
read_sectors_lba32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

.loop:
    cmp cx, 0
    je .done

    call read_sector_lba32
    jc .fail

    add bx, SECTOR_SIZE
    jnc .no_wrap
    ; Advance segment by 512 bytes without clobbering LBA registers.
    mov di, es
    add di, 0x20
    mov es, di
.no_wrap:
    add ax, 1
    adc dx, 0
    dec cx
    jmp .loop
.fail:
    stc
    jmp .ret
.done:
    clc
.ret:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
	ret

; Read source boot sector (LBA 0) from [src_drive] into BUF_SEG.
; CF=1 on error.
read_src_boot_sector:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    ; Use CHS read for sector 0 (0/0/1) so we don't depend on geometry/EDD.
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx

    push cs
    pop ds
    mov dl, [src_drive]
    mov [tmp_drive], dl
    mov [last_drive], dl
    mov byte [last_op], 'R'
    mov word [last_lba_lo], 0
    mov word [last_lba_hi], 0

    mov si, 3
.retry:
    xor ax, ax
    int 0x13                    ; reset drive
    push cs
    pop ds

    mov ah, 0x02
    mov al, 0x01
    xor ch, ch
    mov cl, 0x01
    xor dh, dh
    int 0x13
    push cs
    pop ds
    jnc .ok
    mov [last_ah], ah
    dec si
    jnz .retry
    stc
    jmp .ret
.ok:
    clc

.ret:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --------------------------
; Disk error debug output
; --------------------------
	print_disk_error_detail:
    mov si, msg_disk_detail
    call print_string

    ; op
    mov al, [last_op]
    call print_char
    mov al, ' '
    call print_char

    ; drv
    mov ax, 0
    mov al, [last_drive]
    call print_hex16
    mov al, ' '
    call print_char

    ; lba
    mov ax, [last_lba_hi]
    mov bx, [last_lba_lo]
    call print_hex32_hilo
    mov al, ' '
    call print_char

    ; ah status
    mov ax, 0
    mov al, [last_ah]
    call print_hex16

    call print_crlf
    ret

; --------------------------
; FAT12 helpers (source FAT only)
; --------------------------

; AX=cluster, returns AX=entry value (12-bit)
fat12_get_entry:
    push bx
    push dx
    push si
    mov bx, ax
    mov dx, ax
    shr ax, 1
    add bx, ax                 ; offset = cluster + cluster/2
    mov ax, SRC_FAT_SEG
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
    pop dx
    pop bx
    push cs
    pop ds
    ret

; --------------------------
; Source BPB/layout parsing
; --------------------------

parse_src_bpb:
    ; ES:0 = boot sector
    mov ax, [es:0x0B]
    cmp ax, SECTOR_SIZE
    jne .err

    mov al, [es:0x0D]
    mov [src_sec_per_clus], al
    mov ax, [es:0x0E]
    mov [src_reserved], ax
    mov al, [es:0x10]
    mov [src_fats], al
    mov ax, [es:0x11]
    mov [src_root_ents], ax

    mov ax, [es:0x13]
    test ax, ax
    jnz .tot16
    mov ax, [es:0x20]
    mov [src_total_lo], ax
    mov ax, [es:0x22]
    mov [src_total_hi], ax
    jmp .tot_done
.tot16:
    mov [src_total_lo], ax
    mov word [src_total_hi], 0
.tot_done:
    mov ax, [es:0x16]
    mov [src_fat_secs], ax

    ; src_spc_shift = log2(sec_per_clus), reject non power-of-two or >64
    mov al, [src_sec_per_clus]
    mov bl, 0
.spc_loop:
    cmp al, 1
    je  .spc_done
    test al, 1
    jnz .err
    shr al, 1
    inc bl
    cmp bl, 6
    ja  .err
    jmp .spc_loop
.spc_done:
    mov [src_spc_shift], bl

    ; Basic sanity
    mov al, [src_sec_per_clus]
    test al, al
    jz .err
    mov ax, [src_reserved]
    test ax, ax
    jz .err
    mov ax, [src_fat_secs]
    test ax, ax
    jz .err
    clc
    ret
.err:
    stc
    ret

calc_src_layout:
    ; root_dir_sectors = (root_ents + 15)/16
    mov ax, [src_root_ents]
    add ax, 15
    shr ax, 4
    mov [src_root_dir_sectors], ax

    ; fat_start = reserved
    mov ax, [src_reserved]
    mov [src_fat_start_lba], ax

    ; root_start = reserved + fats*fat_secs
    xor bx, bx
    mov bl, [src_fats]
    mov ax, [src_fat_secs]
    mul bx
    add ax, [src_fat_start_lba]
    mov [src_root_start_lba], ax

    ; data_start = root_start + root_dir_sectors
    mov ax, [src_root_start_lba]
    add ax, [src_root_dir_sectors]
    mov [src_data_start_lba], ax

    clc
    ret

; --------------------------
; Destination layout + formatting
; --------------------------

; Choose dst_sec_per_clus and calculate dst_total (32-bit), dst_fat_secs, dst_root_dir_sectors, dst_*_start.
    add ax, [dst_root_dir_sectors]
    mov [dst_data_start_lba], ax

    ; dst_max_cluster = clusters + 1
    mov ax, [tmp_clusters]
    inc ax
    mov [dst_max_cluster], ax

    ; If disk is larger than FAT12 max, message (dst_total may be capped)
    mov ax, [avail_hi]
    cmp ax, [dst_total_hi]
    jb  .no_cap_msg
    ja  .cap_msg
    mov ax, [avail_lo]
    cmp ax, [dst_total_lo]
    jbe .no_cap_msg
.cap_msg:
    mov si, msg_cap
    call print_string
    call print_crlf
.no_cap_msg:
    clc
    ret

.fail_small:
    stc
    ret

; Given total sectors in DX:AX and dst_sec_per_clus/dst_spc_shift, compute tmp_fat_secs and tmp_clusters.
; CF=1 on failure (too small).
calc_fat_layout_for_total:
    ; Input: total sectors in DX:AX
    push bx
    push cx
    push si

    mov [tmp_total_lo], ax
    mov [tmp_total_hi], dx
    mov word [tmp_fat_secs], 1

.iter:
    ; overhead = reserved + root_dir + fats*fat_secs (fats=2)
    mov bx, [dst_reserved]
    add bx, [dst_root_dir_sectors]
    mov cx, [tmp_fat_secs]
    shl cx, 1
    add bx, cx

    ; data = total - overhead
    mov ax, [tmp_total_lo]
    mov dx, [tmp_total_hi]
    sub ax, bx
    sbb dx, 0
    jc .fail

    ; clusters = data >> shift
    mov cl, [dst_spc_shift]
    call shr32_dxax_cl
    test dx, dx
    jnz .too_many
    test ax, ax
    jz .fail
    mov [tmp_clusters], ax

    ; fat_secs_needed from clusters
    mov si, ax
    mov ax, si
    call fat_secs_for_clusters
    cmp ax, [tmp_fat_secs]
    je .done
    mov [tmp_fat_secs], ax
    jmp .iter

.too_many:
    mov word [tmp_clusters], 0xFFFF
    clc
    jmp .ret
.done:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop si
    pop cx
    pop bx
    ret

; AX=clusters (<=4084). Returns AX=fat_secs_needed.
fat_secs_for_clusters:
    push bx
    push dx

    ; fat_bytes = ((clusters+2)*3 + 1) / 2
    add ax, 2
    mov bx, 3
    mul bx              ; DX:AX
    add ax, 1
    adc dx, 0
    shr ax, 1

    ; fat_secs = ceil(fat_bytes/512) = (fat_bytes + 511) >> 9
    add ax, 511
    mov cl, 9
    shr ax, cl

    pop dx
    pop bx
    ret

; DX:AX >>= CL (0..16)
shr32_dxax_cl:
    push bx
    mov bl, cl
.sloop:
    test bl, bl
    jz .sdone
    shr dx, 1
    rcr ax, 1
    dec bl
    jmp .sloop
.sdone:
    pop bx
    ret

; DX:AX = clusters * sec_per_clus (power-of-two, shift in dst_spc_shift).
; Input: AX=clusters, DX must be 0 on entry.
clusters_mul_spc_to_dxax:
    push bx
    mov bl, [dst_spc_shift]
.mloop:
    test bl, bl
    jz .mdone
    shl ax, 1
    rcl dx, 1
    dec bl
    jmp .mloop
.mdone:
    pop bx
    ret

; Patch destination VBR in ES:0 based on dst layout and partition start.
patch_dst_vbr:
    ; bytes/sector
    mov word [es:0x0B], SECTOR_SIZE
    ; sec/clus
    mov al, [dst_sec_per_clus]
    mov [es:0x0D], al
    ; reserved
    mov ax, [dst_reserved]
    mov [es:0x0E], ax
    ; fats
    mov byte [es:0x10], 2
    ; root ents
    mov ax, [dst_root_ents]
    mov [es:0x11], ax
    ; media
    mov byte [es:0x15], 0xF8
    ; fat secs
    mov ax, [dst_fat_secs]
    mov [es:0x16], ax
    ; spt/heads
    mov ax, [dst_spt]
    mov [es:0x18], ax
    mov ax, [dst_heads]
    mov [es:0x1A], ax
    ; hidden sectors
    mov dword [es:0x1C], PART_START_LBA

    ; total sectors (16/32)
    mov ax, [dst_total_hi]
    test ax, ax
    jnz .use32
    mov ax, [dst_total_lo]
    mov [es:0x13], ax
    mov dword [es:0x20], 0
    jmp .tot_done
.use32:
    mov word [es:0x13], 0
    mov ax, [dst_total_lo]
    mov [es:0x20], ax
    mov ax, [dst_total_hi]
    mov [es:0x22], ax
.tot_done:

    ; drive number
    mov byte [es:0x24], 0x80

    ; Signature
    mov word [es:510], 0xAA55
    clc
    ret

; Write MBR sector to HDD LBA0 (patched partition table).
write_mbr:
    push ds
    push si
    push di
    push cx
    push ax
    push dx

    ; Copy template to BUF_SEG
    push cs
    pop ds
    mov si, mbr_template
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    mov cx, 512
    rep movsb

    ; Patch partition entry #1 at 0x1BE
    mov di, 0x1BE
    mov byte [es:di+0], 0x80      ; active
    mov byte [es:di+1], 0x00      ; start head
    mov byte [es:di+2], 0x02      ; start sector (dummy)
    mov byte [es:di+3], 0x00      ; start cylinder
    mov byte [es:di+4], 0x01      ; type FAT12
    mov byte [es:di+5], 0xFE      ; end head (dummy max)
    mov byte [es:di+6], 0xFF      ; end sector/cyl hi
    mov byte [es:di+7], 0xFF      ; end cylinder

    ; start LBA (2048)
    mov word [es:di+8], (PART_START_LBA & 0xFFFF)
    mov word [es:di+10], 0

    ; size sectors (dst_total)
    mov ax, [dst_total_lo]
    mov [es:di+12], ax
    mov ax, [dst_total_hi]
    mov [es:di+14], ax

    ; Write to HDD LBA0
    xor dx, dx
    xor ax, ax
    xor bx, bx
    mov si, DST_DRIVE
    call write_sector_lba32

    pop dx
    pop ax
    pop cx
    pop di
    pop si
    pop ds
    ret

; Build dest FAT in memory and write both copies.
build_and_write_dst_fat:
    push ds
    push si
    push di
    push bx
    push cx
    push ax
    push dx

    ; Clear DST FAT buffer (dst_fat_secs * 512)
    mov ax, DST_FAT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, [dst_fat_secs]
    shl cx, 8            ; sectors * 256 words
    rep stosw

    ; Copy source FAT bytes into DST FAT (min(src_fat_secs, dst_fat_secs))
    mov cx, [src_fat_secs]
    cmp cx, [dst_fat_secs]
    jbe .fat_copy_ok
    mov cx, [dst_fat_secs]
.fat_copy_ok:
    shl cx, 8            ; sectors * 256 words
    push ds
    mov ax, SRC_FAT_SEG
    mov ds, ax
    xor si, si
    mov ax, DST_FAT_SEG
    mov es, ax
    xor di, di
    rep movsw
    pop ds

    ; Force FAT[0:3] = F8 FF FF
    mov ax, DST_FAT_SEG
    mov es, ax
    mov byte [es:0], 0xF8
    mov byte [es:1], 0xFF
    mov byte [es:2], 0xFF

    ; base_lba = PART_START + dst_fat_start
    xor dx, dx
    mov ax, [dst_fat_start_lba]
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, 0
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx

    mov byte [tmp_fat_index], 0
.fat_loop:
    mov al, [tmp_fat_index]
    cmp al, 2
    jae .ok

    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    cmp byte [tmp_fat_index], 0
    je  .lba_ready
    add ax, [dst_fat_secs]
    adc dx, 0
.lba_ready:
    ; write dst_fat_secs sectors from DST_FAT_SEG
    mov bx, 0
    mov cx, [dst_fat_secs]
.wsec:
    push cx
    mov si, DST_DRIVE
    call write_sector_lba32
    pop cx
    jc .fail

    add bx, SECTOR_SIZE
    add ax, 1
    adc dx, 0
    dec cx
    jnz .wsec

    inc byte [tmp_fat_index]
    jmp .fat_loop

.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop dx
    pop ax
    pop cx
    pop bx
    pop di
    pop si
    pop ds
    ret

; Build dest root in memory and write.
build_and_write_dst_root:
    push ds
    push si
    push di
    push cx

    ; Clear DST root buffer (dst_root_dir_sectors * 512)
    mov ax, DST_ROOT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, [dst_root_dir_sectors]
    shl cx, 8
    rep stosw

    ; Copy src root bytes into DST root (src_root_dir_sectors)
    mov cx, [src_root_dir_sectors]
    shl cx, 8
    push ds
    mov ax, SRC_ROOT_SEG
    mov ds, ax
    mov si, 0
    mov ax, DST_ROOT_SEG
    mov es, ax
    mov di, 0
    rep movsw
    pop ds

    ; Write dst_root_dir_sectors sectors to HDD
    mov ax, DST_ROOT_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    xor dx, dx
    mov ax, [dst_root_start_lba]
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, 0
    mov cx, [dst_root_dir_sectors]

.wloop:
    push cx
    call write_sector_lba32
    pop cx
    jc .fail

    add bx, SECTOR_SIZE
    add ax, 1
    adc dx, 0
    dec cx
    jnz .wloop

    clc
    jmp .ret
.fail:
    stc
.ret:
    pop cx
    pop di
    pop si
    pop ds
    ret

; Copy allocated clusters from source to destination (pads cluster tail with zeros if dst cluster is larger).
copy_used_clusters:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; src_cluster_count = (src_total - src_data_start) / src_sec_per_clus
    mov ax, [src_total_lo]
    mov dx, [src_total_hi]
    sub ax, [src_data_start_lba]
    sbb dx, 0
    ; shift right by src spc (only supports spc=1,2,4,8,16,32,64)
    mov cl, [src_spc_shift]
    call shr32_dxax_cl
    mov [src_cluster_count], ax

    ; dst_data_abs = PART_START + dst_data_start
    mov ax, [dst_data_start_lba]
    mov dx, 0
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, 0
    mov [dst_data_abs_lo], ax
    mov [dst_data_abs_hi], dx

    mov bp, 2
.cluster_loop:
    mov ax, [src_cluster_count]
    add ax, 1
    cmp bp, ax
    ja  .done

    ; if FAT entry == 0 -> free
    mov ax, bp
    call fat12_get_entry
    test ax, ax
    jz .next_cluster

    ; ensure cluster fits in destination volume
    mov ax, [dst_max_cluster]
    cmp bp, ax
    ja  .next_cluster

    ; src_cluster_lba = src_data_start + (cluster-2)*src_spc
    mov ax, bp
    sub ax, 2
    xor dx, dx
    mov cl, [src_sec_per_clus]
    mov ch, 0
    mul cx                       ; DX:AX = idx*spc
    add ax, [src_data_start_lba]
    adc dx, 0
    mov [tmp_src_lba_lo], ax
    mov [tmp_src_lba_hi], dx

    ; dst_cluster_lba = dst_data_abs + (cluster-2)*dst_spc
    mov ax, bp
    sub ax, 2
    xor dx, dx
    mov cl, [dst_sec_per_clus]
    mov ch, 0
    mul cx                       ; DX:AX = idx*spc (may exceed 16-bit)
    add ax, [dst_data_abs_lo]
    adc dx, [dst_data_abs_hi]
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx

    ; Copy src spc sectors
    xor di, di
.copy_s:
    mov al, [src_sec_per_clus]
    xor ah, ah
    cmp di, ax
    jae .pad_s

    ; Read src
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    xor cx, cx
    mov cl, [src_drive]
    mov si, cx
    mov ax, [tmp_src_lba_lo]
    mov dx, [tmp_src_lba_hi]
    add ax, di
    adc dx, 0
    call read_sector_lba32
    jc .fail

    ; Write dst
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    add ax, di
    adc dx, 0
    call write_sector_lba32
    jc .fail

    inc di
    jmp .copy_s

.pad_s:
    ; Pad remaining sectors in destination cluster with zeros if needed
    mov al, [dst_sec_per_clus]
    xor ah, ah
    cmp di, ax
    jae .cluster_done

    ; Zero buffer
    mov ax, BUF_SEG
    mov es, ax
    push di
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw
    pop di

    ; Write zeros to dst sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    add ax, di
    adc dx, 0
    call write_sector_lba32
    jc .fail

    inc di
    jmp .pad_s

.cluster_done:
.next_cluster:
    inc bp
    jmp .cluster_loop

.done:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --------------------------
; Filesystem install (rebuild FAT/root for dst cluster size)
; --------------------------

%define BIN_MAX_FILES 32

; Install Seolsem filesystem into the formatted destination partition.
; - Rebuild destination FAT/root/bin for the chosen dst_sec_per_clus.
; - Copy KERNEL.BIN, XENV.ENV and BIN/* from the swapped source floppy.
; CF=1 on failure.
; (install_fs removed - unused FAT12 logic)




; Find a root directory entry by its 11-byte name.
; IN:  DS:SI = 11-byte name
; OUT: ES=SRC_ROOT_SEG, DI=entry offset, CF=0 if found; CF=1 if not found.
find_root_entry:
    push ax
    push bx
    push cx
    push dx
    push si
    push bp

    mov dx, si                   ; save name ptr
    mov ax, SRC_ROOT_SEG
    mov es, ax
    xor di, di
    mov cx, [src_root_ents]
.loop:
    cmp cx, 0
    je  .not_found
    mov al, [es:di]
    cmp al, 0x00
    je  .not_found
    cmp al, 0xE5
    je  .next
    mov al, [es:di+11]
    cmp al, 0x0F
    je  .next

    mov si, dx
    mov bx, di
    mov bp, 11
.cmp:
    mov al, [es:bx]
    cmp al, [ds:si]
    jne .next
    inc bx
    inc si
    dec bp
    jnz .cmp
    clc
    jmp .ret
.next:
    add di, 32
    dec cx
    jmp .loop
.not_found:
    stc
.ret:
    pop bp
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Parse BIN directory on source and populate bin_file_* arrays.
; CF=1 on failure.
parse_src_bin_dir:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov byte [bin_file_count], 0
    mov ax, [src_bin_cluster]
    mov [tmp_parse_cluster], ax

.cluster_loop:
    mov ax, [tmp_parse_cluster]
    cmp ax, FAT12_EOC
    jae .done
    cmp ax, 2
    jb  .done

    ; base LBA for this cluster
    call src_cluster_to_lba
    mov [tmp_src_lba_lo], ax
    mov [tmp_src_lba_hi], dx

    xor bp, bp                  ; sector index within cluster
.sector_loop:
    mov al, [src_sec_per_clus]
    xor ah, ah
    cmp bp, ax
    jae .next_cluster

    ; Read one sector of directory to BUF_SEG
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    xor cx, cx
    mov cl, [src_drive]
    mov si, cx
    mov ax, [tmp_src_lba_lo]
    mov dx, [tmp_src_lba_hi]
    add ax, bp
    adc dx, 0
    call read_sector_lba32
    jc .fail

    ; Parse 16 entries in this sector
    xor di, di                  ; entry offset in sector
.entry_loop:
    cmp di, 512
    jae .sector_next

    mov al, [es:di]
    cmp al, 0x00
    je  .done                   ; end of directory
    cmp al, 0xE5
    je  .entry_next

    mov al, [es:di+11]
    cmp al, 0x0F
    je  .entry_next
    test al, 0x08               ; volume label
    jnz .entry_next

    ; skip '.' and '..'
    mov al, [es:di]
    cmp al, '.'
    jne .not_dot
    mov al, [es:di+1]
    cmp al, ' '
    je  .entry_next
    cmp al, '.'
    je  .entry_next
.not_dot:

    ; skip subdirectories for now
    mov al, [es:di+11]
    test al, 0x10
    jnz .entry_next

    ; store entry if we have room
    mov al, [bin_file_count]
    cmp al, BIN_MAX_FILES
    jae .entry_next

    ; dest base pointers
    mov bl, al                  ; index (0..31)

    ; Copy name[11] to bin_file_name[index]
    mov al, bl
    mov bh, 11
    mul bh                      ; AX = index*11
    mov bx, ax                  ; name offset
    mov si, di
    mov cx, 11
.name_copy:
    mov al, [es:si]
    mov [bin_file_name + bx], al
    inc si
    inc bx
    loop .name_copy

    ; Store attr
    mov al, [bin_file_count]
    xor ah, ah
    mov bx, ax
    mov al, [es:di+11]
    mov [bin_file_attr + bx], al

    ; Store start cluster + size (word arrays: index*2)
    shl bx, 1
    mov ax, [es:di+26]
    mov [bin_file_src_cluster + bx], ax
    mov ax, [es:di+28]
    mov [bin_file_size_lo + bx], ax
    mov ax, [es:di+30]
    mov [bin_file_size_hi + bx], ax

    inc byte [bin_file_count]

.entry_next:
    add di, 32
    jmp .entry_loop

.sector_next:
    inc bp
    jmp .sector_loop

.next_cluster:
    mov ax, [tmp_parse_cluster]
    call fat12_get_entry
    mov [tmp_parse_cluster], ax
    jmp .cluster_loop

.done:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Initialize destination FAT buffer and allocator.
dst_fat_init:
    push ax
    push cx
    push di
    mov ax, DST_FAT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, [dst_fat_secs]
    shl cx, 8
    rep stosw
    mov ax, DST_FAT_SEG
    mov es, ax
    mov byte [es:0], 0xF8
    mov byte [es:1], 0xFF
    mov byte [es:2], 0xFF
    mov word [dst_next_cluster], 2
    pop di
    pop cx
    pop ax
    ret

; Clear destination root directory buffer.
dst_root_init:
    push ax
    push cx
    push di
    mov ax, DST_ROOT_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, [dst_root_dir_sectors]
    shl cx, 8
    rep stosw
    pop di
    pop cx
    pop ax
    ret

; Set one FAT12 entry in the destination FAT cache.
; IN: AX=cluster, BX=value (12-bit)
fat12_set_entry_dst:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    mov dx, ax                   ; cluster for odd/even test
    mov cx, ax
    shr ax, 1
    add cx, ax                   ; offset = cluster + cluster/2
    mov ax, DST_FAT_SEG
    mov ds, ax
    mov si, cx
    test dx, 1
    jz .even
    ; odd cluster:
    ;   fat[offset] high nibble = value low nibble
    ;   fat[offset+1] = value >> 4
    mov al, [ds:si]
    and al, 0x0F
    mov dl, bl
    and dl, 0x0F
    shl dl, 4
    or  al, dl
    mov [ds:si], al

    mov ax, bx
    shr ax, 4
    mov [ds:si+1], al
    jmp .done
.even:
    ; even cluster:
    ;   fat[offset] = value low byte
    ;   fat[offset+1] low nibble = value >> 8 (preserve high nibble)
    mov [ds:si], bl
    mov al, [ds:si+1]
    and al, 0xF0
    mov dl, bh
    and dl, 0x0F
    or  al, dl
    mov [ds:si+1], al
.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Allocate a contiguous cluster chain in destination FAT.
; IN:  AX = cluster count (0 allowed)
; OUT: AX = first cluster (0 if count==0), CF=1 on failure.
dst_alloc_chain:
    push bx
    push cx
    push dx
    push si
    push di
    mov cx, ax
    test cx, cx
    jnz .nonzero
    xor ax, ax
    clc
    jmp .ret
.nonzero:
    mov di, [dst_next_cluster]   ; first
    mov dx, di
    add dx, cx
    dec dx                       ; last
    cmp dx, [dst_max_cluster]
    ja  .fail
    mov si, di                   ; current
.loop:
    dec cx
    jz  .last
    mov ax, si
    mov bx, si
    inc bx
    call fat12_set_entry_dst
    inc si
    jmp .loop
.last:
    mov ax, si
    mov bx, FAT12_EOC_VALUE
    call fat12_set_entry_dst
    mov ax, dx
    inc ax
    mov [dst_next_cluster], ax
    mov ax, di
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; bytes_to_sectors_ceil: sectors = ceil(bytes/512)
; IN: DX:AX bytes. OUT: AX=sectors (word), CF=1 on overflow.
bytes_to_sectors_ceil:
    push cx
    add ax, 511
    adc dx, 0
    mov cl, 9
    call shr32_dxax_cl
    test dx, dx
    jnz .overflow
    clc
    pop cx
    ret
.overflow:
    stc
    pop cx
    ret

; Convert bytes (DX:AX) to required clusters for destination (ceil by sector & cluster size).
; OUT: AX=clusters (0 allowed for empty files). CF=1 if overflow.
bytes_to_clusters_ceil:
    push bx
    push cx
    ; sectors = (bytes + 511) >> 9
    add ax, 511
    adc dx, 0
    mov cl, 9
    call shr32_dxax_cl
    test dx, dx
    jnz .overflow
    ; clusters = (sectors + (spc-1)) >> spc_shift
    xor bx, bx
    mov bl, [dst_sec_per_clus]
    dec bx
    add ax, bx
    adc dx, 0
    mov cl, [dst_spc_shift]
    call shr32_dxax_cl
    test dx, dx
    jnz .overflow
    clc
    jmp .ret
.overflow:
    stc
.ret:
    pop cx
    pop bx
    ret

; Convert source cluster AX to its LBA (DX:AX) on source disk.
src_cluster_to_lba:
    push cx
    sub ax, 2
    xor dx, dx
    xor cx, cx
    mov cl, [src_sec_per_clus]
    mul cx
    add ax, [src_data_start_lba]
    adc dx, 0
    pop cx
    ret

; Convert destination cluster AX to absolute LBA (DX:AX) on HDD (includes PART_START).
dst_cluster_to_abs_lba:
    push cx
    sub ax, 2
    xor dx, dx
    xor cx, cx
    mov cl, [dst_sec_per_clus]
    mul cx
    add ax, [dst_data_abs_lo]
    adc dx, [dst_data_abs_hi]
    pop cx
    ret

; Write destination FAT buffer to both FAT copies on disk.
write_dst_fat_copies:
    push ax
    push bx
    push cx
    push dx
    push si
    ; base_lba = PART_START + dst_fat_start
    xor dx, dx
    mov ax, [dst_fat_start_lba]
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, 0
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx
    mov byte [tmp_fat_index], 0
.fat_loop:
    mov al, [tmp_fat_index]
    cmp al, 2
    jae .ok
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    cmp byte [tmp_fat_index], 0
    je  .lba_ready
    add ax, [dst_fat_secs]
    adc dx, 0
.lba_ready:
    push ax
    mov ax, DST_FAT_SEG
    mov es, ax
    pop ax
    mov bx, 0
    mov cx, [dst_fat_secs]
.wsec:
    push cx
    mov si, DST_DRIVE
    call write_sector_lba32
    pop cx
    jc .fail
    add bx, SECTOR_SIZE
    add ax, 1
    adc dx, 0
    dec cx
    jnz .wsec
    inc byte [tmp_fat_index]
    jmp .fat_loop
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write destination root directory buffer to disk.
write_dst_root:
    push ax
    push bx
    push cx
    push dx
    push si
    mov ax, DST_ROOT_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    xor dx, dx
    mov ax, [dst_root_start_lba]
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, 0
    mov cx, [dst_root_dir_sectors]
.wloop:
    push cx
    call write_sector_lba32
    pop cx
    jc .fail
    add bx, SECTOR_SIZE
    add ax, 1
    adc dx, 0
    dec cx
    jnz .wloop
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Zero BUF_SEG sector buffer (512 bytes).
zero_buf512:
    push ax
    push cx
    push di
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw
    pop di
    pop cx
    pop ax
    ret

; Write BIN directory data ('.', '..', and copied entries) into allocated dst_bin_cluster chain.
write_dst_bin_dir:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    ; total sectors = dst_bin_clusters * dst_sec_per_clus
    mov ax, [dst_bin_clusters]
    xor dx, dx
    call clusters_mul_spc_to_dxax
    test dx, dx
    jnz .fail
    mov cx, ax
    ; start lba
    mov ax, [dst_bin_cluster]
    call dst_cluster_to_abs_lba
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx
    xor bp, bp                   ; entry_index
.sector_loop:
    cmp cx, 0
    je  .ok
    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    mov bx, 16
.slot_loop:
    cmp bx, 0
    je  .write_sector
    cmp bp, 0
    jne .not_dot
    ; '.'
    mov si, name_dot11
    push di
    push cx
    mov cx, 11
    rep movsb
    pop cx
    pop di
    mov byte [es:di+11], 0x10
    mov ax, [dst_bin_cluster]
    mov [es:di+26], ax
    jmp .slot_done
.not_dot:
    cmp bp, 1
    jne .maybe_file
    ; '..'
    mov si, name_dotdot11
    push di
    push cx
    mov cx, 11
    rep movsb
    pop cx
    pop di
    mov byte [es:di+11], 0x10
    mov word [es:di+26], 0
    jmp .slot_done
.maybe_file:
    mov ax, bp
    sub ax, 2
    mov dl, [bin_file_count]
    xor dh, dh
    cmp ax, dx
    jae .slot_done
    push di
    call write_bin_file_entry     ; AX=file index
    pop di
.slot_done:
    add di, 32
    inc bp
    dec bx
    jmp .slot_loop
.write_sector:
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    call write_sector_lba32
    jc .fail
    inc word [tmp_dst_lba_lo]
    jnz .no_c
    inc word [tmp_dst_lba_hi]
.no_c:
    dec cx
    jmp .sector_loop
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write one BIN file directory entry to ES:DI from file index AX.
; Requires DS=CS, ES=BUF_SEG.
write_bin_file_entry:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov bp, ax                   ; index
    mov dx, di                   ; entry base

    ; copy 11-byte name
    mov ax, bp
    mov bl, 11
    mul bl                        ; AX = index*11
    mov si, bin_file_name
    add si, ax
    mov di, dx
    mov cx, 11
    rep movsb

    ; attr (byte array)
    mov bx, bp
    mov al, [bin_file_attr + bx]
    mov di, dx
    mov [es:di+11], al

    ; word arrays offset = index*2
    shl bx, 1
    mov ax, [bin_file_dst_cluster + bx]
    mov [es:di+26], ax
    mov ax, [bin_file_size_lo + bx]
    mov [es:di+28], ax
    mov ax, [bin_file_size_hi + bx]
    mov [es:di+30], ax

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Copy a source file to destination cluster chain, repacking to destination cluster size.
; IN:
;   CX = src_start_cluster
;   DX = src_size_lo
;   SI = src_size_hi
;   AX = dst_start_cluster
;   BX = dst_clusters
; OUT: CF=1 on error.
copy_file_to_dst:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Nothing to do if no destination clusters.
    test bx, bx
    jz  .ok

    mov [copy_dst_cluster], ax

    ; src_sectors_left = ceil(size/512)
    mov ax, dx
    mov dx, si
    call bytes_to_sectors_ceil
    jc .fail
    mov [copy_src_sectors_left], ax

    mov [copy_src_cluster], cx
    mov word [copy_src_sector_off], 0

    ; dest_total_sectors = dst_clusters * dst_sec_per_clus (must fit in 16-bit for our use)
    mov ax, bx
    xor dx, dx
    call clusters_mul_spc_to_dxax
    test dx, dx
    jnz .fail
    mov bp, ax

    mov ax, [copy_dst_cluster]
    call dst_cluster_to_abs_lba
    mov [copy_dst_lba_lo], ax
    mov [copy_dst_lba_hi], dx

.loop:
    cmp bp, 0
    je  .ok

    mov ax, [copy_src_sectors_left]
    test ax, ax
    jz  .write_zero

    mov ax, [copy_src_cluster]
    cmp ax, 2
    jb  .fail
    cmp ax, FAT12_EOC
    jae .fail
    call src_cluster_to_lba       ; DX:AX = base lba
    mov di, [copy_src_sector_off]
    add ax, di
    adc dx, 0

    ; Read source sector to BUF_SEG
    push ax
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    xor ax, ax
    mov al, [src_drive]
    mov si, ax
    pop ax
    call read_sector_lba32
    jc .fail
    jmp .write_out

.write_zero:
    call zero_buf512

.write_out:
    ; Write to destination sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE
    mov ax, [copy_dst_lba_lo]
    mov dx, [copy_dst_lba_hi]
    call write_sector_lba32
    jc .fail

    inc word [copy_dst_lba_lo]
    jnc .no_carry
    inc word [copy_dst_lba_hi]
.no_carry:

    ; If we consumed a real source sector, advance source position.
    cmp word [copy_src_sectors_left], 0
    je  .src_adv_done
    dec word [copy_src_sectors_left]
    inc word [copy_src_sector_off]
    mov ax, [copy_src_sector_off]
    xor bx, bx
    mov bl, [src_sec_per_clus]
    cmp ax, bx
    jb  .src_adv_done
    mov word [copy_src_sector_off], 0
    cmp word [copy_src_sectors_left], 0
    je  .src_adv_done
    mov ax, [copy_src_cluster]
    call fat12_get_entry
    mov [copy_src_cluster], ax
    cmp ax, 2
    jb  .fail
    cmp ax, FAT12_EOC
    jae .fail
.src_adv_done:

    dec bp
    jmp .loop

.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --------------------------
; Data / constants
; --------------------------

LINE_BUF_MAX equ 8
line_buf times LINE_BUF_MAX db 0

; BIOS geometry
src_drive db INSTALL_DRIVE
src_spt   dw 0
src_heads dw 0
src_cyls  dw 0
dst_spt   dw 0
dst_heads dw 0
dst_cyls  dw 0

; HDD total sectors (physical)
dst_secs_lo dw 0
dst_secs_hi dw 0

; src BPB/layout
src_sec_per_clus db 0
src_spc_shift    db 0
src_reserved     dw 0
src_fats         db 0
src_root_ents    dw 0
src_fat_secs     dw 0
src_total_lo     dw 0
src_total_hi     dw 0
src_root_dir_sectors dw 0
src_fat_start_lba  dw 0
src_root_start_lba dw 0
src_data_start_lba dw 0
src_cluster_count  dw 0

; dst BPB/layout (relative to partition start)
dst_sec_per_clus db 0
dst_spc_shift    db 0
dst_reserved     dw 0
dst_fats         db 0
dst_root_ents    dw 0
dst_root_dir_sectors dw 0
dst_fat_secs     dw 0
dst_total_lo     dw 0
dst_total_hi     dw 0
dst_fat_start_lba  dw 0
dst_root_start_lba dw 0
dst_data_start_lba dw 0
dst_max_cluster    dw 0

; temp layout vars
avail_lo dw 0
avail_hi dw 0
tmp_total_lo dw 0
tmp_total_hi dw 0
tmp_clusters dw 0
tmp_fat_secs dw 0

; temp LBA vars
tmp_drive   db 0
tmp_fat_index db 0
tmp_lba_lo  dw 0
tmp_lba_hi  dw 0

tmp_src_lba_lo dw 0
tmp_src_lba_hi dw 0
tmp_dst_lba_lo dw 0
tmp_dst_lba_hi dw 0

dst_data_abs_lo dw 0
dst_data_abs_hi dw 0

; ---- Installer filesystem install state ----
; 11-byte 8.3 names (NAME[8] + EXT[3])
name_kernel11  db 'KERNEL  BIN'
name_env11     db 'XENV    ENV'
name_bin11     db 'BIN        '
name_dot11     db '.          '
name_dotdot11  db '..         '

; Source file info (from swapped Seolsem floppy)
src_kernel_cluster dw 0
src_kernel_size_lo dw 0
src_kernel_size_hi dw 0
src_kernel_attr    db 0
src_env_cluster    dw 0
src_env_size_lo    dw 0
src_env_size_hi    dw 0
src_env_attr       db 0
src_bin_cluster    dw 0

; Destination allocations
dst_next_cluster    dw 0
dst_bin_cluster     dw 0
dst_bin_clusters    dw 0
dst_bin_dir_size_lo dw 0
dst_bin_dir_size_hi dw 0
dst_kernel_cluster  dw 0
dst_kernel_clusters dw 0
dst_env_cluster     dw 0
dst_env_clusters    dw 0

; BIN directory entries
bin_file_count      db 0
bin_entry_count     dw 0
bin_file_name       times (BIN_MAX_FILES*11) db 0
bin_file_attr       times BIN_MAX_FILES db 0
bin_file_src_cluster times BIN_MAX_FILES dw 0
bin_file_size_lo    times BIN_MAX_FILES dw 0
bin_file_size_hi    times BIN_MAX_FILES dw 0
bin_file_dst_cluster times BIN_MAX_FILES dw 0
bin_file_dst_clusters times BIN_MAX_FILES dw 0

; File copy state
copy_src_cluster      dw 0
copy_src_sector_off   dw 0
copy_src_sectors_left dw 0
copy_dst_cluster      dw 0
copy_dst_lba_lo       dw 0
copy_dst_lba_hi       dw 0

; Temp for directory parsing
tmp_parse_cluster dw 0

; Disk error debug fields
last_op    db 0
last_drive db 0
last_lba_lo dw 0
last_lba_hi dw 0
last_ah    db 0
last_method db 0
last_ch    db 0
last_cl    db 0
last_dh    db 0

; EDD parameters buffer (0x1A bytes is enough for total sectors at +0x10)
edd_params times 0x1A db 0
dst_has_edd db 0
src_has_edd db 0

; EDD Disk Address Packet (DAP) for AH=42h/43h (16 bytes)
;  00: size(0x10), 01: reserved
;  02: sectors (word)
;  04: buffer offset (word)
;  06: buffer segment (word)
;  08: LBA low dword
;  12: LBA high dword
calc_dst_layout_fat32:
    ; Partition sectors = (disk total sectors) - PART_START_LBA
    ; Stored as 32-bit in part_secs_hi:part_secs_lo
    mov eax, dword [dst_secs_lo]
    sub eax, PART_START_LBA
    mov dword [part_secs_lo], eax

    ; FAT32 parameters (small but valid FAT32 volume)
    ; - reserved sectors: 32
    ; - sectors/cluster: choose so CountOfClusters >= 65525 (FAT32 per MS spec Section 3.5)
    mov word [dst_res], 32
    mov byte [dst_spc], 32

.choose_spc:
    ; dst_spc_shift = log2(dst_spc) (dst_spc is power-of-two)
    xor ax, ax
    mov al, [dst_spc]
    mov bl, 0
.spc_loop:
    cmp al, 1
    je  .spc_done
    shr al, 1
    inc bl
    jmp .spc_loop
.spc_done:
    mov [dst_spc_shift], bl

    ; Iteratively solve FAT size:
    ;   data = total - reserved - fats*fat_secs
    ;   clusters = data / spc
    ;   fat_bytes = (clusters + 2) * 4
    ;   fat_secs = ceil(fat_bytes / 512)
    mov dword [dst_fat_secs_lo], 1

.iter:
    mov eax, dword [part_secs_lo]

    movzx ecx, word [dst_res]
    sub eax, ecx

    mov edx, dword [dst_fat_secs_lo]
    shl edx, 1                     ; fats = 2
    sub eax, edx
    jc .fail

    ; clusters = data >> dst_spc_shift
    mov ebx, eax
    mov cl, [dst_spc_shift]
    shr ebx, cl

    ; fat_secs_needed = ceil(((clusters+2)*4) / 512)
    mov eax, ebx
    add eax, 2
    shl eax, 2                     ; *4
    add eax, 511
    shr eax, 9

    cmp eax, dword [dst_fat_secs_lo]
    je  .fat_ok
    mov dword [dst_fat_secs_lo], eax
    jmp .iter

.fat_ok:
    ; Ensure the cluster count is safely in the FAT32 range.
    ; MS spec recommends avoiding boundary values by >=16 clusters.
    cmp ebx, (65525 + 16)
    jae .ok

    ; Cluster count is too small -> reduce SecPerClus and retry.
    mov al, [dst_spc]
    cmp al, 1
    je  .fail
    shr al, 1
    mov [dst_spc], al
    jmp .choose_spc

.ok:
    clc
    ret
.fail:
    stc
    ret

create_fat32_vbr:
    push ds
    push cs
    pop ds
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    
    ; Copy boot32 code and BPB template
    mov si, fat32_boot_code
    mov di, 0
    mov cx, 512
    rep movsb

    ; (Skip manual OEM copy as it is in the template, or overwrite if needed)
    ; But our BPB patching below will update fields.
    ; Ensure JMP is correct in template (it is).

    
    ; OEM name (8 bytes)
    mov si, oem_name
    mov di, 0x03
    mov cx, 8
    rep movsb

    ; BPB
    mov word [es:0x0B], 512        ; BytesPerSec
    mov al, [dst_spc]
    mov byte [es:0x0D], al         ; SecPerClus
    mov ax, [dst_res]
    mov word [es:0x0E], ax         ; RsvdSecCnt
    mov byte [es:0x10], 2          ; NumFATs
    mov word [es:0x11], 0          ; RootEntCnt (0 for FAT32)
    mov word [es:0x13], 0          ; TotSec16
    mov byte [es:0x15], 0xF8       ; Media
    mov word [es:0x16], 0          ; FATSz16
    mov ax, [dst_spt]
    mov word [es:0x18], ax         ; SecPerTrk
    mov ax, [dst_heads]
    mov word [es:0x1A], ax         ; NumHeads
    mov word [es:0x1C], (PART_START_LBA & 0xFFFF) ; HiddenSec
    mov word [es:0x1E], (PART_START_LBA >> 16)
    
    mov ax, [part_secs_lo]
    mov dx, [part_secs_hi]
    mov word [es:0x20], ax         ; TotSec32
    mov word [es:0x22], dx
    
    mov ax, [dst_fat_secs_lo]
    mov dx, [dst_fat_secs_hi]
    mov word [es:0x24], ax         ; FATSz32
    mov word [es:0x26], dx
    
    mov word [es:0x28], 0          ; ExtFlags
    mov word [es:0x2A], 0          ; FSVer
    mov dword [es:0x2C], ROOT_CLUSTER ; RootClus (2)
    mov word [es:0x30], 1          ; FSInfo (Sector 1)
    mov word [es:0x32], 0          ; BkBootSec (0 = no backup boot sector)

    ; Extended BPB (FAT32) fields
    mov byte [es:0x40], 0x80       ; Drive number
    mov byte [es:0x41], 0x00
    mov byte [es:0x42], 0x29       ; Boot signature
    mov dword [es:0x43], 0x12345678
    mov si, vol_label
    mov di, 0x47
    mov cx, 11
    rep movsb
    mov si, fs_type32
    mov di, 0x52
    mov cx, 8
    rep movsb
    
    ; Sig
    mov word [es:510], 0xAA55
    pop ds
    ret

create_fs_info:
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw
    
    mov dword [es:0], 0x41615252   ; LeadSig
    mov dword [es:484], 0x61417272 ; StrucSig
    mov dword [es:488], 0xFFFFFFFF ; Free_Count
    mov dword [es:492], 0xFFFFFFFF ; Nxt_Free
    mov word [es:510], 0xAA55
    ret

write_mbr_fat32:
    push ds
    push si
    push di
    push cx
    push ax
    push dx

    ; Copy MBR boot code template to BUF_SEG
    push cs
    pop ds
    mov si, mbr_template
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    mov cx, 512
    rep movsb

    ; Patch partition entry #1 at 0x1BE
    mov di, 0x1BE
    mov byte [es:di+0], 0x80       ; active
    mov byte [es:di+4], 0x0C       ; type: FAT32 LBA
    mov word [es:di+8],  (PART_START_LBA & 0xFFFF)
    mov word [es:di+10], (PART_START_LBA >> 16)
    mov ax, [part_secs_lo]
    mov [es:di+12], ax
    mov ax, [part_secs_hi]
    mov [es:di+14], ax

    ; Signature (should already be present in template, but enforce it)
    mov word [es:510], 0xAA55

    ; Write to HDD LBA0
    xor ax, ax
    xor dx, dx
    xor bx, bx
    mov si, DST_DRIVE
    call write_sector_lba32

    pop dx
    pop ax
    pop cx
    pop di
    pop si
    pop ds
    ret

clear_fat32_tables:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Zero buffer in BUF_SEG
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw

    ; Clear both FAT copies (FAT size = dst_fat_secs_lo sectors)
    mov byte [tmp_fat_index], 0
.fat_loop:
    mov al, [tmp_fat_index]
    cmp al, 2
    jae .init_reserved

    ; base_lba = PART_START + dst_res + (fat_index * dst_fat_secs)
    mov ax, (PART_START_LBA & 0xFFFF)
    mov dx, (PART_START_LBA >> 16)
    add ax, [dst_res]
    adc dx, 0
    cmp byte [tmp_fat_index], 0
    je  .base_ok
    add ax, [dst_fat_secs_lo]
    adc dx, [dst_fat_secs_hi]
.base_ok:
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx

    mov cx, [dst_fat_secs_lo]
.clear_loop:
    push cx
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    pop cx
    jc .fail

    inc word [tmp_lba_lo]
    jnz .no_carry
    inc word [tmp_lba_hi]
.no_carry:
    loop .clear_loop

    inc byte [tmp_fat_index]
    jmp .fat_loop

.init_reserved:
    ; Initialize Cluster 0, 1, 2 in the first FAT sector and write to both FAT copies.
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw
    mov dword [es:0], 0x0FFFFFF8
    mov dword [es:4], 0x0FFFFFFF
    mov dword [es:8], 0x0FFFFFFF

    ; FAT1 first sector
    mov ax, (PART_START_LBA & 0xFFFF)
    mov dx, (PART_START_LBA >> 16)
    add ax, [dst_res]
    adc dx, 0
    mov si, DST_DRIVE
    xor bx, bx
    call write_sector_lba32
    jc .fail

    ; FAT2 first sector
    add ax, [dst_fat_secs_lo]
    adc dx, [dst_fat_secs_hi]
    mov si, DST_DRIVE
    xor bx, bx
    call write_sector_lba32
    jc .fail

    clc
    jmp .ret
.fail:
    stc
.ret:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

init_root_cluster:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; DataStart (absolute LBA) = PART_START + dst_res + (2 * dst_fat_secs)
    mov ax, [dst_fat_secs_lo]
    mov dx, [dst_fat_secs_hi]
    shl ax, 1
    rcl dx, 1
    add ax, [dst_res]
    adc dx, 0
    add ax, (PART_START_LBA & 0xFFFF)
    adc dx, (PART_START_LBA >> 16)

    ; Cache for cluster->LBA conversion helpers (cluster 2 starts here)
    mov [dst_data_abs_lo], ax
    mov [dst_data_abs_hi], dx

    ; Current LBA for clearing loop
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx

    ; Zero buffer once
    mov ax, BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw

    ; Clear root directory cluster (cluster 2): dst_spc sectors at DataStart
    xor cx, cx
    mov cl, [dst_spc]
.clean_root:
    push cx
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    pop cx
    jc .fail

    inc word [tmp_lba_lo]
    jnz .no_carry
    inc word [tmp_lba_hi]
.no_carry:
    loop .clean_root

    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
    
install_fs_fat32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    push cs
    pop ds

    ; Sync destination cluster size helpers
    mov al, [dst_spc]
    mov [dst_sec_per_clus], al

    ; Next free cluster starts after root cluster (2)
    mov word [dst_next_cluster], 3

    ; ---- Find required files on source disk (FAT12 root) ----
    mov si, name_kernel11
    call find_root_entry
    jc .missing_kernel
    mov ax, [es:di+26]
    mov [src_kernel_cluster], ax
    mov ax, [es:di+28]
    mov [src_kernel_size_lo], ax
    mov ax, [es:di+30]
    mov [src_kernel_size_hi], ax

    mov si, name_env11
    call find_root_entry
    jc .missing_env
    mov ax, [es:di+26]
    mov [src_env_cluster], ax
    mov ax, [es:di+28]
    mov [src_env_size_lo], ax
    mov ax, [es:di+30]
    mov [src_env_size_hi], ax

    mov si, name_bin11
    call find_root_entry
    jc .missing_bin
    mov ax, [es:di+26]
    mov [src_bin_cluster], ax

    call parse_src_bin_dir
    jc .fail

    ; ---- Allocate BIN directory cluster (one cluster is enough for up to 64 entries) ----
    mov ax, 1
    call fat32_alloc_chain
    jc .fail
    mov [dst_bin_cluster], ax
    mov word [dst_bin_clusters], 1

    ; ---- Allocate/copy KERNEL.BIN ----
    mov ax, [src_kernel_size_lo]
    mov dx, [src_kernel_size_hi]
    call bytes_to_clusters_ceil
    jc .fail
    mov [dst_kernel_clusters], ax
    call fat32_alloc_chain
    jc .fail
    mov [dst_kernel_cluster], ax

    mov cx, [src_kernel_cluster]
    mov dx, [src_kernel_size_lo]
    mov si, [src_kernel_size_hi]
    mov ax, [dst_kernel_cluster]
    mov bx, [dst_kernel_clusters]
    call copy_file_to_dst
    jc .fail

    ; ---- Allocate/copy XENV.ENV ----
    mov ax, [src_env_size_lo]
    mov dx, [src_env_size_hi]
    call bytes_to_clusters_ceil
    jc .fail
    mov [dst_env_clusters], ax
    call fat32_alloc_chain
    jc .fail
    mov [dst_env_cluster], ax

    mov cx, [src_env_cluster]
    mov dx, [src_env_size_lo]
    mov si, [src_env_size_hi]
    mov ax, [dst_env_cluster]
    mov bx, [dst_env_clusters]
    call copy_file_to_dst
    jc .fail

    ; ---- Allocate/copy BIN/* files ----
    xor bp, bp
.bin_loop:
    mov al, [bin_file_count]
    xor ah, ah
    cmp bp, ax
    jae .bin_done

    mov di, bp
    shl di, 1

    mov cx, [bin_file_src_cluster + di]
    mov ax, [bin_file_size_lo + di]
    mov dx, [bin_file_size_hi + di]
    call bytes_to_clusters_ceil
    jc .fail
    mov [bin_file_dst_clusters + di], ax

    mov bx, ax                  ; dst_clusters
    mov ax, bx
    call fat32_alloc_chain
    jc .fail
    mov [bin_file_dst_cluster + di], ax

    mov dx, [bin_file_size_lo + di]
    mov si, [bin_file_size_hi + di]
    mov ax, [bin_file_dst_cluster + di]
    mov bx, [bin_file_dst_clusters + di]
    call copy_file_to_dst
    jc .fail

    inc bp
    jmp .bin_loop

.bin_done:
    ; ---- Write BIN directory entries ----
    call write_dst_bin_dir
    jc .fail

    ; ---- Write FAT32 root directory (cluster 2) ----
    call write_root_dir_cluster_fat32
    jc .fail

    clc
    jmp .ret

.missing_kernel:
    mov si, msg_missing_kernel
    call print_string
    stc
    jmp .ret
.missing_env:
    mov si, msg_missing_env
    call print_string
    stc
    jmp .ret
.missing_bin:
    mov si, msg_missing_bin
    call print_string
    stc
    jmp .ret
.fail:
    stc
.ret:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --------------------------
; FAT32 helpers (write minimal FAT chains to both FAT copies)
; --------------------------

; Set one FAT32 entry in both FAT copies.
; IN: AX=cluster, BX=value_lo, DX=value_hi
; OUT: CF=1 on error.
fat32_set_entry:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
    push ds

    push cs
    pop ds

    mov [fat32_tmp_cluster], ax
    mov [fat32_tmp_val_lo], bx
    mov [fat32_tmp_val_hi], dx

    ; sector_offset = cluster / 128
    mov ax, [fat32_tmp_cluster]
    mov cx, ax
    shr cx, 7

    ; byte_offset = (cluster % 128) * 4
    mov ax, [fat32_tmp_cluster]
    and ax, 0x007F
    shl ax, 2
    mov [fat32_tmp_byte_off], ax

    ; FAT1 sector LBA = PART_START + dst_res + sector_offset
    mov ax, (PART_START_LBA & 0xFFFF)
    mov dx, (PART_START_LBA >> 16)
    add ax, [dst_res]
    adc dx, 0
    add ax, cx
    adc dx, 0
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx

    ; Read FAT1 sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call read_sector_lba32
    jc .fail

    ; Patch dword entry (preserve upper 4 bits)
    mov ax, BUF_SEG
    mov es, ax
    mov bx, [fat32_tmp_byte_off]

    movzx eax, word [fat32_tmp_val_lo]
    movzx edx, word [fat32_tmp_val_hi]
    shl edx, 16
    or eax, edx
    and eax, 0x0FFFFFFF

    mov ecx, dword [es:bx]
    and ecx, 0xF0000000
    or eax, ecx
    mov dword [es:bx], eax

    ; Write FAT1 sector back
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    ; Write FAT2 sector (same sector_offset)
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    add ax, [dst_fat_secs_lo]
    adc dx, [dst_fat_secs_hi]
    mov si, DST_DRIVE
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    call write_sector_lba32
    jc .fail

    clc
    jmp .ret
.fail:
    stc
.ret:
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Allocate a contiguous cluster chain and write FAT32 entries.
; IN:  AX = cluster count (0 allowed)
; OUT: AX = first cluster (0 if count==0), CF=1 on failure.
fat32_alloc_chain:
    push bx
    push cx
    push dx
    push si
    push di

    mov cx, ax
    test cx, cx
    jnz .nonzero
    xor ax, ax
    clc
    jmp .ret
.nonzero:
    mov di, [dst_next_cluster]      ; first
    mov si, di                      ; current
.loop:
    dec cx
    jz  .last
    mov ax, si
    mov bx, si
    inc bx
    xor dx, dx
    call fat32_set_entry
    jc .fail
    inc si
    jmp .loop
.last:
    mov ax, si
    mov bx, 0xFFFF
    mov dx, 0x0FFF
    call fat32_set_entry
    jc .fail

    mov ax, si
    inc ax
    mov [dst_next_cluster], ax
    mov ax, di
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Write FAT32 root directory entries to root cluster (cluster 2).
; Requires dst_data_abs_lo/hi to point at cluster 2 start LBA.
write_root_dir_cluster_fat32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds

    push cs
    pop ds

    call zero_buf512               ; ES=BUF_SEG
    mov ax, BUF_SEG
    mov es, ax

    ; Entry 0: KERNEL.BIN
    mov di, 0
    mov si, name_kernel11
    mov cx, 11
    rep movsb
    mov byte [es:0+11], 0x20
    mov word [es:0+20], 0
    mov ax, [dst_kernel_cluster]
    mov word [es:0+26], ax
    mov ax, [src_kernel_size_lo]
    mov word [es:0+28], ax
    mov ax, [src_kernel_size_hi]
    mov word [es:0+30], ax

    ; Entry 1: XENV.ENV
    mov di, 32
    mov si, name_env11
    mov cx, 11
    rep movsb
    mov byte [es:32+11], 0x20
    mov word [es:32+20], 0
    mov ax, [dst_env_cluster]
    mov word [es:32+26], ax
    mov ax, [src_env_size_lo]
    mov word [es:32+28], ax
    mov ax, [src_env_size_hi]
    mov word [es:32+30], ax

    ; Entry 2: BIN (directory)
    mov di, 64
    mov si, name_bin11
    mov cx, 11
    rep movsb
    mov byte [es:64+11], 0x10
    mov word [es:64+20], 0
    mov ax, [dst_bin_cluster]
    mov word [es:64+26], ax

    ; Write first sector of root cluster (rest is already zeroed by init_root_cluster)
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [dst_data_abs_lo]
    mov dx, [dst_data_abs_hi]
    mov si, DST_DRIVE
    call write_sector_lba32

    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

edd_dap: ; Marker to identify location
    db 0x10, 0x00
    dw 0x0000
    dw 0x0000
    dd 0x00000000
    dd 0x00000000

; FAT32 specific variables removed (moved to main data area)

spc_table   db 1,2,4,8,16,32,64
shift_table db 0,1,2,3,4,5,6

; MBR template binary (built as build/mbr.bin by Makefile)
mbr_template:
    incbin "build/mbr.bin"

; Messages
msg_banner       db 'Seolsem Installer (FAT32 Support)',13,10,0
msg_hdd_sectors  db 'HDD Sectors: ',0
msg_confirm1     db 'Install to HDD? (Y/N): ',0
msg_confirm2     db 'Type YES to confirm data loss: ',0
msg_swap         db 'Swap to Seolsem Disk and press Key...',0
msg_copy_reserved db 'Copying system...',13,10,0
msg_install_fs   db 'Formatting FAT32 & Copying...',13,10,0
msg_done         db 'Success. Reboot now.',0
msg_cancel       db 'Aborted.',0
msg_cap          db 'Disk larger than FAT12 max; using first part only.',0
msg_disk_error   db 'Disk Error!',0
msg_disk_detail  db ' op/drive/lba/ah: ',0
msg_bad_bpb      db 'Bad Source BPB!',0
msg_layout_fail  db 'Layout Calc Fail!',0
msg_missing_kernel db 'Missing KERNEL.BIN on source disk.',13,10,0
msg_missing_env    db 'Missing XENV.ENV on source disk.',13,10,0
msg_missing_bin    db 'Missing BIN directory on source disk.',13,10,0
msg_install_fail db 'Install failed.',13,10,0

; FAT32 specific variables (moved to end)
part_secs_lo dw 0
part_secs_hi dw 0
dst_spc db 0
dst_res dw 0
dst_fat_secs_lo dw 0
dst_fat_secs_hi dw 0
fat32_tmp_cluster dw 0
fat32_tmp_val_lo dw 0
fat32_tmp_val_hi dw 0
fat32_tmp_byte_off dw 0

; Copy Stage 2 (Reserved Sectors) from Source Floppy to Destination Partition
copy_stage2_to_hdd:
    mov cx, [src_reserved]
    dec cx
    cmp cx, 0
    jz .done

    ; Source Start: LBA 1
    mov word [tmp_src_lba_lo], 1
    mov word [tmp_src_lba_hi], 0

    ; Dest Start: PART_START + 2
    mov ax, (PART_START_LBA & 0xFFFF)
    add ax, 2
    mov dx, (PART_START_LBA >> 16)
    adc dx, 0
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx

.loop:
    push cx
    
    ; Read from Source
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    xor cx, cx
    mov cl, [src_drive]
    mov si, cx
    mov ax, [tmp_src_lba_lo]
    mov dx, [tmp_src_lba_hi]
    call read_sector_lba32
    jc .fail

    ; Write to Dest
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov si, DST_DRIVE    ; HDD
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    call write_sector_lba32
    jc .fail

    inc word [tmp_src_lba_lo]
    
    inc word [tmp_dst_lba_lo]
    jnz .no_c
    inc word [tmp_dst_lba_hi]
.no_c:

    pop cx
    loop .loop

.done:
    call print_crlf
    clc
    ret
.fail:
    pop cx
    stc
    ret

msg_dot db '.', 0

oem_name db 'MSWIN4.1', 0 ; FAT32 compatibility
vol_label db 'SEOLSEM    ' ; 11 bytes
fs_type32 db 'FAT32   '    ; 8 bytes

fat32_boot_code:
    incbin "build/boot32.bin"
