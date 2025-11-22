% pixelpilot-rk(SECTION) | User Commands
%
% "October  1 2025"

[comment]: # The lines above form a Pandoc metadata block. They must be
[comment]: # the first ones in the file.
[comment]: # See https://pandoc.org/MANUAL.html#metadata-blocks for details.

[comment]: # pandoc -s -f markdown -t man package.md -o package.1
[comment]: #
[comment]: # A manual page package.1 will be generated. You may view the
[comment]: # manual page with: nroff -man package.1 | less. A typical entry
[comment]: # in a Makefile or Makefile.am is:
[comment]: #
[comment]: # package.1: package.md
[comment]: #         pandoc --standalone --from=markdown --to=man $< --output=$@
[comment]: #
[comment]: # The pandoc binary is found in the pandoc package. Please remember
[comment]: # that if you create the nroff version in one of the debian/rules
[comment]: # file targets, such as build, you will need to include pandoc in
[comment]: # your Build-Depends control field.

[comment]: # lowdown is a low dependency, lightweight alternative to
[comment]: # pandoc as a markdown to manpage translator. Use with:
[comment]: #
[comment]: # package.1: package.md
[comment]: #         lowdown -s -Tman -o $@ $<
[comment]: #
[comment]: # And add lowdown to the Build-Depends control field.

[comment]: # Remove the lines starting with '[comment]:' in this file in order
[comment]: # to avoid warning messages.

# NAME

pixelpilot-rk - OpenIPC video display client for wfb-ng

# SYNOPSIS

```
  Usage:
    pixelpilot [Arguments]

  Arguments:
    --config <configfile>  - Load pixelpilot config from file      (Default: /etc/pixelpilot.yaml)

    -p <port>              - UDP port for RTP video stream         (Default: 5600)

    --socket <socket>      - read data from socket

    --mavlink-port <port>  - UDP port for mavlink telemetry        (Default: 14550)

    --mavlink-dvr-on-arm   - Start recording when armed

    --codec <codec>        - Video codec, should be the same as on VTX  (Default: h265 <h264|h265>)

    --log-level <level>    - Log verbosity level, debug|info|warn|error (Default: info)

    --osd                  - Enable OSD

    --osd-config <file>    - Path to OSD configuration file

    --osd-refresh <rate>   - Defines the delay between osd refresh (Default: 1000 ms)

    --osd-custom-message   - Enables the display of /run/pixelpilot.msg (beta feature, may be removed)

    --dvr-template <path>  - Save the video feed (no osd) to the provided filename template.
                             DVR is toggled by SIGUSR1 signal
                             Supports placeholders %Y - year, %m - month, %d - day,
                             %H - hour, %M - minute, %S - second. Ex: /media/DVR/%Y-%m-%d_%H-%M-%S.mp4

    --dvr-sequenced-files  - Prepend a sequence number to the names of the dvr files

    --dvr-start            - Start DVR immediately

    --dvr-framerate <rate> - Force the dvr framerate for smoother dvr, ex: 60

    --dvr-fmp4             - Save the video feed as a fragmented mp4

    --screen-mode <mode>   - Override default screen mode. <width>x<heigth>@<fps> ex: 1920x1080@120

    --video-plane-id       - Override default drm plane used for video by plane-id

    --video-scale <factor> - Scale video output size (0.5 <= factor <= 1.0) (Default: 1.0)

    --osd-plane-id         - Override default drm plane used for osd by plane-id

    --disable-vsync        - Disable VSYNC commits

    --screen-mode-list     - Print the list of supported screen modes and exit.

    --wfb-api-port         - Port of wfb-server for cli statistics. (Default: 8003)
                             Use "0" to disable this stats

    --version              - Show program version

```

# DESCRIPTION

WFB-ng client (Video Decoder) for Rockchip platform powered by the Rockchip MPP library. It also
displays a simple cairo based OSD that shows the bandwidth, decoding latency, and framerate of
the decoded video, and wfb-ng link statistics.

# OPTIONS

The program follows the usual GNU command line syntax, with long options
starting with two dashes ('-'). A summary of options is included below. For
a complete description, see the **info**(1) files.

**--config \<configfile\>**
: Load pixelpilot config from file      (Default: /etc/pixelpilot.yaml)

**-p \<port\>**
: UDP port for RTP video stream         (Default: 5600)

**--socket \<socket\>**
: read data from socket

**--mavlink-port \<port\>**
: UDP port for mavlink telemetry        (Default: 14550)

**--mavlink-dvr-on-arm**
: Start recording when armed

**--codec \<codec\>**
: Video codec, should be the same as on VTX  (Default: h265 <h264|h265>)

**--log-level \<level\>**
: Log verbosity level, debug|info|warn|error (Default: info)

**--osd**
: Enable OSD

**--osd-config \<file\>**
: Path to OSD configuration file

**--osd-refresh \<rate\>**
: Defines the delay between osd refresh (Default: 1000 ms)

**--osd-custom-message**
: Enables the display of /run/pixelpilot.msg (beta feature, may be removed)

**--dvr-template \<path\>**
: Save the video feed (no osd) to the provided filename template.
  DVR is toggled by SIGUSR1 signal
  Supports placeholders %Y - year, %m - month, %d - day,
  %H - hour, %M - minute, %S - second. Ex: /media/DVR/%Y-%m-%d_%H-%M-%S.mp4

**--dvr-sequenced-files**
: Prepend a sequence number to the names of the dvr files

**--dvr-start**
: Start DVR immediately

**--dvr-framerate \<rate\>**
: Force the dvr framerate for smoother dvr, ex: 60

**--dvr-fmp4**
: Save the video feed as a fragmented mp4

**--screen-mode \<mode\>**
: Override default screen mode. `<width>x<heigth>@<fps>` ex: 1920x1080@120

**--video-plane-id**
: Override default drm plane used for video by plane-id

**--video-scale \<factor\>**
: Scale video output size (0.5 <= factor <= 1.0) (Default: 1.0)

**--osd-plane-id**
: Override default drm plane used for osd by plane-id

**--disable-vsync**
: Disable VSYNC commits

**--screen-mode-list**
: Print the list of supported screen modes and exit.

**--wfb-api-port**
: Port of wfb-server for cli statistics. (Default: 8003)
  Use "0" to disable this stats

**--version**
: Show program version

# FILES

/etc/default/pixelpilot
:   Start-up configuration

# ENVIRONMENT


# DIAGNOSTICS

To increase the log verbosity, use `--log-level` parameter, however keep in mind that
`debug` level is only enabled in debug builds.

# BUGS

The upstream BTS can be found at https://github.com/OpenIPC/PixelPilot_rk

# SEE ALSO

**wfb-ng**(1)

# AUTHOR

Sergey Prokhorov <seriy.pr@gmail.com>
:   Wrote this manpage for the Debian system.

# COPYRIGHT

Copyright © 2025 Sergey Prokhorov

This manual page was written for the Debian system (and may be used by
others).

Permission is granted to copy, distribute and/or modify this document under
the terms of the GNU General Public License, Version 2 or (at your option)
any later version published by the Free Software Foundation.

On Debian systems, the complete text of the GNU General Public License
can be found in /usr/share/common-licenses/GPL.

[comment]: #  Local Variables:
[comment]: #  mode: markdown
[comment]: #  End:
