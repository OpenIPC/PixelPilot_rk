/**
 * osd_gl.cpp
 *
 * GBM/EGL/GLES2 inverse colortrans processor for the Cairo OSD buffer.
 *
 * Pipeline per frame:
 *   1. Import buf->prime_fd (ARGB8888 dumb buffer) as EGLImage via
 *      EGL_LINUX_DMA_BUF_EXT — zero copy, no upload.
 *   2. Bind as GL_TEXTURE_2D source.
 *   3. Render fullscreen quad with LUT shader into GBM BO render target.
 *      Shader does: un-premultiply → gain/offset inverse → re-premultiply.
 *   4. GBM BO is already registered as a DRM framebuffer (fb_id).
 *      Caller uses this fb_id for the OSD plane commit instead of buf->fb.
 *
 * The Cairo dumb buffer (buf->map) is never touched by the CPU after render.
 * The shader runs entirely on Mali-G52.
 */

#include "osd_gl.hpp"

#include <cstring>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// Shader source
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

// Inverse colortrans for premultiplied ARGB8888:
//   un-premultiply → x = y/gain - offset → re-premultiply
// Fully transparent pixels are passed through unchanged (alpha == 0 guard).
static const char* kFrag = R"GLSL(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D tex;
uniform float gain;
uniform float offset;

void main() {
    vec4 c = texture2D(tex, v_uv);
    if (c.a < 0.004) {          // fully transparent — pass through
        gl_FragColor = vec4(0.0);
        return;
    }
    // un-premultiply
    vec3 straight = c.rgb / c.a;
    // inverse transform: x = y/gain - offset
    vec3 corrected = clamp(straight / gain - offset, 0.0, 1.0);
    // re-premultiply
    gl_FragColor = vec4(corrected * c.a, c.a);
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
        spdlog::error("OSD GL: shader compile error: {}", log);
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
        spdlog::error("OSD GL: program link error: {}", log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------
// OsdGl
// ---------------------------------------------------------------------------
OsdGl::~OsdGl() { deinit(); }

bool OsdGl::ensure_functions() {
    eglGetPlatformDisplayEXT_ =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    eglCreateImageKHR_ =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
        !glEGLImageTargetTexture2DOES_) {
        spdlog::error("OSD GL: required EGL/GL extension functions not available");
        return false;
    }
    return true;
}

bool OsdGl::init(int drm_fd, uint32_t width, uint32_t height,
                 float gain, float offset)
{
    drm_fd_  = drm_fd;
    width_   = width;
    height_  = height;
    gain_    = gain;
    offset_  = offset;

    // --- GBM device ----------------------------------------------------------
    gbm_ = gbm_create_device(drm_fd_);
    if (!gbm_) {
        spdlog::error("OSD GL: gbm_create_device failed");
        return false;
    }

    // --- EGL display ---------------------------------------------------------
    if (!ensure_functions()) return false;

    dpy_ = eglGetPlatformDisplayEXT_
         ? eglGetPlatformDisplayEXT_(EGL_PLATFORM_GBM_KHR, gbm_, nullptr)
         : eglGetDisplay((EGLNativeDisplayType)gbm_);

    if (dpy_ == EGL_NO_DISPLAY) {
        spdlog::error("OSD GL: eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(dpy_, nullptr, nullptr)) {
        spdlog::error("OSD GL: eglInitialize failed");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        spdlog::error("OSD GL: eglBindAPI failed");
        return false;
    }

    // --- EGL config ----------------------------------------------------------
    const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(dpy_, cfg_attrs, &cfg_, 1, &n) || n == 0) {
        // Relax — some Mali drivers don't advertise pbuffer for all configs
        const EGLint cfg_relaxed[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        if (!eglChooseConfig(dpy_, cfg_relaxed, &cfg_, 1, &n) || n == 0) {
            spdlog::error("OSD GL: eglChooseConfig failed");
            return false;
        }
    }

    // --- EGL context ---------------------------------------------------------
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx_ = eglCreateContext(dpy_, cfg_, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx_ == EGL_NO_CONTEXT) {
        spdlog::error("OSD GL: eglCreateContext failed");
        return false;
    }

    // Small pbuffer surface — needed on drivers that don't support surfaceless
    const EGLint pb_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    surf_ = eglCreatePbufferSurface(dpy_, cfg_, pb_attrs);
    // surf_ may be EGL_NO_SURFACE on surfaceless drivers — that's OK,
    // we pass EGL_NO_SURFACE to eglMakeCurrent in that case.

    if (!eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
        spdlog::error("OSD GL: eglMakeCurrent failed (egl error 0x{:x})",
                      eglGetError());
        return false;
    }

    spdlog::info("OSD GL: GL vendor={} renderer={}",
                 (const char*)glGetString(GL_VENDOR),
                 (const char*)glGetString(GL_RENDERER));

    // --- Shader --------------------------------------------------------------
    if (!build_shader()) return false;

    // --- Render targets ------------------------------------------------------
    if (!create_targets()) return false;

    ready_ = true;
    spdlog::info("OSD GL: ready ({}x{} gain={} offset={})",
                 width_, height_, gain_, offset_);
    return true;
}

bool OsdGl::build_shader() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kVert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    prog_ = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!prog_) return false;

    loc_tex_    = glGetUniformLocation(prog_, "tex");
    loc_gain_   = glGetUniformLocation(prog_, "gain");
    loc_offset_ = glGetUniformLocation(prog_, "offset");
    return true;
}

bool OsdGl::create_targets() {
    for (int i = 0; i < kTargets; i++) {
        Target& t = targets_[i];

        // GBM BO: ARGB8888, scanout + render
        t.bo = gbm_bo_create(gbm_, width_, height_, GBM_FORMAT_ARGB8888,
                             GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!t.bo) {
            spdlog::error("OSD GL: gbm_bo_create failed for target {}", i);
            return false;
        }

        // Export as DMA-buf and import into EGL as render target
        int bo_fd = gbm_bo_get_fd(t.bo);
        if (bo_fd < 0) {
            spdlog::error("OSD GL: gbm_bo_get_fd failed");
            return false;
        }

        const EGLint img_attrs[] = {
            EGL_WIDTH,                       (EGLint)width_,
            EGL_HEIGHT,                      (EGLint)height_,
            EGL_LINUX_DRM_FOURCC_EXT,        (EGLint)DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT,       bo_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,   0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,    (EGLint)gbm_bo_get_stride(t.bo),
            EGL_NONE
        };
        t.img = eglCreateImageKHR_(dpy_, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, nullptr, img_attrs);
        close(bo_fd);
        if (t.img == EGL_NO_IMAGE_KHR) {
            spdlog::error("OSD GL: eglCreateImageKHR failed for target {} "
                          "(egl error 0x{:x})", i, eglGetError());
            return false;
        }

        // Bind as texture → framebuffer
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
            spdlog::error("OSD GL: framebuffer incomplete for target {}", i);
            return false;
        }

        // Register as DRM framebuffer
        uint32_t handles[4] = { gbm_bo_get_handle(t.bo).u32, 0, 0, 0 };
        uint32_t pitches[4] = { gbm_bo_get_stride(t.bo),     0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(drm_fd_, width_, height_, DRM_FORMAT_ARGB8888,
                          handles, pitches, offsets, &t.fb_id, 0) != 0) {
            spdlog::error("OSD GL: drmModeAddFB2 failed for target {}", i);
            return false;
        }

        spdlog::info("OSD GL: render target {} ready fb_id={}", i, t.fb_id);
    }
    return true;
}

void OsdGl::set_params(float gain, float offset) {
    gain_   = gain;
    offset_ = offset;
    // Uniforms are set per-draw so no extra work needed here
}

uint32_t OsdGl::process(struct modeset_buf* buf)
{
    if (!ready_ || buf->prime_fd < 0) return 0;

    // --- Import Cairo dumb buffer as EGLImage --------------------------------
    const EGLint src_attrs[] = {
        EGL_WIDTH,                       (EGLint)buf->width,
        EGL_HEIGHT,                      (EGLint)buf->height,
        EGL_LINUX_DRM_FOURCC_EXT,        (EGLint)DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,       buf->prime_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,   0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,    (EGLint)buf->stride,
        EGL_NONE
    };
    EGLImageKHR src_img = eglCreateImageKHR_(dpy_, EGL_NO_CONTEXT,
                                             EGL_LINUX_DMA_BUF_EXT,
                                             nullptr, src_attrs);
    if (src_img == EGL_NO_IMAGE_KHR) {
        spdlog::warn("OSD GL: eglCreateImageKHR for src failed (0x{:x}); "
                     "falling back", eglGetError());
        return 0;
    }

    // Bind as source texture (unit 0)
    GLuint src_tex;
    glGenTextures(1, &src_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, src_img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // --- Render into next target ---------------------------------------------
    target_idx_ = (target_idx_ + 1) % kTargets;
    Target& tgt = targets_[target_idx_];

    glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
    glViewport(0, 0, (GLsizei)width_, (GLsizei)height_);

    glUseProgram(prog_);
    glUniform1i(loc_tex_,    0);
    glUniform1f(loc_gain_,   gain_);
    glUniform1f(loc_offset_, offset_);

    const GLfloat verts[] = {
        // x,   y,    u,   v
        -1.f, -1.f,  0.f, 0.f, // Bottom Left  -> UV (0, 0)
        1.f, -1.f,  1.f, 0.f, // Bottom Right -> UV (1, 0)
        -1.f,  1.f,  0.f, 1.f, // Top Left     -> UV (0, 1)
        1.f,  1.f,  1.f, 1.f, // Top Right    -> UV (1, 1)
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), verts);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), verts + 2);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Ensure GPU has finished before DRM scans out
    glFinish();

    // --- Cleanup source texture/image ----------------------------------------
    glDeleteTextures(1, &src_tex);
    eglDestroyImageKHR_(dpy_, src_img);

    return tgt.fb_id;
}

void OsdGl::destroy_targets() {
    for (int i = 0; i < kTargets; i++) {
        Target& t = targets_[i];
        if (t.fbo)    { glDeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
        if (t.tex)    { glDeleteTextures(1, &t.tex);     t.tex = 0; }
        if (t.img != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(dpy_, t.img);
            t.img = EGL_NO_IMAGE_KHR;
        }
        if (t.fb_id)  { drmModeRmFB(drm_fd_, t.fb_id);  t.fb_id = 0; }
        if (t.bo)     { gbm_bo_destroy(t.bo);            t.bo = nullptr; }
    }
}

void OsdGl::deinit() {
    if (!gbm_) return;  // never inited

    if (ctx_ != EGL_NO_CONTEXT)
        eglMakeCurrent(dpy_, surf_, surf_, ctx_);

    destroy_targets();

    if (prog_)              { glDeleteProgram(prog_); prog_ = 0; }
    if (ctx_ != EGL_NO_CONTEXT) {
        eglDestroyContext(dpy_, ctx_);
        ctx_ = EGL_NO_CONTEXT;
    }
    if (surf_ != EGL_NO_SURFACE) {
        eglDestroySurface(dpy_, surf_);
        surf_ = EGL_NO_SURFACE;
    }
    if (dpy_ != EGL_NO_DISPLAY) {
        eglTerminate(dpy_);
        dpy_ = EGL_NO_DISPLAY;
    }
    if (gbm_) {
        gbm_device_destroy(gbm_);
        gbm_ = nullptr;
    }
    ready_ = false;
}