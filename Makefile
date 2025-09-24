.PHONY: all
all: seolsem.qcow2

.PHONY: clean
clean:
	rm seolsem.qcow2 seolsem.img
	make -C bootloader clean
	make -C kernel clean
.PHONY: run
run:
	qemu-system-i386 -hda seolsem.qcow2
seolsem.qcow2: seolsem.img
	qemu-img create -f qcow2 seolsem.qcow2 64K
	qemu-img convert -O qcow2 seolsem.img seolsem.qcow2
seolsem.img: bootloader/bootloader.img kernel/kernel.img
	cat $^ > $@

bootloader/bootloader.img: bootloader/Makefile
	make -C bootloader

kernel/kernel.img: kernel/Makefile
	cd kernel
	make -C kernel



