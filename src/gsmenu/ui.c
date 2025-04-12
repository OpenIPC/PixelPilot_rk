#include <stdio.h>
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
extern uint64_t gtotal_tunnel_bytes; // global variable for easyer access in gsmenu
uint64_t last_count = 0;
int consecutive_increases = 0;

void check_connection_timer(lv_timer_t * timer)
{

#ifndef USE_SIMULATOR
    if (last_count < gtotal_tunnel_bytes) {
        consecutive_increases++;
        last_count = gtotal_tunnel_bytes;
    } else {
        consecutive_increases=0;
    }
#else
    consecutive_increases ++;
#endif

    if (consecutive_increases == 5) {
        lv_obj_remove_state(air_wfbng_cont, LV_STATE_DISABLED);
        lv_obj_remove_state(air_camera_cont, LV_STATE_DISABLED);
        lv_obj_remove_state(air_telemetry_cont, LV_STATE_DISABLED);
        lv_obj_remove_state(air_actions_cont, LV_STATE_DISABLED);
    } else if (consecutive_increases == 0) {
        last_count = 0;
        lv_obj_add_state(air_wfbng_cont, LV_STATE_DISABLED);
        lv_obj_add_state(air_camera_cont, LV_STATE_DISABLED);
        lv_obj_add_state(air_telemetry_cont, LV_STATE_DISABLED);
        lv_obj_add_state(air_actions_cont, LV_STATE_DISABLED);
        if (sub_air_wfbng_page == lv_menu_get_cur_main_page(menu) ||
            sub_air_camera_page == lv_menu_get_cur_main_page(menu) ||
            sub_air_telemetry_page == lv_menu_get_cur_main_page(menu) ||
            sub_air_actions_page == lv_menu_get_cur_main_page(menu) ||
            air_wfbng_cont == lv_group_get_focused(main_group) ||
            air_camera_cont == lv_group_get_focused(main_group) ||
            air_telemetry_cont == lv_group_get_focused(main_group) ||
            air_actions_cont == lv_group_get_focused(main_group)
            ) {

            lv_menu_set_page(menu,sub_gs_main_page);
            lv_menu_clear_history(menu);
            lv_obj_remove_state(air_wfbng_cont, LV_STATE_CHECKED);
            lv_obj_remove_state(air_camera_cont, LV_STATE_CHECKED);
            lv_obj_remove_state(air_telemetry_cont, LV_STATE_CHECKED);
            lv_obj_remove_state(air_actions_cont, LV_STATE_CHECKED);

            // Find the first focusable object recursively
            lv_obj_t * first_obj = find_first_focusable_obj(root_page);

            // Focus the first focusable object
            if (first_obj) {
                lv_group_t * group = lv_group_get_default();
                if (group) {
                    lv_group_focus_obj(first_obj);
                }
            }

        }
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
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_ENABLED);
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

    air_camera_cont = create_text(section, LV_SYMBOL_IMAGE, "Camera", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_camera_cont);
    lv_menu_set_load_page_event(menu, air_camera_cont, sub_air_camera_page);

    air_telemetry_cont = create_text(section, LV_SYMBOL_DOWNLOAD, "Telemetry", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_telemetry_cont);
    lv_menu_set_load_page_event(menu, air_telemetry_cont, sub_air_telemetry_page);

    air_actions_cont = create_text(section, LV_SYMBOL_PLAY, "Actions", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,air_actions_cont);
    lv_menu_set_load_page_event(menu, air_actions_cont, sub_air_actions_page);

    create_text(root_page, NULL, "GS Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(root_page);
    lv_obj_add_style(section, &style_openipc_section, 0);

    gs_wfbng_cont = create_text(section, LV_SYMBOL_WIFI, "WFB-NG", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_wfbng_cont);
    lv_menu_set_load_page_event(menu, gs_wfbng_cont, sub_gs_wfbng_page);

    gs_system_cont = create_text(section, LV_SYMBOL_SETTINGS, "System Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_system_cont);
    lv_menu_set_load_page_event(menu, gs_system_cont, sub_gs_system_page);

    gs_wlan_cont = create_text(section, LV_SYMBOL_WIFI, "WLAN", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_wlan_cont);
    lv_menu_set_load_page_event(menu, gs_wlan_cont, sub_wlan_page);

    gs_actions_cont = create_text(section, LV_SYMBOL_PLAY, "Actions", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_group_add_obj(main_group,gs_actions_cont);
    lv_menu_set_load_page_event(menu, gs_actions_cont, sub_gs_actions_page);    

    lv_menu_set_sidebar_page(menu, root_page);
    lv_menu_set_page(menu,sub_gs_main_page);
    lv_menu_clear_history(menu);

    lv_obj_t * sidebar_menu_back_button = lv_menu_get_sidebar_header_back_button(menu);
    lv_obj_add_style(sidebar_menu_back_button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_group_add_obj(main_group,sidebar_menu_back_button);

    // lv_timer_t * timer = lv_timer_create(check_connection_timer, 500, NULL);

    lv_group_set_default(default_group);
    return menu;
}

static void back_event_handler(lv_event_t * e)
{
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
