#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"

#include "lvgl/lvgl.h"
#include "helper.h"

extern lv_group_t * default_group;

#define ENTRIES 4
lv_obj_t * serial;
lv_obj_t * router;
lv_obj_t * osd_fps;
lv_obj_t * air_gs_rendering;

void create_air_telemetry_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t) + sizeof(PageEntry) * ENTRIES);
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "telemetry");
    menu_page_data->page_load_callback = generic_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = ENTRIES;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * section;

    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    
    serial = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Serial Port", "","serial",menu_page_data,false);
    router = create_dropdown(cont,LV_SYMBOL_SETTINGS,"Router","","router",menu_page_data,false);

    create_text(parent, NULL, "MSPOSD Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    osd_fps = create_slider(cont,LV_SYMBOL_SETTINGS,"OSD FPS",1,60,1,"osd_fps",menu_page_data,false);
    air_gs_rendering = create_switch(cont,LV_SYMBOL_SETTINGS,"GS Rendering","gs_rendering", menu_page_data,false);

    PageEntry entries[] = {
        { "Loading serial ...", serial, reload_dropdown_value },
        { "Loading router ...", router, reload_dropdown_value },
        { "Loading osd_fps ...", osd_fps, reload_slider_value },
        { "Loading air_gs_rendering ...", air_gs_rendering, reload_switch_value },
    };
    memcpy(menu_page_data->page_entries, entries, sizeof(entries));

    lv_group_set_default(default_group);
}