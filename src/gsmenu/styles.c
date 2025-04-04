#include "lvgl/lvgl.h"


lv_style_t style_rootmenu;
lv_style_t style_openipc;
lv_style_t style_openipc_dropdown;
lv_style_t style_openipc_outline;
lv_style_t style_openipc_textcolor;
lv_style_t style_openipc_disabled;
lv_style_t style_openipc_section;
lv_style_t style_openipc_dark_background;
lv_style_t style_openipc_lightdark_background;


int style_init(void) {
    lv_style_reset(&style_rootmenu);
    lv_style_init(&style_rootmenu);
    lv_style_set_bg_color(&style_rootmenu, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 50));
    lv_style_set_pad_top(&style_rootmenu, 0);
    lv_style_set_pad_bottom(&style_rootmenu, 0);
    lv_style_set_pad_left(&style_rootmenu, 0);
    lv_style_set_pad_right(&style_rootmenu, 0);
    lv_style_set_radius(&style_rootmenu, 0);
    lv_style_set_border_width(&style_rootmenu, 0);
    lv_style_set_border_color(&style_rootmenu, lv_color_hex(0xff4c60d8));

    lv_style_reset(&style_openipc_section);
    lv_style_init(&style_openipc_section);
    lv_style_set_bg_color(&style_openipc_section, lv_color_lighten( lv_color_make(0xcd, 0xcd, 0xcd), 50));

    lv_style_reset(&style_openipc_dark_background);
    lv_style_init(&style_openipc_dark_background);
    lv_style_set_bg_color(&style_openipc_dark_background, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 90));    

    lv_style_reset(&style_openipc_lightdark_background);
    lv_style_init(&style_openipc_lightdark_background);
    lv_style_set_bg_color(&style_openipc_lightdark_background, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 30));    

    lv_style_reset(&style_openipc);
    lv_style_init(&style_openipc);
    lv_style_set_bg_color(&style_openipc, lv_color_hex(0xff4c60d8));
    lv_style_set_outline_color(&style_openipc, lv_color_hex(0xff4c60d8));
    lv_style_set_arc_color(&style_openipc, lv_color_hex(0xff4c60d8));

    lv_style_init(&style_openipc_dropdown);
    lv_style_set_bg_color(&style_openipc_dropdown, lv_color_hex(0xff4c60d8));

    lv_style_reset(&style_openipc_outline);
    lv_style_init(&style_openipc_outline);
    lv_style_set_outline_color(&style_openipc_outline, lv_color_hex(0xff4c60d8));
    lv_style_set_outline_width(&style_openipc_outline,7);

    lv_style_reset(&style_openipc_textcolor);
    lv_style_init(&style_openipc_textcolor);
    lv_style_set_text_color(&style_openipc_textcolor, lv_color_hex(0xff4c60d8));    

    lv_style_reset(&style_openipc_disabled);
    lv_style_init(&style_openipc_disabled);
    lv_style_set_bg_color(&style_openipc_disabled, lv_color_hex(0xff4c60d8));
    lv_style_set_text_color(&style_openipc_disabled, lv_color_darken( lv_color_make(0xff, 0xff, 0xff), 50));
    //lv_style_set_line_color(&style_openipc_disabled, lv_color_hex(0xffd8ce36));
    //lv_style_set_border_color(&style_openipc_disabled, lv_color_hex(0xffe61212));

    return 0;
}
