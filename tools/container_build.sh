#!/bin/bash

# This file is intended to be used from inside a container.

set -x

ROOTDIR=/usr/src/PixelPilot_rk
BUILD_TYPE="deb"
DEBIAN_CODENAME=bookworm
SKIP_SETUP=0

print_help() {
    echo "$0 --wipe-boot --pkg-version X.Y.Z --root-dir /path/to/surces --build-type <deb|bin|debug> --skip-setup --help"
}


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
        --build-type)
            BUILD_TYPE=$2
            shift 2
            ;;
        --debian-codename)
            DEBIAN_CODENAME=$2
            shift 2
            ;;
        --skip-setup)
            SKIP_SETUP=1
            shift
            ;;
        --help)
            print_help
            exit 0
            ;;
        -*)
            echo "Unknown option $1"
            print_help
            exit 1
            ;;
    esac
done

if [ $SKIP_SETUP -lt 1 ]; then
    # needed for GPG tools to work
    if [ ! -e /dev/null ]; then
        mknod /dev/null c 1 3
        chmod 666 /dev/null
    fi

    # install radxa APT repo, see:
    # * https://radxa-repo.github.io/bookworm/
    # * https://radxa-repo.github.io/bullseye/
    # * https://radxa-repo.github.io/rk3566-bookworm/
    keyring="${ROOTDIR}/keyring.deb"
    version="$(curl -L https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION)"
    curl -L --output "$keyring" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/download/${version}/radxa-archive-keyring_${version}_all.deb"
    dpkg -i $keyring
    rm $keyring

    case $DEBIAN_CODENAME in
        bookworm)
            tee /etc/apt/sources.list.d/70-radxa.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bookworm/ bookworm main"
            tee /etc/apt/sources.list.d/80-radxa-rk3566.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/rk3566-bookworm rk3566-bookworm main"
            ;;
        bullseye)
            tee /etc/apt/sources.list.d/70-radxa.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye/ bullseye main"
            tee /etc/apt/sources.list.d/80-rockchip.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye rockchip-bullseye main"
            ;;
    esac
    apt-get update

    case $BUILD_TYPE in
        deb)
            apt-get install -y cmake build-essential git pkg-config devscripts equivs
            ;;
        bin|debug)
            apt-get install -y cmake build-essential git pkg-config librockchip-mpp-dev libcairo-dev libdrm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libspdlog-dev nlohmann-json3-dev libmsgpack-dev libgpiod-dev libyaml-cpp-dev
            ;;
    esac
fi

cd $ROOTDIR

case $BUILD_TYPE in
    deb)
        ORIG_ARCHIVE=pixelpilot-rk_${PKG_VERSION}.orig.tar.gz
        SRCDIR=pixelpilot-rk_${PKG_VERSION}
        BUILD_DEPS_FILE=pixelpilot-rk-build-deps_${PKG_VERSION}-1_all.deb

        # Generate the "orig" package and then unpack it in <name>_<version> directory to get a clean source tree
        git ls-files --recurse-submodules | tar -caf $ORIG_ARCHIVE -T-
        rm -rf $SRCDIR
        mkdir $SRCDIR
        tar -axf $ORIG_ARCHIVE -C $SRCDIR
        cd $SRCDIR

        # Install build dependencies
        mk-build-deps
        apt-get install -y ./$BUILD_DEPS_FILE
        rm pixelpilot-rk-build-deps*

        dpkg-buildpackage -uc -us -b

        #debuild -S -I
        ;;
    bin)
        cmake -B build
        cmake --build build -j`nproc` --target install
        ;;
    debug)
        cmake -B build -DCMAKE_BUILD_TYPE=Debug
        cmake --build build -j`nproc` --target install
esac
