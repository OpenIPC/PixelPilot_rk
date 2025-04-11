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

lv_obj_t * version ;
lv_obj_t * disk ;
lv_obj_t * wfb_nics ;
lv_obj_t * channel ;
lv_obj_t * screen_mode ;

void gs_main_page_load_callback(lv_obj_t * page)
{
    reload_label_value(page,version);
    reload_label_value(page,disk);
    reload_label_value(page,wfb_nics);
    reload_label_value(page,channel);
    reload_label_value(page,screen_mode);
}

void create_main_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "main");
    menu_page_data->page_load_callback = gs_main_page_load_callback;
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * label;
    lv_obj_t * section;
    lv_obj_t * obj;

    create_text(parent, NULL, "Main", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    channel = create_text(cont, LV_SYMBOL_SETTINGS, "Channel", "Channel", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    screen_mode = create_text(cont, LV_SYMBOL_SETTINGS, "HDMI-OUT", "HDMI-OUT", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    wfb_nics = create_text(cont, LV_SYMBOL_SETTINGS, "WFB_NICS", "WFB_NICS", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    disk = create_text(cont, LV_SYMBOL_SETTINGS, "Version", "Disk", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    version = create_text(cont, LV_SYMBOL_SETTINGS, "Version", "Version", menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
}
