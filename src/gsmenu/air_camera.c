#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"
#include "lvgl/lvgl.h"
#include "helper.h"

extern lv_group_t * default_group;

lv_obj_t * mirror;
lv_obj_t * flip;
lv_obj_t * contrast;
lv_obj_t * hue;
lv_obj_t * saturation;
lv_obj_t * luminace;
lv_obj_t * size;
lv_obj_t * fps;
lv_obj_t * bitrate;
lv_obj_t * video_codec;
lv_obj_t * gopsize;
lv_obj_t * rc_mode;
lv_obj_t * rec_enable;
lv_obj_t * rec_split;
lv_obj_t * rec_maxusage;
lv_obj_t * exposure;
lv_obj_t * antiflicker;
lv_obj_t * sensor_file;
lv_obj_t * fpv_enable;
lv_obj_t * noiselevel;

void air_camera_page_load_callback(lv_obj_t * page)
{

    lv_obj_t * msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(msgbox,&style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * label = lv_label_create(msgbox);
    lv_obj_t * bar1 = lv_bar_create(msgbox);
    lv_obj_add_style(bar1,&style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(bar1,&style_openipc_dropdown, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_bar_set_range(bar1,0,20);
    lv_obj_center(bar1);
    int progress = 0;

    lv_label_set_text(label,"Loading mirror ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,mirror);
    lv_label_set_text(label,"Loading flip ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,flip);
    lv_label_set_text(label,"Loading contrast ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,contrast);
    lv_label_set_text(label,"Loading hue ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,hue);
    lv_label_set_text(label,"Loading saturation ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,saturation);
    lv_label_set_text(label,"Loading luminace ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,luminace);
    lv_label_set_text(label,"Loading size ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,size);
    lv_label_set_text(label,"Loading fps ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,fps);
    lv_label_set_text(label,"Loading bitrate ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,bitrate);
    lv_label_set_text(label,"Loading video_codec ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,video_codec);
    lv_label_set_text(label,"Loading gopsize ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,gopsize);
    lv_label_set_text(label,"Loading mirror ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,rc_mode);
    lv_label_set_text(label,"Loading rec_enable ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,rec_enable);
    lv_label_set_text(label,"Loading rec_split ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,rec_split);
    lv_label_set_text(label,"Loading rec_maxusage ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,rec_maxusage);
    lv_label_set_text(label,"Loading exposure ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,exposure);
    lv_label_set_text(label,"Loading antiflicker ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,antiflicker);
    lv_label_set_text(label,"Loading sensor_file ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_dropdown_value(page,sensor_file);
    lv_label_set_text(label,"Loading fpv_enable ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_switch_value(page,fpv_enable);
    lv_label_set_text(label,"Loading noiselevel ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
    reload_slider_value(page,noiselevel);

    lv_msgbox_close(msgbox);
}

void create_air_camera_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "camera");
    menu_page_data->page_load_callback = air_camera_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    create_text(parent, NULL, "Video", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    size = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Size","","size",menu_page_data,false);
    fps = create_dropdown(cont,LV_SYMBOL_SETTINGS, "FPS","","fps",menu_page_data,false);
    bitrate = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Bitrate","","bitrate",menu_page_data,false);
    video_codec = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Codec","","codec",menu_page_data,false);
    gopsize = create_slider(cont,LV_SYMBOL_SETTINGS,"Gopsize",0,20,1,"gopsize",menu_page_data,false);
    rc_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "RC Mode","","rc_mode",menu_page_data,false);

    create_text(parent, NULL, "Image", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN); 
    
    mirror = create_switch(cont,LV_SYMBOL_SETTINGS,"Mirror","mirror", menu_page_data,false);
    flip = create_switch(cont,LV_SYMBOL_SETTINGS,"Flip","flip", menu_page_data,false);
    contrast = create_slider(cont,LV_SYMBOL_SETTINGS,"Contrast",0,100,11,"contrast",menu_page_data,false);
    hue = create_slider(cont,LV_SYMBOL_SETTINGS,"Hue",0,100,11,"hue",menu_page_data,false);
    saturation = create_slider(cont,LV_SYMBOL_SETTINGS,"Saturation",0,100,11,"saturation",menu_page_data,false);
    luminace = create_slider(cont,LV_SYMBOL_SETTINGS,"Luminance",0,100,11,"luminace",menu_page_data,false);

    create_text(parent, NULL, "Recording", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    rec_enable = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","rec_enable", menu_page_data,false);
    rec_split = create_slider(cont,LV_SYMBOL_SETTINGS,"Split",0,50,25,"rec_split",menu_page_data,false);
    rec_maxusage = create_slider(cont,LV_SYMBOL_SETTINGS,"Maxusage",0,99,90,"rec_maxusage",menu_page_data,false);

    create_text(parent, NULL, "ISP", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    exposure = create_slider(cont,LV_SYMBOL_SETTINGS,"Exposure",5,55,11,"exposure",menu_page_data,false);
    antiflicker = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Antiflicker","","antiflicker",menu_page_data,false);
    sensor_file = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Sensor File","","sensor_file",menu_page_data,false);

    create_text(parent, NULL, "FPV", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    
    fpv_enable = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","fpv_enable", menu_page_data,false);
    noiselevel = create_slider(cont,LV_SYMBOL_SETTINGS,"Noiselevel",0,1,0,"noiselevel",menu_page_data,false);

    lv_group_set_default(default_group);
}