#pragma once
#include "lvgl/lvgl.h"

enum RXMode {
    WFB,
    APFPV
};

void toggle_rec_enabled(void);
void create_gs_system_receiver_menu(lv_obj_t * parent);
void create_gs_system_display_menu(lv_obj_t * parent);
void create_gs_system_dvr_menu(lv_obj_t * parent);
void create_gs_system_menu(lv_obj_t * parent, lv_obj_t * receiver_page, lv_obj_t * display_page, lv_obj_t * dvr_page);
