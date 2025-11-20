/*
 * drm.h offers a list of methods to use linux DRM and perform modeset to display video frames and the OSD. 
 * It uses two different planes for the OSD and the video feed.
 * The OSD is drawn using lib cairo.
 */

#ifndef DRM_H
#define DRM_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <pthread.h>
#include <rockchip/rk_mpi.h>
#include <assert.h>

#define OSD_BUF_COUNT	2

struct drm_object {
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

struct modeset_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_output {
	struct drm_object connector;
	struct drm_object crtc;
	drmModeCrtc *saved_crtc;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;
	int video_crtc_width;
	int video_crtc_height;

	// OSD variables
	drmModeAtomicReq *osd_request;
	unsigned int osd_buf_switch;
	struct modeset_buf osd_bufs[OSD_BUF_COUNT];
	struct drm_object osd_plane;

	// Video variables
	drmModeAtomicReq *video_request;
	struct drm_object video_plane;
	uint32_t video_frm_width;
	uint32_t video_frm_height;
	int video_fb_x, video_fb_y, video_fb_width, video_fb_height;
	int video_fb_id;
	float video_scale_factor;

    // Used to calculate latency
    uint64_t decoding_pts;
	int video_poc;

	bool cleanup;
};


int modeset_open(int *out, const char *node);

int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,const char *name);

void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type);

int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,  const char *name, uint64_t value);

int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_output *out);

const char* drm_fourcc_to_string(uint32_t fourcc);

int modeset_find_plane(int fd, struct modeset_output *out, struct drm_object *plane_out, uint32_t plane_format, uint32_t plane_id_override);

void modeset_drm_object_fini(struct drm_object *obj);

int modeset_setup_objects(int fd, struct modeset_output *out);

void modeset_destroy_objects(int fd, struct modeset_output *out);

int modeset_create_fb(int fd, struct modeset_buf *buf);

void modeset_destroy_fb(int fd, struct modeset_buf *buf);

int modeset_setup_framebuffers(int fd, drmModeConnector *conn, struct modeset_output *out);

void modeset_output_destroy(int fd, struct modeset_output *out);

struct modeset_output *modeset_output_create(int fd, drmModeRes *res, drmModeConnector *conn, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, uint32_t video_plane_id, uint32_t osd_plane_id, float video_scale_factor);

struct modeset_output *modeset_prepare(int fd, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, uint32_t video_plane_id, uint32_t osd_plane_id, float video_scale_factor);

void *modeset_print_modes(int fd);

int modeset_perform_modeset(int fd, struct modeset_output *out, drmModeAtomicReq * req, struct drm_object *plane, int fb_id, uint32_t width, uint32_t height, int zpos);

int modeset_atomic_prepare_commit(int fd, struct modeset_output *out, drmModeAtomicReq *req, struct drm_object *plane, int fb_id, uint32_t width, uint32_t height, int zpos);

void restore_planes_zpos(int fd, struct modeset_output *output_list);

void modeset_cleanup(int fd, struct modeset_output *output_list);

#endif
