#include "mpp_encoder.h"

#include <pthread.h>

#include "spdlog/spdlog.h"

MppEncoder::MppEncoder(MppEncoderParams p, FrameCallback cb)
    : params(p), output_cb(cb) {}

MppEncoder::~MppEncoder() {
    cleanup_encoder();
}

void MppEncoder::push_frame(MppBuffer buffer,
                             uint32_t width, uint32_t height,
                             uint32_t hor_stride, uint32_t ver_stride,
                             MppFrameFormat fmt, uint64_t pts) {
    EncRpc rpc;
    rpc.command    = EncRpc::RPC_FRAME;
    rpc.buffer     = buffer;
    rpc.width      = width;
    rpc.height     = height;
    rpc.hor_stride = hor_stride;
    rpc.ver_stride = ver_stride;
    rpc.fmt        = fmt;
    rpc.pts        = pts;
    enqueue(std::move(rpc));
}

void MppEncoder::request_idr() {
    EncRpc rpc;
    rpc.command = EncRpc::RPC_IDR;
    enqueue(std::move(rpc));
}

void MppEncoder::shutdown() {
    EncRpc rpc;
    rpc.command = EncRpc::RPC_SHUTDOWN;
    enqueue(std::move(rpc));
}

void MppEncoder::enqueue(EncRpc rpc) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(std::move(rpc));
    }
    cv.notify_one();
}

void *MppEncoder::__THREAD__(void *param) {
    pthread_setname_np(pthread_self(), "__ENCODER");
    ((MppEncoder *)param)->loop();
    return nullptr;
}

void MppEncoder::loop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !queue.empty(); });

        EncRpc rpc = std::move(queue.front());
        queue.pop();
        lock.unlock();

        switch (rpc.command) {
        case EncRpc::RPC_IDR:
            idr_pending = true;
            spdlog::debug("Encoder IDR pending (will apply to next frame)");
            break;

        case EncRpc::RPC_FRAME:
            encode_frame(rpc);
            break;

        case EncRpc::RPC_SHUTDOWN:
            // Release any buffer that was in-flight at shutdown
            if (rpc.buffer) {
                mpp_buffer_put(rpc.buffer);
            }
            goto end;
        }
    }
end:
    cleanup_encoder();
    spdlog::info("Encoder thread done.");
}

bool MppEncoder::init_encoder(uint32_t width, uint32_t height,
                               uint32_t hor_stride, uint32_t ver_stride,
                               MppFrameFormat fmt) {
    cleanup_encoder();

    MppCodingType coding = (params.codec == VideoCodec::H265)
                               ? MPP_VIDEO_CodingHEVC
                               : MPP_VIDEO_CodingAVC;

    if (mpp_create(&ctx, &mpi) != MPP_OK) {
        spdlog::error("MPP encoder: mpp_create failed");
        return false;
    }

    if (mpp_init(ctx, MPP_CTX_ENC, coding) != MPP_OK) {
        spdlog::error("MPP encoder: mpp_init failed");
        mpp_destroy(ctx);
        ctx = nullptr;
        return false;
    }

    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);

    mpp_enc_cfg_set_s32(cfg, "prep:width",      (int)width);
    mpp_enc_cfg_set_s32(cfg, "prep:height",     (int)height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", (int)hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", (int)ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format",     (int)fmt);

    int target_bps = params.bitrate_kbps * 1000;
    mpp_enc_cfg_set_s32(cfg, "rc:mode",       MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", target_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",    target_bps * 3 / 2);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",    target_bps / 2);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex",   0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",    params.fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom",  1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex",  0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",   params.fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",           params.fps * 2);

    if (params.codec == VideoCodec::H265) {
        mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingHEVC);
    } else {
        mpp_enc_cfg_set_s32(cfg, "codec:type",   MPP_VIDEO_CodingAVC);
        mpp_enc_cfg_set_s32(cfg, "h264:profile", 100); // High profile
        mpp_enc_cfg_set_s32(cfg, "h264:level",   51);
    }

    if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        spdlog::error("MPP encoder: MPP_ENC_SET_CFG failed");
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(ctx);
        ctx = nullptr;
        return false;
    }
    mpp_enc_cfg_deinit(cfg);

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode) != MPP_OK) {
        spdlog::warn("MPP encoder: MPP_ENC_SET_HEADER_MODE failed (recordings may lack SPS/PPS)");
    }

    enc_width      = width;
    enc_height     = height;
    enc_hor_stride = hor_stride;
    enc_ver_stride = ver_stride;
    initialized    = true;
    idr_pending    = true;

    spdlog::info("MPP encoder initialized: {}x{} @ {}fps {}kbps codec={}",
                 width, height, params.fps, params.bitrate_kbps,
                 params.codec == VideoCodec::H265 ? "h265" : "h264");
    return true;
}

void MppEncoder::cleanup_encoder() {
    if (ctx) {
        mpp_destroy(ctx);
        ctx = nullptr;
        mpi = nullptr;
    }
    initialized = false;
}

void MppEncoder::encode_frame(EncRpc &rpc) {
    // Init or re-init if resolution changed
    if (!initialized ||
        rpc.width != enc_width || rpc.height != enc_height) {
        if (!init_encoder(rpc.width, rpc.height,
                          rpc.hor_stride, rpc.ver_stride, rpc.fmt)) {
            mpp_buffer_put(rpc.buffer);
            rpc.buffer = nullptr;
            return;
        }
    }

    // Request IDR if pending (e.g. recording just started)
    if (idr_pending) {
        mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
        idr_pending = false;
    }

    // Zero-copy: build encoder input frame directly from the decoded DRM buffer
    MppFrame mpp_frame = nullptr;
    mpp_frame_init(&mpp_frame);
    mpp_frame_set_width(mpp_frame,      enc_width);
    mpp_frame_set_height(mpp_frame,     enc_height);
    mpp_frame_set_hor_stride(mpp_frame, enc_hor_stride);
    mpp_frame_set_ver_stride(mpp_frame, enc_ver_stride);
    mpp_frame_set_fmt(mpp_frame,        rpc.fmt);
    mpp_frame_set_buffer(mpp_frame,     rpc.buffer);
    mpp_frame_set_pts(mpp_frame,        rpc.pts);

    if (mpi->encode_put_frame(ctx, mpp_frame) != MPP_OK) {
        spdlog::warn("MPP encoder: encode_put_frame failed");
        mpp_frame_deinit(&mpp_frame);
        mpp_buffer_put(rpc.buffer);
        rpc.buffer = nullptr;
        return;
    }
    mpp_frame_deinit(&mpp_frame);

    // Synchronous: one frame in → one packet out.
    // encode_get_packet blocks until the VPU finishes reading the input buffer.
    MppPacket packet = nullptr;
    if (mpi->encode_get_packet(ctx, &packet) == MPP_OK && packet) {
        void *data = mpp_packet_get_pos(packet);
        size_t len  = mpp_packet_get_length(packet);
        if (len > 0 && output_cb) {
            output_cb(std::make_shared<std::vector<uint8_t>>(
                static_cast<uint8_t *>(data),
                static_cast<uint8_t *>(data) + len));
        }
        mpp_packet_deinit(&packet);
    }

    // Encoding done — release the extra ref taken in the frame thread
    mpp_buffer_put(rpc.buffer);
    rpc.buffer = nullptr;
}
