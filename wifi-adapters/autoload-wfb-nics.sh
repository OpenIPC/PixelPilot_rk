#!/bin/bash

# Define the MAC address prefix to exclude (Internal Wifi adapter)
EXCLUDE_PREFIX=("98:03:cf" "38:7a:cc")

# Initialize log file and type variables
LOG_FILE=""
TYPE=""

# Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
    --log)
        LOG_FILE="$2"
        shift
        ;;
    --type)
        TYPE="$2"
        shift
        ;;
    *)
        echo "Unknown parameter passed: $1"
        exit 1
        ;;
    esac
    shift
done

# Function to check if an interface's MAC address matches the exclude prefix
matches_exclude_prefix() {
    local mac_address=$1
    for prefix in "${EXCLUDE_PREFIX[@]}"; do
        if [[ $mac_address == $prefix* ]]; then
            return 0 # Match found
        fi
    done
    return 1 # No match
}

# Initialize an empty string to store matching interface names
matching_interfaces=""

# Variable to track if we have renamed an interface to wlan0
renamed_to_wlan0=false

# Check if wlan0 already exists
if ip link show wlan0 &>/dev/null; then
    renamed_to_wlan0=true
fi

# List all network interfaces and filter only wireless ones
for iface in /sys/class/net/*; do
    iface_name=$(basename $iface)
    # Check if the interface is wireless
    if [ -d "/sys/class/net/$iface_name/wireless" ]; then
        # Get the MAC address of the interface
        mac_address=$(cat /sys/class/net/$iface_name/address)
        # Check if the MAC address matches the exclude prefix
        if matches_exclude_prefix $mac_address; then
            # Rename the first excluded interface to wlan0 if wlan0 does not already exist
            if ! $renamed_to_wlan0; then
                ip link set $iface_name down
                ip link set $iface_name name wlan0
                ip link set wlan0 up
                renamed_to_wlan0=true
                if [[ -n "$LOG_FILE" ]]; then
                    echo "$(date) [$TYPE]: Internal Adapter renamed to wlan0" >>"$LOG_FILE"
                fi
            fi
        else
            # Append the interface name to the list
            matching_interfaces+="$iface_name "
        fi
    fi
done

# Output the list of matching interfaces as a space-separated string
new_wfb_nics="WFB_NICS=\"${matching_interfaces% }\""

# Read the current contents of /etc/default/wifibroadcast
current_wfb_nics=$(cat /etc/default/wifibroadcast 2>/dev/null)

# Compare and update if different
if [[ "$current_wfb_nics" != "$new_wfb_nics" ]]; then
    echo "$new_wfb_nics" >/etc/default/wifibroadcast
    systemctl restart wifibroadcast &
    # Log to file if --log argument was provided
    if [[ -n "$LOG_FILE" ]]; then
        echo "$(date) [$TYPE]: Network interface $ACTION $INTERFACE" >>"$LOG_FILE"
        echo "$(date) [$TYPE]: $new_wfb_nics" >>"$LOG_FILE"
        echo "$(date) [$TYPE]: Restarting Wifibroadcast service" >>"$LOG_FILE"
    fi
fi