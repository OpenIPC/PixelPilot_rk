/**
 * Device side of the OSD: the LVGL display driver on top of the DRM OSD plane and
 * the "__OSD" render thread. The widget engine itself (facts, widgets, config) lives
 * in osd.cpp and is shared with the SDL simulator, which brings its own display.
 */
#include <cassert>
#include <cstring>
#include <chrono>
#include <thread>
extern "C" {
#include "drm.h"
#include "menu.h"
#include "input.h"
}
#include "osd.h"
#include "osd.hpp"
#include "spdlog/spdlog.h"
#include "../lvgl/lvgl.h"
#include "osd_gl.hpp"
#include "frame_processor.h"

int osd_zpos = 2;
extern pthread_mutex_t video_mutex;
extern pthread_cond_t video_cond;
bool osd_update_ready = false;
bool gsmenu_enabled = false;

OsdGl osd_gl;
extern bool enable_live_colortrans;
extern float live_colortrans_offset;
extern float live_colortrans_gain;

extern FrameProcessor *frame_proc;
extern bool dvr_osd;

pthread_mutex_t osd_mutex;

osd_thread_params *p;

/* LVGL renders into this cached (normal RAM) shadow buffer instead of the
 * write-combined DRM dumb buffers. Alpha blending is read-modify-write, and
 * uncached reads on ARM are extremely slow — rendering directly into the dumb
 * buffers is what made the menu sluggish. The shadow always holds the complete
 * current UI; my_flush_cb copies it to the off-screen DRM buffer and flips. */
static uint8_t * lvgl_shadow;

void my_flush_cb(lv_display_t * display, const lv_area_t * area, uint8_t * px_map)
{
	(void)area; (void)px_map;

	/* Direct mode calls flush once per invalidated area. Acting on the
	 * intermediate calls used to flip the visible buffer mid-frame — the
	 * display thread commits on every video frame, so scanout could catch a
	 * half-rendered frame (rows blinking out during navigation, stale fade
	 * ghosts). Only publish once the frame is complete. */
	if (!lv_display_flush_is_last(display)) {
		lv_display_flush_ready(display);
		return;
	}

	/* osd_buf_switch is only ever flipped on this thread, so the back buffer
	 * is stable: copy without the lock. */
	int back = p->out->osd_buf_switch ^ 1;
	struct modeset_buf *dst = &p->out->osd_bufs[back];
	memcpy(dst->map, lvgl_shadow, dst->size);

	int ret = pthread_mutex_lock(&osd_mutex);
	assert(!ret);
	p->out->osd_buf_switch = back;
	if (enable_live_colortrans) {
		dst->gl_fb_id = osd_gl_process(dst);
	}
	ret = pthread_mutex_unlock(&osd_mutex);
	assert(!ret);

	if (dvr_osd && frame_proc)
		frame_proc->set_osd_blend(dst->prime_fd, dst->width, dst->height,
		                         dst->stride / 4);

	// tell the display thread that we have a update
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);
	osd_update_ready = true;
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);

    /* IMPORTANT!!!
     * Inform LVGL that flushing is complete so buffer can be modified again. */
    lv_display_flush_ready(display);
}

uint32_t my_get_milliseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

lv_display_t * display;
static lv_draw_buf_t lvgl_draw_buf1;

void setup_lvgl(osd_thread_params *p) {

	/* Initialize LVGL. */
    lv_init();

	struct modeset_buf *buf1 = &p->out->osd_bufs[0];

	display = lv_display_create(buf1->width, buf1->height);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);

	/* Single cached shadow buffer as the one draw buffer (see my_flush_cb).
	 * With one buffer LVGL renders in place and never runs its dual-buffer
	 * dirty-area sync (which read back from uncached DRM memory every frame).
	 * Keep the DRM pitch (buf->stride) so the shadow's row layout matches the
	 * hardware-aligned scanline stride and the flush copy is one flat memcpy. */
	size_t shadow_sz = ((size_t)buf1->size + 63) & ~(size_t)63;
	lvgl_shadow = (uint8_t *)aligned_alloc(64, shadow_sz);
	assert(lvgl_shadow);
	memset(lvgl_shadow, 0, shadow_sz);
	lv_draw_buf_init(&lvgl_draw_buf1, buf1->width, buf1->height,
	                 LV_COLOR_FORMAT_ARGB8888, buf1->stride, lvgl_shadow, buf1->size);
	lv_display_set_draw_buffers(display, &lvgl_draw_buf1, NULL);
	lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_DIRECT);

	lv_display_set_flush_cb(display, my_flush_cb);

	lv_tick_set_cb(my_get_milliseconds);

    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, LV_PART_MAIN);

}

void *__OSD_THREAD__(void *param) {
	p = (osd_thread_params *)param;
	pthread_setname_np(pthread_self(), "__OSD");

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane,
								  buf->fb, buf->width, buf->height, osd_zpos);

	if (!osd_gl.init(p->fd, buf->width, buf->height,
						live_colortrans_gain, live_colortrans_offset)) {
		spdlog::warn("OSD GL: init failed");
	}

	// LVGL is always initialized: OSD rendering now uses LVGL widgets
	setup_lvgl(p);

	// The fact processor thread starts inside osd_engine_start and blocks on the
	// lock until the widgets and the menu are built.
	osd_lock();
	// Widgets go on layer_bottom, below every screen (OSD and menu)
	osd_engine_start(lv_layer_bottom(), buf->width, buf->height,
	                 p->config_path.c_str(), nullptr);

	// Force an initial render so the video thread receives an OSD buffer signal
	lv_task_handler();

	if (gsmenu_enabled) {
		pp_menu_main();
	}
	osd_unlock();

	// Main loop: focus purely on rendering
	while (!osd_thread_signal) {

		if (gsmenu_enabled) {
			handle_keyboard_input();
		}

		osd_lock();
		// Update time-based widgets at fixed intervals
		osd_tick_if_due();
		// Render frequently for smooth animations (~60Hz)
		lv_task_handler();
		osd_unlock();

		// Sleep briefly to avoid busy loop, ~60Hz
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

	spdlog::info("OSD thread done.");
	return nullptr;
}

extern "C" uint32_t osd_gl_process(struct modeset_buf* buf){
	return osd_gl.process(buf);
}
