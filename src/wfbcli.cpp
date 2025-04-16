/**
 * Client for WFB-ng stats API using MessagePack
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h> // for close
#include <chrono>
#include <thread>

#include <stdint.h>
#include <sys/types.h>
#include <cstdint>
#include <iostream>
#include <msgpack.hpp>
#include "spdlog/spdlog.h"

#include "wfbcli.hpp"
extern "C" {
#include "osd.h"
}

#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 10 * 1024

int wfb_thread_signal = 0;

uint64_t gtotal_tunnel_data = 0; // global variable for easyer access in gsmenu

int process_rx(const msgpack::object& packet) {

    void *batch = osd_batch_init(2);
    osd_tag tags[2];

    // Access the map and find the required keys
    std::string id;
    msgpack::object packets;
    msgpack::object rx_ant_stats;

    for (size_t i = 0; i < packet.via.map.size; ++i) {
        std::string key = packet.via.map.ptr[i].key.as<std::string>();
        if (key == "id") {
            id = packet.via.map.ptr[i].val.as<std::string>();
            strcpy(tags[0].key, "id");
            strcpy(tags[0].val, id.c_str());
        } else if (key == "packets") {
            packets = packet.via.map.ptr[i].val;
        } else if (key == "rx_ant_stats") {
            rx_ant_stats = packet.via.map.ptr[i].val;
        }
    }

    // Process packets
    for (size_t i = 0; i < packets.via.map.size; ++i) {
		std::string key = packets.via.map.ptr[i].key.as<std::string>();
		msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
		uint32_t delta = array.ptr[0].as<uint32_t>();
		uint64_t total = array.ptr[1].as<uint64_t>();
		if (! strcmp("data",key.c_str()) && ! strcmp("tunnel rx",tags[0].val))
			gtotal_tunnel_data = total; // store total in global variable for gsmenu
		// If you need to use these values elsewhere, you can pass them to other functions
		osd_add_uint_fact(batch, (std::string("wfbcli.rx.packets.") + key + ".delta").c_str(), tags, 1, delta);
		osd_add_uint_fact(batch, (std::string("wfbcli.rx.packets.") + key + ".total").c_str(), tags, 1, total);
    }

    // Process rx_ant_stats
    for (size_t i = 0; i < rx_ant_stats.via.map.size; ++i) {
        // Extract the key (which is an array: [[frequency, mcs, bandwidth], antenna_id])
        msgpack::object_array key_array = rx_ant_stats.via.map.ptr[i].key.via.array;

        // Extract the first element of the key (which is an array: [frequency, mcs, bandwidth])
        msgpack::object_array freq_mcs_bw_array = key_array.ptr[0].via.array;

        // Extract frequency, mcs, and bandwidth from the inner array
        uint32_t frequency = freq_mcs_bw_array.ptr[0].as<uint32_t>();
        uint32_t mcs = freq_mcs_bw_array.ptr[1].as<uint32_t>();
        uint32_t bandwidth = freq_mcs_bw_array.ptr[2].as<uint32_t>();

        // Extract the second element of the key (antenna_id)
        uint64_t antenna_id = key_array.ptr[1].as<uint64_t>();

        strcpy(tags[1].key, "ant_id");
        snprintf(tags[1].val, sizeof(tags[0].val), "%u", antenna_id);

        // Extract the value (which is an array: [packets_delta, rssi_min, rssi_avg, rssi_max, snr_min, snr_avg, snr_max])
        msgpack::object_array value_array = rx_ant_stats.via.map.ptr[i].val.via.array;
        // Extract packets_delta, rssi_min, rssi_avg, rssi_max, snr_min, snr_avg, snr_max from the value
        int32_t packets_delta = value_array.ptr[0].as<int32_t>();
        int32_t rssi_min = value_array.ptr[1].as<int32_t>();
        int32_t rssi_avg = value_array.ptr[2].as<int32_t>();
        int32_t rssi_max = value_array.ptr[3].as<int32_t>();
        int32_t snr_min = value_array.ptr[4].as<int32_t>();
        int32_t snr_avg = value_array.ptr[5].as<int32_t>();
        int32_t snr_max = value_array.ptr[6].as<int32_t>();
        osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.freq", tags, 2, frequency);
        osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.mcs", tags, 2, mcs);
        osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.bw", tags, 2, bandwidth);
        osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.pkt_recv", tags, 2, packets_delta);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.rssi_min", tags, 2, rssi_min);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.rssi_avg", tags, 2, rssi_avg);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.rssi_max", tags, 2, rssi_max);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.snr_min", tags, 2, snr_min);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.snr_avg", tags, 2, snr_avg);
        osd_add_int_fact(batch, "wfbcli.rx.ant_stats.snr_max", tags, 2, snr_max);
    }

    osd_publish_batch(batch);
    return 0;
}

int process_tx(const msgpack::object& packet) {

    void *batch = osd_batch_init(2);
    osd_tag tags[2];

    // Access the map and find the required keys
    std::string id;
    msgpack::object packets;
    msgpack::object latency;
	msgpack::object rf_temperature;

    //std::cout << "Unpacked MessagePack packet: " << packet << std::endl;

    for (size_t i = 0; i < packet.via.map.size; ++i) {
        std::string key = packet.via.map.ptr[i].key.as<std::string>();
        if (key == "id") {
            id = packet.via.map.ptr[i].val.as<std::string>();
            strcpy(tags[0].key, "id");
            strcpy(tags[0].val, id.c_str());
        } else if (key == "packets") {
            packets = packet.via.map.ptr[i].val;
        } else if (key == "latency") {
            latency = packet.via.map.ptr[i].val;
        } else if (key == "rf_temperature") {
            rf_temperature = packet.via.map.ptr[i].val;
        }
    }

	for (size_t i = 0; i < packets.via.map.size; ++i) {
        std::string key = packets.via.map.ptr[i].key.as<std::string>();
        if (key == "fec_timeouts") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.fec_timeouts.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.fec_timeouts.total", tags, 1, total);			
        } else if (key == "incoming") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.incoming.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.incoming.total", tags, 1, total);	
        } else if (key == "incoming_bytes") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.incoming_bytes.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.incoming_bytes.total", tags, 1, total);
        } else if (key == "injected") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.injected.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.injected.total", tags, 1, total);
        } else if (key == "injected_bytes") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.injected_bytes.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.injected_bytes.total", tags, 1, total);
        } else if (key == "dropped") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.dropped.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.dropped.total", tags, 1, total);
        } else if (key == "truncated") {
			msgpack::object_array& array = packets.via.map.ptr[i].val.via.array;
			uint32_t delta = array.ptr[0].as<uint32_t>();
			uint64_t total = array.ptr[1].as<uint64_t>();
			osd_add_uint_fact(batch, "wfbcli.tx.packets.truncated.delta", tags, 1, delta);			
			osd_add_uint_fact(batch, "wfbcli.tx.packets.truncated.total", tags, 1, total);
        }
    }

	for (size_t i = 0; i < rf_temperature.via.map.size; ++i) {
        int antenna_id = rf_temperature.via.map.ptr[i].key.as<int>();
		int temperature = rf_temperature.via.map.ptr[i].val.as<int>();
		strcpy(tags[1].key, "ant_id");
		snprintf(tags[1].val, sizeof(tags[0].val), "%u", antenna_id);
		osd_add_uint_fact(batch, "wfbcli.rf_temperature", tags, 2, temperature);
    }	

    osd_publish_batch(batch);

    return 0;
}

int process_title(const msgpack::object& packet) {
    std::string cli_title;
    bool is_cluster = false;
    uint32_t temp_overheat_warning = 0;

    // Access the map and find the required keys
    for (size_t i = 0; i < packet.via.map.size; ++i) {
        std::string key = packet.via.map.ptr[i].key.as<std::string>();
        if (key == "cli_title") {
            cli_title = packet.via.map.ptr[i].val.as<std::string>();
        } else if (key == "is_cluster") {
            is_cluster = packet.via.map.ptr[i].val.as<bool>();
        } else if (key == "temp_overheat_warning") {
            temp_overheat_warning = packet.via.map.ptr[i].val.as<int>();
        }
    }

    void *batch = osd_batch_init(2);
    osd_add_str_fact(batch, "wfbcli.cli_title", nullptr, 0, cli_title.c_str());
    osd_add_bool_fact(batch, "wfbcli.is_cluster", nullptr, 0, is_cluster);
    osd_add_uint_fact(batch, "wfbcli.temp_overheat_warning", nullptr, 0, temp_overheat_warning);
    osd_publish_batch(batch);
    return 0;
}

int process_packet(const msgpack::object& packet) {
    std::string type;

    // Find the "type" key in the map
    for (size_t i = 0; i < packet.via.map.size; ++i) {
        std::string key = packet.via.map.ptr[i].key.as<std::string>();
        if (key == "type") {
            type = packet.via.map.ptr[i].val.as<std::string>();
            break;
        }
    }

    if (type == "rx") {
        process_rx(packet);
    } else if (type == "tx") {
        process_tx(packet);
    } else if (type == "cli_title") {
        process_title(packet);
    } else {
        SPDLOG_ERROR("Unknown wfbcli packet type {}", type);
        return -1;
    }
    return 0;
}

void handle_server_connection(int sock) {
	std::vector<char> buffer(BUFFER_SIZE);
	while (!wfb_thread_signal) {
		// Read the length prefix (4 bytes)
		uint32_t msg_length;
		ssize_t bytes_read = recv(sock, &msg_length, sizeof(msg_length), 0);
		if (bytes_read <= 0) {
			SPDLOG_ERROR("Server disconnected or error occurred");
			close(sock);
			return;
		}

		msg_length = ntohl(msg_length); // Convert from network byte order

		// Read the actual MessagePack data
		std::vector<char> data(msg_length);
		size_t total_read = 0;
		while (total_read < msg_length) {
			bytes_read = recv(sock, data.data() + total_read, msg_length - total_read, 0);
			if (bytes_read <= 0) {
				SPDLOG_ERROR("Incomplete data, connection closed.");
				close(sock);
				return;
			}
			total_read += bytes_read;
		}

		// Unpack the MessagePack data
		try {
			msgpack::object_handle oh = msgpack::unpack(data.data(), data.size());
			msgpack::object obj = oh.get();
		    // std::cout << "Unpacked MessagePack data: " << obj << std::endl;
			process_packet(obj);
		} catch (const msgpack::unpack_error& e) {
			SPDLOG_ERROR("Failed to unpack data: {}", e.what());
		}
	}
}

int reconnect_to_server(int port) {
	while (!wfb_thread_signal) {
		SPDLOG_DEBUG("Attempting to connect to WFB API server...");

		int sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			perror("Socket creation failed");
		} else {
			struct sockaddr_in server_address;
			server_address.sin_family = AF_INET;
			server_address.sin_port = htons(port);

			if (inet_pton(AF_INET, SERVER_IP, &server_address.sin_addr) > 0) {
				if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) == 0) {
					SPDLOG_DEBUG("Successfully connected to WFB API server.");
					return sock;
				} else {
					SPDLOG_ERROR("Connection failed");
				}
			} else {
				SPDLOG_ERROR("Invalid address/Address not supported");
			}

			close(sock); // Clean up the socket if connection fails
		}

		SPDLOG_WARN("Reconnection failed. Retrying in 1 second");
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return -1;
}

void *__WFB_CLI_THREAD__(void *param) {
	wfb_thread_params *p = (wfb_thread_params *)param;
	pthread_setname_np(pthread_self(), "__WFB_CLI");

	while (!wfb_thread_signal) {
		int sock = reconnect_to_server(p->port);
		handle_server_connection(sock);
		// If we return from handle_server_connection, the server is disconnected
	}

	spdlog::info("WFB_CLI thread done.");
	return nullptr;
}
