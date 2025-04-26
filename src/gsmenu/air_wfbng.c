#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "helper.h"
#include "air_wfbng.h"
#include "executor.h"
#include "styles.h"

extern lv_group_t * default_group;

#define ENTRIES 10
lv_obj_t * driver_txpower_override;
lv_obj_t * air_channel;
lv_obj_t * air_bandwidth;
lv_obj_t * mcs_index;
lv_obj_t * stbc;
lv_obj_t * ldpc;
lv_obj_t * fec_k;
lv_obj_t * fec_n;
lv_obj_t * mlink;
lv_obj_t * air_adaptivelink;

void create_air_wfbng_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t) + sizeof(PageEntry) * ENTRIES);
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "wfbng");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = ENTRIES;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);    

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN); 


    driver_txpower_override = create_dropdown(cont,LV_SYMBOL_SETTINGS,"Power", "","power",menu_page_data,false);
    air_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS,"Frequency", "","air_channel",menu_page_data,false);
    air_bandwidth = create_dropdown(cont,LV_SYMBOL_SETTINGS, "bandwidth", "","width",menu_page_data,false);
    mcs_index = create_slider(cont,LV_SYMBOL_SETTINGS, "MCS Index", 0 , 11, 1,"mcs_index",menu_page_data,false);
    stbc = create_switch(cont,LV_SYMBOL_SETTINGS,"STBC","stbc", menu_page_data,false);
    ldpc = create_switch(cont,LV_SYMBOL_SETTINGS,"LDPC","ldpc", menu_page_data,false);
    fec_k = create_slider(cont,LV_SYMBOL_SETTINGS, "FEC_K", 0 , 12, 12,"fec_k",menu_page_data,false);
    fec_n = create_slider(cont,LV_SYMBOL_SETTINGS, "FEC_N", 0 , 12, 8,"fec_n",menu_page_data,false);
    mlink = create_dropdown(cont,LV_SYMBOL_SETTINGS,"MLink", "","mlink",menu_page_data,false);

    create_text(parent, NULL, "Adaptive Link", NULL, NULL, false,  LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    air_adaptivelink = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","adaptivelink", menu_page_data,false);

    PageEntry entries[] = {
        { "Loading driver_txpower_override ...", driver_txpower_override, reload_dropdown_value },
        { "Loading air_channel ...", air_channel, reload_dropdown_value },
        { "Loading air_bandwidth ...", air_bandwidth, reload_dropdown_value },
        { "Loading mcs_index ...", mcs_index, reload_slider_value },
        { "Loading stbc ...", stbc, reload_switch_value },
        { "Loading ldpc ...", ldpc, reload_switch_value },
        { "Loading fec_k ...", fec_k, reload_slider_value },
        { "Loading fec_n ...", fec_n, reload_slider_value },
        { "Loading mlink ...", mlink, reload_dropdown_value },
        { "Loading air_adaptivelink ...", air_adaptivelink, reload_switch_value },
    };
    memcpy(menu_page_data->page_entries, entries, sizeof(entries));

    lv_group_set_default(default_group);
}
