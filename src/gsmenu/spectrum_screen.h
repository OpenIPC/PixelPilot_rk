#pragma once
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the Spectrum Scanner screen inside `parent` (called once from menu.c,
 * like create_table() for TX-Profiles). */
void spectrum_screen_init(lv_obj_t * parent);

#ifdef __cplusplus
}
#endif
