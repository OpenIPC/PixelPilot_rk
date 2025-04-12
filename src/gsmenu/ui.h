#pragma once

#include "../../lvgl/lvgl.h"
#include "ui.h"

typedef struct {
    char type[100];
    char page[100];
    void (*page_load_callback)(lv_obj_t * page);
    lv_group_t *indev_group;
} menu_page_data_t;

lv_obj_t * pp_header_create(lv_obj_t * screen);
lv_obj_t * pp_menu_create(lv_obj_t * screen);
