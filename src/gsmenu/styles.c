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


/*
 * dark & translucent theme.
 *
 * The whole gsmenu renders on a transparent LVGL screen on top of the live
 * video, so semi-transparent dark panels let the video subtly show through
 * while keeping text readable. The default LVGL theme is light, so we force a
 * light text color on the container styles (text color inherits to children).
 */

/* Panel / surface colors (deep, cool near-black shades) */
#define COL_PANEL       lv_color_hex(0x0b0e14)   /* outer menu background      */
#define COL_SECTION     lv_color_hex(0x161b24)   /* grouped rows (cards)       */
#define COL_DARK        lv_color_hex(0x080a10)   /* dropdown lists / tables    */
#define COL_LIGHTDARK   lv_color_hex(0x232a37)   /* text areas / knobs         */
#define COL_ACCENT      lv_color_hex(0x4c60d8)   /* OpenIPC brand blue (selection / focus) */
#define COL_TEXT        lv_color_hex(0xf2f4f8)   /* primary text               */
#define COL_TEXT_DIM    lv_color_hex(0x8a93a6)   /* disabled / secondary text  */
#define COL_BORDER      lv_color_hex(0x39424f)   /* subtle panel borders       */

/* Translucency levels (0 = transparent, 255 = opaque) */
#define OPA_PANEL       180   /* ~47% (semi-transparent page surface) */
#define OPA_SECTION     200   /* ~55% */
#define OPA_DARK        235   /* ~92% (dropdowns need good contrast) */
#define OPA_LIGHTDARK   210   /* ~82% */

#define RADIUS_CARD     8
#define RADIUS_ITEM     6


int style_init(void) {
    lv_style_reset(&style_rootmenu);
    lv_style_init(&style_rootmenu);
    /* Semi-transparent panel: the page reads as one cohesive dark surface,
     * but the live video still shows through it. */
    lv_style_set_bg_color(&style_rootmenu, COL_PANEL);
    lv_style_set_bg_opa(&style_rootmenu, OPA_PANEL);
    lv_style_set_text_color(&style_rootmenu, COL_TEXT);
    lv_style_set_pad_top(&style_rootmenu, 0);
    lv_style_set_pad_bottom(&style_rootmenu, 0);
    lv_style_set_pad_left(&style_rootmenu, 0);
    lv_style_set_pad_right(&style_rootmenu, 0);
    lv_style_set_radius(&style_rootmenu, 0);
    lv_style_set_border_width(&style_rootmenu, 0);
    lv_style_set_border_color(&style_rootmenu, COL_ACCENT);

    lv_style_reset(&style_openipc_section);
    lv_style_init(&style_openipc_section);
    lv_style_set_bg_color(&style_openipc_section, COL_SECTION);
    lv_style_set_bg_opa(&style_openipc_section, OPA_SECTION);
    lv_style_set_text_color(&style_openipc_section, COL_TEXT);
    lv_style_set_radius(&style_openipc_section, RADIUS_CARD);
    lv_style_set_border_width(&style_openipc_section, 1);
    lv_style_set_border_color(&style_openipc_section, COL_BORDER);
    lv_style_set_border_opa(&style_openipc_section, LV_OPA_40);

    lv_style_reset(&style_openipc_dark_background);
    lv_style_init(&style_openipc_dark_background);
    lv_style_set_bg_color(&style_openipc_dark_background, COL_DARK);
    lv_style_set_bg_opa(&style_openipc_dark_background, OPA_DARK);
    lv_style_set_text_color(&style_openipc_dark_background, COL_TEXT);
    lv_style_set_radius(&style_openipc_dark_background, RADIUS_ITEM);
    lv_style_set_border_width(&style_openipc_dark_background, 1);
    lv_style_set_border_color(&style_openipc_dark_background, COL_BORDER);
    lv_style_set_border_opa(&style_openipc_dark_background, LV_OPA_50);

    lv_style_reset(&style_openipc_lightdark_background);
    lv_style_init(&style_openipc_lightdark_background);
    lv_style_set_bg_color(&style_openipc_lightdark_background, COL_LIGHTDARK);
    lv_style_set_bg_opa(&style_openipc_lightdark_background, OPA_LIGHTDARK);
    lv_style_set_text_color(&style_openipc_lightdark_background, COL_TEXT);
    lv_style_set_radius(&style_openipc_lightdark_background, RADIUS_ITEM);

    lv_style_reset(&style_openipc);
    lv_style_init(&style_openipc);
    lv_style_set_bg_color(&style_openipc, COL_ACCENT);
    lv_style_set_bg_opa(&style_openipc, LV_OPA_COVER);
    lv_style_set_text_color(&style_openipc, COL_TEXT);
    lv_style_set_radius(&style_openipc, RADIUS_ITEM);
    lv_style_set_outline_color(&style_openipc, COL_ACCENT);
    lv_style_set_arc_color(&style_openipc, COL_ACCENT);

    lv_style_init(&style_openipc_dropdown);
    lv_style_set_bg_color(&style_openipc_dropdown, COL_ACCENT);

    lv_style_reset(&style_openipc_outline);
    lv_style_init(&style_openipc_outline);
    lv_style_set_outline_color(&style_openipc_outline, COL_ACCENT);
    lv_style_set_outline_opa(&style_openipc_outline, LV_OPA_COVER);
    lv_style_set_outline_width(&style_openipc_outline, 3);
    lv_style_set_radius(&style_openipc_outline, RADIUS_ITEM);

    lv_style_reset(&style_openipc_textcolor);
    lv_style_init(&style_openipc_textcolor);
    lv_style_set_text_color(&style_openipc_textcolor, COL_ACCENT);

    lv_style_reset(&style_openipc_disabled);
    lv_style_init(&style_openipc_disabled);
    lv_style_set_bg_color(&style_openipc_disabled, COL_DARK);
    lv_style_set_bg_opa(&style_openipc_disabled, OPA_SECTION);
    lv_style_set_text_color(&style_openipc_disabled, COL_TEXT_DIM);

    return 0;
}
