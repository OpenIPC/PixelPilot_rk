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

extern lv_obj_t * menu;
extern gsmenu_control_mode_t control_mode;
extern lv_group_t * default_group;

#define ENTRIES 3
lv_obj_t * ap_fpv_ssid;
lv_obj_t * ap_fpv_password;
lv_obj_t * ap_fpv_channel;

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

static void change_textarea_value(lv_event_t *e ,lv_obj_t * ta) {

    thread_data_t * user_data = (thread_data_t*) lv_obj_get_user_data(ta);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    user_data->argument_string = strdup(lv_textarea_get_text(ta));
    strcat(final_command,"\"");
    strcat(final_command,user_data->argument_string);
    strcat(final_command,"\"");

    run_command_and_block(e,final_command,NULL);
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

    if(code == LV_EVENT_READY) {
        change_textarea_value(e,lv_keyboard_get_textarea(kb));
    }
}

void create_apfpv_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t) + sizeof(PageEntry) * ENTRIES);
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "apfpv");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = ENTRIES;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    ap_fpv_ssid = create_textarea(cont, "", "SSID", "ssid", menu_page_data, false);
    ap_fpv_password = create_textarea(cont, "", "Password", "password", menu_page_data, true);
    ap_fpv_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Channel","","channel",menu_page_data,false);

    lv_obj_t * kb = lv_keyboard_create(lv_obj_get_parent(section));
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_style(kb, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_dark_background, LV_PART_ITEMS| LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_textcolor, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);    
    
    lv_obj_add_event_cb(lv_obj_get_child_by_type(ap_fpv_ssid,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(ap_fpv_password,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL,kb);
    lv_keyboard_set_textarea(kb, NULL);

    PageEntry entries[] = {
        { "Loading SSID ...", ap_fpv_ssid, reload_textarea_value },
        { "Loading Password ...", ap_fpv_password, reload_textarea_value },
        { "Loading Channel ...", ap_fpv_channel, reload_dropdown_value },
    };
    memcpy(menu_page_data->page_entries, entries, sizeof(entries));

    lv_group_set_default(default_group);
}
