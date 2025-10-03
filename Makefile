# https://cloud.debian.org/images/cloud/bullseye/latest/debian-11-generic-arm64.tar.xz
# https://github.com/OpenIPC/sbc-groundstations/releases/download/zero3w-v1.7.0/radxaZero3wrev2.img.xz

DEBIAN_HOST=https://cloud.debian.org/images/cloud/bullseye
DEBIAN_RELEASE=latest
DEBIAN_SYSTEM=debian-11-generic-arm64.tar
OUTPUT=output
DEB_VERSION=1.3.0

all:
	cmake -B build
	cmake --build build -j`nproc` --target install

$(DEBIAN_SYSTEM).xz:
	wget -nv $(DEBIAN_HOST)/$(DEBIAN_RELEASE)/$(DEBIAN_SYSTEM).xz

disk.raw: $(DEBIAN_SYSTEM).xz
	tar -xf $(DEBIAN_SYSTEM).xz
	touch disk.raw

.PHONY: qemu_build
qemu_build: disk.raw
	mkdir -p $(OUTPUT)
	sudo mount `sudo losetup -P --show -f disk.raw`p1 $(OUTPUT)
	sudo mkdir -p $(OUTPUT)/usr/src/PixelPilot_rk
	sudo mount -o bind `pwd` $(OUTPUT)/usr/src/PixelPilot_rk

	#sudo cp -r build.sh run.sh CMakeLists.txt pixelpilot_config.h.in src lvgl lv_conf.h $(OUTPUT)/home
	sudo rm $(OUTPUT)/etc/resolv.conf
	echo nameserver 1.1.1.1 | sudo tee -a $(OUTPUT)/etc/resolv.conf
	sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_build.sh --wipe-boot
	sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_run.sh --version
	sudo cp $(OUTPUT)/usr/src/PixelPilot_rk/build/pixelpilot .
	sudo umount $(OUTPUT)/usr/src/PixelPilot_rk
	sudo umount $(OUTPUT)

-PHONY: qemu_build_deb
qemu_build_deb: disk.raw
	mkdir -p $(OUTPUT)
	sudo mount `sudo losetup -P --show -f disk.raw`p1 $(OUTPUT)
	sudo mkdir -p $(OUTPUT)/usr/src/PixelPilot_rk
	sudo mount -o bind `pwd` $(OUTPUT)/usr/src/PixelPilot_rk

	sudo rm $(OUTPUT)/etc/resolv.conf
	echo nameserver 1.1.1.1 | sudo tee -a $(OUTPUT)/etc/resolv.conf
	sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_build_deb.sh --wipe-boot --pkg-version $(DEB_VERSION)
	sudo umount $(OUTPUT)/usr/src/PixelPilot_rk
	sudo umount $(OUTPUT)
