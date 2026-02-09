
# ===== Top-level Makefile =====
SHELL       := /bin/sh
QEMU        ?= qemu-system-i386
QEMUIMG     ?= qemu-img
PY          ?= python3
QEMU_AUDIO_DRV ?= alsa
QEMU_AUDIO_ID ?= snd0
QEMU_AUDIO_ARGS ?= -machine pc,pcspk-audiodev=$(QEMU_AUDIO_ID) -audiodev $(QEMU_AUDIO_DRV),id=$(QEMU_AUDIO_ID)

BOOT_BIN    := bootloader/build/bin/boot.bin
STAGE2_BIN  := bootloader/build/bin/sima.bin
KERNEL_IMG  := kernel/build/bin/kernel.img
RAW_IMG     := seolsem.img
QCOW2_IMG   := seolsem.qcow2
IMG_TOOL    := tools/make_fat12_image.py
VIMPORT_DIR := ../vim_port
VIMPORT_STAMP := $(VIMPORT_DIR)/build/.shim_ready

# 생성물(clean 대상). 소스(.c/.h/.asm/.py 등)는 절대 지우지 않는다.
# - 루트 디렉터리의 각종 덤프/테스트 이미지 파일을 정리
# - __pycache__/pyc 같은 캐시도 정리
CLEAN_FILES := \
	$(RAW_IMG) \
	$(QCOW2_IMG) \
	seolsem*.img \
	seolsem*.qcow2 \
	seolsem*.raw \
	*.map \
	*.obj \
	*.o \
	vram.bin \
	screen*.bin \
	dgroup*.bin \
	code_mem*.bin \
	mem_dump.bin \
	data_start \
	seolsem-qmp.sock

CLEAN_DIRS := \
	bootloader/build \
	kernel/build

.PHONY: all clean run bootloader kernel vim-port

all: $(QCOW2_IMG)

run: $(QCOW2_IMG)
	$(QEMU) $(QEMU_AUDIO_ARGS) -hda $(QCOW2_IMG)

$(QCOW2_IMG): $(RAW_IMG)
	@echo "[QCOW2] $@"
	$(QEMUIMG) convert -f raw -O qcow2 $< $@

$(RAW_IMG): $(KERNEL_IMG) $(BOOT_BIN) $(STAGE2_BIN) $(IMG_TOOL) $(VIMPORT_STAMP)
	@echo "[FAT12] $@"
	$(PY) $(IMG_TOOL) --boot $(BOOT_BIN) --stage2 $(STAGE2_BIN) --kernel $(KERNEL_IMG) --output $@

# 서브메이크: 항상 위임 (하위 Makefile이 변경 여부 판단)
bootloader:
	$(MAKE) -C bootloader

kernel:
	$(MAKE) -C kernel

vim-port: $(VIMPORT_STAMP)

$(VIMPORT_STAMP):
	$(MAKE) -C $(VIMPORT_DIR) shim

$(BOOT_BIN) $(STAGE2_BIN): bootloader

$(KERNEL_IMG): kernel

clean:
	@echo "[CLEAN]"
	@$(RM) -f $(CLEAN_FILES)
	@rm -rf $(CLEAN_DIRS)
	@find . -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
	@find . -name '*.pyc' -type f -delete 2>/dev/null || true
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
