/*
 * Fake fact source for the SDL simulator: publishes plausible link, video, system
 * and telemetry facts (same names, tags and types as the real publishers), so the
 * OSD widgets show live values and the OSD editor's fact picker has something to
 * offer without a drone or a radio.
 */
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "osd.h"

extern int dvr_enabled;

#define TICK_MS 100

static void tag(osd_tag * t, const char * key, const char * val)
{
    snprintf(t->key, sizeof(t->key), "%s", key);
    snprintf(t->val, sizeof(t->val), "%s", val);
}

/* slow random walk inside [lo, hi] */
static double wander(double v, double lo, double hi, double step)
{
    v += ((double)rand() / RAND_MAX - 0.5) * 2 * step;
    if(v < lo) v = lo;
    if(v > hi) v = hi;
    return v;
}

static void * sim_facts_thread(void * arg)
{
    (void)arg;
    static const char * ant_ids[] = { "0", "1", "256", "257" };
    double rssi[4] = { -48, -52, -60, -66 };
    double snr[4]  = { 28, 25, 20, 16 };
    double mbps = 12, temp_soc = 52000, temp_gpu = 48000, volts = 16400;
    int last_rec = -1;
    unsigned long n = 0;

    /* published once per stream by the decoder */
    osd_publish_uint_fact("video.width", NULL, 0, 1920);
    osd_publish_uint_fact("video.height", NULL, 0, 1080);
    osd_publish_str_fact("video.codec", NULL, 0, "h265");
    osd_publish_uint_fact("video.codec_id", NULL, 0, 1);
    osd_publish_str_fact("wfbcli.cli_title", NULL, 0, "WFB-ng simulator");

    for(;; n++) {
        double t = n * TICK_MS / 1000.0;
        void * batch = osd_batch_init(64);
        osd_tag tags[3];

        /* wfb-ng link */
        for(int i = 0; i < 4; i++) {
            rssi[i] = wander(rssi[i], -95, -38, 1.5);
            snr[i]  = wander(snr[i], 5, 35, 0.7);
            tag(&tags[0], "id", "video rx");
            tag(&tags[1], "ant_id", ant_ids[i]);
            osd_add_int_fact(batch, "wfbcli.rx.ant_stats.rssi_avg", tags, 2, lround(rssi[i]));
            osd_add_int_fact(batch, "wfbcli.rx.ant_stats.snr_avg", tags, 2, lround(snr[i]));
            osd_add_uint_fact(batch, "wfbcli.rx.ant_stats.mcs", tags, 2, 2);
        }
        tag(&tags[0], "id", "video rx");
        osd_add_uint_fact(batch, "wfbcli.rx.packets.fec_rec.delta", tags, 1, rand() % 6);
        osd_add_uint_fact(batch, "wfbcli.rx.packets.lost.delta", tags, 1, rand() % 10 == 0);
        osd_add_uint_fact(batch, "wfbcli.rx.packets.all.delta", tags, 1, 900 + rand() % 50);

        /* video: ~60 fps, decode latency, received bytes as ~1400 byte packets */
        for(int i = 0; i < 6; i++) osd_add_uint_fact(batch, "video.displayed_frame", NULL, 0, 1);
        osd_add_uint_fact(batch, "video.decode_and_handover_ms", NULL, 0, 8 + rand() % 6);
        mbps = wander(mbps, 4, 20, 0.5);
        osd_publish_batch(batch);

        long bytes = (long)(mbps * 125000 * TICK_MS / 1000);
        batch = osd_batch_init(bytes / 1400 + 1);
        for(; bytes > 0; bytes -= 1400)
            osd_add_uint_fact(batch, "gstreamer.received_bytes", NULL, 0, bytes > 1400 ? 1400 : bytes);
        osd_publish_batch(batch);

        /* ground station system, once per second like os_mon */
        if(n % 10 == 0) {
            batch = osd_batch_init(16);
            osd_add_uint_fact(batch, "os_mon.cpu.load_total", NULL, 0, 30 + rand() % 25);
            osd_add_uint_fact(batch, "os_mon.cpu.load_iowait", NULL, 0, rand() % 4);
            temp_soc = wander(temp_soc, 40000, 85000, 600);
            temp_gpu = wander(temp_gpu, 38000, 80000, 600);
            tag(&tags[0], "sensor", "thermal_zone0");
            tag(&tags[1], "name", "soc-thermal");
            osd_add_int_fact(batch, "os_mon.temperature", tags, 2, lround(temp_soc));
            tag(&tags[0], "sensor", "thermal_zone1");
            tag(&tags[1], "name", "gpu-thermal");
            osd_add_int_fact(batch, "os_mon.temperature", tags, 2, lround(temp_gpu));
            volts = wander(volts, 14000, 16800, 20);
            tag(&tags[0], "sensor", "hwmon0");
            osd_add_int_fact(batch, "os_mon.power.voltage", tags, 1, lround(volts));
            osd_add_int_fact(batch, "os_mon.power.current", tags, 1, 1400 + rand() % 200);
            osd_add_int_fact(batch, "os_mon.power.power", tags, 1, lround(volts * 1.5 * 1000));
            osd_publish_batch(batch);
        }

        /* drone telemetry (mavlink) */
        batch = osd_batch_init(16);
        tag(&tags[0], "sysid", "1");
        tag(&tags[1], "compid", "1");
        osd_add_double_fact(batch, "mavlink.attitude.roll", tags, 2, 0.3 * sin(t * 0.7));
        osd_add_double_fact(batch, "mavlink.attitude.pitch", tags, 2, 0.2 * sin(t * 0.5));
        osd_add_double_fact(batch, "mavlink.attitude.yaw", tags, 2, fmod(t * 0.2, 6.28) - 3.14);
        osd_add_double_fact(batch, "mavlink.vfr_hud.groundspeed", tags, 2, 12 + 3 * sin(t * 0.3));
        osd_add_double_fact(batch, "mavlink.vfr_hud.alt", tags, 2, 80 + 20 * sin(t * 0.1));
        osd_add_uint_fact(batch, "mavlink.vfr_hud.throttle", tags, 2, 45 + rand() % 10);
        osd_add_uint_fact(batch, "mavlink.sys_status.voltage_battery", tags, 2, lround(volts));
        osd_add_uint_fact(batch, "mavlink.gps_raw.fix_type", tags, 2, 3);
        osd_add_uint_fact(batch, "mavlink.gps_raw.satellites_visible", tags, 2, 14);
        osd_add_int_fact(batch, "mavlink.gps_raw.lat", tags, 2, 523700000 + lround(1000 * sin(t * 0.05)));
        osd_add_int_fact(batch, "mavlink.gps_raw.lon", tags, 2, 49000000 + lround(1000 * cos(t * 0.05)));
        osd_publish_batch(batch);

        /* DVR state follows the sim's 'r' key; a popup message every 20 s */
        if(dvr_enabled != last_rec) {
            last_rec = dvr_enabled;
            osd_publish_bool_fact("dvr.recording", NULL, 0, dvr_enabled != 0);
        }
        if(n % 200 == 50) osd_publish_str_fact("osd.custom_message", NULL, 0, "Hello from the simulator");

        usleep(TICK_MS * 1000);
    }
    return NULL;
}

void sim_facts_start(void)
{
    pthread_t tid;
    if(pthread_create(&tid, NULL, sim_facts_thread, NULL) == 0) pthread_detach(tid);
}
