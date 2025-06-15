#include "lvgl/lvgl.h"


extern lv_obj_t * pp_osd_screen;
lv_group_t * osd_group;

void pp_osd_main(void)
{
    pp_osd_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(pp_osd_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pp_osd_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    osd_group = lv_group_create();
}