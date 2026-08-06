# PixelPilot_rk — Copilot Instructions

## Current Branch: `majestic-to-waybeam`

This branch converts the GStreamer pipeline source from **Majestic** (OpenIPC streamer) to **Waybeam**. The Waybeam video encoder API reference is at:
```
~/waybeam_venc_api_ref
```
Consult it when working with Waybeam encoder parameters, pipeline configuration, or any Waybeam-specific calls.

## Project Overview

WFB-ng video decoder and ground station display application for **Rockchip ARM64 platforms** (tested on RK3566/RK3588s). It uses Rockchip MPP for hardware-accelerated H.264/H.265 decoding, DRM/KMS for display, and Cairo/LVGL for the OSD. The real target hardware is an SBC running Debian Bullseye or Bookworm; there is a simulator mode for local development without hardware.

## Build Commands

All builds use CMake out-of-source in `build/`.

**Production build (on Rockchip hardware):**
```bash
cmake -B build
sudo cmake --build build --target install
```

**Debug build (enables ASAN + UBSAN, verbose spdlog):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

**Tests (Catch2, ARM64 only):**
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
./build/pixelpilot_tests                     # all tests
./build/pixelpilot_tests "[ExpressionTree]"  # single test tag
```

**GSMenu simulator (local, no Rockchip hardware required):**
```bash
./sim.sh   # builds with -DUSE_SIMULATOR=ON and runs via SDL2
```
Simulator controls: `w/a/s/d/Enter` for navigation, `t` to toggle drone detection.

**Cross-compile (QEMU on any x86 host):**
```bash
make qemu_build DEBIAN_CODENAME=bookworm     # binary
make qemu_build_deb DEBIAN_CODENAME=bookworm # .deb package
make qemu_test DEBIAN_CODENAME=bookworm      # run tests
```
Requires `qemu-user-static` installed. Downloads a Debian arm64 disk image.

## Architecture

### Threading model
`src/main.cpp` is the orchestrator — it sets up DRM, spawns all threads, and tears them down on SIGTERM/SIGINT. Each subsystem runs as a dedicated pthread:

| Thread function | Source | Purpose |
|---|---|---|
| `__OSD_THREAD__` | `src/osd.cpp` | Renders OSD via Cairo onto a DRM overlay plane |
| `__WFB_CLI_THREAD__` | `src/wfbcli.cpp` | Polls wfb-ng statistics API and publishes Facts |
| `FrameProcessor::__THREAD__` | `src/frame_processor.cpp` | Color-correct + OSD blend on GPU |
| `FrameProcessor::__TIMER_THREAD__` | `src/frame_processor.cpp` | Paces frames to the DVR encoder at a steady FPS |
| Mavlink thread | `src/mavlink.c` | Reads MAVLink UDP stream, publishes telemetry Facts |

Thread functions follow the naming convention `__NAME_THREAD__` (uppercase with double underscores).

### OSD / Facts pub-sub system
The OSD (`src/osd.cpp`) is built around a **Facts** publish-subscribe pattern:

- **Publishers** call `osd_publish_<type>()` / `osd_add_<type>()` (declared in `src/osd.h`) to emit named data points with optional string tags.
- **Widgets** subscribe in JSON config by `"name"` and optional `"tags"` key-value pairs.
- Fact types are strict: `int`, `uint`, `double`, `bool`, `string` — no implicit casting between them.
- A `"convert"` expression on numeric facts (e.g. `"x / 1000"`) converts the value to `double` and appends `.converted` to the fact name.
- New widget types must be implemented in C++ inside `src/osd.cpp`. The JSON config only parameterises existing widget types.

OSD widgets render to a Cairo surface on a DRM plane separate from the video plane, managed by `src/drm.c`.

### Video pipeline
1. **GStreamer** (`src/gstrtpreceiver.cpp`) — receives RTP video over UDP/multicast.
2. **Rockchip MPP decoder** — hardware-decoded frames (YUV420SP) arrive in `main.cpp`'s decoder loop.
3. **FrameProcessor** (`src/frame_processor.cpp`) — receives decoded frames, optionally applies GPU color correction (EGL/GLES2 via `src/frame_colorcorrect.*` + `src/osd_gl.*`), blends OSD, then paces output to the DVR encoder.
4. **MppEncoder** (`src/mpp_encoder.cpp`) — hardware re-encodes for DVR recording (`src/dvr.cpp`).
5. **DRM/KMS** (`src/drm.c`) — final display; video on one plane, OSD on another.

### GSMenu (LVGL on-screen menu)
`src/gsmenu/` implements a hierarchical on-screen menu using LVGL. Structure:
- `gs_main.c` — creates root menu; coordinates ground-station pages (`gs_*`)
- `air_*` files — communicate with the air unit (camera, wfb-ng TX profiles, etc.)
- `executor.c` — runs shell commands triggered by menu actions, with blocking/non-blocking variants
- `ui.c` / `styles.c` — shared LVGL widget helpers and visual styles

The simulator (`src/simulator.c`, `-DUSE_SIMULATOR`) builds only the GSMenu with SDL2 so it can be developed without Rockchip hardware.

### Configuration files
| File | Location | Purpose |
|---|---|---|
| `pixelpilot.yaml` | `/etc/pixelpilot/pixelpilot.yaml` | GPIO pin mapping, GSMenu enable, OS sensors, restream IP |
| `config_osd.json` | `/etc/pixelpilot/config_osd.json` | OSD widget layout and fact subscriptions |
| `lv_conf.h` | project root | LVGL compile-time configuration (not inside `lvgl/`) |

## Key Conventions

### Mixed C/C++ boundary
Headers that are included from both C and C++ use `#pragma once`. C++ files that call into C APIs wrap the includes in `extern "C" { ... }`. Pure C files (e.g., `drm.c`, `mavlink.c`, `gsmenu/*.c`) are compiled as C99; the main application is C++17.

### Test-only code exposure
The `TEST` preprocessor macro (defined when `BUILD_TESTS=ON` via `target_compile_definitions`) gates test-accessor wrapper classes in `src/osd.hpp`:
```cpp
#ifdef TEST
class TestExpressionTree { ... };
class TestTplTextWidget  { ... };
#endif
```
Tests in `tests/test_osd.cpp` use these wrappers to reach private internals.

### Simulator guard
`USE_SIMULATOR` is defined when building with `-DUSE_SIMULATOR=ON`. Code that requires real Rockchip hardware (MPP, DRM, RGA) must be guarded or excluded from simulator builds.

### Negative OSD coordinates
In `config_osd.json`, negative `x`/`y` values are relative to the right/bottom edge of the screen. This is a display-resolution-independent positioning system used throughout the OSD widget layout.

### spdlog logging
All logging uses `spdlog`. In `Debug` builds `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE` is defined, enabling trace-level messages. Use the `spdlog::` API rather than `printf`/`fprintf` for any new logging.

### Submodule
`lvgl/` is a git submodule. Always run `git submodule update --init` after cloning or when `lvgl/` appears empty.

### gsmenu.sh mirror
`gsmenu.sh` in this repo is mirrored in the [sbc-groundstations](https://github.com/OpenIPC/sbc-groundstations) repository. Whenever `gsmenu.sh` is modified here, the same change must be applied to:
```
~/sbc-groundstations/package/pixelpilot/files/gsmenu.sh
```
