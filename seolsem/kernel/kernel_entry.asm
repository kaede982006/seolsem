bits 16
extern _kernel_main
extern _sima_heap_init
extern __bss_start
extern __bss_end
extern _kbd_isr
global _start

segment _TEXT class=CODE use16
_start:
    jmp short .real_start
    db "KENTRY_MARKER_v1", 0
.real_start:
    cli

    ; DS는 부트로더에서 DGROUP으로 설정됨(Real mode).
    ; 여기서 16-bit Protected Mode로 전환해 커널을 실행한다.
    ;
    ; Protected Mode에서는 BIOS INT(10h/16h/13h 등)을 사용할 수 없으므로,
    ; 커널 I/O는 직접 하드웨어(VGA, 8042, IDE PIO 등)를 사용해야 한다.

    ; ------------------------------
    ; Build GDT/IDT (based on real-mode CS/DS) and enter 16-bit Protected Mode
    ; ------------------------------
    ; Small memory model uses near pointers for locals; keep SS=DS while in real mode too.
    mov  ax, ds
    mov  ss, ax
    mov  sp, 0xFFFE
    mov  bp, sp

    call pm_enter_16

    ; ------------------------------
    ; Now running in 16-bit Protected Mode with:
    ;   CS = PM_CODE_SEL, DS/ES/SS = PM_DATA_SEL
    ; ------------------------------
    cld

    ; Small memory model uses near pointers for locals; keep SS=DS.
    mov  ax, ds
    mov  ss, ax
    mov  sp, 0xFFFE
    mov  bp, sp
    mov  es, ax                  ; BSS clear uses ES:DI

    ; BSS 0클리어
    mov  di, __bss_start
    mov  cx, __bss_end
    sub  cx, di
    xor  ax, ax
    rep  stosb

    ; 내부 아레나 힙 초기화 (freestanding)
    call _sima_heap_init

    ; C 커널 진입
    call _kernel_main

.halt:
    cli
    hlt
    jmp .halt

; ------------------------------
; 16-bit Protected Mode bring-up
; ------------------------------
%define PM_CODE_SEL  0x08
%define PM_DATA_SEL  0x10
%define PM_VIDEO_SEL 0x18
%define PM_EXC_ATTR  0x4F
%define PM_VGA_MISC_READ 0x3CC
%define PM_CRTC_IDX_C 0x3D4
%define PM_CRTC_IDX_M 0x3B4
%define PM_CRTC_CURSOR_START 0x0A
%define PM_PIT_CMD 0x43
%define PM_PIT_CH0 0x40
%define PM_PIT_CH2 0x42
%define PM_SPKR_PORT 0x61

pm_enter_16:
    push bp
    mov  bp, sp

    ; Mask all IRQs before remap/install.
    mov  al, 0xFF
    out  0x21, al
    out  0xA1, al

    ; Compute real-mode bases for current CS and DS (segment << 4).
    xor  eax, eax
    mov  ax, cs
    shl  eax, 4
    mov  [pm_code_base], eax

    xor  eax, eax
    mov  ax, ds
    shl  eax, 4
    mov  [pm_data_base], eax

    ; Build GDT entries (code/data base derived above, 64KiB limit, 16-bit segments).
    call pm_build_gdt
    call pm_build_idt

    ; Load GDTR/IDTR with physical base addresses.
    lgdt [pm_gdtr]
    lidt [pm_idtr]

    ; Enter Protected Mode (CR0.PE = 1).
    mov  eax, cr0
    or   eax, 0x00000001
    mov  cr0, eax

    ; Far jump to flush prefetch and load CS selector.
    jmp  PM_CODE_SEL:pm_protected_start

pm_protected_start:
    ; Load data selectors. Do not touch memory before these loads.
    mov  ax, PM_DATA_SEL
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  fs, ax
    mov  gs, ax

    ; Remap PIC to vectors 0x20..0x2F and unmask only keyboard IRQ1.
    call pm_pic_remap
    mov  al, 0xFD              ; 11111101b -> allow IRQ1 only on master
    out  0x21, al
    mov  al, 0xFF              ; keep all slave IRQs masked
    out  0xA1, al

    pop  bp
    ret

pm_build_gdt:
    push ax
    push bx
    push dx

    ; Descriptor layout:
    ;  0-1: limit low
    ;  2-3: base low
    ;  4  : base mid
    ;  5  : access
    ;  6  : flags/limit high
    ;  7  : base high
    ;
    ; Code: base=pm_code_base, limit=0xFFFF, access=0x9A, flags=0x00 (16-bit, byte granularity)
    mov  word [pm_gdt_code + 0], 0xFFFF
    mov  eax, [pm_code_base]
    mov  word [pm_gdt_code + 2], ax
    shr  eax, 16
    mov  byte [pm_gdt_code + 4], al
    mov  byte [pm_gdt_code + 5], 0x9A
    mov  byte [pm_gdt_code + 6], 0x00
    mov  byte [pm_gdt_code + 7], ah

    ; Data: base=pm_data_base, limit=0xFFFF, access=0x92, flags=0x00
    mov  word [pm_gdt_data + 0], 0xFFFF
    mov  eax, [pm_data_base]
    mov  word [pm_gdt_data + 2], ax
    shr  eax, 16
    mov  byte [pm_gdt_data + 4], al
    mov  byte [pm_gdt_data + 5], 0x92
    mov  byte [pm_gdt_data + 6], 0x00
    mov  byte [pm_gdt_data + 7], ah

    ; Video: base=0x000B8000, limit=0xFFFF, access=0x92, flags=0x00
    mov  word [pm_gdt_video + 0], 0xFFFF
    mov  word [pm_gdt_video + 2], 0x8000
    mov  byte [pm_gdt_video + 4], 0x0B
    mov  byte [pm_gdt_video + 5], 0x92
    mov  byte [pm_gdt_video + 6], 0x00
    mov  byte [pm_gdt_video + 7], 0x00

    ; GDTR.base = pm_data_base + offset(pm_gdt)
    mov  eax, [pm_data_base]
    xor  ebx, ebx
    mov  bx, pm_gdt
    add  eax, ebx
    mov  [pm_gdtr + 2], eax

    pop  dx
    pop  bx
    pop  ax
    ret

pm_build_idt:
    push ax
    push bx
    push cx
    push di
    push si

    ; IDTR.base = pm_data_base + offset(pm_idt)
    mov  eax, [pm_data_base]
    xor  ebx, ebx
    mov  bx, pm_idt
    add  eax, ebx
    mov  [pm_idtr + 2], eax

    ; Fill IDT with a safe IRET gate first.
    mov  ax, pm_isr_iret
    mov  di, pm_idt
    mov  cx, 256
.fill:
    mov  word [di + 0], ax             ; offset low
    mov  word [di + 2], PM_CODE_SEL    ; selector
    mov  byte [di + 4], 0              ; reserved
    mov  byte [di + 5], 0x86           ; P=1, DPL=0, 16-bit interrupt gate
    mov  word [di + 6], 0              ; offset high (unused for 16-bit gate)
    add  di, 8
    loop .fill

    ; CPU exceptions (0x00..0x1F) -> detailed red-screen handler stubs.
    mov  si, pm_exc_stub_table
    xor  bx, bx
    mov  cx, 32
.exc_install:
    mov  ax, [si]
    call pm_set_idt_gate
    add  si, 2
    inc  bl
    loop .exc_install

    ; IRQ1 (keyboard) -> vector 0x21
    mov  bl, 0x21
    mov  ax, _kbd_isr
    call pm_set_idt_gate

    ; Other PIC vectors use a generic EOI+IRET stub so unexpected IRQs
    ; do not halt the system.
    mov  ax, pm_isr_pic_eoi
    mov  bl, 0x20
.pic_install:
    cmp  bl, 0x21
    je   .pic_next
    call pm_set_idt_gate
.pic_next:
    inc  bl
    cmp  bl, 0x30
    jb   .pic_install

    pop  si
    pop  di
    pop  cx
    pop  bx
    pop  ax
    ret

; IN: BL=vector, AX=offset
pm_set_idt_gate:
    push bx
    push di

    xor  bh, bh
    mov  di, bx
    shl  di, 3
    add  di, pm_idt

    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86           ; 16-bit interrupt gate, present
    mov  word [di + 6], 0

    pop  di
    pop  bx
    ret

%macro PM_EXC_STUB_NOERR 1
pm_exc_stub_%1:
    push word 0
    push word %1
    jmp  pm_exc_common
%endmacro

%macro PM_EXC_STUB_ERR 1
pm_exc_stub_%1:
    push word %1
    jmp  pm_exc_common
%endmacro

PM_EXC_STUB_NOERR 0
PM_EXC_STUB_NOERR 1
PM_EXC_STUB_NOERR 2
PM_EXC_STUB_NOERR 3
PM_EXC_STUB_NOERR 4
PM_EXC_STUB_NOERR 5
PM_EXC_STUB_NOERR 6
PM_EXC_STUB_NOERR 7
PM_EXC_STUB_ERR   8
PM_EXC_STUB_NOERR 9
PM_EXC_STUB_ERR   10
PM_EXC_STUB_ERR   11
PM_EXC_STUB_ERR   12
PM_EXC_STUB_ERR   13
PM_EXC_STUB_ERR   14
PM_EXC_STUB_NOERR 15
PM_EXC_STUB_NOERR 16
PM_EXC_STUB_ERR   17
PM_EXC_STUB_NOERR 18
PM_EXC_STUB_NOERR 19
PM_EXC_STUB_NOERR 20
PM_EXC_STUB_ERR   21
PM_EXC_STUB_NOERR 22
PM_EXC_STUB_NOERR 23
PM_EXC_STUB_NOERR 24
PM_EXC_STUB_NOERR 25
PM_EXC_STUB_NOERR 26
PM_EXC_STUB_NOERR 27
PM_EXC_STUB_NOERR 28
PM_EXC_STUB_NOERR 29
PM_EXC_STUB_ERR   30
PM_EXC_STUB_NOERR 31

pm_exc_common:
    cli
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov  bp, sp

    mov  ax, PM_DATA_SEL
    mov  ds, ax
    mov  ax, PM_VIDEO_SEL
    mov  es, ax

    call pm_exc_hide_cursor

    mov  eax, cr2
    mov  [pm_exc_cr2], eax

    call pm_exc_clear_screen
    mov  byte [pm_exc_row], 0
    mov  byte [pm_exc_col], 0
    call pm_exc_beep

    mov  si, pm_exc_title
    call pm_exc_puts
    call pm_exc_newline

    mov  si, pm_exc_vector_label
    call pm_exc_puts
    mov  ax, [ss:bp + 18]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  al, '('
    call pm_exc_putc
    mov  bx, [ss:bp + 18]
    and  bx, 0x001F
    shl  bx, 1
    mov  si, [pm_exc_name_table + bx]
    call pm_exc_puts
    mov  al, ')'
    call pm_exc_putc
    call pm_exc_newline

    mov  si, pm_exc_err_label
    call pm_exc_puts
    mov  ax, [ss:bp + 20]
    call pm_exc_puthex16
    call pm_exc_newline

    mov  si, pm_exc_csip_label
    call pm_exc_puts
    mov  ax, [ss:bp + 24]
    call pm_exc_puthex16
    mov  al, ':'
    call pm_exc_putc
    mov  ax, [ss:bp + 22]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_flags_label
    call pm_exc_puts
    mov  ax, [ss:bp + 26]
    call pm_exc_puthex16
    call pm_exc_newline

    mov  si, pm_exc_sp_label
    call pm_exc_puts
    lea  ax, [bp + 28]
    call pm_exc_puthex16
    call pm_exc_newline

    mov  si, pm_exc_ax_label
    call pm_exc_puts
    mov  ax, [ss:bp + 16]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_bx_label
    call pm_exc_puts
    mov  ax, [ss:bp + 14]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_cx_label
    call pm_exc_puts
    mov  ax, [ss:bp + 12]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_dx_label
    call pm_exc_puts
    mov  ax, [ss:bp + 10]
    call pm_exc_puthex16
    call pm_exc_newline

    mov  si, pm_exc_si_label
    call pm_exc_puts
    mov  ax, [ss:bp + 8]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_di_label
    call pm_exc_puts
    mov  ax, [ss:bp + 6]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_bp_label
    call pm_exc_puts
    mov  ax, [ss:bp + 4]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_ds_label
    call pm_exc_puts
    mov  ax, [ss:bp + 2]
    call pm_exc_puthex16
    mov  al, ' '
    call pm_exc_putc
    mov  si, pm_exc_es_label
    call pm_exc_puts
    mov  ax, [ss:bp + 0]
    call pm_exc_puthex16
    call pm_exc_newline

    mov  si, pm_exc_cr2_label
    call pm_exc_puts
    mov  ax, [pm_exc_cr2 + 2]
    call pm_exc_puthex16
    mov  ax, [pm_exc_cr2 + 0]
    call pm_exc_puthex16
    call pm_exc_newline
    call pm_exc_newline
    mov  si, pm_exc_halt
    call pm_exc_puts

.hang:
    cli
    hlt
    jmp  .hang

pm_exc_get_crtc_base:
    push ax
    push dx
    mov  dx, PM_VGA_MISC_READ
    in   al, dx
    pop  dx
    mov  dx, PM_CRTC_IDX_C
    test al, 0x01
    jnz  .done
    mov  dx, PM_CRTC_IDX_M
.done:
    pop  ax
    ret

pm_exc_hide_cursor:
    push ax
    push dx
    call pm_exc_get_crtc_base
    mov  al, PM_CRTC_CURSOR_START
    out  dx, al
    inc  dx
    in   al, dx
    or   al, 0x20
    out  dx, al
    pop  dx
    pop  ax
    ret

pm_exc_beep:
    push ax
    push bx
    push cx
    push dx

    ; PIT ch2 square wave, lower tone (~440Hz)
    mov  al, 0xB6
    out  PM_PIT_CMD, al
    mov  ax, 2712
    out  PM_PIT_CH2, al
    mov  al, ah
    out  PM_PIT_CH2, al

    ; single beep, ~1 second.
    in   al, PM_SPKR_PORT
    mov  bl, al
    mov  al, bl
    or   al, 0x03
    out  PM_SPKR_PORT, al
    ; ~1000 ms
    mov  dx, 0x0012
    mov  ax, 0x34DE
    call pm_exc_wait_ticks

    mov  al, bl
    out  PM_SPKR_PORT, al
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; IN: DX:AX = PIT ticks to wait (32-bit, hi:lo)
pm_exc_wait_ticks:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov  di, ax                   ; target low
    mov  bx, dx                   ; target high
    call pm_exc_pit_read0
    mov  si, ax                   ; previous counter sample
    xor  cx, cx                   ; elapsed low
    xor  dx, dx                   ; elapsed high
.wait_loop:
    call pm_exc_pit_read0
    xchg ax, si                   ; AX=prev, SI=current
    sub  ax, si                   ; delta modulo 16-bit (down-counter wrap-safe)
    add  cx, ax
    adc  dx, 0
    cmp  dx, bx
    ja   .done
    jb   .wait_loop
    cmp  cx, di
    jb   .wait_loop
.done:
    pop  di
    pop  si
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; OUT: AX = current PIT channel 0 counter value
pm_exc_pit_read0:
    push dx
    mov  al, 0x00                 ; latch channel 0
    out  PM_PIT_CMD, al
    in   al, PM_PIT_CH0           ; low
    mov  dl, al
    in   al, PM_PIT_CH0           ; high
    mov  ah, al
    mov  al, dl
    pop  dx
    ret

pm_exc_clear_screen:
    push ax
    push cx
    push di
    xor  di, di
    mov  ax, (PM_EXC_ATTR << 8) | ' '
    mov  cx, 80*25
    rep  stosw
    pop  di
    pop  cx
    pop  ax
    ret

pm_exc_newline:
    mov  byte [pm_exc_col], 0
    mov  al, [pm_exc_row]
    cmp  al, 24
    jae  .done
    inc  byte [pm_exc_row]
.done:
    ret

; IN: AL=character
pm_exc_putc:
    cmp  al, 10
    je   pm_exc_newline
    cmp  al, 13
    je   .ret
    push ax
    push bx
    push dx
    push di

    mov  dl, al
    xor  ax, ax
    mov  al, [pm_exc_row]
    mov  bl, 160
    mul  bl
    mov  di, ax
    xor  bx, bx
    mov  bl, [pm_exc_col]
    shl  bx, 1
    add  di, bx
    mov  al, dl
    mov  [es:di], al
    mov  byte [es:di + 1], PM_EXC_ATTR

    mov  al, [pm_exc_col]
    cmp  al, 79
    jb   .col_inc
    mov  byte [pm_exc_col], 0
    mov  al, [pm_exc_row]
    cmp  al, 24
    jae  .done
    inc  byte [pm_exc_row]
    jmp  .done
.col_inc:
    inc  byte [pm_exc_col]
.done:
    pop  di
    pop  dx
    pop  bx
    pop  ax
.ret:
    ret

; IN: DS:SI NUL-terminated string
pm_exc_puts:
    push ax
.next:
    lodsb
    test al, al
    jz   .done
    call pm_exc_putc
    jmp  .next
.done:
    pop  ax
    ret

; IN: AX=value
pm_exc_puthex16:
    push ax
    push bx
    push cx
    mov  bx, ax
    mov  cx, 4
.loop:
    rol  bx, 4
    mov  al, bl
    and  al, 0x0F
    cmp  al, 9
    jbe  .digit
    add  al, 'A' - 10
    jmp  .emit
.digit:
    add  al, '0'
.emit:
    call pm_exc_putc
    loop .loop
    pop  cx
    pop  bx
    pop  ax
    ret

pm_isr_iret:
    iret

pm_isr_pic_eoi:
    mov  al, 0x20
    out  0x20, al
    iret

pm_pic_remap:
    ; ICW1: start init (edge-triggered, expect ICW4)
    mov  al, 0x11
    out  0x20, al
    out  0xA0, al
    ; ICW2: vector offsets
    mov  al, 0x20
    out  0x21, al
    mov  al, 0x28
    out  0xA1, al
    ; ICW3: master has slave at IRQ2, slave identity 2
    mov  al, 0x04
    out  0x21, al
    mov  al, 0x02
    out  0xA1, al
    ; ICW4: 8086 mode
    mov  al, 0x01
    out  0x21, al
    out  0xA1, al
    ret

segment _DATA class=DATA use16
align 4
pm_code_base dd 0
pm_data_base dd 0

align 8
pm_gdt:
    dq 0
pm_gdt_code:
    dq 0
pm_gdt_data:
    dq 0
pm_gdt_video:
    dq 0
pm_gdt_end:

pm_gdtr:
    dw pm_gdt_end - pm_gdt - 1
    dd 0

align 8
pm_idt:
    times 256 dq 0
pm_idt_end:

pm_idtr:
    dw pm_idt_end - pm_idt - 1
    dd 0

pm_exc_row db 0
pm_exc_col db 0
pm_exc_cr2 dd 0

pm_exc_title         db "EXCEPTION: SYSTEM HALTED", 0
pm_exc_vector_label  db "VECTOR=0x", 0
pm_exc_err_label     db "ERROR =0x", 0
pm_exc_csip_label    db "CS:IP =0x", 0
pm_exc_flags_label   db "FLAGS=0x", 0
pm_exc_sp_label      db "SP    =0x", 0
pm_exc_ax_label      db "AX=0x", 0
pm_exc_bx_label      db "BX=0x", 0
pm_exc_cx_label      db "CX=0x", 0
pm_exc_dx_label      db "DX=0x", 0
pm_exc_si_label      db "SI=0x", 0
pm_exc_di_label      db "DI=0x", 0
pm_exc_bp_label      db "BP=0x", 0
pm_exc_ds_label      db "DS=0x", 0
pm_exc_es_label      db "ES=0x", 0
pm_exc_cr2_label     db "CR2  =0x", 0
pm_exc_halt          db "Kernel halted. Reboot required.", 0

pm_exc_name_00 db "Divide Error", 0
pm_exc_name_01 db "Debug", 0
pm_exc_name_02 db "NMI", 0
pm_exc_name_03 db "Breakpoint", 0
pm_exc_name_04 db "Overflow", 0
pm_exc_name_05 db "Bound Range Exceeded", 0
pm_exc_name_06 db "Invalid Opcode", 0
pm_exc_name_07 db "Device Not Available", 0
pm_exc_name_08 db "Double Fault", 0
pm_exc_name_09 db "Coprocessor Segment Overrun", 0
pm_exc_name_0A db "Invalid TSS", 0
pm_exc_name_0B db "Segment Not Present", 0
pm_exc_name_0C db "Stack-Segment Fault", 0
pm_exc_name_0D db "General Protection Fault", 0
pm_exc_name_0E db "Page Fault", 0
pm_exc_name_0F db "Reserved", 0
pm_exc_name_10 db "x87 Floating-Point Exception", 0
pm_exc_name_11 db "Alignment Check", 0
pm_exc_name_12 db "Machine Check", 0
pm_exc_name_13 db "SIMD Floating-Point Exception", 0
pm_exc_name_14 db "Virtualization Exception", 0
pm_exc_name_15 db "Control Protection Exception", 0
pm_exc_name_16 db "Reserved", 0
pm_exc_name_17 db "Reserved", 0
pm_exc_name_18 db "Reserved", 0
pm_exc_name_19 db "Reserved", 0
pm_exc_name_1A db "Reserved", 0
pm_exc_name_1B db "Reserved", 0
pm_exc_name_1C db "Hypervisor Injection Exception", 0
pm_exc_name_1D db "VMM Communication Exception", 0
pm_exc_name_1E db "Security Exception", 0
pm_exc_name_1F db "Reserved", 0

pm_exc_name_table:
    dw pm_exc_name_00, pm_exc_name_01, pm_exc_name_02, pm_exc_name_03
    dw pm_exc_name_04, pm_exc_name_05, pm_exc_name_06, pm_exc_name_07
    dw pm_exc_name_08, pm_exc_name_09, pm_exc_name_0A, pm_exc_name_0B
    dw pm_exc_name_0C, pm_exc_name_0D, pm_exc_name_0E, pm_exc_name_0F
    dw pm_exc_name_10, pm_exc_name_11, pm_exc_name_12, pm_exc_name_13
    dw pm_exc_name_14, pm_exc_name_15, pm_exc_name_16, pm_exc_name_17
    dw pm_exc_name_18, pm_exc_name_19, pm_exc_name_1A, pm_exc_name_1B
    dw pm_exc_name_1C, pm_exc_name_1D, pm_exc_name_1E, pm_exc_name_1F

pm_exc_stub_table:
    dw pm_exc_stub_0, pm_exc_stub_1, pm_exc_stub_2, pm_exc_stub_3
    dw pm_exc_stub_4, pm_exc_stub_5, pm_exc_stub_6, pm_exc_stub_7
    dw pm_exc_stub_8, pm_exc_stub_9, pm_exc_stub_10, pm_exc_stub_11
    dw pm_exc_stub_12, pm_exc_stub_13, pm_exc_stub_14, pm_exc_stub_15
    dw pm_exc_stub_16, pm_exc_stub_17, pm_exc_stub_18, pm_exc_stub_19
    dw pm_exc_stub_20, pm_exc_stub_21, pm_exc_stub_22, pm_exc_stub_23
    dw pm_exc_stub_24, pm_exc_stub_25, pm_exc_stub_26, pm_exc_stub_27
    dw pm_exc_stub_28, pm_exc_stub_29, pm_exc_stub_30, pm_exc_stub_31
