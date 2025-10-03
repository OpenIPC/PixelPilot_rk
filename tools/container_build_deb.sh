#!/bin/bash

set -x

ROOTDIR=/usr/src/PixelPilot_rk

while [[ $# -gt 0 ]]; do
    case $1 in
        --wipe-boot)
            rm -r /boot/* #save space
            shift
            ;;
        --pkg-version)
            PKG_VERSION=$2
            shift 2
            ;;
        --root-dir)
            ROOTDIR=$2
            shift 2
            ;;
        -*)
            echo "Unknown option $1"
            exit 1
            ;;
    esac
done

# install radxa APT repo, see https://radxa-repo.github.io/bullseye/
keyring="${ROOTDIR}/keyring.deb"
version="$(curl -L https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION)"

curl -L --output "$keyring" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/download/${version}/radxa-archive-keyring_${version}_all.deb"
dpkg -i $keyring
tee /etc/apt/sources.list.d/70-radxa.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye/ bullseye main"
tee /etc/apt/sources.list.d/80-rockchip.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye rockchip-bullseye main"

apt-get update
apt clean
apt-get install -y cmake g++ git pkg-config librockchip-mpp-dev libcairo-dev libdrm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libspdlog-dev nlohmann-json3-dev libmsgpack-dev libgpiod-dev libyaml-cpp-dev devscripts
apt clean

cd $ROOTDIR

# Generate the "orig" package and then unpack it in <name>_<version> directory to get a clean source tree
ORIG_ARCHIVE=pixelpilot-rk_${PKG_VERSION}.orig.tar.gz
SRCDIR=pixelpilot-rk_${PKG_VERSION}


git ls-files --recurse-submodules | tar -caf $ORIG_ARCHIVE -T-
rm -rf $SRCDIR
mkdir $SRCDIR
tar -axf $ORIG_ARCHIVE -C $SRCDIR
cd $SRCDIR

dpkg-buildpackage -uc -us -b
#debuild -S -I
