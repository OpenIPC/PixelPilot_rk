//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#include "gstrtpreceiver.h"
#include "gst/gstparse.h"
#include "gst/gstpipeline.h"
#include "gst/net/gstnetaddressmeta.h"
#include "gst/app/gstappsink.h"
#include "gst/app/gstappsrc.h"
#include "spdlog/spdlog.h"
#include <gio/gio.h>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <sstream>
#include <iostream>
#include <memory>
#include <utility>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <random>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace pipeline {
    static std::string gst_create_rtp_caps(const VideoCodec& videoCodec){
        std::stringstream ss;
        if(videoCodec==VideoCodec::H264){
            ss<<"caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H264, payload=(int)96\"";
        }else if(videoCodec==VideoCodec::H265){
            ss<<"caps=\"application/x-rtp, media=(string)video, encoding-name=(string)H265, clock-rate=(int)90000\"";
        }
        return ss.str();
    }
    static std::string create_rtp_depacketize_for_codec(const VideoCodec& codec){
        if(codec==VideoCodec::H264)return "rtph264depay ! ";
        if(codec==VideoCodec::H265)return "rtph265depay ! ";
        assert(false);
        return "";
    }
    static std::string create_parse_for_codec(const VideoCodec& codec){
        // config-interval=-1 = makes 100% sure each keyframe has SPS and PPS
        if(codec==VideoCodec::H264)return "h264parse config-interval=-1 ! ";
        if(codec==VideoCodec::H265)return "h265parse config-interval=-1  ! ";
        assert(false);
        return "";
    }
    static std::string create_out_caps(const VideoCodec& codec){
        if(codec==VideoCodec::H264){
            std::stringstream ss;
            ss<<"video/x-h264";
            ss<<", stream-format=\"byte-stream\",alignment=nal";
            //ss<<", alignment=\"nal\"";
            ss<<" ! ";
            return ss.str();
        }else if(codec==VideoCodec::H265){
            std::stringstream ss;
            ss<<"video/x-h265";
            ss<<", stream-format=\"byte-stream\"";
            //ss<<", alignment=\"nal\"";
            ss<<" ! ";
            return ss.str();
        }
        assert(false);
    }
}

namespace {
    static constexpr int kIdrUdpPort = 11223;
    static constexpr int kIdrBurstCount = 3;
    static constexpr int kIdrBurstSpacingMs = 100;
    static constexpr int kIdrRepeatCount = 3;
    static constexpr int kIdrRepeatSpacingMs = 100;
    static constexpr int kIdrRecordRepeatCount = 3;
    static constexpr int kIdrRecordRepeatSpacingMs = 150;
    static constexpr uint64_t kStreamDownMs = 1200;
    static constexpr uint64_t kStreamTickMs = 200;
    static constexpr uint64_t kIntegrityCooldownMs = 350;
    static constexpr uint64_t kRtpGapCooldownMs = 500;
    static constexpr uint64_t kDecodeStallMs = 700;
    static constexpr uint64_t kDecodeStallCooldownMs = 700;
    static constexpr uint64_t kDecodeStallPktWindowMs = 500;
    static constexpr uint64_t kRtpSeqResetMs = 1000;

    static std::mutex g_idr_sock_mutex;
    static int g_idr_sock = -1;
    static std::atomic<bool> g_idr_sock_ready{false};

    static std::mutex g_last_hop_mutex;
    static std::string g_last_hop_ip;
    static std::atomic<uint64_t> g_last_pkt_ms{0};
    static std::atomic<bool> g_stream_up{false};
    static std::atomic<bool> g_pending_rec_idr{false};
    static std::atomic<uint64_t> g_last_integrity_idr_ms{0};
    static std::atomic<uint64_t> g_last_rtp_gap_idr_ms{0};
    static std::atomic<uint64_t> g_last_decode_stall_idr_ms{0};
    static std::atomic<uint64_t> g_last_decoded_ms{0};
    static std::atomic<uint64_t> g_last_rtp_seq_ms{0};
    static std::atomic<uint16_t> g_last_rtp_seq{0};
    static std::atomic<bool> g_last_rtp_seq_valid{false};
    static std::atomic<bool> g_idr_enabled{true};
    static std::atomic<bool> g_stream_idr_pending{false};
    static std::atomic<bool> g_record_idr_pending{false};

    static uint64_t now_ms() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    static void request_idr_bursts(const char* reason, int request_count, bool allow_pending);

    static bool is_stream_idr_reason(const char* reason) {
        return reason && !strcmp(reason, "stream-up");
    }

    static bool is_record_idr_reason(const char* reason) {
        return reason && !strncmp(reason, "record-start", strlen("record-start"));
    }

    static bool ensure_idr_socket() {
        if (g_idr_sock_ready.load(std::memory_order_acquire)) {
            return true;
        }

        std::lock_guard<std::mutex> lock(g_idr_sock_mutex);
        if (g_idr_sock_ready.load(std::memory_order_relaxed)) {
            return true;
        }

        g_idr_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_idr_sock < 0) {
            spdlog::warn("[IDR] socket(AF_INET,SOCK_DGRAM) failed: {}", strerror(errno));
            return false;
        }

        g_idr_sock_ready.store(true, std::memory_order_release);
        spdlog::info("[IDR] UDP socket ready");
        return true;
    }

    static uint32_t secure_random_u32() {
        uint32_t out = 0;
#if defined(__linux__)
        ssize_t n = getrandom(&out, sizeof(out), 0);
        if (n == sizeof(out)) {
            return out;
        }
#endif
        static std::random_device rd;
        out = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
        return out;
    }

    static void make_idr_token3(char out[4]) {
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
        const uint32_t r0 = secure_random_u32();
        const uint32_t r1 = secure_random_u32();
        const uint32_t r2 = secure_random_u32();
        out[0] = alphabet[r0 % 26];
        out[1] = alphabet[r1 % 26];
        out[2] = alphabet[r2 % 26];
        out[3] = '\0';
    }

    static bool extract_sender_ip_from_buffer(GstBuffer* buf, std::string& out_ip) {
        out_ip.clear();
        if (!buf) {
            return false;
        }

        GstNetAddressMeta* meta = (GstNetAddressMeta*)gst_buffer_get_meta(buf, GST_NET_ADDRESS_META_API_TYPE);
        if (!meta || !meta->addr) {
            return false;
        }

        if (!G_IS_INET_SOCKET_ADDRESS(meta->addr)) {
            return false;
        }

        GInetSocketAddress* isa = G_INET_SOCKET_ADDRESS(meta->addr);
        GInetAddress* ia = g_inet_socket_address_get_address(isa);
        if (!ia) {
            return false;
        }

        gchar* s = g_inet_address_to_string(ia);
        if (!s) {
            return false;
        }

        out_ip = s;
        g_free(s);
        return !out_ip.empty();
    }

    static void maybe_update_last_hop_from_buffer(GstBuffer* buf) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        std::string ip;
        if (!extract_sender_ip_from_buffer(buf, ip)) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        if (ip != g_last_hop_ip) {
            g_last_hop_ip = ip;
            spdlog::info("[NET] Last-hop sender: {}", g_last_hop_ip);
        }
    }

    static std::string get_last_hop_ip_copy() {
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        return g_last_hop_ip;
    }

    static bool extract_rtp_sequence(GstBuffer* buf, uint16_t* out_seq) {
        if (!buf || !out_seq) {
            return false;
        }

        GstMapInfo map;
        if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
            return false;
        }

        bool ok = false;
        if (map.size >= 4) {
            const uint8_t* data = map.data;
            *out_seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
            ok = true;
        }

        gst_buffer_unmap(buf, &map);
        return ok;
    }

    static void maybe_request_idr_for_rtp_gap(uint16_t gap_count) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t now = now_ms();
        const uint64_t last = g_last_rtp_gap_idr_ms.load(std::memory_order_relaxed);
        if (last && (now - last) < kRtpGapCooldownMs) {
            return;
        }

        g_last_rtp_gap_idr_ms.store(now, std::memory_order_relaxed);
        spdlog::info("[IDR] RTP gap detected (missing {} packet(s)) -> request IDR", gap_count);
        request_idr_bursts("rtp-gap", 1, false);
    }

    static void maybe_track_rtp_sequence(GstBuffer* buf) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        uint16_t seq = 0;
        if (!extract_rtp_sequence(buf, &seq)) {
            return;
        }

        const uint64_t now = now_ms();
        if (!g_last_rtp_seq_valid.load(std::memory_order_relaxed)) {
            g_last_rtp_seq.store(seq, std::memory_order_relaxed);
            g_last_rtp_seq_ms.store(now, std::memory_order_relaxed);
            g_last_rtp_seq_valid.store(true, std::memory_order_relaxed);
            return;
        }

        const uint16_t last = g_last_rtp_seq.load(std::memory_order_relaxed);
        const uint16_t diff = static_cast<uint16_t>(seq - last);
        if (diff == 0) {
            return;
        }

        if (diff >= 30000) {
            const uint64_t last_ms = g_last_rtp_seq_ms.load(std::memory_order_relaxed);
            if (last_ms == 0 || (now - last_ms) > kRtpSeqResetMs) {
                g_last_rtp_seq.store(seq, std::memory_order_relaxed);
                g_last_rtp_seq_ms.store(now, std::memory_order_relaxed);
            }
            return;
        }

        if (diff > 1) {
            maybe_request_idr_for_rtp_gap(static_cast<uint16_t>(diff - 1));
        }

        g_last_rtp_seq.store(seq, std::memory_order_relaxed);
        g_last_rtp_seq_ms.store(now, std::memory_order_relaxed);
    }

    static void for_each_nal(const uint8_t* data, size_t size,
                             const std::function<void(const uint8_t*, size_t)>& cb) {
        auto find_start = [&](size_t from, size_t& start_len) -> size_t {
            for (size_t i = from; i + 3 < size; i++) {
                if (data[i] == 0x00 && data[i + 1] == 0x00) {
                    if (data[i + 2] == 0x01) {
                        start_len = 3;
                        return i;
                    }
                    if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                        start_len = 4;
                        return i;
                    }
                }
            }
            start_len = 0;
            return size;
        };

        size_t pos = 0;
        while (pos < size) {
            size_t start_len = 0;
            size_t start = find_start(pos, start_len);
            if (start == size) {
                break;
            }
            size_t nal_start = start + start_len;
            size_t next_len = 0;
            size_t next = find_start(nal_start, next_len);
            size_t nal_end = (next == size) ? size : next;
            if (nal_end > nal_start) {
                cb(data + nal_start, nal_end - nal_start);
            }
            pos = nal_end;
        }
    }

    static bool has_idr_frame(const uint8_t* data, size_t size, VideoCodec codec) {
        bool found = false;
        if (!data || size == 0) {
            return false;
        }
        for_each_nal(data, size, [&](const uint8_t* nal, size_t nal_size) {
            if (found || !nal || nal_size == 0) {
                return;
            }
            if (codec == VideoCodec::H265) {
                uint8_t nal_type = (nal[0] >> 1) & 0x3f;
                if (nal_type >= 16 && nal_type <= 21) {
                    found = true;
                }
            } else if (codec == VideoCodec::H264) {
                uint8_t nal_type = nal[0] & 0x1f;
                if (nal_type == 5) {
                    found = true;
                }
            }
        });
        return found;
    }

    static void maybe_mark_idr_received(const uint8_t* data, size_t size, VideoCodec codec) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_idr_pending.load(std::memory_order_relaxed) &&
            !g_record_idr_pending.load(std::memory_order_relaxed)) {
            return;
        }

        if (!has_idr_frame(data, size, codec)) {
            return;
        }

        if (g_stream_idr_pending.exchange(false, std::memory_order_relaxed)) {
            spdlog::info("[IDR] Stream refresh confirmed (IDR received)");
        }
        if (g_record_idr_pending.exchange(false, std::memory_order_relaxed)) {
            g_pending_rec_idr.store(false, std::memory_order_relaxed);
            spdlog::info("[IDR] Record refresh confirmed (IDR received)");
        }
    }

    static void send_idr_token_to_ip(const char* ip, const char token3[4]) {
        if (!ip || !ip[0]) {
            return;
        }

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(static_cast<uint16_t>(kIdrUdpPort));

        if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
            spdlog::warn("[IDR] inet_pton failed for ip={}", ip);
            return;
        }

        char payload[16];
        snprintf(payload, sizeof(payload), "%s\n", token3);
        int rc = sendto(g_idr_sock, payload, static_cast<int>(strlen(payload)), 0,
                        reinterpret_cast<sockaddr*>(&dst), static_cast<int>(sizeof(dst)));
        if (rc < 0) {
            spdlog::warn("[IDR] sendto({}:{}) failed: {}", ip, kIdrUdpPort, strerror(errno));
        }
    }

    static void send_idr_burst(const std::string& ip) {
        for (int i = 0; i < kIdrBurstCount; ++i) {
            char tok[4];
            make_idr_token3(tok);
            send_idr_token_to_ip(ip.c_str(), tok);
            if (i + 1 < kIdrBurstCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kIdrBurstSpacingMs));
            }
        }
    }

    static void request_idr_bursts(const char* reason, int request_count, bool allow_pending) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        const bool track_stream = is_stream_idr_reason(reason);
        const bool track_record = is_record_idr_reason(reason);
        if (track_stream) {
            g_stream_idr_pending.store(true, std::memory_order_relaxed);
        }
        if (track_record) {
            g_record_idr_pending.store(true, std::memory_order_relaxed);
        }

        const std::string ip = get_last_hop_ip_copy();
        if (ip.empty()) {
            spdlog::warn("[IDR] Cannot request IDR (last-hop unknown) reason={}", reason ? reason : "(null)");
            if (allow_pending) {
                g_pending_rec_idr.store(true, std::memory_order_relaxed);
            }
            return;
        }

        if (!ensure_idr_socket()) {
            return;
        }

        g_pending_rec_idr.store(false, std::memory_order_relaxed);
        const std::string reason_str = reason ? reason : "";

        if (track_record) {
            std::thread([ip, reason_str, request_count]() {
                const char* reason_c = reason_str.empty() ? "no-reason" : reason_str.c_str();
                for (int r = 0; r < request_count; ++r) {
                    if (!g_record_idr_pending.load(std::memory_order_relaxed)) {
                        spdlog::info("[IDR] Record refresh confirmed; skipping remaining bursts");
                        break;
                    }
                    spdlog::info("[IDR] Request 1 burst(s) to {}:{} ({} {}/{})",
                                 ip, kIdrUdpPort, reason_c, r + 1, request_count);
                    send_idr_burst(ip);
                    if (r + 1 < request_count) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(kIdrRecordRepeatSpacingMs));
                    }
                }
            }).detach();
            return;
        }

        std::thread([ip, reason_str, request_count]() {
            const char* reason_c = reason_str.empty() ? "no-reason" : reason_str.c_str();
            const bool track_stream = is_stream_idr_reason(reason_c);
            spdlog::info("[IDR] Request {} burst(s) to {}:{} ({})", request_count, ip, kIdrUdpPort, reason_c);
            for (int r = 0; r < request_count; ++r) {
                if (track_stream && !g_stream_idr_pending.load(std::memory_order_relaxed)) {
                    spdlog::info("[IDR] Stream refresh confirmed; skipping remaining bursts");
                    break;
                }
                send_idr_burst(ip);
                if (r + 1 < request_count) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kIdrRepeatSpacingMs));
                }
            }
        }).detach();
    }

    static void on_incoming_stream_buffer(GstBuffer* buf, const char* tag) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        g_last_pkt_ms.store(now_ms(), std::memory_order_relaxed);
        maybe_update_last_hop_from_buffer(buf);

        if (!g_stream_up.exchange(true)) {
            spdlog::info("[NET] Stream UP ({})", tag ? tag : "unknown");
            request_idr_bursts("stream-up", kIdrRepeatCount, false);
        }

        if (g_pending_rec_idr.load(std::memory_order_relaxed)) {
            if (!g_record_idr_pending.load(std::memory_order_relaxed)) {
                g_pending_rec_idr.store(false, std::memory_order_relaxed);
            } else {
                const std::string ip = get_last_hop_ip_copy();
                if (!ip.empty()) {
                    g_pending_rec_idr.store(false, std::memory_order_relaxed);
                    request_idr_bursts("record-start(pending)", kIdrRecordRepeatCount, false);
                }
            }
        }
    }

    static void maybe_request_decode_stall(uint64_t now) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t last_pkt = g_last_pkt_ms.load(std::memory_order_relaxed);
        const uint64_t last_decoded = g_last_decoded_ms.load(std::memory_order_relaxed);
        if (last_decoded == 0) {
            return;
        }

        if (last_pkt && (now - last_pkt) > kDecodeStallPktWindowMs) {
            return;
        }

        if (last_pkt > last_decoded && (now - last_decoded) > kDecodeStallMs) {
            const uint64_t last_idr = g_last_decode_stall_idr_ms.load(std::memory_order_relaxed);
            if (!last_idr || (now - last_idr) > kDecodeStallCooldownMs) {
                g_last_decode_stall_idr_ms.store(now, std::memory_order_relaxed);
                spdlog::info("[IDR] Decode stall (no frames for {} ms) -> request IDR", now - last_decoded);
                request_idr_bursts("decode-stall", 1, false);
            }
        }
    }

    static void tick_stream_presence() {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        static uint64_t last_tick = 0;
        const uint64_t now = now_ms();
        if (now - last_tick < kStreamTickMs) {
            return;
        }
        last_tick = now;

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t last = g_last_pkt_ms.load(std::memory_order_relaxed);
        if (last && now > last && (now - last) > kStreamDownMs) {
            if (g_stream_up.exchange(false)) {
                spdlog::info("[NET] Stream DOWN (no packets for {} ms)", now - last);
                g_last_rtp_seq_valid.store(false, std::memory_order_relaxed);
                g_last_rtp_seq_ms.store(0, std::memory_order_relaxed);
            }
        }

        maybe_request_decode_stall(now);
    }

    static void reset_stream_tracking() {
        g_stream_up.store(false, std::memory_order_relaxed);
        g_last_pkt_ms.store(0, std::memory_order_relaxed);
        g_last_decoded_ms.store(0, std::memory_order_relaxed);
        g_last_rtp_seq_valid.store(false, std::memory_order_relaxed);
        g_last_rtp_seq_ms.store(0, std::memory_order_relaxed);
        g_stream_idr_pending.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        g_last_hop_ip.clear();
    }

    static GstPadProbeReturn udp_last_hop_probe(GstPad*, GstPadProbeInfo* info, gpointer) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return GST_PAD_PROBE_OK;
        }

        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
            GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
            if (buf) {
                on_incoming_stream_buffer(buf, "udpsrc");
                maybe_track_rtp_sequence(buf);
            }
        }
        return GST_PAD_PROBE_OK;
    }

    static void attach_last_hop_probes(GstElement* pipeline) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!pipeline || !GST_IS_BIN(pipeline)) {
            return;
        }

        GstIterator* it = gst_bin_iterate_recurse(GST_BIN(pipeline));
        if (!it) {
            return;
        }

        GValue v = G_VALUE_INIT;
        while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
            GstElement* e = GST_ELEMENT(g_value_get_object(&v));
            GstElementFactory* f = e ? gst_element_get_factory(e) : nullptr;
            const gchar* fname = f ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)) : nullptr;

            if (fname && (!strcmp(fname, "udpsrc") || !strcmp(fname, "ts-udpsrc"))) {
                if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), "retrieve-sender-address")) {
                    g_object_set(G_OBJECT(e), "retrieve-sender-address", TRUE, NULL);
                }

                GstPad* src_pad = gst_element_get_static_pad(e, "src");
                if (src_pad) {
                    gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, udp_last_hop_probe, nullptr, nullptr);
                    gst_object_unref(src_pad);
                    spdlog::info("[NET] last-hop probe attached to {}", fname);
                }
            }

            g_value_unset(&v);
        }
        gst_iterator_free(it);
    }

    static void maybe_request_idr_rate_limited(const char* reason, const char* context) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        if (!g_stream_up.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t now = now_ms();
        const uint64_t last = g_last_integrity_idr_ms.load(std::memory_order_relaxed);
        if (last && (now - last) < kIntegrityCooldownMs) {
            return;
        }

        g_last_integrity_idr_ms.store(now, std::memory_order_relaxed);
        if (context && context[0]) {
            spdlog::info("[IDR] {} -> request IDR", context);
        } else {
            spdlog::info("[IDR] Decoder issue -> request IDR");
        }

        request_idr_bursts(reason ? reason : "decoder-issue", 1, false);
    }
}

static void initGstreamerOrThrow() {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        g_error_free(error);
        throw std::runtime_error("GStreamer initialization failed");
    }
}

GstRtpReceiver::GstRtpReceiver(int udp_port, const VideoCodec& codec)
{
    m_port=udp_port;
    m_video_codec=codec;
    initGstreamerOrThrow();

}

GstRtpReceiver::GstRtpReceiver(const char *s, const VideoCodec& codec) {
    unix_socket = strdup(s);
    m_video_codec = codec;
    initGstreamerOrThrow();

    spdlog::debug("Creating receiver socket on {}", unix_socket);

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;

    // Abstract socket: Start sun_path with a null byte, then copy the rest.
    // The "@" in logs is a placeholder for the null byte.
    addr.sun_path[0] = '\0';  // First byte is null
    strncpy(addr.sun_path + 1, unix_socket, sizeof(addr.sun_path) - 2);  // Leave room for null
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';  // Ensure null-terminated

    // Length = sizeof(sun_family) + 1 (null byte) + strlen(path)
    socklen_t addr_len = sizeof(addr.sun_family) + 1 + strlen(unix_socket);

    if (bind(sock, (struct sockaddr*)&addr, addr_len) < 0) {
        close(sock);
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }

    spdlog::debug("Bound successfully to abstract socket: @{}", unix_socket);
}

GstRtpReceiver::~GstRtpReceiver(){
    if (sock >= 0) {
        close(sock);
    }
}

static std::shared_ptr<std::vector<uint8_t>> gst_copy_buffer(GstBuffer* buffer){
    assert(buffer);
    const auto buff_size = gst_buffer_get_size(buffer);
    auto ret = std::make_shared<std::vector<uint8_t>>(buff_size);
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    assert(map.size == buff_size);
    std::memcpy(ret->data(), map.data, buff_size);
    gst_buffer_unmap(buffer, &map);
    return ret;
}

static void loop_pull_appsink_samples(bool& keep_looping,GstElement *app_sink_element,
                                      const GstRtpReceiver::NEW_FRAME_CALLBACK out_cb){
    assert(app_sink_element);
    assert(out_cb);
    const uint64_t timeout_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(100)).count();
    while (keep_looping){
        //GstSample* sample = nullptr;
        GstSample* sample= gst_app_sink_try_pull_sample(GST_APP_SINK(app_sink_element),timeout_ns);
        if (sample) {
            //gst_debug_sample(sample);
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            if (buffer) {
                on_incoming_stream_buffer(buffer, "appsink");
                auto buff_copy=gst_copy_buffer(buffer);
                out_cb(buff_copy);
            }
            gst_sample_unref(sample);
        }
        tick_stream_presence();
    }
}


std::string GstRtpReceiver::construct_gstreamer_pipeline()
{
    std::stringstream ss;
    if (! unix_socket)
        ss<<"udpsrc port="<<m_port<<" "<<pipeline::gst_create_rtp_caps(m_video_codec)<<" ! ";
    else
        ss<<"appsrc name=appsrc "<<pipeline::gst_create_rtp_caps(m_video_codec)<<" ! ";
    ss<<pipeline::create_rtp_depacketize_for_codec(m_video_codec);
    ss<<pipeline::create_parse_for_codec(m_video_codec);
    ss<<pipeline::create_out_caps(m_video_codec);
    ss<<" appsink drop=true name=out_appsink";
    return ss.str();
}

void GstRtpReceiver::loop_pull_samples()
{
    assert(m_app_sink_element);
    auto cb=[this](std::shared_ptr<std::vector<uint8_t>> sample){
        this->on_new_sample(sample);
    };
    loop_pull_appsink_samples(m_pull_samples_run,m_app_sink_element,cb);
}

void GstRtpReceiver::on_new_sample(std::shared_ptr<std::vector<uint8_t> > sample)
{
    if (sample && !sample->empty()) {
        maybe_mark_idr_received(sample->data(), sample->size(), m_video_codec);
    }
    if(m_cb){
        //debug_sample(sample);
        m_cb(sample);
    }else{
    }
}

/* socket → appsrc */
static constexpr int SOCKET_POLL_TIMEOUT_MS = 100;

static void loop_read_socket(bool& keep_looping, int sock_fd, GstAppSrc* appsrc) {
    GstBufferPool* pool = GST_BUFFER_POOL(g_object_get_data(G_OBJECT(appsrc), "buffer-pool"));
    uint64_t pkt_counter = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (keep_looping) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_fd, &read_fds);

        struct timeval timeout = { .tv_sec = 0, .tv_usec = SOCKET_POLL_TIMEOUT_MS * 1000 };
        int ready = select(sock_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        // Get buffer from pool
        GstBuffer* buffer = nullptr;
        GstFlowReturn ret = gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr);
        if (ret != GST_FLOW_OK || !buffer) {
            spdlog::warn("Failed to acquire buffer from pool");
            continue;
        }

        // Map buffer for writing
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            spdlog::warn("Failed to map buffer");
            gst_buffer_unref(buffer);
            continue;
        }

        // Read data directly into buffer
        ssize_t n = recv(sock_fd, map.data, map.size, 0);
        gst_buffer_unmap(buffer, &map);
        
        if (n <= RTP_HEADER_LEN) {
            spdlog::warn("Invalid RTP packet size: {}", n);
            gst_buffer_unref(buffer);
            continue;
        }

        // Resize buffer to actual data size
        gst_buffer_resize(buffer, 0, n);

        // Push to appsrc
        ret = gst_app_src_push_buffer(appsrc, buffer);
        if (ret != GST_FLOW_OK) {
            spdlog::warn("Appsrc push error: {}", gst_flow_get_name(ret));
            break;
        }

        // Log packet rate (optional)
        pkt_counter++;
        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            spdlog::debug("socket pkts/s {}", pkt_counter);
            pkt_counter = 0;
            last_report = now;
        }
    }
    
    if (pool) {
        gst_buffer_pool_set_active(pool, FALSE);
        gst_object_unref(pool);
    }
}

void GstRtpReceiver::start_receiving(NEW_FRAME_CALLBACK cb) {
    spdlog::info("GstRtpReceiver::start_receiving begin");
    assert(m_gst_pipeline == nullptr);
    m_cb = cb;

    switch_to_stream();

    spdlog::info("GstRtpReceiver::start_receiving end");
}

void GstRtpReceiver::stop_receiving() {
     spdlog::info("GstRtpReceiver::stop_receiving start");
    m_pull_samples_run = false;
    m_read_socket_run = false;
    
    if (m_pull_samples_thread) {
        m_pull_samples_thread->join();
        m_pull_samples_thread = nullptr;
    }
    
    if (m_read_socket_thread) {
        m_read_socket_thread->join();
        m_read_socket_thread = nullptr;
    }
    
    if (m_gst_pipeline != nullptr) {
        gst_element_send_event((GstElement*)m_gst_pipeline, gst_event_new_eos());
        gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
        gst_element_set_state(m_gst_pipeline, GST_STATE_NULL);
        gst_object_unref(m_gst_pipeline);
        m_gst_pipeline = nullptr;
    }
    reset_stream_tracking();
    spdlog::info("GstRtpReceiver::stop_receiving end");
}

std::string GstRtpReceiver::construct_file_playback_pipeline(const char * file_path) {
    std::stringstream ss;
    ss<<"filesrc location="<<file_path<<" ! qtdemux ! ";
    ss<<pipeline::create_parse_for_codec(m_video_codec);
    ss << pipeline::create_out_caps(m_video_codec);
    ss << " appsink drop=true name=out_appsink";
    return ss.str();
}

void GstRtpReceiver::switch_to_file_playback(const char * file_path) {
    stop_receiving();
    
    const auto pipeline = construct_file_playback_pipeline(file_path);
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER FILE PLAYBACK PIPE=[{}]", pipeline);
    
    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return;
    }
    
    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct file playback pipeline");
        m_gst_pipeline = nullptr;
        return;
    }

    // Setup appsink
    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);
    
    gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    
    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);
}

void GstRtpReceiver::switch_to_stream() {
    stop_receiving();
    
    const auto pipeline = construct_gstreamer_pipeline();
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER STREAM PIPE=[{}]", pipeline);
    
    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return;
    }
    
    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct streaming pipeline");
        m_gst_pipeline = nullptr;
        return;
    }

    attach_last_hop_probes(m_gst_pipeline);

    // If using Unix socket, setup appsrc with buffer pool
    if (unix_socket) {
        GstElement* appsrc = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "appsrc");
        if (!appsrc) {
            spdlog::error("Failed to get appsrc element from pipeline");
            return;
        }
        
        // Configure appsrc with buffer pool
        GstBufferPool* pool = nullptr;
        GstStructure* config = nullptr;
        
        g_object_set(appsrc,
            "stream-type", 0,
            "is-live", TRUE,
            "format", GST_FORMAT_TIME,
            "block", FALSE,
            "do-timestamp", TRUE,
            NULL);
            
        // Create buffer pool
        pool = gst_buffer_pool_new();
        config = gst_buffer_pool_get_config(pool);
        
        GstCaps* caps = gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "video",
            "encoding-name", G_TYPE_STRING, 
                (m_video_codec == VideoCodec::H264) ? "H264" : "H265",
            NULL);
        
        gst_buffer_pool_config_set_params(config, caps, MAX_PACKET_SIZE, 10, 20);
        gst_buffer_pool_set_config(pool, config);
        gst_caps_unref(caps);
        
        if (!gst_buffer_pool_set_active(pool, TRUE)) {
            spdlog::error("Failed to activate buffer pool");
            gst_object_unref(pool);
        } else {
            g_object_set_data(G_OBJECT(appsrc), "buffer-pool", pool);
        }
            
        // Start socket reading thread
        m_read_socket_run = true;
        m_read_socket_thread = std::make_unique<std::thread>([this, appsrc]() {
            pthread_setname_np(pthread_self(), "socket-reader");
            loop_read_socket(m_read_socket_run, this->sock, GST_APP_SRC(appsrc));
        });
    }

    // Setup appsink
    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);
    
    gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    
    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);
}

void GstRtpReceiver::set_playback_rate(double rate) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot set playback rate: pipeline is not running.");
        return;
    }
    

    spdlog::info("Setting playback rate to: {}", rate);

    // To change the playback rate, we seek to the current position with a new rate.
    // The flags ensure that the pipeline flushes old data and continues smoothly.
    GstEvent *seek_event = gst_event_new_seek(
        rate,
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_NONE, 0, // start from current position
        GST_SEEK_TYPE_NONE, 0  // do not change stop position
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event to change playback rate.");
    } else {
        m_playback_rate = rate;
    }
}

void GstRtpReceiver::fast_forward(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast forward rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    set_playback_rate(rate);
}

void GstRtpReceiver::fast_rewind(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast rewind rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    // For rewind, the rate must be negative
    set_playback_rate(-rate);
}

void GstRtpReceiver::normal_playback() {
    set_playback_rate(1.0);
}

void GstRtpReceiver::pause() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot pause: pipeline is not running.");
        return;
    }

    // If we're already paused, do nothing
    if (m_is_paused) {
        spdlog::debug("Pipeline is already paused.");
        return;
    }

    // Store current playback rate before pausing
    m_pre_pause_rate = m_playback_rate;
    
    // Set pipeline to PAUSED state
    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to pause pipeline");
        return;
    }

    // Wait for state change to complete
    ret = gst_element_get_state(m_gst_pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to complete pause operation");
        return;
    }

    m_is_paused = true;
    spdlog::info("Pipeline paused");
}

void GstRtpReceiver::resume() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot resume: pipeline is not running.");
        return;
    }

    // If we're not paused, do nothing
    if (!m_is_paused) {
        spdlog::debug("Pipeline is not paused.");
        return;
    }

    // Set pipeline back to PLAYING state
    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to resume pipeline");
        return;
    }

    // Restore previous playback rate if it wasn't normal
    if (m_pre_pause_rate != 1.0) {
        set_playback_rate(m_pre_pause_rate);
    }

    m_is_paused = false;
    spdlog::info("Pipeline resumed");
}

void GstRtpReceiver::skip_duration(int64_t skip_ms) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot skip: pipeline is not running.");
        return;
    }

    if (skip_ms == 0) {
        spdlog::debug("Skip duration is zero - no action taken.");
        return;
    }

    // Get current position
    gint64 current_pos;
    if (!gst_element_query_position(m_gst_pipeline, GST_FORMAT_TIME, &current_pos)) {
        spdlog::warn("Could not query current position");
        return;
    }

    // Calculate new position (convert skip_ms to nanoseconds)
    gint64 new_pos = current_pos + (skip_ms * GST_MSECOND);
    
    // Clamp the position to valid range
    if (new_pos < 0) {
        new_pos = 0;
        spdlog::debug("Clamped skip to start of stream");
    }

    spdlog::info("Skipping {} ms (from {} to {} ms)",
                skip_ms,
                current_pos / GST_MSECOND,
                new_pos / GST_MSECOND);

    // Create seek event
    GstEvent* seek_event = gst_event_new_seek(
        1.0,  // Normal playback rate
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, new_pos,  // start from new position
        GST_SEEK_TYPE_NONE, 0        // do not change stop position
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event for skipping.");
    }
}

void idr_set_enabled(bool enabled) {
    g_idr_enabled.store(enabled, std::memory_order_relaxed);
}

void idr_request_record_start() {
    request_idr_bursts("record-start", kIdrRecordRepeatCount, true);
}

void idr_request_decoder_issue(const char* reason) {
    const char* ctx = reason ? reason : "decoder-issue";
    maybe_request_idr_rate_limited(reason, ctx);
}

void idr_notify_decoded_frame() {
    if (!g_idr_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    g_last_decoded_ms.store(now_ms(), std::memory_order_relaxed);
}
