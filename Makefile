# DEBIAN_CODENAME=bookworm | bullseye
DEBIAN_CODENAME=bookworm
DEBIAN_HOST=https://cloud.debian.org/images/cloud/$(DEBIAN_CODENAME)
DEBIAN_RELEASE=latest
SKIP_SETUP=0

ifeq ($(DEBIAN_CODENAME),bookworm)
DEBIAN_SYSTEM=debian-12-generic-arm64.tar
else
DEBIAN_SYSTEM=debian-11-generic-arm64.tar
endif

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
qemu_build:
	mkdir -p $(OUTPUT)
	make mount

	sudo rm $(OUTPUT)/etc/resolv.conf
	echo nameserver 1.1.1.1 | sudo tee -a $(OUTPUT)/etc/resolv.conf
	LC_ALL=en_US.UTF-8 sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_build.sh --wipe-boot --debian-codename $(DEBIAN_CODENAME) --build-type bin $(if $(filter 1,$(SKIP_SETUP)),--skip-setup,)
	sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_run.sh --version
	sudo cp $(OUTPUT)/usr/src/PixelPilot_rk/build/pixelpilot .
	make umount

.PHONY: qemu_build_deb
qemu_build_deb:
	mkdir -p $(OUTPUT)
	make mount

	sudo rm $(OUTPUT)/etc/resolv.conf
	echo nameserver 1.1.1.1 | sudo tee -a $(OUTPUT)/etc/resolv.conf
	LC_ALL=en_US.UTF-8 sudo chroot $(OUTPUT) /usr/src/PixelPilot_rk/tools/container_build.sh --wipe-boot --pkg-version $(DEB_VERSION) --debian-codename $(DEBIAN_CODENAME) --build-type deb
	make umount

.PHONY: mount
mount: disk.raw
	sudo mount `sudo losetup -P --show -f disk.raw`p1 $(OUTPUT)
	sudo mkdir -p $(OUTPUT)/usr/src/PixelPilot_rk
	sudo mount -o bind `pwd` $(OUTPUT)/usr/src/PixelPilot_rk
	mkdir .apt_cache || true
	sudo mount -o bind .apt_cache/ $(OUTPUT)/var/cache/apt

.PHONY: umount
umount:
	sudo umount $(OUTPUT)/var/cache/apt
	sudo umount $(OUTPUT)/usr/src/PixelPilot_rk
	sudo umount $(OUTPUT)
	sudo losetup --detach `losetup | grep disk.raw | cut -f 1 -d " "`

.PHONY: clean
clean:
	rm -f pixelpilot-rk-dbgsym_$(DEB_VERSION)*.deb
	rm -f pixelpilot-rk_$(DEB_VERSION)*.buildinfo pixelpilot-rk_$(DEB_VERSION)*.changes pixelpilot-rk_$(DEB_VERSION)*.deb pixelpilot-rk_*.orig.tar.gz
	rm -rf pixelpilot-rk_$(DEB_VERSION)/
	rm -rf output
	rm -rf build
	rm -rf .apt_cache
	rm -f disk.raw
