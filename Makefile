# ===== Meta Makefile (Build Both Floppies) =====
SHELL := /bin/sh
QEMU  ?= qemu-system-i386
QEMUIMG ?= qemu-img
PY ?= python3
QEMU_AUDIO_DRV ?= alsa
QEMU_AUDIO_ID ?= snd0
QEMU_AUDIO_ARGS ?= -machine pc,pcspk-audiodev=$(QEMU_AUDIO_ID) -audiodev $(QEMU_AUDIO_DRV),id=$(QEMU_AUDIO_ID)

SEOLSEM_DIR := seolsem
INSTALLER_DIR := seolsem_installer

SEOLSEM_IMG := $(SEOLSEM_DIR)/seolsem.img
SEOLSEM_PROGRAM_IMG := $(SEOLSEM_DIR)/seolsem_programs.img
INSTALLER_IMG := $(INSTALLER_DIR)/seolsem_installer.img
HDD_IMG := $(INSTALLER_DIR)/build/hdd.img

.PHONY: all clean seolsem programs installer run-install run-hdd hdd seolc test-host qemu-smoke FORCE

all: $(SEOLSEM_IMG) $(SEOLSEM_PROGRAM_IMG) $(INSTALLER_IMG)

seolsem: $(SEOLSEM_IMG)

programs: $(SEOLSEM_PROGRAM_IMG)

installer: $(INSTALLER_IMG)

seolc:
	@test -n "$(SRC)" || (echo "Usage: make seolc SRC=path/file.XC [OUT=path/file.XEF]" && exit 1)
	@if [ -n "$(OUT)" ]; then \
		$(PY) $(SEOLSEM_DIR)/tools/seolc_host.py "$(SRC)" -o "$(OUT)" --target native16; \
	else \
		$(PY) $(SEOLSEM_DIR)/tools/seolc_host.py "$(SRC)" --target native16; \
	fi

test-host:
	$(MAKE) -C $(SEOLSEM_DIR) test-host

qemu-smoke:
	$(MAKE) -C $(SEOLSEM_DIR) qemu-smoke

$(SEOLSEM_IMG): FORCE
	$(MAKE) -C $(SEOLSEM_DIR) seolsem.img

$(SEOLSEM_PROGRAM_IMG): FORCE
	$(MAKE) -C $(SEOLSEM_DIR) seolsem_programs.img

$(INSTALLER_IMG): FORCE
	$(MAKE) -C $(INSTALLER_DIR) seolsem_installer.img

hdd: $(HDD_IMG)

$(HDD_IMG):
	$(MAKE) -C $(INSTALLER_DIR) hdd

FORCE:

run-install: all hdd
	@echo "When prompted to swap disks, use QEMU monitor command:"
	@echo "  change floppy0 $(SEOLSEM_IMG)            # Disk 1 (kernel)"
	@echo "  change floppy0 $(SEOLSEM_PROGRAM_IMG)    # Disk 2 (programs)"
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
