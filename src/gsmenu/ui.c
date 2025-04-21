#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../lvgl/lvgl.h"

#include "images.h"
#include "helper.h"
#include "air_wfbng.h"
#include "air_camera.h"
#include "air_telemetry.h"
#include "air_actions.h"
#include "gs_main.h"
#include "gs_wfbng.h"
#include "gs_system.h"
#include "gs_wifi.h"
#include "gs_actions.h"
#include "styles.h"

static void back_event_handler(lv_event_t * e);
extern lv_obj_t * menu;
extern lv_indev_t * indev_drv;
lv_obj_t * root_page;
lv_group_t *main_group;
extern lv_group_t *default_group;
extern lv_obj_t * pp_osd_screen;

lv_obj_t * sub_gs_main_page;
lv_obj_t * sub_air_wfbng_page;
lv_obj_t * sub_air_camera_page;
lv_obj_t * sub_air_telemetry_page;
lv_obj_t * sub_air_actions_page;
lv_obj_t * sub_gs_wfbng_page;
lv_obj_t * sub_gs_system_page;
lv_obj_t * sub_wlan_page;
lv_obj_t * sub_gs_actions_page;

lv_obj_t * air_wfbng_cont;
lv_obj_t * air_camera_cont;
lv_obj_t * air_telemetry_cont;
lv_obj_t * air_actions_cont;
lv_obj_t * gs_wfbng_cont;
lv_obj_t * gs_system_cont;
lv_obj_t * gs_wlan_cont;
lv_obj_t * gs_actions_cont;

extern bool menu_active;
extern uint64_t gtotal_tunnel_data; // global variable for easyer access in gsmenu
uint64_t last_count = 0;

static int last_value = 0;
static uint32_t last_increase_time = 0;
static bool objects_active = true;

void recursive_state_set(lv_obj_t *obj, bool enable) {
    if (!obj) return;

    // Set state for the current object
    if (enable) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }

    // Recursively process all children
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        recursive_state_set(child, enable);
    }
}

void check_connection_timer(lv_timer_t * timer)
{
    static uint32_t last_value = 0;
    static uint32_t last_increase_time = 0;

    uint32_t current_value = (uint32_t)gtotal_tunnel_data;
    
    // Reset detection (either manual reset or wraparound)
    bool is_reset = false;
    
    // Case 1: Simple decrease (manual reset)
    if (current_value < last_value) {
        is_reset = true;
    }
    // Case 2: Wraparound detection (for unsigned counters)
    else if ((last_value > UINT32_MAX - 1000) && (current_value < 1000)) {
        is_reset = true;
    }
    
    if (is_reset) {
        if (objects_active) {
            recursive_state_set(air_wfbng_cont, false);
            recursive_state_set(air_camera_cont, false);
            recursive_state_set(air_telemetry_cont, false);
            recursive_state_set(air_actions_cont, false);
            recursive_state_set(sub_air_wfbng_page, false);
            recursive_state_set(sub_air_camera_page, false);
            recursive_state_set(sub_air_telemetry_page, false);
            recursive_state_set(sub_air_actions_page, false);
            setenv("GSMENU_VTX_DETECTED" , "0", 1);
            lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
            if (sub_air_wfbng_page == current_page ||
                sub_air_camera_page == current_page ||
                sub_air_telemetry_page == current_page ||
                sub_air_actions_page == current_page
                ) {
                    lv_indev_set_group(indev_drv,main_group);
                }

            objects_active = false;
        }
        last_value = current_value;
        return;
    }
    
    // Normal increase detection
    if (current_value > last_value) {
        last_increase_time = lv_tick_get();
        last_value = current_value;
        
        if (!objects_active) {
            recursive_state_set(air_wfbng_cont, true);
            recursive_state_set(air_camera_cont, true);
            recursive_state_set(air_telemetry_cont, true);
            recursive_state_set(air_actions_cont, true);
            recursive_state_set(sub_air_wfbng_page, true);
            recursive_state_set(sub_air_camera_page, true);
            recursive_state_set(sub_air_telemetry_page, true);
            recursive_state_set(sub_air_actions_page, true);
            setenv("GSMENU_VTX_DETECTED" , "1", 1);

            lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
            if (sub_air_wfbng_page == current_page ||
                sub_air_camera_page == current_page ||
                sub_air_telemetry_page == current_page ||
                sub_air_actions_page == current_page
                ) {
                    menu_page_data_t* menu_page_data = (menu_page_data_t*) lv_obj_get_user_data(current_page);
                    lv_indev_set_group(indev_drv,menu_page_data->indev_group);
                }
            objects_active = true;
        }
    }
    // Timeout detection
    else if (objects_active && (lv_tick_elaps(last_increase_time) > 2000)) {
        recursive_state_set(air_wfbng_cont, false);
        recursive_state_set(air_camera_cont, false);
        recursive_state_set(air_telemetry_cont, false);
        recursive_state_set(air_actions_cont, false);
        recursive_state_set(sub_air_wfbng_page, false);
        recursive_state_set(sub_air_camera_page, false);
        recursive_state_set(sub_air_telemetry_page, false);
        recursive_state_set(sub_air_actions_page, false);
        setenv("GSMENU_VTX_DETECTED" , "0", 1);

        lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
        if (sub_air_wfbng_page == current_page ||
            sub_air_camera_page == current_page ||
            sub_air_telemetry_page == current_page ||
            sub_air_actions_page == current_page
            ) {
                lv_indev_set_group(indev_drv,main_group);
            }

        objects_active = false;
    }
}

lv_obj_t * pp_header_create(lv_obj_t * screen) {

    lv_obj_t *header = lv_obj_create(screen);

    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_width(header,LV_PCT(100));
    lv_obj_set_height(header,LV_PCT(20));
    lv_obj_set_size(header, LV_PCT(100), LV_PCT(20));
    lv_obj_set_style_bg_color(header, lv_color_hex(0xff4c60d8), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *obj = lv_img_create(header);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_img_set_src(obj, &img_open_ipc_logo);
    lv_img_set_zoom(obj, 20);

}

lv_obj_t * pp_menu_create(lv_obj_t * screen)
{
    main_group = lv_group_create();
    lv_group_set_default(main_group);
    lv_indev_set_group(indev_drv,main_group);

    menu = lv_menu_create(screen);
    lv_obj_set_pos(menu, 0, LV_PCT(20));
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(80));
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
    lv_obj_add_style(menu, &style_rootmenu, LV_PART_MAIN);

    lv_obj_add_event_cb(menu, back_event_handler, LV_EVENT_CLICKED, menu);
    lv_obj_add_event_cb(menu, handle_sub_page_load, LV_EVENT_VALUE_CHANGED, menu);

    lv_obj_t * section;

    /*Create sub pages*/
    sub_gs_main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_hor(sub_gs_main_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_gs_main_page);
    create_main_menu(sub_gs_main_page);

    sub_air_wfbng_page = lv_menu_page_create(menu, LV_SYMBOL_WIFI" WFB-NG");
    lv_obj_set_style_pad_hor(sub_air_wfbng_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_air_wfbng_page);
    create_air_wfbng_menu(sub_air_wfbng_page);

    sub_air_camera_page = lv_menu_page_create(menu, LV_SYMBOL_IMAGE" Camera");
    lv_obj_set_style_pad_hor(sub_air_camera_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_air_camera_page);
    create_air_camera_menu(sub_air_camera_page);

    sub_air_telemetry_page = lv_menu_page_create(menu, LV_SYMBOL_DOWNLOAD" Drone Telemetry");
    lv_obj_set_style_pad_hor(sub_air_telemetry_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_air_telemetry_page);
    create_air_telemetry_menu(sub_air_telemetry_page);

    sub_air_actions_page = lv_menu_page_create(menu, LV_SYMBOL_PLAY" Actions");
    lv_obj_set_style_pad_hor(sub_air_actions_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_air_actions_page);
    create_air_actions_menu(sub_air_actions_page);    

    sub_gs_wfbng_page = lv_menu_page_create(menu, LV_SYMBOL_WIFI" WFB-NG");
    lv_obj_set_style_pad_hor(sub_gs_wfbng_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_gs_wfbng_page);
    create_gs_wfbng_menu(sub_gs_wfbng_page);

    sub_gs_system_page = lv_menu_page_create(menu, LV_SYMBOL_SETTINGS" System");
    lv_obj_set_style_pad_hor(sub_gs_system_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_gs_system_page);
    create_gs_system_menu(sub_gs_system_page);

    sub_wlan_page = lv_menu_page_create(menu, LV_SYMBOL_WIFI" WLAN");
    lv_obj_set_style_pad_hor(sub_wlan_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_wlan_page);
    create_wifi_menu(sub_wlan_page);

    sub_gs_actions_page = lv_menu_page_create(menu, LV_SYMBOL_PLAY" Actions");
    lv_obj_set_style_pad_hor(sub_gs_actions_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(sub_gs_actions_page);
    create_gs_actions_menu(sub_gs_actions_page);     

    /*Create a root page*/
    root_page = lv_menu_page_create(menu, "Menu");
    lv_obj_set_style_pad_hor(root_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);

    create_text(root_page, NULL, "Drone Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);    
    section = lv_menu_section_create(root_page);
    lv_obj_add_style(section, &style_openipc_section, 0);

    air_wfbng_cont = create_text(section, LV_SYMBOL_WIFI, "WFB-NG", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_wfbng_cont);
    lv_menu_set_load_page_event(menu, air_wfbng_cont, sub_air_wfbng_page);
    lv_obj_add_event_cb(air_wfbng_cont,back_event_handler,LV_EVENT_KEY,NULL);

    air_camera_cont = create_text(section, LV_SYMBOL_IMAGE, "Camera", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_camera_cont);
    lv_menu_set_load_page_event(menu, air_camera_cont, sub_air_camera_page);
    lv_obj_add_event_cb(air_camera_cont,back_event_handler,LV_EVENT_KEY,NULL);

    air_telemetry_cont = create_text(section, LV_SYMBOL_DOWNLOAD, "Telemetry", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_telemetry_cont);
    lv_menu_set_load_page_event(menu, air_telemetry_cont, sub_air_telemetry_page);
    lv_obj_add_event_cb(air_telemetry_cont,back_event_handler,LV_EVENT_KEY,NULL);

    air_actions_cont = create_text(section, LV_SYMBOL_PLAY, "Actions", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_actions_cont);
    lv_menu_set_load_page_event(menu, air_actions_cont, sub_air_actions_page);
    lv_obj_add_event_cb(air_actions_cont,back_event_handler,LV_EVENT_KEY,NULL);

    create_text(root_page, NULL, "GS Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(root_page);
    lv_obj_add_style(section, &style_openipc_section, 0);

    gs_wfbng_cont = create_text(section, LV_SYMBOL_WIFI, "WFB-NG", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_wfbng_cont);
    lv_menu_set_load_page_event(menu, gs_wfbng_cont, sub_gs_wfbng_page);
    lv_obj_add_event_cb(gs_wfbng_cont,back_event_handler,LV_EVENT_KEY,NULL);

    gs_system_cont = create_text(section, LV_SYMBOL_SETTINGS, "System Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_system_cont);
    lv_menu_set_load_page_event(menu, gs_system_cont, sub_gs_system_page);
    lv_obj_add_event_cb(gs_system_cont,back_event_handler,LV_EVENT_KEY,NULL);

    gs_wlan_cont = create_text(section, LV_SYMBOL_WIFI, "WLAN", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_wlan_cont);
    lv_menu_set_load_page_event(menu, gs_wlan_cont, sub_wlan_page);
    lv_obj_add_event_cb(gs_wlan_cont,back_event_handler,LV_EVENT_KEY,NULL);

    gs_actions_cont = create_text(section, LV_SYMBOL_PLAY, "Actions", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_actions_cont);
    lv_menu_set_load_page_event(menu, gs_actions_cont, sub_gs_actions_page); 
    lv_obj_add_event_cb(gs_actions_cont,back_event_handler,LV_EVENT_KEY,NULL);

    lv_menu_set_sidebar_page(menu, root_page);
    lv_menu_set_page(menu,sub_gs_main_page);
    lv_menu_clear_history(menu);

    lv_timer_t * timer = lv_timer_create(check_connection_timer, 500, NULL);
    last_value = gtotal_tunnel_data;
    setenv("GSMENU_VTX_DETECTED" , "0", 1);

    lv_group_set_default(default_group);
    return menu;
}

static void back_event_handler(lv_event_t * e)
{
    lv_key_t key = lv_event_get_key(e);
    if (key == LV_KEY_HOME) {
        printf("Go Back\n");
        lv_menu_set_page(menu,NULL);
        lv_menu_set_page(menu,sub_gs_main_page);
        lv_obj_remove_state(air_wfbng_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_camera_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_telemetry_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_actions_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_wfbng_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_system_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_wlan_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_actions_cont, LV_STATE_CHECKED);
        lv_screen_load(pp_osd_screen);
        menu_active = false;
    }
}
