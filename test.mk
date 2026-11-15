.DELETE_ON_ERROR:

.PHONY: test-clean
test-clean:
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk clean
	rm -rf test_image test.hdd mbrtest.hdd test.iso loopback_dev edk2-ovmf-bins.tar.gz

.PHONY: test.hdd
test.hdd:
	rm -f test.hdd
	dd if=/dev/null bs=1024k seek=64 of=test.hdd
	PATH=$$PATH:/usr/sbin:/sbin parted -s test.hdd mklabel msdos
	PATH=$$PATH:/usr/sbin:/sbin parted -s test.hdd mkpart primary 2048s 100%

.PHONY: mbrtest.hdd
mbrtest.hdd:
	rm -f mbrtest.hdd
	dd if=/dev/null bs=1024k seek=64 of=mbrtest.hdd
	printf "o\nn\np\n1\n2048\n\nt\n6\na\nw\n\n" | fdisk mbrtest.hdd -H 16 -S 63

.PHONY: fat12-test
fat12-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-bios
	$(MAKE) initium
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 12 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/initium bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: fat16-test
fat16-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-bios
	$(MAKE) initium
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 16 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/initium bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: legacy-fat16-test
legacy-fat16-test:
	$(MAKE) test-clean
	$(MAKE) mbrtest.hdd
	fdisk -l mbrtest.hdd
	$(MAKE) initium-bios
	$(MAKE) initium
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show mbrtest.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 16 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/initium bios-install mbrtest.hdd
	qemu-system-i386 -cpu pentium2 -m 16M -M isapc -net none   -hda mbrtest.hdd -debugcon stdio

.PHONY: fat32-test
fat32-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-bios
	$(MAKE) initium
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/initium bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: iso9660-test
iso9660-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-bios
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -R $(BINDIR)/* test_image/boot/
	cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	xorriso -as mkisofs -b boot/initium-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table test_image/ -o test.iso
	qemu-system-x86_64 -net none -smp 4   -cdrom test.iso -debugcon stdio

.PHONY: full-hybrid-test
full-hybrid-test:
	$(MAKE) edk2-ovmf-bins
	$(MAKE) test-clean
	$(MAKE) all
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -R $(BINDIR)/* test_image/boot/
	cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	$(MKDIR_P) test_image/EFI/BOOT
	cp $(BINDIR)/BOOT*.EFI test_image/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/initium-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/initium-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label test_image/ -o test.iso
	$(BINDIR)/initium bios-install test.iso
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -hda test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-ia32.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-ia32.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -hda test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -net none -smp 4   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -net none -smp 4   -hda test.iso -debugcon stdio

.PHONY: pxe-test
pxe-test:
	$(MAKE) test-clean
	$(MAKE) initium-bios
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -R $(BINDIR)/* test_image/boot/
	cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	qemu-system-x86_64  -smp 4  -netdev user,id=n0,tftp=./test_image,bootfile=boot/initium-bios-pxe.bin -device rtl8139,netdev=n0,mac=00:00:00:11:11:11 -debugcon stdio

# OVMF's PXE stack needs an entropy source to come up: without the virtio RNG
# no network boot option is registered at all.
.PHONY: uefi-x86-64-pxe-test
uefi-x86-64-pxe-test:
	$(MAKE) edk2-ovmf-bins
	$(MAKE) test-clean
	$(MAKE) initium-uefi-x86-64
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -R $(BINDIR)/* test_image/boot/
	cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci -netdev user,id=n0,tftp=./test_image,bootfile=boot/BOOTX64.EFI -device virtio-net-pci,netdev=n0,mac=00:00:00:11:11:11 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -boot n -debugcon stdio

.PHONY: uefi-x86-64-test
uefi-x86-64-test:
	$(MAKE) edk2-ovmf-bins
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-uefi-x86-64
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTX64.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -hda test.hdd -debugcon stdio

.PHONY: uefi-ia32-test
uefi-ia32-test:
	$(MAKE) edk2-ovmf-bins
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) initium-uefi-ia32
	$(MAKE) -C '$(call SHESCAPE,$(SRCDIR))/test' -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -R $(BINDIR)/* test_image/boot/
	sudo cp -R '$(call SHESCAPE,$(SRCDIR))/test'/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTIA32.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-ia32.fd,readonly=on -net none -smp 4 -device virtio-tablet-pci -device virtio-mouse-pci   -hda test.hdd -debugcon stdio
