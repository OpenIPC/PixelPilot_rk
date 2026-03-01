/**
 * frame_colorcorrect.cpp
 *
 * See frame_colorcorrect.h for design notes.
 */

#include "frame_colorcorrect.h"

#include <unistd.h>
#include <drm_fourcc.h>
#include <rga/im2d.h>
#include <rga/rga.h>
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char* kVert = R"GLSL(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";

// Forward colortrans: y = clamp((x + offset) * gain, 0, 1).
// samplerExternalOES lets the driver convert NV12 → RGB transparently.
static const char* kFrag = R"GLSL(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 v_uv;
uniform samplerExternalOES tex;
uniform float gain;
uniform float offset;
void main() {
    vec3 c = texture2D(tex, v_uv).rgb;
    gl_FragColor = vec4(clamp((c + offset) * gain, 0.0, 1.0), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        spdlog::error("FrameCC: shader compile error: {}", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512]; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        spdlog::error("FrameCC: program link error: {}", log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------
// FrameColorCorrect
// ---------------------------------------------------------------------------
FrameColorCorrect::~FrameColorCorrect() { deinit(); }

bool FrameColorCorrect::ensure_functions() {
    eglGetPlatformDisplayEXT_ =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    eglCreateImageKHR_ =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_) {
        spdlog::error("FrameCC: required EGL/GL extension functions not available");
        return false;
    }
    return true;
}

bool FrameColorCorrect::init(int drm_fd, uint32_t width, uint32_t height,
                              float gain, float offset)
{
    drm_fd_  = drm_fd;
    width_   = width;
    height_  = height;
    gain_    = gain;
    offset_  = offset;

    gbm_ = gbm_create_device(drm_fd_);
    if (!gbm_) { spdlog::error("FrameCC: gbm_create_device failed"); return false; }

    if (!ensure_functions()) return false;

    dpy_ = eglGetPlatformDisplayEXT_
         ? eglGetPlatformDisplayEXT_(EGL_PLATFORM_GBM_KHR, gbm_, nullptr)
         : eglGetDisplay((EGLNativeDisplayType)gbm_);
    if (dpy_ == EGL_NO_DISPLAY) {
        spdlog::error("FrameCC: eglGetDisplay failed"); return false;
    }
    if (!eglInitialize(dpy_, nullptr, nullptr)) {
        spdlog::error("FrameCC: eglInitialize failed"); return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        spdlog::error("FrameCC: eglBindAPI failed"); return false;
    }

    const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(dpy_, cfg_attrs, &cfg_, 1, &n) || n == 0) {
        const EGLint relaxed[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        if (!eglChooseConfig(dpy_, relaxed, &cfg_, 1, &n) || n == 0) {
            spdlog::error("FrameCC: eglChooseConfig failed"); return false;
        }
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx_ = eglCreateContext(dpy_, cfg_, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx_ == EGL_NO_CONTEXT) {
        spdlog::error("FrameCC: eglCreateContext failed"); return false;
    }

    const EGLint pb_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    surf_ = eglCreatePbufferSurface(dpy_, cfg_, pb_attrs);

    if (!eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
        spdlog::error("FrameCC: eglMakeCurrent failed (0x{:x})", eglGetError());
        return false;
    }

    if (!build_shader())   return false;
    if (!create_targets()) return false;

    ready_ = true;
    spdlog::info("FrameCC: ready ({}x{} gain={} offset={})", width_, height_, gain_, offset_);
    return true;
}

bool FrameColorCorrect::build_shader() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kVert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    prog_ = link_program(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!prog_) return false;

    loc_tex_    = glGetUniformLocation(prog_, "tex");
    loc_gain_   = glGetUniformLocation(prog_, "gain");
    loc_offset_ = glGetUniformLocation(prog_, "offset");
    return true;
}

bool FrameColorCorrect::create_targets() {
    for (int i = 0; i < kTargets; i++) {
        Target& t = targets_[i];

        t.bo = gbm_bo_create(gbm_, width_, height_, GBM_FORMAT_ARGB8888,
                             GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!t.bo) {
            spdlog::error("FrameCC: gbm_bo_create failed for target {}", i);
            return false;
        }

        // Export once; keep fd open for RGA use in process()
        t.prime_fd = gbm_bo_get_fd(t.bo);
        if (t.prime_fd < 0) {
            spdlog::error("FrameCC: gbm_bo_get_fd failed for target {}", i);
            return false;
        }
        t.stride_px = gbm_bo_get_stride(t.bo) / 4;  // bytes/row ÷ 4 bytes/px

        // Import as EGLImage → bind as texture → attach as FBO
        const EGLint img_attrs[] = {
            EGL_WIDTH,                       (EGLint)width_,
            EGL_HEIGHT,                      (EGLint)height_,
            EGL_LINUX_DRM_FOURCC_EXT,        (EGLint)DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT,       t.prime_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,   0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,    (EGLint)gbm_bo_get_stride(t.bo),
            EGL_NONE
        };
        t.img = eglCreateImageKHR_(dpy_, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, nullptr, img_attrs);
        if (t.img == EGL_NO_IMAGE_KHR) {
            spdlog::error("FrameCC: eglCreateImageKHR for target {} failed (0x{:x})",
                          i, eglGetError());
            return false;
        }

        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, t.img);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, t.tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::error("FrameCC: framebuffer incomplete for target {}", i);
            return false;
        }
        spdlog::debug("FrameCC: render target {} ready stride_px={}", i, t.stride_px);
    }
    return true;
}

bool FrameColorCorrect::process(int src_fd, uint32_t width, uint32_t height,
                                 uint32_t hor_stride, uint32_t ver_stride,
                                 int dst_fd)
{
    // --- Import NV12 source as EGLImage (two-plane) -------------------------
    EGLint uv_offset = (EGLint)((size_t)hor_stride * ver_stride);
    const EGLint src_attrs[] = {
        EGL_WIDTH,                       (EGLint)width,
        EGL_HEIGHT,                      (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT,        (EGLint)DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT,       src_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,   0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,    (EGLint)hor_stride,
        EGL_DMA_BUF_PLANE1_FD_EXT,       src_fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,   uv_offset,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,    (EGLint)hor_stride,
        EGL_NONE
    };
    EGLImageKHR src_img = eglCreateImageKHR_(dpy_, EGL_NO_CONTEXT,
                                             EGL_LINUX_DMA_BUF_EXT,
                                             nullptr, src_attrs);
    if (src_img == EGL_NO_IMAGE_KHR) {
        spdlog::warn("FrameCC: eglCreateImageKHR for NV12 src failed (0x{:x})", eglGetError());
        return false;
    }

    GLuint src_tex;
    glGenTextures(1, &src_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_tex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, src_img);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // --- Render into next RGBA target ----------------------------------------
    target_idx_ = (target_idx_ + 1) % kTargets;
    Target& tgt = targets_[target_idx_];

    glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
    glViewport(0, 0, (GLsizei)width_, (GLsizei)height_);

    glUseProgram(prog_);
    glUniform1i(loc_tex_,    0);
    glUniform1f(loc_gain_,   gain_);
    glUniform1f(loc_offset_, offset_);

    // Flip V so that top-origin NV12 memory maps to the top of the GL FB.
    const GLfloat verts[] = {
        // x,    y,    u,   v
        -1.f, -1.f,  0.f, 0.f, // Bottom Left  -> UV (0, 0)
         1.f, -1.f,  1.f, 0.f, // Bottom Right -> UV (1, 0)
        -1.f,  1.f,  0.f, 1.f, // Top Left     -> UV (0, 1)
         1.f,  1.f,  1.f, 1.f, // Top Right    -> UV (1, 1)
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // glFinish() instead of glFlush(): Panfrost does not reliably signal the
    // DMA-buf implicit fence before glFlush() returns, so the RGA imcvtcolor()
    // below would read the GBM BO before the GPU is done writing it, producing
    // mixed old/new frame data that appears as jitter at motion boundaries.
    glFinish();

    glDeleteTextures(1, &src_tex);
    eglDestroyImageKHR_(dpy_, src_img);

    // --- RGA: convert RGBA target → NV12 destination -----------------------
    // DRM_FORMAT_ARGB8888 is BGRA in memory on little-endian → RK_FORMAT_BGRA_8888
    rga_buffer_t src_rga = wrapbuffer_fd_t(
        tgt.prime_fd,
        (int)width_, (int)height_,
        (int)tgt.stride_px, (int)height_,
        RK_FORMAT_BGRA_8888);
    rga_buffer_t dst_rga = wrapbuffer_fd_t(
        dst_fd,
        (int)width, (int)height,
        (int)hor_stride, (int)ver_stride,
        RK_FORMAT_YCbCr_420_SP);

    if (imcvtcolor(src_rga, dst_rga, RK_FORMAT_BGRA_8888,
                   RK_FORMAT_YCbCr_420_SP) != IM_STATUS_SUCCESS) {
        spdlog::warn("FrameCC: RGA RGBA→NV12 conversion failed");
        return false;
    }
    return true;
}

void FrameColorCorrect::destroy_targets() {
    for (int i = 0; i < kTargets; i++) {
        Target& t = targets_[i];
        if (t.fbo)  { glDeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
        if (t.tex)  { glDeleteTextures(1, &t.tex);     t.tex = 0; }
        if (t.img != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(dpy_, t.img); t.img = EGL_NO_IMAGE_KHR;
        }
        if (t.prime_fd >= 0) { close(t.prime_fd); t.prime_fd = -1; }
        if (t.bo)   { gbm_bo_destroy(t.bo);            t.bo = nullptr; }
    }
}

void FrameColorCorrect::deinit() {
    if (!gbm_) return;

    if (ctx_ != EGL_NO_CONTEXT)
        eglMakeCurrent(dpy_, surf_, surf_, ctx_);

    // glFlush() in process() submits commands to the Panfrost batch but does not
    // wait for completion.  If we destroy the GBM BOs while the batch is still
    // in-flight, the next eglMakeCurrent() in init() will try to flush and unbind
    // the old context (via _mesa_make_current → panfrost_flush_all_batches) and
    // crash in panfrost_batch_add_bo_old because the backing GEM memory is freed.
    // glFinish() drains the batch queue before we touch any resources.
    glFinish();

    destroy_targets();

    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }

    // Explicitly detach the context from this thread before destroying it.
    // Without this the thread is left with a stale "current" context, and the
    // next eglMakeCurrent() call (in init() after a resolution change) will
    // attempt to flush and unbind it — crashing because the GBM BOs are gone.
    if (ctx_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy_, ctx_); ctx_ = EGL_NO_CONTEXT;
    }
    if (surf_ != EGL_NO_SURFACE) {
        eglDestroySurface(dpy_, surf_); surf_ = EGL_NO_SURFACE;
    }
    if (dpy_ != EGL_NO_DISPLAY) {
        eglTerminate(dpy_); dpy_ = EGL_NO_DISPLAY;
    }
    gbm_device_destroy(gbm_); gbm_ = nullptr;
    ready_ = false;
}
