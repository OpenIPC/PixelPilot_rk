#ifndef LATENCY_PROBE_HPP
#define LATENCY_PROBE_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>   // size_t
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace latency_probe {

struct RtpHeaderInfo {
    uint32_t ssrc;
    uint32_t timestamp;
    bool marker;
};

// Parses the fixed 12-byte RTP header (no CSRC, no extension).
// Returns true and fills info on success, false on short buffer or
// version != 2. Does NOT validate CSRC count or extension — we only
// need ssrc/timestamp/marker which all sit in the fixed prefix.
bool parse_rtp_header(const uint8_t* data, size_t len, RtpHeaderInfo& info);

// Cheap-to-check gate for hot paths. Loaded with acquire memory order;
// changes only at start()/stop() which the application drives at startup
// and shutdown.
extern std::atomic<bool> active;

// Lifecycle. host is the drone's IP, port is the sidecar UDP port
// (typical default 5602). Returns true on success.
// Safe to call when already started (no-op). Safe to call with active==false.
bool start(const std::string& host, uint16_t port);
void stop();

// Hot-path hook: called from the GStreamer pad probe for every RTP
// packet received on the video udpsrc. data points at the raw RTP packet
// (header + payload); len is the full packet length. Implementation
// parses the header and acts only on packets with marker=1.
// No-op when active==false.
void on_rtp_buffer(const uint8_t* data, size_t len, uint64_t gs_recv_us);

// Hot-path hook: called from the display thread after a real video-frame
// commit (NOT on OSD-only redraws), passing the value already computed
// for the existing `video.decode_and_handover_ms` fact. Stored as the
// "GS pipeline" component of the per-frame total.
// No-op when active==false.
void record_gs_pipeline_ms(uint64_t gs_pipeline_ms);

// Returns a CLOCK_MONOTONIC sample in microseconds. Convenience for callers.
uint64_t now_us();

class ClockOffset {
public:
    // t1: probe send time (GS clock, us)
    // t2: drone recv time (drone clock, us)
    // t3: drone reply send time (drone clock, us)
    // t4: probe recv time (GS clock, us)
    void add_sample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);

    // Out-params for current best offset/rtt. Returns 0/0 until at
    // least one sample is recorded.
    // Convention: GS_clock + offset_us  ==  drone_clock.
    void get(int64_t& offset_us, uint64_t& rtt_us) const;

private:
    struct Sample {
        bool      valid;
        int64_t   offset_us;
        uint64_t  rtt_us;
        uint64_t  taken_us;   // wall-monotonic timestamp of insertion
    };
    static constexpr size_t kRing = 16;

    mutable std::mutex m_;
    std::array<Sample, kRing> samples_{};
    size_t  next_ = 0;
    int64_t best_offset_us_ = 0;
    uint64_t best_rtt_us_   = 0;
    bool    have_best_      = false;
};

struct FrameTimings {
    uint32_t ssrc           = 0;
    uint32_t rtp_ts         = 0;
    uint64_t gs_recv_last_us     = 0;  // pad probe (marker=1)
    uint64_t capture_us          = 0;  // from MSG_FRAME (drone clock)
    uint64_t frame_ready_us      = 0;  // from MSG_FRAME (drone clock)
    uint64_t last_pkt_send_us    = 0;  // from MSG_FRAME (drone clock)
    bool     sidecar_seen        = false;
    uint64_t inserted_us         = 0;  // for TTL
};

// Pairs RTP marker-bit arrivals (from the pad probe) with MSG_FRAME
// (from the sidecar) by (ssrc, rtp_timestamp). The two halves can arrive
// in either order. A slot publishes when both are present. Older orphans
// are aged out by ttl_sweep.
class FrameMatcher {
public:
    using PublishFn = std::function<void(const FrameTimings&)>;
    static constexpr size_t kRingCap = 64;

    void set_publish_callback(PublishFn cb);

    // All entry points take 'now' separately so tests can drive time
    // deterministically. In production, callers pass latency_probe::now_us().
    void on_marker_arrival(uint32_t ssrc, uint32_t rtp_ts,
                           uint64_t gs_recv_us, uint64_t now);
    void on_msg_frame(uint32_t ssrc, uint32_t rtp_ts,
                      uint64_t capture_us, uint64_t frame_ready_us,
                      uint64_t last_pkt_send_us, uint64_t now);

    // Evict slots older than ttl_us.
    void ttl_sweep(uint64_t now, uint64_t ttl_us);

    size_t size() const;

private:
    mutable std::mutex m_;
    std::deque<FrameTimings> ring_;
    PublishFn publish_;

    // Caller holds m_.
    FrameTimings* find_by_key_locked(uint32_t ssrc, uint32_t rtp_ts);
    FrameTimings& push_new_locked(uint64_t now);
    void try_publish_for_locked(FrameTimings& slot);  // publishes + erases
                                                      // the given slot if
                                                      // complete.
};

// Test scaffolding: captured-facts vector used by tests in place of
// the real osd_publish_*_fact calls.
struct PublishedFacts {
    std::vector<std::pair<std::string, uint64_t>> uint_facts;
    std::vector<std::pair<std::string, int64_t>>  int_facts;
};

using PublishUintFn = std::function<void(const char* name, uint64_t value)>;
using PublishIntFn  = std::function<void(const char* name, int64_t  value)>;

// Test-only: override the OSD publish path with custom callbacks. Pass
// nullptr/nullptr to revert to the real osd_publish_*_fact functions.
// Has effect only on subsequent publishes.
void set_publish_overrides_for_test(PublishUintFn pub_u, PublishIntFn pub_i);

// Compute all video.latency.* values from one matched FrameTimings + the
// current clock offset/rtt + the latest GS-pipeline ms (from the existing
// `video.decode_and_handover_ms` fact, fed via record_gs_pipeline_ms), and
// emit via the provided callbacks. Mutates wire_clamp_counter if the
// computed wire delta would have been negative.
//
// capture_us == 0 means the drone did not provide a PTS-derived capture
// time; in that case capture_to_encode_ms / capture_to_encode_us are
// suppressed.
//
// rtt_us == 0 is the "no sync yet" sentinel from ClockOffset::get(). In
// that state wire_ms and total_ms are suppressed (cross-clock arithmetic
// would be meaningless) and wire_clamp_counter is not incremented. The
// drone-local segments and the clock diagnostic facts (clock_offset_us,
// clock_rtt_us=0, wire_clamp_count) still publish so the OSD can see that
// sync hasn't converged.
//
// total_ms = capture_to_encode_ms + encode_to_send_ms + wire_ms + gs_pipeline_ms.
// Published only when sync is available (any other missing summand
// contributes 0).
void compute_and_publish(const FrameTimings& f,
                         int64_t  offset_us,
                         uint64_t rtt_us,
                         uint64_t  gs_pipeline_ms,
                         uint64_t& wire_clamp_counter,
                         PublishUintFn pub_u,
                         PublishIntFn  pub_i);

} // namespace latency_probe

#endif // LATENCY_PROBE_HPP
