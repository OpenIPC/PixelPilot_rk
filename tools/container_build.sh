#!/bin/bash

set -x

if [ "$1" = "--wipe-boot" ]; then
    rm -r /boot/* #save space
fi

# install radxa APT repo, see https://radxa-repo.github.io/bullseye/
keyring="/usr/src/PixelPilot_rk/keyring.deb"
version="$(curl -L https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION)"

curl -L --output "$keyring" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/download/${version}/radxa-archive-keyring_${version}_all.deb"
dpkg -i $keyring
tee /etc/apt/sources.list.d/70-radxa.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye/ bullseye main"
tee /etc/apt/sources.list.d/80-rockchip.list <<< "deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/bullseye rockchip-bullseye main"

apt-get update
apt clean
apt-get install -y cmake g++ git pkg-config librockchip-mpp-dev libcairo-dev libdrm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libspdlog-dev nlohmann-json3-dev libmsgpack-dev libgpiod-dev libyaml-cpp-dev
apt clean


cd /usr/src/PixelPilot_rk

cmake -B build
cmake --build build -j4 --target install
