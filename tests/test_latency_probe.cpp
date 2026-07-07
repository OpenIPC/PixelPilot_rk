#include <catch2/catch.hpp>
#include <cstdint>
#include <vector>

#include "../src/latency_probe.hpp"
#include "../src/latency_probe_wire.hpp"

namespace lp = latency_probe;

namespace {
// Helper: build a synthetic RTP header with the given fields.
// Returns a 12-byte vector.
std::vector<uint8_t> make_header(uint8_t version, bool marker,
                                 uint32_t timestamp, uint32_t ssrc) {
    std::vector<uint8_t> b(12, 0);
    b[0] = (version & 0x03) << 6;            // V=version, P=0, X=0, CC=0
    b[1] = (marker ? 0x80 : 0x00) | 0x60;    // M + PT=96 (dyn)
    b[2] = 0; b[3] = 0;                      // seq
    b[4] = (timestamp >> 24) & 0xff;
    b[5] = (timestamp >> 16) & 0xff;
    b[6] = (timestamp >> 8)  & 0xff;
    b[7] = (timestamp     )  & 0xff;
    b[8]  = (ssrc >> 24) & 0xff;
    b[9]  = (ssrc >> 16) & 0xff;
    b[10] = (ssrc >> 8)  & 0xff;
    b[11] = (ssrc     )  & 0xff;
    return b;
}
}

TEST_CASE("RTP header parse: marker=1, ssrc, timestamp extracted",
          "[latency_probe][rtp]") {
    auto h = make_header(/*version=*/2, /*marker=*/true,
                         /*ts=*/0x11223344u, /*ssrc=*/0xDEADBEEFu);

    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.marker == true);
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
}

TEST_CASE("RTP header parse: marker=0 still parses but flags off",
          "[latency_probe][rtp]") {
    auto h = make_header(2, false, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.marker == false);
    REQUIRE(info.timestamp == 1u);
    REQUIRE(info.ssrc == 2u);
}

TEST_CASE("RTP header parse: wrong version rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(/*version=*/3, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), h.size(), info));
}

TEST_CASE("RTP header parse: non-zero CSRC count still parses (CC bits ignored)",
          "[latency_probe][rtp]") {
    // The parser only reads bytes 0..11. CSRCs sit AFTER byte 11, so they
    // do not affect us — but we should prove that a CC>0 packet still
    // parses correctly. Real RTP packets do sometimes carry CSRCs.
    auto h = make_header(2, true, 0x11223344u, 0xDEADBEEFu);
    h[0] |= 0x03;  // CC = 3 (three CSRC entries would follow if we read further)
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
    REQUIRE(info.marker == true);
}

TEST_CASE("RTP header parse: extension bit set still parses",
          "[latency_probe][rtp]") {
    // Extension header sits after CSRC list; the fixed 12-byte prefix
    // is unaffected.
    auto h = make_header(2, true, 0x11223344u, 0xDEADBEEFu);
    h[0] |= 0x10;  // X bit
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
    REQUIRE(info.marker == true);
}

TEST_CASE("RTP header parse: too short rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(2, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), 11, info));
    REQUIRE_FALSE(lp::parse_rtp_header(nullptr, 0, info));
}

TEST_CASE("ClockOffset: returns 0/0 before any sample",
          "[latency_probe][clock]") {
    lp::ClockOffset c;
    int64_t off = 999;
    uint64_t rtt = 999;
    c.get(off, rtt);
    REQUIRE(off == 0);
    REQUIRE(rtt == 0);
}

TEST_CASE("ClockOffset: picks min-RTT sample",
          "[latency_probe][clock]") {
    lp::ClockOffset c;

    // Sample 1: drone is +1000us ahead, RTT=200us
    //   t1=1000, t2=1500 (drone), t3=1600 (drone), t4=1200
    //   rtt = (t4-t1) - (t3-t2) = 200 - 100 = 100   (NOTE: small RTT here)
    //   offset = ((t2-t1) + (t3-t4))/2 = (500 + 400)/2 = 450
    c.add_sample(1000, 1500, 1600, 1200);

    // Sample 2: same offset, but with extra queueing in one direction.
    //   t1=2000, t2=2500 (true offset still ~500), t3=2600, t4=2800
    //   rtt = 800 - 100 = 700
    //   offset = (500 + -200)/2 = 150  (BIASED by queueing)
    c.add_sample(2000, 2500, 2600, 2800);

    int64_t off = 0; uint64_t rtt = 0;
    c.get(off, rtt);
    REQUIRE(rtt == 100);   // min-RTT picked
    REQUIRE(off == 450);   // its offset, not the biased one
}

TEST_CASE("ClockOffset: rescans after ring eviction of current best",
          "[latency_probe][clock]") {
    lp::ClockOffset c;

    // First sample becomes best (RTT=0).
    c.add_sample(0, 100, 110, 10);    // rtt = 10-0 - 10 = 0  (use generous arith)
    int64_t off; uint64_t rtt;
    c.get(off, rtt);
    auto best_rtt_before = rtt;
    auto best_off_before = off;

    // Fill 15 more samples with much larger RTT so the first stays best.
    for (int i = 0; i < 15; ++i) {
        uint64_t base = 1000ull + i * 100ull;
        // Big asymmetry -> big RTT.
        c.add_sample(base, base + 5000, base + 5100, base + 10000);
    }
    c.get(off, rtt);
    REQUIRE(rtt == best_rtt_before);
    REQUIRE(off == best_off_before);

    // Add one more -- the original best gets evicted (ring=16, this is the 17th).
    // The new best should be from the remaining 16 samples.
    c.add_sample(20'000, 25'000, 25'100, 30'000);
    c.get(off, rtt);
    REQUIRE(rtt != best_rtt_before);   // the original is gone
}

TEST_CASE("FrameMatcher: arrival then sidecar publishes once",
          "[latency_probe][matcher]") {
    lp::FrameMatcher m;
    std::vector<lp::FrameTimings> published;
    m.set_publish_callback([&](const lp::FrameTimings& f){ published.push_back(f); });

    constexpr uint32_t ssrc = 0xABCDu;
    constexpr uint32_t rtp_ts = 90000u;

    m.on_marker_arrival(ssrc, rtp_ts, /*gs_recv_us=*/100'000, /*now=*/100'000);
    // Sidecar arrives second — should trigger publish.
    m.on_msg_frame(ssrc, rtp_ts,
                   /*capture_us=*/  50'000,
                   /*frame_ready_us=*/ 90'000,
                   /*last_pkt_send_us=*/95'000,
                   /*now=*/130'000);

    REQUIRE(published.size() == 1);
    REQUIRE(published[0].ssrc == ssrc);
    REQUIRE(published[0].rtp_ts == rtp_ts);
    REQUIRE(published[0].gs_recv_last_us == 100'000);
    REQUIRE(published[0].capture_us == 50'000);
    REQUIRE(published[0].frame_ready_us == 90'000);
    REQUIRE(published[0].last_pkt_send_us == 95'000);
}

TEST_CASE("FrameMatcher: sidecar before arrival also works",
          "[latency_probe][matcher]") {
    lp::FrameMatcher m;
    std::vector<lp::FrameTimings> published;
    m.set_publish_callback([&](const lp::FrameTimings& f){ published.push_back(f); });

    m.on_msg_frame(1u, 100u, 50'000, 90'000, 95'000, /*now=*/96'000);
    REQUIRE(published.empty());
    m.on_marker_arrival(1u, 100u, 100'000, 100'000);
    REQUIRE(published.size() == 1);
    REQUIRE(published[0].gs_recv_last_us == 100'000);
}

TEST_CASE("FrameMatcher: TTL evicts orphans",
          "[latency_probe][matcher]") {
    lp::FrameMatcher m;
    std::vector<lp::FrameTimings> published;
    m.set_publish_callback([&](const lp::FrameTimings& f){ published.push_back(f); });

    // Arrival without ever getting a MSG_FRAME — orphan.
    m.on_marker_arrival(1u, 100u, 1'000, /*now=*/1'000);
    // Sweep with strict-> eviction: need (now - inserted) > ttl, so
    // 1us past the boundary at 500'000us-since-insert is the minimum.
    m.ttl_sweep(/*now=*/501'001, /*ttl_us=*/500'000);
    REQUIRE(m.size() == 0);

    // Subsequent frame completes normally.
    m.on_marker_arrival(1u, 200u, 600'000, 600'000);
    m.on_msg_frame(1u, 200u, 0, 599'000, 599'500, 603'000);

    REQUIRE(published.size() == 1);
    REQUIRE(published[0].rtp_ts == 200u);
}

TEST_CASE("FrameMatcher: ring cap discards oldest",
          "[latency_probe][matcher]") {
    lp::FrameMatcher m;
    std::vector<lp::FrameTimings> published;
    m.set_publish_callback([&](const lp::FrameTimings& f){ published.push_back(f); });

    // Push 200 arrivals — cap should hold at 64 (kRingCap).
    for (uint32_t i = 0; i < 200; ++i) {
        m.on_marker_arrival(1u, i, i * 1000ull, i * 1000ull);
    }
    REQUIRE(m.size() == lp::FrameMatcher::kRingCap);
}

TEST_CASE("FrameMatcher: duplicate marker arrival is a no-op stamp",
          "[latency_probe][matcher]") {
    lp::FrameMatcher m;
    std::vector<lp::FrameTimings> published;
    m.set_publish_callback([&](const lp::FrameTimings& f){ published.push_back(f); });

    m.on_marker_arrival(1u, 100u, 1'000, 1'000);
    // Duplicate marker for the same frame must not overwrite the recorded time.
    m.on_marker_arrival(1u, 100u, 2'000, 2'000);

    m.on_msg_frame(1u, 100u, 0, 900, 950, 5'000);

    REQUIRE(published.size() == 1);
    REQUIRE(published[0].gs_recv_last_us == 1'000);
}

TEST_CASE("compute_and_publish: normal values flow through",
          "[latency_probe][publish]") {
    lp::PublishedFacts captured;
    auto cb = [&](const char* name, uint64_t v) {
        captured.uint_facts.emplace_back(name, v);
    };
    auto cbi = [&](const char* name, int64_t v) {
        captured.int_facts.emplace_back(name, v);
    };

    lp::FrameTimings f{};
    f.ssrc = 1; f.rtp_ts = 100;
    f.capture_us = 1'000;
    f.frame_ready_us = 11'000;
    f.last_pkt_send_us = 14'000;
    f.gs_recv_last_us = 14'500;
    f.sidecar_seen = true;

    int64_t offset_us = -1'000;
    uint64_t rtt_us   = 400;
    uint64_t gs_pipeline_ms = 35;  // matches video.decode_and_handover_ms
    uint64_t clamp_counter = 0;
    lp::compute_and_publish(f, offset_us, rtt_us, gs_pipeline_ms,
                            clamp_counter, cb, cbi);

    auto get = [&](const char* name) -> uint64_t {
        for (auto& [n, v] : captured.uint_facts) if (n == name) return v;
        FAIL("missing fact " << name);
        return 0;
    };

    REQUIRE(get("video.latency.capture_to_encode_ms") == 10);
    REQUIRE(get("video.latency.capture_to_encode_us") == 10'000);
    REQUIRE(get("video.latency.encode_to_send_ms")    == 3);
    REQUIRE(get("video.latency.encode_to_send_us")    == 3'000);
    REQUIRE(get("video.latency.wire_ms")              == 0);
    REQUIRE(clamp_counter == 1);
    // total_ms = capture(10) + encode(3) + wire(0) + gs_pipeline(35) = 48
    REQUIRE(get("video.latency.total_ms")             == 48);
    REQUIRE(get("video.latency.clock_rtt_us")         == 400);
    REQUIRE(get("video.latency.wire_clamp_count")     == 1);

    // decode_ms and display_ms were removed in the refactor.
    for (auto& [n, _] : captured.uint_facts) {
        REQUIRE(n != "video.latency.decode_ms");
        REQUIRE(n != "video.latency.display_ms");
    }

    bool found_offset = false;
    for (auto& [n, v] : captured.int_facts) {
        if (n == "video.latency.clock_offset_us") {
            REQUIRE(v == -1'000);
            found_offset = true;
        }
    }
    REQUIRE(found_offset);
}

TEST_CASE("compute_and_publish: capture_us == 0 skips capture_to_encode but still publishes total",
          "[latency_probe][publish]") {
    lp::PublishedFacts captured;
    auto cb  = [&](const char* n, uint64_t v){ captured.uint_facts.emplace_back(n, v); };
    auto cbi = [&](const char* n, int64_t v){ captured.int_facts.emplace_back(n, v); };

    lp::FrameTimings f{};
    f.capture_us = 0;
    f.frame_ready_us = 11'000;
    f.last_pkt_send_us = 14'000;
    f.gs_recv_last_us = 15'000;
    f.sidecar_seen = true;

    uint64_t clamp = 0;
    // rtt_us=1 (any nonzero) signals "sync acquired" — the focus of this
    // test is the capture_us=0 branch, not the pre-sync gate.
    lp::compute_and_publish(f, 0, 1, /*gs_pipeline_ms=*/40, clamp, cb, cbi);

    for (auto& [n, _] : captured.uint_facts) {
        REQUIRE(n != "video.latency.capture_to_encode_ms");
        REQUIRE(n != "video.latency.capture_to_encode_us");
    }
    bool saw_wire = false, saw_send_us = false, saw_total = false;
    uint64_t total = 0;
    for (auto& [n, v] : captured.uint_facts) {
        if (n == "video.latency.wire_ms")           saw_wire = true;
        if (n == "video.latency.encode_to_send_us") saw_send_us = true;
        if (n == "video.latency.total_ms") { saw_total = true; total = v; }
    }
    REQUIRE(saw_wire);
    REQUIRE(saw_send_us);
    REQUIRE(saw_total);
    // total = encode(3) + wire(1) + gs_pipeline(40) = 44
    REQUIRE(total == 44);
}

TEST_CASE("compute_and_publish: pre-sync (rtt_us=0) suppresses wire_ms and total_ms",
          "[latency_probe][publish]") {
    // Regression guard for the cold-start race: before the first
    // MSG_SYNC_RESP lands, ClockOffset::get() returns (offset=0, rtt=0).
    // Without a valid offset, gs_recv_last_us (GS CLOCK_MONOTONIC) minus
    // last_pkt_send_us (drone CLOCK_MONOTONIC) is (GS_uptime - drone_uptime)
    // — unbounded, often huge. Field symptom: total_ms publishes absurd
    // values until sync converges. Guard against republishing this bug.
    lp::PublishedFacts captured;
    auto cb  = [&](const char* n, uint64_t v){ captured.uint_facts.emplace_back(n, v); };
    auto cbi = [&](const char* n, int64_t v){ captured.int_facts.emplace_back(n, v); };

    lp::FrameTimings f{};
    f.ssrc = 1; f.rtp_ts = 100;
    f.capture_us       = 1'000;        // drone clock
    f.frame_ready_us   = 11'000;       // drone clock
    f.last_pkt_send_us = 14'000;       // drone clock
    // GS clock is millions of µs away from drone clock — the exact divergence
    // we'd see if both machines have been up for different durations.
    f.gs_recv_last_us  = 9'999'999'000ull;
    f.sidecar_seen     = true;

    uint64_t clamp = 0;
    lp::compute_and_publish(f, /*offset_us=*/0, /*rtt_us=*/0,
                            /*gs_pipeline_ms=*/35, clamp, cb, cbi);

    auto has_uint = [&](const char* name) {
        for (auto& [n, _] : captured.uint_facts) if (n == name) return true;
        return false;
    };

    // Drone-local segments are safe to publish without sync.
    REQUIRE(has_uint("video.latency.capture_to_encode_ms"));
    REQUIRE(has_uint("video.latency.capture_to_encode_us"));
    REQUIRE(has_uint("video.latency.encode_to_send_ms"));
    REQUIRE(has_uint("video.latency.encode_to_send_us"));
    // Clock diagnostics still publish — seeing rtt=0 is itself signal.
    REQUIRE(has_uint("video.latency.clock_rtt_us"));
    REQUIRE(has_uint("video.latency.wire_clamp_count"));

    // wire_ms and total_ms MUST be suppressed pre-sync.
    for (auto& [n, _] : captured.uint_facts) {
        REQUIRE(n != "video.latency.wire_ms");
        REQUIRE(n != "video.latency.total_ms");
    }
    // wire_clamp_counter must not tick on bogus pre-sync deltas.
    REQUIRE(clamp == 0);
}

TEST_CASE("wire: encode_subscribe round-trip header bytes",
          "[latency_probe][wire]") {
    uint8_t buf[lp::wire::kSizeSubscribe] = {};
    lp::encode_subscribe(buf);
    REQUIRE(buf[0] == 0x52);
    REQUIRE(buf[1] == 0x54);
    REQUIRE(buf[2] == 0x50);
    REQUIRE(buf[3] == 0x53);
    REQUIRE(buf[4] == 1);   // version
    REQUIRE(buf[5] == 1);   // msg_type = SUBSCRIBE
}

TEST_CASE("wire: encode_sync_req carries t1 big-endian",
          "[latency_probe][wire]") {
    uint8_t buf[lp::wire::kSizeSyncReq] = {};
    lp::encode_sync_req(buf, 0x0102030405060708ull);
    REQUIRE(buf[5] == 3);  // SYNC_REQ
    REQUIRE(buf[8]  == 0x01);
    REQUIRE(buf[9]  == 0x02);
    REQUIRE(buf[10] == 0x03);
    REQUIRE(buf[11] == 0x04);
    REQUIRE(buf[12] == 0x05);
    REQUIRE(buf[13] == 0x06);
    REQUIRE(buf[14] == 0x07);
    REQUIRE(buf[15] == 0x08);
}

TEST_CASE("wire: decode_message rejects bad magic / version / length",
          "[latency_probe][wire]") {
    uint8_t buf[64] = {};
    lp::SyncRespFields sr{};
    lp::MsgFrameFields mf{};
    REQUIRE(lp::decode_message(buf, 0, sr, mf)  == 0);
    buf[0]=0; buf[1]=0; buf[2]=0; buf[3]=0; buf[4]=1; buf[5]=4;
    REQUIRE(lp::decode_message(buf, lp::wire::kSizeSyncResp, sr, mf) == 0);
    buf[0]=0x52; buf[1]=0x54; buf[2]=0x50; buf[3]=0x53; buf[4]=99; buf[5]=4;
    REQUIRE(lp::decode_message(buf, lp::wire::kSizeSyncResp, sr, mf) == 0);
    // (d) null pointer
    REQUIRE(lp::decode_message(nullptr, 32, sr, mf) == 0);
    // (e) unknown msg_type
    buf[0]=0x52; buf[1]=0x54; buf[2]=0x50; buf[3]=0x53; buf[4]=1; buf[5]=0x99;
    REQUIRE(lp::decode_message(buf, sizeof(buf), sr, mf) == 0);
    // (f) sub-message too short — valid magic+version+kMsgSyncResp but len < kSizeSyncResp
    buf[0]=0x52; buf[1]=0x54; buf[2]=0x50; buf[3]=0x53; buf[4]=1; buf[5]=lp::wire::kMsgSyncResp;
    REQUIRE(lp::decode_message(buf, lp::wire::kSizeSyncResp - 1, sr, mf) == 0);
}

TEST_CASE("wire: decode_message reads SYNC_RESP fields big-endian",
          "[latency_probe][wire]") {
    uint8_t buf[lp::wire::kSizeSyncResp] = {};
    buf[0]=0x52; buf[1]=0x54; buf[2]=0x50; buf[3]=0x53;
    buf[4]=1; buf[5]=lp::wire::kMsgSyncResp;
    uint64_t t1 = 0xAABBCCDDEEFF0011ull;
    for (int i = 0; i < 8; ++i) buf[8+i] = (t1 >> (56 - 8*i)) & 0xff;
    buf[23] = 100;
    buf[31] = 200;

    lp::SyncRespFields sr{};
    lp::MsgFrameFields mf{};
    REQUIRE(lp::decode_message(buf, sizeof(buf), sr, mf) == lp::wire::kMsgSyncResp);
    REQUIRE(sr.t1_us == 0xAABBCCDDEEFF0011ull);
    REQUIRE(sr.t2_us == 100);
    REQUIRE(sr.t3_us == 200);
}

TEST_CASE("wire: decode_message reads MSG_FRAME fields",
          "[latency_probe][wire]") {
    uint8_t buf[lp::wire::kSizeFrame] = {};
    buf[0]=0x52; buf[1]=0x54; buf[2]=0x50; buf[3]=0x53;
    buf[4]=1; buf[5]=lp::wire::kMsgFrame;
    uint32_t ssrc = 0xDEADBEEFu;
    for (int i=0;i<4;++i) buf[8+i] = (ssrc >> (24-8*i)) & 0xff;
    uint32_t rtp_ts = 0x11223344u;
    for (int i=0;i<4;++i) buf[12+i] = (rtp_ts >> (24-8*i)) & 0xff;
    auto write_be64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) buf[off + i] = (v >> (56 - 8 * i)) & 0xff;
    };
    write_be64(24, 0xAABBCCDDEEFF1122ull);  // frame_ready_us
    write_be64(36, 0x0102030405060708ull);  // capture_us
    write_be64(44, 0xFEDCBA9876543210ull);  // last_pkt_send_us

    lp::SyncRespFields sr{};
    lp::MsgFrameFields mf{};
    REQUIRE(lp::decode_message(buf, sizeof(buf), sr, mf) == lp::wire::kMsgFrame);
    REQUIRE(mf.ssrc == ssrc);
    REQUIRE(mf.rtp_timestamp == rtp_ts);
    REQUIRE(mf.frame_ready_us == 0xAABBCCDDEEFF1122ull);
    REQUIRE(mf.capture_us == 0x0102030405060708ull);
    REQUIRE(mf.last_pkt_send_us == 0xFEDCBA9876543210ull);
}
