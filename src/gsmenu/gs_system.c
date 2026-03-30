#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main.h"
#include "../drm.h"
#include "../gstrtpreceiver.h"
#include "gs_system.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

extern lv_group_t * default_group;
enum RXMode RXMODE = WFB;

lv_obj_t * rx_codec;
lv_obj_t * rx_mode;
lv_obj_t * gs_rendering;
lv_obj_t * connector;
lv_obj_t * resolution;
lv_obj_t * video_scale;
lv_obj_t * gs_live_colortrans;
lv_obj_t * gs_dvr_colortrans;

// DVR widgets (unified section)
lv_obj_t * rec_enabled;
static lv_obj_t * dvr_mode_dd;
static lv_obj_t * dvr_max_size;
lv_obj_t * rec_fps;
static lv_obj_t * dvr_reenc_codec;
static lv_obj_t * dvr_reenc_fps;
static lv_obj_t * dvr_reenc_bitrate;
static lv_obj_t * dvr_reenc_resolution;
static lv_obj_t * dvr_reenc_osd;

extern lv_obj_t * ap_fpv_ssid;
extern lv_obj_t * ap_fpv_password;

extern int dvr_enabled;
extern bool enable_live_colortrans;
extern float live_colortrans_gain;
extern float live_colortrans_offset;
extern gamma_lut_controller lut_ctrl;

// C interface to DVR control (defined in main.cpp)
void dvr_reenc_notify_colortrans(int enabled);
void dvr_reenc_set_fps(int fps);
void dvr_reenc_set_osd(int enabled);
void dvr_reenc_set_bitrate(int kbps);
void dvr_reenc_set_codec(int idx);
void dvr_reenc_set_resolution(int idx);
void dvr_set_mode(int mode);
void dvr_start_all(void);
void dvr_stop_all(void);
int  dvr_get_mode(void);
int  dvr_reenc_get_fps(void);
int  dvr_reenc_get_bitrate(void);
int  dvr_reenc_get_osd(void);
int  dvr_reenc_get_codec(void);
int  dvr_reenc_get_resolution(void);
void dvr_set_max_size(int mb);
int  dvr_get_max_size(void);

// Raw FPS setter (defined in dvr.cpp)
typedef struct Dvr* Dvr;
void dvr_set_video_framerate(Dvr* dvr, int f);
extern Dvr *dvr_raw;

// Show/hide DVR widgets based on current mode.
// mode 0=raw: show raw fps, hide reenc options
// mode 1=reencode: hide raw fps, show reenc options
// mode 2=both: show all
static void update_dvr_mode_visibility(void)
{
    int mode = dvr_get_mode();
    bool show_raw   = (mode == 0 || mode == 2);
    bool show_reenc = (mode == 1 || mode == 2);

    if (show_raw) lv_obj_remove_flag(rec_fps, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(rec_fps, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *reenc_widgets[] = {dvr_reenc_codec, dvr_reenc_fps,
                                  dvr_reenc_bitrate, dvr_reenc_resolution, dvr_reenc_osd};
    for (int i = 0; i < 5; i++) {
        if (show_reenc) lv_obj_remove_flag(reenc_widgets[i], LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(reenc_widgets[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Custom reload functions for items not handled by gsmenu.sh get ───────────

static void reload_live_colortrans_fn(lv_obj_t *page, lv_obj_t *parameter) {
    lv_obj_t *sw = lv_obj_get_child_by_type(parameter, 0, &lv_switch_class);
    lv_lock();
    lv_obj_set_state(sw, LV_STATE_CHECKED, enable_live_colortrans);
    lv_unlock();
}

static void reload_rec_enabled_fn(lv_obj_t *page, lv_obj_t *parameter) {
    lv_obj_t *sw = lv_obj_get_child_by_type(parameter, 0, &lv_switch_class);
    lv_lock();
    lv_obj_set_state(sw, LV_STATE_CHECKED, dvr_enabled);
    lv_unlock();
}

static void reload_dvr_reenc_osd_fn(lv_obj_t *page, lv_obj_t *parameter) {
    lv_obj_t *sw = lv_obj_get_child_by_type(parameter, 0, &lv_switch_class);
    lv_lock();
    lv_obj_set_state(sw, LV_STATE_CHECKED, dvr_reenc_get_osd());
    lv_unlock();
}

static void reload_rx_mode_fn(lv_obj_t *page, lv_obj_t *parameter) {
    reload_dropdown_value(page, parameter);
    lv_lock();
    RXMODE = lv_dropdown_get_selected(lv_obj_get_child_by_type(parameter, 0, &lv_dropdown_class));
    lv_unlock();
}

// dvr_max_size: slider position = max_size/100, label shows actual MB (pos*100).
// reload_slider_value would overwrite the label with the raw position, so use a
// custom function that displays slider_pos * 100 as the label.
static void reload_dvr_max_size_fn(lv_obj_t *page, lv_obj_t *parameter) {
    lv_obj_t *slider = lv_obj_get_child_by_type(parameter, 0, &lv_slider_class);
    lv_obj_t *label  = lv_obj_get_child_by_type(parameter, 1, &lv_label_class);
    thread_data_t *param_user_data = (thread_data_t*)lv_obj_get_user_data(slider);
    char *raw = get_paramater(page, param_user_data->parameter);
    char *values_str = NULL;
    char *value = split_value_and_options(raw, &values_str);
    int slider_pos = atoi(value);
    if (values_str) {
        float min, max;
        if (sscanf(values_str, "%f %f", &min, &max) == 2) {
            lv_lock();
            lv_slider_set_range(slider, (int32_t)min, (int32_t)max);
            lv_unlock();
        }
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", slider_pos * 100);
    lv_lock();
    lv_slider_set_value(slider, slider_pos, LV_ANIM_OFF);
    lv_label_set_text(label, buf);
    lv_unlock();
}

// dvr_mode must be last among DVR entries so visibility is applied after all
// DVR items have been loaded.
static void reload_dvr_mode_fn(lv_obj_t *page, lv_obj_t *parameter) {
    reload_dropdown_value(page, parameter);
    lv_lock();
    update_dvr_mode_visibility();
    lv_unlock();
}

void toggle_rec_enabled()
{
    lv_obj_t * rec_switch = lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class);
    if (lv_obj_has_state(rec_switch, LV_STATE_CHECKED)) lv_obj_clear_state(rec_switch, LV_STATE_CHECKED);
    else lv_obj_add_state(rec_switch, LV_STATE_CHECKED);
    lv_obj_send_event(rec_switch, LV_EVENT_VALUE_CHANGED, NULL);
}

void rec_enabled_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        if (lv_obj_has_state(ta, LV_STATE_CHECKED)) {
#ifndef USE_SIMULATOR
            dvr_start_all();
#else
            printf("dvr_start_all();\n");
#endif
        } else {
#ifndef USE_SIMULATOR
            dvr_stop_all();
#else
            printf("dvr_stop_all();\n");
#endif
        }
    }
}


void gs_live_colortrans_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);

        if (lv_obj_has_state(ta, LV_STATE_CHECKED)) {
            gamma_lut_enable(&lut_ctrl, live_colortrans_offset, live_colortrans_gain);
            enable_live_colortrans = true;
            lv_obj_invalidate(lv_screen_active());
#ifndef USE_SIMULATOR
            dvr_reenc_notify_colortrans(1);
#endif
            printf("Live colortrans ENABLED (offset=%f, gain=%f)\n",
                         live_colortrans_offset, live_colortrans_gain);
        } else {
            gamma_lut_disable(&lut_ctrl);
            enable_live_colortrans = false;
            lv_obj_invalidate(lv_screen_active());
#ifndef USE_SIMULATOR
            dvr_reenc_notify_colortrans(0);
#endif
            printf("Live colortrans DISABLED\n");
        }
    }
}

void rec_fps_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        char val[100] = "";
        lv_dropdown_get_selected_str(ta,val,99);
        int fps = atoi(val);
#ifndef USE_SIMULATOR
        dvr_set_video_framerate(dvr_raw, fps);
#else
        printf("dvr_set_video_framerate(dvr_raw,%i);\n",fps);
#endif
    }
}

void dvr_reenc_fps_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        char val[32] = "";
        lv_dropdown_get_selected_str(ta, val, sizeof(val) - 1);
        int fps = atoi(val);
        if (fps > 0) {
#ifndef USE_SIMULATOR
            // Hardware limit: OSD blend + re-encoding is capped at 30fps.
            if (fps > 30 && dvr_reenc_get_osd()) {
                fps = 30;
                int32_t idx = lv_dropdown_get_option_index(ta, "30");
                if (idx >= 0) lv_dropdown_set_selected(ta, (uint32_t)idx);
            }
            dvr_reenc_set_fps(fps);
#else
            printf("dvr_reenc_set_fps(%d);\n", fps);
#endif
        }
    }
}

void dvr_reenc_osd_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        int osd_on = lv_obj_has_state(ta, LV_STATE_CHECKED) ? 1 : 0;
#ifndef USE_SIMULATOR
        dvr_reenc_set_osd(osd_on);
        // Hardware limit: OSD blend caps re-encoding at 30fps.
        if (osd_on) {
            lv_obj_t *fps_dd = lv_obj_get_child_by_type(dvr_reenc_fps, 0, &lv_dropdown_class);
            char cur[32] = "";
            lv_dropdown_get_selected_str(fps_dd, cur, sizeof(cur) - 1);
            if (atoi(cur) > 30) {
                int32_t idx = lv_dropdown_get_option_index(fps_dd, "30");
                if (idx >= 0) lv_dropdown_set_selected(fps_dd, (uint32_t)idx);
                dvr_reenc_set_fps(30);
                lv_obj_send_event(fps_dd, LV_EVENT_VALUE_CHANGED, NULL);
            }
        }
#else
        printf("dvr_reenc_set_osd(%d);\n", osd_on);
#endif
    }
}

void dvr_reenc_bitrate_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        char val[32] = "";
        lv_dropdown_get_selected_str(ta, val, sizeof(val) - 1);
        int kbps = atoi(val);
        if (kbps > 0) {
#ifndef USE_SIMULATOR
            dvr_reenc_set_bitrate(kbps);
#else
            printf("dvr_reenc_set_bitrate(%d);\n", kbps);
#endif
        }
    }
}

void dvr_reenc_codec_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        int idx = lv_dropdown_get_selected(ta); // 0=h264, 1=h265
#ifndef USE_SIMULATOR
        dvr_reenc_set_codec(idx);
#else
        printf("dvr_reenc_set_codec(%d);\n", idx);
#endif
    }
}

void dvr_reenc_resolution_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        int idx = lv_dropdown_get_selected(ta); // 0=720p, 1=1080p
#ifndef USE_SIMULATOR
        dvr_reenc_set_resolution(idx);
#else
        printf("dvr_reenc_set_resolution(%d);\n", idx);
#endif
    }
}

void dvr_max_size_label_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child_by_type(lv_obj_get_parent(slider), 1, &lv_label_class);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", lv_slider_get_value(slider) * 100);
    lv_label_set_text(label, buf);
}

void dvr_max_size_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int mb = lv_slider_get_value(slider) * 100;
#ifndef USE_SIMULATOR
    dvr_set_max_size(mb);
#else
    printf("dvr_set_max_size(%d);\n", mb);
#endif
}

void dvr_mode_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        int mode = lv_dropdown_get_selected(ta); // 0=raw, 1=reencode, 2=both
#ifndef USE_SIMULATOR
        dvr_set_mode(mode);
#else
        printf("dvr_set_mode(%d);\n", mode);
#endif
        update_dvr_mode_visibility();
    }
}

void rx_mode_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        RXMODE = lv_dropdown_get_selected(ta);
        gsmenu_toggle_rxmode();
    }
}

void create_gs_system_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * label;
    lv_obj_t * section;
    lv_obj_t * obj;

    create_text(parent, NULL, "General", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rx_codec = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Codec","","rx_codec",menu_page_data,false);
    rx_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "RX Mode","","rx_mode",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rx_mode,0,&lv_dropdown_class), rx_mode_cb, LV_EVENT_VALUE_CHANGED,NULL);
    thread_data_t* data = lv_obj_get_user_data(lv_obj_get_child_by_type(rx_mode,0,&lv_dropdown_class));
    data->arguments[0] = lv_obj_get_child_by_type(ap_fpv_ssid,0,&lv_textarea_class);
    data->arguments[1] = lv_obj_get_child_by_type(ap_fpv_password,0,&lv_textarea_class);
    reload_dropdown_value(parent,rx_mode);
    RXMODE = lv_dropdown_get_selected(lv_obj_get_child_by_type(rx_mode,0,&lv_dropdown_class));

    gs_rendering = create_switch(cont,LV_SYMBOL_SETTINGS,"GS Rendering","gs_rendering", menu_page_data,false);
    connector = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Connector","","connector",menu_page_data,false);
    resolution = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Resolution","","resolution",menu_page_data,false);
    video_scale = create_slider(cont, LV_SYMBOL_SETTINGS, "Video scale factor", "video_scale", menu_page_data, false, 2);

    gs_live_colortrans = create_switch(cont,LV_SYMBOL_SETTINGS,"Live Colortrans","gs_live_colortrans", menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_live_colortrans,0,&lv_switch_class), gs_live_colortrans_cb, LV_EVENT_VALUE_CHANGED,NULL);

    // ── Section: DVR (unified) ──────────────────────────────────────────────
    create_text(parent, NULL, "DVR", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rec_enabled = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","rec_enabled", menu_page_data, true);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), rec_enabled_cb, LV_EVENT_VALUE_CHANGED,NULL);

    dvr_mode_dd = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Mode", "","dvr_mode", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_mode_dd,0,&lv_dropdown_class), dvr_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    dvr_max_size = create_slider(cont, LV_SYMBOL_SETTINGS, "Max file size (MB)", "dvr_max_size", menu_page_data, false, 0);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_max_size,0,&lv_slider_class), dvr_max_size_label_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_max_size,0,&lv_slider_class), dvr_max_size_cb, LV_EVENT_CLICKED, NULL);

    rec_fps = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Raw FPS", "","rec_fps",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_fps,0,&lv_dropdown_class), rec_fps_cb, LV_EVENT_VALUE_CHANGED,NULL);

    dvr_reenc_codec   = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Codec", "","dvr_reenc_codec", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_codec,0,&lv_dropdown_class), dvr_reenc_codec_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_resolution = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Resolution", "","dvr_reenc_resolution", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_resolution,0,&lv_dropdown_class), dvr_reenc_resolution_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_fps     = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Re-encode FPS", "","dvr_reenc_fps", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_fps,0,&lv_dropdown_class), dvr_reenc_fps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_bitrate = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Bitrate (kbps)", "","dvr_reenc_bitrate", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_bitrate,0,&lv_dropdown_class), dvr_reenc_bitrate_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_osd     = create_switch(cont, LV_SYMBOL_SETTINGS, "Record OSD in DVR","dvr_osd", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_osd,0,&lv_switch_class), dvr_reenc_osd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Register entries for the generic threaded page loader.
    // dvr_mode_fn must be last among DVR items: it calls update_dvr_mode_visibility()
    // after all dvr widgets have been loaded so none are skipped due to hidden state.
    add_entry_to_menu_page(menu_page_data, "Loading Codec ...",         rx_codec,          reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading RX Mode ...",       rx_mode,           reload_rx_mode_fn);
    add_entry_to_menu_page(menu_page_data, "Loading GS Rendering ...",  gs_rendering,      reload_switch_value);
    add_entry_to_menu_page(menu_page_data, "Loading Connector ...",     connector,         reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Resolution ...",    resolution,        reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Video Scale ...",   video_scale,       reload_slider_value);
    add_entry_to_menu_page(menu_page_data, "Loading Colortrans ...",    gs_live_colortrans, reload_live_colortrans_fn);
    add_entry_to_menu_page(menu_page_data, "Loading DVR enabled ...",   rec_enabled,       reload_rec_enabled_fn);
    add_entry_to_menu_page(menu_page_data, "Loading DVR max size ...",  dvr_max_size,      reload_dvr_max_size_fn);
    add_entry_to_menu_page(menu_page_data, "Loading Raw FPS ...",       rec_fps,           reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Codec ...",  dvr_reenc_codec,   reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Res ...",    dvr_reenc_resolution, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc FPS ...",    dvr_reenc_fps,     reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Bitrate ...", dvr_reenc_bitrate, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading DVR OSD ...",       dvr_reenc_osd,     reload_dvr_reenc_osd_fn);
    add_entry_to_menu_page(menu_page_data, "Loading DVR Mode ...",      dvr_mode_dd,       reload_dvr_mode_fn);

    lv_group_set_default(default_group);
}
