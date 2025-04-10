#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "../input.h"
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

lv_obj_t * ssid;
lv_obj_t * password;
lv_obj_t * wlan;
lv_obj_t * hotspot;

void wifi_page_load_callback(lv_obj_t * page)
{
    reload_switch_value(page,hotspot);
    reload_switch_value(page,wlan);
    reload_textarea_value(page,ssid);
    reload_textarea_value(page,password);
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
        lv_group_focus_obj(lv_keyboard_get_textarea(kb));
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
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    hotspot = create_switch(section,NULL,"Hotspot","hotspot", menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(hotspot,0,&lv_switch_class), interlocking_switch_callback, LV_EVENT_VALUE_CHANGED, hotspot);


    ssid = create_textarea(section, "", "SSID", "ssid", menu_page_data, false);
    password = create_textarea(section, "", "Password", "password", menu_page_data, true);

    wlan = create_switch(section,NULL,"Connected","wlan", menu_page_data,false);
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
}
