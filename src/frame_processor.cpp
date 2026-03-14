#include <pthread.h>
#include <time.h>
#include <chrono>
#include <cstring>

#include "spdlog/spdlog.h"

#include <rga/im2d.h>
#include <rga/rga.h>

#include "dvr.h"
#include "frame_processor.h"

// Map MPP pixel format to the corresponding RGA format for im2d DMA copies.
static int mpp_fmt_to_rga(MppFrameFormat fmt)
{
    switch (fmt) {
    case MPP_FMT_YUV420SP:        return RK_FORMAT_YCbCr_420_SP;
    case MPP_FMT_YUV420SP_10BIT:  return RK_FORMAT_YCbCr_420_SP_10B;
    default:                      return RK_FORMAT_YCbCr_420_SP;
    }
}

static inline uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

FrameProcessor::FrameProcessor(MppEncoder *enc, int fps, EncResolution res)
    : encoder(enc), interval_ns(1000000000L / fps), target_res_((int)res) {
    mpp_buffer_group_get_internal(&hold_grp, MPP_BUFFER_TYPE_DRM);
}

FrameProcessor::~FrameProcessor() {
    shutdown();
    if (last_copy)   { mpp_buffer_put(last_copy);   last_copy   = nullptr; }
    if (proc_copy_)  { mpp_buffer_put(proc_copy_);  proc_copy_  = nullptr; }
    if (blend_rgba_) { mpp_buffer_put(blend_rgba_); blend_rgba_ = nullptr; }
    if (hold_grp)    { mpp_buffer_group_put(hold_grp); hold_grp = nullptr; }
}

void FrameProcessor::push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                                uint32_t hs, uint32_t vs, MppFrameFormat fmt) {
    if (!running) return;
    mpp_buffer_inc_ref(buf);
    FrameProcFrame nf;
    nf.buffer = buf; nf.width = w; nf.height = h;
    nf.hor_stride = hs; nf.ver_stride = vs; nf.fmt = fmt;
    {
        std::lock_guard<std::mutex> lock(mtx);
        pending.release();   // drop any previous un-consumed frame
        pending = nf;
    }
    cv_.notify_one();  // wake processor if waiting
}

void FrameProcessor::shutdown() {
    running = false;
    cv_.notify_one();
    ready_cv_.notify_one();
}

void FrameProcessor::drain_decoder_refs() {
    // Drop any pending decoder buffer so the caller can free the group.
    {
        std::lock_guard<std::mutex> lock(mtx);
        pending.release();
    }
    // Wait for any in-flight copy (which holds a decoder buffer ref) to finish.
    std::lock_guard<std::mutex> copy_lock(copy_mtx_);
    // At this point no decoder buffers are referenced by the pacer.
}

void FrameProcessor::set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px) {
    std::lock_guard<std::mutex> lock(osd_mtx_);
    osd_info_.prime_fd   = prime_fd;
    osd_info_.width      = w;
    osd_info_.height     = h;
    osd_info_.stride_px  = stride_px;
}

void FrameProcessor::set_color_correction(float gain, float offset, int drm_fd) {
    cc_gain_    = gain;
    cc_offset_  = offset;
    cc_drm_fd_  = drm_fd;
    color_correct_.store(true, std::memory_order_relaxed);
    // Actual EGL/GL init happens lazily on the processor thread (first frame)
}

// ── Processor thread entry point ────────────────────────────────────────────

void *FrameProcessor::__THREAD__(void *p) {
    pthread_setname_np(pthread_self(), "__ENCPROC");
    ((FrameProcessor *)p)->process_loop();
    return nullptr;
}

// ── Timer thread entry point ────────────────────────────────────────────────

void *FrameProcessor::__TIMER_THREAD__(void *p) {
    pthread_setname_np(pthread_self(), "__ENCTIMER");
    ((FrameProcessor *)p)->timer_loop();
    return nullptr;
}

// ── Processor loop ──────────────────────────────────────────────────────────
//
// Receives decoded frames, performs copy/resize/color-correction/OSD-blend,
// and publishes the result for the timer thread to pick up.
// Runs continuously — not paced by a timer.  Processing is decoupled from
// the submission rate so heavy GPU/RGA work never starves the timer.

void FrameProcessor::process_loop() {
    // Start the timer thread; it will be joined when we exit.
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, &FrameProcessor::__TIMER_THREAD__, this);

    while (running) {
        // Wait for a new decoded frame.
        FrameProcFrame fresh;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv_.wait(lock, [&]{ return pending.buffer != nullptr || !running; });
            if (!running) break;
            if (pending.buffer) {
                fresh = pending;
                pending.buffer = nullptr;
            }
        }
        if (!fresh.buffer) continue;

        // If DVR is not active, just drain the frame to release the decoder ref.
        if (!dvr_enabled || !encoder) {
            fresh.release();
            continue;
        }

        auto t_start = std::chrono::steady_clock::now();

        // ── Copy / resize / color-correct ───────────────────────────────
        // copy_mtx_ is held so drain_decoder_refs() can safely wait for us.
        {
            std::lock_guard<std::mutex> copy_lock(copy_mtx_);

            // Compute target dimensions based on resolution setting.
            uint32_t dst_w, dst_h, dst_hs, dst_vs;
            if (target_res_.load(std::memory_order_relaxed) == 0) {
                dst_w = 1280; dst_h = 720;
            } else {
                dst_w = 1920; dst_h = 1080;
            }
            dst_hs = align_up(dst_w, 16);
            dst_vs = align_up(dst_h, 16);

            size_t dst_sz = (size_t)dst_hs * dst_vs * 3 / 2;  // NV12

            if (hold_grp && (!proc_copy_ || mpp_buffer_get_size(proc_copy_) < dst_sz)) {
                if (proc_copy_) { mpp_buffer_put(proc_copy_); proc_copy_ = nullptr; }
                mpp_buffer_get(hold_grp, &proc_copy_, dst_sz);
            }
            if (proc_copy_) {
                // Re-init color correction if source dimensions changed.
                if (cc_init_done_ &&
                    (fresh.width != cc_width_ || fresh.height != cc_height_)) {
                    color_gl_.deinit();
                    cc_init_done_ = false;
                }
                // Lazy GL init on first frame (must run on processor thread, try once)
                if (color_correct_.load(std::memory_order_relaxed) && !cc_init_done_) {
                    cc_init_done_ = true;
                    cc_width_  = fresh.width;
                    cc_height_ = fresh.height;
                    color_gl_.init(cc_drm_fd_, fresh.width, fresh.height,
                                   cc_gain_, cc_offset_);
                }

                bool copied = false;
                if (color_correct_.load(std::memory_order_relaxed) && color_gl_.ready()) {
                    // GPU path: NV12 → corrected RGBA (shader) → NV12+resize (RGA CSC)
                    copied = color_gl_.process(
                        mpp_buffer_get_fd(fresh.buffer),
                        fresh.width, fresh.height,
                        fresh.hor_stride, fresh.ver_stride,
                        mpp_buffer_get_fd(proc_copy_),
                        dst_w, dst_h, dst_hs, dst_vs);
                }
                if (!copied) {
                    // Fallback: RGA resize (or copy if same size)
                    int rga_fmt = mpp_fmt_to_rga(fresh.fmt);
                    rga_buffer_t src_rga = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(fresh.buffer),
                        fresh.width, fresh.height,
                        fresh.hor_stride, fresh.ver_stride, rga_fmt);
                    rga_buffer_t dst_rga = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(proc_copy_),
                        dst_w, dst_h, dst_hs, dst_vs, rga_fmt);
                    if (fresh.width == dst_w && fresh.height == dst_h) {
                        if (imcopy(src_rga, dst_rga) != IM_STATUS_SUCCESS) {
                            size_t copy_sz = (size_t)fresh.hor_stride * fresh.ver_stride * 3 / 2;
                            size_t actual = mpp_buffer_get_size(fresh.buffer);
                            if (copy_sz > actual) copy_sz = actual;
                            void *sp = mpp_buffer_get_ptr(fresh.buffer);
                            void *dp = mpp_buffer_get_ptr(proc_copy_);
                            if (sp && dp) memcpy(dp, sp, copy_sz);
                        }
                    } else {
                        if (imresize(src_rga, dst_rga) != IM_STATUS_SUCCESS) {
                            spdlog::warn("RGA resize failed {}x{} -> {}x{}",
                                         fresh.width, fresh.height, dst_w, dst_h);
                        }
                    }
                }
                proc_meta_.width      = dst_w;
                proc_meta_.height     = dst_h;
                proc_meta_.hor_stride = dst_hs;
                proc_meta_.ver_stride = dst_vs;
                proc_meta_.fmt        = fresh.fmt;
                proc_meta_.buffer     = nullptr;
            }
            fresh.release();  // decoder buffer is free again
        }

        if (!proc_copy_) continue;

        // ── OSD blend on proc_copy_ ─────────────────────────────────────
        {
            OsdInfo osd_snap;
            {
                std::lock_guard<std::mutex> lock(osd_mtx_);
                osd_snap = osd_info_;
            }
            if (osd_snap.prime_fd >= 0 && osd_snap.width > 0 && osd_snap.height > 0) {
                size_t bgra_sz = (size_t)proc_meta_.hor_stride * proc_meta_.ver_stride * 4;
                if (!blend_rgba_ || mpp_buffer_get_size(blend_rgba_) < bgra_sz) {
                    if (blend_rgba_) { mpp_buffer_put(blend_rgba_); blend_rgba_ = nullptr; }
                    mpp_buffer_get(hold_grp, &blend_rgba_, bgra_sz);
                }
                if (blend_rgba_) {
                    rga_buffer_t nv12 = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(proc_copy_),
                        proc_meta_.width, proc_meta_.height,
                        proc_meta_.hor_stride, proc_meta_.ver_stride,
                        RK_FORMAT_YCbCr_420_SP);
                    rga_buffer_t bgra = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(blend_rgba_),
                        proc_meta_.width, proc_meta_.height,
                        proc_meta_.hor_stride, proc_meta_.ver_stride,
                        RK_FORMAT_BGRA_8888);
                    rga_buffer_t osd = wrapbuffer_fd_t(
                        osd_snap.prime_fd,
                        osd_snap.width, osd_snap.height,
                        osd_snap.stride_px, osd_snap.height,
                        RK_FORMAT_BGRA_8888);
                    imcvtcolor(nv12, bgra, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGRA_8888);
                    imblend(osd, bgra, IM_ALPHA_BLEND_SRC_OVER);
                    imcvtcolor(bgra, nv12, RK_FORMAT_BGRA_8888, RK_FORMAT_YCbCr_420_SP);
                }
            }
        }

        // ── Publish: swap proc buffer into last_copy for the timer ──────
        {
            std::lock_guard<std::mutex> lock(ready_mtx_);
            std::swap(proc_copy_, last_copy);
            last_meta = proc_meta_;
            ready_fresh_ = true;
        }
        ready_cv_.notify_one();

        // ── Timing diagnostics ──────────────────────────────────────────
        {
            auto t_end = std::chrono::steady_clock::now();
            long proc_us = std::chrono::duration_cast<std::chrono::microseconds>(
                t_end - t_start).count();
            long ival_us = interval_ns.load(std::memory_order_relaxed) / 1000;

            // Running average over ~64 frames (shift-based EMA).
            static long avg_us = 0;
            avg_us = avg_us ? avg_us + (proc_us - avg_us) / 64 : proc_us;
            static int log_counter = 0;
            if (++log_counter >= 120) {
                spdlog::debug("FrameProcessor process avg={} us (budget {} us)",
                              avg_us, ival_us);
                log_counter = 0;
            }

            if (proc_us > ival_us) {
                spdlog::debug("FrameProcessor process took {} us, exceeds frame budget {} us",
                             proc_us, ival_us);
            }
        }
    }

    // Wait for timer thread to finish before we return.
    pthread_join(timer_tid, NULL);

    // Release pending decoder buffer on exit
    std::lock_guard<std::mutex> lock(mtx);
    pending.release();
}

// ── Timer loop ──────────────────────────────────────────────────────────────
//
// Lightweight: just timing + pick latest processed frame + push to encoder.
// No image processing happens here, so it never misses a tick.

void FrameProcessor::timer_loop() {
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (running) {
        next.tv_nsec += interval_ns.load(std::memory_order_relaxed);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        if (!running) break;
        if (!dvr_enabled || !encoder) continue;

        // Pick the latest processed frame.  If no fresh frame is ready,
        // wait up to half an interval for the processor to finish — this
        // avoids unnecessary repeats (micro-stutters) when processing is
        // just slightly slower than the timer tick.
        MppBuffer frame = nullptr;
        FrameProcFrame meta;
        {
            std::unique_lock<std::mutex> lock(ready_mtx_);
            if (!ready_fresh_ && last_copy) {
                auto grace = std::chrono::nanoseconds(
                    interval_ns.load(std::memory_order_relaxed) / 2);
                ready_cv_.wait_for(lock, grace,
                    [&]{ return ready_fresh_ || !running; });
                if (!running) break;
                if (ready_fresh_) {
                    // Fresh frame arrived during grace — re-anchor timer
                    // so we don't cascade from the delayed tick.
                    clock_gettime(CLOCK_MONOTONIC, &next);
                    spdlog::debug("FrameProcessor grace period absorbed late frame");
                }
            }
            ready_fresh_ = false;
            if (last_copy) {
                mpp_buffer_inc_ref(last_copy);
                frame = last_copy;
                meta  = last_meta;
            }
        }
        if (frame) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t pts_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            encoder->push_frame(frame,
                                meta.width, meta.height,
                                meta.hor_stride, meta.ver_stride,
                                meta.fmt, pts_ms);
        }
    }
}
