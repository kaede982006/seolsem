
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
PROGRAM_IMG := seolsem_programs.img
QCOW2_IMG   := seolsem.qcow2
IMG_TOOL    := tools/make_fat12_image.py
SEOLC_TOOL  := tools/seolc_host.py
PROGRAM_MANIFEST := programs/manifest.json
PROGRAM_ENV := programs/XENV.ENV
PROGRAMS_DIR := programs
PROGRAMS_MK := $(PROGRAMS_DIR)/Makefile
SEOLC_INC_DIRS ?=
SEOLC_I_FLAGS := $(foreach d,$(SEOLC_INC_DIRS),-I $(d))

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

.PHONY: all clean run bootloader kernel seolc programs test-host qemu-smoke

all: $(QCOW2_IMG) $(PROGRAM_IMG)

programs: $(PROGRAM_IMG)

run: $(QCOW2_IMG)
	$(QEMU) $(QEMU_AUDIO_ARGS) -hda $(QCOW2_IMG)

$(QCOW2_IMG): $(RAW_IMG)
	@echo "[QCOW2] $@"
	$(QEMUIMG) convert -f raw -O qcow2 $< $@

$(RAW_IMG): $(KERNEL_IMG) $(BOOT_BIN) $(STAGE2_BIN) $(IMG_TOOL)
	@echo "[FAT12] $@"
	$(PY) $(IMG_TOOL) --boot $(BOOT_BIN) --stage2 $(STAGE2_BIN) --kernel $(KERNEL_IMG) --no-programs --output $@

$(PROGRAM_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(IMG_TOOL) $(PROGRAM_MANIFEST) $(PROGRAM_ENV) $(PROGRAMS_MK)
	$(MAKE) -C $(PROGRAMS_DIR) all
	@echo "[FAT12] $@"
	$(PY) $(IMG_TOOL) --boot $(BOOT_BIN) --stage2 $(STAGE2_BIN) --manifest $(PROGRAM_MANIFEST) --no-kernel --output $@

# 서브메이크: 항상 위임 (하위 Makefile이 변경 여부 판단)
bootloader:
	$(MAKE) -C bootloader

kernel:
	$(MAKE) -C kernel

$(BOOT_BIN) $(STAGE2_BIN): bootloader

$(KERNEL_IMG): kernel

clean:
	@echo "[CLEAN]"
	@$(RM) -f $(CLEAN_FILES)
	@rm -rf $(CLEAN_DIRS)
	$(MAKE) -C $(PROGRAMS_DIR) clean
	@find . -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
	@find . -name '*.pyc' -type f -delete 2>/dev/null || true
	$(MAKE) -C bootloader clean
	$(MAKE) -C kernel clean

seolc:
	@test -n "$(SRC)" || (echo "Usage: make -C seolsem seolc SRC=path/file.XC [OUT=path/file.XEF]" && exit 1)
	@if [ -n "$(OUT)" ]; then \
		$(PY) $(SEOLC_TOOL) "$(SRC)" -o "$(OUT)" --target native16 $(SEOLC_I_FLAGS); \
	else \
		$(PY) $(SEOLC_TOOL) "$(SRC)" --target native16 $(SEOLC_I_FLAGS); \
	fi

test-host:
	PYTHONPATH=tools $(PY) -m unittest discover -s tools/tests -p 'test_*.py'

qemu-smoke: $(RAW_IMG) $(PROGRAM_IMG)
	$(PY) tools/qemu_smoke.py --kernel-img $(RAW_IMG) --programs-img $(PROGRAM_IMG)
