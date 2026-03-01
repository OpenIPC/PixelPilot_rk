#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <stdint.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <functional>

#include <rockchip/rk_mpi.h>

#include "gstrtpreceiver.h"

struct MppEncoderParams {
    VideoCodec codec = VideoCodec::H264;
    int fps = 30;
    int bitrate_kbps = 8000;
};

struct EncRpc {
    enum {
        RPC_FRAME,
        RPC_IDR,
        RPC_SHUTDOWN
    } command;

    // For RPC_FRAME: decoded DRM buffer (caller has called mpp_buffer_inc_ref)
    MppBuffer buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hor_stride = 0;
    uint32_t ver_stride = 0;
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    uint64_t pts = 0;
};

class MppEncoder {
public:
    using FrameCallback = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;

    explicit MppEncoder(MppEncoderParams params, FrameCallback cb);
    ~MppEncoder();

    void push_frame(MppBuffer buffer,
                    uint32_t width, uint32_t height,
                    uint32_t hor_stride, uint32_t ver_stride,
                    MppFrameFormat fmt, uint64_t pts);
    void request_idr();
    void shutdown();

    VideoCodec get_codec() const { return params.codec; }

    static void *__THREAD__(void *context);

private:
    void loop();
    bool init_encoder(uint32_t width, uint32_t height,
                      uint32_t hor_stride, uint32_t ver_stride,
                      MppFrameFormat fmt);
    void cleanup_encoder();
    void encode_frame(EncRpc &rpc);
    void enqueue(EncRpc rpc);

private:
    MppEncoderParams params;
    FrameCallback output_cb;

    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;

    bool initialized = false;
    bool idr_pending = true;
    bool headers_sent = false;
    std::vector<uint8_t> extra_data; // VPS/SPS/PPS from MPP_ENC_GET_EXTRA_INFO
    uint32_t enc_width = 0;
    uint32_t enc_height = 0;
    uint32_t enc_hor_stride = 0;
    uint32_t enc_ver_stride = 0;

    std::queue<EncRpc> queue;
    std::mutex mtx;
    std::condition_variable cv;
};

#endif // MPP_ENCODER_H
