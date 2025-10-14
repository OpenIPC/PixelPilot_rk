#include "gs_connection_checker.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../osd.h"

extern uint64_t gtotal_tunnel_data; // global variable for easyer access in gsmenu
                                    // we missue the variable here for easyer integration
                                    // this is also used from wfb cli thread for incomming traffic

#define PROC_NET_DEV "/proc/net/dev"
#define MAX_LINE_LENGTH 256
#define INTERFACE_PREFIX "wlx"

unsigned long long get_rx_bytes(const char *interface_name) {
    FILE *file = fopen(PROC_NET_DEV, "r");
    if (!file) {
        perror("Error opening /proc/net/dev");
        return 0;
    }

    char line[MAX_LINE_LENGTH];
    unsigned long long rx_bytes = 0;
    int found = 0;

    // Skip the first two header lines
    fgets(line, sizeof(line), file);
    fgets(line, sizeof(line), file);

    while (fgets(line, sizeof(line), file)) {
        char iface[64];
        unsigned long long rb, rp, re, rd, rr, rf, rm, rmc;
        
        // Parse the line: interface_name: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame rx_compressed rx_multicast ...
        if (sscanf(line, " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu",
                  iface, &rb, &rp, &re, &rd, &rf, &rr, &rm, &rmc) >= 9) {
            
            // Remove trailing colon if present
            if (iface[strlen(iface)-1] == ':') {
                iface[strlen(iface)-1] = '\0';
            }
            
            if (strncmp(iface, interface_name, strlen(interface_name)) == 0) {
                rx_bytes += rb;
                found = 1;
                // printf("Found interface %s: %llu bytes\n", iface, rb);
            }
        }
    }

    fclose(file);
    
    if (!found) {
        printf("No interfaces found with prefix '%s'\n", interface_name);
    }
    
    return rx_bytes;
}

// Update network status
void update_network_status() {

    gtotal_tunnel_data = get_rx_bytes("wlx");

#ifndef USE_SIMULATOR
    // ugly hack to hide osd bars
    // ToDo: come back and use VRX side WLAN stats to populate rssi or lq
    osd_tag tags[2];
    strcpy(tags[0].key, "id");
    strcpy(tags[0].val, "video rx");
    osd_publish_int_fact("wfbcli.rx.packets.all.total", tags, 1,2);  // out of bound for osd config values

    strcpy(tags[1].key, "ant_id");
    for(int i=0;i< 10;i++) {
        sprintf(tags[1].val, "%d", i * 256);
        osd_publish_int_fact("wfbcli.rx.ant_stats.rssi_avg", tags, 2,2);  // out of bound for osd config values
        sprintf(tags[1].val, "%d", i * 256 + 1);
        osd_publish_int_fact("wfbcli.rx.ant_stats.rssi_avg", tags, 2,2);  // out of bound for osd config values
    }
#endif
}
