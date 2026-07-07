#include <catch2/catch.hpp>

#include "../src/latency_probe.hpp"
#include "../src/latency_probe_wire.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace lp = latency_probe;

namespace {

struct FakeWaybeam {
    int      fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread       th;
    std::atomic<int>  subscribe_count{0};
    std::atomic<int>  sync_responded{0};

    struct FrameSpec {
        uint32_t ssrc;
        uint32_t rtp_ts;
        uint64_t capture_us;
        uint64_t frame_ready_us;
        uint64_t last_pkt_send_us;
    };
    std::vector<FrameSpec> frames;
    std::atomic<size_t>    frames_emitted{0};

    bool start_listening() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); fd=-1; return false; }
        socklen_t alen = sizeof(addr);
        getsockname(fd, (sockaddr*)&addr, &alen);
        port = ntohs(addr.sin_port);
        return true;
    }

    void run() {
        sockaddr_in peer{};
        socklen_t   plen = sizeof(peer);
        bool        have_peer = false;

        while (!stop.load()) {
            uint8_t buf[128];
            ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                 (sockaddr*)&peer, &plen);
            if (n > 0) {
                have_peer = true;
                if (n >= (ssize_t)lp::wire::kSizeSubscribe &&
                    buf[5] == lp::wire::kMsgSubscribe) {
                    subscribe_count++;
                }
                else if (n >= (ssize_t)lp::wire::kSizeSyncReq &&
                         buf[5] == lp::wire::kMsgSyncReq) {
                    uint8_t out[lp::wire::kSizeSyncResp] = {};
                    out[0]=0x52;out[1]=0x54;out[2]=0x50;out[3]=0x53;
                    out[4]=1; out[5]=lp::wire::kMsgSyncResp;
                    memcpy(out+8, buf+8, 8);
                    uint64_t t2 = 10'000'000ull, t3 = 10'000'100ull;
                    for (int i=0;i<8;++i){ out[16+i]=(t2>>(56-8*i))&0xff; }
                    for (int i=0;i<8;++i){ out[24+i]=(t3>>(56-8*i))&0xff; }
                    sendto(fd, out, sizeof(out), 0, (sockaddr*)&peer, plen);
                    sync_responded++;
                }
            }

            if (have_peer && frames_emitted < frames.size()) {
                const auto& f = frames[frames_emitted++];
                uint8_t out[lp::wire::kSizeFrame] = {};
                out[0]=0x52;out[1]=0x54;out[2]=0x50;out[3]=0x53;
                out[4]=1; out[5]=lp::wire::kMsgFrame;
                auto wbe32=[&](size_t off, uint32_t v){
                    out[off]=(v>>24)&0xff; out[off+1]=(v>>16)&0xff;
                    out[off+2]=(v>>8)&0xff; out[off+3]=v&0xff;
                };
                auto wbe64=[&](size_t off, uint64_t v){
                    for (int i=0;i<8;++i) out[off+i]=(v>>(56-8*i))&0xff;
                };
                wbe32(8, f.ssrc);
                wbe32(12, f.rtp_ts);
                wbe64(24, f.frame_ready_us);
                wbe64(36, f.capture_us);
                wbe64(44, f.last_pkt_send_us);
                sendto(fd, out, sizeof(out), 0, (sockaddr*)&peer, plen);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

} // namespace

TEST_CASE("latency_probe: integration loopback publishes facts",
          "[latency_probe][integration]") {
    FakeWaybeam fw;
    REQUIRE(fw.start_listening());
    fw.frames = {
        {1u, 100u, 1'000, 11'000, 14'000},
        {1u, 200u, 2'000, 12'000, 15'000},
        {1u, 300u, 3'000, 13'000, 16'000},
    };
    fw.th = std::thread([&]{ fw.run(); });

    std::mutex captured_mu;
    std::vector<std::pair<std::string,uint64_t>> uint_facts;
    std::vector<std::pair<std::string,int64_t>>  int_facts;
    lp::set_publish_overrides_for_test(
        [&](const char* n, uint64_t v){
            std::lock_guard<std::mutex> lk(captured_mu); uint_facts.emplace_back(n,v);
        },
        [&](const char* n, int64_t v){
            std::lock_guard<std::mutex> lk(captured_mu); int_facts.emplace_back(n,v);
        });

    REQUIRE(lp::start("127.0.0.1", fw.port));

    // Wait for the first clock sync to land. compute_and_publish suppresses
    // wire_ms / total_ms until the offset is valid (rtt_us > 0), so without
    // this gate the first frames would publish only drone-local segments and
    // the wire_ms / total_ms assertions below would race. The probe sends
    // MSG_SYNC_REQ at startup; fake waybeam replies within a few ms on loopback.
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fw.sync_responded.load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(fw.sync_responded.load() >= 1);
        // Give the probe thread a beat to handle_packet the sync response.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Feed a synthetic GS-pipeline reading; total_ms uses this.
    lp::record_gs_pipeline_ms(35);

    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t rtp_ts = 100u * (i + 1);
        uint64_t base   = lp::now_us();
        // null/empty input is a no-op (parser rejects).
        lp::on_rtp_buffer(nullptr, 0, base);
        // Synthetic 12-byte RTP header for the parser path.
        uint8_t hdr[12] = {};
        hdr[0] = 0x80;                       // V=2
        hdr[1] = 0x80;                       // marker=1
        hdr[4]=(rtp_ts>>24)&0xff; hdr[5]=(rtp_ts>>16)&0xff;
        hdr[6]=(rtp_ts>>8)&0xff;  hdr[7]= rtp_ts &0xff;
        hdr[8]=0; hdr[9]=0; hdr[10]=0; hdr[11]=1;   // ssrc=1
        lp::on_rtp_buffer(hdr, sizeof(hdr), base);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::lock_guard<std::mutex> lk(captured_mu);
        size_t totals = 0;
        for (auto& [n, _] : uint_facts) if (n == "video.latency.total_ms") totals++;
        if (totals >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    lp::stop();
    lp::set_publish_overrides_for_test(nullptr, nullptr);
    fw.stop = true;
    if (fw.th.joinable()) fw.th.join();
    close(fw.fd);

    REQUIRE(fw.subscribe_count.load() >= 1);
    REQUIRE(fw.sync_responded.load() >= 1);

    std::lock_guard<std::mutex> lk(captured_mu);
    size_t totals = 0;
    for (auto& [n, _] : uint_facts) if (n == "video.latency.total_ms") totals++;
    REQUIRE(totals >= 3);

    bool saw_offset = false;
    for (auto& [n, v] : int_facts) {
        if (n == "video.latency.clock_offset_us") { saw_offset = true; break; }
    }
    REQUIRE(saw_offset);

    // Verify the spec-required facts are present. Names match
    // compute_and_publish's emit list after the refactor (decode_ms and
    // display_ms were removed; total now sums via gs_pipeline_ms).
    auto has_uint = [&](const char* name) {
        for (auto& [n, _] : uint_facts) if (n == name) return true;
        return false;
    };
    REQUIRE(has_uint("video.latency.capture_to_encode_ms"));
    REQUIRE(has_uint("video.latency.capture_to_encode_us"));
    REQUIRE(has_uint("video.latency.encode_to_send_ms"));
    REQUIRE(has_uint("video.latency.encode_to_send_us"));
    REQUIRE(has_uint("video.latency.wire_ms"));
    REQUIRE(has_uint("video.latency.total_ms"));
    REQUIRE(has_uint("video.latency.clock_rtt_us"));
    REQUIRE(has_uint("video.latency.wire_clamp_count"));
    // clock_offset_us is signed (int_facts) — already covered by saw_offset above.
    // decode_ms / display_ms were removed in the refactor; verify they're gone.
    for (auto& [n, _] : uint_facts) {
        REQUIRE(n != "video.latency.decode_ms");
        REQUIRE(n != "video.latency.display_ms");
    }
}
