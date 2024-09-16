# Hot Plug Wifi Adapters for OpenIPC Radxa GroundStation (PixelPilot_rk)

## Description
Configure the `autoload-wfb-nics.sh` script in your Radxa GroundStation to enable the support for hot plugging Wifi Adapters.

## Features
- Hot plug wfb-ng supported wireless NICs
- Wifibroadcast will automatically be restarted with the currently plugged in adapters
- No system reboot required
- No configuration changes for wfb-ng required


## Installation
Installation involves copying these script files into the filesystem of radxa. This can be either done using ssh and scp if your radxa is already connected to your home network using wifi, or you can directly copy the files into it by putting the SD card in a card reader, and mounting the rootfs filesystem in your PC.
- Download as zip and extract or Clone the repo
- This will create a folder hot-plug-wfb-nics
- Copy this folder into `/home/radxa/` directory on your ground station. Either use ssh if already connected to radxa or directly copy the hot-plug-wfb-nics folder into your sd-card's `rootfs` partition inside home/radxa directory. To mount this partition in Windows/Mac you can use a Linux virtual machine or some 3rd party tools like `Ext2Fsd` for Windows and `Paragon ExtFS for Mac`
- Access radxa's command-line either through ssh or by directly connecting a monitor and keyboard.
- Execute the following commands one-by-one to move the scripts from hot-plug-wfb-nics folder to correct locations:
  - `cd /home/radxa/hot-plug-wfb-nics`
  - `sudo chmod +x autoload-wfb-nics.sh`
  - `sudo cp autoload-wfb-nics.sh /home/radxa/scripts/`
  - `sudo cp init-nics.service /etc/systemd/system/`
  - `sudo systemctl enable init-nics.service`
  - `sudo cp 98-custom-wifi.rules /etc/udev/rules.d/`
  - `sudo cp 99-custom-wifi.rules /etc/udev/rules.d/`
- Reboot the radxa

## Usage
- Once installed, you can plug or remove wifi adapters into the ground station and it will automatically detect and restart wifibroadcast with the correct adapters
- Internal Wifi adapter should work normally and you should be able to connect to your network using it
 