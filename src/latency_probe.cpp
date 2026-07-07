#include "latency_probe.hpp"
#include "latency_probe_wire.hpp"
#include "osd.h"
#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <thread>

namespace latency_probe {

std::atomic<bool> active{false};

namespace {

PublishUintFn g_pub_u_override;
PublishIntFn  g_pub_i_override;

// Module-internal state. There is one global instance protected by g_start_mu;
// the thread itself does not need additional locking against g_state — only
// the application driver calls start()/stop().
struct ProbeState {
    std::thread        thread;
    std::atomic<bool>  stop_flag{false};
    int                fd = -1;
    sockaddr_in        peer{};
    ClockOffset        clock;
    FrameMatcher       matcher;
    uint64_t           wire_clamp_count = 0;
    uint64_t           started_us = 0;
};

std::mutex      g_start_mu;
ProbeState*     g_state = nullptr;

// Latest "decode_and_handover" reading from the existing display-thread
// fact, fed in via record_gs_pipeline_ms(). Read by the publish callback
// at frame-publish time. Relaxed ordering is fine — the value is a smooth
// per-frame number; transient staleness across the GS pipeline pairing is
// acceptable.
std::atomic<uint64_t> g_gs_pipeline_ms{0};

void publish_uint_real(const char* name, uint64_t value) {
    osd_publish_uint_fact(name, NULL, 0, static_cast<unsigned long>(value));
}
void publish_int_real(const char* name, int64_t value) {
    osd_publish_int_fact(name, NULL, 0, static_cast<long>(value));
}

void send_subscribe(ProbeState& s) {
    uint8_t buf[wire::kSizeSubscribe];
    encode_subscribe(buf);
    sendto(s.fd, buf, sizeof(buf), 0,
           reinterpret_cast<sockaddr*>(&s.peer), sizeof(s.peer));
}

void send_sync_req(ProbeState& s) {
    uint8_t buf[wire::kSizeSyncReq];
    uint64_t t1 = now_us();
    encode_sync_req(buf, t1);
    sendto(s.fd, buf, sizeof(buf), 0,
           reinterpret_cast<sockaddr*>(&s.peer), sizeof(s.peer));
}

void handle_packet(ProbeState& s, const uint8_t* buf, size_t len) {
    SyncRespFields sr{};
    MsgFrameFields mf{};
    uint8_t kind = decode_message(buf, len, sr, mf);
    switch (kind) {
        case wire::kMsgSyncResp: {
            uint64_t t4 = now_us();
            s.clock.add_sample(sr.t1_us, sr.t2_us, sr.t3_us, t4);
            break;
        }
        case wire::kMsgFrame: {
            s.matcher.on_msg_frame(mf.ssrc, mf.rtp_timestamp,
                                   mf.capture_us, mf.frame_ready_us,
                                   mf.last_pkt_send_us,
                                   /*now=*/now_us());
            break;
        }
        default: break;
    }
}

void probe_thread_main(ProbeState* sp) {
    ProbeState& s = *sp;
    constexpr uint64_t kSubInterval_us  = 2'000'000;
    constexpr uint64_t kSyncFast_us     = 1'000'000;
    constexpr uint64_t kSyncSlow_us     = 5'000'000;
    constexpr uint64_t kFastWindow_us   = 10'000'000;
    constexpr uint64_t kTtlSweepStep_us = 100'000;
    constexpr uint64_t kTtl_us          = 500'000;

    s.started_us = now_us();
    uint64_t next_subscribe = s.started_us;
    uint64_t next_sync      = s.started_us;
    uint64_t next_sweep     = s.started_us + kTtlSweepStep_us;

    // Wire publish callback. The matcher holds its own mutex while calling
    // back, so this runs under that mutex — keep it cheap.
    s.matcher.set_publish_callback(
        [&](const FrameTimings& f) {
            int64_t  off; uint64_t rtt;
            s.clock.get(off, rtt);
            uint64_t gs_pipe = g_gs_pipeline_ms.load(std::memory_order_relaxed);
            PublishUintFn pu = g_pub_u_override ? g_pub_u_override : publish_uint_real;
            PublishIntFn  pi = g_pub_i_override ? g_pub_i_override : publish_int_real;
            compute_and_publish(f, off, rtt, gs_pipe, s.wire_clamp_count, pu, pi);
        });

    pollfd pfd{ s.fd, POLLIN, 0 };

    while (!s.stop_flag.load(std::memory_order_relaxed)) {
        uint64_t now = now_us();

        if (now >= next_subscribe) {
            send_subscribe(s);
            next_subscribe = now + kSubInterval_us;
        }
        if (now >= next_sync) {
            send_sync_req(s);
            uint64_t step = (now - s.started_us < kFastWindow_us)
                ? kSyncFast_us : kSyncSlow_us;
            next_sync = now + step;
        }
        if (now >= next_sweep) {
            s.matcher.ttl_sweep(now, kTtl_us);
            next_sweep = now + kTtlSweepStep_us;
        }

        uint64_t soonest = std::min({next_subscribe, next_sync, next_sweep});
        int timeout_ms = (soonest > now) ? static_cast<int>((soonest - now) / 1000ull) : 0;
        if (timeout_ms < 1) timeout_ms = 1;
        if (timeout_ms > 100) timeout_ms = 100;

        int prc = poll(&pfd, 1, timeout_ms);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 8; ++i) {
                uint8_t buf[128];
                ssize_t n = recv(s.fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) break;
                handle_packet(s, buf, static_cast<size_t>(n));
            }
        }
    }
}

bool resolve_peer(const std::string& host, uint16_t port, sockaddr_in& out) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    auto* a = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out.sin_addr = a->sin_addr;
    freeaddrinfo(res);
    return true;
}

} // namespace

bool start(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lk(g_start_mu);
    if (g_state) return true;
    if (port == 0 || host.empty()) {
        spdlog::warn("[latency-probe] disabled: empty host/port");
        return false;
    }

    auto s = std::make_unique<ProbeState>();
    if (!resolve_peer(host, port, s->peer)) {
        spdlog::warn("[latency-probe] failed to resolve {}:{}", host, port);
        return false;
    }

    s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s->fd < 0) {
        spdlog::warn("[latency-probe] socket: {}", strerror(errno));
        return false;
    }
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(s->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        spdlog::warn("[latency-probe] fcntl O_NONBLOCK: {}", strerror(errno));
        close(s->fd);
        return false;
    }

    g_state = s.release();
    g_state->thread = std::thread(probe_thread_main, g_state);
    active.store(true, std::memory_order_release);
    spdlog::info("[latency-probe] started → {}:{}", host, port);
    return true;
}

void stop() {
    ProbeState* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_start_mu);
        s = g_state;
        g_state = nullptr;
    }
    if (!s) return;
    active.store(false, std::memory_order_release);
    s->stop_flag.store(true, std::memory_order_release);
    if (s->thread.joinable()) s->thread.join();
    if (s->fd >= 0) close(s->fd);
    delete s;
}

// g_state is read without a lock in the hot-path hooks. The `active` atomic
// is the gate: it's flipped to `true` AFTER g_state is fully initialized
// (release store at end of start()), and flipped to `false` BEFORE g_state
// is torn down (release store at the top of stop()). The hot-path acquire
// load synchronizes-with that release, making the prior write to g_state
// visible without a lock.
void on_rtp_buffer(const uint8_t* data, size_t len, uint64_t gs_recv_us) {
    if (!active.load(std::memory_order_acquire)) return;
    ProbeState* s = g_state;
    if (!s) return;
    RtpHeaderInfo h;
    if (!parse_rtp_header(data, len, h)) return;
    if (!h.marker) return;
    s->matcher.on_marker_arrival(h.ssrc, h.timestamp, gs_recv_us, gs_recv_us);
}

void record_gs_pipeline_ms(uint64_t gs_pipeline_ms) {
    if (!active.load(std::memory_order_acquire)) return;
    g_gs_pipeline_ms.store(gs_pipeline_ms, std::memory_order_relaxed);
}

bool parse_rtp_header(const uint8_t* data, size_t len, RtpHeaderInfo& info) {
    if (!data || len < 12) return false;
    if ((data[0] >> 6) != 2) return false;  // version must be 2

    info.marker    = (data[1] & 0x80) != 0;
    info.timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                     (static_cast<uint32_t>(data[5]) << 16) |
                     (static_cast<uint32_t>(data[6]) << 8)  |
                     (static_cast<uint32_t>(data[7]));
    info.ssrc      = (static_cast<uint32_t>(data[8])  << 24) |
                     (static_cast<uint32_t>(data[9])  << 16) |
                     (static_cast<uint32_t>(data[10]) << 8)  |
                     (static_cast<uint32_t>(data[11]));
    return true;
}

uint64_t now_us() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1'000ull;
}

void ClockOffset::add_sample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    // rtt_us = (t4 - t1) - (t3 - t2). Both subtractions can be negative
    // in pathological samples (out-of-order delivery, clock jumps). In
    // that case we treat rtt as huge so this sample never wins.
    uint64_t rtt;
    if (t4 >= t1 && t3 >= t2 && (t4 - t1) >= (t3 - t2))
        rtt = (t4 - t1) - (t3 - t2);
    else
        rtt = std::numeric_limits<uint64_t>::max();

    // offset_us = ((t2 - t1) + (t3 - t4)) / 2, signed arithmetic.
    int64_t off = ((static_cast<int64_t>(t2) - static_cast<int64_t>(t1)) +
                   (static_cast<int64_t>(t3) - static_cast<int64_t>(t4))) / 2;

    std::lock_guard<std::mutex> lk(m_);
    Sample& slot = samples_[next_];
    bool overwriting_best =
        have_best_ && slot.valid &&
        slot.rtt_us == best_rtt_us_ && slot.offset_us == best_offset_us_;

    slot.valid     = true;
    slot.offset_us = off;
    slot.rtt_us    = rtt;
    slot.taken_us  = 0; // not used; kept for future drift handling
    next_ = (next_ + 1) % kRing;

    if (!have_best_ || rtt < best_rtt_us_) {
        best_rtt_us_    = rtt;
        best_offset_us_ = off;
        have_best_      = true;
        return;
    }
    if (overwriting_best) {
        // Re-scan the ring to pick the new minimum.
        uint64_t new_best_rtt = std::numeric_limits<uint64_t>::max();
        int64_t  new_best_off = 0;
        bool     any = false;
        for (const auto& s : samples_) {
            if (s.valid && s.rtt_us < new_best_rtt) {
                new_best_rtt = s.rtt_us;
                new_best_off = s.offset_us;
                any = true;
            }
        }
        have_best_      = any;
        best_rtt_us_    = any ? new_best_rtt : 0;
        best_offset_us_ = any ? new_best_off : 0;
    }
}

void ClockOffset::get(int64_t& offset_us, uint64_t& rtt_us) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!have_best_) { offset_us = 0; rtt_us = 0; return; }
    offset_us = best_offset_us_;
    rtt_us    = best_rtt_us_;
}

static bool is_complete(const FrameTimings& f) {
    return f.sidecar_seen && f.gs_recv_last_us != 0;
}

void FrameMatcher::set_publish_callback(PublishFn cb) {
    std::lock_guard<std::mutex> lk(m_);
    publish_ = std::move(cb);
}

size_t FrameMatcher::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return ring_.size();
}

FrameTimings* FrameMatcher::find_by_key_locked(uint32_t ssrc, uint32_t rtp_ts) {
    for (auto& f : ring_) {
        if (f.ssrc == ssrc && f.rtp_ts == rtp_ts) return &f;
    }
    return nullptr;
}

FrameTimings& FrameMatcher::push_new_locked(uint64_t now) {
    if (ring_.size() >= kRingCap) ring_.pop_front();
    ring_.emplace_back();
    ring_.back().inserted_us = now;
    return ring_.back();
}

void FrameMatcher::try_publish_for_locked(FrameTimings& slot) {
    if (!is_complete(slot)) return;
    if (publish_) publish_(slot);
    for (auto it = ring_.begin(); it != ring_.end(); ++it) {
        if (&(*it) == &slot) { ring_.erase(it); return; }
    }
}

void FrameMatcher::on_marker_arrival(uint32_t ssrc, uint32_t rtp_ts,
                                     uint64_t gs_recv_us, uint64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    FrameTimings* slot = find_by_key_locked(ssrc, rtp_ts);
    if (slot) {
        if (slot->gs_recv_last_us == 0) slot->gs_recv_last_us = gs_recv_us;
    } else {
        slot = &push_new_locked(now);
        slot->ssrc = ssrc;
        slot->rtp_ts = rtp_ts;
        slot->gs_recv_last_us = gs_recv_us;
    }
    try_publish_for_locked(*slot);
}

void FrameMatcher::on_msg_frame(uint32_t ssrc, uint32_t rtp_ts,
                                uint64_t capture_us, uint64_t frame_ready_us,
                                uint64_t last_pkt_send_us, uint64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    FrameTimings* slot = find_by_key_locked(ssrc, rtp_ts);
    if (!slot) {
        slot = &push_new_locked(now);
        slot->ssrc = ssrc;
        slot->rtp_ts = rtp_ts;
    }
    slot->capture_us        = capture_us;
    slot->frame_ready_us    = frame_ready_us;
    slot->last_pkt_send_us  = last_pkt_send_us;
    slot->sidecar_seen      = true;
    try_publish_for_locked(*slot);
}

void FrameMatcher::ttl_sweep(uint64_t now, uint64_t ttl_us) {
    std::lock_guard<std::mutex> lk(m_);
    while (!ring_.empty() && now - ring_.front().inserted_us > ttl_us) {
        ring_.pop_front();
    }
}

void compute_and_publish(const FrameTimings& f,
                         int64_t  offset_us,
                         uint64_t rtt_us,
                         uint64_t  gs_pipeline_ms,
                         uint64_t& wire_clamp_counter,
                         PublishUintFn pub_u,
                         PublishIntFn  pub_i) {
    bool have_capture = f.capture_us != 0;
    // rtt_us == 0 is ClockOffset::get()'s sentinel for "no sync sample yet".
    // Without an offset, gs_recv_last_us (GS CLOCK_MONOTONIC) minus
    // last_pkt_send_us (drone CLOCK_MONOTONIC) is (GS_uptime − drone_uptime)
    // — two independent boot clocks, often off by hours. Publishing wire_ms
    // before sync causes the field-observed "absurdly high latency at startup
    // then settles to ~50 ms" symptom; suppress until first MSG_SYNC_RESP.
    bool have_sync = rtt_us != 0;

    uint64_t cap_to_enc_us = 0;
    uint64_t cap_to_enc_ms = 0;
    uint64_t enc_to_send_us = 0;
    uint64_t enc_to_send_ms = 0;
    if (have_capture && f.frame_ready_us >= f.capture_us) {
        cap_to_enc_us = f.frame_ready_us - f.capture_us;
        cap_to_enc_ms = cap_to_enc_us / 1000ull;
    }
    if (f.last_pkt_send_us >= f.frame_ready_us) {
        enc_to_send_us = f.last_pkt_send_us - f.frame_ready_us;
        enc_to_send_ms = enc_to_send_us / 1000ull;
    }

    if (have_capture) {
        pub_u("video.latency.capture_to_encode_ms", cap_to_enc_ms);
        pub_u("video.latency.capture_to_encode_us", cap_to_enc_us);
    }
    pub_u("video.latency.encode_to_send_ms", enc_to_send_ms);
    pub_u("video.latency.encode_to_send_us", enc_to_send_us);

    if (have_sync) {
        int64_t adjusted_send_us =
            static_cast<int64_t>(f.last_pkt_send_us) - offset_us;
        int64_t wire_us = static_cast<int64_t>(f.gs_recv_last_us) - adjusted_send_us;
        if (wire_us < 0) {
            wire_us = 0;
            wire_clamp_counter++;
        }
        uint64_t wire_ms = static_cast<uint64_t>(wire_us) / 1000ull;
        pub_u("video.latency.wire_ms", wire_ms);
        pub_u("video.latency.total_ms",
              cap_to_enc_ms + enc_to_send_ms + wire_ms + gs_pipeline_ms);
    }

    pub_i("video.latency.clock_offset_us",   offset_us);
    pub_u("video.latency.clock_rtt_us",      rtt_us);
    pub_u("video.latency.wire_clamp_count",  wire_clamp_counter);
}

void set_publish_overrides_for_test(PublishUintFn pub_u, PublishIntFn pub_i) {
    g_pub_u_override = std::move(pub_u);
    g_pub_i_override = std::move(pub_i);
}

namespace {

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8)  & 0xff;
    p[3] = (v)       & 0xff;
}

inline void write_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (v >> (56 - 8 * i)) & 0xff;
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline uint64_t read_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

} // namespace

void encode_subscribe(uint8_t* out) {
    write_be32(out, wire::kMagic);
    out[4] = wire::kVersion;
    out[5] = wire::kMsgSubscribe;
    out[6] = 0;
    out[7] = 0;
}

void encode_sync_req(uint8_t* out, uint64_t t1_us) {
    write_be32(out, wire::kMagic);
    out[4] = wire::kVersion;
    out[5] = wire::kMsgSyncReq;
    out[6] = 0;
    out[7] = 0;
    write_be64(out + 8, t1_us);
}

uint8_t decode_message(const uint8_t* in, size_t len,
                       SyncRespFields& sr, MsgFrameFields& mf) {
    if (!in || len < 6) return 0;
    if (read_be32(in) != wire::kMagic) return 0;
    if (in[4] != wire::kVersion) return 0;

    uint8_t msg = in[5];
    switch (msg) {
        case wire::kMsgSyncResp:
            if (len < wire::kSizeSyncResp) return 0;
            sr.t1_us = read_be64(in + 8);
            sr.t2_us = read_be64(in + 16);
            sr.t3_us = read_be64(in + 24);
            return msg;

        case wire::kMsgFrame:
            if (len < wire::kSizeFrame) return 0;
            mf.ssrc             = read_be32(in + 8);
            mf.rtp_timestamp    = read_be32(in + 12);
            mf.frame_ready_us   = read_be64(in + 24);
            mf.capture_us       = read_be64(in + 36);
            mf.last_pkt_send_us = read_be64(in + 44);
            return msg;

        default:
            return 0;
    }
}

} // namespace latency_probe
