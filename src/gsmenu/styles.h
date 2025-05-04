#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

extern lv_style_t style_rootmenu;
extern lv_style_t style_openipc;
extern lv_style_t style_openipc_dropdown;
extern lv_style_t style_openipc_outline;
extern lv_style_t style_openipc_textcolor;
extern lv_style_t style_openipc_disabled;
extern lv_style_t style_openipc_section;
extern lv_style_t style_openipc_dark_background;
extern lv_style_t style_openipc_lightdark_background;

int style_init(void);

#ifdef __cplusplus
}
#endif
