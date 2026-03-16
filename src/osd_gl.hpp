#pragma once
/**
 * osd_gl.hpp
 *
 * Minimal GBM/EGL/GLES2 processor for the Cairo OSD buffer.
 * Applies the inverse colortrans LUT shader to an ARGB8888 dumb buffer
 * (identified by its prime_fd) and returns a DRM fb_id ready for scanout.
 *
 * Usage in __OSD_THREAD__:
 *
 *   // Once, after modeset:
 *   OsdGl osd_gl;
 *   if (enable_live_colortrans)
 *       osd_gl.init(p->fd, buf->width, buf->height,
 *                   live_colortrans_gain, live_colortrans_offset);
 *
 *   // Each refresh, replacing apply_inverse_colortrans_inplace(buf):
 *   uint32_t fb_id = buf->fb;  // fallback
 *   if (osd_gl.ready())
 *       fb_id = osd_gl.process(buf);  // 0 = fallback to buf->fb
 *
 *   // Use fb_id for the DRM plane commit instead of buf->fb.
 */

#include <cstdint>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

extern "C" {
#include "drm.h"
}

class OsdGl {
public:
    OsdGl() = default;
    ~OsdGl();

    // Initialise EGL context, compile shader, allocate render targets.
    // drm_fd   — the same fd used for modeset (for GBM device + drmModeAddFB2)
    // width/height — OSD buffer dimensions
    // gain/offset  — initial colortrans parameters (can be updated via set_params)
    bool init(int drm_fd, uint32_t width, uint32_t height,
              float gain, float offset);

    void set_params(float gain, float offset);

    // Process one frame.
    // Imports buf->prime_fd as EGLImage, runs shader, returns fb_id of result.
    // Returns 0 on failure — caller should fall back to buf->fb + CPU path.
    // premultiplied: true  = Cairo (CAIRO_FORMAT_ARGB32, premultiplied alpha)
    //               false = LVGL  (LV_COLOR_FORMAT_ARGB8888, straight alpha)
    uint32_t process(struct modeset_buf* buf, bool premultiplied = true);

    bool ready() const { return ready_; }
    void deinit();

private:
    bool build_shader();
    bool create_targets();
    void destroy_targets();
    bool ensure_functions();

    int      drm_fd_{-1};
    uint32_t width_{0};
    uint32_t height_{0};
    float    gain_{1.f};
    float    offset_{0.f};
    bool     ready_{false};

    // GBM
    gbm_device*  gbm_{nullptr};

    // EGL
    EGLDisplay   dpy_{EGL_NO_DISPLAY};
    EGLContext   ctx_{EGL_NO_CONTEXT};
    EGLSurface   surf_{EGL_NO_SURFACE};  // small pbuffer, needed on some drivers
    EGLConfig    cfg_{nullptr};

    // GLES program
    GLuint prog_{0};
    GLint  loc_tex_{-1};
    GLint  loc_gain_{-1};
    GLint  loc_offset_{-1};
    GLint  loc_premul_{-1};

    // Double-buffered GBM render targets
    static constexpr int kTargets = 2;
    struct Target {
        gbm_bo*      bo{nullptr};
        EGLImageKHR  img{EGL_NO_IMAGE_KHR};
        GLuint       tex{0};
        GLuint       fbo{0};
        uint32_t     fb_id{0};
    } targets_[kTargets];
    int target_idx_{0};

    // EGL extension function pointers
    PFNEGLCREATEIMAGEKHRPROC        eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC       eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT_{nullptr};
};