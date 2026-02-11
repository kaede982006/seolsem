# ===== Seolsem Installer (Floppy #1) =====
SHELL := /bin/sh
AS    := nasm
PY    ?= python3
QEMU  ?= qemu-system-i386
QEMUIMG ?= qemu-img
QEMU_AUDIO_DRV ?= alsa
QEMU_AUDIO_ID ?= snd0
QEMU_AUDIO_ARGS ?= -machine pc,pcspk-audiodev=$(QEMU_AUDIO_ID) -audiodev $(QEMU_AUDIO_DRV),id=$(QEMU_AUDIO_ID)

SEOLSEM_KERNEL_IMG ?= ../seolsem/seolsem.img
SEOLSEM_PROGRAM_IMG ?= ../seolsem/seolsem_programs.img

BOOT_SRC   := bootloader/boot.asm
MBR_SRC    := bootloader/mbr.asm
STAGE2_SRC := bootloader/installer.asm
READ_SRC   := bootloader/read.asm

TOOL       := tools/make_installer_image.py

BUILDDIR   := build
BOOT_BIN   := $(BUILDDIR)/boot.bin
MBR_BIN    := $(BUILDDIR)/mbr.bin
STAGE2_BIN := $(BUILDDIR)/installer.bin
IMG        := seolsem_installer.img
HDD        := $(BUILDDIR)/hdd.img

.PHONY: all clean run hdd

all: $(IMG)

$(BUILDDIR):
	@mkdir -p $@

$(BOOT_BIN): $(BOOT_SRC) $(READ_SRC) | $(BUILDDIR)
	@echo "[AS] $<"
	$(AS) -I bootloader -f bin -o "$@" "$(BOOT_SRC)"

$(MBR_BIN): $(MBR_SRC) | $(BUILDDIR)
	@echo "[AS] $(MBR_SRC)"
	$(AS) -f bin -o "$@" "$(MBR_SRC)"

BOOT32_SRC := bootloader/boot32.asm
BOOT32_BIN := $(BUILDDIR)/boot32.bin

$(BOOT32_BIN): $(BOOT32_SRC) $(READ_SRC) | $(BUILDDIR)
	@echo "[AS] $<"
	$(AS) -I bootloader -f bin -o "$@" "$(BOOT32_SRC)"

$(STAGE2_BIN): $(STAGE2_SRC) $(MBR_BIN) $(BOOT32_BIN) | $(BUILDDIR)
	@echo "[AS] $(STAGE2_SRC)"
	$(AS) -O0 -w-label-redef-late -f bin -o "$@" "$(STAGE2_SRC)"

$(IMG): $(BOOT_BIN) $(STAGE2_BIN) $(TOOL)
	@echo "[IMG] $@"
	$(PY) $(TOOL) --boot $(BOOT_BIN) --stage2 $(STAGE2_BIN) --output $@

hdd: $(HDD)

$(HDD): | $(BUILDDIR)
	@echo "[HDD] $@"
	$(QEMUIMG) create -f raw "$@" 256M

run: $(IMG) hdd $(SEOLSEM_KERNEL_IMG) $(SEOLSEM_PROGRAM_IMG)
	@echo "When prompted to swap disks, use QEMU monitor command:"
	@echo "  change floppy0 $(SEOLSEM_KERNEL_IMG)     # Disk 1 (kernel)"
	@echo "  change floppy0 $(SEOLSEM_PROGRAM_IMG)    # Disk 2 (programs)"
	$(QEMU) $(QEMU_AUDIO_ARGS) -m 16 -boot a -monitor stdio \
		-drive if=floppy,format=raw,file=$(IMG) \
		-drive if=ide,format=raw,file=$(HDD)

clean:
	@echo "[CLEAN]"
	@rm -rf $(BUILDDIR)
	@rm -f $(IMG)

$(SEOLSEM_KERNEL_IMG):
	@$(MAKE) -C ../seolsem seolsem.img

$(SEOLSEM_PROGRAM_IMG):
	@$(MAKE) -C ../seolsem seolsem_programs.img
