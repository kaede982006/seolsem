# ------------------------------
# Toolchain & Dirs
# ------------------------------
SHELL   := /bin/sh
CC      := wcc
AS      := nasm
LD      := wlink
PY      ?= python3

OBJDIR  := build/obj
BINDIR  := build/bin
INCDIR  := include

# ------------------------------
# Flags (기존 플래그 유지 + 정리)
#   -ms : small memory model
#   -s  : strip
#   -ecc, -1, -zl, -os, -d0, -i... : 기존 사용값 유지/정리
# ------------------------------
# Keep the kernel within the 64KiB segment window required by our 16-bit
# protected-mode + small-model layout (CS/DS base = image base).
# `-os` is critical here; debug-friendly `-od/-d2` can push the image past 64KiB
# and crash on the first command execution.
CFLAGS  := -s -ecc -1 -ms -zl -os -d0 -i$(INCDIR)
ASFLAGS := -f obj
# wlink는 보통 응답파일(link.lk)을 쓰지만, 여기선 그대로 사용
LDFILE  := link.lk

# ------------------------------
# Sources
# ------------------------------
SRCS_C     := kernel.c sima_mem.c sima_conv.c sima_ide.c sima_fs.c sima_program.c sima_env.c sima_user.c sima_perm.c sima_proc.c sima_syscall.c xef_loader.c commands.c
SRCS_ASM   := dgroup_marker.asm bss_start.asm code_marker.asm api.asm kernel_entry.asm math_helpers.asm sima_heap.asm sima_ioport.asm xef_exec.asm code_end.asm bss_end.asm

# ------------------------------
# Objects (빌드 디렉터리에 모아서 생성)
# ------------------------------
OBJS_C     := $(SRCS_C:%.c=$(OBJDIR)/%.obj)
OBJS_ASM   := $(SRCS_ASM:%.asm=$(OBJDIR)/%.obj)
OBJS       := $(OBJS_C) $(OBJS_ASM)

# 최종 출력물
IMG        := $(BINDIR)/kernel.img

# ------------------------------
# Top-level
# ------------------------------
.PHONY: all clean distclean run dirs
all: $(IMG)

# 출력만 루트에 별칭으로 두고 싶으면(선택):
kernel.img: $(IMG)
	@cp -f $< $@

# ------------------------------
# Link & pad to 512 bytes
# ------------------------------
$(IMG): $(OBJS) | $(BINDIR)
	@echo "[LD] $@"
	$(LD) @$(LDFILE) NAME $@
	@echo "[PACK] $@ (embed BSS for RAW output)"
	@$(PY) tools/pack_kernel_image.py --img "$@" --map "$(BINDIR)/kernel.map"
	@size=$$(stat -c%s "$@"); \
	  pad=$$(( ((size + 511)/512)*512 )); \
	  if [ $$pad -ne $$size ]; then \
	    echo "[PAD] $$size -> $$pad bytes (512-align)"; \
	    truncate -s $$pad "$@"; \
	  fi; \
	  if [ $$pad -gt 65536 ]; then \
	    echo "[ERR] kernel.img too large: $$pad bytes (must be <= 65536 for 16-bit protected-mode layout)"; \
	    exit 1; \
	  fi

# ------------------------------
# Pattern rules
# ------------------------------
# C -> OBJ
$(OBJDIR)/%.obj: %.c | $(OBJDIR)
	@echo "[CC] $<"
	$(CC) $(CFLAGS) $< -fo=$@

# ASM -> OBJ
$(OBJDIR)/%.obj: %.asm | $(OBJDIR)
	@echo "[AS] $<"
	$(AS) $(ASFLAGS) $< -o $@

# ------------------------------
# Headers (간단 수동 의존성—필요시 추가)
# ------------------------------
# C 파일별 직접 의존(예: 타입/프로토타입 헤더)
$(OBJDIR)/kernel.obj:        sima_io.h sima_type.h sima_mem.h sima_fs.h sima_env.h sima_user.h
$(OBJDIR)/sima_mem.obj:      sima_type.h sima_mem.h
$(OBJDIR)/sima_conv.obj:     sima_type.h sima_mem.h sima_conv.h
$(OBJDIR)/sima_ide.obj:      sima_type.h sima_ide.h
$(OBJDIR)/sima_fs.obj:       sima_type.h sima_mem.h sima_fs.h sima_ide.h
$(OBJDIR)/sima_program.obj:  sima_type.h sima_program.h sima_fs.h sima_mem.h sima_heap.h xef_loader.h
$(OBJDIR)/sima_proc.obj:     sima_type.h sima_proc.h sima_program.h
$(OBJDIR)/sima_syscall.obj:  sima_type.h sima_syscall.h sima_io.h
$(OBJDIR)/xef_loader.obj:    sima_type.h xef_loader.h sima_mem.h sima_syscall.h sima_io.h sima_heap.h sima_conv.h
$(OBJDIR)/commands.obj:      sima_type.h sima_mem.h sima_conv.h sima_fs.h sima_program.h sima_io.h sima_env.h sima_user.h sima_perm.h sima_heap.h
$(OBJDIR)/sima_env.obj:      sima_type.h sima_env.h sima_fs.h sima_mem.h
$(OBJDIR)/sima_user.obj:     sima_type.h sima_user.h sima_fs.h sima_mem.h sima_env.h sima_conv.h
$(OBJDIR)/sima_perm.obj:     sima_type.h sima_perm.h sima_user.h sima_fs.h sima_mem.h sima_env.h sima_conv.h

# ------------------------------
# Utility targets
# ------------------------------
dirs: | $(OBJDIR) $(BINDIR)

$(OBJDIR) $(BINDIR):
	@mkdir -p $@

clean:
	@echo "[CLEAN]" 
	@rm -rf build
	@rm -f kernel.img

# 선택: QEMU로 테스트(환경에 맞게 수정)
run: $(IMG)
	@qemu-system-i386 -hda $(IMG) -boot a -monitor stdio
