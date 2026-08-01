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
#include <algorithm>
#include <stdexcept>
#include <cassert>
#include <sstream>
#include <iostream>
#include <memory>
#include <utility>
#include <functional>
#include <fstream>
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
    static std::string create_rtp_depacketize_for_codec(const VideoCodec& codec, const std::string& name = ""){
        const std::string n = name.empty() ? "" : (" name=" + name);
        if(codec==VideoCodec::H264)return "rtph264depay" + n + " ! ";
        if(codec==VideoCodec::H265)return "rtph265depay" + n + " ! ";
        assert(false);
        return "";
    }
    // Bare RTP caps fields for use as an in-pipeline capsfilter on the video
    // branch when the source caps are left generic (audio muxed in). Unlike
    // gst_create_rtp_caps() this omits the udpsrc-style caps="..." wrapper and
    // does not pin a payload type, so the actual video PT is accepted as-is.
    static std::string gst_rtp_video_caps_fields(const VideoCodec& videoCodec){
        std::stringstream ss;
        ss<<"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)";
        ss<<((videoCodec==VideoCodec::H264) ? "H264" : "H265");
        return ss.str();
    }
    // Opus audio playback branch, fed from the shared rtp_tee.
    //
    // The leading leaky queue decouples the branch from the tee so a stalled or
    // slow ALSA sink can never back-pressure upstream and stall the video branch.
    //
    // Caps are asserted with capssetter, NOT a capsfilter: the tee broadcasts one
    // caps to all its branches, so a capsfilter demanding audio caps here would
    // force the tee to negotiate video-caps ∩ audio-caps = empty and the whole
    // pipeline (video included) would fail with not-negotiated. capssetter has an
    // ANY sink template, so it imposes nothing on the tee while still handing the
    // Opus RTP caps to rtpopusdepay downstream. A pad probe (attached after
    // parsing) drops the non-audio-PT packets that still arrive here.
    // Map the user's output selection to an alsasink "device". Empty or "default"
    // -> system default (no device property). A bare ALSA card id (e.g. the
    // "rockchiphdmi"/"HEADSET" ids from /proc/asound/cards, as the OSD menu
    // provides) -> plughw:CARD=<id> so format/rate conversion is handled. A value
    // that already looks like a full ALSA device string (contains ':') is used
    // verbatim, so power users can still pass e.g. plughw:CARD=x,DEV=1 via CLI.
    static std::string resolve_alsa_device(const std::string& sel){
        if(sel.empty() || sel == "default") return "";
        if(sel.find(':') != std::string::npos) return sel;
        return "plughw:CARD=" + sel + ",DEV=0";
    }

    static std::string create_audio_branch(int audio_pt, const std::string& device){
        std::stringstream ss;
        ss<<" rtp_tee. ! queue name=audio_in_queue leaky=downstream max-size-buffers=128"
            " max-size-bytes=0 max-size-time=0 silent=true"
            " ! capssetter replace=true caps=\"application/x-rtp, media=(string)audio,"
            " clock-rate=(int)48000, encoding-name=(string)OPUS, payload=(int)"<<audio_pt<<"\""
            " ! rtpopusdepay name=audio_depay ! opusdec ! audioconvert ! audioresample"
            // Software volume (named so it can be set live) — works regardless of
            // whether the sink card exposes a hardware mixer, e.g. HDMI has none.
            " ! volume name=audio_volume"
            // Keep live latency low. The dominant delay is alsasink's ring buffer,
            // which defaults to 200 ms; pin it (and the pre-sink queue) to ~50 ms.
            // buffer-time/latency-time are in microseconds. Lower = less delay but
            // more prone to dropouts on a loaded system — raise buffer-time (e.g.
            // 100000) if you hear crackle/underruns.
            " ! queue leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=50000000 silent=true"
            // provide-clock=false is critical: audio sinks are GStreamer's default
            // pipeline-clock provider, so without this alsasink hijacks the clock
            // and the (sync=true) video appsink paces the display to the *audio*
            // clock. When audio is sparse/absent that clock stalls -> video frames
            // are held (repeats) and back-pressure drops RTP packets (scrambled
            // frames). Keeping the system clock makes video timing independent of
            // audio, exactly as in the audio-off pipeline.
            " ! alsasink name=audio_sink sync=false async=false provide-clock=false buffer-time=50000 latency-time=10000";
        const std::string dev = resolve_alsa_device(device);
        if(!dev.empty()){
            ss<<" device=\""<<dev<<"\"";
        }
        return ss.str();
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
            ss<<", stream-format=\"byte-stream\", alignment=au";
            ss<<" ! ";
            return ss.str();
        }
        assert(false);
        return "";
    }
}

static VideoCodec detect_mp4_codec(const char* file_path) {
    auto scan = [](const uint8_t* buf, size_t n) -> VideoCodec {
        for (size_t i = 0; i + 3 < n; i++) {
            if (buf[i]=='a' && buf[i+1]=='v' && buf[i+2]=='c' && buf[i+3]=='1')
                return VideoCodec::H264;
            if (buf[i]=='h' && buf[i+1]=='v' && buf[i+2]=='c' && buf[i+3]=='1')
                return VideoCodec::H265;
            if (buf[i]=='h' && buf[i+1]=='e' && buf[i+2]=='v' && buf[i+3]=='1')
                return VideoCodec::H265;
        }
        return VideoCodec::UNKNOWN;
    };

    FILE* f = fopen(file_path, "rb");
    if (!f) return VideoCodec::UNKNOWN;

    uint8_t buf[16384];
    size_t n = fread(buf, 1, sizeof(buf), f);
    VideoCodec result = scan(buf, n);

    if (result == VideoCodec::UNKNOWN) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        long tail_offset = (fsize > (long)sizeof(buf)) ? fsize - (long)sizeof(buf) : 0;
        fseek(f, tail_offset, SEEK_SET);
        n = fread(buf, 1, sizeof(buf), f);
        result = scan(buf, n);
    }

    fclose(f);
    return result;
}

// Defined below; classifies a single RTP packet as H264/H265/UNKNOWN.
static VideoCodec classify_rtp_packet(const uint8_t* pkt, size_t len);

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

    // Number of consecutive RTP packets that must classify as the *other* codec
    // before a mid-stream switch is accepted. Frequent active-codec packets reset
    // the run, so a stray misclassified packet cannot trigger a rebuild.
    static constexpr int kCodecSwitchConfirm = 12;

    // Mid-stream codec-switch detection. g_active_codec is the codec the running
    // pipeline was built for; g_codec_switch_cb performs the rebuild and is set
    // by the receiver. The cb is invoked at most once until the rebuild clears
    // g_codec_switch_pending.
    static std::atomic<bool> g_codec_auto{false};
    static std::atomic<int> g_active_codec{static_cast<int>(VideoCodec::UNKNOWN)};
    static std::atomic<int> g_codec_switch_run{0};
    static std::atomic<bool> g_codec_switch_pending{false};
    static std::mutex g_codec_switch_mutex;
    static std::function<void(VideoCodec)> g_codec_switch_cb;

    // RTP payload type carrying muxed Opus audio, or -1 when audio is disabled.
    // Used to keep audio packets out of the video-only stream trackers (IDR
    // sequence-gap detection and mid-stream codec-switch detection).
    static std::atomic<int> g_audio_pt{-1};
    // Wall-clock ms of the last observed audio (Opus) packet; 0 = never seen.
    // Lets the DVR decide whether a recording should carry an audio track.
    static std::atomic<uint64_t> g_last_audio_pkt_ms{0};

    static bool is_audio_pt(uint8_t pt) {
        const int a = g_audio_pt.load(std::memory_order_relaxed);
        return a >= 0 && pt == static_cast<uint8_t>(a);
    }

    static std::mutex g_idr_sock_mutex;
    static int g_idr_sock = -1;
    static std::atomic<bool> g_idr_sock_ready{false};

    static std::mutex g_restream_mutex;
    static GstElement* g_restream_valve = nullptr;
    static GstElement* g_restream_sink = nullptr;
    static std::atomic<bool> g_restream_enabled{false};
    static std::string g_restream_target_ip;
    static std::string g_restream_manual_ip; // user's active selection; empty = auto-discover
    static std::string g_restream_pinned_ip;  // always shown in dropdown, set from config

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

    // True if Opus is flowing right now (a packet seen very recently). Recording
    // an empty audio track — which happens when --audio is on but the air sends
    // no audio — produces an unplayable mp4, so the DVR only adds an audio track
    // when this is true. Kept short so a recording started shortly after audio
    // stops doesn't get an empty track either.
    static bool audio_recently_seen() {
        const uint64_t last = g_last_audio_pkt_ms.load(std::memory_order_relaxed);
        return last != 0 && (now_ms() - last) < 1500;
    }

    static void request_idr_bursts(const char* reason, int request_count, bool allow_pending);
    static void maybe_update_restream_target(bool force);

    static bool contains_ip(const std::vector<std::string>& ips, const std::string& ip) {
        return !ip.empty() && std::find(ips.begin(), ips.end(), ip) != ips.end();
    }

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

    static void set_restream_valve_locked(bool enabled) {
        if (!g_restream_valve) {
            return;
        }
        g_object_set(G_OBJECT(g_restream_valve), "drop", enabled ? FALSE : TRUE, NULL);
    }

    static void update_restream_valve(bool enabled) {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        // Only force-close when disabling. Opening is handled by maybe_update_restream_target
        // once a valid target IP is confirmed, to avoid briefly streaming to 127.0.0.1.
        if (!enabled) {
            set_restream_valve_locked(false);
        }
    }

    static void clear_restream_valve() {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (!g_restream_valve) {
            if (!g_restream_sink) {
                return;
            }
        }
        if (g_restream_valve) {
            gst_object_unref(g_restream_valve);
            g_restream_valve = nullptr;
        }
        if (g_restream_sink) {
            gst_object_unref(g_restream_sink);
            g_restream_sink = nullptr;
        }
        g_restream_target_ip.clear();
    }

    static void bind_restream_valve(GstElement* pipeline) {
        clear_restream_valve();
        if (!pipeline || !GST_IS_BIN(pipeline)) {
            return;
        }

        GstElement* valve = gst_bin_get_by_name(GST_BIN(pipeline), "restream_valve");
        if (!valve) {
            return;
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "restream_sink");
        if (!sink) {
            gst_object_unref(valve);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_restream_mutex);
            g_restream_valve = valve;
            g_restream_sink = sink;
            g_restream_target_ip.clear();
            set_restream_valve_locked(false);
        }

        maybe_update_restream_target(true);
    }

    static std::string create_restream_branch() {
        std::stringstream ss;
        ss << " rtp_tee. ! valve name=restream_valve drop=true"
              " ! queue leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=1000000000 silent=true"
              " ! udpsink name=restream_sink host=0.0.0.0 port=5600 sync=false async=false qos=false";
        return ss.str();
    }

    static std::vector<std::string> scan_hotspot_clients() {
        std::ifstream arp_file("/proc/net/arp");
        if (!arp_file.is_open()) return {};
        std::string line;
        std::getline(arp_file, line); // skip header
        std::vector<std::string> result;
        while (std::getline(arp_file, line)) {
            std::istringstream iss(line);
            std::string ip, hw_type, flags, hw_address, mask, device;
            if (!(iss >> ip >> hw_type >> flags >> hw_address >> mask >> device)) continue;
            if (device != "wlan0" && device != "usb0") continue;
            if (flags == "0x0" || hw_address == "00:00:00:00:00:00") continue;
            result.push_back(ip);
        }
        return result;
    }

    static std::string find_first_hotspot_client_ip() {
        const auto clients = scan_hotspot_clients();
        if (contains_ip(clients, g_restream_target_ip)) {
            return g_restream_target_ip;
        }
        return clients.empty() ? "" : clients.front();
    }

    static void maybe_update_restream_target(bool force) {
        static uint64_t last_probe_ms = 0;
        const uint64_t now = now_ms();
        if (!force && (now - last_probe_ms) < 1000) {
            return;
        }
        last_probe_ms = now;

        bool new_target = false;
        {
            std::lock_guard<std::mutex> lock(g_restream_mutex);
            if (!g_restream_valve || !g_restream_sink) {
                return;
            }
            if (!g_restream_enabled.load(std::memory_order_relaxed)) {
                set_restream_valve_locked(false);
                return;
            }

            // If the user picked a specific IP use it, otherwise auto-discover.
            const std::string next_ip = !g_restream_manual_ip.empty()
                ? g_restream_manual_ip
                : find_first_hotspot_client_ip();
            if (next_ip.empty()) {
                if (!g_restream_target_ip.empty()) {
                    spdlog::info("[RESTREAM] No target client found; stopping unicast restream");
                    g_restream_target_ip.clear();
                }
                set_restream_valve_locked(false);
                return;
            }

            if (next_ip != g_restream_target_ip) {
                g_restream_target_ip = next_ip;
                g_object_set(G_OBJECT(g_restream_sink), "host", g_restream_target_ip.c_str(), NULL);
                spdlog::info("[RESTREAM] Streaming to {}:{}",
                             g_restream_target_ip,
                             5600);
                new_target = true;
            }

            set_restream_valve_locked(true);
        }

        if (new_target) {
            request_idr_bursts("restream-start", kIdrRepeatCount, false);
        }
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

        // Muxed audio has its own SSRC/sequence space; feeding it into the video
        // gap detector would trip spurious IDR requests, so skip audio packets.
        if (g_audio_pt.load(std::memory_order_relaxed) >= 0) {
            GstMapInfo map;
            if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                const bool audio = map.size >= 2 && is_audio_pt(map.data[1] & 0x7f);
                gst_buffer_unmap(buf, &map);
                if (audio) {
                    return;
                }
            }
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

    static void note_pipeline_codec(VideoCodec codec) {
        g_active_codec.store(static_cast<int>(codec), std::memory_order_relaxed);
        g_codec_switch_run.store(0, std::memory_order_relaxed);
        g_codec_switch_pending.store(false, std::memory_order_relaxed);
    }

    static void set_codec_switch_callback(std::function<void(VideoCodec)> cb) {
        std::lock_guard<std::mutex> lock(g_codec_switch_mutex);
        g_codec_switch_cb = std::move(cb);
    }

    // Inspect a raw RTP packet and, if the stream has switched to the other
    // codec for a sustained run, hand off a rebuild to the registered callback.
    // Called from RTP-ingress threads; must not block or rebuild inline.
    static void maybe_detect_codec_switch(const uint8_t* rtp, size_t len) {
        if (!g_codec_auto.load(std::memory_order_relaxed)) {
            return; // codec pinned by the user; never override it
        }
        const int active = g_active_codec.load(std::memory_order_relaxed);
        if (active == static_cast<int>(VideoCodec::UNKNOWN)) {
            return; // no pipeline built yet, nothing to compare against
        }
        if (g_codec_switch_pending.load(std::memory_order_relaxed)) {
            return; // a switch is already being applied
        }
        if (len >= 2 && is_audio_pt(rtp[1] & 0x7f)) {
            return; // muxed audio packet: not a video codec signal
        }

        const VideoCodec c = classify_rtp_packet(rtp, len);
        if (c == VideoCodec::UNKNOWN) {
            return; // ambiguous packet: neither confirm nor reset
        }
        if (static_cast<int>(c) == active) {
            g_codec_switch_run.store(0, std::memory_order_relaxed);
            return;
        }

        if (g_codec_switch_run.fetch_add(1, std::memory_order_relaxed) + 1 < kCodecSwitchConfirm) {
            return;
        }

        // Confirmed switch; ensure we only fire once until the rebuild completes.
        if (g_codec_switch_pending.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        g_codec_switch_run.store(0, std::memory_order_relaxed);
        spdlog::info("[CODEC] Mid-stream switch detected: {} -> {}",
                     active == static_cast<int>(VideoCodec::H265) ? "H.265" : "H.264",
                     c == VideoCodec::H265 ? "H.265" : "H.264");

        std::function<void(VideoCodec)> cb;
        {
            std::lock_guard<std::mutex> lock(g_codec_switch_mutex);
            cb = g_codec_switch_cb;
        }
        if (cb) {
            cb(c);
        } else {
            g_codec_switch_pending.store(false, std::memory_order_relaxed);
        }
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
                GstMapInfo map;
                if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                    maybe_detect_codec_switch(map.data, map.size);
                    gst_buffer_unmap(buf, &map);
                }
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

    // Payload-type demux for the shared RTP flow. When audio is muxed in, the
    // tee hands every packet to both the video and audio branches; these probes
    // drop the packets that do not belong on a given branch before they reach a
    // depayloader that would choke on them. drop_when_match=true keeps the audio
    // PT off the video branch; false keeps everything but the audio PT off the
    // audio branch.
    struct RtpPtFilter {
        uint8_t pt;
        bool drop_when_match;
    };

    static GstPadProbeReturn rtp_pt_filter_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
        const RtpPtFilter* f = static_cast<const RtpPtFilter*>(user_data);
        if (!f || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
            return GST_PAD_PROBE_OK;
        }
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buf) {
            return GST_PAD_PROBE_OK;
        }
        bool drop = false;
        GstMapInfo map;
        if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
            if (map.size >= 2) {
                const bool match = ((map.data[1] & 0x7f) == f->pt);
                // f->pt is the audio PT for both branch filters, so a match means
                // an Opus packet just went by — note it for audio_recently_seen().
                if (match) g_last_audio_pkt_ms.store(now_ms(), std::memory_order_relaxed);
                drop = f->drop_when_match ? match : !match;
            }
            gst_buffer_unmap(buf, &map);
        }
        return drop ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
    }

    static void attach_pt_filter(GstElement* pipeline, const char* elem_name,
                                 uint8_t pt, bool drop_when_match) {
        if (!pipeline || !GST_IS_BIN(pipeline)) {
            return;
        }
        GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline), elem_name);
        if (!e) {
            return;
        }
        GstPad* pad = gst_element_get_static_pad(e, "sink");
        if (pad) {
            RtpPtFilter* f = g_new(RtpPtFilter, 1);
            f->pt = pt;
            f->drop_when_match = drop_when_match;
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, rtp_pt_filter_probe, f,
                              (GDestroyNotify)g_free);
            gst_object_unref(pad);
        }
        gst_object_unref(e);
    }

    static bool audio_factory_exists(const char* name) {
        GstElementFactory* f = gst_element_factory_find(name);
        if (f) {
            gst_object_unref(f);
            return true;
        }
        spdlog::warn("[AUDIO] Missing GStreamer element '{}'; disabling audio", name);
        return false;
    }

    // Verify the Opus/ALSA elements exist and the resolved output device can
    // actually be opened, so audio can gracefully fall back to video-only instead
    // of failing the whole pipeline (which would also kill video). Important for
    // hot-pluggable sinks (e.g. a USB headset): if the selected card is gone the
    // audio branch is simply left out until the next rebuild finds it available.
    // `device` is the resolved alsasink device string ("" = system default).
    static bool audio_stack_available(const std::string& device) {
        static const char* kNeeded[] = {
            "capssetter", "rtpopusdepay", "opusdec", "audioconvert", "audioresample", "volume", "alsasink"
        };
        for (const char* name : kNeeded) {
            if (!audio_factory_exists(name)) {
                return false;
            }
        }

        GstElement* sink = gst_element_factory_make("alsasink", nullptr);
        if (!sink) {
            return false;
        }
        if (!device.empty()) {
            g_object_set(G_OBJECT(sink), "device", device.c_str(), NULL);
        }
        // NULL -> READY opens the PCM device; a failure here means the device is
        // absent or busy, so keep audio off rather than break the pipeline.
        const GstStateChangeReturn r = gst_element_set_state(sink, GST_STATE_READY);
        const bool ok = (r != GST_STATE_CHANGE_FAILURE);
        gst_element_set_state(sink, GST_STATE_NULL);
        gst_object_unref(sink);
        if (!ok) {
            spdlog::warn("[AUDIO] ALSA device '{}' unavailable; disabling audio",
                         device.empty() ? "default" : device);
        }
        return ok;
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

// --- RTP codec autodetection ---------------------------------------------
// Classify a single RTP/AVP packet as H264 or H265 by inspecting the NAL
// header that follows the RTP header. Returns UNKNOWN when the packet is not a
// usable video packet or is ambiguous.
static VideoCodec classify_rtp_packet(const uint8_t* pkt, size_t len) {
    if (len < RTP_HEADER_LEN + 1) return VideoCodec::UNKNOWN;
    if (((pkt[0] >> 6) & 0x3) != 2) return VideoCodec::UNKNOWN;   // RTP version 2
    size_t off = RTP_HEADER_LEN + (pkt[0] & 0x0F) * 4;            // skip CSRCs
    if (pkt[0] & 0x10) {                                          // skip extension
        if (len < off + 4) return VideoCodec::UNKNOWN;
        off += 4 + ((pkt[off + 2] << 8 | pkt[off + 3]) * 4);
    }
    if (off >= len) return VideoCodec::UNKNOWN;

    const uint8_t nb = pkt[off];
    if (nb & 0x80) return VideoCodec::UNKNOWN;                    // forbidden_zero_bit

    // Fragmentation units carry the bulk of a video stream and use disjoint
    // header bytes between the two codecs, so they are the most reliable signal.
    if (nb == 0x1C || nb == 0x3C || nb == 0x5C || nb == 0x7C)     // H264 FU-A (type 28)
        return VideoCodec::H264;
    if (nb == 0x62 || nb == 0x63)                                 // H265 FU (type 49)
        return VideoCodec::H265;

    // Otherwise (parameter sets, SEI, single-NAL slices) prefer the codec whose
    // NAL-type interpretation is valid while the other's is not, e.g. VPS(32)
    // -> 0x40 is invalid as H264 type 0, H264 SPS(7) -> 0x67 is invalid as
    // H265 type 51.
    const uint8_t t264 = nb & 0x1F;
    const uint8_t t265 = (nb >> 1) & 0x3F;
    const bool h264_valid = (t264 >= 1 && t264 <= 23);
    const bool h265_valid = (t265 <= 40);
    if (h265_valid && !h264_valid) return VideoCodec::H265;
    if (h264_valid && !h265_valid) return VideoCodec::H264;
    return VideoCodec::UNKNOWN;
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
    m_auto_codec = (codec == VideoCodec::UNKNOWN);
    initGstreamerOrThrow();

}

GstRtpReceiver::GstRtpReceiver(const char *s, const VideoCodec& codec) {
    unix_socket = strdup(s);
    m_video_codec = codec;
    m_auto_codec = (codec == VideoCodec::UNKNOWN);
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
    // Drop the detection callback so the ingress threads can never call back
    // into a destroyed receiver.
    set_codec_switch_callback(nullptr);
    g_codec_auto.store(false, std::memory_order_relaxed);
    note_pipeline_codec(VideoCodec::UNKNOWN);
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
                                      const GstRtpReceiver::NEW_FRAME_CALLBACK out_cb,
                                      const std::function<void()>& on_tick){
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
        maybe_update_restream_target(false);
        tick_stream_presence();
        // Drive the DVR record branch here (not from the menu/signal threads):
        // this thread's lifetime is bounded by the pipeline's, so add/remove of
        // the splitmuxsink branch is always against the live pipeline.
        if (on_tick) on_tick();
    }
}


std::string GstRtpReceiver::construct_gstreamer_pipeline()
{
    std::stringstream ss;
    // Two independent concerns:
    //  - demux_audio: the air unit muxes Opus into the RTP flow (i.e. the user
    //    enabled audio), so the source caps must stay generic and the video
    //    branch must DROP audio packets by payload type — otherwise the video
    //    depayloader chokes on Opus and corrupts the picture. This must hold even
    //    when playback is impossible (e.g. USB sink unplugged mid-flight): the
    //    drone keeps sending audio, we just can't play it.
    //  - play_audio: the ALSA sink is actually usable, so build the playback
    //    branch. A subset of demux_audio.
    const bool demux_audio = m_audio_enabled;   // audio present in the flow
    const bool play_audio  = m_audio_active;     // sink usable -> alsasink branch
    const std::string src_caps = demux_audio
        ? std::string("caps=\"application/x-rtp\"")
        : pipeline::gst_create_rtp_caps(m_video_codec);
    if (! unix_socket)
        ss<<"udpsrc port="<<m_port<<" "<<src_caps<<" ! tee name=rtp_tee ";
    else
        ss<<"appsrc name=appsrc "<<src_caps<<" ! tee name=rtp_tee ";
    ss<<"rtp_tee. ! ";
    if (demux_audio) {
        ss<<pipeline::gst_rtp_video_caps_fields(m_video_codec)<<" ! ";
        ss<<pipeline::create_rtp_depacketize_for_codec(m_video_codec, "video_depay");
    } else {
        ss<<pipeline::create_rtp_depacketize_for_codec(m_video_codec);
    }
    ss<<pipeline::create_parse_for_codec(m_video_codec);
    ss<<pipeline::create_out_caps(m_video_codec);
    ss<<"appsink drop=true name=out_appsink";
    if (play_audio) {
        ss<<pipeline::create_audio_branch(m_audio_pt, m_audio_device);
    }
    ss<<create_restream_branch();
    return ss.str();
}

void GstRtpReceiver::loop_pull_samples()
{
    assert(m_app_sink_element);
    auto cb=[this](std::shared_ptr<std::vector<uint8_t>> sample){
        this->on_new_sample(sample);
    };
    loop_pull_appsink_samples(m_pull_samples_run,m_app_sink_element,cb,
                              [this]{ this->dvr_tick(); this->handle_bus_messages(); });
}

void GstRtpReceiver::handle_bus_messages()
{
    if (!m_gst_pipeline) return;
    GstBus* bus = gst_element_get_bus(m_gst_pipeline);
    if (!bus) return;

    bool audio_failed = false;
    GstMessage* msg;
    // Pop ERROR/WARNING only — the DVR relies on ELEMENT (fragment-closed)
    // messages, which stay in the bus for dvr_remove_*_bin() to consume. Draining
    // here is also what stops a failing sink's message spam from exhausting memory.
    while ((msg = gst_bus_pop_filtered(
                bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) != nullptr) {
        GstObject* src = GST_MESSAGE_SRC(msg);
        gchar* name = src ? gst_object_get_name(src) : nullptr;
        const bool from_audio_sink = name && strstr(name, "audio_sink") != nullptr;

        GError* err = nullptr;
        gchar* dbg = nullptr;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            gst_message_parse_error(msg, &err, &dbg);
        else
            gst_message_parse_warning(msg, &err, &dbg);

        if (from_audio_sink) {
            // Throttle: a disconnected USB sink posts an error per failed write
            // (hundreds/s). Log once, only while we still think audio is up; after
            // the fallback below flips m_audio_active off we drain silently.
            if (m_audio_active && !audio_failed)
                spdlog::warn("[AUDIO] output sink {}: {} — falling back to video-only",
                             GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ? "error" : "warning",
                             err ? err->message : "unknown");
            audio_failed = true;
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            spdlog::warn("[PIPE] error from {}: {}", name ? name : "?", err ? err->message : "unknown");
        }
        if (err) g_error_free(err);
        g_free(dbg);
        g_free(name);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);

    // A dead audio sink (e.g. USB headset unplugged) would otherwise have alsasink
    // spinning at 100% CPU. Rebuild video-only: switch_to_stream()'s pre-flight
    // (audio_stack_available) finds the device gone and leaves audio out, which
    // also tears the spinning sink down. Off-thread because switch_to_stream()
    // joins this pull thread.
    //
    // Re-entry gate: clear m_audio_active *now*. handle_bus_messages() only ever
    // runs on the pull thread, so this store alone stops the fast-ticking loop
    // from spawning a second rebuild — with no persistent flag that could get
    // stuck true if a later teardown stalls (which made only the first unplug
    // work). The demux stays up (keyed on m_audio_enabled), so the drone's
    // ongoing audio keeps being dropped from the video path meanwhile.
    if (audio_failed && m_audio_active) {
        m_audio_active = false;
        spdlog::warn("[AUDIO] Output sink failed (device unplugged?) — falling back to video-only");
        std::thread([this]() { switch_to_stream(); }).detach();
    }
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
        if (n > 0) {
            maybe_detect_codec_switch(map.data, static_cast<size_t>(n));
        }
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
        clear_restream_valve();
        // Finalize any active recording cleanly BEFORE tearing the pipeline down
        // (writes each mp4's moov via EOS + fragment-closed wait). The pull thread
        // is already joined, so no dvr_tick can race this. m_dvr_want is left as-is
        // so a recording that spans a rebuild is re-armed on the new pipeline.
        if (m_dvr_rec_bin) dvr_remove_record_bin();
        if (m_dvr_reenc_bin) dvr_remove_reenc_bin();
        // NOTE: no pipeline-wide EOS here. It's redundant now that the recorders
        // are finalized above, and a pipeline EOS makes alsasink *drain* its ring
        // buffer — which never completes on a disconnected USB device and would
        // hang this teardown (freezing video, since the pull thread is already
        // joined). set_state(NULL) flushes without draining, so a dead sink can't
        // stall the fallback rebuild.
        gst_element_set_state(m_gst_pipeline, GST_STATE_NULL);
        gst_object_unref(m_gst_pipeline);
        m_gst_pipeline = nullptr;
        // Safety net: drop any handles the removes above didn't (shouldn't happen).
        if (m_dvr_tee_video_pad) { gst_object_unref(m_dvr_tee_video_pad); m_dvr_tee_video_pad = nullptr; }
        if (m_dvr_tee_audio_pad) { gst_object_unref(m_dvr_tee_audio_pad); m_dvr_tee_audio_pad = nullptr; }
        m_dvr_rec_bin = nullptr;
        m_dvr_active.store(false, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(m_dvr_reenc_src_mutex); m_dvr_reenc_appsrc = nullptr; }
        if (m_dvr_reenc_tee_audio_pad) { gst_object_unref(m_dvr_reenc_tee_audio_pad); m_dvr_reenc_tee_audio_pad = nullptr; }
        m_dvr_reenc_bin = nullptr;
        m_dvr_reenc_active.store(false, std::memory_order_relaxed);
    }
    reset_stream_tracking();
    spdlog::info("GstRtpReceiver::stop_receiving end");
}

std::string GstRtpReceiver::construct_file_playback_pipeline(const char * file_path) {
    VideoCodec file_codec = detect_mp4_codec(file_path);
    if (file_codec == VideoCodec::UNKNOWN) {
        spdlog::warn("Could not detect codec in {}, falling back to stream codec", file_path);
        file_codec = m_video_codec;
    } else {
        spdlog::info("Detected {} codec in DVR file",
                     file_codec == VideoCodec::H265 ? "H.265" : "H.264");
    }
    m_playback_codec = file_codec;

    std::stringstream ss;
    ss<<"filesrc location="<<file_path<<" ! qtdemux ! ";
    ss<<pipeline::create_parse_for_codec(file_codec);
    ss << pipeline::create_out_caps(file_codec);
    ss << " appsink drop=true name=out_appsink";
    return ss.str();
}

VideoCodec GstRtpReceiver::switch_to_file_playback(const char * file_path) {
    std::lock_guard<std::mutex> lock(m_stream_mutex);
    stop_receiving();
    m_file_playback = true;

    // File playback has no live RTP ingress; make sure the audio-PT tracker
    // guard is inert so it can't affect anything during DVR review.
    g_audio_pt.store(-1, std::memory_order_relaxed);

    const auto pipeline = construct_file_playback_pipeline(file_path);
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline.c_str(), &error);
    spdlog::info("GSTREAMER FILE PLAYBACK PIPE=[{}]", pipeline);

    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return m_playback_codec;
    }

    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct file playback pipeline");
        m_gst_pipeline = nullptr;
        return m_playback_codec;
    }

    // Setup appsink
    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);

    gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);

    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstRtpReceiver::loop_pull_samples, this);
    return m_playback_codec;
}

void GstRtpReceiver::switch_to_stream() {
    std::lock_guard<std::mutex> lock(m_stream_mutex);
    stop_receiving();
    m_file_playback = false;

    // Auto mode: build for H.265 up front and let mid-stream detection flip to
    // H.264 from the RTP ingress if the stream turns out to be H.264. The
    // ingress classifier sees raw RTP before depay/parse, so it works even
    // though the initial pipeline guesses wrong, and this avoids blocking
    // startup to sniff (which would stall when no stream is live yet).
    if (m_video_codec == VideoCodec::UNKNOWN) {
        m_video_codec = VideoCodec::H265;
        spdlog::info("[CODEC] Auto mode: defaulting to H.265; mid-stream detection will correct if needed");
    }

    // Resolve the effective audio state: only build the Opus branch if the user
    // wants audio AND the Opus/ALSA stack + selected output device are actually
    // usable right now. Otherwise fall back to video-only so a missing plugin or
    // an unplugged/absent sink can't take the whole pipeline (and thus video)
    // down. The user's intent (m_audio_enabled) and selection (m_audio_device)
    // are kept, so the next rebuild picks the device back up once it returns.
    m_audio_active = m_audio_enabled &&
                     audio_stack_available(pipeline::resolve_alsa_device(m_audio_device));
    // Audio is in the RTP flow whenever the user enabled it (even if the sink is
    // gone), so the payload-type demux must run to keep Opus off the video path.
    g_audio_pt.store(m_audio_enabled ? m_audio_pt : -1, std::memory_order_relaxed);
    if (m_audio_enabled) {
        spdlog::info("[AUDIO] Opus audio {} (pt={}, device={})",
                     m_audio_active ? "enabled" : "requested but unavailable -> video-only",
                     m_audio_pt, m_audio_device.empty() ? "default" : m_audio_device);
    }

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
    bind_restream_valve(m_gst_pipeline);

    // Payload-type demux. The video filter must run whenever audio is in the flow
    // (m_audio_enabled) — including the sink-unplugged fallback — to keep Opus off
    // the video depayloader. The audio-side filter + volume only exist when the
    // playback branch was actually built (m_audio_active).
    if (m_audio_enabled) {
        const uint8_t audio_pt = static_cast<uint8_t>(m_audio_pt);
        attach_pt_filter(m_gst_pipeline, "video_depay", audio_pt, /*drop_when_match=*/true);
        if (m_audio_active) {
            attach_pt_filter(m_gst_pipeline, "audio_depay", audio_pt, /*drop_when_match=*/false);
            // Apply the configured software volume to the freshly-built branch.
            GstElement* vol = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "audio_volume");
            if (vol) {
                g_object_set(vol, "volume", static_cast<gdouble>(m_audio_volume), NULL);
                gst_object_unref(vol);
            }
        }
    }

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
        
        // With audio muxed in, the appsrc carries mixed media so its caps stay
        // generic (matching the pipeline string); the per-branch capsfilters
        // pick the media type. Otherwise pin the video codec as before. Keyed on
        // m_audio_enabled (not _active) so the fallback pipeline (sink gone, audio
        // still in the flow) keeps generic caps and demuxes the audio out.
        GstCaps* caps = m_audio_enabled
            ? gst_caps_new_empty_simple("application/x-rtp")
            : gst_caps_new_simple("application/x-rtp",
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

    // Arm mid-stream codec-switch detection for the codec we just built for,
    // but only in auto mode (a pinned codec is never overridden).
    g_codec_auto.store(m_auto_codec, std::memory_order_relaxed);
    set_codec_switch_callback([this](VideoCodec new_codec) {
        request_codec_switch(new_codec);
    });
    note_pipeline_codec(m_video_codec);
}

void GstRtpReceiver::request_codec_switch(VideoCodec new_codec) {
    // Rebuild on a detached thread: switch_to_stream() tears down the pipeline
    // (set_state NULL) and joins the pull/socket threads, neither of which is
    // safe to do from a GStreamer streaming thread or the socket reader itself.
    std::thread([this, new_codec]() {
        m_video_codec = new_codec;          // non-UNKNOWN: switch_to_stream keeps it as-is
        switch_to_stream();                 // also clears the pending flag
        std::function<void(VideoCodec)> cb;
        {
            std::lock_guard<std::mutex> lock(m_codec_changed_mutex);
            cb = m_on_codec_changed;
        }
        if (cb) {
            cb(new_codec);                  // let the host realign its decoder
        }
    }).detach();
}

void GstRtpReceiver::configure_audio(bool enabled, const std::string& device, int pt, double volume) {
    m_audio_enabled = enabled;
    m_audio_device = device;
    m_audio_pt = (pt > 0 && pt < 128) ? pt : 98;
    m_audio_volume = (volume < 0.0) ? 0.0 : (volume > 1.0 ? 1.0 : volume);
}

void GstRtpReceiver::set_audio_volume(double volume) {
    volume = (volume < 0.0) ? 0.0 : (volume > 1.0 ? 1.0 : volume);
    m_audio_volume = volume;

    // Apply live to the running volume element (no rebuild needed). Serialize
    // the pipeline read with switch_to_stream() so we never touch a half-swapped
    // pipeline; the value is stored above regardless, for the next build.
    std::lock_guard<std::mutex> lock(m_stream_mutex);
    if (!m_gst_pipeline) {
        return;
    }
    GstElement* vol = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "audio_volume");
    if (vol) {
        g_object_set(vol, "volume", static_cast<gdouble>(volume), NULL);
        gst_object_unref(vol);
    }
}

void GstRtpReceiver::set_audio_enabled(bool enabled) {
    // Always record intent and rebuild — no early-out on "unchanged", so tapping
    // the switch re-evaluates device availability and acts as a retry once a
    // hot-plugged sink is back (the switch reports m_audio_active, so it may read
    // off while intent is on).
    m_audio_enabled = enabled;
    spdlog::info("[AUDIO] Runtime toggle -> {}", enabled ? "on" : "off");

    // Only the live streaming pipeline carries the audio branch; during DVR file
    // playback (or before start), just remember the choice for the next stream.
    if (m_file_playback || m_gst_pipeline == nullptr) {
        return;
    }

    // Rebuild off-thread: switch_to_stream() tears down the pipeline and joins
    // the pull/socket threads, which must not run on a GStreamer or UI thread.
    std::thread([this]() { switch_to_stream(); }).detach();
}

void GstRtpReceiver::set_audio_device(const std::string& device) {
    if (device == m_audio_device) {
        return;
    }
    m_audio_device = device;
    spdlog::info("[AUDIO] Output device -> {}", device.empty() ? "default" : device);

    // Only relevant while the live audio branch exists; otherwise the new device
    // is picked up the next time audio is (re)built.
    if (m_file_playback || m_gst_pipeline == nullptr || !m_audio_enabled) {
        return;
    }
    std::thread([this]() { switch_to_stream(); }).detach();
}

void GstRtpReceiver::set_codec_changed_callback(std::function<void(VideoCodec)> cb) {
    std::lock_guard<std::mutex> lock(m_codec_changed_mutex);
    m_on_codec_changed = std::move(cb);
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

// --- DVR record branch (splitmuxsink) ---------------------------------------

// Drop any ELEMENT messages queued on the pipeline bus. Called before EOS so a
// fragment-closed from an earlier size-split can't be mistaken for this stop's.
static void dvr_drain_element_messages(GstBus* bus) {
    if (!bus) return;
    GstMessage* m;
    while ((m = gst_bus_pop_filtered(bus, GST_MESSAGE_ELEMENT)) != nullptr) gst_message_unref(m);
}

// Wait (bounded) for the splitmuxsink to post fragment-closed after an EOS, so
// the mp4 moov is written before the bin is torn down.
static void dvr_wait_fragment_closed(GstBus* bus) {
    if (!bus) return;
    const gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND * 2;
    bool closed = false;
    while (!closed && g_get_monotonic_time() < deadline) {
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, GST_MESSAGE_ELEMENT);
        if (!msg) continue;
        if (gst_message_has_name(msg, "splitmuxsink-fragment-closed")) closed = true;
        gst_message_unref(msg);
    }
    if (!closed) spdlog::warn("[DVR] timed out waiting for mp4 finalization");
}

// Expose an in-bin element's sink pad as a ghost pad so an external tee can link
// into the bin. Returns a reffed pad (or nullptr).
static GstPad* dvr_ghost_sink(GstElement* bin, const char* elem, const char* gname) {
    GstElement* e = gst_bin_get_by_name(GST_BIN(bin), elem);
    if (!e) return nullptr;
    GstPad* p = gst_element_get_static_pad(e, "sink");
    GstPad* g = gst_ghost_pad_new(gname, p);
    gst_pad_set_active(g, TRUE);
    gst_element_add_pad(bin, g);
    gst_object_unref(p);
    gst_object_unref(e);
    return gst_element_get_static_pad(bin, gname); // reffed
}

// gst_element_request_pad_simple() only exists since GStreamer 1.19.1; older
// distros (Debian Bullseye ships 1.18) provide the now-deprecated
// gst_element_get_request_pad(). Pick the right one per build so both compile
// cleanly (no missing symbol on 1.18, no deprecation warning on 1.20+).
static GstPad* dvr_request_tee_pad(GstElement* tee) {
#if GST_CHECK_VERSION(1, 19, 1)
    return gst_element_request_pad_simple(tee, "src_%u");
#else
    return gst_element_get_request_pad(tee, "src_%u");
#endif
}

void GstRtpReceiver::set_dvr_config(int64_t max_size_bytes, std::function<std::string()> base_path_fn) {
    std::lock_guard<std::mutex> lk(m_dvr_cfg_mutex);
    m_dvr_max_size = max_size_bytes > 0 ? max_size_bytes : 0;
    m_dvr_base_path_fn = std::move(base_path_fn);
}

void GstRtpReceiver::dvr_set_max_size(int64_t max_size_bytes) {
    std::lock_guard<std::mutex> lk(m_dvr_cfg_mutex);
    m_dvr_max_size = max_size_bytes > 0 ? max_size_bytes : 0;
    // Apply live to an in-progress recording (honoured at the next split). The
    // element, if present, is only torn down by the pull thread; this setter is
    // called from the menu thread, so read it once under no additional lock —
    // a torn-down bin just means the value applies to the next start.
    if (m_dvr_rec_bin) {
        GstElement* sms = gst_bin_get_by_name(GST_BIN(m_dvr_rec_bin), "dvr_sms");
        if (sms) {
            g_object_set(sms, "max-size-bytes", static_cast<guint64>(m_dvr_max_size), NULL);
            gst_object_unref(sms);
        }
    }
}

void GstRtpReceiver::dvr_request_recording(bool on) {
    m_dvr_want.store(on, std::memory_order_relaxed);
}

gchar* GstRtpReceiver::dvr_format_location(GstElement* splitmux, guint fragment_id, gpointer) {
    const char* base = static_cast<const char*>(g_object_get_data(G_OBJECT(splitmux), "dvr-base"));
    if (!base || !base[0]) base = "/tmp/pixelpilot_record";
    // First fragment keeps the plain name; size-splits get _partN (N from 2), to
    // match the previous minimp4 recorder's split naming.
    if (fragment_id == 0) return g_strdup_printf("%s.mp4", base);
    return g_strdup_printf("%s_part%u.mp4", base, fragment_id + 1);
}

void GstRtpReceiver::dvr_tick() {
    const bool want = m_dvr_want.load(std::memory_order_relaxed)
                      && m_gst_pipeline != nullptr && !m_file_playback;
    const bool active = (m_dvr_rec_bin != nullptr);
    if (want && !active) {
        dvr_add_record_bin();
    } else if (!want && active) {
        dvr_remove_record_bin();
    }
    dvr_reenc_tick();
}

void GstRtpReceiver::dvr_add_record_bin() {
    if (!m_gst_pipeline) return;
    GstElement* tee = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "rtp_tee");
    if (!tee) { spdlog::warn("[DVR] no rtp_tee in pipeline; cannot record"); return; }

    std::string base;
    int64_t max_bytes;
    {
        std::lock_guard<std::mutex> lk(m_dvr_cfg_mutex);
        if (m_dvr_base_path_fn) base = m_dvr_base_path_fn();
        max_bytes = m_dvr_max_size;
    }
    if (base.empty()) {
        spdlog::error("[DVR] no output path resolved; recording disabled");
        m_dvr_want.store(false, std::memory_order_relaxed); // avoid per-tick retry spam
        gst_object_unref(tee);
        return;
    }

    const bool h265 = (m_video_codec == VideoCodec::H265);
    // Only mux an audio track when Opus is actually flowing: recording an empty
    // audio track (--audio on but the air sends none) yields an unplayable mp4.
    const bool audio = m_audio_active && audio_recently_seen();
    // Native video (no re-encode) + muxed Opus (when active) into one mp4 via
    // splitmuxsink. A leading queue on each input decouples the record branch
    // from the tee so a slow disk cannot back-pressure and stall the live video.
    std::stringstream ss;
    ss << "queue name=dvr_vq max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! "
       << pipeline::gst_rtp_video_caps_fields(m_video_codec) << " ! "
       << (h265 ? "rtph265depay" : "rtph264depay") << " name=dvr_vdepay ! "
       << (h265 ? "h265parse" : "h264parse") << " config-interval=-1 ! "
       << "splitmuxsink name=dvr_sms muxer=mp4mux max-size-bytes=" << static_cast<guint64>(max_bytes);
    if (audio) {
        // capssetter (ANY sink template) hands Opus RTP caps to the depayloader
        // without constraining the tee; a PT probe drops non-audio packets.
        ss << " queue name=dvr_aq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 ! "
           << "capssetter replace=true caps=\"application/x-rtp, media=(string)audio, clock-rate=(int)48000,"
           << " encoding-name=(string)OPUS, payload=(int)" << m_audio_pt << "\" ! "
           << "rtpopusdepay name=dvr_adepay ! opusparse ! dvr_sms.audio_0";
    }

    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(ss.str().c_str(), FALSE, &err);
    if (!bin) {
        spdlog::error("[DVR] failed to build record bin: {}", err ? err->message : "unknown");
        if (err) g_error_free(err);
        gst_object_unref(tee);
        m_dvr_want.store(false, std::memory_order_relaxed);
        return;
    }
    gst_bin_add(GST_BIN(m_gst_pipeline), bin);
    // Contain the record bin's async state change so adding it to the live
    // pipeline can't back-pressure/stall it. Without this, starting a second
    // recorder (BOTH mode) deadlocks both splitmuxsinks (they sleep forever on an
    // undefined running time and the udpsrc stalls).
    g_object_set(bin, "async-handling", TRUE, NULL);

    // Custom split filenames from the resolved base path (freed with the sink).
    GstElement* sms = gst_bin_get_by_name(GST_BIN(bin), "dvr_sms");
    if (sms) {
        g_object_set(sms, "async-handling", TRUE, NULL);
        g_object_set_data_full(G_OBJECT(sms), "dvr-base", g_strdup(base.c_str()), g_free);
        g_signal_connect(sms, "format-location", G_CALLBACK(&GstRtpReceiver::dvr_format_location), nullptr);
        gst_object_unref(sms);
    }

    // Payload-type demux for the record branch when audio is muxed into the flow.
    if (audio) {
        const uint8_t pt = static_cast<uint8_t>(m_audio_pt);
        attach_pt_filter(m_gst_pipeline, "dvr_vdepay", pt, /*drop_when_match=*/true);
        attach_pt_filter(m_gst_pipeline, "dvr_adepay", pt, /*drop_when_match=*/false);
    }

    // Expose the input queue sinks as ghost pads so the tee can link into the bin.
    GstPad* gv = dvr_ghost_sink(bin, "dvr_vq", "vsink");
    GstPad* ga = audio ? dvr_ghost_sink(bin, "dvr_aq", "asink") : nullptr;

    // Sync to PLAYING BEFORE linking to the live tee: linking a still-NULL branch
    // makes the tee push into it and the udpsrc pauses with FLUSHING (kills video).
    if (!gst_element_sync_state_with_parent(bin)) {
        spdlog::error("[DVR] record bin failed to reach PLAYING; aborting record");
        if (gv) gst_object_unref(gv);
        if (ga) gst_object_unref(ga);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_gst_pipeline), bin);
        gst_object_unref(tee);
        m_dvr_want.store(false, std::memory_order_relaxed);
        return;
    }

    m_dvr_tee_video_pad = dvr_request_tee_pad(tee);
    if (gv) gst_pad_link(m_dvr_tee_video_pad, gv);
    if (ga) {
        m_dvr_tee_audio_pad = dvr_request_tee_pad(tee);
        gst_pad_link(m_dvr_tee_audio_pad, ga);
    }
    if (gv) gst_object_unref(gv);
    if (ga) gst_object_unref(ga);
    gst_object_unref(tee);

    m_dvr_rec_bin = bin;
    m_dvr_active.store(true, std::memory_order_relaxed);
    spdlog::info("[DVR] recording -> {}.mp4 (video={}, audio={}, split={}MB)",
                 base, h265 ? "H.265" : "H.264", audio ? "opus" : "none",
                 max_bytes / 1000000);

    // Ask the air side for a keyframe so the first fragment opens promptly
    // (splitmuxsink starts a file only on a keyframe boundary).
    idr_request_record_start();
}

void GstRtpReceiver::dvr_remove_record_bin() {
    if (!m_dvr_rec_bin) return;
    GstElement* bin = m_dvr_rec_bin;
    GstElement* tee = m_gst_pipeline ? gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "rtp_tee") : nullptr;

    GstPad* gv = gst_element_get_static_pad(bin, "vsink");
    GstPad* ga = gst_element_get_static_pad(bin, "asink");

    if (m_dvr_tee_video_pad && gv) gst_pad_unlink(m_dvr_tee_video_pad, gv);
    if (m_dvr_tee_audio_pad && ga) gst_pad_unlink(m_dvr_tee_audio_pad, ga);
    if (tee && m_dvr_tee_video_pad) gst_element_release_request_pad(tee, m_dvr_tee_video_pad);
    if (tee && m_dvr_tee_audio_pad) gst_element_release_request_pad(tee, m_dvr_tee_audio_pad);

    // Finalize: EOS the bin inputs so the splitmuxsink writes the mp4 moov, then
    // wait (bounded) for THIS stop's fragment-closed. Stale fragment-closed
    // messages from earlier size-splits are drained first so we don't stop early.
    GstBus* bus = m_gst_pipeline ? gst_element_get_bus(m_gst_pipeline) : nullptr;
    dvr_drain_element_messages(bus);
    if (gv) gst_pad_send_event(gv, gst_event_new_eos());
    if (ga) gst_pad_send_event(ga, gst_event_new_eos());
    dvr_wait_fragment_closed(bus);
    if (bus) gst_object_unref(bus);

    if (gv) gst_object_unref(gv);
    if (ga) gst_object_unref(ga);

    gst_element_set_state(bin, GST_STATE_NULL);
    if (m_gst_pipeline) gst_bin_remove(GST_BIN(m_gst_pipeline), bin);

    if (m_dvr_tee_video_pad) { gst_object_unref(m_dvr_tee_video_pad); m_dvr_tee_video_pad = nullptr; }
    if (m_dvr_tee_audio_pad) { gst_object_unref(m_dvr_tee_audio_pad); m_dvr_tee_audio_pad = nullptr; }
    if (tee) gst_object_unref(tee);
    m_dvr_rec_bin = nullptr;
    m_dvr_active.store(false, std::memory_order_relaxed);
    spdlog::info("[DVR] recording stopped");
}

// --- Re-encode record branch (appsrc video + Opus) --------------------------

void GstRtpReceiver::dvr_reenc_set_config(VideoCodec codec, int64_t max_size_bytes,
                                          std::function<std::string()> base_path_fn) {
    std::lock_guard<std::mutex> lk(m_dvr_reenc_cfg_mutex);
    m_dvr_reenc_codec = codec;
    m_dvr_reenc_max_size = max_size_bytes > 0 ? max_size_bytes : 0;
    m_dvr_reenc_base_path_fn = std::move(base_path_fn);
    if (m_dvr_reenc_bin) {
        GstElement* sms = gst_bin_get_by_name(GST_BIN(m_dvr_reenc_bin), "dvr_reenc_sms");
        if (sms) {
            g_object_set(sms, "max-size-bytes", static_cast<guint64>(m_dvr_reenc_max_size), NULL);
            gst_object_unref(sms);
        }
    }
}

void GstRtpReceiver::set_dvr_reenc_on_start(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(m_dvr_reenc_cfg_mutex);
    m_dvr_reenc_on_start = std::move(cb);
}

void GstRtpReceiver::dvr_reenc_request_recording(bool on) {
    m_dvr_reenc_want.store(on, std::memory_order_relaxed);
}

void GstRtpReceiver::dvr_reenc_roll() {
    m_dvr_reenc_roll_pending.store(true, std::memory_order_relaxed);
}

void GstRtpReceiver::dvr_reenc_push(std::shared_ptr<std::vector<uint8_t>> nal) {
    if (!nal || nal->empty()) return;
    std::lock_guard<std::mutex> lk(m_dvr_reenc_src_mutex);
    if (!m_dvr_reenc_appsrc) return;  // not recording (yet); drop
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, nal->size(), nullptr);
    gst_buffer_fill(buf, 0, nal->data(), nal->size());
    if (gst_app_src_push_buffer(GST_APP_SRC(m_dvr_reenc_appsrc), buf) != GST_FLOW_OK) {
        // The branch is being torn down or blocked; drop quietly.
    }
}

void GstRtpReceiver::dvr_reenc_tick() {
    const bool want = m_dvr_reenc_want.load(std::memory_order_relaxed)
                      && m_gst_pipeline != nullptr && !m_file_playback;
    const bool active = (m_dvr_reenc_bin != nullptr);
    // A pending roll (codec/resolution/fps change) tears the current file down;
    // the next tick re-opens with the new config if recording is still wanted.
    if (active && m_dvr_reenc_roll_pending.exchange(false, std::memory_order_relaxed)) {
        dvr_remove_reenc_bin();
        return;
    }
    if (want && !active) {
        dvr_add_reenc_bin();
    } else if (!want && active) {
        dvr_remove_reenc_bin();
    }
}

void GstRtpReceiver::dvr_add_reenc_bin() {
    if (!m_gst_pipeline) return;
    GstElement* tee = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "rtp_tee");
    if (!tee) { spdlog::warn("[DVR/reenc] no rtp_tee; cannot record"); return; }

    std::string base;
    int64_t max_bytes;
    VideoCodec codec;
    std::function<void()> on_start;
    {
        std::lock_guard<std::mutex> lk(m_dvr_reenc_cfg_mutex);
        if (m_dvr_reenc_base_path_fn) base = m_dvr_reenc_base_path_fn();
        max_bytes = m_dvr_reenc_max_size;
        codec = m_dvr_reenc_codec;
        on_start = m_dvr_reenc_on_start;
    }
    if (base.empty()) {
        spdlog::error("[DVR/reenc] no output path resolved; recording disabled");
        m_dvr_reenc_want.store(false, std::memory_order_relaxed);
        gst_object_unref(tee);
        return;
    }

    const bool h265 = (codec == VideoCodec::H265);
    // Only mux audio when Opus is actually flowing — an empty audio track (--audio
    // on but the air sends none) makes the mp4 unplayable.
    const bool audio = m_audio_active && audio_recently_seen();
    // Video is pushed in from the encoder thread (do-timestamp => live mux). Audio
    // taps the same tee/Opus as the raw recorder.
    std::stringstream ss;
    ss << "appsrc name=dvr_reenc_src is-live=true do-timestamp=true format=time ! "
       << (h265 ? "h265parse" : "h264parse") << " config-interval=-1 ! "
       << "splitmuxsink name=dvr_reenc_sms muxer=mp4mux max-size-bytes=" << static_cast<guint64>(max_bytes);
    if (audio) {
        ss << " queue name=dvr_reenc_aq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 ! "
           << "capssetter replace=true caps=\"application/x-rtp, media=(string)audio, clock-rate=(int)48000,"
           << " encoding-name=(string)OPUS, payload=(int)" << m_audio_pt << "\" ! "
           << "rtpopusdepay name=dvr_reenc_adepay ! opusparse ! dvr_reenc_sms.audio_0";
    }

    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(ss.str().c_str(), FALSE, &err);
    if (!bin) {
        spdlog::error("[DVR/reenc] failed to build record bin: {}", err ? err->message : "unknown");
        if (err) g_error_free(err);
        gst_object_unref(tee);
        m_dvr_reenc_want.store(false, std::memory_order_relaxed);
        return;
    }
    gst_bin_add(GST_BIN(m_gst_pipeline), bin);
    // Contain the record bin's async state change (see dvr_add_record_bin) so
    // coexisting with the raw recorder in BOTH mode doesn't deadlock.
    g_object_set(bin, "async-handling", TRUE, NULL);

    // appsrc caps + config (byte-stream elementary video from the MPP encoder).
    GstElement* src = gst_bin_get_by_name(GST_BIN(bin), "dvr_reenc_src");
    GstCaps* caps = gst_caps_new_simple(h265 ? "video/x-h265" : "video/x-h264",
                                        "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au", NULL);
    g_object_set(src, "caps", caps, "max-bytes", (guint64)(8 * 1024 * 1024), "block", FALSE, NULL);
    gst_caps_unref(caps);

    // Custom split filenames from the resolved base path (freed with the sink).
    GstElement* sms = gst_bin_get_by_name(GST_BIN(bin), "dvr_reenc_sms");
    if (sms) {
        g_object_set(sms, "async-handling", TRUE, NULL);
        g_object_set_data_full(G_OBJECT(sms), "dvr-base", g_strdup(base.c_str()), g_free);
        g_signal_connect(sms, "format-location", G_CALLBACK(&GstRtpReceiver::dvr_format_location), nullptr);
        gst_object_unref(sms);
    }

    // Audio branch: keep non-audio-PT packets off the Opus depayloader.
    GstPad* ga = nullptr;
    if (audio) {
        attach_pt_filter(m_gst_pipeline, "dvr_reenc_adepay", static_cast<uint8_t>(m_audio_pt), /*drop_when_match=*/false);
        ga = dvr_ghost_sink(bin, "dvr_reenc_aq", "asink");
    }

    // Sync to PLAYING before linking the audio tee pad (same FLUSHING guard as raw).
    if (!gst_element_sync_state_with_parent(bin)) {
        spdlog::error("[DVR/reenc] record bin failed to reach PLAYING; aborting record");
        if (ga) gst_object_unref(ga);
        if (src) gst_object_unref(src);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_gst_pipeline), bin);
        gst_object_unref(tee);
        m_dvr_reenc_want.store(false, std::memory_order_relaxed);
        return;
    }
    if (ga) {
        m_dvr_reenc_tee_audio_pad = dvr_request_tee_pad(tee);
        gst_pad_link(m_dvr_reenc_tee_audio_pad, ga);
        gst_object_unref(ga);
    }
    gst_object_unref(tee);

    // Publish the appsrc for the encoder thread's dvr_reenc_push() (transfers ref).
    {
        std::lock_guard<std::mutex> lk(m_dvr_reenc_src_mutex);
        m_dvr_reenc_appsrc = src;
    }
    m_dvr_reenc_bin = bin;
    m_dvr_reenc_active.store(true, std::memory_order_relaxed);
    spdlog::info("[DVR/reenc] recording -> {}.mp4 (video={}, audio={}, split={}MB)",
                 base, h265 ? "H.265" : "H.264", audio ? "opus" : "none", max_bytes / 1000000);

    idr_request_record_start();          // air-side keyframe
    if (on_start) on_start();            // encoder-side keyframe (open first fragment)
}

void GstRtpReceiver::dvr_remove_reenc_bin() {
    if (!m_dvr_reenc_bin) return;
    GstElement* bin = m_dvr_reenc_bin;
    GstElement* tee = m_gst_pipeline ? gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "rtp_tee") : nullptr;

    // Stop the encoder feed first: null the appsrc so dvr_reenc_push() drops.
    GstElement* src = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_dvr_reenc_src_mutex);
        src = m_dvr_reenc_appsrc;
        m_dvr_reenc_appsrc = nullptr;
    }

    GstPad* ga = gst_element_get_static_pad(bin, "asink");
    if (m_dvr_reenc_tee_audio_pad && ga) gst_pad_unlink(m_dvr_reenc_tee_audio_pad, ga);
    if (tee && m_dvr_reenc_tee_audio_pad) gst_element_release_request_pad(tee, m_dvr_reenc_tee_audio_pad);

    // Finalize: EOS the appsrc (video) + audio ghost, wait for fragment-closed.
    GstBus* bus = m_gst_pipeline ? gst_element_get_bus(m_gst_pipeline) : nullptr;
    dvr_drain_element_messages(bus);
    if (src) gst_app_src_end_of_stream(GST_APP_SRC(src));
    if (ga) gst_pad_send_event(ga, gst_event_new_eos());
    dvr_wait_fragment_closed(bus);
    if (bus) gst_object_unref(bus);

    if (ga) gst_object_unref(ga);
    if (src) gst_object_unref(src);      // release the ref held in m_dvr_reenc_appsrc

    gst_element_set_state(bin, GST_STATE_NULL);
    if (m_gst_pipeline) gst_bin_remove(GST_BIN(m_gst_pipeline), bin);

    if (m_dvr_reenc_tee_audio_pad) { gst_object_unref(m_dvr_reenc_tee_audio_pad); m_dvr_reenc_tee_audio_pad = nullptr; }
    if (tee) gst_object_unref(tee);
    m_dvr_reenc_bin = nullptr;
    m_dvr_reenc_active.store(false, std::memory_order_relaxed);
    spdlog::info("[DVR/reenc] recording stopped");
}

void idr_set_enabled(bool enabled) {
    g_idr_enabled.store(enabled, std::memory_order_relaxed);
}

bool idr_get_enabled() {
    return g_idr_enabled.load(std::memory_order_relaxed);
}

void restream_set_enabled(bool enabled) {
    g_restream_enabled.store(enabled, std::memory_order_relaxed);
    update_restream_valve(enabled);
}

bool restream_get_enabled() {
    return g_restream_enabled.load(std::memory_order_relaxed);
}

void restream_scan_clients(char* buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return;
    }

    const auto ips = scan_hotspot_clients();
    std::string manual_ip;
    std::string pinned_ip;
    {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        manual_ip = g_restream_manual_ip;
        pinned_ip = g_restream_pinned_ip;
    }

    std::string combined = "Auto";
    for (const auto& ip : ips) {
        combined += '\n';
        combined += ip;
    }
    // Always include the pinned IP from config so it stays in the list
    // regardless of whether the user currently has it selected.
    if (!pinned_ip.empty() && !contains_ip(ips, pinned_ip)) {
        combined += '\n';
        combined += pinned_ip;
    }
    // Also include the active manual IP if it differs from the pinned/default ones.
    if (!manual_ip.empty() && manual_ip != "Auto" && manual_ip != pinned_ip
            && !contains_ip(ips, manual_ip)) {
        combined += '\n';
        combined += manual_ip;
    }
    strncpy(buf, combined.c_str(), buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void restream_set_manual_ip(const char* ip) {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    g_restream_manual_ip = (ip && ip[0] != '\0' && strcmp(ip, "Auto") != 0) ? ip : "";
    g_restream_target_ip.clear(); // force retarget on next probe
}

void restream_set_pinned_ip(const char* ip) {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    g_restream_pinned_ip = (ip && ip[0] != '\0') ? ip : "";
}

const char* restream_get_manual_ip() {
    std::lock_guard<std::mutex> lock(g_restream_mutex);
    static char buf[64];
    strncpy(buf, g_restream_manual_ip.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
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
