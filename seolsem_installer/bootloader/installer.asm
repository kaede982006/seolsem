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
%define SRC_BOOT_MAGIC_OFS 0x01F0
%define SRC_BOOT_MAGIC_LEN 8

jmp STAGE2_SEG:start

; UI/TUI helpers (VGA text mode)
%include "bootloader/ui.inc"

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

    call ui_init

    ; ---- Confirmation UI (TUI) ----
    call ui_confirm_install
    jc .cancel

    ; Optional: collect user list to pre-create /ETC/PASSWD and /HOME entries.
    call prompt_users

    ; ---- Progress UI ----
    call ui_draw_base
    xor ax, ax
    mov [ui_done_mask], ax
    mov byte [ui_cur_step], 0
    mov bx, [ui_done_mask]
    mov al, 0
    call ui_steps_render
    mov si, ui_status_detect
    call ui_status
    xor al, al
    call ui_progress

    mov si, ui_log_start
    call ui_log_push

    ; ---- Get HDD geometry ----
    mov si, ui_log_hdd_geom
    call ui_log_push
    mov dl, DST_DRIVE
    call get_geometry
    jc fatal_disk
    mov [dst_spt], ax
    mov [dst_heads], bx
    mov [dst_cyls], cx

    ; ---- Detect HDD total sectors (EDD preferred) ----
    mov si, ui_log_hdd_size
    call ui_log_push
    mov dl, DST_DRIVE
    call get_total_sectors_32
    jc  .no_edd
    mov byte [dst_has_edd], 1
    jmp .got_sectors
.no_edd:
    mov byte [dst_has_edd], 0
    call calc_total_sectors_chs_32
.got_sectors:

    ; ---- Source disk detection ----
    mov si, ui_log_src_try_b
    call ui_log_push
    ; Two-floppy setup: if B: already has the Seolsem system disk, skip swap prompt.
    mov byte [src_drive], SRC_DRIVE_B
    call try_prepare_source_disk
    jnc .src_ready

    ; One-floppy setup: try up to 4 additional attempts on A: (total tries = 5).
    mov byte [src_retry_left], 4
.retry_src:
    ; Only show retry message if this is not the first attempt
    cmp byte [src_retry_left], 4
    je  .skip_retry_msg
    mov si, ui_log_src_retry
    call ui_log_push
.skip_retry_msg:
    call ui_swap_prompt

    mov byte [src_drive], INSTALL_DRIVE
    ; Reset A: after swap
    push ds
    xor ax, ax
    mov dl, INSTALL_DRIVE
    int 0x13
    pop ds

    call try_prepare_source_disk
    jnc .src_ready

    dec byte [src_retry_left]
    jnz .retry_src
    jmp fatal_source
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
    mov si, ui_log_src_geom
    call ui_log_push
    mov dl, [src_drive]
    call get_geometry
    jc fatal_disk
    mov [src_spt], ax
    mov [src_heads], bx
    mov [src_cyls], cx

    ; ---- Source boot sector is already in BUF_SEG ----
    mov ax, BUF_SEG
    mov es, ax

    mov si, ui_log_parse_bpb
    call ui_log_push
    call parse_src_bpb
    jc fatal_bpb

    ; Safety guard: HDD VBR (boot32.bin) loads stage2 in a fixed 4-sector window.
    ; Source FAT12 image encodes stage2 size as (ReservedSectors - 1).
    ; If stage2 exceeds 4 sectors, installation would appear to succeed but HDD boot will hang.
    mov ax, [src_reserved]
    cmp ax, (1 + 4)
    jbe .stage2_ok
    jmp fatal_stage2
.stage2_ok:
    mov si, ui_log_calc_src
    call ui_log_push
    call calc_src_layout
    jc fatal_bpb

    ; Validate source media before any destructive write to HDD.
    mov si, ui_log_validate_src
    call ui_log_push
    call verify_source_seolsem_image
    jc fatal_source

    ; ---- Compute destination (partition) layout (FAT32) ----
    mov si, ui_log_calc_dst
    call ui_log_push
    call calc_dst_layout_fat32
    jc fatal_layout
    or word [ui_done_mask], (1 << 0)

    ; ---- Write MBR (LBA0) ----
    mov byte [ui_cur_step], 1
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_partition
    call ui_status
    mov al, 20
    call ui_progress
    mov si, ui_log_write_mbr
    call ui_log_push
    call write_mbr_fat32
    jc fatal_disk
    or word [ui_done_mask], (1 << 1)

    ; ---- Create FAT32 VBR ----
    ; We construct a new FAT32 BPB in memory
    mov byte [ui_cur_step], 2
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_format
    call ui_status
    mov al, 35
    call ui_progress
    mov si, ui_log_write_vbr
    call ui_log_push
    call create_fat32_vbr
    
    ; Write VBR to partition start
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov dx, word (PART_START_LBA >> 16)
    mov ax, word (PART_START_LBA & 0xFFFF)
    mov si, DST_DRIVE
    call write_sector_lba32
    jc fatal_disk

    ; Write FS Info Sector (Sector 1 relative to part start)
    mov si, ui_log_write_fsinfo
    call ui_log_push
    call create_fs_info
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov dx, word (PART_START_LBA >> 16)
    mov ax, word (PART_START_LBA & 0xFFFF)
    add ax, 1
    adc dx, 0
    mov si, DST_DRIVE
    call write_sector_lba32
    jc fatal_disk

    ; ---- Clear FAT tables ----
    mov si, ui_log_clear_fat
    call ui_log_push
    call clear_fat32_tables
    jc fatal_disk

    ; ---- Initialize Root Directory Cluster (Cluster 2) ----
    mov si, ui_log_init_root
    call ui_log_push
    call init_root_cluster
    jc fatal_disk
    or word [ui_done_mask], (1 << 2)

    ; ---- Copy reserved sectors (stage2) ----
    mov byte [ui_cur_step], 3
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_copy_boot
    call ui_status
    mov al, 45
    call ui_progress
    mov si, ui_log_copy_boot
    call ui_log_push
    call copy_stage2_to_hdd
    jc fatal_disk
    or word [ui_done_mask], (1 << 3)

    ; ---- Load source FAT and root directory (for file copying) ----
    mov byte [ui_cur_step], 4
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_copy_files
    call ui_status
    mov al, 55
    call ui_progress
    mov si, ui_log_load_src
    call ui_log_push
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
    mov si, ui_log_install
    call ui_log_push
    call install_fs_fat32
    jc fatal_install

    mov si, ui_log_done
    call ui_log_push
    jmp halt_forever

.cancel:
    push cs
    pop ds
    call ui_draw_base
    xor bx, bx
    mov al, 0
    call ui_steps_render
    mov si, ui_status_cancel
    call ui_status
    xor al, al
    call ui_progress
    mov si, ui_log_cancel
    call ui_log_push
    jmp halt_forever

; --------------------------
; Fatal handlers
; --------------------------
fatal_disk:
    push cs
    pop ds
    mov si, ui_err_disk
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_disk
    call ui_log_push
    jmp halt_forever

fatal_bpb:
    push cs
    pop ds
    mov si, ui_err_bpb
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_bpb
    call ui_log_push
    jmp halt_forever

fatal_source:
    push cs
    pop ds
    mov si, ui_err_source
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_source
    call ui_log_push
    jmp halt_forever

fatal_stage2:
    push cs
    pop ds
    mov si, ui_err_stage2
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_stage2
    call ui_log_push
    jmp halt_forever

fatal_layout:
    push cs
    pop ds
    mov si, ui_err_layout
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_layout
    call ui_log_push
    jmp halt_forever

fatal_install:
    push cs
    pop ds
    mov si, ui_err_install
    call ui_status
    mov al, 0
    call ui_progress
    mov si, ui_err_install
    call ui_log_push
    jmp halt_forever

halt_forever:
    push cs
    pop ds
    mov si, ui_status_reboot
    call ui_status
    mov al, 100
    call ui_progress
    xor ax, ax
    int 0x16        ; Wait for key
    int 0x19        ; Warm boot (DL should be drive? 19h usually reloads form boot drive)
    jmp $


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

; --------------------------
; TUI: dialogs/helpers
; --------------------------

; CF=0 confirmed, CF=1 cancelled/failed.
ui_confirm_install:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    push cs
    pop ds

    call ui_draw_base
    xor bx, bx
    mov al, 0
    call ui_steps_render
    mov si, ui_status_confirm
    call ui_status
    xor al, al
    call ui_progress

    mov si, ui_log_confirm
    call ui_log_push

    ; Dialog box
    mov dh, 6
    mov dl, 18
    mov ch, 17
    mov cl, 61
    mov bh, UI_ATTR_BORDER
    mov bl, UI_ATTR_BG
    call ui_draw_box

    mov dh, 7
    mov dl, 26
    mov bl, UI_ATTR_BORDER
    mov si, ui_confirm_title
    call ui_puts

    mov dh, 9
    mov dl, 20
    mov bl, UI_ATTR_TEXT
    mov si, ui_confirm_line1
    call ui_puts
    mov dh, 10
    mov dl, 20
    mov si, ui_confirm_line2
    call ui_puts

    mov dh, 13
    mov dl, 20
    mov si, ui_confirm_q
    call ui_puts

.wait_yn:
    call read_key
    cmp al, 27          ; ESC
    je  .cancel
    cmp al, 'N'
    je  .cancel
    cmp al, 'n'
    je  .cancel
    cmp al, 'Y'
    je  .type_yes
    cmp al, 'y'
    je  .type_yes
    jmp .wait_yn

.type_yes:
    mov si, ui_status_type_yes
    call ui_status
    xor al, al
    call ui_progress

    mov dh, 15
    mov dl, 20
    mov bl, UI_ATTR_TEXT
    mov si, ui_confirm_type
    call ui_puts

    ; Clear input field
    mov dh, 16
    mov dl, 30
    mov cl, 10
    mov bl, UI_ATTR_BG
    call ui_fill_row

    ; Read YES into line_buf at (16,30)
    mov dh, 16
    mov dl, 30
    mov bl, UI_ATTR_TEXT
    call read_line_upper

    ; Accept "YES" (case-insensitive)
    mov si, line_buf
    mov al, [si+0]
    cmp al, 'Y'
    je  .y_ok
    cmp al, 'y'
    jne .bad_yes
.y_ok:
    mov al, [si+1]
    cmp al, 'E'
    je  .e_ok
    cmp al, 'e'
    jne .bad_yes
.e_ok:
    mov al, [si+2]
    cmp al, 'S'
    je  .s_ok
    cmp al, 's'
    jne .bad_yes
.s_ok:
    cmp byte [si+3], 0
    jne .bad_yes

    clc
    jmp .ret

.bad_yes:
    mov si, ui_status_bad_yes
    call ui_status
    xor al, al
    call ui_progress
    stc
    jmp .ret

.cancel:
    stc
.ret:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Wait for swap confirmation (one-floppy).
ui_swap_prompt:
    push ax
    push si
    push ds
    push cs
    pop ds
    mov si, ui_log_swap
    call ui_log_push
    mov si, ui_status_swap
    call ui_status
    xor al, al
    call ui_progress
    call read_key
    pop ds
    pop si
    pop ax
    ret

; Render list of currently added users (inside the user dialog).
ui_render_user_list:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    push cs
    pop ds

    ; Clear list area (rows 11..14)
    mov dh, 11
.clr:
    mov dl, 18
    mov cl, 44
    mov bl, UI_ATTR_BG
    call ui_fill_row
    inc dh
    cmp dh, 14
    jbe .clr

    mov dh, 11
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_list
    call ui_puts

    xor di, di
.ul_loop:
    mov al, [inst_user_count]
    xor ah, ah
    cmp di, ax
    jae .done

    mov ax, di
    mov dl, LINE_BUF_MAX
    mul dl                      ; AX = index * LINE_BUF_MAX
    mov si, inst_user_names
    add si, ax

    mov dh, 12
    mov ax, di
    add dh, al
    mov dl, 20
    mov bl, UI_ATTR_TEXT
    call ui_puts

    inc di
    jmp .ul_loop

.done:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Read a short line. Ends on Enter. Backspace supported.
; Result stored to line_buf and NUL-terminated.
; IN:  DH=row, DL=col, BL=attr (UI)
read_line_upper:
    push ax
    push bx
    push cx
    push dx
    push di
    mov di, line_buf
    mov cx, 0
    mov [ui_in_row], dh
    mov [ui_in_col], dl
    mov [ui_in_col0], dl
    mov [ui_in_attr], bl

    ; Cursor set
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

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
    ; erase one char in UI
    mov dl, [ui_in_col]
    cmp dl, [ui_in_col0]
    jbe .rl_loop
    dec dl
    mov [ui_in_col], dl
    mov dh, [ui_in_row]
    mov bl, [ui_in_attr]
    mov al, ' '
    call ui_putc
    
    ; Cursor update
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

    jmp .rl_loop
.rl_char:
    cmp cx, (LINE_BUF_MAX-1)
    jae .rl_loop
.store:
    mov [di], al
    inc di
    inc cx
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    mov bl, [ui_in_attr]
    call ui_putc
    inc byte [ui_in_col]
    
    ; Cursor update
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

    jmp .rl_loop
.rl_done:
    mov byte [di], 0
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Read a short line but mask characters on screen (for passwords).
; Stores the real input into line_buf, but renders '*' for each character.
; IN:  DH=row, DL=col, BL=attr (UI)
read_line_masked:
    push ax
    push bx
    push cx
    push dx
    push di
    mov di, line_buf
    mov cx, 0
    mov [ui_in_row], dh
    mov [ui_in_col], dl
    mov [ui_in_col0], dl
    mov [ui_in_attr], bl

    ; Cursor set
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

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
    ; erase one char in UI
    mov dl, [ui_in_col]
    cmp dl, [ui_in_col0]
    jbe .rl_loop
    dec dl
    mov [ui_in_col], dl
    mov dh, [ui_in_row]
    mov bl, [ui_in_attr]
    mov al, ' '
    call ui_putc

    ; Cursor update
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

    jmp .rl_loop
.rl_char:
    cmp cx, (LINE_BUF_MAX-1)
    jae .rl_loop
.store:
    mov [di], al
    inc di
    inc cx
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    mov bl, [ui_in_attr]
    mov al, '*'
    call ui_putc
    inc byte [ui_in_col]

    ; Cursor update
    mov dh, [ui_in_row]
    mov dl, [ui_in_col]
    call ui_set_cursor

    jmp .rl_loop
.rl_done:
    mov byte [di], 0
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Prompt for user list (optional). Stores additional users to inst_user_*.
; - Usernames/Passwords: 1..8 chars, A-Z a-z 0-9 '_'
; - Blank line ends input.
prompt_users:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    push cs
    pop ds

    mov byte [inst_user_count], 0
    mov byte [inst_root_pass], 0

    ; Draw user setup dialog
    call ui_draw_base
    xor bx, bx
    mov al, 0
    call ui_steps_render
    mov si, ui_status_users
    call ui_status
    xor al, al
    call ui_progress

    ; Dialog box
    mov dh, 4
    mov dl, 14
    mov ch, 20
    mov cl, 65
    mov bh, UI_ATTR_BORDER
    mov bl, UI_ATTR_BG
    call ui_draw_box

    mov dh, 5
    mov dl, 24
    mov bl, UI_ATTR_BORDER
    mov si, ui_users_title
    call ui_puts

    mov dh, 7
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_q
    call ui_puts

    mov dh, 9
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_opt
    call ui_puts

    mov dh, 11
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_root_pw
    call ui_puts
    ; Clear input field
    mov dh, 11
    mov dl, 44
    mov cl, 12
    mov bl, UI_ATTR_BG
    call ui_fill_row
    ; Read root password into line_buf at (11,44) (blank allowed)
    mov dh, 11
    mov dl, 44
    mov bl, UI_ATTR_TEXT
    call read_line_masked
    call store_root_password
    ; Clear password field so the entered secret is not left visible on screen.
    mov dh, 11
    mov dl, 44
    mov cl, 12
    mov bl, UI_ATTR_BG
    call ui_fill_row

.choose:
    call read_key
    cmp al, 27          ; ESC
    je  .done
    cmp al, 13          ; ENTER (default No)
    je  .done
    cmp al, 'N'
    je  .done
    cmp al, 'n'
    je  .done
    cmp al, 'Y'
    je  .input_loop
    cmp al, 'y'
    je  .input_loop
    jmp .choose

.input_loop:
    ; Render user list
    call ui_render_user_list

    mov al, [inst_user_count]
    cmp al, MAX_INSTALL_USERS
    jae .limit

    ; Prompt
    mov dh, 16
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_name
    call ui_puts
    ; Clear input field
    mov dh, 16
    mov dl, 44
    mov cl, 12
    mov bl, UI_ATTR_BG
    call ui_fill_row
    ; Read username into line_buf at (16,44)
    mov dh, 16
    mov dl, 44
    mov bl, UI_ATTR_TEXT
    call read_line_upper

    mov si, line_buf
    cmp byte [si], 0
    je  .done

    call validate_username
    jc  .invalid

    call is_duplicate_username
    jc  .dup

    mov al, [inst_user_count]
    call store_username_slot
    
.password_loop:
    ; Prompt password for this user
    mov dh, 17
    mov dl, 18
    mov bl, UI_ATTR_TEXT
    mov si, ui_users_pass
    call ui_puts
    ; Clear password field
    mov dh, 17
    mov dl, 44
    mov cl, 12
    mov bl, UI_ATTR_BG
    call ui_fill_row
    ; Read password into line_buf at (17,44)
    mov dh, 17
    mov dl, 44
    mov bl, UI_ATTR_TEXT
    call read_line_masked

    mov si, line_buf
    cmp byte [si], 0
    je  .invalid_pw
    call validate_username
    jc  .invalid_pw

    mov al, [inst_user_count]
    call store_password_slot
    ; Clear password field so the entered secret is not left visible on screen.
    mov dh, 17
    mov dl, 44
    mov cl, 12
    mov bl, UI_ATTR_BG
    call ui_fill_row
    inc byte [inst_user_count]
    jmp .input_loop

.invalid:
    mov si, ui_users_invalid
    call ui_status
    xor al, al
    call ui_progress
    jmp .input_loop

.invalid_pw:
    mov si, ui_users_invalid_pw
    call ui_status
    xor al, al
    call ui_progress
    jmp .password_loop

.dup:
    mov si, ui_users_dup
    call ui_status
    xor al, al
    call ui_progress
    jmp .input_loop

.limit:
    mov si, ui_users_limit
    call ui_status
    xor al, al
    call ui_progress

.done:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; CF=1 if invalid
validate_username:
    push ax
    push cx
    push si
    mov si, line_buf
    xor cx, cx
.vloop:
    mov al, [si]
    cmp al, 0
    je  .vend
    inc cx
    cmp cx, 8
    ja  .bad
    cmp al, 'A'
    jb  .check_lower
    cmp al, 'Z'
    jbe .vnext
.check_lower:
    cmp al, 'a'
    jb  .check_digit
    cmp al, 'z'
    jbe .vnext
.check_digit:
    cmp al, '0'
    jb  .check_us
    cmp al, '9'
    jbe .vnext
.check_us:
    cmp al, '_'
    je  .vnext
    jmp .bad
.vnext:
    inc si
    jmp .vloop
.vend:
    cmp cx, 0
    je  .bad
    clc
    jmp .vret
.bad:
    stc
.vret:
    pop si
    pop cx
    pop ax
    ret

; CF=1 if duplicate exists
is_duplicate_username:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov cl, [inst_user_count]
    xor ch, ch
    xor bx, bx
.dloop:
    cmp bx, cx
    jae .no
    mov al, bl
    mov dl, LINE_BUF_MAX
    mul dl                      ; AX = index * LINE_BUF_MAX
    mov si, inst_user_names
    add si, ax
    mov di, line_buf
    call strcmp_z
    jc  .yes
    inc bx
    jmp .dloop
.no:
    clc
    jmp .dret
.yes:
    stc
.dret:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; CF=1 if equal
strcmp_z:
    push ax
.sloop:
    mov al, [si]
    cmp al, [di]
    jne .neq
    cmp al, 0
    je  .eq
    inc si
    inc di
    jmp .sloop
.eq:
    stc
    pop ax
    ret
.neq:
    clc
    pop ax
    ret

; Store line_buf into inst_user_names[AL]
store_username_slot:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov dl, LINE_BUF_MAX
    mul dl                      ; AX = index * LINE_BUF_MAX
    mov di, inst_user_names
    add di, ax
    mov si, line_buf
    mov cx, LINE_BUF_MAX
.copy:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    dec cx
    jz  .done
    test al, al
    jnz .copy
.zero:
    mov byte [di], 0
    inc di
    dec cx
    jnz .zero
.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Store line_buf into inst_user_passes[AL]
store_password_slot:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov dl, LINE_BUF_MAX
    mul dl                      ; AX = index * LINE_BUF_MAX
    mov di, inst_user_passes
    add di, ax
    mov si, line_buf
    mov cx, LINE_BUF_MAX
.copy:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    dec cx
    jz  .done
    test al, al
    jnz .copy
.zero:
    mov byte [di], 0
    inc di
    dec cx
    jnz .zero
.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Store line_buf into inst_root_pass
store_root_password:
    push ax
    push cx
    push si
    push di
    push ds

    push cs
    pop ds

    mov si, line_buf
    mov di, inst_root_pass
    mov cx, (LINE_BUF_MAX-1)
.rloop:
    lodsb
    stosb
    test al, al
    je  .done
    loop .rloop
    mov byte [di-1], 0
.done:
    pop ds
    pop di
    pop si
    pop cx
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

    call ui_status_tick

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

    call ui_status_tick

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

    call ui_status_tick

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
try_prepare_source_disk:
    call read_src_boot_sector
    jc .fail
    call verify_source_boot_signature
    jc .fail
    clc
    ret
.fail:
    stc
    ret

; Validate source boot sector signature.
; Accepts:
;  1) New explicit signature at 0x1F0 ("SEOLSIG!")
;  2) Legacy Seolsem BPB identity (OEM/LABEL/FSTYPE)
; Uses boot sector currently stored in BUF_SEG.
verify_source_boot_signature:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ax, BUF_SEG
    mov es, ax

    ; Basic boot signature guard.
    cmp word [es:510], 0xAA55
    jne .legacy

    ; New explicit signature marker.
    push cs
    pop ds
    mov si, src_boot_magic
    mov di, SRC_BOOT_MAGIC_OFS
    mov cx, SRC_BOOT_MAGIC_LEN
.sig_loop:
    cmp cx, 0
    je .ok
    mov al, [es:di]
    cmp al, [ds:si]
    jne .legacy
    inc di
    inc si
    dec cx
    jmp .sig_loop

.legacy:
    ; Backward compatibility with older Seolsem images.
    push cs
    pop ds

    mov si, src_oem_legacy
    mov di, 0x0003
    mov cx, 8
    call cmp_ds_es_bytes
    jc .fail

    mov si, src_label_legacy
    mov di, 0x002B
    mov cx, 11
    call cmp_ds_es_bytes
    jc .fail

    mov si, src_fstype_legacy
    mov di, 0x0036
    mov cx, 8
    call cmp_ds_es_bytes
    jc .fail

.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Compare DS:SI and ES:DI for CX bytes. CF=0 when equal.
cmp_ds_es_bytes:
    push ax
.loop:
    cmp cx, 0
    je .same
    mov al, [ds:si]
    cmp al, [es:di]
    jne .diff
    inc si
    inc di
    dec cx
    jmp .loop
.same:
    clc
    pop ax
    ret
.diff:
    stc
    pop ax
    ret

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

; Validate source floppy/root contents before destructive HDD writes.
; Requires source BPB/layout fields to be parsed.
; CF=0 when KERNEL.BIN, XENV.ENV and BIN directory exist.
verify_source_seolsem_image:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push es

    push cs
    pop ds

    call verify_source_boot_signature
    jc .fail

    mov ax, [src_root_dir_sectors]
    test ax, ax
    jz .fail

    mov ax, SRC_ROOT_SEG
    mov es, ax
    xor bx, bx
    xor dx, dx
    mov ax, [src_root_start_lba]
    push ax
    xor ax, ax
    mov al, [src_drive]
    mov si, ax
    pop ax
    mov cx, [src_root_dir_sectors]
    call read_sectors_lba32
    jc .fail

    mov si, name_kernel11
    call find_root_entry
    jc .fail
    mov al, [es:di+11]
    test al, 0x10
    jnz .fail

    mov si, name_env11
    call find_root_entry
    jc .fail
    mov al, [es:di+11]
    test al, 0x10
    jnz .fail

    mov si, name_bin11
    call find_root_entry
    jc .fail
    mov al, [es:di+11]
    test al, 0x10
    jz .fail

    clc
    jmp .ret

.fail:
    stc
.ret:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret




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
    ; Animate status
    push ax
    push bx
    push cx
    push dx
    push si
    call ui_status_tick
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
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
    
    ; Animate status
    push ax
    push bx
    push cx
    push dx
    push si
    call ui_status_tick
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    cmp cx, 0
    jne .wloop
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
    
    ; Animate status
    push ax
    push bx
    push cx
    push dx
    push si
    call ui_status_tick
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

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

LINE_BUF_MAX equ 9
MAX_INSTALL_USERS equ 7
line_buf times LINE_BUF_MAX db 0

; UI input cursor state for read_line_upper
ui_in_row  db 0
ui_in_col  db 0
ui_in_col0 db 0
ui_in_attr db 0

; Installer UI progress state
ui_done_mask dw 0
ui_cur_step  db 0

; BIOS geometry
src_drive db INSTALL_DRIVE
src_retry_left db 0
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
name_etc11     db 'ETC        '
name_home11    db 'HOME       '
name_passwd11  db 'PASSWD     '
name_dot11     db '.          '
name_dotdot11  db '..         '

passwd_root_prefix db 'ROOT:0:/:',0
passwd_home_prefix db '/HOME/',0
passwd_crlf        db 13,10,0
src_boot_magic     db 'SEOLSIG!'
src_oem_legacy     db 'SEOLSEM '
src_label_legacy   db 'SEOLSEM    '
src_fstype_legacy  db 'FAT12   '

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
dst_etc_cluster     dw 0
dst_home_cluster    dw 0
dst_passwd_cluster  dw 0
dst_passwd_size_lo  dw 0
dst_passwd_size_hi  dw 0

; Users to pre-create (additional users; ROOT is always included)
inst_user_count     db 0
inst_root_pass      times LINE_BUF_MAX db 0
inst_user_names     times (MAX_INSTALL_USERS*LINE_BUF_MAX) db 0
inst_user_passes    times (MAX_INSTALL_USERS*LINE_BUF_MAX) db 0
inst_user_home_cluster times MAX_INSTALL_USERS dw 0

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
    mov si, ui_log_find_files
    call ui_log_push
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

    mov si, ui_log_copy_kernel
    call ui_log_push
    mov cx, [src_kernel_cluster]
    mov dx, [src_kernel_size_lo]
    mov si, [src_kernel_size_hi]
    mov ax, [dst_kernel_cluster]
    mov bx, [dst_kernel_clusters]
    call copy_file_to_dst
    jc .fail
    mov al, 65
    call ui_progress

    ; ---- Allocate/copy XENV.ENV ----
    mov ax, [src_env_size_lo]
    mov dx, [src_env_size_hi]
    call bytes_to_clusters_ceil
    jc .fail
    mov [dst_env_clusters], ax
    call fat32_alloc_chain
    jc .fail
    mov [dst_env_cluster], ax

    mov si, ui_log_copy_env
    call ui_log_push
    mov cx, [src_env_cluster]
    mov dx, [src_env_size_lo]
    mov si, [src_env_size_hi]
    mov ax, [dst_env_cluster]
    mov bx, [dst_env_clusters]
    call copy_file_to_dst
    jc .fail
    mov al, 70
    call ui_progress

    ; ---- Allocate/copy BIN/* files ----
    mov si, ui_log_copy_bin
    call ui_log_push
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
    or word [ui_done_mask], (1 << 4)
    mov al, 85
    call ui_progress

    ; ---- Pre-create users (/ETC/PASSWD, /HOME) ----
    mov byte [ui_cur_step], 5
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_create_users
    call ui_status
    mov al, 90
    call ui_progress
    mov si, ui_log_create_users
    call ui_log_push
    call install_users_fat32
    jc .fail
    or word [ui_done_mask], (1 << 5)

    ; ---- Write FAT32 root directory (cluster 2) ----
    mov byte [ui_cur_step], 6
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov si, ui_status_finalize
    call ui_status
    mov al, 95
    call ui_progress
    mov si, ui_log_finalize
    call ui_log_push
    call write_root_dir_cluster_fat32
    jc .fail
    or word [ui_done_mask], (1 << 6)
    mov bx, [ui_done_mask]
    mov al, [ui_cur_step]
    call ui_steps_render
    mov al, 100
    call ui_progress

    clc
    jmp .ret

.missing_kernel:
    mov si, msg_missing_kernel
    call ui_log_push
    stc
    jmp .ret
.missing_env:
    mov si, msg_missing_env
    call ui_log_push
    stc
    jmp .ret
.missing_bin:
    mov si, msg_missing_bin
    call ui_log_push
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

; --------------------------
; User pre-create helpers (FAT32)
;   - Create /ETC, /HOME, and /ETC/PASSWD
;   - Create /HOME/<USER> directories for users entered during install
; --------------------------

; Build 11-byte 8.3 name (no extension) from DS:SI into ES:DI.
write_name11_from_str:
    push ax
    push bx
    push cx
    push si
    push di
    mov bx, di
    mov cx, 8
.base_loop:
    mov al, [si]
    cmp al, 0
    je  .pad_base
    mov [es:bx], al
    inc si
    inc bx
    dec cx
    jnz .base_loop
    jmp .ext
.pad_base:
    mov al, ' '
.pad_loop:
    mov [es:bx], al
    inc bx
    dec cx
    jnz .pad_loop
.ext:
    mov cx, 3
    mov al, ' '
.ext_loop:
    mov [es:bx], al
    inc bx
    loop .ext_loop
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

append_zstring:
    push ax
.aloop:
    lodsb
    test al, al
    jz   .adone
    stosb
    jmp  .aloop
.adone:
    pop ax
    ret

; Write unsigned AX as decimal to ES:DI (no NUL).
utoa_dec_esdi:
    push ax
    push bx
    push cx
    push dx
    cmp ax, 0
    jne .conv
    mov al, '0'
    stosb
    jmp .done
.conv:
    mov bx, 10
    xor cx, cx
.div_loop:
    xor dx, dx
    div bx                      ; AX=quot, DX=rem
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
.out_loop:
    pop dx
    add dl, '0'
    mov al, dl
    stosb
    loop .out_loop
.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Build PASSWD content into BUF_SEG and set dst_passwd_size_*.
; OUT: AX=size, CF=1 on error.
build_passwd_buf:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds

    call zero_buf512            ; ES=BUF_SEG (cleared)
    mov ax, BUF_SEG
    mov es, ax
    xor di, di

    mov si, passwd_root_prefix
    call append_zstring
    mov si, inst_root_pass
    call append_zstring
    mov si, passwd_crlf
    call append_zstring

    xor bx, bx                  ; user index
.user_loop:
    mov al, [inst_user_count]
    xor ah, ah
    cmp bx, ax
    jae .done

    mov al, bl
    mov dl, LINE_BUF_MAX
    mul dl                      ; AX = index * LINE_BUF_MAX
    mov si, inst_user_names
    add si, ax

    call append_zstring          ; NAME
    mov al, ':'
    stosb

    mov ax, bx
    inc ax                       ; uid = index + 1
    call utoa_dec_esdi
    mov al, ':'
    stosb

    mov si, passwd_home_prefix   ; /HOME/
    call append_zstring

    mov al, bl
    mov dl, LINE_BUF_MAX
    mul dl
    mov si, inst_user_names
    add si, ax
    call append_zstring          ; NAME again

    mov al, ':'
    stosb
    mov al, bl
    mov dl, LINE_BUF_MAX
    mul dl
    mov si, inst_user_passes
    add si, ax
    call append_zstring          ; PASSWORD

    mov si, passwd_crlf
    call append_zstring

    inc bx
    jmp .user_loop

.done:
    mov ax, di
    mov [dst_passwd_size_lo], ax
    mov word [dst_passwd_size_hi], 0
    clc
    jmp .ret
.ret:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Write BUF_SEG (first sector) into dst_passwd_cluster, zero remaining sectors in the cluster.
; CF=1 on error.
write_passwd_cluster:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    mov ax, [dst_passwd_cluster]
    call dst_cluster_to_abs_lba
    mov [tmp_lba_lo], ax
    mov [tmp_lba_hi], dx

    xor cx, cx
    mov cl, [dst_sec_per_clus]

    ; First sector contains content (already zero-padded)
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_lba_lo]
    jnz .no_c1
    inc word [tmp_lba_hi]
.no_c1:
    dec cx
    jz  .ok

.zero_loop:
    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_lba_lo]
    mov dx, [tmp_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_lba_lo]
    jnz .no_c2
    inc word [tmp_lba_hi]
.no_c2:
    dec cx
    jnz .zero_loop
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write a minimal directory cluster with '.' and '..'.
; IN: AX=dir_cluster, BX=parent_cluster (0 allowed). CF=1 on error.
write_empty_dir_cluster:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    push cs
    pop ds

    mov bp, ax                  ; dir cluster
    mov ax, bp
    call dst_cluster_to_abs_lba
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx

    xor cx, cx
    mov cl, [dst_sec_per_clus]

    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax

    ; '.'
    mov di, 0
    mov si, name_dot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:0+11], 0x10
    mov word [es:0+20], 0
    mov ax, bp
    mov word [es:0+26], ax

    ; '..'
    mov di, 32
    mov si, name_dotdot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:32+11], 0x10
    mov word [es:32+20], 0
    mov ax, bx
    mov word [es:32+26], ax

    ; Write first sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c1
    inc word [tmp_dst_lba_hi]
.no_c1:
    dec cx
    jz  .ok

.rest:
    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c2
    inc word [tmp_dst_lba_hi]
.no_c2:
    dec cx
    jnz .rest
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write /ETC directory cluster with PASSWD entry.
; CF=1 on error.
write_etc_dir_cluster:
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

    mov ax, [dst_etc_cluster]
    call dst_cluster_to_abs_lba
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx

    xor cx, cx
    mov cl, [dst_sec_per_clus]

    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax

    ; '.'
    mov di, 0
    mov si, name_dot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:0+11], 0x10
    mov word [es:0+20], 0
    mov ax, [dst_etc_cluster]
    mov word [es:0+26], ax

    ; '..' (root => 0)
    mov di, 32
    mov si, name_dotdot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:32+11], 0x10
    mov word [es:32+20], 0
    mov word [es:32+26], 0

    ; PASSWD
    mov di, 64
    mov si, name_passwd11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:64+11], 0x20
    mov word [es:64+20], 0
    mov ax, [dst_passwd_cluster]
    mov word [es:64+26], ax
    mov ax, [dst_passwd_size_lo]
    mov word [es:64+28], ax
    mov ax, [dst_passwd_size_hi]
    mov word [es:64+30], ax

    ; Write first sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c1
    inc word [tmp_dst_lba_hi]
.no_c1:
    dec cx
    jz  .ok

.rest:
    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c2
    inc word [tmp_dst_lba_hi]
.no_c2:
    dec cx
    jnz .rest
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Write /HOME directory cluster with user directories.
; CF=1 on error.
write_home_dir_cluster:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    push cs
    pop ds

    mov ax, [dst_home_cluster]
    call dst_cluster_to_abs_lba
    mov [tmp_dst_lba_lo], ax
    mov [tmp_dst_lba_hi], dx

    xor cx, cx
    mov cl, [dst_sec_per_clus]

    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax

    ; '.'
    mov di, 0
    mov si, name_dot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:0+11], 0x10
    mov word [es:0+20], 0
    mov ax, [dst_home_cluster]
    mov word [es:0+26], ax

    ; '..' (root => 0)
    mov di, 32
    mov si, name_dotdot11
    push cx
    mov cx, 11
    rep movsb
    pop cx
    mov byte [es:32+11], 0x10
    mov word [es:32+20], 0
    mov word [es:32+26], 0

    xor bp, bp                  ; user index
.u_loop:
    mov al, [inst_user_count]
    xor ah, ah
    cmp bp, ax
    jae .write

    ; entry offset = (2 + index) * 32
    mov ax, bp
    add ax, 2
    shl ax, 5
    mov di, ax

    ; name from inst_user_names[index]
    mov ax, bp
    mov dl, LINE_BUF_MAX
    mul dl
    mov si, inst_user_names
    add si, ax
    push di
    call write_name11_from_str
    pop di

    mov byte [es:di+11], 0x10
    mov word [es:di+20], 0
    mov bx, bp
    shl bx, 1
    mov ax, [inst_user_home_cluster + bx]
    mov word [es:di+26], ax

    inc bp
    jmp .u_loop

.write:
    ; Write first sector
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c1
    inc word [tmp_dst_lba_hi]
.no_c1:
    dec cx
    jz  .ok

.rest:
    call zero_buf512
    mov ax, BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [tmp_dst_lba_lo]
    mov dx, [tmp_dst_lba_hi]
    mov si, DST_DRIVE
    call write_sector_lba32
    jc .fail

    inc word [tmp_dst_lba_lo]
    jnz .no_c2
    inc word [tmp_dst_lba_hi]
.no_c2:
    dec cx
    jnz .rest
.ok:
    clc
    jmp .ret
.fail:
    stc
.ret:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Main entry: allocate clusters + write /ETC, /HOME, PASSWD and user home dirs.
; CF=1 on error.
install_users_fat32:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds

    push cs
    pop ds

    ; /ETC
    mov ax, 1
    call fat32_alloc_chain
    jc .fail
    mov [dst_etc_cluster], ax

    ; /HOME
    mov ax, 1
    call fat32_alloc_chain
    jc .fail
    mov [dst_home_cluster], ax

    ; /ETC/PASSWD (small; 1 cluster)
    mov ax, 1
    call fat32_alloc_chain
    jc .fail
    mov [dst_passwd_cluster], ax

    ; Allocate /HOME/<USER> clusters
    xor bp, bp
.alloc_loop:
    mov al, [inst_user_count]
    xor ah, ah
    cmp bp, ax
    jae .alloc_done
    mov ax, 1
    call fat32_alloc_chain
    jc .fail
    mov di, bp
    shl di, 1
    mov [inst_user_home_cluster + di], ax
    inc bp
    jmp .alloc_loop
.alloc_done:

    ; PASSWD content -> BUF_SEG
    call build_passwd_buf
    jc .fail

    call write_passwd_cluster
    jc .fail

    ; Create user home directories (empty)
    xor bp, bp
.home_dirs:
    mov al, [inst_user_count]
    xor ah, ah
    cmp bp, ax
    jae .dirs_done
    mov di, bp
    shl di, 1
    mov ax, [inst_user_home_cluster + di]
    mov bx, [dst_home_cluster]
    call write_empty_dir_cluster
    jc .fail
    inc bp
    jmp .home_dirs
.dirs_done:

    call write_home_dir_cluster
    jc .fail

    call write_etc_dir_cluster
    jc .fail

    clc
    jmp .ret
.fail:
    stc
.ret:
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
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

    ; Entry 3: ETC (directory)
    mov di, 96
    mov si, name_etc11
    mov cx, 11
    rep movsb
    mov byte [es:96+11], 0x10
    mov word [es:96+20], 0
    mov ax, [dst_etc_cluster]
    mov word [es:96+26], ax

    ; Entry 4: HOME (directory)
    mov di, 128
    mov si, name_home11
    mov cx, 11
    rep movsb
    mov byte [es:128+11], 0x10
    mov word [es:128+20], 0
    mov ax, [dst_home_cluster]
    mov word [es:128+26], ax

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
; TUI strings
ui_status_confirm     db 'Confirm installation',0
ui_status_type_yes    db 'Type YES to confirm',0
ui_status_bad_yes     db 'Confirmation failed',0
ui_status_users       db 'User setup (optional)',0
ui_status_swap        db 'Swap to Seolsem disk and press any key',0
ui_status_detect      db 'Detecting disks...',0
ui_status_partition   db 'Partitioning HDD...',0
ui_status_format      db 'Formatting FAT32...',0
ui_status_copy_boot   db 'Copying bootloader...',0
ui_status_copy_files  db 'Copying system files...',0
ui_status_create_users db 'Creating users...',0
ui_status_finalize    db 'Finalizing...',0
ui_status_cancel      db 'Cancelled.',0
ui_status_reboot      db 'Press any key to reboot...',0

ui_err_disk           db 'Disk error.',0
ui_err_bpb            db 'Bad source BPB.',0
ui_err_source         db 'Source disk is not a Seolsem image.',0
ui_err_stage2         db 'Stage2 too large; HDD boot will fail.',0
ui_err_layout         db 'Layout calculation failed.',0
ui_err_install        db 'Install failed.',0

ui_log_confirm        db 'Warning: this will erase HDD data.',0
ui_log_cancel         db 'Installation cancelled.',0
ui_log_start          db 'Starting installation...',0
ui_log_hdd_geom       db 'Detect HDD geometry',0
ui_log_hdd_size       db 'Detect HDD size',0
ui_log_src_try_b      db 'Checking system disk in drive B:',0
ui_log_src_retry      db 'Source disk not valid, retrying...',0
ui_log_swap           db 'Swap to system disk now.',0
ui_log_src_geom       db 'Detect source geometry',0
ui_log_parse_bpb      db 'Parse source BPB',0
ui_log_calc_src       db 'Compute source layout',0
ui_log_validate_src   db 'Validate Seolsem source image',0
ui_log_calc_dst       db 'Compute destination layout',0
ui_log_write_mbr      db 'Write MBR',0
ui_log_write_vbr      db 'Write VBR',0
ui_log_write_fsinfo   db 'Write FSInfo',0
ui_log_clear_fat      db 'Clear FAT tables',0
ui_log_init_root      db 'Init root directory',0
ui_log_copy_boot      db 'Copy reserved sectors',0
ui_log_load_src       db 'Load source FAT/root',0
ui_log_install        db 'Copy files',0
ui_log_done           db 'Success. Reboot now.',0

ui_log_find_files     db 'Find required files',0
ui_log_copy_kernel    db 'Copy KERNEL.BIN',0
ui_log_copy_env       db 'Copy XENV.ENV',0
ui_log_copy_bin       db 'Copy BIN/*',0
ui_log_create_users   db 'Create /ETC/PASSWD and /HOME',0
ui_log_finalize       db 'Write root directory',0

ui_confirm_title      db 'Install Seolsem',0
ui_confirm_line1      db 'This will ERASE all data on the first HDD.',0
ui_confirm_line2      db 'Press Y to continue, N to cancel.',0
ui_confirm_q          db 'Install to HDD? (Y/N)',0
ui_confirm_type       db 'Type YES to confirm:',0

ui_users_title        db 'User Setup',0
ui_users_q            db 'Optional: add user accounts now.',0
ui_users_opt          db '[N]o (root only)    [Y]es (add users)',0
ui_users_root_pw      db 'Root password (blank=none):',0
ui_users_list         db 'Users:',0
ui_users_name         db 'Username (1-8):',0
ui_users_pass         db 'Password (1-8):',0
ui_users_invalid      db 'Invalid username.',0
ui_users_invalid_pw   db 'Invalid password.',0
ui_users_dup          db 'User already added.',0
ui_users_limit        db 'User limit reached.',0

msg_banner       db 'Seolsem Installer (FAT32 Support)',13,10,0
msg_confirm1     db 'Install to HDD? (Y/N): ',0
msg_confirm2     db 'Type YES to confirm data loss: ',0
msg_user_prompt  db 'Add user accounts now? (Y/N): ',0
msg_user_name    db 'Username (<=8, blank=done): ',0
msg_invalid_user db 'Invalid username.',0
msg_duplicate_user db 'User already added.',0
msg_user_limit   db 'User list is full.',0
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
