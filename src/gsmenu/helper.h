#pragma once
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "ui.h"

#include "../../lvgl/lvgl.h"

typedef enum {
    LV_MENU_ITEM_BUILDER_VARIANT_1,
    LV_MENU_ITEM_BUILDER_VARIANT_2
} lv_menu_builder_variant_t;


lv_obj_t * create_text(lv_obj_t * parent, const char * icon, const char * txt, const char * parameter, menu_page_data_t* menu_page_data,bool blocking,lv_menu_builder_variant_t builder_variant);

lv_obj_t * create_slider(lv_obj_t * parent, const char * icon, const char * txt, int32_t min, int32_t max, int32_t val,const char * parameter, menu_page_data_t* menu_page_data,bool blocking);
lv_obj_t * create_switch(lv_obj_t * parent, const char * icon, const char * txt,const char * parameter, menu_page_data_t* menu_page_data,bool blocking);

void dropdown_event_handler(lv_event_t * e);

lv_obj_t * create_dropdown(lv_obj_t * parent, const char * icon, const char * label_txt, const char * txt,const char * parameter, menu_page_data_t* menu_page_data,bool blocking);

lv_obj_t * create_button(lv_obj_t * parent, const char * txt);

lv_obj_t * create_backbutton(lv_obj_t * parent, const char * icon, const char * label_txt);

lv_obj_t * create_textarea(lv_obj_t * parent, char * text, const char * label_txt, const char * parameter, menu_page_data_t* menu_page_data, bool password);

lv_obj_t * create_spinbox(lv_obj_t * parent, const char * icon, const char * txt, int32_t min, int32_t max,
                                int32_t val);

lv_obj_t * find_first_focusable_obj(lv_obj_t * parent);
void handle_sub_page_load(lv_event_t *e);
char* get_paramater(lv_obj_t * page, char * param);
void reload_label_value(lv_obj_t * page,lv_obj_t * parameter);
void reload_switch_value(lv_obj_t * page,lv_obj_t * parameter);
void reload_dropdown_value(lv_obj_t * page,lv_obj_t * parameter);
void reload_textarea_value(lv_obj_t * page,lv_obj_t * parameter);
void reload_slider_value(lv_obj_t * page,lv_obj_t * parameter);
void get_slider_value(lv_obj_t * parent);
void get_dropdown_value(lv_obj_t * parent);