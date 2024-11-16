

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h> // for close

#include <stdint.h>
#include <sys/types.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"

#include "wfbcli.hpp"
extern "C" {
#include "osd.h"
#include <msgpack.h>
}

using json = nlohmann::json;

int wfb_thread_signal = 0;

// Helper function to receive `len` bytes from the socket
bool recv_all(int socket, char* buffer, size_t len) {
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t bytes_received = recv(socket, buffer + total_received, len - total_received, 0);
        if (bytes_received <= 0) {
            return false; // Connection closed or error
        }
        total_received += bytes_received;
    }
    return true;
}

/*
{'type': 'rx', 'timestamp': 1728342615.1328719, 'id': 'video rx', 'tx_wlan': 0,
 'packets': {'all': (661, 3967),
             'all_bytes': (783901, 4704911),
			 'dec_ok': (661, 3967),
			 'fec_rec': (28, 271),
			 'lost': (0, 32),
			 'dec_err': (0, 0),
			 'bad': (0, 0),
			 'out': (466, 2904),
			 'out_bytes': (528304, 3286747)},
 'rx_ant_stats': {
   ((5805, 1, 20), 1): (661, -38, -37, -37, 8, 31, 37),
   ((5805, 1, 20), 0): (661, -42, -40, -40, 5, 30, 35)},
 'session': {'fec_type': 'VDM_RS',
             'fec_k': 8,
			 'fec_n': 12,
			 'epoch': 0}}
{'type': 'rx', 'timestamp': 1728342614.2870364, 'id': 'tunnel rx', 'tx_wlan': 0,
'packets': {'all': (0, 0),
            'all_bytes': (0, 0),
			'dec_ok': (0, 0),
			'fec_rec': (0, 0),
			'lost': (0, 0),
			'dec_err': (0, 0),
			'bad': (0, 0),
			'out': (0, 0),
			'out_bytes': (0, 0)},
'rx_ant_stats': {},
'session': None}
 */

/*
{'type': 'rx', 'timestamp': 1728342615.1976957, 'id': 'mavlink rx', 'tx_wlan': 0,
 'packets': {'all': (24, 104),
             'all_bytes': (11902, 51476),
			 'dec_ok': (24, 104),
			 'fec_rec': (0, 3),
			 'lost': (0, 0),
			 'dec_err': (0, 0),
			 'bad': (0, 0),
			 'out': (12, 55),
			 'out_bytes': (5820, 26798)},
 'rx_ant_stats': {((5805, 1, 20), 1): (24, -38, -38, -38, 20, 28, 36),
                  ((5805, 1, 20), 0): (24, -42, -40, -40, 18, 27, 34)},
 'session': {'fec_type': 'VDM_RS',
             'fec_k': 1,
			 'fec_n': 2,
			 'epoch': 0}}
 */
int process_rx(json packet) {
	void *batch = osd_batch_init(2);
	auto id = packet.at("id").template get<std::string>();
	json packets = packet.at("packets");
	osd_tag tags[3];
	strcpy(tags[0].key, "id");
	strcpy(tags[0].val, id.c_str());
	for (auto &[key, val] : packets.items()) {
		auto total = val.at(1).template get<uint32_t>();
		osd_add_uint_fact(batch, (std::string("wfbcli.rx.packets.") + key).c_str(), tags, 1, total);
	}
	//json rx_ant_stats = packets.at("rx_ant_stats");
	// for (auto &[key, val] : rx_ant_stats) { ... }
	// json session = packets.at("session");
	osd_publish_batch(batch);
	return 0;
}

/*
{'type': 'tx', 'timestamp': 1728342616.1666563, 'id': 'tunnel tx',
 'packets': {'fec_timeouts': (0, 0),
             'incoming': (2, 1055),
			 'incoming_bytes': (0, 261),
			 'injected': (5, 2576),
			 'injected_bytes': (200, 100610),
			 'dropped': (0, 0),
			 'truncated': (0, 0)},
 'latency': {255: (5, 0, 28, 74, 134)},
 'rf_temperature': {0: 42, 1: 42}}
 */
int process_tx(json packet) {
	return 0;
}

/*
{'type': 'cli_title',
 'cli_title': 'WFB-ng_24.9.26.72534 @gs wlx782288192c76 [default]',
 'is_cluster': False}
 */
int process_title(json packet) {
	auto title = packet.at("cli_title").template get<std::string>();
	auto is_cluster = packet.at("is_cluster").template get<bool>();
	
	void *batch = osd_batch_init(2);
	osd_add_str_fact(batch, "wfbcli.cli_title", nullptr, 0, title.c_str());
	osd_add_bool_fact(batch, "wfbcli.is_cluster", nullptr, 0, is_cluster);
	osd_publish_batch(batch);
	return 0;
}

int process_packet(json packet) {
	auto type = packet.at("type").template get<std::string>();
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

size_t msgpack_object_print(const msgpack_object o)
{
    size_t ret;

    switch (o.type) {
    case MSGPACK_OBJECT_NIL:
		SPDLOG_DEBUG("nil");
        break;

    case MSGPACK_OBJECT_BOOLEAN:
		SPDLOG_DEBUG("Bool {}", (o.via.boolean ? "true" : "false"));
        break;

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
		SPDLOG_DEBUG("Pos int {}", o.via.u64);
        break;

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
		SPDLOG_DEBUG("Neg int {}", o.via.i64);
        break;

    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
		SPDLOG_DEBUG("Float {}", o.via.f64);
        break;

    case MSGPACK_OBJECT_STR:
		SPDLOG_DEBUG("Str {}, {}", o.via.str.size, o.via.str.ptr);
        break;

    case MSGPACK_OBJECT_BIN:
        /*if (bytes_contain_zero(&o.via.bin)) {
            SPDLOG_DEBUG("the value contains zero");
            return -1;
			}*/
		SPDLOG_DEBUG("Bin {} {}", (int)o.via.bin.size, o.via.bin.ptr);
        break;

    case MSGPACK_OBJECT_EXT:
        SPDLOG_DEBUG("not support type: MSGPACK_OBJECT_EXT");
        return -1;

    case MSGPACK_OBJECT_ARRAY:
        SPDLOG_DEBUG("ARRAY");
        /*PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, "[");
        if (o.via.array.size != 0) {
            msgpack_object *p = o.via.array.ptr;
            msgpack_object *const pend = o.via.array.ptr + o.via.array.size;
            PRINT_JSONSTR_CALL(ret, msgpack_object_print_jsonstr, aux_buffer, aux_buffer_size, *p);
            ++p;
            for (; p < pend; ++p) {
                PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, ",");
                PRINT_JSONSTR_CALL(ret, msgpack_object_print_jsonstr, aux_buffer, aux_buffer_size, *p);
            }
        }
        PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, "]");*/
        break;

    case MSGPACK_OBJECT_MAP:
		SPDLOG_DEBUG("map");
        /*PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, "{");
        if (o.via.map.size != 0) {
            msgpack_object_kv *p = o.via.map.ptr;
            msgpack_object_kv *const pend = o.via.map.ptr + o.via.map.size;

            for (; p < pend; ++p) {
                if (p->key.type != MSGPACK_OBJECT_STR) {
                    DEBUG("the key of in a map must be string.\n");
                    return -1;
                }
                if (p != o.via.map.ptr) {
                    PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, ",");
                }
                PRINT_JSONSTR_CALL(ret, msgpack_object_print_jsonstr, aux_buffer, aux_buffer_size, p->key);
                PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, ":");
                PRINT_JSONSTR_CALL(ret, msgpack_object_print_jsonstr, aux_buffer, aux_buffer_size, p->val);
            }
        }
        PRINT_JSONSTR_CALL(ret, snprintf, aux_buffer, aux_buffer_size, "}");
        break;*/

    default:
        SPDLOG_DEBUG("unknown type");
        return -1;
    }

    return 0;
}

void parse_msg(char *buf, uint size) {
        msgpack_zone z;
        msgpack_object obj;

        msgpack_zone_init(&z, size);
        msgpack_unpack(buf, size, NULL, &z, &obj);
		msgpack_object_print(obj);
		msgpack_zone_destroy(&z);
}

void *__WFB_CLI_THREAD__(void *param) {
	wfb_thread_params *p = (wfb_thread_params *)param;
	pthread_setname_np(pthread_self(), "__WFB_CLI");
	int client_socket;
    struct sockaddr_in server_address;
    uint32_t size;
	char buf[128 * 1024];

    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		spdlog::error("Socket creation failed {}", strerror(errno));
        return 0;
    }

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(p->port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
		spdlog::error("Invalid address or Address not supported {}", strerror(errno));
        close(client_socket);
        return 0;
    }

    // Connect to the server
    if (connect(client_socket, reinterpret_cast<struct sockaddr*>(&server_address), sizeof(server_address)) < 0) {
		spdlog::error("Connection failed", strerror(errno));
        close(client_socket);
        return 0;
    }

	try {
		while (!wfb_thread_signal) {
			if (!recv_all(client_socket, reinterpret_cast<char*>(&size), sizeof(size))) {
				throw std::runtime_error("Failed to read size prefix or connection closed");
			}

			// Convert the size from network byte order to host byte order
			size = ntohl(size);

			if (size > sizeof(buf)) {
				throw std::runtime_error("WFB stats packet is too large");
			}
			
			if (!recv_all(client_socket, buf, size)) {
				throw std::runtime_error("Failed to read size prefix or connection closed");
			}
			parse_msg(buf, size);
			json as_json = json::from_msgpack(buf, buf + size, false, true);
			process_packet(as_json);
		}
	} catch (const std::exception &e) {
		spdlog::error("WFB stats error: {}", e.what());
    }
    close(client_socket);
	spdlog::info("WFB_CLI thread done.");
	return nullptr;
}
