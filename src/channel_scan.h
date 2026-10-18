#pragma once

#include <stdbool.h>
#include "lvgl/lvgl.h"

#define CHANNEL_SCAN_MAX_ROWS 64

typedef struct {
    int channel;
    int activity;
} channel_scan_row_t;

void channel_scan_start(void);
void channel_scan_stop(void);
int channel_scan_snapshot(channel_scan_row_t *rows, int max_rows);
void channel_scan_screen_init(lv_obj_t *parent);
