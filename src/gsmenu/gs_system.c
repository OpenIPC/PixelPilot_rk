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
lv_obj_t * rec_enabled;
lv_obj_t * rec_fps;
lv_obj_t * vsync_disabled;
lv_obj_t * gs_request_idr;
lv_obj_t * video_scale;
lv_obj_t * gs_live_colortrans;
lv_obj_t * gs_dvr_colortrans;

// DVR re-encoder widgets
static lv_obj_t * dvr_reenc_enabled;
static lv_obj_t * dvr_reenc_codec;
static lv_obj_t * dvr_reenc_fps;
static lv_obj_t * dvr_reenc_bitrate;
static lv_obj_t * dvr_reenc_osd;

extern lv_obj_t * ap_fpv_ssid;
extern lv_obj_t * ap_fpv_password;

typedef struct Dvr* Dvr; // Forward declaration
void dvr_start_recording(Dvr* dvr);
void dvr_stop_recording(Dvr* dvr);
void dvr_set_video_framerate(Dvr* dvr,int f);
extern Dvr *dvr;
extern int dvr_enabled;
extern bool disable_vsync;
extern bool enable_live_colortrans;
extern float live_colortrans_gain;
extern float live_colortrans_offset;
extern gamma_lut_controller lut_ctrl;

// Live control shims for the DVR re-encoder (defined in main.cpp)
void dvr_reenc_notify_colortrans(int enabled);
void dvr_reenc_set_fps(int fps);
void dvr_reenc_set_osd(int enabled);
void dvr_reenc_set_bitrate(int kbps);
void dvr_reenc_set_codec(int idx);
void dvr_reenc_set_mode(int enabled);
int  dvr_reenc_get_fps(void);
int  dvr_reenc_get_bitrate(void);
int  dvr_reenc_is_reenc(void);
int  dvr_reenc_get_osd(void);
int  dvr_reenc_get_codec(void);

// Enable/disable widgets based on whether re-encoding is active.
// Re-enc ON  → raw DVR fps greyed out; re-encoder controls active.
// Re-enc OFF → raw DVR fps active;     re-encoder controls greyed out.
static void update_reenc_widget_states(void)
{
    if (!dvr_reenc_enabled) return;
    int reenc = dvr_reenc_is_reenc();

    lv_obj_t *raw_fps_dd = lv_obj_get_child_by_type(rec_fps,          0, &lv_dropdown_class);
    lv_obj_t *codec_dd   = lv_obj_get_child_by_type(dvr_reenc_codec,   0, &lv_dropdown_class);
    lv_obj_t *fps_dd     = lv_obj_get_child_by_type(dvr_reenc_fps,     0, &lv_dropdown_class);
    lv_obj_t *bitrate_dd = lv_obj_get_child_by_type(dvr_reenc_bitrate, 0, &lv_dropdown_class);
    lv_obj_t *osd_sw     = lv_obj_get_child_by_type(dvr_reenc_osd,     0, &lv_switch_class);

    if (reenc) {
        lv_obj_add_state(raw_fps_dd,   LV_STATE_DISABLED);
        lv_obj_clear_state(codec_dd,   LV_STATE_DISABLED);
        lv_obj_clear_state(fps_dd,     LV_STATE_DISABLED);
        lv_obj_clear_state(bitrate_dd, LV_STATE_DISABLED);
        lv_obj_clear_state(osd_sw,     LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(raw_fps_dd, LV_STATE_DISABLED);
        lv_obj_add_state(codec_dd,   LV_STATE_DISABLED);
        lv_obj_add_state(fps_dd,     LV_STATE_DISABLED);
        lv_obj_add_state(bitrate_dd, LV_STATE_DISABLED);
        lv_obj_add_state(osd_sw,     LV_STATE_DISABLED);
    }
}

void gs_system_page_load_callback(lv_obj_t * page)
{

    reload_dropdown_value(page,rx_codec);
    reload_switch_value(page,gs_rendering);
    reload_dropdown_value(page,rx_mode);
    RXMODE = lv_dropdown_get_selected(lv_obj_get_child_by_type(rx_mode,0,&lv_dropdown_class));
    reload_dropdown_value(page,connector);
    reload_dropdown_value(page,resolution);
    reload_dropdown_value(page,rec_fps);
    reload_slider_value(page, video_scale);
    if (dvr_enabled) lv_obj_add_state(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), LV_STATE_CHECKED);

    if (disable_vsync) lv_obj_add_state(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), LV_STATE_CHECKED);

    if (idr_get_enabled()) lv_obj_add_state(lv_obj_get_child_by_type(gs_request_idr,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(gs_request_idr,0,&lv_switch_class), LV_STATE_CHECKED);

    if (enable_live_colortrans) lv_obj_add_state(lv_obj_get_child_by_type(gs_live_colortrans,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(gs_live_colortrans,0,&lv_switch_class), LV_STATE_CHECKED);

    // DVR re-encoder
    if (dvr_reenc_is_reenc()) lv_obj_add_state(lv_obj_get_child_by_type(dvr_reenc_enabled,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(dvr_reenc_enabled,0,&lv_switch_class), LV_STATE_CHECKED);

    lv_obj_t * obj = lv_obj_get_child_by_type(dvr_reenc_codec,0,&lv_dropdown_class);
    lv_dropdown_set_selected(obj,dvr_reenc_get_codec());

    obj = lv_obj_get_child_by_type(dvr_reenc_fps,0,&lv_dropdown_class);
    char str[8];
    snprintf(str, sizeof(str), "%d", dvr_reenc_get_fps());
    int32_t index = lv_dropdown_get_option_index(obj,str);
    lv_dropdown_set_selected(obj,index);

    obj = lv_obj_get_child_by_type(dvr_reenc_bitrate,0,&lv_dropdown_class);
    snprintf(str, sizeof(str), "%d", dvr_reenc_get_bitrate());
    index = lv_dropdown_get_option_index(obj,str);
    lv_dropdown_set_selected(obj,index);

    if (dvr_reenc_get_osd()) lv_obj_add_state(lv_obj_get_child_by_type(dvr_reenc_osd,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(dvr_reenc_osd,0,&lv_switch_class), LV_STATE_CHECKED);

    update_reenc_widget_states();
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
            dvr_start_recording(dvr);
#else
            printf("dvr_start_recording(dvr);\n");
#endif
        } else {
#ifndef USE_SIMULATOR             
            dvr_stop_recording(dvr);
#else
            printf("dvr_stop_recording(dvr);\n");
#endif            
        }
    }
}


void disable_vsync_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        disable_vsync = lv_obj_has_state(ta, LV_STATE_CHECKED);
    }
}

void gs_request_idr_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        idr_set_enabled(lv_obj_has_state(ta, LV_STATE_CHECKED));
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
        dvr_set_video_framerate(dvr,fps);
#else
        printf("dvr_set_video_framerate(dvr,%i);\n",fps);
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
        // If the current FPS setting is higher, force it down to 30.
        if (osd_on) {
            lv_obj_t *fps_dd = lv_obj_get_child_by_type(dvr_reenc_fps, 0, &lv_dropdown_class);
            char cur[32] = "";
            lv_dropdown_get_selected_str(fps_dd, cur, sizeof(cur) - 1);
            if (atoi(cur) > 30) {
                int32_t idx = lv_dropdown_get_option_index(fps_dd, "30");
                if (idx >= 0) lv_dropdown_set_selected(fps_dd, (uint32_t)idx);
                dvr_reenc_set_fps(30);
                // Fire VALUE_CHANGED so the executor persists the new fps via gsmenu.sh
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

void dvr_reenc_mode_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        int enabled = lv_obj_has_state(ta, LV_STATE_CHECKED) ? 1 : 0;
#ifndef USE_SIMULATOR
        dvr_reenc_set_mode(enabled);
#else
        printf("dvr_reenc_set_mode(%d);\n", enabled);
#endif
        update_reenc_widget_states();
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
    menu_page_data->page_load_callback = gs_system_page_load_callback;
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

    vsync_disabled = create_switch(cont,LV_SYMBOL_SETTINGS,"Disable VSYNC","disable_vsync", NULL,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), disable_vsync_cb, LV_EVENT_VALUE_CHANGED,NULL);
    gs_request_idr = create_switch(cont,LV_SYMBOL_SETTINGS,"Request IDR","gs_request_idr", NULL,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_request_idr,0,&lv_switch_class), gs_request_idr_cb, LV_EVENT_VALUE_CHANGED,NULL);

    gs_live_colortrans = create_switch(cont,LV_SYMBOL_SETTINGS,"Live Colortrans","gs_live_colortrans", menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_live_colortrans,0,&lv_switch_class), gs_live_colortrans_cb, LV_EVENT_VALUE_CHANGED,NULL);

    create_text(parent, NULL, "Recording", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rec_enabled = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","rec_enabled", menu_page_data, true);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), rec_enabled_cb, LV_EVENT_VALUE_CHANGED,NULL);

    rec_fps = create_dropdown(section,LV_SYMBOL_SETTINGS, "Recording FPS", "","rec_fps",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_fps,0,&lv_dropdown_class), rec_fps_cb, LV_EVENT_VALUE_CHANGED,NULL);

    // ── Section: DVR Re-encoder ───────────────────────────────────────────────
    create_text(parent, NULL, "DVR Re-encoder", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    dvr_reenc_enabled = create_switch(cont, LV_SYMBOL_SETTINGS, "Re-encode","dvr_reenc_enabled", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_enabled,0,&lv_switch_class), dvr_reenc_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_codec   = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Codec", "","dvr_reenc_codec", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_codec,0,&lv_dropdown_class), dvr_reenc_codec_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_fps     = create_dropdown(cont, LV_SYMBOL_SETTINGS, "FPS", "","dvr_reenc_fps", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_fps,0,&lv_dropdown_class), dvr_reenc_fps_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_bitrate = create_dropdown(cont, LV_SYMBOL_SETTINGS, "Bitrate (kbps)", "","dvr_reenc_bitrate", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_bitrate,0,&lv_dropdown_class), dvr_reenc_bitrate_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dvr_reenc_osd     = create_switch(cont, LV_SYMBOL_SETTINGS, "Record OSD in DVR","dvr_osd", menu_page_data, false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(dvr_reenc_osd,0,&lv_switch_class), dvr_reenc_osd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    update_reenc_widget_states();

    lv_group_set_default(default_group);
}