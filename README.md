# PixelPilot_rk
> [!IMPORTANT]
> Warning, this is an experimental project.
>
> Use this software at your own risk.

## Introduction

WFB-ng client (Video Decoder) for Rockchip platform powered by the [Rockchip MPP library](https://github.com/rockchip-linux/mpp).
It also displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of the decoded video, and wfb-ng link statistics.

This project is based on a unique frozen development [FPVue_rk](https://github.com/gehee/FPVue_rk) by [Gee He](https://github.com/gehee).

Tested on RK3566 (Radxa Zero 3W) and RK3588s (Orange Pi 5).

## Compilation

Build on the Rockchip linux system directly.

## Install dependencies

- drm, cairo, mpp

```
sudo apt install libdrm-dev libcairo-dev librockchip-mpp-dev
```

- gstreamer

```
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev
```

## Build Instructions

Build and run application in production environment:

```
cmake -B build
sudo cmake --build build --target install
build/pixelpilot
```

Build and run application for debugging purposes:

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
build/pixelpilot --osd
```

## Usage

Show command line options:
```
pixelpilot --help
```

## Known issues

1. Video is cropped when the fpv feed resolution is bigger than the screen mode.
1. Crashes when video feed resolution is higher than the screen resolution.

## The way it works

It uses `mpp` library to decode MPEG frames using Rockchip hardware decoder.
It uses `gstreamer` to read the RTP media stream from video UDP.
It uses `mavlink` decoder to read Mavlink telemetry from telemetry UDP (if enabled), see `mavlink.c`
It uses `cairo` library to draw OSD elements (if enabled), see `osd.c`.
It uses [Direct Rendering Manager (DRM)](https://en.wikipedia.org/wiki/Direct_Rendering_Manager) to
display video on the screen, see `drm.c`.
It writes raw MPEG stream to file as DVR (if enabled) using `minimp4.h` library.

Pixelpilot starts several threads:

* main thread
  controls gstreamer which reads RTP, extracts MPEG frames and
  - feeds them to MPP hardware decoder
  - sends them to DVR thread via mutex-protected `std::queue` (if enabled)
* DVR_THREAD (if enabled)
  reads frames from main thread via `std::queue` and writes them to disk using `minimp4` library
  it yields on a condition variable for DVR queue or `kill` signal variable
* FRAME_THREAD
  reads decoded video frames from MPP hardware decoder and forwards them to `DISPLAY_THREAD`
  through DRM `output_list` protected by `video_mutex`
  Seems that thread vields on `mpi->decode_get_frame()` call waiting for HW decoder to return a new frame
* DISPLAY_THREAD
  reads frames and OSD from `video_mutex`-protected `output_list` and calls `drm*` functions to
  render them on the screen
  The loop yields on `video_mutex` and `video_cond` waiting for a new frame to
  display from FRAME_THREAD
* MAVLINK_THREAD (if OSD and mavlink configured)
  reads mavlink packets from UDP, decodes and updates `osd_vars` (without any mutex)
  The loop yields on UDP read
* OSD_THREAD (if OSD is enabled)
  takes `drm_fd` and `output_list` as thread parameters
  draws telemetry on a buffer inside `output_list` based on `osd_vars` using Cairo library
  The loop yelds on explicit `sleep` to control refresh rate

## Release

* update project version in `CMakeList.txt`, `project(pixelpilot, VERSION <X.Y.Z>)`, commit
* push that commit to master (either directly or with PR)
* tag the tip of the master branch with the same `<X.Y.Z>` version
* run `git push --tags`
