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
    if (!fgets(line, sizeof(line), file) || !fgets(line, sizeof(line), file)) {
        fclose(file);
        return 0;
    }

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
    // Refresh the tunnel-byte counter used for apfpv drone detection (menu.c
    // drone_detect_timer). In WFB mode the wfbcli thread fills gtotal_tunnel_data;
    // in apfpv nothing else does, so we read it from the wlx interface here.
    //
    // Stale facts from the previous RX mode are cleared by osd_flush_facts() on the
    // mode switch (apply_rx_mode), so no per-fact bar-hiding is needed here anymore.
    gtotal_tunnel_data = get_rx_bytes("wlx");
}
