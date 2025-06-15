#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_wfbng.h"
#include "air_wfbng.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

extern lv_group_t * default_group;

lv_obj_t * gs_channel;
lv_obj_t * gs_search;
lv_obj_t * bandwidth;
lv_obj_t * adaptivelink;


void gs_wfbng_page_load_callback(lv_obj_t * page)
{
    reload_dropdown_value(page,gs_channel);
    reload_dropdown_value(page,bandwidth);
    reload_switch_value(page,adaptivelink);
}

void gs_wfbng_search_callback(lv_event_t * event)
{
    run_command_and_block(event,"gsmenu.sh search channel",NULL);
}


void create_gs_wfbng_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "wfbng");
    menu_page_data->page_load_callback = gs_wfbng_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);


    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "WFB-NG", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    gs_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Channel", "","gs_channel",menu_page_data,false);
    gs_search = create_button(cont, "Search");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(gs_search,0,&lv_button_class),gs_wfbng_search_callback,LV_EVENT_CLICKED,menu_page_data);
    bandwidth = create_dropdown(cont,LV_SYMBOL_SETTINGS, "bandwidth", "","bandwidth",menu_page_data,false);

    create_text(parent, NULL, "Adaptive Link", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    adaptivelink = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","adaptivelink", menu_page_data,false);

    lv_group_set_default(default_group);
}