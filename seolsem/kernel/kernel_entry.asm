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

    ; IRQ1 (keyboard) -> vector 0x21
    mov  di, pm_idt + (0x21 * 8)
    mov  ax, _kbd_isr
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    ; Other PIC vectors use a generic EOI+IRET stub so unexpected IRQs
    ; do not halt the system.
    mov  ax, pm_isr_pic_eoi
    mov  di, pm_idt + (0x20 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x22 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x23 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x24 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x25 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x26 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x27 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x28 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x29 * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2A * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2B * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2C * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2D * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2E * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    mov  di, pm_idt + (0x2F * 8)
    mov  word [di + 0], ax
    mov  word [di + 2], PM_CODE_SEL
    mov  byte [di + 4], 0
    mov  byte [di + 5], 0x86
    mov  word [di + 6], 0

    pop  di
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
