#ifndef LATENCY_PROBE_WIRE_HPP
#define LATENCY_PROBE_WIRE_HPP

#include <cstdint>
#include <cstddef>

namespace latency_probe {

// Mirrors waybeam_venc/include/rtp_sidecar.h. Keep in sync when the
// drone protocol bumps RTP_SIDECAR_VERSION.
namespace wire {

constexpr uint32_t kMagic   = 0x52545053u; // "RTPS"
constexpr uint8_t  kVersion = 1;

enum : uint8_t {
    kMsgSubscribe = 1,
    kMsgFrame     = 2,
    kMsgSyncReq   = 3,
    kMsgSyncResp  = 4,
};

constexpr uint8_t kFlagKeyframe       = 0x01;
constexpr uint8_t kFlagEncInfo        = 0x02;
constexpr uint8_t kFlagTransportInfo  = 0x04;

// All sizes match the packed structs on the wire (network byte order).
constexpr size_t kSizeSubscribe = 8;
constexpr size_t kSizeSyncReq   = 16;
constexpr size_t kSizeSyncResp  = 32;
constexpr size_t kSizeFrame     = 52;

} // namespace wire

struct SyncRespFields {
    uint64_t t1_us;
    uint64_t t2_us;
    uint64_t t3_us;
};

struct MsgFrameFields {
    uint32_t ssrc;
    uint32_t rtp_timestamp;
    uint64_t frame_ready_us;
    uint64_t capture_us;
    uint64_t last_pkt_send_us;
};

// Serialise SUBSCRIBE into out[0..kSizeSubscribe).
void encode_subscribe(uint8_t* out);

// Serialise SYNC_REQ with the given t1 (GS clock us) into out[0..kSizeSyncReq).
void encode_sync_req(uint8_t* out, uint64_t t1_us);

// Parse incoming UDP datagram. Returns the message type on success, or 0
// on any validation failure. Fills the appropriate out param.
uint8_t decode_message(const uint8_t* in, size_t len,
                       SyncRespFields& sr_out,
                       MsgFrameFields& mf_out);

} // namespace latency_probe

#endif // LATENCY_PROBE_WIRE_HPP
