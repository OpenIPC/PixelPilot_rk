#include <stdio.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "input.h"
#include "menu.h"
#include "gsmenu/ui.h"
#include "gsmenu/styles.h"
#include "lvosd.h"

lv_obj_t * menu;
lv_indev_t * indev_drv;
lv_group_t * default_group;
lv_obj_t * pp_menu_screen; 
lv_obj_t * pp_osd_screen;

/**
 * PP Main Menu
 */
void pp_menu_main(void)
{

    style_init();

    // create_virtual_keyboard
    indev_drv = create_virtual_keyboard();

    // Create an input group
    default_group = lv_group_create();
    lv_group_set_default(default_group);
    lv_indev_set_group(indev_drv, default_group);

    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t * menu_cont = lv_obj_create(pp_menu_screen);
    lv_obj_set_size(menu_cont,lv_obj_get_width(pp_menu_screen) / 4 * 3,
                                lv_obj_get_height(pp_menu_screen)/ 4 * 3);
    lv_obj_center(menu_cont);
    lv_obj_set_style_border_side(menu_cont, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);    

    pp_header_create(menu_cont);

    pp_menu_create(menu_cont);

    pp_osd_main();

    lv_screen_load(pp_osd_screen);
}