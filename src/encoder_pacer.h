#ifndef ENCODER_PACER_H
#define ENCODER_PACER_H

#include <stdint.h>
#include <atomic>
#include <mutex>

#include <rockchip/rk_mpi.h>

#include "mpp_encoder.h"

// ---------------------------------------------------------------------------
// EncoderPacer: feeds MppEncoder at a steady fps regardless of incoming rate.
//
//  - Decoder thread calls push_latest() on every decoded frame; the pacer
//    keeps only the most recent one (dropping excess frames when source is
//    faster than the target fps).
//  - An internal timer thread wakes at target fps intervals and submits
//    the latest frame — or repeats the last frame if none arrived, preventing
//    a speedup effect when source fps < target fps.
// ---------------------------------------------------------------------------

struct EncPacerFrame {
    MppBuffer      buffer     = nullptr;
    uint32_t       width      = 0;
    uint32_t       height     = 0;
    uint32_t       hor_stride = 0;
    uint32_t       ver_stride = 0;
    MppFrameFormat fmt        = MPP_FMT_YUV420SP;
    void release() {
        if (buffer) { mpp_buffer_put(buffer); buffer = nullptr; }
    }
};

class EncoderPacer {
public:
    EncoderPacer(MppEncoder *enc, int fps);
    ~EncoderPacer();

    // Called from decoder thread: update the latest available frame.
    void push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                     uint32_t hs, uint32_t vs, MppFrameFormat fmt);

    void shutdown();

    static void *__THREAD__(void *p);

private:
    void loop();

    MppEncoder       *encoder;
    long              interval_ns;
    std::atomic<bool> running{true};
    std::mutex        mtx;
    EncPacerFrame     pending;    // latest from decoder (shared with decoder thread)

    // These are only accessed from the pacer thread — no mutex needed:
    MppBufferGroup    hold_grp  = nullptr;  // our own DRM buffer pool
    MppBuffer         last_copy = nullptr;  // copy of last frame pixels
    EncPacerFrame     last_meta;            // geometry/format for last_copy
};

#endif // ENCODER_PACER_H
