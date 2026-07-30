/*
 * colmenu_pages — the full PixelPilot menu tree as data. This replaces the
 * lv_menu builders in ui.c / gs_*.c / air_*.c. Every parameter maps to the same
 * `gsmenu.sh get/set <type> <page> <param>` protocol the old menu used.
 *
 * Parameter pages are plain tables. Actions and WiFi are dynamic (build fns).
 * A few device screens (DVR-Player, TX-Profiles) are launched via the app's
 * existing screens; see colmenu_pages_open_*.
 */
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "colmenu.h"
#include "colmenu_pages.h"
#include "helper.h"       /* show_restart_notice(), find_first_focusable_obj() */
#include "../menu.h"      /* MenuAction, screens */
#include "../main.h"      /* sig_handler() — graceful shutdown / restart */
#include "../osd.h"       /* osd_flush_facts() — clear stale per-mode facts on RX mode switch */
#ifndef USE_SIMULATOR
#include "../drm.h"       /* gamma_lut_controller, gamma_lut_enable/disable */
#endif
#include "gs_scripts.h"   /* dynamic /media/dvr/scripts lookup + runner */

extern MenuAction airactions[]; extern size_t airactions_count;
extern MenuAction gsactions[];  extern size_t gsactions_count;
extern enum RXMode RXMODE;

/* Live Colortrans / Video scale apply their effect to the DRM/GL pipeline live,
 * as the old gs_system.c widget callbacks did — a gsmenu.sh set alone won't. */
extern bool  enable_live_colortrans;
extern float live_colortrans_gain;
extern float live_colortrans_offset;
#ifndef USE_SIMULATOR
extern gamma_lut_controller lut_ctrl;
void dvr_reenc_notify_colortrans(int enabled);
void drm_set_video_scale(float factor);
/* DVR backend live setters — mirror the old gs_system.c widget callbacks so a
 * change takes effect immediately (a gsmenu.sh set alone only applies on restart). */
void dvr_reenc_set_osd(int enabled);
void dvr_reenc_set_fps(int fps);
void dvr_reenc_set_bitrate(int kbps);
void dvr_reenc_set_codec(int idx);
void dvr_reenc_set_resolution(int idx);
void dvr_set_mode(int mode);
void dvr_set_max_size(int mb);
void dvr_set_raw_fps(int fps);
void dvr_start_all(void);
void dvr_stop_all(void);
#endif

static void on_live_colortrans(const char * value)
{
    bool on = value && strcmp(value, "on") == 0;
    if(on) {
#ifndef USE_SIMULATOR
        gamma_lut_enable(&lut_ctrl, live_colortrans_offset, live_colortrans_gain);
#endif
        enable_live_colortrans = true;
    } else {
#ifndef USE_SIMULATOR
        gamma_lut_disable(&lut_ctrl);
#endif
        enable_live_colortrans = false;
    }
    lv_obj_invalidate(lv_screen_active());
#ifndef USE_SIMULATOR
    dvr_reenc_notify_colortrans(on ? 1 : 0);
#endif
    printf("Live colortrans %s (offset=%f, gain=%f)\n",
           on ? "ENABLED" : "DISABLED", live_colortrans_offset, live_colortrans_gain);
    fflush(stdout);
}

static void on_video_scale(const char * value)
{
    float factor = value ? (float)atof(value) : 1.0f;
#ifndef USE_SIMULATOR
    drm_set_video_scale(factor);
#else
    printf("drm_set_video_scale(%.2f);\n", factor);
    fflush(stdout);
#endif
}

/* ── GS DVR live hooks ───────────────────────────────────────────────────────
 * These restore the "apply immediately" behaviour the old gs_system.c widget
 * callbacks had. Without them a change to a DVR setting only persists to config
 * (gsmenu.sh) and takes effect on the next restart. The dropdown value strings
 * come straight from gsmenu.sh's option lists. */
#ifdef USE_SIMULATOR
#define DVR_LIVE(call, fmt, ...) do { printf(fmt "\n", __VA_ARGS__); fflush(stdout); } while(0)
#else
#define DVR_LIVE(call, fmt, ...) do { call; } while(0)
#endif

static void on_rec_enabled(const char * value)   /* start/stop recording now */
{
    int on = value && strcmp(value, "on") == 0;
    if(on) DVR_LIVE(dvr_start_all(), "dvr_start_all()%s", "");
    else   DVR_LIVE(dvr_stop_all(),  "dvr_stop_all()%s",  "");
}
static void on_dvr_osd(const char * value)       /* burn OSD into the re-encode */
{
    int on = (value && strcmp(value, "on") == 0) ? 1 : 0;
    DVR_LIVE(dvr_reenc_set_osd(on), "dvr_reenc_set_osd(%d)", on);
}
static void on_dvr_mode(const char * value)      /* raw=0, reencode=1, both=2 */
{
    int mode = 0;
    if(value) { if(!strcmp(value, "reencode")) mode = 1; else if(!strcmp(value, "both")) mode = 2; }
    DVR_LIVE(dvr_set_mode(mode), "dvr_set_mode(%d)", mode);
}
static void on_dvr_max_size(const char * value)  /* param is in units of 100 MB */
{
    int mb = (value ? atoi(value) : 0) * 100;
    DVR_LIVE(dvr_set_max_size(mb), "dvr_set_max_size(%d)", mb);
}
static void on_rec_fps(const char * value)       /* raw recorder framerate */
{
    int fps = value ? atoi(value) : 0;
    if(fps > 0) DVR_LIVE(dvr_set_raw_fps(fps), "dvr_set_raw_fps(%d)", fps);
}
static void on_dvr_reenc_codec(const char * value)      /* h264=0, h265=1 */
{
    int idx = (value && strcmp(value, "h265") == 0) ? 1 : 0;
    DVR_LIVE(dvr_reenc_set_codec(idx), "dvr_reenc_set_codec(%d)", idx);
}
static void on_dvr_reenc_resolution(const char * value) /* 720p=0, 1080p=1 */
{
    int idx = (value && strcmp(value, "1080p") == 0) ? 1 : 0;
    DVR_LIVE(dvr_reenc_set_resolution(idx), "dvr_reenc_set_resolution(%d)", idx);
}
static void on_dvr_reenc_fps(const char * value)
{
    int fps = value ? atoi(value) : 0;
    if(fps > 0) DVR_LIVE(dvr_reenc_set_fps(fps), "dvr_reenc_set_fps(%d)", fps);
}
static void on_dvr_reenc_bitrate(const char * value)
{
    int kbps = value ? atoi(value) : 0;
    if(kbps > 0) DVR_LIVE(dvr_reenc_set_bitrate(kbps), "dvr_reenc_set_bitrate(%d)", kbps);
}

/* Full-screen editors reached from the menu (built by the app elsewhere). Each
 * has its own input group; on exit they restore a group we set before launch. */
extern lv_indev_t * indev_drv;
extern lv_obj_t   * dvr_screen;
extern lv_obj_t   * txprofiles_screen;
extern lv_group_t * dvr_group;               /* dvr player controls            */
extern lv_group_t * dvr_page_group;          /* Stop returns here              */
extern lv_group_t * tx_profile_group;        /* txprofiles table               */
extern lv_group_t * txprofiles_return_group; /* txprofiles back/save return    */
extern const char * dvr_template;
void change_playbutton_label(const char * label);
#ifndef USE_SIMULATOR
void switch_pipeline_source(const char * type, const char * path);
#endif

/* TX-Profiles editor lives in the drone ALink menu. */
static void open_txprofiles(void)
{
    txprofiles_return_group = lv_indev_get_group(indev_drv);
    lv_screen_load(txprofiles_screen);
    lv_indev_set_group(indev_drv, tx_profile_group);
    lv_obj_t * first = find_first_focusable_obj(txprofiles_screen);
    if(first) lv_group_focus_obj(first);
    /* swallow the release of the ENTER that opened this, so it doesn't
     * immediately click the focused (Back) button */
    lv_indev_wait_release(indev_drv);
}

static void notify_restart(const char * v) { (void)v; show_restart_notice(); }

/* Apply the receiver mode: set RXMODE and the env vars the rest of the app reads
 * (REMOTE_IP / AIR_FIRMWARE_TYPE). Called both at startup and on a mode change, so
 * the env is always in sync with the actual mode (the old gsmenu_toggle_rxmode()
 * set these from menu creation onward). */
static void apply_rx_mode(bool apfpv)
{
    bool changed = ((RXMODE == APFPV) != apfpv);
    RXMODE = apfpv ? APFPV : WFB;
    setenv("REMOTE_IP",         apfpv ? "192.168.0.1" : "10.5.0.10", 1);
    setenv("AIR_FIRMWARE_TYPE", apfpv ? "apfpv"       : "wfb",       1);
    /* Drop the previous mode's stale facts: wfbcli.* (the wfbcli thread only runs in
     * WFB) and os_mon.wifi.* (WiFiRSSIMonitor only runs in apfpv). The active source
     * re-publishes its own. Only these two prefixes - a blanket flush would also clear
     * facts nobody re-publishes, e.g. video.width/height (published once per decoder
     * frame-info change), leaving the VideoWidget stuck on "?x?". */
#ifndef USE_SIMULATOR
    if (changed) {
        static const char *const stale_prefixes[] = { "wfbcli.", "os_mon.wifi." };
        osd_flush_facts(stale_prefixes,                     // osd.cpp isn't in the sim build
                        (int)(sizeof(stale_prefixes) / sizeof(stale_prefixes[0])));
    }
#else
    (void)changed;
#endif
}

/* Receiver mode drives which link pages are shown. Read from the backend, kept
 * in the RXMODE global (shared with the rest of the app). */
static bool mode_is_apfpv(void)
{
    char * v = colmenu_get("gs", "system", "rx_mode", NULL);
    bool apfpv = v && strcmp(v, "apfpv") == 0;
    free(v);
    apply_rx_mode(apfpv);
    return apfpv;
}

/* Switching RX mode: apply the env the rest of the app expects and rebuild the
 * menu so the mode-specific pages appear. (Not the old gsmenu_toggle_rxmode(),
 * which pokes lv_menu objects that don't exist here.) */
static void on_rx_mode_change(const char * value)
{
    bool apfpv = value && strcmp(value, "apfpv") == 0;
    apply_rx_mode(apfpv);
    colmenu_rebuild();
}

/* ── Drone / air ───────────────────────────────────────────────────────────── */

static const colmenu_item_t air_wfbng_items[] = {
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Power",     .param="power" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Frequency", .param="air_channel" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Bandwidth", .param="width" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="MCS Index", .param="mcs_index" },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="STBC",      .param="stbc" },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="LDPC",      .param="ldpc" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="FEC_K",     .param="fec_k" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="FEC_N",     .param="fec_n" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="MLink",     .param="mlink" },
    { .kind=COLMENU_LABEL,    .label="Adaptive Link" },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="Enabled",   .param="adaptivelink" },
};
static const colmenu_page_t air_wfbng_page = { "WFB-NG", "air", "wfbng", air_wfbng_items, 11 };

static const colmenu_item_t air_alink_items[] = {
    { .kind=COLMENU_DROPDOWN, .label="Desired power output (0 pit, 4 highest)", .param="power_level_0_to_4" },
    { .kind=COLMENU_SLIDER,   .label="GS heartbeat lost fallback (ms)",         .param="fallback_ms" },
    { .kind=COLMENU_SLIDER,   .label="Keep link low for min (s)",               .param="hold_fallback_mode_s" },
    { .kind=COLMENU_SLIDER,   .label="Limit time between link changes (ms)",    .param="min_between_changes_ms" },
    { .kind=COLMENU_SLIDER,   .label="Wait before increasing link speed (s)",   .param="hold_modes_down_s" },
    { .kind=COLMENU_SLIDER,   .label="Hysteresis (%)",                          .param="hysteresis_percent" },
    { .kind=COLMENU_SLIDER,   .label="Hysteresis down (%)",                     .param="hysteresis_percent_down" },
    { .kind=COLMENU_SLIDER,   .label="exp_smoothing_factor",       .precision=1, .param="exp_smoothing_factor" },
    { .kind=COLMENU_SLIDER,   .label="exp_smoothing_factor_down",  .precision=1, .param="exp_smoothing_factor_down" },
    { .kind=COLMENU_SWITCH,   .label="Lost GS packet requests keyframe",        .param="allow_request_keyframe" },
    { .kind=COLMENU_SWITCH,   .label="tx_dropped requests keyframe",            .param="allow_rq_kf_by_tx_d" },
    { .kind=COLMENU_SLIDER,   .label="How often to check driver-xtx",           .param="check_xtx_period_ms" },
    { .kind=COLMENU_SLIDER,   .label="Limit between keyframe requests",         .param="request_keyframe_interval_ms" },
    { .kind=COLMENU_SWITCH,   .label="Keyframe at every link change",           .param="idr_every_change" },
    { .kind=COLMENU_DROPDOWN, .label="OSD Details",                             .param="osd_level" },
    { .kind=COLMENU_SLIDER,   .label="Font Size",                  .precision=1, .param="multiply_font_size_by" },
    { .kind=COLMENU_ACTION,   .icon=LV_SYMBOL_LIST, .label="TX-Profiles", .on_activate=open_txprofiles },
};
static const colmenu_page_t air_alink_page = { "ALink", "air", "alink", air_alink_items, 17 };

static const colmenu_item_t air_aalink_items[] = {
    { .kind=COLMENU_LABEL,    .label="WiFi Settings" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Channel",       .param="channel" },
    { .kind=COLMENU_LABEL,    .label="AALink Settings" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="VTX Power Output", .precision=1, .param="SCALE_TX_POWER" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="Link resilience (dB)", .param="THRESH_SHIFT" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="OSD Size",      .precision=1, .param="OSD_SCALE" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="OSD Level",     .param="OSD_LEVEL" },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="OSD Signal Bars", .param="SHOW_SIGNAL_BARS" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="Max Throughput (%)", .param="THROUGHPUT_PCT" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="Temp Throttle (°C)", .param="HIGH_TEMP" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="LQ Consideration", .param="MCS_SOURCE" },
};
static const colmenu_page_t air_aalink_page = { "AALink", "air", "aalink", air_aalink_items, 11 };

/* Camera sub-pages (all type=air page=camera) */
static const colmenu_item_t cam_video_items[] = {
    /* Size and FPS are WFB-specific; APFPV direct connection has fixed camera modes. */
    { .kind=COLMENU_DROPDOWN, .label="Size",       .param="size",       .mode_mask=COLMENU_MODE_WFB },
    { .kind=COLMENU_DROPDOWN, .label="Video Mode", .param="video_mode", .mode_mask=COLMENU_MODE_APFPV },
    { .kind=COLMENU_DROPDOWN, .label="FPS",        .param="fps",        .mode_mask=COLMENU_MODE_WFB },
    { .kind=COLMENU_DROPDOWN, .label="Bitrate",    .param="bitrate" },
    { .kind=COLMENU_DROPDOWN, .label="Codec",      .param="codec" },
    { .kind=COLMENU_SLIDER,   .label="Gopsize",    .param="gopsize" },
    { .kind=COLMENU_DROPDOWN, .label="RC Mode",    .param="rc_mode" },
};
static const colmenu_page_t cam_video_page = { "Video", "air", "camera", cam_video_items, 7 };
static const colmenu_item_t cam_image_items[] = {
    { .kind=COLMENU_SWITCH, .label="Mirror",     .param="mirror" },
    { .kind=COLMENU_SWITCH, .label="Flip",       .param="flip" },
    { .kind=COLMENU_SLIDER, .label="Contrast",   .param="contrast" },
    { .kind=COLMENU_SLIDER, .label="Hue",        .param="hue" },
    { .kind=COLMENU_SLIDER, .label="Saturation", .param="saturation" },
    { .kind=COLMENU_SLIDER, .label="Luminance",  .param="luminace" },
};
static const colmenu_page_t cam_image_page = { "Image", "air", "camera", cam_image_items, 6 };
static const colmenu_item_t cam_rec_items[] = {
    { .kind=COLMENU_SWITCH, .label="Enabled",  .param="rec_enable" },
    { .kind=COLMENU_SLIDER, .label="Split",    .param="rec_split" },
    { .kind=COLMENU_SLIDER, .label="Maxusage", .param="rec_maxusage" },
};
static const colmenu_page_t cam_rec_page = { "Recording", "air", "camera", cam_rec_items, 3 };
static const colmenu_item_t cam_isp_items[] = {
    { .kind=COLMENU_SLIDER,   .label="Exposure",    .param="exposure" },
    { .kind=COLMENU_DROPDOWN, .label="Antiflicker", .param="antiflicker" },
    { .kind=COLMENU_DROPDOWN, .label="Sensor File", .param="sensor_file" },
};
static const colmenu_page_t cam_isp_page = { "ISP", "air", "camera", cam_isp_items, 3 };
static const colmenu_item_t cam_fpv_items[] = {
    { .kind=COLMENU_SWITCH, .label="Enabled",    .param="fpv_enable" },
    { .kind=COLMENU_SLIDER, .label="Noiselevel", .param="noiselevel" },
};
static const colmenu_page_t cam_fpv_page = { "FPV", "air", "camera", cam_fpv_items, 2 };
static const colmenu_item_t camera_items[] = {
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_VIDEO,    .label="Video",     .sub=&cam_video_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_IMAGE,    .label="Image",     .sub=&cam_image_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_VIDEO,    .label="Recording", .sub=&cam_rec_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_EYE_OPEN, .label="ISP",       .sub=&cam_isp_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_GPS,      .label="FPV",       .sub=&cam_fpv_page },
};
static const colmenu_page_t camera_page = { "Camera", "air", "camera", camera_items, 5 };

static const colmenu_item_t air_tel_items[] = {
    { .kind=COLMENU_DROPDOWN, .label="Serial Port", .param="serial" },
    /* Router and MSPOSD are WFB-specific; not used in direct APFPV WiFi mode. */
    { .kind=COLMENU_DROPDOWN, .label="Router",      .param="router", .mode_mask=COLMENU_MODE_WFB },
    { .kind=COLMENU_LABEL,    .label="MSPOSD Settings", .mode_mask=COLMENU_MODE_WFB },
    { .kind=COLMENU_SLIDER,   .label="OSD FPS",     .param="osd_fps", .mode_mask=COLMENU_MODE_WFB },
    { .kind=COLMENU_SWITCH,   .label="GS Rendering", .param="gs_rendering" },
};
static const colmenu_page_t air_tel_page = { "Telemetry", "air", "telemetry", air_tel_items, 5 };

/* ── Actions (dynamic: Reboot + script-provided actions) ───────────────────── */

#ifdef USE_SIMULATOR
/* The simulator links simulator.c instead of main.cpp, so sig_handler() is
 * undefined there; provide a local one that just exits with the given code. */
void sig_handler(int code) { exit(code); }
#endif

/* Restart / Exit PixelPilot go through the app's own graceful-shutdown path:
 * sig_handler() sets return_value = code and trips signal_flag, so main() returns
 * that code. The pixelpilot.sh launcher loop restarts on any exit code except
 * 2/143/137 — hence exit(1) restarts PixelPilot and exit(2) quits it for good. */
static void run_restart_pp(void * c) { (void)c; sig_handler(1); }
static void run_exit_pp   (void * c) { (void)c; sig_handler(2); }

static void run_reboot_air(void * c) { (void)c; colmenu_exec("gsmenu.sh button air actions Reboot"); }
static void run_reboot_gs (void * c) { (void)c; colmenu_exec("gsmenu.sh button gs actions Reboot"); }
static void run_action(void * ctx)   { colmenu_exec((const char *)ctx); }

static void build_air_actions(colmenu_emit_t * e)
{
    colmenu_emit_action(e, LV_SYMBOL_POWER, "Reboot", run_reboot_air, NULL);
    for(size_t i = 0; i < airactions_count; i++)
        colmenu_emit_action(e, LV_SYMBOL_PLAY, airactions[i].label, run_action, strdup(airactions[i].action));
}
static void run_custom_script(void * ctx) { gs_scripts_run((const char *)ctx); }

static void build_gs_actions(colmenu_emit_t * e)
{
    colmenu_emit_action(e, LV_SYMBOL_REFRESH, "Restart PixelPilot", run_restart_pp, NULL);
    colmenu_emit_action(e, LV_SYMBOL_CLOSE,   "Exit PixelPilot",    run_exit_pp,    NULL);
    colmenu_emit_action(e, LV_SYMBOL_POWER, "Reboot", run_reboot_gs, NULL);
    for(size_t i = 0; i < gsactions_count; i++)
        colmenu_emit_action(e, LV_SYMBOL_PLAY, gsactions[i].label, run_action, strdup(gsactions[i].action));

    /* dynamic custom scripts scanned from /media/dvr/scripts */
    char * scripts[128];
    int n = gs_scripts_collect(scripts, 128);
    for(int i = 0; i < n; i++)      /* names are strdup'd; ownership passes to the row ctx */
        colmenu_emit_action(e, LV_SYMBOL_DIRECTORY, scripts[i], run_custom_script, scripts[i]);
}
static const colmenu_page_t air_actions_page = { .title="Actions", .type="air", .page="actions", .build=build_air_actions };
static const colmenu_page_t gs_actions_page  = { .title="Actions", .type="gs",  .page="actions", .build=build_gs_actions };

/* ── GS ────────────────────────────────────────────────────────────────────── */

static const colmenu_item_t gs_wfbng_items[] = {
    { .kind=COLMENU_LABEL,    .label="WFB-NG" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Channel",   .param="gs_channel" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Bandwidth", .param="bandwidth" },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="TXPower (%)", .param="txpower" },
    { .kind=COLMENU_LABEL,    .label="Adaptive Link" },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="Enabled",   .param="adaptivelink" },
};
static const colmenu_page_t gs_wfbng_page = { "WFB-NG", "gs", "wfbng", gs_wfbng_items, 6 };

/* System → Receiver / Display / DVR */
static const colmenu_item_t sys_receiver_items[] = {
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Codec",   .param="rx_codec" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="RX Mode", .param="rx_mode", .on_change=on_rx_mode_change },
};
static const colmenu_page_t sys_receiver_page = { "Receiver", "gs", "system", sys_receiver_items, 2 };
static const colmenu_item_t sys_display_items[] = {
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="GS Rendering",   .param="gs_rendering" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Connector",      .param="connector" },
    { .kind=COLMENU_DROPDOWN, .icon=LV_SYMBOL_SETTINGS, .label="Resolution",     .param="resolution", .on_change=notify_restart },
    { .kind=COLMENU_SLIDER,   .icon=LV_SYMBOL_SETTINGS, .label="Video scale factor", .precision=2, .param="video_scale", .on_change=on_video_scale, .on_live=on_video_scale },
    { .kind=COLMENU_SWITCH,   .icon=LV_SYMBOL_SETTINGS, .label="Live Colortrans", .param="gs_live_colortrans", .on_change=on_live_colortrans },
};
static const colmenu_page_t sys_display_page = { "Display", "gs", "system", sys_display_items, 5 };
static const colmenu_item_t sys_dvr_items[] = {
    { .kind=COLMENU_SWITCH,   .label="Enabled",           .param="rec_enabled",          .on_change=on_rec_enabled },
    { .kind=COLMENU_DROPDOWN, .label="Mode",              .param="dvr_mode",             .on_change=on_dvr_mode },
    { .kind=COLMENU_SLIDER,   .label="Max file size (MB)", .param="dvr_max_size",         .on_change=on_dvr_max_size, .display_scale=100 },
    { .kind=COLMENU_DROPDOWN, .label="Raw FPS",           .param="rec_fps",              .on_change=on_rec_fps },
    { .kind=COLMENU_DROPDOWN, .label="Codec",             .param="dvr_reenc_codec",      .on_change=on_dvr_reenc_codec },
    { .kind=COLMENU_DROPDOWN, .label="Resolution",        .param="dvr_reenc_resolution", .on_change=on_dvr_reenc_resolution },
    { .kind=COLMENU_DROPDOWN, .label="Re-encode FPS",     .param="dvr_reenc_fps",        .on_change=on_dvr_reenc_fps },
    { .kind=COLMENU_DROPDOWN, .label="Bitrate (kbps)",    .param="dvr_reenc_bitrate",    .on_change=on_dvr_reenc_bitrate },
    { .kind=COLMENU_SWITCH,   .label="Record OSD in DVR", .param="dvr_osd",              .on_change=on_dvr_osd },
};
static const colmenu_page_t sys_dvr_page = { "DVR", "gs", "system", sys_dvr_items, 9 };
static const colmenu_item_t system_items[] = {
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_WIFI,  .label="Receiver", .sub=&sys_receiver_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_IMAGE, .label="Display",  .sub=&sys_display_page },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_VIDEO, .label="DVR",      .sub=&sys_dvr_page },
};
static const colmenu_page_t system_page = { "System", "gs", "system", system_items, 3 };

/* WiFi. The WiFi page shows the live connection (get gs wifi ssid) — entering the
 * connected network gives Disconnect / Forget. "Networks" lists only AVAILABLE
 * networks to join: open ones and those whose password the GS already saved
 * connect straight away; other secured ones prompt for a password. There is no
 * standalone "set password" — the password is passed at connect time. */

static char g_wifi_ssid[128];       /* SSID awaiting a typed password              */
static char g_connected_ssid[128];  /* the network the connected-submenu acts on   */

static void wifi_connect_with_password(const char * password, void * user)
{
    (void)user;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gsmenu.sh set gs wifi connect \"%s\" \"%s\"", g_wifi_ssid, password);
    colmenu_exec(cmd);
}

typedef struct { char ssid[96]; bool secured; char saved[128]; } wifi_net_t;
static void connect_network(void * ctx)
{
    wifi_net_t * net = ctx;
    if(!net->secured || net->saved[0]) {          /* open, or password already known */
        char cmd[512];
        if(net->saved[0])
            snprintf(cmd, sizeof(cmd), "gsmenu.sh set gs wifi connect \"%s\" \"%s\"", net->ssid, net->saved);
        else
            snprintf(cmd, sizeof(cmd), "gsmenu.sh set gs wifi connect \"%s\"", net->ssid);
        colmenu_exec(cmd);
    } else {                                        /* secured, unknown → ask */
        snprintf(g_wifi_ssid, sizeof(g_wifi_ssid), "%s", net->ssid);
        char title[160];
        snprintf(title, sizeof(title), "Password for %s", net->ssid);
        colmenu_prompt(title, wifi_connect_with_password, NULL);
    }
}
static void wifi_rescan(void * ctx) { (void)ctx; colmenu_rescan(); }

/* Saved-networks blob is "SSID:password" per line ('\:' escapes a colon in the
 * SSID). Copy the password for `ssid` into `out` (empty if not saved). */
static void wifi_saved_password(const char * blob, const char * ssid, char * out, size_t outsz)
{
    out[0] = '\0';
    if(!blob || !*blob) return;
    char * dup = strdup(blob), * save = NULL;
    for(char * line = strtok_r(dup, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if(line[0] == '\0') continue;
        int len = (int)strlen(line), colon = -1;
        for(int j = 0; j < len; j++)
            if(line[j] == ':' && (j == 0 || line[j-1] != '\\')) { colon = j; break; }
        if(colon < 0) continue;
        const char * pwd = line + colon + 1;
        line[colon] = '\0';
        char name[96]; int si = 0, di = 0;
        while(line[si] && di < (int)sizeof(name) - 1) {
            if(line[si] == '\\' && line[si+1] == ':') { name[di++] = ':'; si += 2; }
            else name[di++] = line[si++];
        }
        name[di] = '\0';
        if(strcmp(name, ssid) == 0) { snprintf(out, outsz, "%s", pwd); break; }
    }
    free(dup);
}

/* Scan line format from `gsmenu.sh get gs wifi networks`:  SSID:SECURITY:SIGNAL
 * (literal ':' in the SSID escaped as '\:'; SECURITY "--" = open, else secured). */
static void build_networks(colmenu_emit_t * e)
{
    char * saved = colmenu_get("gs", "wifi", "savednetworks", NULL);
    char * out   = colmenu_get("gs", "wifi", "networks", NULL);
    int n = 0;
    if(out && *out) {
        char * save = NULL;
        for(char * line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if(line[0] == '\0') continue;
            /* split on the last two UNescaped colons: SSID : SECURITY : SIGNAL */
            int len = (int)strlen(line), last = -1, prev = -1;
            for(int i = 0; i < len; i++)
                if(line[i] == ':' && (i == 0 || line[i-1] != '\\')) { prev = last; last = i; }
            if(last < 0 || prev < 0) continue;
            const char * security = line + prev + 1;
            line[last] = '\0';
            line[prev] = '\0';
            char ssid[96]; int si = 0, di = 0;          /* unescape '\:' → ':' */
            while(line[si] && di < (int)sizeof(ssid) - 1) {
                if(line[si] == '\\' && line[si+1] == ':') { ssid[di++] = ':'; si += 2; }
                else ssid[di++] = line[si++];
            }
            ssid[di] = '\0';
            if(ssid[0] == '\0') continue;

            wifi_net_t * net = malloc(sizeof(*net));
            snprintf(net->ssid, sizeof(net->ssid), "%s", ssid);
            net->secured = (security[0] != '\0' && strcmp(security, "--") != 0);
            wifi_saved_password(saved, ssid, net->saved, sizeof(net->saved));

            const char * icon = !net->secured ? LV_SYMBOL_WIFI      /* open           */
                              : net->saved[0] ? LV_SYMBOL_OK         /* password known */
                              : LV_SYMBOL_EYE_CLOSE;                 /* needs password */
            colmenu_emit_action(e, icon, ssid, connect_network, net);
            n++;
        }
    }
    free(out);
    free(saved);
    if(n == 0) colmenu_emit_label(e, "No networks found");
    /* Always present + focusable — re-scan, and keeps an empty list navigable. */
    colmenu_emit_action(e, LV_SYMBOL_REFRESH, "Rescan", wifi_rescan, NULL);
}
/* `networks` triggers the (slow) iw scan; warm both it and the saved list on a
 * worker thread behind a spinner so opening/rescanning doesn't freeze the UI. */
static const char * const networks_prefetch[] = { "networks", "savednetworks", NULL };
static const colmenu_page_t networks_page = { .title="Networks", .type="gs", .page="wifi", .build=build_networks, .prefetch=networks_prefetch };

/* Connected-network submenu: Disconnect + Forget. (Forget is best-effort — the
 * backend may not implement `set gs wifi forget`.) */
static void wifi_disconnect(void * ctx) { (void)ctx; colmenu_exec("gsmenu.sh set gs wifi disconnect"); }
static void wifi_forget(void * ctx)
{
    (void)ctx;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gsmenu.sh set gs wifi forget \"%s\"", g_connected_ssid);
    colmenu_exec(cmd);
}
static void build_connected(colmenu_emit_t * e)
{
    colmenu_emit_value(e, LV_SYMBOL_WIFI,  "Network",    g_connected_ssid);
    colmenu_emit_action(e, LV_SYMBOL_CLOSE, "Disconnect", wifi_disconnect, NULL);
    colmenu_emit_action(e, LV_SYMBOL_TRASH, "Forget",     wifi_forget,     NULL);
}
static const colmenu_page_t wifi_connected_page = { .title="Connected", .type="gs", .page="wifi", .build=build_connected };

/* WiFi main page (dynamic — reflects the live connection state). */
static void build_wifi(colmenu_emit_t * e)
{
    char * ip = colmenu_get("gs", "wifi", "IP", NULL);
    colmenu_emit_value(e, LV_SYMBOL_SETTINGS, "IP", (ip && *ip) ? ip : "-");
    free(ip);

    char * ssid = colmenu_get("gs", "wifi", "ssid", NULL);
    if(ssid && *ssid) {                              /* connected → enter for actions */
        snprintf(g_connected_ssid, sizeof(g_connected_ssid), "%s", ssid);
        char lbl[160];
        snprintf(lbl, sizeof(lbl), LV_SYMBOL_OK " %s", ssid);
        colmenu_emit_submenu(e, LV_SYMBOL_WIFI, lbl, &wifi_connected_page);
    } else {
        colmenu_emit_value(e, LV_SYMBOL_WIFI, "Not connected", "");
    }
    free(ssid);

    colmenu_emit_submenu(e, LV_SYMBOL_WIFI, "Networks", &networks_page);
    colmenu_emit_switch (e, LV_SYMBOL_WIFI, "Hotspot", "gs", "wifi", "hotspot", NULL);
}
static const colmenu_page_t wifi_page = { .title="WiFi", .type="gs", .page="wifi", .build=build_wifi };

/* DVR-Player: a recordings browser (like the old create_gs_dvr_menu); selecting
 * a file starts playback on dvr_screen, and the player's Stop button returns. */
static void play_recording(void * ctx)
{
    dvr_page_group = lv_indev_get_group(indev_drv);   /* Stop returns to this list */
#ifndef USE_SIMULATOR
    switch_pipeline_source("file", (const char *)ctx);
#endif
    lv_screen_load(dvr_screen);
    lv_indev_set_group(indev_drv, dvr_group);
    lv_indev_wait_release(indev_drv);   /* don't let the ENTER release hit a control */
    change_playbutton_label(LV_SYMBOL_PAUSE);
}
static void build_dvr(colmenu_emit_t * e)
{
    char dir[256] = "./";
    if(dvr_template) {
        const char * sl = strrchr(dvr_template, '/');
        if(sl) { size_t n = sl - dvr_template + 1; if(n < sizeof(dir)) { memcpy(dir, dvr_template, n); dir[n] = '\0'; } }
    }
    colmenu_emit_label(e, "Recordings");
    int n = 0;
    DIR * d = opendir(dir);
    if(d) {
        struct dirent * ent;
        while((ent = readdir(d))) {
            char * ext = strrchr(ent->d_name, '.');
            if(ext && strcmp(ext, ".mp4") == 0) {
                char full[512];
                snprintf(full, sizeof(full), "%s%s", dir, ent->d_name);
                colmenu_emit_action(e, LV_SYMBOL_VIDEO, ent->d_name, play_recording, strdup(full));
                n++;
            }
        }
        closedir(d);
    }
    if(n == 0) colmenu_emit_label(e, "No recordings found");
}
static const colmenu_page_t dvr_page = { .title="DVR-Player", .type="gs", .page="dvr", .build=build_dvr };

/* GS APFPV connection page (shown in APFPV mode) */
static void reset_apfpv_action(void) { colmenu_exec("gsmenu.sh set gs apfpv reset"); }

/* Per-wlx-adapter enable + connection status, listed dynamically from the live
 * interface set (the old apfpv page did this inline). Enable toggles the adapter
 * (gsmenu.sh set gs apfpv <iface> on/off); status is read-only ("status <iface>"). */
static void build_apfpv_adapters(colmenu_emit_t * e)
{
    FILE * fp = popen("ip -o link show | awk -F': ' '{print $2}' | grep '^wlx' 2>/dev/null", "r");
    int n = 0;
    if(fp) {
        char line[128];
        while(fgets(line, sizeof(line), fp)) {
            line[strcspn(line, " \t\r\n")] = '\0';   /* trim to bare iface name */
            if(line[0] == '\0') continue;
            n++;
            char lbl[160];
            snprintf(lbl, sizeof(lbl), "Enable %s", line);
            colmenu_emit_switch(e, LV_SYMBOL_WIFI, lbl, "gs", "apfpv", line, NULL);

            char sparam[160];
            snprintf(sparam, sizeof(sparam), "status %s", line);
            char * st = colmenu_get("gs", "apfpv", sparam, NULL);
            colmenu_emit_value(e, LV_SYMBOL_SETTINGS, "Status", st ? st : "-");
            free(st);
        }
        pclose(fp);
    }
    if(n == 0) colmenu_emit_label(e, "No wlx adapters found");
}
static const colmenu_page_t apfpv_adapters_page = { .title="Adapters", .type="gs", .page="apfpv", .build=build_apfpv_adapters };

static const colmenu_item_t gs_apfpv_items[] = {
    { .kind=COLMENU_TEXT,    .icon=LV_SYMBOL_WIFI,      .label="SSID",        .param="ssid" },
    { .kind=COLMENU_TEXT,    .icon=LV_SYMBOL_EYE_CLOSE, .label="Password",    .param="password" },
    { .kind=COLMENU_SUBMENU, .icon=LV_SYMBOL_LIST,      .label="Adapters",    .sub=&apfpv_adapters_page },
    { .kind=COLMENU_ACTION,  .icon=LV_SYMBOL_REFRESH,   .label="Reset APFPV", .on_activate=reset_apfpv_action },
};
static const colmenu_page_t gs_apfpv_page = { "APFPV", "gs", "apfpv", gs_apfpv_items, 4 };

/* ── root (mode-dependent) ──────────────────────────────────────────────────── */

static void build_root(colmenu_emit_t * e)
{
    bool apfpv = (RXMODE == APFPV);   /* RXMODE is set from the backend at open,
                                         and updated live by on_rx_mode_change */

    /* Drone (air) pages are gated on VTX detection — greyed until the drone is
     * seen (mirrors the old check_connection_timer behaviour). */
    colmenu_emit_label(e, "Drone Settings");
    if(apfpv) {
        colmenu_emit_submenu_gated(e, LV_SYMBOL_WIFI, "AALink", &air_aalink_page);
    } else {
        colmenu_emit_submenu_gated(e, LV_SYMBOL_WIFI, "WFB-NG", &air_wfbng_page);
        colmenu_emit_submenu_gated(e, LV_SYMBOL_WIFI, "ALink",  &air_alink_page);
    }
    colmenu_emit_submenu_gated(e, LV_SYMBOL_IMAGE,    "Camera",    &camera_page);
    colmenu_emit_submenu_gated(e, LV_SYMBOL_DOWNLOAD, "Telemetry", &air_tel_page);
    colmenu_emit_submenu_gated(e, LV_SYMBOL_PLAY,     "Actions",   &air_actions_page);

    colmenu_emit_label(e, "GS Settings");
    if(apfpv) colmenu_emit_submenu(e, LV_SYMBOL_WIFI, "APFPV",  &gs_apfpv_page);
    else      colmenu_emit_submenu(e, LV_SYMBOL_WIFI, "WFB-NG", &gs_wfbng_page);
    colmenu_emit_submenu(e, LV_SYMBOL_VIDEO,    "DVR-Player",      &dvr_page);
    colmenu_emit_submenu(e, LV_SYMBOL_SETTINGS, "System Settings", &system_page);
    colmenu_emit_submenu(e, LV_SYMBOL_WIFI,     "WiFi",            &wifi_page);
    colmenu_emit_submenu(e, LV_SYMBOL_PLAY,     "Actions",         &gs_actions_page);
}
static const colmenu_page_t root_page = { .title="Menu", .build=build_root };

const colmenu_page_t * colmenu_pages_root(void)
{
    mode_is_apfpv();   /* initialise RXMODE from the backend before first build */
    return &root_page;
}
