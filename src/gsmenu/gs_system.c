#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_system.h"
#include "lvgl/lvgl.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"

extern lv_group_t * default_group;

lv_obj_t * gs_rendering;
lv_obj_t * resolution;
lv_obj_t * rec_enabled;
lv_obj_t * rec_fps;
lv_obj_t * vsync_disabled;

typedef struct Dvr* Dvr; // Forward declaration
void dvr_start_recording(Dvr* dvr);
void dvr_stop_recording(Dvr* dvr);
void dvr_set_video_framerate(Dvr* dvr,int f);
extern Dvr *dvr;
extern int dvr_enabled;
extern bool disable_vsync;

void gs_system_page_load_callback(lv_obj_t * page)
{
    reload_switch_value(page,gs_rendering);
    reload_dropdown_value(page,resolution);
    reload_dropdown_value(page,rec_fps);

    if (dvr_enabled) lv_obj_add_state(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), LV_STATE_CHECKED);

    if (disable_vsync) lv_obj_add_state(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), LV_STATE_CHECKED);

}

void toggle_rec_enabled()
{
    lv_obj_t * rec_switch = lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class);
    if (lv_obj_has_state(rec_switch, LV_STATE_CHECKED)) lv_obj_clear_state(rec_switch, LV_STATE_CHECKED);
    else lv_obj_add_state(rec_switch, LV_STATE_CHECKED);
    lv_obj_send_event(rec_switch, LV_EVENT_VALUE_CHANGED, NULL);
}

void rec_enabled_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        if (lv_obj_has_state(ta, LV_STATE_CHECKED)) {
#ifndef USE_SIMULATOR 
            dvr_start_recording(dvr);
#else
            printf("dvr_start_recording(dvr);\n");
#endif
        } else {
#ifndef USE_SIMULATOR             
            dvr_stop_recording(dvr);
#else
            printf("dvr_stop_recording(dvr);\n");
#endif            
        }
    }
}


void disable_vsync_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        disable_vsync = lv_obj_has_state(ta, LV_STATE_CHECKED);
    }
}

void rec_fps_cb(lv_event_t *e) {
    lv_event_code_t event = lv_event_get_code(e);
    if (event == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *ta = lv_event_get_target(e);
        char val[100] = "";
        lv_dropdown_get_selected_str(ta,val,99);
        int fps = atoi(val);
#ifndef USE_SIMULATOR 
        dvr_set_video_framerate(dvr,fps);
#else
        printf("dvr_set_video_framerate(dvr,%i);\n",fps);
#endif
    }
}

void create_gs_system_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "system");
    menu_page_data->page_load_callback = gs_system_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * cont;
    lv_obj_t * label;
    lv_obj_t * section;
    lv_obj_t * obj;

    create_text(parent, NULL, "General", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);    

    gs_rendering = create_switch(cont,LV_SYMBOL_SETTINGS,"GS Rendering","gs_rendering", menu_page_data,false);
    resolution = create_dropdown(cont,LV_SYMBOL_SETTINGS, "Resolution","","resolution",menu_page_data,false);
    vsync_disabled = create_switch(cont,LV_SYMBOL_SETTINGS,"Disable VSYNC","disable_vsync", NULL,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(vsync_disabled,0,&lv_switch_class), disable_vsync_cb, LV_EVENT_VALUE_CHANGED,NULL);

    create_text(parent, NULL, "Recording", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    rec_enabled = create_switch(cont,LV_SYMBOL_SETTINGS,"Enabled","rec_enabled", menu_page_data, true);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_enabled,0,&lv_switch_class), rec_enabled_cb, LV_EVENT_VALUE_CHANGED,NULL);

    rec_fps = create_dropdown(section,LV_SYMBOL_SETTINGS, "Recording FPS", "","rec_fps",menu_page_data,false);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(rec_fps,0,&lv_dropdown_class), rec_fps_cb, LV_EVENT_VALUE_CHANGED,NULL);

    lv_group_set_default(default_group);
}