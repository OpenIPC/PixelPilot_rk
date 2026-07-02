#pragma once

#include "lvgl/lvgl.h"
#include "ui.h"

void gs_scripts_page_refresh(lv_obj_t *page);
void gs_scripts_init_in_page(lv_obj_t *parent, menu_page_data_t *menu_page_data);

/* colmenu integration: list the .sh files under the script dir (each strdup'd,
 * caller owns), and run one (shows the confirm dialog + streaming output). */
int  gs_scripts_collect(char **names, int max);
void gs_scripts_run(const char *name);
