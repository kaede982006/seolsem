# ===== Top-level Makefile =====
SHELL       := /bin/sh
QEMU        ?= qemu-system-i386
QEMUIMG     ?= qemu-img

BOOT_IMG    := bootloader/build/bin/bootloader.img
KERNEL_IMG  := kernel/build/bin/kernel.img
RAW_IMG     := seolsem.img
QCOW2_IMG   := seolsem.qcow2

.PHONY: all clean run bootloader kernel

all: $(QCOW2_IMG)

run: $(QCOW2_IMG)
	$(QEMU) -hda $(QCOW2_IMG)

$(QCOW2_IMG): $(RAW_IMG)
	@echo "[QCOW2] $@"
	$(QEMUIMG) convert -f raw -O qcow2 $< $@

$(RAW_IMG): $(KERNEL_IMG) $(BOOT_IMG)
	@echo "[CAT] $@"
	cat $(BOOT_IMG) $(KERNEL_IMG) > $@

# 서브메이크: 결과물이 없으면 해당 디렉터리 빌드
$(BOOT_IMG):
	$(MAKE) -C bootloader

$(KERNEL_IMG):
	$(MAKE) -C kernel

clean:
	@$(RM) -f $(RAW_IMG) $(QCOW2_IMG)
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean

