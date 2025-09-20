.PHONY: all
all: bootloader.img

.PHONY: clean
clean: 
	rm -f *.bin bootloader.img

bootloader.img: boot.bin sima.bin
	cat $^ > $@

%.bin: %.asm
	nasm -f bin -o $@ $<
