#pragma once

typedef struct {
    int channel;
    const char *frequency;
} FrequencyEntry;

char *get_frequencies_string(FrequencyEntry *entries, int size);

void create_air_wfbng_menu(lv_obj_t * parent);