.PHONY: all
all: seolsem.img

.PHONY: clean
clean: 
	rm -f seolsem.img *.bin

seolsem.img: boot.bin kernel.bin
	cat $^ > $@

%.bin: %.asm
	nasm -f bin -o $@ $<
