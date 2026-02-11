[org 0x00]
[bits 16]

section .text

%define KERNEL_LOAD_SEG 0x1080
%define BOOTSEG         0x07C0
%define FAT_BUF_SEG     0x9000
%define ROOT_BUF_SEG    0x9200

%define ENTRY_OFF (CODE_BASE + ENTRY_REL)
%define KERNEL_BASE_SEG (KERNEL_LOAD_SEG + DGROUP_DELTA)

; Kernel image must fit within a single 64KiB window because:
; - Stage2 jumps to KERNEL_BASE_SEG:ENTRY_OFF (offset is 16-bit)
; - Kernel switches to 16-bit protected mode with CS/DS base = image base
;   and segment limit = 0xFFFF.
; If this grows beyond 64KiB, the first command execution can GP exception and hang.
%if (CODE_BASE + CODE_SIZE) > 0x10000
    %error Kernel image exceeds 64KiB. Reduce size (e.g. optimize for size) or redesign segmentation.
%endif

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

    ; Detect INT 13h extensions (EDD) for reliable LBA reads on HDDs.
    mov byte [edd_ok], 0
    push ds
    mov dl, [boot_drive]
    mov bx, 0x55AA
    mov ah, 0x41
    int 0x13
    pop ds
    jc .no_edd
    cmp bx, 0xAA55
    jne .no_edd
    test cx, 0x0001
    jz .no_edd
    mov byte [edd_ok], 1
.no_edd:

    call enable_a20
    cld         ; Ensure direction flag is clear for string ops
    call serial_init


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
    mov ax, [es:0x1C]
    mov [vol_base_lba_lo], ax
    mov ax, [es:0x1E]
    mov [vol_base_lba_hi], ax

    ; Determine FAT type
    cmp word [bpb_fat_secs], 0
    jne .is_fat12_16
    ; FAT32
    mov byte [fat_type], 2 ; 2=FAT32
    mov ax, [es:0x24]
    mov [bpb_fat_secs], ax ; FATSz32 Low
    mov ax, [es:0x26]
    mov [bpb_fat_secs_hi], ax ; High (ignore? assumes < 64k sectors for fat self? No, can be large)
    
    mov ax, [es:0x2C]
    mov [fat32_root_cluster], ax
    mov [root_dir_sectors], 0 ; FAT32 has no fixed root dir
    jmp .calc_lba

.is_fat12_16:
    mov byte [fat_type], 0 ; 0=FAT12 (simplified)

    ; root_dir_sectors = (root_ents * 32 + bytes_per_sec - 1) / bytes_per_sec
    mov ax, [bpb_root_ents]
    mov bx, 32
    mul bx
    add ax, [bpb_bytes_per_sec]
    dec ax
    xor dx, dx
    div word [bpb_bytes_per_sec]
    mov [root_dir_sectors], ax

.calc_lba:
    ; first FAT LBA = volume_base + reserved
    mov ax, [vol_base_lba_lo]
    mov dx, [vol_base_lba_hi]
    add ax, [bpb_res_sectors]
    adc dx, 0
    mov [fat_start_lba_lo], ax
    mov [fat_start_lba_hi], dx

    ; root/data LBA
    ; Total FAT sectors = Fats * FatSecs
    ; If FAT32 and FatSecs > 64k, we need 32-bit math.
    ; Assuming simple case first (FatSecs fits in 16bit or we use LO).
    ; For robust FAT32, we should use 32-bit FatSecs.
    
    mov ax, [bpb_fat_secs]
    mov dx, [bpb_fat_secs_hi] ; 0 for FAT12
    mov bl, [bpb_fats]
    xor bh, bh
    ; DX:AX * BX -> DX:AX (approx, if BX small and result fits 32bit)
    ; Standard mul:
    ; Low * BX
    push dx
    mul bx
    mov cx, dx ; carry from low
    pop dx
    push ax
    mov ax, dx
    mul bx
    add ax, cx ; High result
    mov dx, ax
    pop ax
    
    ; DX:AX = Total FAT bytes/sectors
    
    add ax, [fat_start_lba_lo]
    adc dx, [fat_start_lba_hi]
    mov [root_start_lba_lo], ax
    mov [root_start_lba_hi], dx

    cmp byte [fat_type], 2
    je .fat32_data
    
    ; FAT12: Data starts after Root Dir
    mov ax, [root_start_lba_lo]
    mov dx, [root_start_lba_hi]
    add ax, [root_dir_sectors]
    adc dx, 0
    mov [data_start_lba_lo], ax
    mov [data_start_lba_hi], dx
    jmp .read_root

.fat32_data:
    ; FAT32: Data starts at root_start_lba (which is actually Data Region start)
    ; Root Dir is just a cluster chain in Data Region.
    mov ax, [root_start_lba_lo]
    mov dx, [root_start_lba_hi]
    mov [data_start_lba_lo], ax
    mov [data_start_lba_hi], dx

    ; (No banner/progress prints in normal boot path; keep output for errors only.)

.read_root:


    ; ---- Read FAT (First few sectors for FAT12, or just cache?) ----
    ; Note: For FAT32, FAT can be huge. We shouldn't read entire FAT into memory.
    ; sima.asm reads entire FAT to 0x9000? 
    ; FAT_BUF_SEG=0x9000. 
    ; If FAT is huge, this overwrites everything.
    ; We should only read relevant FAT sectors on demand?
    ; Or just read first segment.
    
    ; For compatibility, we try to load "some" FAT.
    ; FAT12 floppy FAT is small (9 sectors).
    ; FAT32 HDD FAT is large.
    ; Let's cache one sector at a time or implement caching logic.
    ; Existing code: call read_sectors_lba with CX=[bpb_fat_secs].
    ; This is dangerous for FAT32.
    
    cmp byte [fat_type], 2
    je .fat32_root_load
    
    ; FAT12 case: Load whole FAT
    mov ax, FAT_BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [fat_start_lba_lo]
    mov dx, [fat_start_lba_hi]
    mov cx, [bpb_fat_secs]
    call read_sectors_lba
    
    ; Load Fixed Root Directory
    mov ax, ROOT_BUF_SEG

    mov es, ax
    xor bx, bx
    mov ax, [root_start_lba_lo]
    mov dx, [root_start_lba_hi]
    mov cx, [root_dir_sectors]
    call read_sectors_lba

    ; ---- Find KERNEL.BIN ----
    mov ax, ROOT_BUF_SEG
    mov es, ax
    xor di, di
    mov cx, [bpb_root_ents]
    jmp .find_kernel

.fat32_root_load:
    ; FAT32: Root Dir is a cluster chain.

    ; We treat it like a file. Load it to ROOT_BUF_SEG.
    ; We load ONLY the first cluster of root dir for scanning KERNEL.BIN.
    ; (Assuming KERNEL.BIN is early in root dir).
    
    mov ax, [fat32_root_cluster]
    mov [cur_cluster], ax
    
    mov ax, ROOT_BUF_SEG
    mov es, ax
    xor bx, bx
    call read_cluster_to_es_bx
    
    ; Continue to find_kernel (it scans ES:DI, DI=0)


    ; ---- Find KERNEL.BIN ----
    mov ax, ROOT_BUF_SEG

    mov es, ax
    xor di, di
    ; FAT32 has RootEntCnt=0. We loaded 1 root-dir cluster (SecPerClus sectors).
    ; Each sector holds 16 directory entries (512 / 32).
    xor ax, ax
    mov al, [bpb_sec_per_clus]
    shl ax, 4
    mov cx, ax
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
    mov cx, 11
    repe cmpsb
    je .kernel_found_pop

    
    pop di
    pop cx
    jmp .next_entry

.kernel_found_pop:
    pop di
    pop cx
    jmp kernel_found



.next_entry:
    add di, 32
    dec cx
    jmp .find_kernel

kernel_not_found:
    mov si, msg_kernel_not_found
    call print_string
    mov si, msg_reboot
    call print_string
    xor ax, ax
    int 0x16
    int 0x19
    jmp $





kernel_found:
    xor eax, eax
    mov ax, [es:di+20]      ; FirstClusterHigh (FAT32)
    shl eax, 16
    mov ax, [es:di+26]      ; FirstClusterLow
    mov [cur_cluster], eax

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

    mov eax, [cur_cluster]
    mov [cur_cluster_orig], eax
    cmp eax, 0x0FFFFFF8
    jae kernel_loaded

    ; lba = data_start + (cluster-2) * sec_per_clus
    ; Use 32-bit math.
    mov eax, [cur_cluster]
    sub eax, 2
    xor ecx, ecx
    mov cl, [bpb_sec_per_clus]
    mul ecx ; EAX * ECX -> EDX:EAX. Assuming result fits in 32-bit LBA (max 2TB).
    
    ; Add base LBA (stored as a 32-bit value at data_start_lba_lo/data_start_lba_hi).
    add eax, [data_start_lba_lo]
    
    mov [cur_lba_lo], eax ; Store back to 32-bit LBA var (reusing cur_lba_lo as dword base)
    
    ; Setup for read_sectors_lba (expects DX:AX)
    mov dx, [cur_lba_lo+2]
    mov ax, [cur_lba_lo]

    xor cx, cx
    mov cl, [bpb_sec_per_clus]
    call read_sectors_lba

    ; kernel_size -= min(kernel_size, sec_per_clus * bytes_per_sec)
    ; (Handle last partial cluster without underflowing the size counter.)
    xor ax, ax
    mov al, [bpb_sec_per_clus]
    mul word [bpb_bytes_per_sec]      ; DX:AX = cluster_bytes

    ; NOTE: BX is the kernel load pointer (ES:BX). Do not clobber it here.
    mov si, [kernel_size_low]
    mov di, [kernel_size_high]
    ; if kernel_size <= cluster_bytes -> set size to 0
    cmp di, dx
    jb .size_to_zero
    ja .size_sub
    cmp si, ax
    jbe .size_to_zero
.size_sub:
    sub [kernel_size_low], ax
    sbb [kernel_size_high], dx
    jmp .size_done
.size_to_zero:
    mov word [kernel_size_low], 0
    mov word [kernel_size_high], 0
.size_done:

    ; next cluster
    mov eax, [cur_cluster_orig]
    call get_next_cluster_32 ; EAX->EAX

    mov dx, [kernel_size_low]
    mov cx, [kernel_size_high]
    or dx, cx
    jz .size_ok
    ; Check EOC based on fat type
    cmp byte [fat_type], 2
    je .check_eoc32
    cmp ax, 0x0FF8
    jb .size_ok
    jmp .chain_err
.check_eoc32:
    cmp eax, 0x0FFFFFF8
    jb .size_ok
.chain_err:
    mov si, msg_kernel_chain_error
    call print_string
    jmp $
.size_ok:
    mov [cur_cluster], eax
    jmp .load_loop

kernel_loaded:
    ; Verify entry opcode at loaded location (expect 0xEB)
    mov ax, KERNEL_BASE_SEG
    mov ds, ax
    mov si, ENTRY_OFF
    mov al, [ds:si]
    cmp al, 0xEB
    je .entry_ok
    mov bl, al
    push cs
    pop ds
    mov si, msg_kernel_entry_error
    call print_string
    mov al, bl
    call print_hex_byte
    mov al, 13
    call print_char
    mov al, 10
    call print_char
    jmp $
.entry_ok:
    ; Set DS/ES to DGROUP and jump to kernel entry
    mov ax, KERNEL_BASE_SEG
    mov ds, ax
    mov es, ax

    jmp KERNEL_BASE_SEG:ENTRY_OFF

; --------------------
; FAT12 helpers
; --------------------

; AX = cluster, returns AX = next cluster

; Reads cluster [cur_cluster] (32-bit) to ES:BX
read_cluster_to_es_bx:
    mov eax, [cur_cluster]
    sub eax, 2
    xor ecx, ecx
    mov cl, [bpb_sec_per_clus]
    mul ecx
    add eax, [data_start_lba_lo]
    
    mov dx, ax
    shr eax, 16
    xchg dx, ax ; DX:AX
    
    xor cx, cx
    mov cl, [bpb_sec_per_clus]
    call read_sectors_lba
    ret

; Universal get_next_cluster
; IN: EAX = current cluster
; OUT: EAX = next cluster
get_next_cluster_32:
    cmp byte [fat_type], 2
    je .fat32
    call fat12_next_cluster
    and eax, 0xFFFF ; Clear high bits for safety
    ret
.fat32:
    ; FAT32 entry lookup (cluster -> FAT sector + offset)
    ;   fat_offset_bytes = cluster * 4
    ;   fat_sector_lba   = fat_start + (fat_offset_bytes / 512)
    ;   entry_off        = fat_offset_bytes % 512
    push bx
    push es
    push ds
    push si
    push dx
    push cx

    mov edx, eax
    shl edx, 2                ; fat_offset_bytes

    mov eax, edx
    shr eax, 9                ; sector offset within FAT

    and edx, 0x1FF            ; entry offset within sector
    mov si, dx                ; save entry offset (restored after read_sectors_lba)

    add eax, [fat_start_lba_lo] ; dword add (lo+hi words)
    mov [cur_lba_lo], eax

    mov ax, FAT_BUF_SEG
    mov es, ax
    xor bx, bx
    mov ax, [cur_lba_lo]
    mov dx, [cur_lba_lo+2]
    mov cx, 1
    call read_sectors_lba

    mov ax, FAT_BUF_SEG
    mov ds, ax
    mov eax, [ds:si]
    and eax, 0x0FFFFFFF

    pop cx
    pop dx
    pop si
    pop ds
    pop es
    pop bx
    ret


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


; Read CX sectors from LBA DX:AX into ES:BX
read_sectors_lba:
    push ax
    push cx
    push dx
    push si
    push di

    mov si, ax            ; current LBA low
    mov di, dx            ; current LBA high
.read_loop:
    cmp cx, 0
    je .read_done

    mov ax, si
    mov dx, di
    call read_sector_lba

    mov ax, [bpb_bytes_per_sec]
    add bx, ax
    jnc .no_wrap
    ; BX wrapped past 0xFFFF -> advance ES by 64KiB (0x10000 bytes).
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_wrap:

    add si, 1
    adc di, 0
    dec cx
    jmp .read_loop

.read_done:
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    ret

; Read one sector at LBA DX:AX into ES:BX
read_sector_lba:
    push ax
    push bx
    push cx
    push dx
    push es
    push ds
    push cs
    pop ds

    ; Prefer EDD (LBA) read when available; fall back to CHS on failure.
    cmp byte [edd_ok], 0
    je .chs
    push si
    mov byte [edd_dap+0], 0x10
    mov byte [edd_dap+1], 0x00
    mov word [edd_dap+2], 0x0001
    mov word [edd_dap+4], bx
    mov word [edd_dap+6], es
    mov word [edd_dap+8], ax
    mov word [edd_dap+10], dx
    mov dword [edd_dap+12], 0
    mov si, edd_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    pop si
    jnc .done
.chs:

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

.done:
    pop ds
    pop es
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
    mov si, msg_reboot
    call print_string
    xor ax, ax
    int 0x16
    int 0x19
    jmp $


a20_support_error:
    jmp $

; --------------------
; Data
; --------------------

boot_drive        db 0
kernel_name       db 'KERNEL', 0x20, 0x20, 'BIN'


bpb_bytes_per_sec dw 0
bpb_sec_per_clus  db 0
bpb_res_sectors   dw 0
bpb_fats          db 0
bpb_root_ents     dw 0
bpb_fat_secs      dw 0
bpb_fat_secs_hi   dw 0 ; High word for FAT32
bpb_sec_per_track dw 0
bpb_heads         dw 0
fat_type          db 0 ; 0=FAT12, 1=FAT16, 2=FAT32
fat32_root_cluster dw 0

vol_base_lba_lo   dw 0
vol_base_lba_hi   dw 0

disk_spt          dw 0
disk_heads        dw 0
edd_ok            db 0
edd_dap           times 16 db 0

root_dir_sectors  dw 0
fat_start_lba_lo  dw 0
fat_start_lba_hi  dw 0
root_start_lba_lo dw 0
root_start_lba_hi dw 0
data_start_lba_lo dw 0
data_start_lba_hi dw 0

cur_cluster       dd 0
cur_cluster_orig  dd 0

kernel_size_low   dw 0
kernel_size_high  dw 0
cur_lba_lo        dw 0
cur_lba_hi        dw 0

serial_init:
    push ax
    push dx
    ; Init COM1 (Port 0x3F8)
    mov dx, 0x3F8 + 1 ; Interrupt Enable
    mov al, 0
    out dx, al
    mov dx, 0x3F8 + 3 ; Line Control (DLAB=1)
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0 ; Divisor Lo (9600 baud = 115200 / 12)
    mov al, 12
    out dx, al
    mov dx, 0x3F8 + 1 ; Divisor Hi
    mov al, 0
    out dx, al
    mov dx, 0x3F8 + 3 ; Line Control (8N1)
    mov al, 0x03
    out dx, al
    pop dx
    pop ax
    ret

print_char:
    push bx
    push ds
    push dx
    push es
    push ax
    
    ; BIOS Output
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    
    ; Serial Output
    pop ax ; restore AL
    push ax
    mov dx, 0x3F8 + 5 ; LSR
.wait_tx:
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov dx, 0x3F8 + 0 ; THR
    pop ax
    out dx, al
    push ax ; balance stack
    
    pop ax
    pop es
    pop dx
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
msg_reboot              db 'Press key to reboot...', 13, 10, 0
msg_sima_start          db 'Sima Stage2 Started.', 13, 10, 0
msg_bpb_ok              db 'BPB Read.', 13, 10, 0
msg_root_load           db 'Loading RootDir...', 13, 10, 0
msg_kernel_search       db 'Searching Kernel...', 13, 10, 0
msg_kernel_jump         db 'Jumping to kernel...', 13, 10, 0

print_hex_byte:
    push ax
    push bx
    mov bl, al
    shr al, 4
    call .hex_nibble
    mov al, bl
    and al, 0x0F
    call .hex_nibble
    pop bx
    pop ax
    ret
.hex_nibble:
    add al, '0'
    cmp al, '9'
    jbe .print
    add al, 7
.print:
    call print_char
    ret

print_word_hex:
    push ax
    push ax
    mov al, ah
    call print_hex_byte
    pop ax
    call print_hex_byte
    pop ax
    ret





%include "a20.asm"
