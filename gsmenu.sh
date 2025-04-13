#!/bin/bash
set -o pipefail
SSH='timeout -k 1 11 sshpass -p 12345 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ControlMaster=auto -o ControlPath=/run/ssh_control:%h:%p:%r -o ControlPersist=15s -o ServerAliveInterval=3 -o ServerAliveCountMax=2 root@10.5.0.10 '

case "$@" in
    "values air wfbng mcs_index")
        echo -n 0 10
        ;;
    "values air wfbng fec_k")
        echo -n 0 15
        ;;
    "values air wfbng fec_n")
        echo -n 0 15
        ;;
    "values air camera contrast")
        echo -n 0 100
        ;;
    "values air camera hue")
        echo -n 0 100
        ;;
    "values air camera saturation")
        echo -n 0 100
        ;;
    "values air camera luminace")
        echo -n 0 100
        ;;
    "values air camera gopsize")
        echo -n 0 10
        ;;
    "values air camera rec_split")
        echo -n 0 60
        ;;
    "values air camera rec_maxusage")
        echo -n 0 100
        ;;
    "values air camera exposure")
        echo -n 5 50
        ;;
    "values air camera noiselevel")
        echo -n 0 1
        ;;
    "values air telemetry tel_mcs_index")
        echo -n 0 15
        ;;
    "values air telemetry aggregate")
        echo -n 0 50
        ;;
    "values air telemetry rc_channel")
        echo -n 0 16
        ;;
    "values air wfbng power")
        echo -e "1\n20\n25\n30\n35\n40\n45\n50\n55\n58"
        ;;
    "values air wfbng air_channel")
        iw list | grep MHz | grep -v disabled | grep -v "radar detection" | grep \* | tr -d '[]' | awk '{print $4 " (" $2 " " $3 ")"}' | grep '^[1-9]' | sort -n |  uniq
        ;;
    "values air wfbng bandwidth")
        echo -e "20\n40"
        ;;
    "values air camera size")
        echo -e "1280x720\n1456x816\n1920x1080\n1440x1080\n1920x1440\n2104x1184\n2208x1248\n2240x1264\n2312x1304\n2436x1828\n2512x1416\n2560x1440\n2560x1920\n2720x1528\n2944x1656\n3200x1800\n3840x2160"
        ;;
    "values air camera fps")
        echo -e "60\n90\n120"
        ;;
    "values air camera bitrate")
        echo -e "1024\n2048\n3072\n4096\n5120\n6144\n7168\n8192\n9216\n10240\n11264\n12288\n13312\n14336\n15360\n16384\n17408\n18432\n19456\n20480\n21504\n22528\n23552\n24576\n25600\n26624\n27648\n28672\n29692\n30720"
        ;;
    "values air camera codec")
        echo -e "h264\nh265"
        ;;
    "values air camera rc_mode")
        echo -e "vbr\navbr\ncbr"
        ;;
    "values air camera antiflicker")
        echo -e "disabled\n50\n60"
        ;;
    "values air camera sensor_file")
        echo -e "/etc/sensors/imx307.bin\n/etc/sensors/imx335.bin\n/etc/sensors/imx335_fpv.bin\n/etc/sensors/imx415_fpv.bin\n/etc/sensors/imx415_fpv.bin\n/etc/sensors/imx415_milos10.bin\n/etc/sensors/imx415_milos15.bin\n/etc/sensors/imx335_milos12tweak.bin\n/etc/sensors/imx335_greg15.bin\n/etc/sensors/imx335_spike5.bin\n/etc/sensors/gregspike05.bin"
        ;;
    "values air telemetry tty")
        echo -e "/dev/ttyS0\n/dev/ttyS1\n/dev/ttyS2"
        ;;
    "values air telemetry speed")
        echo -e "4800\n9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600"
        ;;
    "values air telemetry router")
        echo -e "mavfwd\nmavlink-routerd\nmsposd"
        ;;

    "get air camera mirror")
        [ "true" = $($SSH cli -g .image.mirror) ] && echo 1 || echo 0
        ;;
    "get air camera flip")
        [ "true" = $($SSH cli -g .image.flip) ] && echo 1 || echo 0
        ;;
    "get air camera contrast")
        $SSH cli -g .image.contrast | tr -d '\n'
        ;;
    "get air camera hue")
        $SSH cli -g .image.hue | tr -d '\n'
        ;;
    "get air camera saturation")
        $SSH cli -g .image.saturation | tr -d '\n'
        ;;
    "get air camera luminace")
        $SSH cli -g .image.luminance | tr -d '\n'
        ;;
    "get air camera size")
        $SSH cli -g .video0.size | tr -d '\n'
        ;;
    "get air camera fps")
        $SSH cli -g .video0.fps | tr -d '\n'
        ;;
    "get air camera bitrate")
        $SSH cli -g .video0.bitrate | tr -d '\n'
        ;;
    "get air camera codec")
        $SSH cli -g .video0.codec | tr -d '\n'
        ;;
    "get air camera gopsize")
        $SSH cli -g .video0.gopSize | tr -d '\n'
        ;;
    "get air camera rc_mode")
        $SSH cli -g .video0.rcMode | tr -d '\n'
        ;;
    "get air camera rec_enable")
         [ "true" = $($SSH cli -g .records.enabled | tr -d '\n') ] && echo 1 || echo 0
        ;;
    "get air camera rec_split")
        $SSH cli -g .records.split | tr -d '\n'
        ;;
    "get air camera rec_maxusage")
        $SSH cli -g .records.maxUsage | tr -d '\n'
        ;;
    "get air camera exposure")
        $SSH cli -g .isp.exposure | tr -d '\n'
        ;;
    "get air camera antiflicker")
        $SSH cli -g .isp.antiFlicker | tr -d '\n'
        ;;
    "get air camera sensor_file")
        $SSH cli -g .isp.sensorConfig | tr -d '\n' || [ $? -eq 1 ] && exit 0
        ;;
    "get air camera fpv_enable")
        $SSH cli -g .fpv.enabled | grep -q true && echo 1 || echo 0
        ;;
    "get air camera noiselevel")
        $SSH cli -g .fpv.noiseLevel | tr -d '\n' || [ $? -qe 1 ] && exit 0
        ;;

    "set air camera mirror"*)
        if [ "$5" = "on" ]
        then 
            $SSH 'cli -s .image.mirror true && killall -1 majestic'
        else
            $SSH 'cli -s .image.mirror false && killall -1 majestic'
        fi
        ;;
    "set air camera flip"*)
        if [ "$5" = "on" ]
        then 
            $SSH 'cli -s .image.flip true && killall -1 majestic'
        else
            $SSH 'cli -s .image.flip false && killall -1 majestic'
        fi
        ;;
    "set air camera contrast"*)
        $SSH "cli -s .image.contrast $5 && killall -1 majestic"
        ;;
    "set air camera hue"*)
        $SSH "cli -s .image.hue $5 && killall -1 majestic"
        ;;
    "set air camera saturation"*)
        $SSH "cli -s .image.saturation $5 && killall -1 majestic"
        ;;
    "set air camera luminace"*)
        $SSH "cli -s .image.luminance $5 && killall -1 majestic"
        ;;
    "set air camera size"*)
        $SSH "cli -s .video0.size $5 && killall -1 majestic"
        ;;
    "set air camera fps"*)
        $SSH "cli -s .video0.fps $5 && killall -1 majestic"
        ;;
    "set air camera bitrate"*)
        $SSH "cli -s .video0.bitrate $5 && killall -1 majestic"
        ;;
    "set air camera codec"*)
        $SSH "cli -s .video0.codec $5 && killall -1 majestic"
        ;;
    "set air camera gopsize"*)
        $SSH "cli -s .video0.gopSize $5 && killall -1 majestic"
        ;;
    "set air camera rc_mode"*)
        $SSH "cli -s .video0.rcMode $5 && killall -1 majestic"
        ;;
    "set air camera rec_enable"*)
        if [ "$5" = "on" ]
        then 
            $SSH 'cli -s .records.enable true && killall -1 majestic'
        else
            $SSH 'cli -s .records.enable false && killall -1 majestic'
        fi
        ;;
    "set air camera rec_split"*)
        $SSH "cli -s .records.split $5 && killall -1 majestic"
        ;;
    "set air camera rec_maxusage"*)
        $SSH "cli -s .records.maxUsage $5 && killall -1 majestic"
        ;;
    "set air camera exposure"*)
        $SSH "cli -s .isp.exposure $5 && killall -1 majestic"
        ;;
    "set air camera antiflicker"*)
        $SSH "cli -s .isp.antiFlicker $5 && killall -1 majestic"
        ;;
    "set air camera sensor_file"*)
        $SSH "cli -s .isp.sensorConfig $5 && killall -1 majestic"
        ;;
    "set air camera fpv_enable"*)
        if [ "$5" = "on" ]
        then     
            $SSH 'cli -s .fpv.enabled true && killall -1 majestic'
        else
            $SSH 'cli -s .fpv.enabled false && killall -1 majestic'
        fi
        ;;
    "set air camera noiselevel"*)
        $SSH "cli -s .fpv.noiseLevel $5 && killall -1 majestic"
        ;;

    "get air telemetry tty")
        $SSH grep ^serial /etc/telemetry.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air telemetry speed")
        $SSH grep ^baud /etc/telemetry.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air telemetry router")
        router=$($SSH grep ^router /etc/telemetry.conf | cut -d = -f 2| tr -d '\n')
        case "$router" in
            0) echo -n "mavfwd" ;;
            1) echo -n "mavlink-routerd" ;;
            2) echo -n "msposd" ;;
            *) echo "Unknown router value: $router" >&2; exit 1 ;;
        esac
        ;;
    "get air telemetry tel_mcs_index")
        $SSH grep ^mcs_index /etc/telemetry.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air telemetry aggregate")
        $SSH grep ^aggregate /etc/telemetry.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air telemetry rc_channel")
        $SSH grep ^channels /etc/telemetry.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air telemetry gs_rendering")
        $SSH grep "\\\-osd" /usr/bin/telemetry | grep -q osd && echo 0 || echo 1
        ;;

    "set air telemetry tty"*)
        $SSH 'sed -i "s#^serial=.*#serial='$5'#" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry speed"*)
        $SSH 'sed -i "s/^baud=.*/baud='$5'/" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry router"*)
        case "$5" in
            "mavfwd") router=0;;
            "mavlink-routerd") router=1;;
            "msposd") router=2;;
            *) echo "Unknown router value: $router" >&2; exit 1 ;;
        esac
        $SSH 'sed -i "s/^router=.*/router='$router'/" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry tel_mcs_index"*)
        $SSH 'sed -i "s/^mcs_index=.*/mcs_index='$5'/" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry aggregate"*)
        $SSH 'sed -i "s/^aggregate=.*/aggregate='$5'/" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry rc_channel"*)
        $SSH 'sed -i "s/^channels=.*/channels='$5'/" /etc/telemetry.conf && telemetry stop ; telemetry start'
        ;;
    "set air telemetry gs_rendering"*)
        if [ "$5" = "on" ]
        then 
            $SSH "sed -i 's/--out 127.0.0.1/--out 10.5.0.1/' /usr/bin/telemetry && sed -i 's/ -osd//' /usr/bin/telemetry"
            $SSH "telemetry stop ; telemetry start"
        else
            $SSH "sed -i 's/--out 10.5.0.1/--out 127.0.0.1/' /usr/bin/telemetry && sed -i 's/ -r/ -osd -r/' /usr/bin/telemetry"
            $SSH "telemetry stop ; telemetry start"
        fi        
        ;;

    "get air wfbng power")
        $SSH grep ^driver_txpower_override /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng air_channel")
        channel=$($SSH grep ^channel /etc/wfb.conf | cut -d = -f 2| tr -d '\n')
        iw list | grep "\[$channel\]" | tr -d '[]' | awk '{print $4 " (" $2 " " $3 ")"}' | sort -n | uniq | tr -d '\n'        
        ;;
    "get air wfbng bandwidth")
        $SSH grep ^bandwidth /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng mcs_index")
        $SSH grep ^mcs_index /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng stbc")
        $SSH grep ^stbc /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng ldpc")
        $SSH grep ^ldpc /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng fec_k")
        $SSH grep ^fec_k /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng fec_n")
        $SSH grep ^fec_n /etc/wfb.conf | cut -d = -f 2| tr -d '\n'
        ;;
    "get air wfbng adaptivelink")
        $SSH grep ^alink_drone /etc/rc.local | grep -q 'alink_drone' && echo 1 || echo 0
        ;;

    "set air wfbng power"*)
        $SSH 'sed -i "s/^driver_txpower_override=.*/driver_txpower_override='$5'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        ;;
    "set air wfbng air_channel"*)
        channel=$(echo $5 | awk '{print $1}')
        $SSH 'sed -i "s/^channel=.*/channel='$channel'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        sed -i "s/^wifi_channel =.*/wifi_channel = $channel/" /etc/wifibroadcast.cfg
        systemctl restart wifibroadcast.service
        ;;
    "set air wfbng bandwidth"*)
        $SSH 'sed -i "s/^bandwidth=.*/bandwidth='$5'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        ;;
    "set air wfbng mcs_index"*)
        $SSH 'sed -i "s/^mcs_index=.*/mcs_index='$5'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        ;;
    "set air wfbng stbc"*)
        if [ "$5" = "on" ]
        then 
            $SSH 'sed -i "s/^stbc=.*/stbc=1/" /etc/wfb.conf'
            $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        else
            $SSH 'sed -i "s/^stbc=.*/stbc=0/" /etc/wfb.conf'
            $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        fi
        ;;
    "set air wfbng ldpc"*)
        if [ "$5" = "on" ]
        then 
            $SSH 'sed -i "s/^ldpc=.*/ldpc=1/" /etc/wfb.conf'
            $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        else
            $SSH 'sed -i "s/^ldpc=.*/ldpc=0/" /etc/wfb.conf'
            $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        fi        ;;
    "set air wfbng fec_k"*)
        $SSH 'sed -i "s/^fec_k=.*/fec_k='$5'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        ;;
    "set air wfbng fec_n"*)
        $SSH 'sed -i "s/^fec_n=.*/fec_n='$5'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        ;;
    "set air wfbng adaptivelink"*)
        if [ "$5" = "on" ]
        then
            $SSH 'sed -i "/alink_drone &/d" /etc/rc.local && sed -i -e "\$i alink_drone &" /etc/rc.local && cli -s .video0.qpDelta -12 && killall -1 majestic && alink_drone &'
        else
            $SSH 'killall -q -9 alink_drone;  sed -i "/alink_drone &/d" /etc/rc.local  ; cli -d .video0.qpDelta && killall -1 majestic'
        fi
        ;;

    "values gs wfbng gs_channel")
        iw list | grep MHz | grep -v disabled | grep -v "radar detection" | grep \* | tr -d '[]' | awk '{print $4 " (" $2 " " $3 ")"}' | grep '^[1-9]' | sort -n |  uniq
        ;;
    "values gs wfbng bandwidth")
        echo -e "20\n40"
        ;;
    "values gs system resolution")
        drm_info -j /dev/dri/card0 2>/dev/null | jq -r '."/dev/dri/card0".connectors[1].modes[] | select(.name | contains("i") | not) | .name + "@" + (.vrefresh|tostring)' | sort | uniq
        ;;
    "values gs system rec_fps")
        echo -e "60\n90\n120"
        ;;

    "get gs system gs_rendering")
        [ "$(grep ground /config/scripts/osd | cut -d ' ' -f 3)" = "ground" ] && echo 1 || echo 0
        ;;
    "get gs system resolution")
        drm_info -j /dev/dri/card0 2>/dev/null | jq -r '."/dev/dri/card0".crtcs[0].mode| .name + "@" + (.vrefresh|tostring)' | tr -d '\n'
        ;;
    "get gs system rec_fps")
        grep fps /config/scripts/rec-fps | cut -d ' ' -f 3  | tr -d '\n'
        ;;
    "set gs system gs_rendering"*)
        if [ "$5" = "off" ]
        then
            sed -i 's/^render =.*/render = air/' /config/scripts/osd
            killall -q msposd_rockchip
        else
            sed -i 's/^render =.*/render = ground/' /config/scripts/osd
            msposd_rockchip --osd --ahi 0 --matrix 11 -v -r 5 --master 0.0.0.0:14551 &
        fi
        ;;
    "set gs system resolution"*)
        sed -i "s/^mode =.*/mode = $5/" /config/scripts/screen-mode
        ;;
    "set gs system rec_fps"*)
        sed -i "s/^fps =.*/fps = $5/" /config/scripts/rec-fps
        ;;

    "get gs wifi hotspot")
        nmcli connection show --active | grep -q "Hotspot" && echo 1 || echo 0
        ;;
    "get gs wifi wlan")
        connection=$(nmcli -t connection show --active | grep wlan0 | cut -d : -f1)
        [ -z "${connection}" ] && echo 0 || echo 1
        ;;
    "get gs wifi ssid")
        if [ -d /sys/class/net/wlan0 ]; then
            nmcli -t connection show --active | grep wlan0 | cut -d : -f1 | tr -d '\n'
        else
            echo -n ""
        fi
        ;;
    "get gs wifi password")
        if [ -d /sys/class/net/wlan0 ]; then
            connection=$(nmcli -t connection show --active | grep wlan0 | cut -d : -f1)
            nmcli -t connection show $connection --show-secrets | grep 802-11-wireless-security.psk: | cut -d : -f2 | tr -d '\n'
        else
            echo -n ""
        fi
        ;;
    "set gs wifi wlan"*)
        [ ! -d /sys/class/net/wlan0 ] && exit 0 # we have no wifi
        if [ "$5" = "on" ]
        then
            # Check if connection already exists
            if nmcli connection show | grep -q "$6"; then
                echo "$6 connection exists. Starting it..."
                nmcli con up "$6"
            else
                echo "Creating new "$6" connection..."
                nmcli device wifi connect "$6" password "$7"
                echo "Starting Wlan..."
                nmcli con up "$6"
            fi
        else
            nmcli con down "$6"
        fi
        ;;
    "set gs wifi hotspot"*)
        [ ! -d /sys/class/net/wlan0 ] && exit 0 # we have no wifi
        if [ "$5" = "on" ]
        then
            # Check if connection already exists
            if nmcli connection show | grep -q "Hotspot"; then
                echo "Hotspot connection exists. Starting it..."
                nmcli con up Hotspot
            else
                echo "Creating new Hotspot connection..."
                nmcli con add type wifi ifname wlan0 con-name Hotspot autoconnect no ssid "OpenIPC GS"
                nmcli con modify Hotspot 802-11-wireless.mode ap 802-11-wireless.band bg ipv4.method shared
                nmcli con modify Hotspot wifi-sec.key-mgmt wpa-psk
                nmcli con modify Hotspot wifi-sec.psk "openipcgs"
                nmcli con modify Hotspot ipv4.addresses 192.168.4.1/24
                echo "Starting Hotspot..."
                nmcli con up Hotspot
            fi
        else
            nmcli con down Hotspot
        fi
        ;;

    "get gs wfbng adaptivelink")
        systemctl is-active --quiet alink_gs.service && echo 1 || echo 0
        ;;
    "get gs wfbng gs_channel")
        channel=$(grep wifi_channel /etc/wifibroadcast.cfg | cut -d ' ' -f 3)
        iw list | grep "\[$channel\]" | tr -d '[]' | awk '{print $4 " (" $2 " " $3 ")"}' | sort -n | uniq | tr -d '\n'
        ;;
    "get gs wfbng bandwidth")
        grep ^bandwidth /etc/wifibroadcast.cfg | cut -d ' ' -f 3| tr -d '\n'
        ;;

    "set gs wfbng adaptivelink"*)
        if [ "$5" = "on" ]
        then
            systemctl start alink_gs.service
            systemctl enable alink_gs.service
        else
            systemctl stop alink_gs.service
            systemctl disable alink_gs.service
        fi
        ;;
    "set gs wfbng gs_channel"*)
        channel=$(echo $5 | awk '{print $1}')
        $SSH 'sed -i "s/^channel=.*/channel='$channel'/" /etc/wfb.conf'
        $SSH "(wifibroadcast stop ;  wifibroadcast start) >/dev/null 2>&1 &"
        sed -i "s/^wifi_channel =.*/wifi_channel = $channel/" /etc/wifibroadcast.cfg
        systemctl restart wifibroadcast.service
        ;;
    "set gs wfbng bandwidth"*)
        sed -i "s/^bandwidth = .*/bandwidth = $5/" /etc/wifibroadcast.cfg
        systemctl restart wifibroadcast.service
        ;;
    "get gs main Channel")
        gsmenu.sh get gs wfbng gs_channel
        ;;
    "get gs main HDMI-OUT")
        gsmenu.sh get gs system resolution
        ;;
    "get gs main Version")
        grep PRETTY_NAME= /etc/os-release | cut -d \" -f2 | tr -d '\n'
        ;;
    "get gs main Disk")
        read -r size avail pcent <<< $(df -h / | awk 'NR==2 {print $2, $4, $5}')
        echo -e "\n   Size: $size\n   Available: $avail\n   Pct: $pcent\c"
        ;;
    "get gs main WFB_NICS")
        grep ^WFB_NICS /etc/default/wifibroadcast | cut -d \" -f 2| tr -d '\n'
        ;;
    "search channel")
        echo "Not implmented"
        echo "Not implmented" >&2
        exit 1
        ;;
    *)
        echo "Unknown $@"
        exit 1
        ;;
esac
