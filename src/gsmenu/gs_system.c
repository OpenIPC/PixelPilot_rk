#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../main.h"
#ifndef USE_SIMULATOR
#include "../drm.h"
#endif
#include "../gstrtpreceiver.h"
#include "gs_system.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

extern lv_group_t * default_group;
extern lv_obj_t * menu;
extern lv_indev_t * indev_drv;
extern lv_obj_t * sub_gs_system_page;
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
#ifndef USE_SIMULATOR
extern gamma_lut_controller lut_ctrl;
#endif

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
#ifndef USE_SIMULATOR
void drm_set_video_scale(float factor);
#endif

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

// ── Custom reload functions ───────────────────────────────────────────────────

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
#ifndef USE_SIMULATOR
            gamma_lut_enable(&lut_ctrl, live_colortrans_offset, live_colortrans_gain);
#endif
            enable_live_colortrans = true;
            lv_obj_invalidate(lv_screen_active());
#ifndef USE_SIMULATOR
            dvr_reenc_notify_colortrans(1);
#endif
            printf("Live colortrans ENABLED (offset=%f, gain=%f)\n",
                         live_colortrans_offset, live_colortrans_gain);
        } else {
#ifndef USE_SIMULATOR
            gamma_lut_disable(&lut_ctrl);
#endif
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

void resolution_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        show_restart_notice();
}

void video_scale_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *slider = lv_event_get_target(e);
        float factor = lv_slider_get_value(slider) / 100.0f;
#ifndef USE_SIMULATOR
        drm_set_video_scale(factor);
#else
        printf("drm_set_video_scale(%.2f);\n", factor);
#endif
    }
}

/* Reverts DRM to the pre-edit scale when the user cancels the slider.
 * Reads start_value from the label's user_data if it's still there (we ran
 * before generic_back_event_handler), otherwise falls back to the current
 * slider value which generic_back_event_handler already restored. */
static void video_scale_revert_cb(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ESC) return;
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *label  = lv_obj_get_child_by_type(lv_obj_get_parent(slider), 1, &lv_label_class);
    thread_data_t *data = lv_obj_get_user_data(slider);

    int32_t *start_value = label ? lv_obj_get_user_data(label) : NULL;
    float factor;
    if (start_value)
        factor = *start_value / powf(10, data->precision);
    else
        factor = lv_slider_get_value(slider) / powf(10, data->precision);

#ifndef USE_SIMULATOR
    drm_set_video_scale(factor);
#else
    printf("drm_set_video_scale(%.2f) [revert]\n", factor);
#endif
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

// ── Sub-page back navigation ──────────────────────────────────────────────────

// Back handler for Receiver/Display/DVR sub-pages: go up to System Settings
// landing page rather than all the way to the main menu.
static void gs_system_sub_back_handler(lv_event_t * e) {
    lv_key_t key = lv_event_get_key(e);
    if (key == LV_KEY_HOME) {
        menu_page_data_t *sys_data = (menu_page_data_t *)lv_obj_get_user_data(sub_gs_system_page);
        lv_menu_set_page(menu, sub_gs_system_page);
        if (sys_data) lv_indev_set_group(indev_drv, sys_data->indev_group);
    }
}

// Replace generic_back_event_handler on the focusable inner child of a widget
// container with gs_system_sub_back_handler.
static void use_sub_back_handler(lv_obj_t *container) {
    lv_obj_t *inner = lv_obj_get_child_by_type(container, 0, &lv_dropdown_class);
    if (!inner) inner = lv_obj_get_child_by_type(container, 0, &lv_switch_class);
    if (!inner) inner = lv_obj_get_child_by_type(container, 0, &lv_slider_class);
    if (!inner) return;
    lv_obj_remove_event_cb(inner, generic_back_event_handler);
    lv_obj_add_event_cb(inner, gs_system_sub_back_handler, LV_EVENT_KEY, NULL);
}

// ── Sub-page create functions ─────────────────────────────────────────────────

void create_gs_system_receiver_menu(lv_obj_t * parent) {
    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent, menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "Receiver", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rx_codec = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Codec", "", "rx_codec", menu_page_data, false);
    use_sub_back_handler(rx_codec);
    rx_mode = create_dropdown(cont, LV_SYMBOL_SETTINGS, "RX Mode", "", "rx_mode", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rx_mode, 0, &lv_dropdown_class), rx_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(rx_mode);
    thread_data_t* data = lv_obj_get_user_data(lv_obj_get_child_by_type(rx_mode, 0, &lv_dropdown_class));
    data->arguments[0] = lv_obj_get_child_by_type(ap_fpv_ssid, 0, &lv_textarea_class);
    data->arguments[1] = lv_obj_get_child_by_type(ap_fpv_password, 0, &lv_textarea_class);
    reload_dropdown_value(parent, rx_mode);
    RXMODE = lv_dropdown_get_selected(lv_obj_get_child_by_type(rx_mode, 0, &lv_dropdown_class));

    add_entry_to_menu_page(menu_page_data, "Loading Codec ...",   rx_codec, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading RX Mode ...", rx_mode,  reload_rx_mode_fn);

    lv_group_set_default(default_group);
}

void create_gs_system_display_menu(lv_obj_t * parent) {
    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent, menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "Display", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    gs_rendering = create_switch(cont, LV_SYMBOL_SETTINGS, "GS Rendering", "gs_rendering", menu_page_data, false);
    use_sub_back_handler(gs_rendering);
    connector = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Connector", "", "connector", menu_page_data, false);
    use_sub_back_handler(connector);
    resolution = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Resolution", "", "resolution", menu_page_data, true);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(resolution, 0, &lv_dropdown_class), resolution_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(resolution);
    video_scale = create_slider(cont, LV_SYMBOL_SETTINGS, "Video scale factor", "video_scale", menu_page_data, false, 2);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(video_scale, 0, &lv_slider_class), video_scale_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(video_scale, 0, &lv_slider_class), video_scale_revert_cb, LV_EVENT_KEY, NULL);
    use_sub_back_handler(video_scale);
    gs_live_colortrans = create_switch(cont, LV_SYMBOL_SETTINGS, "Live Colortrans", "gs_live_colortrans", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_live_colortrans, 0, &lv_switch_class), gs_live_colortrans_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(gs_live_colortrans);

    add_entry_to_menu_page(menu_page_data, "Loading GS Rendering ...", gs_rendering,       reload_switch_value);
    add_entry_to_menu_page(menu_page_data, "Loading Connector ...",    connector,          reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Resolution ...",   resolution,         reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Video Scale ...",  video_scale,        reload_slider_value);
    add_entry_to_menu_page(menu_page_data, "Loading Colortrans ...",   gs_live_colortrans, reload_live_colortrans_fn);

    lv_group_set_default(default_group);
}

void create_gs_system_dvr_menu(lv_obj_t * parent) {
    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent, menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "DVR", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rec_enabled = create_switch(cont, LV_SYMBOL_SETTINGS, "Enabled", "rec_enabled", menu_page_data, true);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_enabled, 0, &lv_switch_class), rec_enabled_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(rec_enabled);

    dvr_mode_dd = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Mode", "", "dvr_mode", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_mode_dd, 0, &lv_dropdown_class), dvr_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_mode_dd);

    dvr_max_size = create_slider(cont, LV_SYMBOL_SETTINGS, "Max file size (MB)", "dvr_max_size", menu_page_data, false, 0);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_max_size, 0, &lv_slider_class), dvr_max_size_label_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_max_size, 0, &lv_slider_class), dvr_max_size_cb, LV_EVENT_CLICKED, NULL);
    use_sub_back_handler(dvr_max_size);

    rec_fps = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Raw FPS", "", "rec_fps", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_fps, 0, &lv_dropdown_class), rec_fps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(rec_fps);

    dvr_reenc_codec = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Codec", "", "dvr_reenc_codec", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_codec, 0, &lv_dropdown_class), dvr_reenc_codec_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_reenc_codec);
    dvr_reenc_resolution = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Resolution", "", "dvr_reenc_resolution", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_resolution, 0, &lv_dropdown_class), dvr_reenc_resolution_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_reenc_resolution);
    dvr_reenc_fps = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Re-encode FPS", "", "dvr_reenc_fps", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_fps, 0, &lv_dropdown_class), dvr_reenc_fps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_reenc_fps);
    dvr_reenc_bitrate = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Bitrate (kbps)", "", "dvr_reenc_bitrate", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_bitrate, 0, &lv_dropdown_class), dvr_reenc_bitrate_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_reenc_bitrate);
    dvr_reenc_osd = create_switch(cont, LV_SYMBOL_SETTINGS, "Record OSD in DVR", "dvr_osd", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_osd, 0, &lv_switch_class), dvr_reenc_osd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    use_sub_back_handler(dvr_reenc_osd);

    // dvr_mode_fn must be last: it calls update_dvr_mode_visibility() after all
    // DVR widgets have been loaded so none are skipped due to hidden state.
    add_entry_to_menu_page(menu_page_data, "Loading DVR enabled ...",    rec_enabled,          reload_rec_enabled_fn);
    add_entry_to_menu_page(menu_page_data, "Loading DVR max size ...",   dvr_max_size,         reload_dvr_max_size_fn);
    add_entry_to_menu_page(menu_page_data, "Loading Raw FPS ...",        rec_fps,              reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Codec ...",   dvr_reenc_codec,      reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Res ...",     dvr_reenc_resolution, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc FPS ...",     dvr_reenc_fps,        reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading Re-enc Bitrate ...", dvr_reenc_bitrate,    reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data, "Loading DVR OSD ...",        dvr_reenc_osd,        reload_dvr_reenc_osd_fn);
    add_entry_to_menu_page(menu_page_data, "Loading DVR Mode ...",       dvr_mode_dd,          reload_dvr_mode_fn);

    lv_group_set_default(default_group);
}

void create_gs_system_menu(lv_obj_t * parent, lv_obj_t * receiver_page, lv_obj_t * display_page, lv_obj_t * dvr_page) {
    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = NULL;
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent, menu_page_data);

    lv_obj_t * section;

    create_text(parent, NULL, "System Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    lv_obj_t * receiver_item = create_text(section, LV_SYMBOL_WIFI, "Receiver", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(menu_page_data->indev_group, receiver_item);
    lv_menu_set_load_page_event(menu, receiver_item, receiver_page);
    lv_obj_add_event_cb(receiver_item, generic_back_event_handler, LV_EVENT_KEY, NULL);

    lv_obj_t * display_item = create_text(section, LV_SYMBOL_IMAGE, "Display", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(menu_page_data->indev_group, display_item);
    lv_menu_set_load_page_event(menu, display_item, display_page);
    lv_obj_add_event_cb(display_item, generic_back_event_handler, LV_EVENT_KEY, NULL);

    lv_obj_t * dvr_item = create_text(section, LV_SYMBOL_VIDEO, "DVR", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(menu_page_data->indev_group, dvr_item);
    lv_menu_set_load_page_event(menu, dvr_item, dvr_page);
    lv_obj_add_event_cb(dvr_item, generic_back_event_handler, LV_EVENT_KEY, NULL);

    lv_group_set_default(default_group);
}
