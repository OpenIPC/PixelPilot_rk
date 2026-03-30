#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "../input.h"
#include "../gstrtpreceiver.h"
#include "helper.h"
#include "styles.h"
#include "executor.h"
#include "gs_wifi.h"

static void scan_wifi_event_handler(lv_event_t * e);
static void connect_wifi_event_handler(lv_event_t * e);
static void disconnect_wifi_event_handler(lv_event_t * e);
static void ta_event_cb(lv_event_t * e);

extern lv_obj_t * menu;
extern gsmenu_control_mode_t control_mode;
extern lv_group_t * default_group;

lv_obj_t * ssid;
lv_obj_t * password;
lv_obj_t * wlan;
lv_obj_t * hotspot;
lv_obj_t * ipinfo;
lv_obj_t * restream;
lv_obj_t * ip_dropdown;

static uint16_t find_dropdown_option_index(const char * options, const char * value)
{
    if (!value || value[0] == '\0') {
        return 0;
    }

    uint16_t idx = 0;
    const char * option = options;

    while (option && *option) {
        const char * nl = strchr(option, '\n');
        size_t len = nl ? (size_t)(nl - option) : strlen(option);
        if (strlen(value) == len && strncmp(option, value, len) == 0) {
            return idx;
        }
        idx++;
        option = nl ? nl + 1 : NULL;
    }

    return 0;
}

void wifi_page_load_callback(lv_obj_t * page)
{
    reload_switch_value(page,hotspot);
    reload_switch_value(page,wlan);
    reload_textarea_value(page,ssid);
    reload_textarea_value(page,password);
    reload_label_value(page,ipinfo);

#ifndef USE_SIMULATOR
    if (restream_get_enabled()) lv_obj_add_state(lv_obj_get_child_by_type(restream,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(restream,0,&lv_switch_class), LV_STATE_CHECKED);
    {
        char clients[512];
        restream_scan_clients(clients, sizeof(clients));
        lv_dropdown_set_options(ip_dropdown, clients);
        lv_dropdown_set_selected(ip_dropdown, find_dropdown_option_index(clients, restream_get_manual_ip()));
    }
#endif
}

static void ip_dropdown_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        char buf[64];
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
#ifndef USE_SIMULATOR
        restream_set_manual_ip(buf);
#endif
    }
}

static void restream_switch_callback(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * target = lv_event_get_target(e);
#ifndef USE_SIMULATOR
        restream_set_enabled(lv_obj_has_state(target, LV_STATE_CHECKED));
#endif
    }
}

static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e); 
    lv_obj_t * target_ta = lv_obj_get_user_data(btn); // Retrieve associated textarea

    if(code == LV_EVENT_CLICKED) {
        if (target_ta) {
            lv_keyboard_set_textarea(kb, target_ta);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_view_recursive(target_ta, LV_ANIM_OFF);
            lv_indev_wait_release(lv_event_get_param(e));
            lv_group_focus_obj(kb);
        }
    }
}

static void kb_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e);

    if (code == LV_EVENT_FOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
    else if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(NULL, ta);   /*To forget the last clicked object to make it focusable again*/
        lv_group_focus_obj(lv_obj_get_child_by_type(lv_obj_get_parent(lv_keyboard_get_textarea(kb)),0,&lv_button_class));
        lv_obj_update_layout(lv_obj_get_parent(kb));
    }
}

static void interlocking_switch_callback(lv_event_t * e) {
    lv_obj_t * target = lv_event_get_target(e);
    lv_obj_t * parent = lv_event_get_user_data(e);

    if(lv_obj_has_state(target, LV_STATE_CHECKED)) {
        if (parent == hotspot) {
            lv_obj_add_flag(wlan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ssid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(password, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(lv_obj_get_child_by_type(wlan,0,&lv_switch_class),LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(lv_obj_get_child_by_type(hotspot,0,&lv_switch_class),LV_STATE_CHECKED);
        }
    } else {
        if (parent == hotspot) {
            lv_obj_clear_flag(wlan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ssid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(password, LV_OBJ_FLAG_HIDDEN);            
        }       
    }
}

void create_wifi_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "wifi");
    menu_page_data->page_load_callback = wifi_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    hotspot = create_switch(cont,NULL,"Hotspot","hotspot", menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(hotspot,0,&lv_switch_class), interlocking_switch_callback, LV_EVENT_VALUE_CHANGED, hotspot);


    ssid = create_textarea(cont, "Loading ...", "SSID", "ssid", menu_page_data, false);
    password = create_textarea(cont, "Loading ...", "Password", "password", menu_page_data, true);

    wlan = create_switch(cont,NULL,"Connected","wlan", menu_page_data,false);
    thread_data_t* data = lv_obj_get_user_data(lv_obj_get_child_by_type(wlan,0,&lv_switch_class));
    data->arguments[0] = lv_obj_get_child_by_type(ssid,0,&lv_textarea_class);
    data->arguments[1] = lv_obj_get_child_by_type(password,0,&lv_textarea_class);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(wlan,0,&lv_switch_class), interlocking_switch_callback, LV_EVENT_VALUE_CHANGED, wlan);

  
    lv_obj_t * kb = lv_keyboard_create(lv_obj_get_parent(section));
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_style(kb, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_dark_background, LV_PART_ITEMS| LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_textcolor, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);    
    
    lv_obj_add_event_cb(lv_obj_get_child_by_type(ssid,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(password,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL,kb);
    lv_keyboard_set_textarea(kb, NULL);

    create_text(parent, NULL, "Phone Restream", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    restream = create_switch(cont,LV_SYMBOL_VIDEO,"Phone Restream",NULL, NULL,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(restream,0,&lv_switch_class), restream_switch_callback, LV_EVENT_VALUE_CHANGED,NULL);

    lv_obj_t * ip_dropdown_row = lv_menu_cont_create(cont);
    lv_obj_t * dd_icon = lv_image_create(ip_dropdown_row);
    lv_image_set_src(dd_icon, LV_SYMBOL_WIFI);
    lv_obj_t * dd_label = lv_label_create(ip_dropdown_row);
    lv_label_set_text(dd_label, "Stream To");
    lv_obj_set_flex_grow(dd_label, 1);
    ip_dropdown = lv_dropdown_create(ip_dropdown_row);
    lv_dropdown_set_options(ip_dropdown, "Auto");
    lv_dropdown_set_dir(ip_dropdown, LV_DIR_RIGHT);
    lv_dropdown_set_symbol(ip_dropdown, LV_SYMBOL_RIGHT);
    lv_obj_set_width(ip_dropdown, 200);
    lv_obj_add_style(ip_dropdown, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(ip_dropdown, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * dd_list = lv_dropdown_get_list(ip_dropdown);
    lv_obj_add_style(dd_list, &style_openipc, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_style(dd_list, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ip_dropdown, dropdown_event_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ip_dropdown, ip_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ip_dropdown, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(ip_dropdown, on_focus, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(menu_page_data->indev_group, ip_dropdown);

    create_text(parent, NULL, "Network", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    ipinfo = create_text(cont, LV_SYMBOL_SETTINGS, "Network", "IP", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t * ipinfo_label = lv_obj_get_child_by_type(ipinfo,0, &lv_label_class);
    lv_group_add_obj(menu_page_data->indev_group,ipinfo_label);


    lv_group_set_default(default_group);
}
