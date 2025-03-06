#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

lv_obj_t * air_reboot;

void air_actions_reboot_callback(lv_event_t * event)
{
    run_command("sshpass -p '12345' ssh -o StrictHostKeyChecking=no root@10.5.0.10 reboot &");
}

void create_air_actions_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "actions");
    menu_page_data->page_load_callback = NULL;
    lv_obj_set_user_data(parent,menu_page_data);    

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);


    air_reboot = create_button(section, "Reboot");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(air_reboot,0,&lv_button_class),air_actions_reboot_callback,LV_EVENT_CLICKED,NULL);
    
}
