#pragma once
#include "../../lvgl/lvgl.h"

lv_obj_t * find_first_focusable_obj(lv_obj_t * parent);
const char* find_resource_file(const char* relative_path);
void gsmenu_toggle_rxmode(void);
void show_restart_notice(void);

/* Apply the dark gsmenu theme to a msgbox (body, header, footer, content).
 * Call after the title and any footer buttons have been added. */
void theme_msgbox(lv_obj_t *mbox);
