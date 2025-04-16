#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "styles.h"

#include "lvgl/lvgl.h"
#include "helper.h"

extern lv_group_t * default_group;

lv_obj_t * serial;
lv_obj_t * router;
lv_obj_t * osd_fps;
lv_obj_t * air_gs_rendering;

void air_telemetry_page_load_callback(lv_obj_t * page)
{
  lv_obj_t * msgbox = lv_msgbox_create(NULL);
  lv_obj_add_style(msgbox,&style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_t * label = lv_label_create(msgbox);
  lv_obj_t * bar1 = lv_bar_create(msgbox);
  lv_obj_add_style(bar1,&style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
  lv_obj_add_style(bar1,&style_openipc_dropdown, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_bar_set_range(bar1,0,4);
  lv_obj_center(bar1);
  int progress = 0;

  lv_label_set_text(label,"Loading serial ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
  reload_dropdown_value(page,serial);
  lv_label_set_text(label,"Loading router ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
  reload_dropdown_value(page,router);
  lv_label_set_text(label,"Loading osd_fps ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
  reload_slider_value(page,osd_fps);
  lv_label_set_text(label,"Loading air_gs_rendering ..."); lv_bar_set_value(bar1, progress++, LV_ANIM_OFF); lv_refr_now(NULL);
  reload_switch_value(page,air_gs_rendering);

  lv_msgbox_close(msgbox);

}

void create_air_telemetry_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "air");
    strcpy(menu_page_data->page, "telemetry");
    menu_page_data->page_load_callback = air_telemetry_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
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

    lv_group_set_default(default_group);
}