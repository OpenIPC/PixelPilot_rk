#include "drm.h"

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
#include <cairo.h>
#include <pthread.h>
#include <rockchip/rk_mpi.h>
#include <assert.h>
#include <math.h>

int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t cap;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "failed to set atomic cap, %d", ret);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support atomic KMS\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}


int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,
				  const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;
	for (j = 0; j < props->count_props && !found; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name)) {
			value = props->prop_values[j];
			found = true;
		}
		drmModeFreeProperty(prop);
	}

	if (!found)
		return -1;
	return value;
}


void modeset_get_object_properties(int fd, struct drm_object *obj,
					  uint32_t type)
{
	const char *type_str;
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props) {
		switch(type) {
			case DRM_MODE_OBJECT_CONNECTOR:
				type_str = "connector";
				break;
			case DRM_MODE_OBJECT_PLANE:
				type_str = "plane";
				break;
			case DRM_MODE_OBJECT_CRTC:
				type_str = "CRTC";
				break;
			default:
				type_str = "unknown type";
				break;
		}
		fprintf(stderr, "cannot get %s %d properties: %s\n",
			type_str, obj->id, strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}


int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
				   const char *name, uint64_t value)
{
	int i;
	uint32_t prop_id = 0;
	for (i = 0; i < obj->props->count_props; i++) {
		if (!strcmp(obj->props_info[i]->name, name)) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id == 0) {
		fprintf(stderr, "no object property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}


int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, struct modeset_output *out)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	uint32_t crtc;

	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			if (crtc > 0) {
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->saved_crtc = drmModeGetCrtc(fd, crtc);
				for (i = 0; i < res->count_crtcs; ++i) {
					if (res->crtcs[i] == crtc) {
						out->crtc_index = i;
						break;
					}
				}
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		for (j = 0; j < res->count_crtcs; ++j) {
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			crtc = res->crtcs[j];

			if (crtc > 0) {
				out->saved_crtc = drmModeGetCrtc(fd, crtc);
				fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
					crtc, conn->encoders[i]);;
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

const char* drm_fourcc_to_string(uint32_t fourcc) {
    char* result = malloc(5);
    result[0] = (char)((fourcc >> 0) & 0xFF);
    result[1] = (char)((fourcc >> 8) & 0xFF);
    result[2] = (char)((fourcc >> 16) & 0xFF);
    result[3] = (char)((fourcc >> 24) & 0xFF);
    result[4] = '\0';
    return result;
}

int modeset_find_plane(int fd, struct modeset_output *out, struct drm_object *plane_out, uint32_t plane_format, uint32_t plane_id_override)
{
	drmModePlaneResPtr plane_res;
	bool found_plane = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
				strerror(errno));
		return -ENOENT;
	}

	if (plane_id_override) {
		// Try to use the user-specified plane id
		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id_override);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id_override, strerror(errno));
			drmModeFreePlaneResources(plane_res);
			return -ENOENT;
		}
		if (plane->possible_crtcs & (1 << out->crtc_index)) {
			for (int j = 0; j < plane->count_formats; j++) {
				if (plane->formats[j] == plane_format) {
					found_plane = true;
					plane_out->id = plane_id_override;
					ret = 0;
					break;
				}
			}
		}
		if (!found_plane) {
			fprintf(stderr, "Specified plane id %u does not support required format or CRTC\n", plane_id_override);
			ret = -EINVAL;
		}
		drmModeFreePlane(plane);
		drmModeFreePlaneResources(plane_res);
		return ret;
	}

	for (i = 0; (i < plane_res->count_planes) && !found_plane; i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id,
					strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << out->crtc_index)) {
			for (int j=0; j<plane->count_formats; j++) {
				if (plane->formats[j] ==  plane_format) {
					found_plane = true;
				 	plane_out->id = plane_id;
				 	ret = 0;
					break;
				}
			}
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	return ret;
}


void modeset_drm_object_fini(struct drm_object *obj)
{
	for (int i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}


int modeset_setup_objects(int fd, struct modeset_output *out)
{
	struct drm_object *connector = &out->connector;
	struct drm_object *crtc = &out->crtc;
	struct drm_object *plane_video = &out->video_plane;
	struct drm_object *plane_osd = &out->osd_plane;

	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;

	modeset_get_object_properties(fd, plane_video, DRM_MODE_OBJECT_PLANE);
	if (!plane_video->props)
		goto out_plane;
	modeset_get_object_properties(fd, plane_osd, DRM_MODE_OBJECT_PLANE);
	if (!plane_osd->props)
		goto out_plane;
	return 0;

out_plane:
	modeset_drm_object_fini(crtc);
out_crtc:
	modeset_drm_object_fini(connector);
out_conn:
	return -ENOMEM;
}


void modeset_destroy_objects(int fd, struct modeset_output *out)
{
	modeset_drm_object_fini(&out->connector);
	modeset_drm_object_fini(&out->crtc);
	modeset_drm_object_fini(&out->video_plane);
	modeset_drm_object_fini(&out->osd_plane);
}


int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	struct drm_prime_handle ph;
	int ret;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* Export as DMA-buf prime fd for RGA. */
	buf->prime_fd = -1;
	buf->gl_fb_id = 0;
	memset(&ph, 0, sizeof(ph));
	ph.handle = buf->handle;
	ph.flags  = DRM_CLOEXEC | DRM_RDWR;
	if (drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0) {
		buf->prime_fd = ph.fd;
	} else {
		fprintf(stderr, "warning: PRIME_HANDLE_TO_FD failed (%m); RGA path unavailable\n");
	}

	handles[0] = buf->handle;
	pitches[0] = buf->stride;

	ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_ARGB8888,
			    handles, pitches, offsets, &buf->fb, 0);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	if (buf->prime_fd >= 0) { close(buf->prime_fd); buf->prime_fd = -1; }
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}


void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	if (buf->prime_fd >= 0) { close(buf->prime_fd); buf->prime_fd = -1; }

	munmap(buf->map, buf->size);

	drmModeRmFB(fd, buf->fb);

	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}


int modeset_setup_framebuffers(int fd, drmModeConnector *conn, struct modeset_output *out)
{
	for (int i=0; i<OSD_BUF_COUNT; i++) {
		out->osd_bufs[i].width = out->mode.hdisplay;
		out->osd_bufs[i].height = out->mode.vdisplay;
		int ret = modeset_create_fb(fd, &out->osd_bufs[i]);
		if (ret) {
			return ret;
		}
	}
	out->video_crtc_width = out->mode.hdisplay;
	out->video_crtc_height = out->mode.vdisplay;
	return 0;
}


void modeset_output_destroy(int fd, struct modeset_output *out)
{
	modeset_destroy_objects(fd, out);

	for (int i=0; i<OSD_BUF_COUNT; i++) { 
		modeset_destroy_fb(fd, &out->osd_bufs[i]);
	}
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
	free(out);
}

struct modeset_output *modeset_output_create(int fd, drmModeRes *res, drmModeConnector *conn, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, uint32_t video_plane_id, uint32_t osd_plane_id, float video_scale_factor)
{
	int ret;
	struct modeset_output *out;

	out = malloc(sizeof(*out));
	memset(out, 0, sizeof(*out));
	out->video_scale_factor = video_scale_factor;
	out->connector.id = conn->connector_id;

	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	int fc = 0;
	int preferred_fc = -1;
	if (mode_width>0 && mode_height>0 && mode_vrefresh>0) {
		fc = -1;
		printf( "Available modes:\n");
		for (int i = 0; i < conn->count_modes; i++ ) {
			printf( "%d : %dx%d@%d\n",i, conn->modes[i].hdisplay, conn->modes[i].vdisplay , conn->modes[i].vrefresh );
			if (conn->modes[i].hdisplay == mode_width &&
			conn->modes[i].vdisplay == mode_height &&
			conn->modes[i].vrefresh == mode_vrefresh
			) {
				if (fc < 0)
					fc = i;
				if (fc >= 0 && (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) == 0) // prefer progressive modes
					fc = i;
			} else if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				preferred_fc = i;
			}
		}
		if (fc < 0  && preferred_fc < 0) {
			fprintf(stderr, "couldn't find a matching mode for %dx%d@%d\n", mode_width , mode_height , mode_vrefresh);
			goto out_error;
		} else if (fc < 0  && preferred_fc >= 0) {
			fprintf(stderr, "couldn't find a matching mode, useing preferred mode %dx%d@%d\n", conn->modes[preferred_fc].hdisplay, conn->modes[preferred_fc].vdisplay , conn->modes[preferred_fc].vrefresh);
			fc = preferred_fc;
		}
		printf( "Using screen mode %dx%d@%d\n",conn->modes[fc].hdisplay, conn->modes[fc].vdisplay , conn->modes[fc].vrefresh );
	}
	memcpy(&out->mode, &conn->modes[fc], sizeof(out->mode));
	if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode), &out->mode_blob_id) != 0) {
		fprintf(stderr, "couldn't create a blob property\n");
		goto out_error;
	}

	ret = modeset_find_crtc(fd, res, conn, out);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
		goto out_blob;
	}

	ret = modeset_find_plane(fd, out, &out->video_plane, DRM_FORMAT_NV12, video_plane_id);
	if (ret) {
		fprintf(stderr, "no valid video plane with format NV12 for crtc %u\n", out->crtc.id);
		goto out_blob;
	}
	fprintf(stdout, "Using plane %d (NV12) for Video\n",  out->video_plane.id);

	ret = modeset_find_plane(fd, out, &out->osd_plane, DRM_FORMAT_ARGB8888, osd_plane_id);
	if (ret) {
		fprintf(stderr, "no valid osd plane with format ARGB8888 for crtc %u\n", out->crtc.id);
		goto out_blob;
	}
	fprintf(stdout, "Using plane %d (ARGB8888) for OSD\n",  out->osd_plane.id);

	ret = modeset_setup_objects(fd, out);
	if (ret) {
		fprintf(stderr, "cannot get plane properties\n");
		goto out_blob;
	}

	ret = modeset_setup_framebuffers(fd, conn, out);
	if (ret) {
		fprintf(stderr, "cannot create framebuffers for connector %u\n",
			conn->connector_id);
		goto out_obj;
	}

	out->video_request = drmModeAtomicAlloc();
	assert(out->video_request);
	out->osd_request = drmModeAtomicAlloc();
	assert(out->video_request);

	return out;

out_obj:
	modeset_destroy_objects(fd, out);
out_blob:
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
	free(out);
	return NULL;
}

void *modeset_print_modes(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeModeInfo info;
	uint prev_h, prev_v, prev_refresh = 0;
	int at_least_one = 0;
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return NULL;
	}

	for (int i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}
		for (int i = 0; i < conn->count_modes; i++ ) {
			info = conn->modes[i];
			// Assuming modes list is sorted
			if (info.hdisplay == prev_h && info.vdisplay == prev_v && info.vrefresh == prev_refresh)
				continue;
			printf("%dx%d@%d\n", info.hdisplay, info.vdisplay, info.vrefresh);
			prev_h = info.hdisplay;
			prev_v = info.vdisplay;
			prev_refresh = info.vrefresh;
			at_least_one = 1;
		}
		drmModeFreeConnector(conn);
	}
	if (!at_least_one) {
		fprintf(stderr, "No displays found\n");
	}
	drmModeFreeResources(res);
	return NULL;

}

struct modeset_output *modeset_prepare(int fd, uint16_t mode_width, uint16_t mode_height, uint32_t mode_vrefresh, uint32_t video_plane_id, uint32_t osd_plane_id, float video_scale_factor)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_output *out;

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return NULL;
	}

	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		out = modeset_output_create(fd, res, conn, mode_width, mode_height, mode_vrefresh, video_plane_id, osd_plane_id, video_scale_factor);
		drmModeFreeConnector(conn);
		if (out) {
			drmModeFreeResources(res);
			return out;
		}
	}
	fprintf(stderr, "couldn't create any outputs\n");
	drmModeFreeResources(res);
	return NULL;
}

void modeset_apply_video_scale(int fd, struct modeset_output *out)
{
	drmModePlane *plane = drmModeGetPlane(fd, out->video_plane.id);
	if (!plane) {
		fprintf(stderr, "modeset_apply_video_scale: drmModeGetPlane failed\n");
		return;
	}
	uint32_t fb_id = plane->fb_id;
	drmModeFreePlane(plane);

	if (!fb_id) return;

	int64_t zpos = get_property_value(fd, out->video_plane.props, "zpos");

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret = modeset_atomic_prepare_commit(fd, out, req, &out->video_plane,
	                                        (int)fb_id, out->video_frm_width, out->video_frm_height, zpos);
	if (ret < 0) {
		fprintf(stderr, "modeset_apply_video_scale: prepare commit failed\n");
	} else {
		ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret < 0)
			fprintf(stderr, "modeset_apply_video_scale: atomic commit failed: %m\n");
	}
	drmModeAtomicFree(req);
}

int modeset_perform_modeset(int fd, struct modeset_output *out, drmModeAtomicReq * req, struct drm_object *plane, int fb_id, uint32_t width, uint32_t height, int zpos)
{
	int ret, flags;

	ret = modeset_atomic_prepare_commit(fd, out, req, plane, fb_id, width, height, zpos);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for plane %d: %m\n", plane->id);
		return ret;
	}

	/* perform test-only atomic commit */
	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "test-only atomic commit failed for plane %d: %m\n", plane->id);
		return ret;
	}

	/* initial modeset on all outputs */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0)
		fprintf(stderr, "modeset atomic commit failed for plane %d: %m\n", plane->id);

	return ret;
}


int modeset_atomic_prepare_commit(int fd, struct modeset_output *out, drmModeAtomicReq *req, struct drm_object *plane, 
	int fb_id, uint32_t width, uint32_t height, int zpos)
{
	if (set_drm_object_property(req, &out->connector, "CRTC_ID", out->crtc.id) < 0)
		return -1;
	if (set_drm_object_property(req, &out->crtc, "MODE_ID", out->mode_blob_id) < 0)
		return -1;
	if (set_drm_object_property(req, &out->crtc, "ACTIVE", 1) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "FB_ID", fb_id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_ID", out->crtc.id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_W", width << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_H", height << 16) < 0)
		return -1;

	uint32_t orig_crtcw = out->video_crtc_width;
	uint32_t orig_crtch = out->video_crtc_height;
	float video_ratio = (float)width / height;
	if (orig_crtcw / video_ratio > orig_crtch) {
		orig_crtcw = orig_crtch * video_ratio;
		orig_crtch = orig_crtch;
	} else {
		orig_crtcw = orig_crtcw;
		orig_crtch = orig_crtcw / video_ratio;
	}

	
	float scale_factor = out->video_scale_factor;
	uint32_t crtcw = (uint32_t)(orig_crtcw * scale_factor);
	uint32_t crtch = (uint32_t)(orig_crtch * scale_factor);

	int crtcx = (out->video_crtc_width - crtcw) / 2;
	int crtcy = (out->video_crtc_height - crtch) / 2;
	if (set_drm_object_property(req, plane, "CRTC_X", crtcx) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_Y", crtcy) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_W", crtcw) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_H", crtch) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "zpos", zpos) < 0)
		return -1;

	return 0;
}

void restore_planes_zpos(int fd, struct modeset_output *output_list) {
	// restore osd zpos
	int ret, flags;
	struct modeset_buf *buf = &output_list->osd_bufs[0];

	// TODO(geehe) Find a more elegant way to do this.
	int64_t zpos = get_property_value(fd, output_list->osd_plane.props, "zpos");
	ret = modeset_atomic_prepare_commit(fd, output_list, output_list->osd_request, &output_list->osd_plane, buf->fb, buf->width, buf->height, zpos);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for plane %d, %m\n", output_list->osd_plane.id);
		return;
	}
	ret = drmModeAtomicCommit(fd, output_list->osd_request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) 
		fprintf(stderr, "modeset atomic commit failed for plane %d, %m\n", output_list->osd_plane.id);

	zpos = get_property_value(fd, output_list->video_plane.props, "zpos");
	ret = modeset_atomic_prepare_commit(fd, output_list, output_list->video_request, &output_list->video_plane, buf->fb, buf->width, buf->height, zpos);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
		return;
	}
	ret = drmModeAtomicCommit(fd, output_list->video_request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) 
		fprintf(stderr, "modeset atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
}

void modeset_cleanup(int fd, struct modeset_output *output_list)
{
	modeset_output_destroy(fd, output_list);
}

// Initialize the controller
void gamma_lut_controller_init(gamma_lut_controller* ctrl, int drm_fd, struct modeset_output* output_list) {
    ctrl->drm_fd = drm_fd;
    ctrl->output_list = output_list;
    ctrl->gamma_lut_blob_id = 0;
    ctrl->last_offset = 0.0f;
    ctrl->last_gain = 1.0f;
    ctrl->is_enabled = false;
}

// Get LUT size from DRM properties
static uint64_t get_lut_size(gamma_lut_controller* ctrl) {
    uint64_t lut_size = get_property_value(ctrl->drm_fd, ctrl->output_list->crtc.props, "GAMMA_LUT_SIZE");
    if (lut_size == (uint64_t)-1) {
        lut_size = get_property_value(ctrl->drm_fd, ctrl->output_list->crtc.props, "gamma_lut_size");
    }
    return lut_size;
}

// Enable gamma LUT with specified offset and gain
bool gamma_lut_enable(gamma_lut_controller* ctrl, float offset, float gain) {
    // Store parameters for potential re-enable
    ctrl->last_offset = offset;
    ctrl->last_gain = gain;

    // Get LUT size
    uint64_t lut_size = get_lut_size(ctrl);
    if (lut_size == 0 || lut_size == (uint64_t)-1) {
        // Fallback to a reasonable default if size not available
        lut_size = 4096;  // Common LUT size
    }

    // Create the LUT data
    struct drm_color_lut* lut = malloc(lut_size * sizeof(struct drm_color_lut));
    if (!lut) {
        return false;
    }

    for (uint64_t i = 0; i < lut_size; i++) {
        float x = (lut_size > 1) ? (float)i / (float)(lut_size - 1) : 0.0f;
        float y = (x + offset) * gain;

        // Clamp to [0, 1]
        if (y < 0.0f) y = 0.0f;
        if (y > 1.0f) y = 1.0f;

        uint16_t v = (uint16_t)lroundf(y * 65535.0f);
        lut[i].red = v;
        lut[i].green = v;
        lut[i].blue = v;
        lut[i].reserved = 0;
    }

    // Destroy previous blob if exists
    if (ctrl->gamma_lut_blob_id > 0) {
        drmModeDestroyPropertyBlob(ctrl->drm_fd, ctrl->gamma_lut_blob_id);
        ctrl->gamma_lut_blob_id = 0;
    }

    // Create new property blob
    int ret = drmModeCreatePropertyBlob(ctrl->drm_fd, lut, 
                                        lut_size * sizeof(struct drm_color_lut), 
                                        &ctrl->gamma_lut_blob_id);
    free(lut);

    if (ret != 0) {
        return false;
    }

    // Apply the LUT via atomic commit
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        drmModeDestroyPropertyBlob(ctrl->drm_fd, ctrl->gamma_lut_blob_id);
        ctrl->gamma_lut_blob_id = 0;
        return false;
    }

    // Try both property names
    int set_ret = set_drm_object_property(req, &ctrl->output_list->crtc, "GAMMA_LUT", ctrl->gamma_lut_blob_id);
    if (set_ret < 0) {
        set_ret = set_drm_object_property(req, &ctrl->output_list->crtc, "gamma_lut", ctrl->gamma_lut_blob_id);
    }

    if (set_ret < 0) {
        drmModeAtomicFree(req);
        drmModeDestroyPropertyBlob(ctrl->drm_fd, ctrl->gamma_lut_blob_id);
        ctrl->gamma_lut_blob_id = 0;
        return false;
    }

    int commit_ret = drmModeAtomicCommit(ctrl->drm_fd, req, 0, NULL);
    drmModeAtomicFree(req);

    if (commit_ret != 0) {
        drmModeDestroyPropertyBlob(ctrl->drm_fd, ctrl->gamma_lut_blob_id);
        ctrl->gamma_lut_blob_id = 0;
        return false;
    }

    ctrl->is_enabled = true;
    return true;
}

// Disable gamma LUT (revert to defaults)
bool gamma_lut_disable(gamma_lut_controller* ctrl) {
    if (!ctrl->is_enabled && ctrl->gamma_lut_blob_id == 0) {
        return true;  // Already disabled
    }
    return gamma_lut_enable(ctrl, 0, 1);
}


// Re-enable with last used parameters
bool gamma_lut_reenable(gamma_lut_controller* ctrl) {
    return gamma_lut_enable(ctrl, ctrl->last_offset, ctrl->last_gain);
}

// Toggle between enabled and disabled states
bool gamma_lut_toggle(gamma_lut_controller* ctrl) {
    if (ctrl->is_enabled) {
        return gamma_lut_disable(ctrl);
    } else {
        return gamma_lut_reenable(ctrl);
    }
}

// Check if gamma LUT is currently enabled
bool gamma_lut_is_enabled(gamma_lut_controller* ctrl) {
    return ctrl->is_enabled;
}

// Get current parameters
void gamma_lut_get_params(gamma_lut_controller* ctrl, float* offset, float* gain) {
    *offset = ctrl->last_offset;
    *gain = ctrl->last_gain;
}

// Clean up and disable
void gamma_lut_cleanup(gamma_lut_controller* ctrl) {
    gamma_lut_disable(ctrl);
}
