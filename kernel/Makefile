# ------------------------------
# Toolchain & Dirs
# ------------------------------
SHELL   := /bin/sh
CC      := wcc
AS      := nasm
LD      := wlink

OBJDIR  := build/obj
BINDIR  := build/bin
INCDIR  := include

# ------------------------------
# Flags (기존 플래그 유지 + 정리)
#   -ms : small memory model
#   -s  : strip
#   -ecc, -1, -zl, -od, -d2, -i... : 기존 사용값 유지
# ------------------------------
CFLAGS  := -s -ecc -1 -ms -zl -od -d2 -i$(INCDIR)
ASFLAGS := -f obj
# wlink는 보통 응답파일(link.lk)을 쓰지만, 여기선 그대로 사용
LDFILE  := link.lk

# ------------------------------
# Sources
# ------------------------------
SRCS_C     := kernel.c sima_mem.c
SRCS_ASM   := api.asm kernel_entry.asm

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
	@size=$$(stat -c%s "$@"); \
	  pad=$$(( ((size + 511)/512)*512 )); \
	  if [ $$pad -ne $$size ]; then \
	    echo "[PAD] $$size -> $$pad bytes (512-align)"; \
	    truncate -s $$pad "$@"; \
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
$(OBJDIR)/kernel.obj:     simaio.h sima_type.h sima_mem.h
$(OBJDIR)/sima_mem.obj:   sima_type.h sima_mem.h

# ------------------------------
# Utility targets
# ------------------------------
dirs: | $(OBJDIR) $(BINDIR)

$(OBJDIR) $(BINDIR):
	@mkdir -p $@

clean:
	@echo "[CLEAN]" 
	@rm -r build

# 선택: QEMU로 테스트(환경에 맞게 수정)
run: $(IMG)
	@qemu-system-i386 -hda $(IMG) -boot a -monitor stdio

