/**
 * Client for WFB-ng stats API
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
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"

#include "wfbcli.hpp"
extern "C" {
#include "osd.h"
}

using json = nlohmann::json;

#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 10 * 1024

int wfb_thread_signal = 0;

int process_rx(json packet) {
	void *batch = osd_batch_init(2);
	auto id = packet.at("id").template get<std::string>();
	osd_tag tags[2];
	strcpy(tags[0].key, "id");
	strcpy(tags[0].val, id.c_str());

	json packets = packet.at("packets");
	for (auto &[key, val] : packets.items()) {
		auto total = val.at(1).template get<uint32_t>();
		osd_add_uint_fact(batch, (std::string("wfbcli.rx.packets.") + key).c_str(), tags, 1, total);
	}
	json rx_ant_stats = packet.at("rx_ant_stats");
	for (auto &obj : rx_ant_stats) {
		auto ant_id = obj.at("ant").template get<uint>();
		strcpy(tags[1].key, "ant_id");
		strncpy(tags[1].val, std::to_string(ant_id).c_str(), sizeof(tags[1].val));

		osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.freq", tags, 2,
						  obj.at("freq").template get<uint>());
		osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.mcs", tags, 2,
						  obj.at("mcs").template get<uint>());
		osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.bw", tags, 2,
						  obj.at("bw").template get<uint>());

		osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.pkt_recv", tags, 2,
						  obj.at("pkt_recv").template get<uint>());
		osd_add_int_fact(batch, "wfbcli.rx.ant_stats.rssi_avg", tags, 2,
						 obj.at("rssi_avg").template get<int>());
		osd_add_double_fact(batch, "wfbcli.rx.ant_stats.snr_avg", tags, 2,
							obj.at("snr_avg").template get<double>());
	}
	//json session = packets.at("session");
	osd_publish_batch(batch);
	return 0;
}

int process_tx(json packet) {
	void *batch = osd_batch_init(2);
	auto id = packet.at("id").template get<std::string>();
	osd_tag tags[2];
	strcpy(tags[0].key, "id");
	strcpy(tags[0].val, id.c_str());

	json packets = packet.at("packets");
	for (auto &[key, val] : packets.items()) {
		auto total = val.at(1).template get<uint32_t>();
		osd_add_uint_fact(batch, (std::string("wfbcli.tx.packets.") + key).c_str(), tags, 1, total);
	}
	json tx_ant_stats = packet.at("tx_ant_stats");
	for (auto &obj : tx_ant_stats) {
		auto ant_id = obj.at("ant").template get<uint>();
		strcpy(tags[1].key, "ant_id");
		strncpy(tags[1].val, std::to_string(ant_id).c_str(), sizeof(tags[1].val));

		osd_add_uint_fact(batch, "wfbcli.tx.ant_stats.pkt_sent", tags, 2,
						  obj.at("pkt_sent").template get<uint>());
		osd_add_uint_fact(batch, "wfbcli.tx.ant_stats.pkt_drop", tags, 2,
						  obj.at("pkt_drop").template get<uint>());
		osd_add_uint_fact(batch, "wfbcli.tx.ant_stats.lat_avg", tags, 2,
						  obj.at("lat_avg").template get<uint>());
	}
	//json temp = packets.at("rf_temperature");
	osd_publish_batch(batch);
	return 0;
}

int process_title(json packet) {
	auto profile = packet.at("profile").template get<std::string>();
	auto is_cluster = packet.at("is_cluster").template get<bool>();
	auto channel = packet.at("settings").at("common").at("wifi_channel").template get<ulong>();
	auto version = packet.at("settings").at("common").at("version").template get<std::string>();
	
	void *batch = osd_batch_init(2);
	osd_add_str_fact(batch, "wfbcli.profile", nullptr, 0, profile.c_str());
	osd_add_str_fact(batch, "wfbcli.version", nullptr, 0, version.c_str());
	osd_add_bool_fact(batch, "wfbcli.is_cluster", nullptr, 0, is_cluster);
	osd_add_uint_fact(batch, "wfbcli.wifi_channel", nullptr, 0, channel);
	osd_publish_batch(batch);
	return 0;
}

int process_packet(json packet) {
	auto type = packet.at("type").template get<std::string>();
	if (type == "rx") {
		process_rx(packet);
	} else if (type == "tx") {
		process_tx(packet);
	} else if (type == "settings") {
		process_title(packet);
	} else {
		SPDLOG_ERROR("Unknown wfbcli packet type {}", type);
		return -1;
	}
	return 0;
}

// Code below is partially generated by chatgpt

void handle_server_connection(int sock) {
	std::string partial_data; // To accumulate incomplete data across reads
	char buffer[BUFFER_SIZE] = {0};

	while (!wfb_thread_signal) {
		ssize_t bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
		if (bytes_read <= 0) {
			SPDLOG_ERROR("Server disconnected or error occurred");
			close(sock);
			return;
		}

		buffer[bytes_read] = '\0'; // Null-terminate the received data
		partial_data += buffer;	  // Append to the accumulated data

		// Process each complete line
		size_t newline_pos;
		while ((newline_pos = partial_data.find('\n')) != std::string::npos) {
			std::string line = partial_data.substr(0, newline_pos);
			partial_data.erase(0, newline_pos + 1); // Remove the processed line

			try {
				// Parse the line as JSON
				nlohmann::json parsed_json = nlohmann::json::parse(line);
				process_packet(parsed_json);
			} catch (const nlohmann::json::parse_error &e) {
				SPDLOG_ERROR("Failed to parse JSON: {}", e.what());
			}
		}
	}
}

// Function to reconnect to the server in a loop with retries
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
