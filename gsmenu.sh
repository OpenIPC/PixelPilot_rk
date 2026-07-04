#!/bin/bash
#
# gsmenu.sh — PixelPilot settings interface (BOILERPLATE / integrator template)
# ══════════════════════════════════════════════════════════════════════════════
#
# PixelPilot's on-screen menu drives ALL configuration through this single
# script. The command *interface* (the "get/set/button ..." strings matched in
# the case statement below) is fixed: the OSD menu invokes these exact commands.
#
# The *implementation* — how a value is actually read from or written to the
# air unit / ground station — depends entirely on the integrator's OS, firmware
# and tooling (ssh, yaml-cli, nmcli, sysfs, config files, ...). That logic is
# intentionally NOT provided here.
#
# This template ships with:
#   • dummy getters  — print a placeholder current value (+ the allowed values
#                      that the menu widget should offer).
#   • no-op setters  — accept the new value ($5) and do nothing.
#   • no-op buttons  — accept the action and do nothing.
#
# Integrators: replace the bodies below with real logic for your platform.
#
# ── Protocol ──────────────────────────────────────────────────────────────────
#   get <path> :
#       Print the current value on stdout (may be multi-line). For dropdown /
#       slider widgets, follow it with the record separator (\x1e) and the list
#       of allowed values, using emit_values / emit_values_cmd.
#         emit_values "20\n40"        – static list
#         emit_values "0 100"         – numeric range "min max"
#         emit_values_cmd <cmd...>    – dynamic values from a command's stdout
#   set <path> <value> :
#       Apply <value> (positional parameter $5) to the setting.
#   button <path> :
#       Perform a one-shot action (reboot, reset, ...).
#
#   Exit 0 on success. Exit 1 is treated as "handled, ignore".
# ══════════════════════════════════════════════════════════════════════════════

set -o pipefail

# ══════════════════════════════════════════════════════════════════════════════
# Configuration (integrator: define endpoints / credentials / paths here)
# ══════════════════════════════════════════════════════════════════════════════

REMOTE_IP="${REMOTE_IP:-10.5.0.10}"
AIR_FIRMWARE_TYPE="${AIR_FIRMWARE_TYPE:-wfb}"

# ══════════════════════════════════════════════════════════════════════════════
# Protocol helpers (do not remove — used by the menu widgets)
# ══════════════════════════════════════════════════════════════════════════════

# Emit the record separator followed by the allowed values (for dropdowns/sliders).
# Called after the current value has been printed.
#   emit_values "1\n20\n25\n30"        – static list / range
#   emit_values_cmd <command> [args]   – dynamic values from a command
emit_values()     { printf '\x1e'"$1"; }
emit_values_cmd() { printf '\x1e'; "$@"; }

# ══════════════════════════════════════════════════════════════════════════════
# Main command dispatch
# ══════════════════════════════════════════════════════════════════════════════

case "$@" in

# ── Air: WFB-NG ─────────────────────────────────────────────────────────────

    "get air wfbng power")
        echo 20                     # integrator: return current value
        emit_values "1\n20\n25\n30\n35\n40\n45\n50\n55\n58"
        ;;
    "get air wfbng air_channel")
        echo "161"                  # integrator: return current channel
        emit_values "36\n40\n44\n48\n149\n153\n157\n161\n165"
        ;;
    "get air wfbng width")
        echo 20
        emit_values "20\n40"
        ;;
    "get air wfbng mcs_index")
        echo 0
        emit_values "0 10"
        ;;
    "get air wfbng stbc")
        echo 0
        ;;
    "get air wfbng ldpc")
        echo 0
        ;;
    "get air wfbng fec_k")
        echo 8
        emit_values "0 15"
        ;;
    "get air wfbng fec_n")
        echo 12
        emit_values "0 15"
        ;;
    "get air wfbng mlink")
        echo 1500
        emit_values "1500\n1600\n1700\n1800\n1900\n2000\n2100\n2200\n2300\n2400\n2500\n2600\n2700\n2800\n2900\n3000\n3100\n3200\n3300\n3400\n3500\n3600\n3700\n3800\n3900\n4000"
        ;;
    "get air wfbng adaptivelink")
        echo 0
        ;;

    "set air wfbng power"*)         : ;; # integrator: apply $5
    "set air wfbng air_channel"*)   : ;;
    "set air wfbng width"*)         : ;;
    "set air wfbng mcs_index"*)     : ;;
    "set air wfbng stbc"*)          : ;;
    "set air wfbng ldpc"*)          : ;;
    "set air wfbng fec_k"*)         : ;;
    "set air wfbng fec_n"*)         : ;;
    "set air wfbng mlink"*)         : ;;
    "set air wfbng adaptivelink"*)  : ;;

# ── Air: Camera ──────────────────────────────────────────────────────────────

    "get air camera mirror")
        echo 0
        ;;
    "get air camera flip")
        echo 0
        ;;
    "get air camera contrast")
        echo 50
        emit_values "0 100"
        ;;
    "get air camera hue")
        echo 50
        emit_values "0 100"
        ;;
    "get air camera saturation")
        echo 50
        emit_values "0 100"
        ;;
    "get air camera luminace")
        echo 50
        emit_values "0 100"
        ;;
    "get air camera size")
        echo "1920x1080"
        emit_values "1280x720\n1456x816\n1920x1080\n1440x1080\n1920x1440\n2104x1184\n2208x1248\n2240x1264\n2312x1304\n2436x1828\n2512x1416\n2560x1440\n2560x1920\n2720x1528\n2944x1656\n3200x1800\n3840x2160"
        ;;
    "get air camera video_mode")
        echo "16:9 1080p 60"
        emit_values "16:9 720p 30\n\n16:9 720p 30 50HzAC\n16:9 1080p 30\n16:9 1080p 30 50HzAC\n16:9 1440p 30\n16:9 1440p 30 50HzAC\n16:9 4k 2160p 30\n16:9 4k 2160p 30 50HzAC\n16:9 540p 60\n16:9 540p 60 50HzAC\n16:9 720p 60\n16:9 720p 60 50HzAC\n16:9 1080p 60\n16:9 1080p 60 50HzAC\n16:9 1440p 60\n16:9 1440p 60 50HzAC\n16:9 1688p 60\n16:9 1688p 60 50HzAC\n16:9 540p 90\n16:9 540p 90 50HzAC\n16:9 720p 90\n16:9 720p 90 50HzAC\n16:9 1080p 90\n16:9 1080p 90 50HzAC\n16:9 540p 120\n16:9 720p 120\n16:9 816p 120\n4:3 720p 30\n4:3 720p 30 50HzAC\n4:3 960p 30\n4:3 960p 30 50HzAC\n4:3 1080p 30\n4:3 1080p 30 50HzAC\n4:3 1440p 30\n4:3 1440p 30 50HzAC\n4:3 2160p 30\n4:3 2160p 30 50HzAC\n4:3 720p 60\n4:3 720p 60 50HzAC\n4:3 960p 60\n4:3 960p 60 50HzAC\n4:3 1080p 60\n4:3 1080p 60 50HzAC\n4:3 1440p 60\n4:3 1440p 60 50HzAC\n4:3 1688p 60\n4:3 1688p 60 50HzAC\n4:3 720p 90\n4:3 720p 90 50HzAC\n4:3 960p 90\n4:3 960p 90 50HzAC\n4:3 1080p 90\n4:3 1080p 90 50HzAC\n4:3 540p 120\n4:3 720p 120\n4:3 816p 120"
        ;;
    "get air camera fps")
        echo 60
        emit_values "60\n90\n120"
        ;;
    "get air camera bitrate")
        echo 8192
        emit_values "1024\n2048\n3072\n4096\n5120\n6144\n7168\n8192\n9216\n10240\n11264\n12288\n13312\n14336\n15360\n16384\n17408\n18432\n19456\n20480\n21504\n22528\n23552\n24576\n25600\n26624\n27648\n28672\n29692\n30720"
        ;;
    "get air camera codec")
        echo h265
        emit_values "h264\nh265"
        ;;
    "get air camera gopsize")
        echo 1
        emit_values "0 10"
        ;;
    "get air camera rc_mode")
        echo cbr
        emit_values "vbr\navbr\ncbr"
        ;;
    "get air camera rec_enable")
        echo 0
        ;;
    "get air camera rec_split")
        echo 5
        emit_values "0 60"
        ;;
    "get air camera rec_maxusage")
        echo 90
        emit_values "0 100"
        ;;
    "get air camera exposure")
        echo 12
        emit_values "5 50"
        ;;
    "get air camera antiflicker")
        echo disabled
        emit_values "disabled\n50\n60"
        ;;
    "get air camera sensor_file")
        echo imx415_fpv
        emit_values "imx307\nimx335\nimx335_fpv\nimx415_fpv\nimx415_fpv\nimx415_milos10\nimx415_milos15\nimx335_milos12tweak\nimx335_greg15\nimx335_spike5\ngregspike05"
        ;;
    "get air camera fpv_enable")
        echo 0
        ;;
    "get air camera noiselevel")
        echo 0
        emit_values "0 1"
        ;;

    "set air camera mirror"*)       : ;;
    "set air camera flip"*)         : ;;
    "set air camera contrast"*)     : ;;
    "set air camera hue"*)          : ;;
    "set air camera saturation"*)   : ;;
    "set air camera luminace"*)     : ;;
    "set air camera size"*)         : ;;
    "set air camera video_mode"*)   : ;;
    "set air camera fps"*)          : ;;
    "set air camera bitrate"*)      : ;;
    "set air camera codec"*)        : ;;
    "set air camera gopsize"*)      : ;;
    "set air camera rc_mode"*)      : ;;
    "set air camera rec_enable"*)   : ;;
    "set air camera rec_split"*)    : ;;
    "set air camera rec_maxusage"*) : ;;
    "set air camera exposure"*)     : ;;
    "set air camera antiflicker"*)  : ;;
    "set air camera sensor_file"*)  : ;;
    "set air camera fpv_enable"*)   : ;;
    "set air camera noiselevel"*)   : ;;

# ── Air: Telemetry ───────────────────────────────────────────────────────────

    "get air telemetry serial")
        echo ttyS2
        emit_values "ttyS0\nttyS1\nttyS2\nttyS3"
        ;;
    "get air telemetry router")
        echo msposd
        emit_values "mavfwd\nmsposd"
        ;;
    "get air telemetry osd_fps")
        echo 30
        emit_values "0 60"
        ;;
    "get air telemetry gs_rendering")
        echo 0
        ;;

    "set air telemetry serial"*)        : ;;
    "set air telemetry router"*)        : ;;
    "set air telemetry osd_fps"*)       : ;;
    "set air telemetry gs_rendering"*)  : ;;

# ── Air: Alink ───────────────────────────────────────────────────────────────

    "get air alink power_level_0_to_4")
        echo 1
        emit_values "0\n1\n2\n3\n4"
        ;;
    "get air alink fallback_ms")
        echo 1000
        emit_values "1 2000"
        ;;
    "get air alink hold_fallback_mode_s")
        echo 2
        emit_values "1 10"
        ;;
    "get air alink min_between_changes_ms")
        echo 100
        emit_values "1 10000"
        ;;
    "get air alink hold_modes_down_s")
        echo 2
        emit_values "1 10"
        ;;
    "get air alink hysteresis_percent")
        echo 10
        emit_values "0 100"
        ;;
    "get air alink hysteresis_percent_down")
        echo 10
        emit_values "0 100"
        ;;
    "get air alink exp_smoothing_factor")
        echo 0.8
        emit_values "0 1.6"
        ;;
    "get air alink exp_smoothing_factor_down")
        echo 0.8
        emit_values "0 1.6"
        ;;
    "get air alink check_xtx_period_ms")
        echo 100
        emit_values "1 5000"
        ;;
    "get air alink request_keyframe_interval_ms")
        echo 1000
        emit_values "1 5000"
        ;;
    "get air alink osd_level")
        echo 2
        emit_values "0\n1\n2\n3\n4\n5\n6"
        ;;
    "get air alink multiply_font_size_by")
        echo 1.0
        emit_values "0 1.5"
        ;;
    "get air alink"*)
        echo ""                     # integrator: return current value for $4
        ;;

    "set air alink"*)               : ;;

# ── Air: Aalink ──────────────────────────────────────────────────────────────

    "get air aalink SHOW_SIGNAL_BARS")
        echo 0
        ;;
    "get air aalink channel")
        echo 157
        emit_values "36\n40\n44\n48\n52\n56\n60\n64\n100\n104\n108\n112\n116\n120\n124\n128\n132\n136\n140\n144\n149\n153\n157\n161\n165\n36_40\n44_48\n52_56\n60_64\n100_104\n108_112\n116_120\n124_128\n132_136\n140_144\n149_153\n157_161"
        ;;
    "get air aalink SCALE_TX_POWER")
        echo 1.0
        emit_values "0.2 1.2"
        ;;
    "get air aalink THRESH_SHIFT")
        echo 0
        emit_values "-50 50"
        ;;
    "get air aalink OSD_SCALE")
        echo 1.0
        emit_values "0.2 2"
        ;;
    "get air aalink OSD_LEVEL")
        echo 1
        emit_values "0\n1\n2\n3"
        ;;
    "get air aalink THROUGHPUT_PCT")
        echo 100
        emit_values "0 100"
        ;;
    "get air aalink HIGH_TEMP")
        echo 85
        emit_values "70 100"
        ;;
    "get air aalink MCS_SOURCE")
        echo downlink
        emit_values "lowest\ndownlink"
        ;;
    "get air aalink"*)
        echo ""                     # integrator: return current value for $4
        ;;

    "set air aalink channel"*)          : ;;
    "set air aalink SHOW_SIGNAL_BARS"*) : ;;
    "set air aalink"*)                  : ;;

# ── GS: WFB-NG ──────────────────────────────────────────────────────────────

    "get gs wfbng gs_channel")
        echo "161"                  # integrator: return current channel
        emit_values "36\n40\n44\n48\n149\n153\n157\n161\n165"
        ;;
    "get gs wfbng bandwidth")
        echo 20
        emit_values "20\n40"
        ;;
    "get gs wfbng txpower")
        echo 50
        emit_values "1\n100"
        ;;
    "get gs wfbng adaptivelink")
        echo 0
        ;;

    "set gs wfbng gs_channel"*)     : ;;
    "set gs wfbng bandwidth"*)      : ;;
    "set gs wfbng txpower"*)        : ;;
    "set gs wfbng adaptivelink"*)   : ;;

# ── GS: System ──────────────────────────────────────────────────────────────

    "get gs system rx_codec")
        echo "h265"
        emit_values "h265"
        ;;
    "get gs system rx_mode")
        echo wfb
        emit_values "wfb\napfpv"
        ;;
    "get gs system gs_rendering")
        echo 0
        ;;
    "get gs system connector")
        echo HDMI
        emit_values "HDMI"
        ;;
    "get gs system resolution")
        echo "1920x1080@60"
        emit_values "1280x720@60\n1920x1080@60\n2560x1440@60\n3840x2160@60"
        ;;
    "get gs system video_scale")
        echo 1.0
        emit_values "0.5 1.0"
        ;;
    "get gs system gs_live_colortrans")
        echo 0
        ;;
    "get gs system rec_fps")
        echo 60
        emit_values "60\n90\n120"
        ;;
    "get gs system dvr_mode"*)
        echo "raw"
        emit_values "raw\nreencode\nboth"
        ;;
    "get gs system dvr_max_size"*)
        echo -n "40"                # will be multiplied by 100
        emit_values "1 40"
        ;;
    "get gs system dvr_reenc_codec"*)
        echo -n "h264"
        emit_values "h264\nh265"
        ;;
    "get gs system dvr_reenc_resolution"*)
        echo -n "1080p"
        emit_values "720p\n1080p"
        ;;
    "get gs system dvr_reenc_fps"*)
        echo -n "60"
        emit_values "30\n60"
        ;;
    "get gs system dvr_reenc_bitrate"*)
        echo -n "10000"
        emit_values "5000\n10000\n15000\n20000\n25000\n30000\n35000\n40000\n45000\n50000"
        ;;
    "get gs system rec_enabled"*)
        echo 0
        ;;
    "get gs system dvr_osd"*)
        echo 0
        ;;

    "set gs system rx_codec"*)              : ;;
    "set gs system rx_mode"*)               : ;;
    "set gs system gs_rendering"*)          : ;;
    "set gs system connector"*)             : ;;
    "set gs system resolution"*)            : ;;
    "set gs system video_scale"*)           : ;;
    "set gs system gs_live_colortrans"*)    : ;;
    "set gs system rec_fps"*)               : ;;
    "set gs system rec_enabled"*)           : ;;
    "set gs system dvr_mode"*)              : ;;
    "set gs system dvr_max_size"*)          : ;; # needs division by 100
    "set gs system dvr_reenc_codec"*)       : ;;
    "set gs system dvr_reenc_resolution"*)  : ;;
    "set gs system dvr_reenc_fps"*)         : ;;
    "set gs system dvr_reenc_bitrate"*)     : ;;
    "set gs system dvr_osd"*)               : ;;

# ── GS: APFPV ───────────────────────────────────────────────────────────────

    "get gs apfpv ssid")
        echo "OpenIPC"
        ;;
    "get gs apfpv password")
        echo "12345678"
        ;;
    "get gs apfpv wlx"*)
        echo 0
        ;;
    "get gs apfpv status wlx"*)
        echo Disconnected
        ;;

    "set gs apfpv ssid"*)       : ;;
    "set gs apfpv password"*)   : ;;
    "set gs apfpv wlx"*)        : ;;
    "set gs apfpv reset")       : ;;

# ── GS: WiFi ────────────────────────────────────────────────────────────────

    "get gs wifi hotspot")
        echo 0
        ;;
    "get gs wifi wlan")
        echo 0
        ;;
    "get gs wifi ssid")
        echo -n ""
        ;;
    "get gs wifi password")
        echo -n ""
        ;;
    "get gs wifi IP")
        echo ""
        ;;
    "get gs wifi savednetworks")
        # integrator: print ESCAPED_SSID:password per saved network
        : ;;
    "get gs wifi networks")
        # integrator: print ESCAPED_SSID:SECURITY:SIGNAL per visible network
        : ;;

    "set gs wifi connect"*)     : ;;
    "set gs wifi disconnect"*)  : ;;
    "set gs wifi forget"*)      : ;;   # integrator: forget/remove the saved network ($4 = SSID)
    "set gs wifi wlan"*)        : ;;
    "set gs wifi hotspot"*)     : ;;

# ── GS: Main page (info labels) ─────────────────────────────────────────────

    "get gs main Channel")
        echo "161"
        ;;
    "get gs main HDMI-OUT")
        echo "1920x1080@60"
        ;;
    "get gs main Version")
        echo "ddeeaaddbbeeff"
        ;;
    "get gs main Disk")
        echo -e "   Size: 914G\n   Available: 20G\n   Pct: 98%"
        ;;
    "get gs main WFB_NICS")
        echo "wlx1122334566"
        ;;

# ── Buttons / Actions ───────────────────────────────────────────────────────

    "button air actions Reboot")    : ;; # integrator: reboot air unit
    "button gs actions Reboot")     : ;; # integrator: reboot ground station
    "search channel")
        echo "Not implemented"
        echo "Not implemented" >&2
        exit 1
        ;;

# ── NetworkManager Dispatcher ────────────────────────────────────────────────

    "wlx"*"dhcp4-change")
        : ;; # integrator: per-NIC txpower / dispatcher hook

# ── Unknown command ──────────────────────────────────────────────────────────

    *)
        echo "Unknown $@"
        exit 1
        ;;
esac

exit 0
