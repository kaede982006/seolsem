.PHONY: all
all: seolsem.qcow2

.PHONY: clean
clean: 
	rm seolsem.qcow2
seolsem.qcow2: seolsem.img
	qemu-img create -f qcow2 seolsem.qcow2 64K
	qemu-img convert -O qcow2 seolsem.img seolsem.qcow2
seolsem.img: bootloader/bootloader.img kernel/kernel.img
	cat $^ > seolsem.img
