
#include "osd.h"

#include "drm.h"
#include <cairo.h>
#include "mavlink.h"
#include "icons/icons.h"

#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

#define PATH_MAX	4096

struct osd_vars osd_vars;

extern uint32_t frames_received;
uint32_t stats_rx_bytes = 0;
struct timespec last_timestamp = {0, 0};
float rx_rate = 0;
int hours = 0 , minutes = 0 , seconds = 0 , milliseconds = 0;

double getTimeInterval(struct timespec* timestamp, struct timespec* last_meansure_timestamp) {
  return (timestamp->tv_sec - last_meansure_timestamp->tv_sec) +
       (timestamp->tv_nsec - last_meansure_timestamp->tv_nsec) / 1000000000.;
}


cairo_surface_t *fps_icon;
cairo_surface_t *lat_icon;
cairo_surface_t* net_icon;
cairo_surface_t* sdcard_icon;

pthread_mutex_t osd_mutex;

void modeset_paint_buffer(struct modeset_buf *buf) {
	unsigned int j,k,off;
	cairo_t* cr;
	cairo_surface_t *surface;
	char msg[80];
	memset(msg, 0x00, sizeof(msg));

	// TODO(gehee) This takes forever.
	for (j = 0; j < buf->height; ++j) {
	    for (k = 0; k < buf->width; ++k) {
	        off = buf->stride * j + k * 4;
	        *(uint32_t*)&buf->map[off] = (0 << 24) | (0 << 16) | (0 << 8) | 0;
	    }
	}

	int osd_x = buf->width - 300;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 20);

	if (osd_vars.enable_video || osd_vars.enable_wfbng ) {
		// stats height
		int stats_top_margin = 5;
		int stats_row_height = 33;
		int stats_height = 30;
		int row_count = 0;
		if (osd_vars.enable_recording) {
			stats_height+=stats_row_height;
		}
		if (osd_vars.enable_video) {
			stats_height+=stats_row_height*2;
			if (osd_vars.enable_latency) {
				stats_height+=stats_row_height;
			}
		}
		if (osd_vars.enable_wfbng) {
			stats_height+=stats_row_height;
		} 

		cairo_set_source_rgba(cr, 0, 0, 0, 0.4); // R, G, B, A
		cairo_rectangle(cr, osd_x, 0, 300, stats_height); 
		cairo_fill(cr);
		

		if (osd_vars.enable_video) {
			row_count++;
			cairo_set_source_surface (cr, fps_icon, osd_x+22, stats_top_margin+stats_row_height-19);
			cairo_paint(cr);
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height);
			sprintf(msg, "%d fps | %dx%d", osd_vars.current_framerate, osd_vars.video_width, osd_vars.video_height);
			cairo_show_text (cr, msg);

			if (osd_vars.enable_latency) {
				row_count++;
				cairo_set_source_surface (cr, lat_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
				cairo_paint (cr);
				cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
				cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height*2);
				sprintf(msg, "%.2f ms (%.f, %.f)", osd_vars.latency_avg, osd_vars.latency_min, osd_vars.latency_max);
				cairo_show_text (cr, msg);
			}
			
			// Video Link Elements
			double avg_bw = 0;
			int avg_cnt = 0;
			for (int i = osd_vars.bw_curr; i<(osd_vars.bw_curr+10); ++i) {
				int h = osd_vars.bw_stats[i%10]/10000 * 1.2;
				if (h<0) {
					h = 0;
				}
				if (osd_vars.bw_stats[i%10]>0) {
					avg_bw += osd_vars.bw_stats[i%10];
					avg_cnt++;
				}
			}
			avg_bw = avg_bw / avg_cnt;
			if (avg_bw < 1000) {
				sprintf(msg, "%.2f Kbps", avg_bw / 125 );
			} else {
				sprintf(msg, "%.2f Mbps", avg_bw / 125000 );
			}
			row_count++;
			cairo_set_source_surface (cr, net_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
			cairo_paint (cr);
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to (cr, osd_x+60, stats_top_margin+stats_row_height*row_count);
			cairo_show_text (cr, msg);
		}

		// WFB-ng Elements
		if (osd_vars.enable_wfbng) {
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			sprintf(msg, "WFB %3d F%d L%d", osd_vars.wfb_rssi, osd_vars.wfb_fec_fixed, osd_vars.wfb_errors);
			// //TODO (gehee) Only getting WFB_LINK_LOST when testing.
			// if (osd_vars.wfb_flags & WFB_LINK_LOST) {
			// 		sprintf(msg, "%s (LOST)", msg);
			// } else if (osd_vars.wfb_flags & WFB_LINK_JAMMED) {
			// 		sprintf(msg, "%s (JAMMED)", msg);
			// }
			row_count++;
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to(cr, osd_x+25, stats_top_margin+stats_row_height*row_count);
			cairo_show_text(cr, msg);
		}

		// Recording
		if (osd_vars.enable_recording) {
			
			// Sets the source pattern within cr to a translucent color. This color will then be used for any subsequent drawing operation until a new source pattern is set.
			// did not change anything
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			sprintf(msg, "Recording");
						
			row_count++;
			// This is a convenience function for creating a pattern from surface and setting it as the source in cr with cairo_set_source().
			// if when we have an icon
			cairo_set_source_surface (cr, sdcard_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
			cairo_paint (cr);

			// // set to red font
			cairo_set_source_rgba (cr, 255.0, 0.0, 0.0, 1);

			// Begin a new sub-path. After this call the current point will be (x, y)
			cairo_move_to (cr, osd_x+60, stats_top_margin+stats_row_height*row_count);
			cairo_show_text (cr, msg);
		}
	}

	if (!osd_vars.enable_telemetry){
		return;
	}

	cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
	// Mavlink elements
	uint32_t x_center = buf->width / 2;
	if (osd_vars.telemetry_level > 1){
		// TODO(geehe) How to draw lines with cairo?
		// // Artificial Horizon
		// int32_t offset_pitch = osd_vars.telemetry_pitch * 4;
		// int32_t offset_roll = osd_vars.telemetry_roll * 4;
		// int32_t y_pos_left = ((int32_t)buf->height / 2 - 2 + offset_pitch + offset_roll);
		// int32_t y_pos_right = ((int32_t)buf->height / 2 - 2 + offset_pitch - offset_roll);

		// for (int i = 0; i < 4; i++) {
		// if (y_pos_left > 0 && y_pos_left < buf->height &&
		// 	y_pos_right > 0 && y_pos_right < buf->height) {
		// 	//fbg_line(cr, x_center - 180, y_pos_left + i, x_center + 180, y_pos_right + i, 255, 255, 255);
		// }
		// }

		// // Vertical Speedometer
		// int32_t offset_vspeed = osd_vars.telemetry_vspeed * 5;
		// int32_t y_pos_vspeed = ((int32_t)buf->height / 2 - offset_vspeed);
		// for (int i = 0; i < 8; i++) {
		// if (y_pos_vspeed > 0 && y_pos_vspeed < buf->height) {
		// 	//fbg_line(cr, x_center + 242 + i, buf->height / 2, x_center + 242 + i, y_pos_vspeed, 255, 255, 255);
		// }
		// }

		// for (int i = 0; i < 25; i++) {
		// 	uint32_t width = (i == 12) ? 10 : 0;

		// 	//   fbg_line(cr, x_center - 240 - width,
		// 	//     buf->height / 2 - 120 + i * 10, x_center - 220,
		// 	//     buf->height / 2 - 120 + i * 10, 255, 255, 255);
		// 	//   fbg_line(cr, x_center - 240 - width,
		// 	//     buf->height / 2 - 120 + i * 10 + 1, x_center - 220,
		// 	//     buf->height / 2 - 120 + i * 10 + 1, 255, 255, 255);

		// 	//   fbg_line(cr, x_center + 220, buf->height / 2 - 120 + i * 10,
		// 	//     x_center + 240 + width, buf->height / 2 - 120 + i * 10, 255, 255, 255);
		// 	//   fbg_line(cr, x_center + 220, buf->height / 2 - 120 + i * 10 + 1,
		// 	//     x_center + 240 + width, buf->height / 2 - 120 + i * 10 + 1, 255, 255, 255);
		// }

		// OSD telemetry
		sprintf(msg, "ALT:%.00fM", osd_vars.telemetry_altitude);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "SPD:%.00fKM/H", osd_vars.telemetry_gspeed);
		cairo_move_to(cr, x_center - (16 * 3) - 360, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "VSPD:%.00fM/S", osd_vars.telemetry_vspeed);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 + 22);
		cairo_show_text(cr, msg);
	}

    sprintf(msg, "BAT:%.02fV", osd_vars.telemetry_battery / 1000);
    cairo_move_to(cr, 40, buf->height - 30);
    cairo_show_text(cr, msg);
    sprintf(msg, "CONS:%.00fmAh", osd_vars.telemetry_current_consumed);
    cairo_move_to(cr, 40, buf->height - 60);
    cairo_show_text(cr, msg);
    sprintf(msg, "CUR:%.02fA", osd_vars.telemetry_current / 100);
    cairo_move_to(cr, 40, buf->height - 90);
    cairo_show_text(cr, msg);
    sprintf(msg, "THR:%.00f%%", osd_vars.telemetry_throttle);
    cairo_move_to(cr, 40, buf->height - 120);
    cairo_show_text(cr, msg);
    
	if (osd_vars.telemetry_level > 1){
		sprintf(msg, "SATS:%.00f", osd_vars.telemetry_sats);
		cairo_move_to(cr,buf->width - 140, buf->height - 30);
		cairo_show_text(cr, msg);
		sprintf(msg, "HDG:%.00f", osd_vars.telemetry_hdg);
		cairo_move_to(cr,buf->width - 140, buf->height - 120);
		cairo_show_text(cr, msg);
		sprintf(osd_vars.c1, "%.00f", osd_vars.telemetry_lat);

		if (osd_vars.telemetry_lat < 10000000) {
			insertString(osd_vars.c1, "LAT:0.", 0);
		}

		if (osd_vars.telemetry_lat > 9999999) {
			if (numOfChars(osd_vars.c1) == 8) {
				insertString(osd_vars.c1, ".", 1);
			} else {
				insertString(osd_vars.c1, ".", 2);
			}
			insertString(osd_vars.c1, "LAT:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 90);
		cairo_show_text(cr,  osd_vars.c1);

		sprintf(osd_vars.c2, "%.00f", osd_vars.telemetry_lon);
		if (osd_vars.telemetry_lon < 10000000) {
			insertString(osd_vars.c2, "LON:0.", 0);
		}
		if (osd_vars.telemetry_lon > 9999999) {
			if (numOfChars(osd_vars.c2) == 8) {
				insertString(osd_vars.c2, ".", 1);
			} else {
				insertString(osd_vars.c2, ".", 2);
			}
			insertString(osd_vars.c2, "LON:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 60);
		cairo_show_text(cr,  osd_vars.c2);
		sprintf(msg, "PITCH:%.00f", osd_vars.telemetry_pitch);
		cairo_move_to(cr, x_center + 440, buf->height - 140);
		sprintf(msg, "ROLL:%.00f", osd_vars.telemetry_roll);
		cairo_move_to(cr, x_center + 440, buf->height - 170);
		sprintf(msg, "DIST:%.03fM", osd_vars.telemetry_distance);
		cairo_move_to(cr, x_center - 350, buf->height - 30);
		cairo_show_text(cr, msg);
	}
    
    sprintf(msg, "RSSI:%.00f", osd_vars.telemetry_rssi);
    cairo_move_to(cr, x_center - 50, buf->height - 30);
    cairo_show_text(cr,  msg);

    // Print rate stats
    struct timespec current_timestamp;
    if (!clock_gettime(CLOCK_MONOTONIC_COARSE, &current_timestamp)) {
      double interval = getTimeInterval(&current_timestamp, &last_timestamp);
      if (osd_vars.telemetry_arm > 1700){
        seconds = seconds + interval;
      }
      if (interval > 1) {
        last_timestamp = current_timestamp;
        rx_rate = ((float)stats_rx_bytes+(((float)stats_rx_bytes*25)/100)) / 1024.0f * 8;
        stats_rx_bytes = 0;
      }
    }

    char hud_frames_rx[32];
    if (osd_vars.telemetry_level > 1){
    	cairo_move_to(cr, x_center - strlen(hud_frames_rx) / 2 * 16, 40);
    	cairo_show_text(cr, hud_frames_rx);
    } else {
    	cairo_move_to(cr, buf->width - 300, buf->height - 60);
    	cairo_show_text(cr, hud_frames_rx);
		sprintf(msg, "TIME:%.2d:%.2d", minutes,seconds);
		cairo_move_to(cr, buf->width - 300, buf->height - 90);
		cairo_show_text(cr, msg);
		if(seconds > 59){
			seconds = 0;
			++minutes;  
		}
		if(minutes > 59){
			seconds = 0;
			minutes = 0;
		}
    }
    float percent = rx_rate / (1024 * 10);
    if (percent > 1) {
      percent = 1;
    }

    uint32_t width = (strlen(hud_frames_rx) * 16) * percent;
    if (osd_vars.telemetry_level > 1){
		cairo_set_source_rgba(cr, 255, 255, 255, 0.8); // R, G, B, A
		cairo_rectangle(cr,  x_center - strlen(hud_frames_rx) / 2 * 16, 64, width, 8); 
    } else {
		cairo_set_source_rgba(cr, 255, 255, 255, 0.8); // R, G, B, A
		cairo_rectangle(cr,  buf->width - 300, buf->height - 36, width, 8);
    }
	cairo_fill(cr);
}

int osd_thread_signal;

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;

cairo_status_t on_read_png_stream(png_closure_t * closure, unsigned char * data, unsigned int length)
{
	if(length > closure->bytes_left) return CAIRO_STATUS_READ_ERROR;
	
	memcpy(data, closure->iter, length);
	closure->iter += length;
	closure->bytes_left -= length;
	return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t * surface_from_embedded_png(const unsigned char * png, size_t length)
{
	int rc = -1;
	png_closure_t closure[1] = {{
		.iter = (unsigned char *)png,
		.bytes_left = length,
	}};
	return cairo_image_surface_create_from_png_stream(
		(cairo_read_func_t)on_read_png_stream,
		closure);
}

void *__OSD_THREAD__(void *param) {
	osd_thread_params *p = param;
	fps_icon = surface_from_embedded_png(framerate_icon, framerate_icon_length);
	lat_icon = surface_from_embedded_png(latency_icon, latency_icon_length);
	net_icon = surface_from_embedded_png(bandwidth_icon, bandwidth_icon_length);
	sdcard_icon = surface_from_embedded_png(sdcard_white_icon, sdcard_white_icon_length);

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane, buf->fb, buf->width, buf->height, osd_vars.plane_zpos);
	assert(ret >= 0);
	while(!osd_thread_signal) {
		int buf_idx = p->out->osd_buf_switch ^ 1;
		struct modeset_buf *buf = &p->out->osd_bufs[buf_idx];
		modeset_paint_buffer(buf);

		int ret = pthread_mutex_lock(&osd_mutex);
		assert(!ret);	
		p->out->osd_buf_switch = buf_idx;
		ret = pthread_mutex_unlock(&osd_mutex);
		assert(!ret);
		usleep(osd_vars.refresh_frequency_ms*1000);
    }
	printf("OSD thread done.\n");
}
