#include "json_stats.h"
#include <chrono>
#include <thread>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"
#include "osd.h"

using json = nlohmann::json;

int json_stats_thread_signal = 0;

void *__JSON_STATS_THREAD__(void *param) {
    const char* SERVER_IP = "127.0.0.1";
    const int SERVER_PORT = 8103;
    char buffer[4096];
    std::string accumulated_data;
    
    spdlog::info("Starting JSON stats thread, connecting to {}:{}", SERVER_IP, SERVER_PORT);

    while (!json_stats_thread_signal) {
        // Create socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            spdlog::error("Socket creation error");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Set socket timeout
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        // Configure server address
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

        // Connect to server
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            spdlog::debug("Connection attempt failed, will retry");
            close(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Read data
        while (!json_stats_thread_signal) {
            int bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read <= 0) {
                break; // Connection closed or error
            }

            buffer[bytes_read] = '\0';
            accumulated_data += buffer;

            // Process complete lines
            size_t pos;
            while ((pos = accumulated_data.find('\n')) != std::string::npos) {
                std::string line = accumulated_data.substr(0, pos);
                accumulated_data.erase(0, pos + 1);

                if (line.empty()) continue;

                try {
                    auto obj = json::parse(line);
                    if (obj["type"] == "rx" && obj.contains("rx_ant_stats")) {
                        // Create a batch for multiple facts
                        void* batch = osd_batch_init(obj["rx_ant_stats"].size());
                        
                        for (const auto& ant_stat : obj["rx_ant_stats"]) {
                            if (ant_stat.contains("rssi_avg")) {
                                // Create tags for the antenna
                                osd_tag tags[1];
                                snprintf(tags[0].key, TAG_MAX_LEN, "ant");
                                snprintf(tags[0].val, TAG_MAX_LEN, "%d", ant_stat["ant"].get<int>());
                                
                                // Add RSSI fact to batch
                                osd_add_int_fact(batch, "radio.rssi_avg", tags, 1, 
                                               ant_stat["rssi_avg"].get<int>());

                                spdlog::debug("Got RSSI avg {} for antenna {}", 
                                            ant_stat["rssi_avg"].get<int>(),
                                            ant_stat["ant"].get<int>());
                            }
                        }
                        
                        // Publish all facts at once
                        osd_publish_batch(batch);
                    }
                } catch (const json::parse_error& e) {
                    spdlog::debug("Failed to parse JSON line: {}", e.what());
                }
            }
        }

        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return nullptr;
}
