#ifndef FRAME_PROCESSOR_H
#define FRAME_PROCESSOR_H

#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include <rockchip/rk_mpi.h>

#include "mpp_encoder.h"
#include "frame_colorcorrect.h"

// ---------------------------------------------------------------------------
// FrameProcessor: feeds MppEncoder at a steady fps regardless of incoming rate.
//
//  Two internal threads:
//   1. Processor thread (has GL context): receives decoded frames, performs
//      copy/resize/color-correction/OSD-blend, publishes the result.
//   2. Timer thread: wakes at target fps intervals and submits the latest
//      processed frame to the encoder — or repeats the last one if none
//      arrived, preventing a speedup effect when source fps < target fps.
//
//  Decoupling processing from pacing ensures the timer is never blocked by
//  heavy image work.  Throughput is limited by the slowest single stage
//  instead of the serial sum of all stages.
// ---------------------------------------------------------------------------

struct FrameProcFrame {
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

class FrameProcessor {
public:
    FrameProcessor(MppEncoder *enc, int fps, EncResolution res = EncResolution::Res1080p);
    ~FrameProcessor();

    // Called from decoder thread: update the latest available frame.
    void push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                     uint32_t hs, uint32_t vs, MppFrameFormat fmt);

    void shutdown();

    // Live-update the pacing interval (thread-safe).
    void set_fps(int fps) { interval_ns.store(1000000000L / fps, std::memory_order_relaxed); }

    // Set encoder output resolution (thread-safe).
    void set_resolution(EncResolution r) { target_res_.store((int)r, std::memory_order_relaxed); }

    // Enable GPU color correction using the DRM gamma formula y = clamp((x+offset)*gain, 0, 1).
    // Safe to call from any thread.  drm_fd is used to create the GBM/EGL context (lazy).
    void set_color_correction(float gain, float offset, int drm_fd);

    // Enable/disable color correction at runtime without changing the stored params.
    // Thread-safe: safe to toggle from the UI thread while the pacer is running.
    void set_color_correction_enabled(bool on) {
        color_correct_.store(on, std::memory_order_relaxed);
    }

    // Called from the frame thread before freeing the decoder buffer group
    // (on resolution change).  Releases any pending decoder buffer ref and
    // waits for any in-flight copy to finish so the group can be freed safely.
    void drain_decoder_refs();

    // Called from the OSD thread each time a new OSD frame is ready.
    // prime_fd  — DMA-buf fd of the OSD modeset_buf (BGRA/ARGB8888)
    // w, h      — OSD pixel dimensions
    // stride_px — row stride in pixels (= buf->stride / 4)
    void set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px);

    static void *__THREAD__(void *p);
    static void *__TIMER_THREAD__(void *p);

private:
    void process_loop();
    void timer_loop();

    MppEncoder            *encoder;
    std::atomic<long>     interval_ns;
    std::atomic<int>      target_res_{1};  // 0=720p, 1=1080p
    std::atomic<bool>     running{true};
    std::mutex              mtx;       // guards pending (shared with frame/decoder thread)
    std::condition_variable cv_;       // signalled by push_latest(); processor waits here
    std::mutex              copy_mtx_; // held by processor while it uses a decoder buffer
    FrameProcFrame     pending;   // latest from decoder (shared with decoder thread)

    // Shared between processor (writer) and timer (reader):
    std::mutex              ready_mtx_;       // guards last_copy / last_meta
    std::condition_variable ready_cv_;        // signalled when fresh frame published
    bool                    ready_fresh_{false}; // true = last_copy updated since last pickup
    MppBuffer         last_copy   = nullptr;  // latest processed frame pixels
    FrameProcFrame     last_meta;              // geometry/format for last_copy

    // Only accessed from the processor thread — no mutex needed:
    MppBufferGroup    hold_grp  = nullptr;  // our own DRM buffer pool
    MppBuffer         proc_copy_  = nullptr;  // processor's working buffer
    MppBuffer         blend_rgba_ = nullptr;  // BGRA intermediate for OSD compositing
    FrameProcFrame     proc_meta_;              // metadata being built by processor

    // OSD blend — shared between OSD thread (writer) and processor thread (reader)
    struct OsdInfo {
        int      prime_fd{-1};
        uint32_t width{0}, height{0}, stride_px{0};
    };
    std::mutex  osd_mtx_;
    OsdInfo     osd_info_;      // latest OSD frame descriptor

    // Color correction — lazy-initialized on the processor thread on first frame.
    // Written by UI thread (set_color_correction / set_color_correction_enabled),
    // read by processor thread — must be atomic.
    std::atomic<bool>  color_correct_{false};
    bool               cc_init_done_{false};   // only attempt init once
    uint32_t           cc_width_{0}, cc_height_{0}; // dimensions at last init
    float              cc_gain_{1.f}, cc_offset_{0.f};
    int                cc_drm_fd_{-1};
    FrameColorCorrect  color_gl_;
};

#endif // FRAME_PROCESSOR_H
