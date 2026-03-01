#pragma once
/**
 * frame_colorcorrect.h
 *
 * GPU-accelerated color correction for NV12 video frames.
 *
 * Applies the same formula as the DRM gamma LUT:
 *   y = clamp((x + offset) * gain, 0, 1)  (in RGB space)
 *
 * Pipeline per frame:
 *   1. Import src NV12 DMA-buf as GL_TEXTURE_EXTERNAL_OES.
 *      The driver converts YCbCr → RGB implicitly during sampling.
 *   2. Render fullscreen quad with correction shader into RGBA GBM BO target.
 *   3. RGA converts RGBA GBM BO → NV12 into the destination DMA-buf.
 *      The driver converts RGB → YCbCr implicitly.
 *
 * Intended to run on the __ENCPACER thread (init() must be called from
 * the same thread that will call process()).
 */

#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

class FrameColorCorrect {
public:
    FrameColorCorrect() = default;
    ~FrameColorCorrect();

    // Init EGL context, compile shader, create RGBA render targets.
    // Must be called from the thread that will call process().
    bool init(int drm_fd, uint32_t width, uint32_t height,
              float gain, float offset);

    void deinit();
    bool ready() const { return ready_; }

    // Apply color correction: read src NV12 DMA-buf, write corrected NV12
    // into dst DMA-buf.  Both fds come from mpp_buffer_get_fd().
    // Returns false on failure — caller should fall back to plain copy.
    bool process(int src_fd, uint32_t width, uint32_t height,
                 uint32_t hor_stride, uint32_t ver_stride,
                 int dst_fd);

private:
    bool build_shader();
    bool create_targets();
    void destroy_targets();
    bool ensure_functions();

    int      drm_fd_{-1};
    uint32_t width_{0}, height_{0};
    float    gain_{1.f}, offset_{0.f};
    bool     ready_{false};

    gbm_device*  gbm_{nullptr};
    EGLDisplay   dpy_{EGL_NO_DISPLAY};
    EGLContext   ctx_{EGL_NO_CONTEXT};
    EGLSurface   surf_{EGL_NO_SURFACE};
    EGLConfig    cfg_{nullptr};

    GLuint prog_{0};
    GLint  loc_tex_{-1};
    GLint  loc_gain_{-1};
    GLint  loc_offset_{-1};

    // Double-buffered RGBA render targets
    static constexpr int kTargets = 2;
    struct Target {
        gbm_bo*     bo{nullptr};
        EGLImageKHR img{EGL_NO_IMAGE_KHR};
        GLuint      tex{0};
        GLuint      fbo{0};
        int         prime_fd{-1};  // DMA-buf fd kept open for RGA
        uint32_t    stride_px{0};  // RGBA row stride in pixels
    } targets_[kTargets];
    int target_idx_{0};

    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};
    PFNEGLGETPLATFORMDISPLAYEXTPROC     eglGetPlatformDisplayEXT_{nullptr};
};
