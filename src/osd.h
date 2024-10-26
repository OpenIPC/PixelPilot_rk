#ifndef OSD_H
#define OSD_H

#include "drm.h"
#include <cairo.h>

// OSD Vars
struct osd_vars {
    int plane_zpos;
    int refresh_frequency_ms;

	// Video Decoder
    bool enable_video;
	int current_framerate;
    bool enable_latency;
	float latency_avg;
	float latency_max;
	float latency_min;
    bool enable_recording;

	// Video Feed
	int bw_curr;
	long long bw_stats[10];
	uint32_t video_width;
	uint32_t video_height;

	// Mavlink WFB-ng
    bool enable_wfbng;
    int8_t wfb_rssi;
    uint16_t wfb_errors;
    uint16_t wfb_fec_fixed;
    int8_t wfb_flags;

	// Mavlink
    int enable_telemetry;
    int telemetry_level;
    float telemetry_altitude;
    float telemetry_pitch;
    float telemetry_roll;
    float telemetry_yaw;
    float telemetry_battery;
    float telemetry_current;
    float telemetry_current_consumed;
    double telemetry_lat;
    double telemetry_lon;
    double telemetry_lat_base;
    double telemetry_lon_base;
    double telemetry_hdg;
    double telemetry_distance;
    double s1_double;
    double s2_double;
    double s3_double;
    double s4_double;
    float telemetry_sats;
    float telemetry_gspeed;
    float telemetry_vspeed;
    float telemetry_rssi;
    float telemetry_throttle;
    float telemetry_raw_imu;
    float telemetry_resolution;
    float telemetry_arm;
    float armed;
    char c1[30];
    char c2[30];
    char s1[30];
    char s2[30];
    char s3[30];
    char s4[30];
    char* ptr;
};

extern struct osd_vars osd_vars;
extern int osd_thread_signal;
extern bool osd_custom_message;
extern pthread_mutex_t osd_mutex;

typedef struct {
	struct modeset_output *out;
	int fd;
} osd_thread_params;

#define TAG_MAX_LEN 64

typedef struct {
    char key[TAG_MAX_LEN];
    char val[TAG_MAX_LEN];
} osd_tag;

void *__OSD_THREAD__(void *param);

#ifdef __cplusplus
extern "C" {
#endif
void osd_publish_bool_fact(char const *name, osd_tag *tags, int n_tags, bool value);
void osd_publish_int_fact(char const *name, osd_tag *tags, int n_tags, int value);
void osd_publish_uint_fact(char const *name, osd_tag *tags, int n_tags, uint value);
void osd_publish_double_fact(char const *name, osd_tag *tags, int n_tags, double value);
void osd_publish_str_fact(char const *name, osd_tag *tags, int n_tags, char *value);
#ifdef __cplusplus
}
#endif

#endif
