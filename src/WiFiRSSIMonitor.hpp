#ifndef WIFI_RSSI_MONITOR_H
#define WIFI_RSSI_MONITOR_H

#include <string>

extern "C" {
#include "osd.h"
}

class WiFiRSSIMonitor {
public:
    WiFiRSSIMonitor();
    void run();
    void publish_reset();

private:
    struct WiFiStats {
        int rssi_a;
        int rssi_b;
        int rssi_min;
        int rssi_percent;
        bool is_linked;
        
        WiFiStats();
    };
    
    std::string base_path_;
    
    WiFiStats parse_interface_stats(const std::string& file_path);
    void add_interface_stats_to_batch(void* batch, const std::string& interface_name, const WiFiStats& stats);
    void add_rssi_fact_to_batch(void* batch, const std::string& rssi_type, int value, osd_tag* interface_tag);
    void publish_interface_reset(void* batch, const std::string& interface_name);

};

#endif