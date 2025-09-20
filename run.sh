#!/bin/bash
cd bootloader
make
cd ..
cd kernel
make
cd ..

cat bootloader/bootloader.img kernel/kernel.img > seolsem.img
qemu-system-i386 -hda seolsem.img
