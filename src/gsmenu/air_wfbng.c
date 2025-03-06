#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "helper.h"
#include "air_wfbng.h"
#include "executor.h"
#include "styles.h"

lv_obj_t * driver_txpower_override;
lv_obj_t * air_channel;
lv_obj_t * air_bandwidth;
lv_obj_t * mcs_index;
lv_obj_t * stbc;
lv_obj_t * ldpc;
lv_obj_t * fec_k;
lv_obj_t * fec_n;
lv_obj_t * air_adaptivelink;

void air_wfbng_page_load_callback(lv_obj_t * page)
{

    lv_obj_t * msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(msgbox,&style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * label = lv_label_create(msgbox);
    lv_obj_t * bar1 = lv_bar_create(msgbox);
    lv_obj_add_style(bar1,&style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(bar1,&style_openipc_dropdown, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_bar_set_range(bar1,0,6);
    lv_obj_center(bar1);
    int progress = 0;

    lv_label_set_text(label,"Loading driver_txpower_override ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,driver_txpower_override);
    lv_label_set_text(label,"Loading air_channel ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,air_channel);
    lv_label_set_text(label,"Loading air_bandwidth ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,air_bandwidth);
    lv_label_set_text(label,"Loading mcs_index ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,mcs_index);
    lv_label_set_text(label,"Loading stbc ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,stbc);
    lv_label_set_text(label,"Loading ldpc ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,ldpc);
    lv_label_set_text(label,"Loading fec_k ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,fec_k);
    lv_label_set_text(label,"Loading fec_n ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,fec_n);
    lv_label_set_text(label,"Loading air_adaptivelink ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,air_adaptivelink);

    lv_msgbox_close(msgbox);

}

void create_air_wfbng_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "wfbng");
    menu_page_data->page_load_callback = air_wfbng_page_load_callback;
    lv_obj_set_user_data(parent,menu_page_data);    

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN); 


    driver_txpower_override = create_dropdown(cont,LV_SYMBOL_SETTINGS,"Power", "","power",menu_page_data,false);
    air_channel = create_dropdown(cont,LV_SYMBOL_SETTINGS,"Frequency", "","air_channel",menu_page_data,false);
    air_bandwidth = create_dropdown(cont,LV_SYMBOL_SETTINGS, "bandwidth", "","bandwidth",menu_page_data,false);
    mcs_index = create_slider(cont,LV_SYMBOL_SETTINGS, "MCS Index", 0 , 11, 1,"mcs_index",menu_page_data,false);
    stbc = create_switch(cont,LV_SYMBOL_SETTINGS,"STBC","stbc", menu_page_data,false);
    ldpc = create_switch(cont,LV_SYMBOL_SETTINGS,"LDPC","ldpc", menu_page_data,false);
    fec_k = create_slider(cont,LV_SYMBOL_SETTINGS, "FEC_K", 0 , 12, 12,"fec_k",menu_page_data,false);
    fec_n = create_slider(cont,LV_SYMBOL_SETTINGS, "FEC_N", 0 , 12, 8,"fec_n",menu_page_data,false);

    create_text(parent, NULL, "Adaptive Link", LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    air_adaptivelink = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","adaptivelink", menu_page_data,false);
    
}
