#include <pthread.h>
#include <time.h>
#include <chrono>
#include <cstring>

#include "spdlog/spdlog.h"

#include <rga/im2d.h>
#include <rga/rga.h>

#include "dvr.h"
#include "encoder_pacer.h"

// Map MPP pixel format to the corresponding RGA format for im2d DMA copies.
static int mpp_fmt_to_rga(MppFrameFormat fmt)
{
    switch (fmt) {
    case MPP_FMT_YUV420SP:        return RK_FORMAT_YCbCr_420_SP;
    case MPP_FMT_YUV420SP_10BIT:  return RK_FORMAT_YCbCr_420_SP_10B;
    default:                      return RK_FORMAT_YCbCr_420_SP;
    }
}

EncoderPacer::EncoderPacer(MppEncoder *enc, int fps)
    : encoder(enc), interval_ns(1000000000L / fps) {
    mpp_buffer_group_get_internal(&hold_grp, MPP_BUFFER_TYPE_DRM);
}

EncoderPacer::~EncoderPacer() {
    shutdown();
    if (last_copy)   { mpp_buffer_put(last_copy);   last_copy   = nullptr; }
    if (blend_rgba_) { mpp_buffer_put(blend_rgba_); blend_rgba_ = nullptr; }
    if (hold_grp)    { mpp_buffer_group_put(hold_grp); hold_grp = nullptr; }
}

void EncoderPacer::push_latest(MppBuffer buf, uint32_t w, uint32_t h,
                                uint32_t hs, uint32_t vs, MppFrameFormat fmt) {
    if (!running) return;
    mpp_buffer_inc_ref(buf);
    EncPacerFrame nf;
    nf.buffer = buf; nf.width = w; nf.height = h;
    nf.hor_stride = hs; nf.ver_stride = vs; nf.fmt = fmt;
    {
        std::lock_guard<std::mutex> lock(mtx);
        pending.release();   // drop any previous un-consumed frame
        pending = nf;
    }
    cv_.notify_one();  // wake pacer if in grace-period wait
}

void EncoderPacer::shutdown() {
    running = false;
    cv_.notify_one();
}

void EncoderPacer::drain_decoder_refs() {
    // Drop any pending decoder buffer so the caller can free the group.
    {
        std::lock_guard<std::mutex> lock(mtx);
        pending.release();
    }
    // Wait for any in-flight copy (which holds a decoder buffer ref) to finish.
    std::lock_guard<std::mutex> copy_lock(copy_mtx_);
    // At this point no decoder buffers are referenced by the pacer.
}

void EncoderPacer::set_osd_blend(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px) {
    std::lock_guard<std::mutex> lock(osd_mtx_);
    osd_info_.prime_fd   = prime_fd;
    osd_info_.width      = w;
    osd_info_.height     = h;
    osd_info_.stride_px  = stride_px;
}

void EncoderPacer::set_color_correction(float gain, float offset, int drm_fd) {
    cc_gain_    = gain;
    cc_offset_  = offset;
    cc_drm_fd_  = drm_fd;
    color_correct_.store(true, std::memory_order_relaxed);
    // Actual EGL/GL init happens lazily on the pacer thread (first frame)
}

void *EncoderPacer::__THREAD__(void *p) {
    pthread_setname_np(pthread_self(), "__ENCPACER");
    ((EncoderPacer *)p)->loop();
    return nullptr;
}

void EncoderPacer::loop() {
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

        // Take the latest pending frame (under mutex, then process outside).
        // If none arrived yet, wait a grace period (half frame interval) to
        // absorb scheduling jitter before falling back to frame repetition.
        // If the grace period was used, re-anchor the timer so the next
        // interval is measured from now, keeping even frame spacing.
        EncPacerFrame fresh;
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (!pending.buffer) {
                auto t0 = std::chrono::steady_clock::now();
                auto grace = std::chrono::nanoseconds(
                    interval_ns.load(std::memory_order_relaxed) / 2);
                cv_.wait_for(lock, grace,
                             [&]{ return pending.buffer != nullptr || !running; });
                auto waited_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (pending.buffer) {
                    spdlog::warn("Grace period: frame arrived after {} us", waited_us);
                    // Frame arrived during grace period — re-anchor the
                    // timer so the next tick is a full interval from now.
                    clock_gettime(CLOCK_MONOTONIC, &next);
                } else {
                    spdlog::warn("Grace period: expired after {} us, repeating frame", waited_us);
                }
            }
            if (pending.buffer) {
                fresh = pending;
                pending.buffer = nullptr;
            }
        }

        // If a new frame arrived, copy its pixels into our own hold buffer
        // and immediately release the decoder buffer.  This ensures we never
        // hold a slot in the decoder's fixed-size buffer pool any longer than
        // the copy itself, regardless of how many times we repeat the frame.
        //
        // copy_mtx_ is held for the entire duration so drain_decoder_refs()
        // can safely wait for us to finish before the caller frees the group.
        if (fresh.buffer) {
            std::lock_guard<std::mutex> copy_lock(copy_mtx_);

            size_t sz = (size_t)fresh.hor_stride * fresh.ver_stride;
            sz += sz / 2;  // NV12: Y plane + UV plane
            size_t actual = mpp_buffer_get_size(fresh.buffer);
            if (sz > actual) sz = actual;

            if (hold_grp && (!last_copy || mpp_buffer_get_size(last_copy) < sz)) {
                if (last_copy) { mpp_buffer_put(last_copy); last_copy = nullptr; }
                mpp_buffer_get(hold_grp, &last_copy, sz);
            }
            if (last_copy) {
                // Re-init color correction if frame dimensions changed.
                if (cc_init_done_ &&
                    (fresh.width != cc_width_ || fresh.height != cc_height_)) {
                    color_gl_.deinit();
                    cc_init_done_ = false;
                }
                // Lazy GL init on first frame (must run on pacer thread, try once)
                if (color_correct_.load(std::memory_order_relaxed) && !cc_init_done_) {
                    cc_init_done_ = true;
                    cc_width_  = fresh.width;
                    cc_height_ = fresh.height;
                    color_gl_.init(cc_drm_fd_, fresh.width, fresh.height,
                                   cc_gain_, cc_offset_);
                }

                bool copied = false;
                if (color_correct_.load(std::memory_order_relaxed) && color_gl_.ready()) {
                    // GPU path: NV12 → corrected RGBA (shader) → NV12 (RGA CSC)
                    copied = color_gl_.process(
                        mpp_buffer_get_fd(fresh.buffer),
                        fresh.width, fresh.height,
                        fresh.hor_stride, fresh.ver_stride,
                        mpp_buffer_get_fd(last_copy));
                }
                if (!copied) {
                    // Fallback: plain RGA copy (no color correction)
                    int rga_fmt = mpp_fmt_to_rga(fresh.fmt);
                    rga_buffer_t src_rga = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(fresh.buffer),
                        fresh.width, fresh.height,
                        fresh.hor_stride, fresh.ver_stride, rga_fmt);
                    rga_buffer_t dst_rga = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(last_copy),
                        fresh.width, fresh.height,
                        fresh.hor_stride, fresh.ver_stride, rga_fmt);
                    if (imcopy(src_rga, dst_rga) != IM_STATUS_SUCCESS) {
                        void *sp = mpp_buffer_get_ptr(fresh.buffer);
                        void *dp = mpp_buffer_get_ptr(last_copy);
                        if (sp && dp) memcpy(dp, sp, sz);
                    }
                }
                last_meta = fresh;
                last_meta.buffer = nullptr;
            }
            fresh.release();  // decoder buffer is free again
        }

        // OSD blend: composite the latest OSD frame (BGRA) over last_copy (NV12).
        // RGA alpha blending requires both buffers in an RGB-family format, so we
        // use a BGRA intermediate: NV12 → BGRA → blend OSD → BGRA → NV12.
        if (last_copy) {
            OsdInfo osd_snap;
            {
                std::lock_guard<std::mutex> lock(osd_mtx_);
                osd_snap = osd_info_;
            }
            if (osd_snap.prime_fd >= 0 && osd_snap.width > 0 && osd_snap.height > 0) {
                // Ensure the BGRA intermediate buffer is large enough.
                size_t bgra_sz = (size_t)last_meta.hor_stride * last_meta.ver_stride * 4;
                if (!blend_rgba_ || mpp_buffer_get_size(blend_rgba_) < bgra_sz) {
                    if (blend_rgba_) { mpp_buffer_put(blend_rgba_); blend_rgba_ = nullptr; }
                    mpp_buffer_get(hold_grp, &blend_rgba_, bgra_sz);
                }
                if (blend_rgba_) {
                    rga_buffer_t nv12 = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(last_copy),
                        last_meta.width, last_meta.height,
                        last_meta.hor_stride, last_meta.ver_stride,
                        RK_FORMAT_YCbCr_420_SP);
                    rga_buffer_t bgra = wrapbuffer_fd_t(
                        mpp_buffer_get_fd(blend_rgba_),
                        last_meta.width, last_meta.height,
                        last_meta.hor_stride, last_meta.ver_stride,
                        RK_FORMAT_BGRA_8888);
                    rga_buffer_t osd = wrapbuffer_fd_t(
                        osd_snap.prime_fd,
                        osd_snap.width, osd_snap.height,
                        osd_snap.stride_px, osd_snap.height,
                        RK_FORMAT_BGRA_8888);
                    // Step 1: convert video NV12 → BGRA
                    imcvtcolor(nv12, bgra, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGRA_8888);
                    // Step 2: alpha-blend OSD over video (both BGRA)
                    imblend(osd, bgra, IM_ALPHA_BLEND_SRC_OVER);
                    // Step 3: convert result back BGRA → NV12
                    imcvtcolor(bgra, nv12, RK_FORMAT_BGRA_8888, RK_FORMAT_YCbCr_420_SP);
                }
            }
        }

        if (last_copy) {
            mpp_buffer_inc_ref(last_copy);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t pts_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            encoder->push_frame(last_copy,
                                last_meta.width, last_meta.height,
                                last_meta.hor_stride, last_meta.ver_stride,
                                last_meta.fmt, pts_ms);
        }
    }
    // Release pending decoder buffer on exit
    std::lock_guard<std::mutex> lock(mtx);
    pending.release();
}
