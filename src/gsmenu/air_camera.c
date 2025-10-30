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
lv_obj_t * video_mode;
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


extern lv_obj_t * rec_fps;
void air_rec_fps_cb(lv_event_t *e) {
    char val[100] = "";

    lv_obj_t *ta = lv_event_get_target(e);
    lv_dropdown_get_selected_str(ta, val, sizeof(val) - 1);

    lv_obj_t *obj = lv_obj_get_child_by_type(rec_fps, 0, &lv_dropdown_class);
    int index = lv_dropdown_get_option_index(obj,val);
    lv_dropdown_set_selected(obj, index);
    lv_obj_send_event(obj,LV_EVENT_VALUE_CHANGED,NULL);
}

void create_air_camera_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "camera");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
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
    video_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Video Mode","","video_mode",menu_page_data,false);

    fps = create_dropdown(cont,LV_SYMBOL_SETTINGS, "FPS","","fps",menu_page_data,false);
    // change rec fps when changeing camera fps
    lv_obj_add_event_cb(lv_obj_get_child_by_type(fps,0,&lv_dropdown_class), air_rec_fps_cb, LV_EVENT_VALUE_CHANGED,fps);

    bitrate = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Bitrate","","bitrate",menu_page_data,false);
    video_codec = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Codec","","codec",menu_page_data,false);
    gopsize = create_slider(cont,LV_SYMBOL_SETTINGS,"Gopsize","gopsize",menu_page_data,false,0);
    rc_mode = create_dropdown(cont,LV_SYMBOL_SETTINGS, "RC Mode","","rc_mode",menu_page_data,false);

    create_text(parent, NULL, "Image", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN); 
    
    mirror = create_switch(cont,LV_SYMBOL_SETTINGS,"Mirror","mirror", menu_page_data,false);
    flip = create_switch(cont,LV_SYMBOL_SETTINGS,"Flip","flip", menu_page_data,false);
    contrast = create_slider(cont,LV_SYMBOL_SETTINGS,"Contrast","contrast",menu_page_data,false,0);
    hue = create_slider(cont,LV_SYMBOL_SETTINGS,"Hue","hue",menu_page_data,false,0);
    saturation = create_slider(cont,LV_SYMBOL_SETTINGS,"Saturation","saturation",menu_page_data,false,0);
    luminace = create_slider(cont,LV_SYMBOL_SETTINGS,"Luminance","luminace",menu_page_data,false,0);

    create_text(parent, NULL, "Recording", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    rec_enable = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","rec_enable", menu_page_data,false);
    rec_split = create_slider(cont,LV_SYMBOL_SETTINGS,"Split","rec_split",menu_page_data,false,0);
    rec_maxusage = create_slider(cont,LV_SYMBOL_SETTINGS,"Maxusage","rec_maxusage",menu_page_data,false,0);

    create_text(parent, NULL, "ISP", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    exposure = create_slider(cont,LV_SYMBOL_SETTINGS,"Exposure","exposure",menu_page_data,false,0);
    antiflicker = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Antiflicker","","antiflicker",menu_page_data,false);
    sensor_file = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Sensor File","","sensor_file",menu_page_data,false);

    create_text(parent, NULL, "FPV", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    
    fpv_enable = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","fpv_enable", menu_page_data,false);
    noiselevel = create_slider(cont,LV_SYMBOL_SETTINGS,"Noiselevel","noiselevel",menu_page_data,false,0);


    add_entry_to_menu_page(menu_page_data,"Loading mirror ...", mirror, reload_switch_value );
    add_entry_to_menu_page(menu_page_data,"Loading flip ...", flip, reload_switch_value );
    add_entry_to_menu_page(menu_page_data,"Loading contrast ...", contrast, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading hue ...", hue, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading saturation ...", saturation, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading luminace ...", luminace, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading size ...", size, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading video_mode ...", video_mode, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading fps ...", fps, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading bitrate ...", bitrate, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading video_codec ...", video_codec, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading gopsize ...", gopsize, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading rc_mode ...", rc_mode, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading rec_enable ...", rec_enable, reload_switch_value );
    add_entry_to_menu_page(menu_page_data,"Loading rec_split ...", rec_split, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading rec_maxusage ...", rec_maxusage, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading exposure ...", exposure, reload_slider_value );
    add_entry_to_menu_page(menu_page_data,"Loading antiflicker ...", antiflicker, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading sensor_file ...", sensor_file, reload_dropdown_value );
    add_entry_to_menu_page(menu_page_data,"Loading fpv_enable ...", fpv_enable, reload_switch_value );
    add_entry_to_menu_page(menu_page_data,"Loading noiselevel ...", noiselevel, reload_slider_value);

    lv_group_set_default(default_group);
}