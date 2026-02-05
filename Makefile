# ===== Meta Makefile (Build Both Floppies) =====
SHELL := /bin/sh
QEMU  ?= qemu-system-i386
QEMUIMG ?= qemu-img

SEOLSEM_DIR := seolsem
INSTALLER_DIR := seolsem_installer

SEOLSEM_IMG := $(SEOLSEM_DIR)/seolsem.img
INSTALLER_IMG := $(INSTALLER_DIR)/seolsem_installer.img
HDD_IMG := $(INSTALLER_DIR)/build/hdd.img

.PHONY: all clean seolsem installer run-install run-hdd hdd

all: $(SEOLSEM_IMG) $(INSTALLER_IMG)

seolsem: $(SEOLSEM_IMG)

installer: $(INSTALLER_IMG)

$(SEOLSEM_IMG):
	$(MAKE) -C $(SEOLSEM_DIR) seolsem.img

$(INSTALLER_IMG):
	$(MAKE) -C $(INSTALLER_DIR) seolsem_installer.img

hdd: $(HDD_IMG)

$(HDD_IMG):
	$(MAKE) -C $(INSTALLER_DIR) hdd

run-install: all hdd
	@echo "When prompted to swap disks, use QEMU monitor command:"
	@echo "  change floppy0 $(SEOLSEM_IMG)"
	$(QEMU) -m 16 -boot a -monitor stdio \
		-drive if=floppy,format=raw,file=$(INSTALLER_IMG) \
		-drive if=ide,format=raw,file=$(HDD_IMG)

run-hdd: hdd
	$(QEMU) -m 16 \
		-drive if=ide,format=raw,file=$(HDD_IMG)

clean:
	@echo "[CLEAN]"
	$(MAKE) -C $(SEOLSEM_DIR) clean
	$(MAKE) -C $(INSTALLER_DIR) clean
	@rm -f *.bin
	@rm -f $(HDD_IMG)
