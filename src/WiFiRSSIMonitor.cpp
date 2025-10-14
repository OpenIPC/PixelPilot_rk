#include "WiFiRSSIMonitor.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <regex>
#include <filesystem>
#include "spdlog/spdlog.h"

extern "C" {
#include "osd.h"
}

namespace fs = std::filesystem;

// Constructor implementation
WiFiRSSIMonitor::WiFiRSSIMonitor() : base_path_("/proc/net/rtl88x2eu") {}

// WiFiStats constructor implementation
WiFiRSSIMonitor::WiFiStats::WiFiStats() : rssi_a(0), rssi_b(0), rssi_min(0), rssi_percent(0), is_linked(false) {}

void WiFiRSSIMonitor::run() {

    if (!fs::exists(base_path_)) {
        spdlog::error("RTL88x2eu proc path not found: {}", base_path_);
        return;
    }
    
    // Initialize batch - estimate 5 facts per interface
    void* batch = osd_batch_init(20);
    
    // Find all WiFi interfaces and collect their stats
    for (const auto& entry : fs::directory_iterator(base_path_)) {
        if (!entry.is_directory()) continue;
        
        std::string interface_name = entry.path().filename();
        std::string debug_file = entry.path() / "trx_info_debug";
        
        if (fs::exists(debug_file)) {
            WiFiStats stats = parse_interface_stats(debug_file);
            add_interface_stats_to_batch(batch, interface_name, stats);
        }
    }
    
    // Publish all collected facts
    osd_publish_batch(batch);
}

WiFiRSSIMonitor::WiFiStats WiFiRSSIMonitor::parse_interface_stats(const std::string& file_path) {

    WiFiStats stats;
    std::ifstream file(file_path);
    
    if (!file.is_open()) {
        return stats;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Parse RSSI A and B
        if (line.find("rssi_a =") != std::string::npos) {
            std::regex rssi_ab_regex(R"(rssi_a\s*=\s*(\d+)\(%\),\s*rssi_b\s*=\s*(\d+)\(%\))");
            std::smatch match;
            if (std::regex_search(line, match, rssi_ab_regex) && match.size() == 3) {
                stats.rssi_a = std::stoi(match[1]);
                stats.rssi_b = std::stoi(match[2]);
            }
        }
        // Parse RSSI percentage
        else if (line.find("rssi :") != std::string::npos) {
            std::regex rssi_regex(R"(rssi\s*:\s*(\d+)\s*\(\%\))");
            std::smatch match;
            if (std::regex_search(line, match, rssi_regex) && match.size() > 1) {
                stats.rssi_percent = std::stoi(match[1]);
            }
        }
        else if (line.find("is_linked =") != std::string::npos) {
            std::regex linked_regex(R"(is_linked\s*=\s*(\d+))");
            std::smatch match;
            if (std::regex_search(line, match, linked_regex) && match.size() > 1) {
                stats.is_linked = (std::stoi(match[1]) == 1);
            }
        }
    }
    
    file.close();
    return stats;
}

void WiFiRSSIMonitor::add_interface_stats_to_batch(void* batch, const std::string& interface_name, const WiFiStats& stats) {
    if (!stats.is_linked) {
        return; // Don't publish stats for disconnected interfaces
    }
    
    // Prepare common tags
    osd_tag interface_tag;
    strncpy(interface_tag.key, "interface", TAG_MAX_LEN - 1);
    strncpy(interface_tag.val, interface_name.c_str(), TAG_MAX_LEN - 1);
    interface_tag.key[TAG_MAX_LEN - 1] = '\0';
    interface_tag.val[TAG_MAX_LEN - 1] = '\0';
    
    // Publish RSSI A
    add_rssi_fact_to_batch(batch, "rssi_a", stats.rssi_a, &interface_tag);
    
    // Publish RSSI B  
    add_rssi_fact_to_batch(batch, "rssi_b", stats.rssi_b, &interface_tag);
    
    // Publish RSSI Overall Percentage
    add_rssi_fact_to_batch(batch, "rssi_percent", stats.rssi_percent, &interface_tag);
    
    // Publish connection status
    add_rssi_fact_to_batch(batch, "connected", 1, &interface_tag);
}

void WiFiRSSIMonitor::add_rssi_fact_to_batch(void* batch, const std::string& rssi_type, int value, osd_tag* interface_tag) {
    osd_tag tags[2];
    
    // Copy interface tag
    memcpy(&tags[0], interface_tag, sizeof(osd_tag));
    
    // Add type tag
    strncpy(tags[1].key, "type", TAG_MAX_LEN - 1);
    strncpy(tags[1].val, rssi_type.c_str(), TAG_MAX_LEN - 1);
    tags[1].key[TAG_MAX_LEN - 1] = '\0';
    tags[1].val[TAG_MAX_LEN - 1] = '\0';
    
    // Add fact to batch
    std::string fact_name = "os_mon.wifi.rssi";
    osd_add_int_fact(batch, fact_name.c_str(), tags, 2, value);
}

void WiFiRSSIMonitor::publish_reset() {
    if (!fs::exists(base_path_)) {
        spdlog::warn("RTL88x2eu proc path not found for reset: {}", base_path_);
        return;
    }
    
    // Initialize batch - estimate 5 facts per interface
    void* batch = osd_batch_init(20);
    
    // Find all WiFi interfaces and publish reset values
    for (const auto& entry : fs::directory_iterator(base_path_)) {
        if (!entry.is_directory()) continue;
        
        std::string interface_name = entry.path().filename();
        publish_interface_reset(batch, interface_name);
    }
    
    // Publish all reset facts
    osd_publish_batch(batch);
    
    spdlog::debug("Published WiFi RSSI reset values for all interfaces");
}

void WiFiRSSIMonitor::publish_interface_reset(void* batch, const std::string& interface_name) {
    // Prepare common tags
    osd_tag interface_tag;
    strncpy(interface_tag.key, "interface", TAG_MAX_LEN - 1);
    strncpy(interface_tag.val, interface_name.c_str(), TAG_MAX_LEN - 1);
    interface_tag.key[TAG_MAX_LEN - 1] = '\0';
    interface_tag.val[TAG_MAX_LEN - 1] = '\0';
    
    // Publish all RSSI values as -1 (reset/error value)
    add_rssi_fact_to_batch(batch, "rssi_a", -1, &interface_tag);
    add_rssi_fact_to_batch(batch, "rssi_b", -1, &interface_tag);
    add_rssi_fact_to_batch(batch, "rssi_min", -1, &interface_tag);
    add_rssi_fact_to_batch(batch, "rssi_percent", -1, &interface_tag);
    add_rssi_fact_to_batch(batch, "connected", 0, &interface_tag);  // 0 = disconnected
}

// C-callable function implementations
extern "C" {

void wifi_rssi_monitor_reset(void) {
    static WiFiRSSIMonitor monitor;
    monitor.publish_reset();
}

} // extern "C"