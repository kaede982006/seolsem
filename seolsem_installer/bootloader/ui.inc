; ------------------------------------------------------------
; Seolsem Installer TUI helpers (VGA text mode, 80x25)
; - 16-bit real mode
; - Writes directly to VRAM (0xB800)
; ------------------------------------------------------------

[bits 16]

UI_COLS        equ 80
UI_ROWS        equ 25
UI_VRAM_SEG    equ 0xB800

; Color attributes
UI_ATTR_BG         equ 0x17 ; white on blue
UI_ATTR_HDR        equ 0x1F ; bright white on blue
UI_ATTR_BORDER     equ 0x1F
UI_ATTR_TEXT       equ 0x17
UI_ATTR_STEP_CUR   equ 0x1E ; yellow on blue
UI_ATTR_STEP_DONE  equ 0x1A ; light green on blue
UI_ATTR_STATUS     equ 0x70 ; black on light gray

; Layout constants (0-based row/col)
UI_MAIN_TOP     equ 2
UI_MAIN_BOTTOM  equ 22
UI_MAIN_LEFT    equ 0
UI_MAIN_RIGHT   equ 79
UI_DIV_COL      equ 26

UI_STEP_HDR_ROW     equ 3
UI_STEP_START_ROW   equ 5
UI_STEP_LEFT_COL    equ 2
UI_STEP_CLEAR_LEFT  equ 1
UI_STEP_CLEAR_WIDTH equ 25 ; cols 1..25

UI_LOG_HDR_ROW      equ 3
UI_LOG_START_ROW    equ 4
UI_LOG_BOTTOM_ROW   equ 21
UI_LOG_LEFT_COL     equ 28
UI_LOG_RIGHT_COL    equ 78
UI_LOG_WIDTH        equ (UI_LOG_RIGHT_COL-UI_LOG_LEFT_COL+1)

UI_STATUS_ROW    equ 24
UI_PROGRESS_COL  equ 59
UI_PROGRESS_SEGS equ 14
UI_STATUS_HINT_COL equ 2
UI_STATUS_MSG_COL  equ 24
UI_STATUS_MSG_MAX  equ (UI_PROGRESS_COL-UI_STATUS_MSG_COL-1)

; ------------------------------------------------------------
; UI state
; ------------------------------------------------------------
ui_log_next_row db UI_LOG_START_ROW
ui_step_current db 0

ui_box_top      db 0
ui_box_left     db 0
ui_box_bottom   db 0
ui_box_right    db 0
ui_box_border   db 0
ui_box_fill     db 0
ui_status_src_ptr dw 0
ui_status_msg_len dw 0
ui_status_anim_limit dw 0
ui_status_offset dw 0
ui_status_scroll_buf times (UI_STATUS_MSG_MAX*2 + 96) db 0

; Row offset lookup (row * 160)
ui_row_off:
    dw 0
    dw 160
    dw 320
    dw 480
    dw 640
    dw 800
    dw 960
    dw 1120
    dw 1280
    dw 1440
    dw 1600
    dw 1760
    dw 1920
    dw 2080
    dw 2240
    dw 2400
    dw 2560
    dw 2720
    dw 2880
    dw 3040
    dw 3200
    dw 3360
    dw 3520
    dw 3680
    dw 3840

; ------------------------------------------------------------
; Static strings (in CS)
; ------------------------------------------------------------
ui_hdr_title     db 'Seolsem Installer (FAT32)',0
ui_hdr_right     db 'Target: HDD',0
ui_steps_hdr     db 'Steps',0
ui_log_hdr       db 'Log',0
ui_status_hint   db 'ESC Cancel | ENTER |',0

ui_step_labels:
    dw ui_step0, ui_step1, ui_step2, ui_step3, ui_step4, ui_step5, ui_step6
ui_step0 db 'Detect disk',0
ui_step1 db 'Partition (MBR)',0
ui_step2 db 'Format FAT32',0
ui_step3 db 'Copy boot/stage2',0
ui_step4 db 'Copy system files',0
ui_step5 db 'Create users',0
ui_step6 db 'Finalize',0

; ------------------------------------------------------------
; ui_init: set video mode 03h and clear
; ------------------------------------------------------------
ui_init:
    push ax
    push cx
    mov ax, 0x0003
    int 0x10
    ; Hide cursor by default; it is shown only during text input.
    call ui_hide_cursor
    pop cx
    pop ax
    mov bl, UI_ATTR_BG
    call ui_clear
    ret

; ------------------------------------------------------------
; ui_clear: fill entire 80x25 with spaces using BL=attr
; ------------------------------------------------------------
ui_clear:
    push ax
    push cx
    push di
    push es
    mov ax, UI_VRAM_SEG
    mov es, ax
    xor di, di
    mov ah, bl
    mov al, ' '
    mov cx, UI_COLS*UI_ROWS
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

; ------------------------------------------------------------
; ui_putc: write one cell
; IN: DH=row, DL=col, AL=ch, BL=attr
; ------------------------------------------------------------
ui_putc:
    push ax
    push bx
    push dx
    push di
    push es
    push bp

    mov ah, bl
    mov bp, ax                  ; BP = cell (ch + attr)

    mov ax, UI_VRAM_SEG
    mov es, ax
    xor ax, ax
    mov al, dh
    shl ax, 1
    mov bx, ax
    mov di, [cs:ui_row_off + bx]
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
    mov [es:di], bp

    pop bp
    pop es
    pop di
    pop dx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_puts: write zstr at position
; IN: DH=row, DL=col, BL=attr, DS:SI=zstr
; ------------------------------------------------------------
ui_puts:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    push bp
    mov ax, UI_VRAM_SEG
    mov es, ax
    xor ax, ax
    mov al, dh
    shl ax, 1
    mov bp, ax
    mov di, [cs:ui_row_off + bp]
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, bl
    stosw
    jmp .loop
.done:
    pop bp
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_puts_n: write up to CL chars from zstr
; IN: DH=row, DL=col, BL=attr, CL=max chars, DS:SI=zstr
; OUT: SI advanced to first non-written char (or NUL)
; ------------------------------------------------------------
ui_puts_n:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    push bp
    mov ax, UI_VRAM_SEG
    mov es, ax
    xor ax, ax
    mov al, dh
    shl ax, 1
    mov bp, ax
    mov di, [cs:ui_row_off + bp]
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
.loop:
    cmp cl, 0
    je .done
    lodsb
    test al, al
    jz .done
    mov ah, bl
    stosw
    dec cl
    jmp .loop
.done:
    pop bp
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_strlen: CX = length of zstr at DS:SI (does not modify SI)
; ------------------------------------------------------------
ui_strlen:
    push ax
    push si
    xor cx, cx
.sloop:
    mov al, [si]
    test al, al
    jz .sdone
    inc si
    inc cx
    jmp .sloop
.sdone:
    pop si
    pop ax
    ret

; ------------------------------------------------------------
; ui_puts_center: center string on row
; IN: DH=row, BL=attr, DS:SI=zstr
; ------------------------------------------------------------
ui_puts_center:
    push ax
    push cx
    push dx
    push si
    call ui_strlen               ; CX=len
    mov ax, UI_COLS
    sub ax, cx
    shr ax, 1
    mov dl, al
    pop si
    call ui_puts
    pop dx
    pop cx
    pop ax
    ret

; ------------------------------------------------------------
; ui_fill_row: fill a row segment with spaces
; IN: DH=row, DL=left, CL=width, BL=attr
; ------------------------------------------------------------
ui_fill_row:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    push bp
    mov ax, UI_VRAM_SEG
    mov es, ax
    xor ax, ax
    mov al, dh
    shl ax, 1
    mov bp, ax
    mov di, [cs:ui_row_off + bp]
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
    mov ah, bl
    mov al, ' '
    mov ch, 0
    rep stosw
    pop bp
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_draw_box
; IN: DH=top, DL=left, CH=bottom, CL=right, BH=border_attr, BL=fill_attr
; ------------------------------------------------------------
ui_draw_box:
    push ax
    push bx
    push cx
    push dx

    mov [ui_box_top], dh
    mov [ui_box_left], dl
    mov [ui_box_bottom], ch
    mov [ui_box_right], cl
    mov [ui_box_border], bh
    mov [ui_box_fill], bl

    ; Fill interior
    mov al, [ui_box_bottom]
    sub al, [ui_box_top]
    cmp al, 2
    jb  .border_only
    mov al, [ui_box_right]
    sub al, [ui_box_left]
    cmp al, 2
    jb  .border_only

    mov dh, [ui_box_top]
    inc dh
    mov ah, [ui_box_bottom]
    dec ah
    mov dl, [ui_box_left]
    inc dl
    mov cl, [ui_box_right]
    sub cl, [ui_box_left]
    dec cl                      ; width = right-left-1
    mov bl, [ui_box_fill]
.fill_loop:
    cmp dh, ah
    ja  .border_only
    call ui_fill_row
    inc dh
    jmp .fill_loop

.border_only:
    mov bl, [ui_box_border]

    ; Top border
    mov dh, [ui_box_top]
    mov dl, [ui_box_left]
    mov al, 0xDA               ; TL
    call ui_putc
    mov dl, [ui_box_right]
    mov al, 0xBF               ; TR
    call ui_putc
    mov dl, [ui_box_left]
    inc dl
    mov al, 0xC4               ; H
.top_h:
    cmp dl, [ui_box_right]
    jae .top_done
    call ui_putc
    inc dl
    jmp .top_h
.top_done:

    ; Bottom border
    mov dh, [ui_box_bottom]
    mov dl, [ui_box_left]
    mov al, 0xC0               ; BL
    call ui_putc
    mov dl, [ui_box_right]
    mov al, 0xD9               ; BR
    call ui_putc
    mov dl, [ui_box_left]
    inc dl
    mov al, 0xC4
.bot_h:
    cmp dl, [ui_box_right]
    jae .bot_done
    call ui_putc
    inc dl
    jmp .bot_h
.bot_done:

    ; Vertical sides
    mov dh, [ui_box_top]
    inc dh
    mov ah, [ui_box_bottom]
    dec ah
    cmp dh, ah
    ja  .done
    mov al, 0xB3               ; V
.vloop:
    mov dl, [ui_box_left]
    call ui_putc
    mov dl, [ui_box_right]
    call ui_putc
    inc dh
    cmp dh, ah
    jbe .vloop

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_draw_base: header + main panels + status bar
; ------------------------------------------------------------
ui_draw_base:
    push ax
    push bx
    push cx
    push dx
    push si

    mov bl, UI_ATTR_BG
    call ui_clear

    ; Header bar row 0
    mov dh, 0
    mov dl, 0
    mov cl, UI_COLS
    mov bl, UI_ATTR_HDR
    call ui_fill_row
    mov dh, 0
    mov dl, 2
    mov bl, UI_ATTR_HDR
    mov si, ui_hdr_title
    call ui_puts
    mov dh, 0
    mov dl, 60
    mov bl, UI_ATTR_HDR
    mov si, ui_hdr_right
    call ui_puts

    ; Separator row 1 (line)
    mov dh, 1
    mov dl, 0
    mov cl, UI_COLS
    mov bl, UI_ATTR_HDR
    call ui_fill_row
    mov dh, 1
    mov dl, 0
    mov bl, UI_ATTR_HDR
    mov al, 0xC4
    mov cx, UI_COLS
.sep_loop:
    call ui_putc
    inc dl
    loop .sep_loop

    ; Main outer box
    mov dh, UI_MAIN_TOP
    mov dl, UI_MAIN_LEFT
    mov ch, UI_MAIN_BOTTOM
    mov cl, UI_MAIN_RIGHT
    mov bh, UI_ATTR_BORDER
    mov bl, UI_ATTR_BG
    call ui_draw_box

    ; Divider
    mov bl, UI_ATTR_BORDER
    mov al, 0xB3
    mov dl, UI_DIV_COL
    mov dh, (UI_MAIN_TOP+1)
    mov ah, (UI_MAIN_BOTTOM-1)
.div_loop:
    cmp dh, ah
    ja  .div_done
    call ui_putc
    inc dh
    jmp .div_loop
.div_done:

    ; Panel headings
    mov dh, UI_STEP_HDR_ROW
    mov dl, UI_STEP_LEFT_COL
    mov bl, UI_ATTR_BORDER
    mov si, ui_steps_hdr
    call ui_puts
    mov dh, UI_LOG_HDR_ROW
    mov dl, UI_LOG_LEFT_COL
    mov bl, UI_ATTR_BORDER
    mov si, ui_log_hdr
    call ui_puts

    ; Status bar row 24 (initial)
    mov dh, UI_STATUS_ROW
    mov dl, 0
    mov cl, UI_COLS
    mov bl, UI_ATTR_STATUS
    call ui_fill_row
    mov dh, UI_STATUS_ROW
    mov dl, UI_STATUS_HINT_COL
    mov bl, UI_ATTR_STATUS
    mov si, ui_status_hint
    call ui_puts

    ; Reset log cursor
    mov byte [ui_log_next_row], UI_LOG_START_ROW

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_status_set: register status message and prepare marquee buffer
; IN: DS:SI=zstr
; ------------------------------------------------------------
ui_status_set:
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

    mov [ui_status_src_ptr], si
    mov si, [ui_status_src_ptr]
    call ui_strlen
    mov [ui_status_msg_len], cx

    ; Build: [spaces(max)] + [message] + [spaces(max)] + NUL
    mov ax, ds
    mov es, ax
    cld
    mov di, ui_status_scroll_buf
    mov al, ' '
    mov cx, UI_STATUS_MSG_MAX
    rep stosb

    mov si, [ui_status_src_ptr]
    mov cx, [ui_status_msg_len]
    rep movsb

    mov al, ' '
    mov cx, UI_STATUS_MSG_MAX
    rep stosb
    mov al, 0
    stosb

    mov ax, [ui_status_msg_len]
    add ax, UI_STATUS_MSG_MAX
    mov [ui_status_anim_limit], ax
    mov word [ui_status_offset], 0

    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_status_tick: render one marquee frame (right -> left)
; ------------------------------------------------------------
ui_status_tick:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds

    push cs
    pop ds

    ; Clear status row and draw hint every frame.
    mov dh, UI_STATUS_ROW
    mov dl, 0
    mov cl, UI_STATUS_MSG_MAX  ; Only clear to start of progress bar area
    add cl, UI_STATUS_MSG_COL 
    mov bl, UI_ATTR_STATUS
    call ui_fill_row

    mov dh, UI_STATUS_ROW
    mov dl, UI_STATUS_HINT_COL
    mov bl, UI_ATTR_STATUS
    mov si, ui_status_hint
    call ui_puts

    mov si, ui_status_scroll_buf
    add si, [ui_status_offset]
    mov dh, UI_STATUS_ROW
    mov dl, UI_STATUS_MSG_COL
    mov cl, UI_STATUS_MSG_MAX
    mov bl, UI_ATTR_STATUS
    call ui_puts_n

    mov ax, [ui_status_offset]
    inc ax
    cmp ax, [ui_status_anim_limit]
    jbe .save
    xor ax, ax
.save:
    mov [ui_status_offset], ax

    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_set_cursor: move hardware cursor
; IN: DH=row, DL=col
; ------------------------------------------------------------
ui_set_cursor:
    push ax
    push bx
    push dx
    mov bh, 0
    mov ah, 0x02
    int 0x10
    pop dx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_hide_cursor: make hardware cursor invisible
; ------------------------------------------------------------
ui_hide_cursor:
    push ax
    push cx
    mov ah, 0x01
    mov ch, 0x20           ; bit 5 set = cursor off
    xor cl, cl
    int 0x10
    pop cx
    pop ax
    ret

; ------------------------------------------------------------
; ui_show_cursor: make hardware cursor visible (scanlines 6-7)
; ------------------------------------------------------------
ui_show_cursor:
    push ax
    push cx
    mov ah, 0x01
    mov cx, 0x0607
    int 0x10
    pop cx
    pop ax
    ret

; Compatibility wrapper used by existing installer code.
; IN: DS:SI=zstr
ui_status:
    call ui_status_set
    call ui_status_tick
    ret

; ------------------------------------------------------------
; ui_progress: draw progress bar on status bar
; IN: AL=percent (0..100)
; ------------------------------------------------------------
ui_progress:
    push ax
    push bx
    push cx
    push dx
    push si

    mov bh, al                   ; save percent

    ; filled = percent * UI_PROGRESS_SEGS / 100
    xor ah, ah
    mov bl, UI_PROGRESS_SEGS
    mul bl                       ; AX = percent * segs
    mov bl, 100
    div bl                       ; AL = filled
    xor ch, ch
    mov cl, al                   ; CX = filled

    ; Draw "["
    mov dh, UI_STATUS_ROW
    mov dl, UI_PROGRESS_COL
    mov bl, UI_ATTR_STATUS
    mov al, '['
    call ui_putc

    ; Segments
    mov si, 0
    mov dl, UI_PROGRESS_COL
    inc dl
.seg_loop:
    cmp si, UI_PROGRESS_SEGS
    jae .seg_done
    mov al, '-'
    cmp si, cx
    jae .seg_put
    mov al, '#'
.seg_put:
    call ui_putc
    inc dl
    inc si
    jmp .seg_loop
.seg_done:

    ; "] "
    mov al, ']'
    call ui_putc
    inc dl
    mov al, ' '
    call ui_putc
    inc dl

    ; Percent "NNN%"
    mov al, bh
    xor ah, ah
    mov bl, 100
    div bl                       ; AL=hundreds, AH=rem
    mov ch, al                   ; hundreds
    mov al, ah
    xor ah, ah
    mov bl, 10
    div bl                       ; AL=tens, AH=ones
    mov cl, al                   ; tens
    mov bh, ah                   ; ones

    ; hundreds
    mov al, ' '
    cmp ch, 0
    je  .h_put
    mov al, ch
    add al, '0'
.h_put:
    call ui_putc
    inc dl
    ; tens
    mov al, ' '
    cmp ch, 0
    jne .t_digit
    cmp cl, 0
    je  .t_put
.t_digit:
    mov al, cl
    add al, '0'
.t_put:
    call ui_putc
    inc dl
    ; ones
    mov al, bh
    add al, '0'
    call ui_putc
    inc dl
    ; %
    mov al, '%'
    call ui_putc

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_log_clear_line: clear one log line (row in DH)
; ------------------------------------------------------------
ui_log_clear_line:
    push ax
    push cx
    push dx
    mov dl, UI_LOG_LEFT_COL
    mov cl, UI_LOG_WIDTH
    mov bl, UI_ATTR_TEXT
    call ui_fill_row
    pop dx
    pop cx
    pop ax
    ret

; ------------------------------------------------------------
; ui_log_scroll: scroll log area up by 1 line
; ------------------------------------------------------------
ui_log_scroll:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ax, UI_VRAM_SEG
    mov ds, ax
    mov es, ax

    mov dh, UI_LOG_START_ROW
.row_loop:
    cmp dh, UI_LOG_BOTTOM_ROW
    jae .clear_last

    ; src = row(dh+1)
    mov al, dh
    inc al
    xor ah, ah
    shl ax, 1
    mov bx, ax
    mov si, [cs:ui_row_off + bx]
    ; dst = row(dh)
    mov al, dh
    xor ah, ah
    shl ax, 1
    mov bx, ax
    mov di, [cs:ui_row_off + bx]

    mov ax, UI_LOG_LEFT_COL
    shl ax, 1
    add si, ax
    add di, ax

    mov cx, UI_LOG_WIDTH
    rep movsw

    inc dh
    jmp .row_loop

.clear_last:
    ; Clear bottom line
    mov al, UI_LOG_BOTTOM_ROW
    xor ah, ah
    shl ax, 1
    mov bx, ax
    mov di, [cs:ui_row_off + bx]
    mov ax, UI_LOG_LEFT_COL
    shl ax, 1
    add di, ax
    mov ah, UI_ATTR_TEXT
    mov al, ' '
    mov cx, UI_LOG_WIDTH
    rep stosw

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_log_push: append message to log panel (handles CR/LF)
; IN: DS:SI=zstr
; ------------------------------------------------------------
ui_log_push:
    push ax
    push bx
    push cx
    push dx
    push si

    mov dh, [ui_log_next_row]
.new_line:
    cmp dh, UI_LOG_BOTTOM_ROW
    jbe .ok_row
    call ui_log_scroll
    mov dh, UI_LOG_BOTTOM_ROW
.ok_row:
    call ui_log_clear_line
    mov dl, UI_LOG_LEFT_COL

.char_loop:
    lodsb
    test al, al
    jz .finish
    cmp al, 13
    je  .break
    cmp al, 10
    je  .break
    cmp dl, (UI_LOG_RIGHT_COL+1)
    jae .break
    mov bl, UI_ATTR_TEXT
    call ui_putc
    inc dl
    jmp .char_loop

.break:
    ; consume LF after CR if present
    cmp al, 13
    jne .next_line
    cmp byte [si], 10
    jne .next_line
    inc si
.next_line:
    inc dh
    jmp .new_line

.finish:
    inc dh
    mov [ui_log_next_row], dh
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; ui_steps_render: render steps list
; IN: AL=current_step (0..6), BX=done_mask (bit i)
; ------------------------------------------------------------
ui_steps_render:
    push ax
    push bx
    push bp
    push cx
    push dx
    push si
    push di

    mov [ui_step_current], al
    mov bp, bx                   ; Preserve done_mask; BL is reused for colors.

    ; Clear steps area (rows 4..21, cols 1..25)
    mov dh, 4
.clr_rows:
    mov dl, UI_STEP_CLEAR_LEFT
    mov cl, UI_STEP_CLEAR_WIDTH
    mov bl, UI_ATTR_BG
    call ui_fill_row
    inc dh
    cmp dh, UI_LOG_BOTTOM_ROW
    jbe .clr_rows

    xor di, di                   ; step index
.step_loop:
    cmp di, 7
    jae .done
    mov dh, UI_STEP_START_ROW
    mov ax, di
    add dh, al

    ; Determine attr for this step
    mov bl, UI_ATTR_TEXT
    mov cx, di
    mov ax, 1
    shl ax, cl
    test bp, ax
    jz .not_done
    mov bl, UI_ATTR_STEP_DONE
.not_done:
    mov al, [ui_step_current]
    xor ah, ah
    cmp ax, di
    jne .not_cur
    mov bl, UI_ATTR_STEP_CUR
.not_cur:

    ; Current indicator
    mov dl, UI_STEP_LEFT_COL
    mov al, [ui_step_current]
    xor ah, ah
    cmp ax, di
    mov al, ' '
    jne .ind_put
    mov al, '>'
.ind_put:
    call ui_putc

    ; Done checkmark
    inc dl
    mov al, '-'
    push ax                     ; preserve AL while computing mask in AX
    mov cx, di
    mov ax, 1
    shl ax, cl
    test bp, ax
    pop ax
    jz .chk_put
    mov al, 'V'
.chk_put:
    call ui_putc

    ; Space
    inc dl
    mov al, ' '
    call ui_putc

    ; Label
    inc dl
    mov si, di
    shl si, 1
    mov si, [cs:ui_step_labels + si]
    call ui_puts

    inc di
    jmp .step_loop

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bp
    pop bx
    pop ax
    ret
