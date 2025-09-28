# ---------------------------------
# Toolchain & Layout
# ---------------------------------
SHELL   := /bin/sh
AS      := nasm

BINDIR  := build/bin
SECTOR  := 512

# ---------------------------------
# Sources / Build Order
# ---------------------------------
ORDER   := boot sima
SRCS    := $(addsuffix .asm,$(ORDER))
BINS    := $(addprefix $(BINDIR)/,$(addsuffix .bin,$(ORDER)))

IMG     := $(BINDIR)/bootloader.img

# 커널 바이너리 경로(상위 빌드 산출물)
KERNEL_IMG := ../kernel/build/bin/kernel.img

.PHONY: all
all: bootloader.img

# 루트에 옛 이름(alias)
bootloader.img: $(IMG)
	@cp -f $< $@

# ---------------------------------
# build rules
# ---------------------------------

# boot1
$(BINDIR)/boot.bin: boot.asm | $(BINDIR)
	@echo "[AS] $<"
	$(AS) -f bin -o "$@" "$<"

# boot2: 커널 섹터 수를 '레시피 안에서' 계산해 -D로 주입
$(BINDIR)/sima.bin: sima.asm $(KERNEL_IMG) | $(BINDIR)
	@KRN_BYTES=$$(stat -c%s "$(KERNEL_IMG)"); \
	  KRN_SECT=$$(( (KRN_BYTES + 511) / 512 )); \
	  echo "[AS] $<  (KRN_SECT=$$KRN_SECT)"; \
	  $(AS) -f bin -D KRN_SECT=$$KRN_SECT -o "$@" "$<"

# bootloader.img: 단일 레시피(중복 금지)
$(IMG): $(BINS) | $(BINDIR)
	@echo "[IMG] $@"
	cat $(BINS) > "$@"
	@size=$$(stat -c%s "$@"); \
	  pad=$$(( ((size + $(SECTOR) - 1)/$(SECTOR))*$(SECTOR) )); \
	  if [ $$pad -ne $$size ]; then \
	    echo "[PAD] $$size -> $$pad bytes ($(SECTOR)-align)"; \
	    truncate -s $$pad "$@"; \
	  fi

$(BINDIR):
	@mkdir -p "$@"

# ---------------------------------
# Housekeeping
# ---------------------------------
.PHONY: clean distclean dirs
dirs: | $(BINDIR)

clean:
	@echo "[CLEAN]"
	@rm -r build
