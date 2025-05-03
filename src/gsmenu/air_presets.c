#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"

#include "lvgl/lvgl.h"
#include "helper.h"

extern lv_group_t * default_group;

#define ENTRIES 1
lv_obj_t * preset;
lv_obj_t * apply;
lv_obj_t * update;
lv_obj_t * profile_info;

void preset_event_cb(lv_event_t * e) {
    lv_obj_t * user_data = lv_event_get_user_data(e);
    lv_obj_t * dd = lv_obj_get_child_by_type(user_data,0,&lv_dropdown_class);
    char arg[100] = "";
    lv_dropdown_get_selected_str(dd,arg,99);

    char command[200] = "gsmenu.sh get air presets info ";
    strcat(command,"\"");
    strcat(command,arg);
    strcat(command,"\"");
    lv_label_set_text(lv_obj_get_child_by_type(profile_info,0,&lv_label_class),run_command(command));
}

void air_presets_apply_callback(lv_event_t * e) {
    lv_obj_t * user_data = lv_event_get_user_data(e);
    lv_obj_t * dd = lv_obj_get_child_by_type(user_data,0,&lv_dropdown_class);
    char arg[100] = "";
    lv_dropdown_get_selected_str(dd,arg,99);

    char command[200] = "gsmenu.sh set air presets ";
    strcat(command,"\"");
    strcat(command,arg);
    strcat(command,"\"");
    run_command_and_block(e,command,NULL);
}

void air_presets_update_callback_callback(void) {
    get_dropdown_value(preset);
    lv_obj_send_event(lv_obj_get_child_by_type(preset,0,&lv_dropdown_class),LV_EVENT_VALUE_CHANGED,preset);
}

void air_presets_update_callback(lv_event_t * e) {
    run_command_and_block(e,"gsmenu.sh get air presets update",air_presets_update_callback_callback);
}

void air_presets_page_load_callback(lv_obj_t *page) {
    lv_obj_send_event(lv_obj_get_child_by_type(preset,0,&lv_dropdown_class),LV_EVENT_VALUE_CHANGED,preset);
}

void create_air_presets_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t) + sizeof(PageEntry) * ENTRIES);
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "presets");
    menu_page_data->page_load_callback = air_presets_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = ENTRIES;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    preset = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Preset", "","preset",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(preset,0,&lv_dropdown_class), preset_event_cb, LV_EVENT_VALUE_CHANGED,preset);
    lv_obj_remove_event_cb(lv_obj_get_child_by_type(preset,0,&lv_dropdown_class), generic_dropdown_event_cb);

    apply = create_button(cont, "Apply");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(apply,0,&lv_button_class),air_presets_apply_callback,LV_EVENT_CLICKED,preset);

    update = create_button(cont, "Update (Online)");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(update,0,&lv_button_class),air_presets_update_callback,LV_EVENT_CLICKED,NULL);

    cont = lv_menu_cont_create(section);
    profile_info = create_text(cont, NULL, "Preset Info", "preset_info", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);

    lv_group_set_default(default_group);
}