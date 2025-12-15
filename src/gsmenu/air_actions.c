#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"
#include "helper.h"
#include "air_actions.h"

extern lv_group_t * default_group;
lv_obj_t * air_reboot;
lv_obj_t * air_custom_action;

void create_air_actions_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "actions");
    menu_page_data->page_load_callback = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);    

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);


    air_reboot = create_button(section, "Reboot");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(air_reboot,0,&lv_button_class),generic_button_callback,LV_EVENT_CLICKED,menu_page_data);

    for (size_t i = 0; i < airactions_count; i++) {
        air_custom_action = create_button(section, airactions[i].label);
        lv_obj_add_event_cb(lv_obj_get_child_by_type(air_custom_action,0,&lv_button_class),custom_actions_cb,LV_EVENT_CLICKED,&airactions[i]);
    }

    lv_group_set_default(default_group);
}
