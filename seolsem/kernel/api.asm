%ifndef __API__
%define __API__

[bits 16]

; ============================================================
; Kernel API (16-bit Protected Mode)
;   - VGA text output (0xB8000 via PM_VIDEO_SEL)
;   - i8042 keyboard polling (scancode set 1, minimal US map)
;   - No BIOS INT calls (unavailable in protected mode)
; ============================================================

segment _TEXT class=CODE use16

%define INPUT_MAX 64
%define KBD_QUEUE_SIZE 32

; Selectors set up by kernel_entry.asm (GDT)
%define PM_VIDEO_SEL 0x18

; VGA text-mode cursor control (CRTC)
%define VGA_CRTC_INDEX_C 0x3D4
%define VGA_CRTC_DATA_C  0x3D5
%define VGA_CRTC_INDEX_M 0x3B4
%define VGA_CRTC_DATA_M  0x3B5
%define VGA_CURSOR_LOW 0x0F
%define VGA_CURSOR_HIGH 0x0E
%define VGA_CURSOR_START 0x0A
%define VGA_CURSOR_END   0x0B

; VGA Miscellaneous Output Register (read)
; Bit 0 selects CRTC I/O base: 1 => color (0x3D4/0x3D5), 0 => mono (0x3B4/0x3B5)
%define VGA_MISC_READ 0x3CC

; i8042 keyboard controller
%define KBD_STATUS 0x64
%define KBD_DATA   0x60

global _clear_screen
global _print_message
global _wait_prompt
global _read_key
global _set_cursor
global _write_char
global _sync_ds
global _enable_irq
global _disable_irq
global _kbd_isr

; ------------------------------------------------------------
; Screen
; ------------------------------------------------------------
_clear_screen:
    pusha
    push es

    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    xor  di, di
    mov  ax, 0x0720         ; space + gray
    mov  cx, 80*25
    cld
    rep  stosw

    mov  word [_line], 0
    mov  word [_di_pos], 0
    xor  dx, dx
    call set_cursor_hw
    call show_cursor_hw

    pop  es
    popa
    ret

_set_cursor:
    push bp
    mov  bp, sp
    mov  dh, [bp+4]
    mov  dl, [bp+6]
    call set_cursor_hw
    pop  bp
    ret

_write_char:
    push bp
    mov  bp, sp
    push ax
    push bx
    push es

    mov  ax, PM_VIDEO_SEL
    mov  es, ax

    mov  ax, [bp+4]         ; row
    mov  bx, 160
    mul  bx                 ; AX = row * 160
    mov  bx, [bp+6]         ; col
    shl  bx, 1
    add  ax, bx
    mov  di, ax

    mov  al, [bp+8]         ; ch
    mov  ah, [bp+10]        ; attr
    mov  [es:di], ax

    pop  es
    pop  bx
    pop  ax
    pop  bp
    ret

; ------------------------------------------------------------
; DS/IRQ helpers
; ------------------------------------------------------------
_sync_ds:
    mov  ax, ss
    mov  ds, ax
    ret

_enable_irq:
    sti
    ret

_disable_irq:
    cli
    ret

; ------------------------------------------------------------
; Keyboard
; ------------------------------------------------------------
; UINT16 read_key(void)
; returns AX = (scan << 8) | ascii
_read_key:
    push bx
    push cx
    push dx
    push ds

    ; Use SS as DGROUP for keyboard state vars.
    mov  ax, ss
    mov  ds, ax

.wait_key:
    ; 1) Prefer queued IRQ events (fast path)
    cli
    mov  bl, [_kbd_q_head]
    cmp  bl, [_kbd_q_tail]
    jne  .have_key
    sti

    ; 2) Poll i8042 as a fallback (works even if IRQ/IDT is misconfigured)
.poll:
    in   al, KBD_STATUS
    test al, 0x01
    jz   .wait_key
    test al, 0x20                 ; AUX data (mouse) -> consume and ignore
    jnz  .poll_consume
    cli
    in   al, KBD_DATA
    ; Minimal translation/state update mirroring ISR rules:
    ; - handle 0xE0 prefix
    ; - update shift/ctrl state
    ; - ignore break codes
    ; - map to ASCII via tables
    cmp  al, 0xE0
    jne  .p_not_ext
    mov  byte [_kbd_ext], 1
    sti
    jmp  .wait_key
.p_not_ext:
    cmp  al, 0x2A
    je   .p_shift_on
    cmp  al, 0x36
    je   .p_shift_on
    cmp  al, 0xAA
    je   .p_shift_off
    cmp  al, 0xB6
    je   .p_shift_off
    cmp  al, 0x1D
    je   .p_ctrl_on
    cmp  al, 0x9D
    je   .p_ctrl_off
    test al, 0x80
    jnz  .p_clear_ext

    mov  ah, al                   ; scan
    cmp  byte [_kbd_ext], 0
    jne  .p_ext_key
    xor  bx, bx
    mov  bl, al
    cmp  byte [_kbd_shift], 0
    je   .p_unshift
    mov  al, [cs:kbd_map_shift + bx]
    jmp  .p_apply_ctrl
.p_unshift:
    mov  al, [cs:kbd_map + bx]
.p_apply_ctrl:
    cmp  byte [_kbd_ctrl], 0
    je   .p_emit
    cmp  al, 'A'
    jb   .p_emit
    cmp  al, 'Z'
    jbe  .p_ctrl_map
    cmp  al, 'a'
    jb   .p_emit
    cmp  al, 'z'
    ja   .p_emit
.p_ctrl_map:
    and  al, 0x1F
.p_emit:
    mov  byte [_kbd_ext], 0
    sti
    jmp  .done

.p_ext_key:
    mov  byte [_kbd_ext], 0
    xor  ax, ax
    sti
    jmp  .wait_key

.p_shift_on:
    mov  byte [_kbd_shift], 1
    jmp  .p_clear_ext
.p_shift_off:
    mov  byte [_kbd_shift], 0
    jmp  .p_clear_ext
.p_ctrl_on:
    mov  byte [_kbd_ctrl], 1
    jmp  .p_clear_ext
.p_ctrl_off:
    mov  byte [_kbd_ctrl], 0
    jmp  .p_clear_ext
.p_clear_ext:
    mov  byte [_kbd_ext], 0
    sti
    jmp  .wait_key

.poll_consume:
    in   al, KBD_DATA
    jmp  .wait_key

.have_key:
    mov  bl, [_kbd_q_tail]
    xor  bh, bh
    shl  bx, 1
    mov  ax, [_kbd_queue + bx]
    mov  bl, [_kbd_q_tail]
    inc  bl
    and  bl, (KBD_QUEUE_SIZE - 1)
    mov  [_kbd_q_tail], bl
    sti

.done:
    pop  ds
    pop  dx
    pop  cx
    pop  bx
    ret

; IRQ1 keyboard handler: push translated key events into ring buffer.
_kbd_isr:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    mov  ax, ss
    mov  ds, ax

    ; Drain the controller output buffer.
    ; Some keys emit multi-byte sequences; draining prevents missed edges.
.drain:
    in   al, KBD_STATUS
    test al, 0x01
    jz   .eoi
    test al, 0x20                 ; AUX (mouse) data?
    jz   .read_kbd
    in   al, KBD_DATA             ; consume and ignore
    jmp  .drain
.read_kbd:
    in   al, KBD_DATA

    cmp  al, 0xE0
    jne  .not_ext_prefix
    mov  byte [_kbd_ext], 1
    jmp  .drain
.not_ext_prefix:

    ; Shift press/release
    cmp  al, 0x2A
    je   .shift_on
    cmp  al, 0x36
    je   .shift_on
    cmp  al, 0xAA
    je   .shift_off
    cmp  al, 0xB6
    je   .shift_off

    ; Ctrl press/release
    cmp  al, 0x1D
    je   .ctrl_on
    cmp  al, 0x9D
    je   .ctrl_off

    ; Ignore break codes for regular keys
    test al, 0x80
    jnz  .clear_ext_only

    ; AX = event (scan<<8 | ascii)
    mov  ah, al
    cmp  byte [_kbd_ext], 0
    jne  .ext_key

    xor  bx, bx
    mov  bl, al
    cmp  byte [_kbd_shift], 0
    je   .use_unshift_isr
    mov  al, [cs:kbd_map_shift + bx]
    jmp  .apply_ctrl_isr
.use_unshift_isr:
    mov  al, [cs:kbd_map + bx]

.apply_ctrl_isr:
    cmp  byte [_kbd_ctrl], 0
    je   .queue_event
    cmp  al, 'A'
    jb   .queue_event
    cmp  al, 'Z'
    jbe  .ctrl_map_isr
    cmp  al, 'a'
    jb   .queue_event
    cmp  al, 'z'
    ja   .queue_event
.ctrl_map_isr:
    and  al, 0x1F
    jmp  .queue_event

.ext_key:
    mov  byte [_kbd_ext], 0
    xor  al, al
    jmp  .queue_event

.queue_event:
    mov  bl, [_kbd_q_head]
    mov  cl, bl
    inc  cl
    and  cl, (KBD_QUEUE_SIZE - 1)
    cmp  cl, [_kbd_q_tail]
    je   .overflow
    xor  bh, bh
    shl  bx, 1
    mov  [_kbd_queue + bx], ax
    mov  [_kbd_q_head], cl
    jmp  .drain

.overflow:
    mov  byte [_kbd_q_overflow], 1
    jmp  .drain

.shift_on:
    mov  byte [_kbd_shift], 1
    jmp  .clear_ext_only
.shift_off:
    mov  byte [_kbd_shift], 0
    jmp  .clear_ext_only
.ctrl_on:
    mov  byte [_kbd_ctrl], 1
    jmp  .clear_ext_only
.ctrl_off:
    mov  byte [_kbd_ctrl], 0
    jmp  .clear_ext_only

.clear_ext_only:
    mov  byte [_kbd_ext], 0
    jmp  .drain

.eoi:
    mov  al, 0x20
    out  0x20, al
    pop  es
    pop  ds
    pop  bp
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    iret

; ------------------------------------------------------------
; Prompt input
; ------------------------------------------------------------
_wait_prompt:
    push bp
    mov  bp, sp
    pusha
    push ds
    push es

    mov  ax, ss
    mov  ds, ax

    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    cld
    call show_cursor_hw

    ; --- current line check (+scroll) ---
    mov  ax, [_line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  word [_line], 24
.line_ok:
    mov  al, [_line]
    mov  [_input_row], al
    mov  byte [_input_col], 0

    ; print prompt string
    mov  si, [bp+4]
.prompt_loop:
    mov  cl, [ss:si]
    cmp  cl, 0
    je   .start_input
    call .put_char_cl
    inc  si
    jmp  .prompt_loop

.start_input:
    mov  al, [_input_row]
    mov  [_input_start_row], al
    mov  al, [_input_col]
    mov  [_input_start_col], al

    mov  si, [bp+6]         ; buf
    mov  byte [ss:si], 0

.update_cursor:
    call .sync_input_pos
    mov  dh, [_input_row]
    mov  dl, [_input_col]
    call set_cursor_hw

.poll:
    call _read_key

    cmp  al, 13
    je   .end_line
    cmp  al, 8
    je   .backspace

    cmp  al, 32
    jb   .poll
    cmp  al, 127
    je   .poll

    ; enforce input max (INPUT_MAX - 1)
    push ax
    mov  ax, si
    sub  ax, [bp+6]
    cmp  ax, (INPUT_MAX - 1)
    pop  ax
    jae  .poll

    call .sync_input_pos

    ; avoid writing beyond bottom-right cell
    mov  bl, [_input_row]
    cmp  bl, 24
    jne  .store_char
    mov  bl, [_input_col]
    cmp  bl, 79
    jae  .poll

.store_char:
    mov  bl, al
    mov  cl, al
    call .put_char_cl
    mov  [ss:si], bl
    inc  si
    mov  byte [ss:si], 0
    jmp  .update_cursor

.backspace:
    cmp  si, [bp+6]
    jbe  .poll
    dec  si
    mov  byte [ss:si], 0
    call .sync_input_pos
    call .calc_di
    mov  byte [es:di], 0x20
    mov  byte [es:di+1], 0x07
    jmp  .update_cursor

.end_line:
    call .sync_input_pos
    mov  al, [_input_row]
    inc  al
    cmp  al, 25
    jb   .line_set
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  al, 24
.line_set:
    xor  ah, ah
    mov  [_line], ax
    mov  dh, al
    xor  dl, dl
    call set_cursor_hw
    pop  es
    pop  ds
    popa
    pop  bp
    ret

.put_char_cl:
    push ax
    push di
    call .calc_di
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07

    ; Advance cursor position
    mov  al, [_input_col]
    cmp  al, 79
    jae  .at_right_edge
    ; Not at right edge - just advance column
    inc  byte [_input_col]
    jmp  .advance_done

.at_right_edge:
    ; At column 79 (right edge)
    mov  al, [_input_row]
    cmp  al, 24
    jae  .at_bottom_right
    ; Not at bottom row - wrap to next line
    mov  byte [_input_col], 0
    inc  byte [_input_row]
    jmp  .advance_done

.at_bottom_right:
    ; At row 24, col 79 - don't advance beyond screen
    ; Keep cursor at (24, 79)
    jmp  .advance_done

.advance_done:
    pop  di
    pop  ax
    ret

.sync_input_pos:
    push ax
    push bx
    push cx

    mov  ax, si
    sub  ax, [bp+6]
    mov  bl, [_input_start_row]
    mov  cl, [_input_start_col]

.sync_loop:
    cmp  ax, 0
    je   .sync_done
    cmp  cl, 79
    jb   .sync_col_inc
    cmp  bl, 24
    jae  .sync_clamp
    mov  cl, 0
    inc  bl
    dec  ax
    jmp  .sync_loop

.sync_col_inc:
    inc  cl
    dec  ax
    jmp  .sync_loop

.sync_clamp:
    mov  bl, 24
    mov  cl, 79
    xor  ax, ax
    jmp  .sync_done

.sync_done:
    mov  [_input_row], bl
    mov  [_input_col], cl
    pop  cx
    pop  bx
    pop  ax
    ret

.calc_di:
    push ax
    push bx
    xor  ax, ax
    mov  al, [_input_row]
    mov  bx, 160
    mul  bx
    xor  bx, bx
    mov  bl, [_input_col]
    shl  bx, 1
    add  ax, bx
    mov  di, ax
    pop  bx
    pop  ax
    ret

; ------------------------------------------------------------
; _print_message
; ------------------------------------------------------------
_print_message:
    push bp
    mov  bp, sp
    pusha
    push ds
    push es

    mov  ax, ss
    mov  ds, ax

    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  bl, 0

    mov  ax, [_line]
    cmp  ax, 25
    jb   .line_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  word [_line], 24
.line_ok:

    mov  ax, [_line]
    mov  bx, 160
    mul  bx
    mov  [_di_pos], ax
    mov  di, [_di_pos]

    mov  si, [bp+4]

.print_loop:
    mov  cl, [ss:si]
    test cl, cl
    jz   .end
    cmp  cl, 10
    je   .newline
    cmp  cl, 13
    je   .carriage_return
    mov  [es:di], cl
    mov  byte [es:di+1], 0x07
    inc  si
    add  di, 2
    mov  bl, 0
    mov  ax, di
    sub  ax, [_di_pos]
    cmp  ax, 160
    jb   .print_loop
    jmp  .wrap
.newline:
    call .advance_line
    inc  si
    jmp  .print_loop
.carriage_return:
    mov  ax, [_line]
    mov  bx, 160
    mul  bx
    mov  [_di_pos], ax
    mov  di, [_di_pos]
    mov  bl, 1
    inc  si
    jmp  .print_loop
.wrap:
    call .advance_line
    jmp  .print_loop
.advance_line:
    inc  word [_line]
    mov  ax, [_line]
    cmp  ax, 25
    jb   .advance_ok
    call scroll_screen
    mov  ax, ss
    mov  ds, ax
    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  word [_line], 24
.advance_ok:
    mov  ax, [_line]
    mov  bx, 160
    mul  bx
    mov  [_di_pos], ax
    mov  di, [_di_pos]
    mov  bl, 1
    ret
.end:
    cmp  bl, 1
    je   .done
    inc  word [_line]
.done:
    pop  es
    pop  ds
    popa
    pop  bp
    ret

; DH=row, DL=col
set_cursor_hw:
    ; OSDev-compatible cursor movement:
    ; pos = row * 80 + col
    ; outb(CRTC_INDEX, 0x0F); outb(CRTC_DATA, pos & 0xFF);
    ; outb(CRTC_INDEX, 0x0E); outb(CRTC_DATA, (pos >> 8) & 0xFF);
    pusha

    xor  ax, ax
    mov  al, dh
    mov  bl, 80
    mul  bl                ; AX = row * 80
    xor  bx, bx
    mov  bl, dl
    add  bx, ax            ; BX = pos (offset)

    call get_crtc_base      ; DX = CRTC index port base

    mov  al, VGA_CURSOR_LOW
    out  dx, al
    inc  dx
    mov  al, bl
    out  dx, al

    dec  dx
    mov  al, VGA_CURSOR_HIGH
    out  dx, al
    inc  dx
    mov  al, bh
    out  dx, al

    popa
    ret

; Returns DX = CRTC index port base (0x3D4 color, 0x3B4 mono)
get_crtc_base:
    ; Read VGA Misc Output Register (0x3CC) to select the active CRTC base.
    ; Note: immediate-port form of IN only supports 8-bit ports, so we must use DX.
    push ax
    push dx

    mov  dx, VGA_MISC_READ
    in   al, dx

    pop  dx
    mov  dx, VGA_CRTC_INDEX_C
    test al, 0x01
    jnz  .done
    mov  dx, VGA_CRTC_INDEX_M
.done:
    pop  ax
    ret

hide_cursor_hw:
    ; OSDev-compatible cursor disable:
    ; outb(CRTC_INDEX, 0x0A); outb(CRTC_DATA, 0x20);
    pusha
    call get_crtc_base
    mov  al, VGA_CURSOR_START
    out  dx, al
    inc  dx
    mov  al, 0x20
    out  dx, al
    popa
    ret

; ------------------------------------------------------------
; Software cursor (attribute highlight) for environments where
; VGA hardware cursor updates are unreliable.
; Uses ES=PM_VIDEO_SEL (0xB800 text memory) as set by callers.
; ------------------------------------------------------------
soft_cursor_calc_di:
    push ax
    push bx
    xor  ax, ax
    mov  al, dh
    mov  bx, 160
    mul  bx
    mov  di, ax
    xor  ax, ax
    mov  al, dl
    shl  ax, 1
    add  di, ax
    pop  bx
    pop  ax
    ret

soft_cursor_erase:
    pusha
    cmp  byte [_soft_cur_active], 0
    je   .done
    mov  dh, [_soft_cur_row]
    mov  dl, [_soft_cur_col]
    call soft_cursor_calc_di
    mov  al, [_soft_cur_ch]
    mov  [es:di], al
    mov  al, [_soft_cur_attr]
    mov  [es:di+1], al
    mov  byte [_soft_cur_active], 0
.done:
    popa
    ret

soft_cursor_draw:
    pusha
    call soft_cursor_calc_di
    mov  al, [es:di]
    mov  [_soft_cur_ch], al
    mov  al, [es:di+1]
    mov  [_soft_cur_attr], al
    mov  byte [es:di+1], 0x70     ; black on gray background
    mov  [_soft_cur_row], dh
    mov  [_soft_cur_col], dl
    mov  byte [_soft_cur_active], 1
    popa
    ret

; DH=row, DL=col
soft_cursor_move:
    pusha
    push dx
    call soft_cursor_erase
    pop  dx
    call soft_cursor_draw
    popa
    ret

show_cursor_hw:
    ; OSDev-compatible cursor enable with preserved reserved bits:
    ; outb(CRTC_INDEX, 0x0A);
    ; outb(CRTC_DATA, (inb(CRTC_DATA) & 0xC0) | cursor_start);
    ; outb(CRTC_INDEX, 0x0B);
    ; outb(CRTC_DATA, (inb(CRTC_DATA) & 0xE0) | cursor_end);
    pusha
    call get_crtc_base

    ; Cursor start scanline = 14 (underline), preserve bits 6-7
    mov  al, VGA_CURSOR_START
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0xC0
    or   al, 0x0E
    out  dx, al

    ; Cursor end scanline = 15, preserve bits 5-7
    dec  dx
    mov  al, VGA_CURSOR_END
    out  dx, al
    inc  dx
    in   al, dx
    and  al, 0xE0
    or   al, 0x0F
    out  dx, al

    popa
    ret

scroll_screen:
    pusha
    push ds
    push es

    mov  ax, PM_VIDEO_SEL
    mov  es, ax
    mov  ds, ax
    cld

    mov  si, 160
    mov  di, 0
    mov  cx, 24*80
    rep  movsw

    mov  di, 24*160
    mov  cx, 80
    mov  ax, 0x0720
    rep  stosw

    pop  es
    pop  ds
    popa
    ret

; ------------------------------------------------------------
; Data (in SS segment via DGROUP)
; ------------------------------------------------------------
segment _DATA class=DATA use16

global _line
global _di_pos
global _line_end
global _kbd_shift
global _kbd_ctrl
global _kbd_ext

_line       dw 0
_di_pos     dw 0
_line_end   dw 0
_kbd_shift  db 0
_kbd_ctrl   db 0
_kbd_ext    db 0
_input_col  db 0
_input_row  db 0
_input_start_col db 0
_input_start_row db 0
    ; software cursor state
    _soft_cur_active db 0
    _soft_cur_row    db 0
    _soft_cur_col    db 0
    _soft_cur_ch     db 0
    _soft_cur_attr   db 0
_kbd_q_head db 0
_kbd_q_tail db 0
_kbd_q_overflow db 0
_kbd_queue  times KBD_QUEUE_SIZE dw 0

; ------------------------------------------------------------
; US keyboard scancode->ASCII map
; ------------------------------------------------------------
segment _TEXT

kbd_map:
    db 0,0,'1','2','3','4','5','6','7','8','9','0','-','=',8,9
    db 'q','w','e','r','t','y','u','i','o','p','[',']',13,0,'a','s'
    db 'd','f','g','h','j','k','l',';',39,'`',0,'\','z','x','c','v'
    db 'b','n','m',',','.','/',0,'*',0,' ',0
    times (128-($-kbd_map)) db 0

kbd_map_shift:
    db 0,0,'!','@','#','$','%','^','&','*','(',')','_','+',8,9
    db 'Q','W','E','R','T','Y','U','I','O','P','{','}',13,0,'A','S'
    db 'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V'
    db 'B','N','M','<','>','?',0,'*',0,' ',0
    times (128-($-kbd_map_shift)) db 0

%endif
