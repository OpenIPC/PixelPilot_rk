#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "../input.h"

extern lv_group_t * default_group;
extern lv_indev_t * indev_drv;
extern gsmenu_control_mode_t control_mode;

lv_obj_t * ap_fpv_channel;
lv_obj_t * txPower;
lv_obj_t * mcsShift;
lv_obj_t * temp;
lv_obj_t * throughput;
lv_obj_t * osdscale;
lv_obj_t * mcssource;

void create_air_aalink_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "aalink");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "WLAN Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    ap_fpv_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Channel","","channel",menu_page_data,false);

    create_text(parent, NULL, "AALink Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    txPower = create_slider(cont,LV_SYMBOL_SETTINGS,"VTX Power Output","SCALE_TX_POWER",menu_page_data,false,1);
    mcsShift = create_slider(cont,LV_SYMBOL_SETTINGS,"Link resilience (dB)","THRESH_SHIFT",menu_page_data,false,0);
    osdscale = create_slider(cont,LV_SYMBOL_SETTINGS,"OSD Size","OSD_SCALE",menu_page_data,false,1);
    throughput = create_slider(cont,LV_SYMBOL_SETTINGS,"Maximum Throughput (%)","THROUGHPUT_PCT",menu_page_data,false,0);
    temp = create_slider(cont,LV_SYMBOL_SETTINGS,"Temp Throttle Threshold (°C)","HIGH_TEMP",menu_page_data,false,0);
    mcssource = create_dropdown(cont,LV_SYMBOL_SETTINGS, "LQ Consideration","","MCS_SOURCE",menu_page_data,false);
    

    add_entry_to_menu_page(menu_page_data,"Loading Channel ...", ap_fpv_channel, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading VTX Power Output ...", txPower, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading Link resilience (dB) ...", mcsShift, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading OSD Size ...", osdscale, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading Maximum Throughput ...", throughput, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading Temp Throttle Threshold (°C)", temp, reload_slider_value);
    add_entry_to_menu_page(menu_page_data,"Loading LQ Consideration ...", mcssource, reload_dropdown_value);

    lv_group_set_default(default_group);
}