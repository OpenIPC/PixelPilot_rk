#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "../main.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"
#include <stddef.h>
#include "../menu.h"
#include "gs_actions.h"

#ifdef USE_SIMULATOR
void sig_handler(int exit_code)
{   
    exit(exit_code);
}
#endif

extern lv_group_t * default_group;

lv_obj_t * restart_pp;
lv_obj_t * exit_pp;
lv_obj_t * gs_reboot;
lv_obj_t * gs_custom_action;
int restart_value = 1;
int exit_value = 2;

void gs_actions_exit_pp(lv_event_t * event)
{   
    int *signal = lv_event_get_user_data(event);
    sig_handler(*signal);
}

void create_gs_actions_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "actions");
    menu_page_data->page_load_callback = NULL;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);    

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    restart_pp = create_button(section, "Restart Pixelpilot");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(restart_pp,0,&lv_button_class),gs_actions_exit_pp,LV_EVENT_CLICKED,&restart_value);

    exit_pp = create_button(section, "Exit Pixelpilot");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(exit_pp,0,&lv_button_class),gs_actions_exit_pp,LV_EVENT_CLICKED,&exit_value);

    gs_reboot = create_button(section, "Reboot");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_reboot,0,&lv_button_class),generic_button_callback,LV_EVENT_CLICKED,menu_page_data);


    for (size_t i = 0; i < gsactions_count; i++) {
        gs_custom_action = create_button(section, gsactions[i].label);
        lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_custom_action,0,&lv_button_class),custom_actions_cb,LV_EVENT_CLICKED,&gsactions[i]);
    }

    lv_group_set_default(default_group);
}
