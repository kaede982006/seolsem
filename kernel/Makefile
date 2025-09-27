.PHONY: all
all: kernel.img
# -mt 를 -ms 로 변경
CFLAGS = -s -ecc -1 -ms -zl -od -d2 -iinclude
.PHONY: clean
clean: 
	rm -f kernel.img kernel.bin *.obj

sima_mem.obj: sima_mem.c 
	wcc $(CFLAGS) $< -fo=$@
api.obj: api.asm
	nasm -f obj $< -o $@
kernel_entry.obj: kernel_entry.asm
	nasm -f obj $< -o $@
kernel.obj: kernel.c
	wcc $(CFLAGS) $< -fo=$@
kernel.img: kernel_entry.obj kernel.obj api.obj sima_mem.obj
	wlink @link.lk NAME kernel.img
	@size=$$(\stat -c%s $@); \
	padded_size=$$(( (size + 511) / 512 * 512 )); \
	if [ $$padded_size -eq 0 ]; then padded_size=512; fi; \
	truncate -s $$padded_size $@
