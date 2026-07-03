#pragma once

#include "lvgl/lvgl.h"

/* colmenu integration: list the .sh files under the script dir (each strdup'd,
 * caller owns), and run one (shows the confirm dialog + streaming output). */
int  gs_scripts_collect(char **names, int max);
void gs_scripts_run(const char *name);
