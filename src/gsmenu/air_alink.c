#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "../input.h"

extern lv_group_t * default_group;
extern lv_indev_t * indev_drv;
extern gsmenu_control_mode_t control_mode;

extern lv_obj_t * txprofiles_screen;
extern lv_group_t * tx_profile_group;
lv_obj_t * txprofiles;

// # set desired power output (0 pitmode, 4 highest power)(scales with MCS)
lv_obj_t * power_level_0_to_4;
// ### if gs heartbeat lost for x ms, set link low (fallback) default 1000
lv_obj_t * fallback_ms;
// # keep link low for min x s default 1
lv_obj_t * hold_fallback_mode_s;
// ### limit time between any link change and the next default 200
lv_obj_t * min_between_changes_ms;
// # wait x seconds before increasing link speed default 3
lv_obj_t * hold_modes_down_s;

// ### smooth out rssi/snr readings for link increase / decrease
lv_obj_t * hysteresis_percent; // default 5
lv_obj_t * hysteresis_percent_down; // default 5
lv_obj_t * exp_smoothing_factor; // default 0.1
lv_obj_t * exp_smoothing_factor_down; // default 1.0

// ### allow lost GS packet to request new keyframe
lv_obj_t * allow_request_keyframe; // default 1
// # allow drone driver-tx_dropped to request new keyframe
lv_obj_t * allow_rq_kf_by_tx_d; // default 1
// # how often to check driver-xtx
lv_obj_t * check_xtx_period_ms; // default 2250
// # limit time between keyframe requests
lv_obj_t * request_keyframe_interval_ms; // default 1112
// # request a keyframe at every link change
lv_obj_t * idr_every_change; // default 0

// ### enable higher quality in center of image
// roi_focus_mode=0

// ### allow dynamic fec
// allow_dynamic_fec=0
// # by 1 decreasing k, or 0 increasing n
// fec_k_adjust=1
// # disable when bitrate is <= 4000
// spike_fix_dynamic_fec=0

// ### attempt to help encoder bitrate spikes by strategically lowering FPS when on high resolutions
// allow_spike_fix_fps=0
// # reduce bitrate on driver-tx dropped for 1 second or until xtx stops
// allow_xtx_reduce_bitrate=1
// # reduce bitrate to
// xtx_reduce_bitrate_factor=0.8

// ### how much info on OSD (0 to 6). 4 = all on one line. 6 = all on multiple lines 
lv_obj_t * osd_level; // default 0
// # make custom text smaller/bigger
lv_obj_t * multiply_font_size_by; // default 1

static void txprofiles_callback(lv_event_t * e)
{
    lv_screen_load(txprofiles_screen);
    lv_indev_set_group(indev_drv,tx_profile_group);
    lv_obj_t * first_obj = find_first_focusable_obj(txprofiles_screen);
    lv_group_focus_obj(first_obj);
}

void create_air_alink_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "alink");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    txprofiles = create_button(cont, "TX-Profiles");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(txprofiles,0,&lv_button_class),txprofiles_callback,LV_EVENT_CLICKED,NULL);

    power_level_0_to_4 = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Desired power output (0 pitmode, 4 highest power)","","power_level_0_to_4",menu_page_data,false);
    fallback_ms = create_slider(cont,LV_SYMBOL_SETTINGS,"If GS heartbeat lost for x ms, set link low (fallback) (ms)","fallback_ms",menu_page_data,false,0);
    hold_fallback_mode_s = create_slider(cont,LV_SYMBOL_SETTINGS,"Keep link low for min x s","hold_fallback_mode_s",menu_page_data,false,0);
    min_between_changes_ms = create_slider(cont,LV_SYMBOL_SETTINGS, "Limit time between any link change (ms)","min_between_changes_ms",menu_page_data,false,0);
    hold_modes_down_s = create_slider(cont,LV_SYMBOL_SETTINGS, "Wait x seconds before increasing link speed (s)","hold_modes_down_s",menu_page_data,false,0);
    hysteresis_percent = create_slider(cont,LV_SYMBOL_SETTINGS, "Hysteresis (%)","hysteresis_percent",menu_page_data,false,0);
    hysteresis_percent_down = create_slider(cont,LV_SYMBOL_SETTINGS, "Hysteresis down (%)","hysteresis_percent_down",menu_page_data,false,0);
    exp_smoothing_factor = create_slider(cont,LV_SYMBOL_SETTINGS, "exp_smoothing_factor","exp_smoothing_factor",menu_page_data,false,1);
    exp_smoothing_factor_down = create_slider(cont,LV_SYMBOL_SETTINGS, "exp_smoothing_factor_down","exp_smoothing_factor_down",menu_page_data,false,1);
    allow_request_keyframe = create_switch(cont,LV_SYMBOL_SETTINGS,"Allow lost GS packet to request new keyframe","allow_request_keyframe", menu_page_data,false);
    allow_rq_kf_by_tx_d = create_switch(cont,LV_SYMBOL_SETTINGS,"Allow drone driver-tx_dropped to request new keyframe","allow_rq_kf_by_tx_d", menu_page_data,false);
    check_xtx_period_ms = create_slider(cont,LV_SYMBOL_SETTINGS, "How often to check driver-xtx","check_xtx_period_ms",menu_page_data,false,0);
    request_keyframe_interval_ms = create_slider(cont,LV_SYMBOL_SETTINGS, "Limit time between keyframe requests","request_keyframe_interval_ms",menu_page_data,false,0);
    idr_every_change = create_switch(cont,LV_SYMBOL_SETTINGS,"Request a keyframe at every link change","idr_every_change", menu_page_data,false);
    osd_level = create_dropdown(cont,LV_SYMBOL_SETTINGS, "OSD Details 4 = all on one line. 6 = all on multiple lines","","osd_level",menu_page_data,false);
    multiply_font_size_by = create_slider(cont,LV_SYMBOL_SETTINGS, "Font Size","multiply_font_size_by",menu_page_data,false,1);

    add_entry_to_menu_page(menu_page_data,"Loading power_level_0_to_4 ...", power_level_0_to_4, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data,"Loading fallback_ms ...", fallback_ms, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading hold_fallback_mode_s ...", hold_fallback_mode_s, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading min_between_changes_ms ...", min_between_changes_ms, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading hold_modes_down_s ...", hold_modes_down_s, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading hysteresis_percent ...", hysteresis_percent, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading hysteresis_percent_down ...", hysteresis_percent_down, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading exp_smoothing_factor ...", exp_smoothing_factor, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading exp_smoothing_factor_down ...", exp_smoothing_factor_down, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading allow_request_keyframe ...", allow_request_keyframe, reload_switch_value);
    add_entry_to_menu_page(menu_page_data,"Loading allow_rq_kf_by_tx_d ...", allow_rq_kf_by_tx_d, reload_switch_value);
    add_entry_to_menu_page(menu_page_data,"Loading check_xtx_period_ms ...", check_xtx_period_ms, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading request_keyframe_interval_ms ...", request_keyframe_interval_ms, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading idr_every_change ...", idr_every_change, reload_switch_value);
    add_entry_to_menu_page(menu_page_data,"Loading osd_level ...", osd_level, reload_dropdown_value);
    add_entry_to_menu_page(menu_page_data,"Loading multiply_font_size_by ...", multiply_font_size_by, reload_slider_value);

    lv_group_set_default(default_group);
}