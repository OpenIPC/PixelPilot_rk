#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#define MAX_ACTIONS 5
#define MAX_LABEL_LEN 50
#define MAX_ACTION_LEN 500

typedef struct {
    char label[MAX_LABEL_LEN];
    char action[MAX_ACTION_LEN];
} MenuAction;

/* Receiver mode (WFB vs APFPV). Defined in menu.c. */
enum RXMode { WFB, APFPV };
extern enum RXMode RXMODE;

/* Toggle DVR recording (hardware button). Defined in menu.c. */
void toggle_rec_enabled(void);

/* Query current recording state (for menu sync). */
int menu_is_recording(void);

/* Opus audio on/off (System -> Receiver -> Audio). Defined in main.cpp. */
// int  audio_get_enabled(void);
// void audio_set_enabled(int enabled);

void pp_menu_main(void);