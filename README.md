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

- drm, cairo, mpp, logging, json, msgpack

```
sudo apt install libdrm-dev libcairo-dev librockchip-mpp-dev libspdlog-dev nlohmann-json3-dev libmsgpack-dev libgpiod-dev
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

### OSD config

OSD is set-up declaratively in `/etc/pixelpilot/config_osd.json` file (or whatever is set via `--osd-config`
command line key.

OSD is described as an array of widgets which may subscribe to fact updates (they receive each fact
update they subscribe to) and those widgets are periodically rendered on the screen (in the order they
declared in config). So the goal is that widgets would decide how they should be rendered based on
the values of the facts they are subscribed to. If widget needs to render not the latest value of the
fact, but some processed value (like average / max / total etc), the widget should keep the necessary
state for that. There is a helper class `MovingAverage` that would be helpful to calculate common
statistical parameters.

Each fact has a specific datatype: one of `int` (signed integer) / `uint` (unsigned integer) /
`double` (floating point) / `bool` (true/false) / `string` (text). Type cast is currently not
implemented, so it is important to use the right type in the widget code and templates.

Facts may also have tags: a set of string key->value pairs. Widget may filter facts by tags as well as by name.
Currently there are several generic OSD widgets and several specific ad-hoc ones. There are quite a
lot of facts to which widgets can subscribe to:

| Fact                           | Type | Description                                                               |
|:-------------------------------|:-----|:--------------------------------------------------------------------------|
| `dvr.recording`                | bool | Is DVR currently recording?                                               |
| `video.width`                  | uint | The width of the video stream                                             |
| `video.height`                 | uint | The height of the video stream                                            |
| `video.displayed_frame`        | uint | Published  with value "1" each time a new video frame is displayed        |
| `video.decode_and_handover_ms` | uint | Time from the moment packet is received to time it is displayed on screen |
| `video.decoder_feed_time_ms`   | uint | Time to feed the video packet to hardware decoder                         |
| `gstreamer.received_bytes`     | uint | Number of bytes received from gstreamer (published for each packet)       |
| `osd.custom_message`           | str  | The custom message passed via `--osd-custom-message` feature              |

There are many facts based on Mavlink telemetry, see `mavlink.c`. All of them have tags "sysid" and
"compid", but some have extra tags.
Currently implemented fact categories are grouped by Mavlink message types:

| Fact                                | Type     | Description                                                                                                                                                                                                                       |
|:------------------------------------|:---------|:----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `mavlink.heartbeet.base_mode.armed` | bool     | Is drone armed?                                                                                                                                                                                                                   |
| `mavlink.raw_imu.*`                 | int      | Raw values from gyroscope and accelerometer, See [RAW_IMU](https://mavlink.io/en/messages/common.html#RAW_IMU)                                                                                                                    |
| `mavlink.sys_status.*`              | int/uint | Some of the fields from [SYS_STATUS](https://mavlink.io/en/messages/common.html#SYS_STATUS)                                                                                                                                       |
| `mavlink.battery_status.*`          | int      | Some of the fields from [BATTERY_STATUS](https://mavlink.io/en/messages/common.html#BATTERY_STATUS)                                                                                                                               |
| `mavlink.rc_channels_raw.chanN`     | uint     | Raw values of remote control chanels (N is from 1 to 8)                                                                                                                                                                           |
| `mavlink.gps_raw.*`                 | int/uint | Raw data from GNSS sensor, see [GPS_RAW_INT](https://mavlink.io/en/messages/common.html#GPS_RAW_INT)                                                                                                                              |
| `mavlink.vfr_hud.*`                 | double   | Metrics common for fixed wing OSDs, see [VFR_HUD](https://mavlink.io/en/messages/common.html#VFR_HUD)                                                                                                                             |
| `mavlink.global_position_int.*`     | int      | Position estimation based on sensor fusion, see [GLOBAL_POSITION_INT](https://mavlink.io/en/messages/common.html#GLOBAL_POSITION_INT)                                                                                             |
| `mavlink.attitude.*`                | double   | See [ATTITUDE](https://mavlink.io/en/messages/common.html#ATTITUDE)                                                                                                                                                               |
| `mavlink.radio_status.*`            | uint/int | Status of various radio equipment. Tags `{sysid: 3, compid: 68}` encode the [injected status of WFB-ng receiver](https://github.com/svpcom/wfb-ng/blob/4ea700606c259960ea169bad1f55fde77850013d/wfb_ng/conf/master.cfg#L227-L228) |

More can be easily added later. You can use `DebugWidget` to inspect the current raw value of the fact(s).

Pixelpilot is also able to connect to WFB-ng statistics API and extract some of the facts from there.
Receiving packets statistics (each fact has "id" tag - channel name, eg "video"/"mavlink"/"tunnel" etc):

| Fact                          | Type | Description                          |
|:------------------------------|:-----|:-------------------------------------|
| `wfbcli.rx.packets.all`       | uint | Number of packets received           |
| `wfbcli.rx.packets.all_bytes` | uint | Number of bytes received             |
| `wfbcli.rx.packets.dec_err`   | uint | Number of packets lost               |
| `wfbcli.rx.packets.dec_ok`    | uint | Number of good packets               |
| `wfbcli.rx.packets.fec_rec`   | uint | Number of packets recovered with FEC |

Receiving per-antenna statistics (each fact has "id" - channel name and "ant_id" - antenna number tags)
| Fact                           | Type   | Description                                     |
|:-------------------------------|:-------|:------------------------------------------------|
| `wfbcli.rx.ant_stats.freq`     | uint   | Antenna frequency, MHz, eg 5800                 |
| `wfbcli.rx.ant_stats.mcs`      | uint   | MCS value for this antenna                      |
| `wfbcli.rx.ant_stats.bw`       | uint   | Bandwidth value for antenna (MHz)               |
| `wfbcli.rx.ant_stats.pkt_recv` | uint   | Number of WiFi packets received by this antenna |
| `wfbcli.rx.ant_stats.rssi_avg` | int    | Average RSSI for this antenna                   |
| `wfbcli.rx.ant_stats.snr_avg`  | double | Average SNR for this antenna                    |

Transmitting packets stats (same tags as receiving packets):

| Fact                               | Type | Description                              |
|:-----------------------------------|:-----|:-----------------------------------------|
| `wfbcli.tx.packets.injected`       | uint | Number of successfully injected packets  |
| `wfbcli.tx.packets.injected_bytes` | uint | Number of successfully injected bytes    |
| `wfbcli.tx.packets.dropped`        | uint | Number of dropped packets                |
| `wfbcli.tx.packets.truncated`      | uint | Number of truncated (?) packets          |
| `wfbcli.tx.packets.fec_timeouts`   | uint | ?                                        |
| `wfbcli.tx.packets.incoming`       | uint | Even TX interface may receive, n packets |
| `wfbcli.tx.packets.incoming_bytes` | uint | Even TX interface may receive, n bytes   |

Transmitting per-antenna stats (same tags as receiving antennas):

| Fact                           | Type | Description                                |
|:-------------------------------|:-----|:-------------------------------------------|
| `wfbcli.tx.ant_stats.pkt_sent` | uint | Number packets sent through this antenna   |
| `wfbcli.tx.ant_stats.pkt_drop` | uint | Number packets dropped by this antenna     |
| `wfbcli.tx.ant_stats.lat_avg`  | uint | Average injection latency for this antenna |

#### Widgets

Currently we have generic widgets and more ad-hoc specific ones. Generic widgets normally can be used
to display any fact (as long as datatype matches):

* `TextWidget` - displays a static string of text
* `IconTextWidget` - displays a graphical icon followed by a static text
* `TplTextWidget` - displays a string of text by replacing placeholders with the fact values
* `IconTplTextWidget` - displays a graphical icon followed by templatized text string
* `BoxWidget` - displays a static square. Might be good as a background.
* `BarChartWidget` - displays a simple bar chart for the single fact's statistics. Each bar represents
 either minimum or maximum or sum or count or average of the fact over time interval. Can be used to show
 eg the average video bitrate or RSSI or FPS.
* `PopupWidget` - displays a stacked pop-ups with text facts which fade-away after timeout.
* `DebugWidget` - displays debug information (name, type, tags, value) about fact(s)
* `IconSelectorWidget` - display a icon based on a fact's value

Specific widgets expect quite concrete facts as input:

* `DvrStatusWidget` - shows up when DVR is recording and is hidden when not.
  Uses `dvr.recording` fact
* `VideoWidget` - shows FPS and video resolution.
  Uses `video.displayed_frame`, `video.width`, `video.height` facts
* `VideoBitrateWidget` - shows video bitrate (not radio link, but video!).
  Uses `gstreamer.received_bytes` fact
* `VideoDecodeLatencyWidget` - shows video frame decode and display latency (avg/min/max).
  Uses `video.decode_and_handover_ms` fact
* `GPSWidget` - displays GPS fix type (no fix / 2D fix / 3D fix etc) and GPS coordinates.
  Uses `mavlink.gps_raw.fix_type`, `mavlink.gps_raw.lat` and `mavlink.gps_raw.lon` facts

## Menu

The menu provides options to modify air and ground settings.
Navigation is controlled via a GPIO button, adhering to Ruby wiring conventions.
PixelPilot_rk will take ownership of the needed gpios.
The provided gsmenu.sh script needs https://github.com/openipc/yaml-cli/

### Navigation
Up/Down – Cycles through menu items (wraps around at the top and bottom of the page/sections).
Left/Right/Ok (Back/Menu/QA1 in Ruby terms) – Adjusts selected values.
Ok becomes Rec/QA1 when menu is not shown.

When not in menu any navigation key will open the menu.

## Known issues

1. Video is cropped when the fpv feed resolution is bigger than the screen mode.
1. Crashes when video feed resolution is higher than the screen resolution.

## The way it works

It uses `gstreamer` to read the RTP media stream from video UDP.
It uses `mpp` library to decode MPEG frames using Rockchip hardware decoder.
It uses [Direct Rendering Manager (DRM)](https://en.wikipedia.org/wiki/Direct_Rendering_Manager) to
display video on the screen, see `drm.c`.
It uses `mavlink` decoder to read Mavlink telemetry from telemetry UDP (if enabled), see `mavlink.c`
It uses `cairo` library to draw OSD elements (if enabled), see `osd.c`.
It writes non-decoded MPEG stream to file as DVR (if enabled) using `minimp4.h` library.

Pixelpilot starts several threads:

* main thread:
  controls gstreamer which reads RTP, extracts MPEG frames and
  - feeds them to MPP hardware decoder
  - sends them to DVR thread via mutex-protected `std::queue` (if enabled)
* DVR_THREAD (if enabled):
  reads video frames and start/stop/shutdown commands from main thread via `std::queue` and writes
  frames them to disk using `minimp4` library.
  It yields on a condition variable for DVR queue
* FRAME_THREAD:
  reads decoded video frames from MPP hardware decoder and forwards them to `DISPLAY_THREAD`
  through DRM `output_list` protected by `video_mutex`.
  Seems that thread vields on `mpi->decode_get_frame()` call waiting for HW decoder to return a new frame
* DISPLAY_THREAD:
  reads decoded frames and OSD from `video_mutex`-protected `output_list` and calls `drm*` functions to
  render them on the screen.
  The loop yields on `video_mutex` and `video_cond` waiting for a new frame to
  display from FRAME_THREAD
* MAVLINK_THREAD (if OSD and mavlink configured):
  reads mavlink packets from UDP, decodes and updates `osd_vars` (without any mutex).
  The loop yields on UDP read.
* WFBCLI_THREAD (if OSD is enabled):
  connects to the local WFB instance stats API, reads JSON stats messages and publishes OSD facts.
  The loop yields on TCP read.
* OSD_THREAD (if OSD is enabled):
  takes `drm_fd`, `output_list` and JSON config as thread parameters,
  receives Facts through mutex-with-timeout-protected `std::queue`, feeds Facts to widgets and
  periodically draws widgets on a buffer inside `output_list` using Cairo library.
  There exists legacy OSD, is based on `osd_vars`, draws using Cairo library, to be removed.
  The loop yields on queue's mutex with timeout (timeout in order to re-draw OSD at fixed intervals).

## Release

* update project version in `CMakeList.txt`, `project(pixelpilot, VERSION <X.Y.Z>)`, commit
* push that commit to master (either directly or with PR)
* tag the tip of the master branch with the same `<X.Y.Z>` version
* run `git push --tags`; it will publish a new GitHub release
