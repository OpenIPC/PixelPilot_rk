#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include "../main.h"
#include "styles.h"

extern lv_indev_t * indev_drv;

extern lv_obj_t * pp_menu_screen; 
extern lv_obj_t * dvr_screen;

extern lv_group_t * default_group;
extern lv_group_t * main_group;
lv_group_t * dvr_group;

bool seek_mode = false;

void change_playbutton_label(const char * text) {
    lv_obj_t * screen = lv_screen_active();
    lv_obj_t * play_pause_btn = lv_obj_get_child(lv_obj_get_child(screen, 0), 1);
    if(play_pause_btn) {
        lv_obj_t * label = lv_obj_get_child(play_pause_btn, 0);
        lv_label_set_text(label, text);
    }
}

// Event handler for the play/pause button
static void play_pause_event_handler(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    const char * current_text = lv_label_get_text(label);

    // Toggle the icon and functionality
    if (strcmp(current_text, LV_SYMBOL_PLAY) == 0 && ! seek_mode) {
        change_playbutton_label(LV_SYMBOL_PAUSE);
        printf("Action: Play\n");
#ifndef USE_SIMULATOR
        resume_playback();
#else
        printf("resume_playback();\n");
#endif
    } else if (strcmp(current_text, LV_SYMBOL_PLAY) == 0 && seek_mode) {
        change_playbutton_label(LV_SYMBOL_PAUSE);
#ifndef USE_SIMULATOR
        normal_playback();
#else
        printf("normal_playback();\n");
#endif
        seek_mode = false;
        printf("Action: Resume Play\n");
    } else if (strcmp(current_text, LV_SYMBOL_PAUSE) == 0 && ! seek_mode) {
        change_playbutton_label(LV_SYMBOL_PLAY);
        printf("Action: Pause\n");
#ifndef USE_SIMULATOR
        pause_playback();
#else
        printf("pause_playback();\n");
#endif
    }
}

// Event handler for the fast-rewind button
static void fr_event_handler(lv_event_t * e)
{
    printf("Action: Fast-Rewind\n");
#ifndef USE_SIMULATOR
    fast_rewind(-2);
#else
    printf("fast_rewind(2);\n");
#endif
    change_playbutton_label(LV_SYMBOL_PLAY);
    seek_mode = true;
}

// Event handler for the fast-forward button
static void ff_event_handler(lv_event_t * e)
{
    printf("Action: Fast-Forward\n");
#ifndef USE_SIMULATOR
    fast_forward(2);
#else
    printf("fast_forward(2);");
#endif
    change_playbutton_label(LV_SYMBOL_PLAY);
    seek_mode = true;
}

// Event handler for the stop button
static void stop_event_handler(lv_event_t * e)
{
    printf("Action: Stop\n");

    change_playbutton_label(LV_SYMBOL_PLAY);

#ifndef USE_SIMULATOR
    switch_pipeline_source("stream",NULL);
#else
    printf("switch_pipeline_source(\"stream\",NULL);\n");
#endif

    seek_mode = false;
    lv_screen_load(pp_menu_screen);
    lv_indev_set_group(indev_drv,main_group);
}

void create_video_controls(lv_obj_t * parent)
{
    // Create a container for the buttons with a flex layout
    lv_obj_t * btn_container = lv_obj_create(parent);
    lv_obj_remove_style(btn_container, NULL, LV_PART_SCROLLBAR);
    lv_obj_set_size(btn_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -20); // Position at the bottom
    lv_obj_add_style(btn_container, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);


    // --- Button Definitions ---
    lv_obj_t * btn;
    lv_obj_t * label;

    // Fast-Rewind Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, fr_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_LEFT);
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    // Play/Pause Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, play_pause_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_PLAY); // Initial state is 'Play'
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    // Fast-Forward Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, ff_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_RIGHT);
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    // Stop Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, stop_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_STOP);
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
}

void dvr_player_screen_init(void)
{

    dvr_group = lv_group_create();
    lv_group_set_default(dvr_group);

    // Create the video control buttons
    create_video_controls(dvr_screen);

    lv_group_set_default(default_group);

}