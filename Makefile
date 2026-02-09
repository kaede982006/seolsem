# ===== Meta Makefile (Build Both Floppies) =====
SHELL := /bin/sh
QEMU  ?= qemu-system-i386
QEMUIMG ?= qemu-img
QEMU_AUDIO_DRV ?= alsa
QEMU_AUDIO_ID ?= snd0
QEMU_AUDIO_ARGS ?= -machine pc,pcspk-audiodev=$(QEMU_AUDIO_ID) -audiodev $(QEMU_AUDIO_DRV),id=$(QEMU_AUDIO_ID)

SEOLSEM_DIR := seolsem
INSTALLER_DIR := seolsem_installer
VIMPORT_DIR := vim_port
VIMPORT_STAMP := $(VIMPORT_DIR)/build/.shim_ready

SEOLSEM_IMG := $(SEOLSEM_DIR)/seolsem.img
INSTALLER_IMG := $(INSTALLER_DIR)/seolsem_installer.img
HDD_IMG := $(INSTALLER_DIR)/build/hdd.img

.PHONY: all clean seolsem installer run-install run-hdd hdd vim-port FORCE

all: $(SEOLSEM_IMG) $(INSTALLER_IMG)

seolsem: $(SEOLSEM_IMG)

installer: $(INSTALLER_IMG)

$(SEOLSEM_IMG): FORCE
	$(MAKE) -C $(SEOLSEM_DIR) seolsem.img

vim-port: $(VIMPORT_STAMP)

$(VIMPORT_STAMP):
	$(MAKE) -C $(VIMPORT_DIR) shim

$(INSTALLER_IMG): FORCE
	$(MAKE) -C $(INSTALLER_DIR) seolsem_installer.img

hdd: $(HDD_IMG)

$(HDD_IMG):
	$(MAKE) -C $(INSTALLER_DIR) hdd

FORCE:

run-install: all hdd
	@echo "When prompted to swap disks, use QEMU monitor command:"
	@echo "  change floppy0 $(SEOLSEM_IMG)"
	$(QEMU) $(QEMU_AUDIO_ARGS) -m 16 -boot a -monitor stdio \
		-drive if=floppy,format=raw,file=$(INSTALLER_IMG) \
		-drive if=ide,format=raw,file=$(HDD_IMG)

run-hdd: hdd
	$(QEMU) $(QEMU_AUDIO_ARGS) -m 16 \
		-drive if=ide,format=raw,file=$(HDD_IMG)

clean:
	@echo "[CLEAN]"
	$(MAKE) -C $(SEOLSEM_DIR) clean
	$(MAKE) -C $(INSTALLER_DIR) clean
	@rm -f *.bin
	@rm -f $(HDD_IMG)
	@rm -rf vim_port/build
	@rm -f vim_port/*.err
