
# ===== Top-level Makefile =====
SHELL       := /bin/sh
QEMU        ?= qemu-system-i386
QEMUIMG     ?= qemu-img
PY          ?= python3

BOOT_BIN    := bootloader/build/bin/boot.bin
STAGE2_BIN  := bootloader/build/bin/sima.bin
KERNEL_IMG  := kernel/build/bin/kernel.img
RAW_IMG     := seolsem.img
QCOW2_IMG   := seolsem.qcow2
IMG_TOOL    := tools/make_fat12_image.py

.PHONY: all clean run bootloader kernel

all: $(QCOW2_IMG)

run: $(QCOW2_IMG)
	$(QEMU) -hda $(QCOW2_IMG)

$(QCOW2_IMG): $(RAW_IMG)
	@echo "[QCOW2] $@"
	$(QEMUIMG) convert -f raw -O qcow2 $< $@

$(RAW_IMG): $(KERNEL_IMG) $(BOOT_BIN) $(STAGE2_BIN) $(IMG_TOOL)
	@echo "[FAT12] $@"
	$(PY) $(IMG_TOOL) --boot $(BOOT_BIN) --stage2 $(STAGE2_BIN) --kernel $(KERNEL_IMG) --output $@

# 서브메이크: 항상 위임 (하위 Makefile이 변경 여부 판단)
bootloader:
	$(MAKE) -C bootloader

kernel:
	$(MAKE) -C kernel

$(BOOT_BIN) $(STAGE2_BIN): bootloader

$(KERNEL_IMG): kernel

clean:
	@$(RM) -f $(RAW_IMG) $(QCOW2_IMG)
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean
