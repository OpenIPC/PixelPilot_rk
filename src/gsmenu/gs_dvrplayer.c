#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include "../main.h"
#include "styles.h"

extern lv_indev_t * indev_drv;

extern lv_obj_t * pp_menu_screen; 
extern lv_obj_t * dvr_screen;

extern lv_group_t * default_group;
extern lv_group_t * dvr_page_group;
lv_group_t * dvr_group;

lv_obj_t * btn_container = NULL;
lv_obj_t * btn_play_pause = NULL;
lv_timer_t *hide_timer = NULL;

// Animation callback for fade out
static void set_opa_anim(void * obj, int32_t opa) {
    lv_obj_set_style_opa((lv_obj_t *)obj, opa, LV_PART_MAIN);
}

// Timer callback to hide the controls
static void hide_controls_cb(lv_timer_t *timer)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn_container);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa_anim);
    
    lv_anim_start(&a);
}

// Timer Reset
static void timer_reset_handler(lv_event_t * e)
{
    lv_obj_set_style_opa(btn_container, LV_OPA_COVER, LV_PART_MAIN);
    // Reset the timer
    if (hide_timer) {
        lv_timer_delete(hide_timer);
        hide_timer = lv_timer_create(hide_controls_cb, 5000, NULL);
        lv_timer_set_repeat_count(hide_timer, 1);
        lv_timer_set_auto_delete(hide_timer,false);
    } else {
        hide_timer = lv_timer_create(hide_controls_cb, 5000, NULL);
        lv_timer_set_repeat_count(hide_timer, 1);
        lv_timer_set_auto_delete(hide_timer,false);
    }
}

void change_playbutton_label(const char * text) {
    lv_obj_t * screen = lv_screen_active();
    if(btn_play_pause) {
        lv_obj_t * label = lv_obj_get_child(btn_play_pause, 0);
        lv_label_set_text(label, text);
    }
}

// Event handler for the play/pause button
static void play_pause_event_handler(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    const char * current_text = lv_label_get_text(label);

    timer_reset_handler(e);

    // Toggle the icon and functionality
    if (strcmp(current_text, LV_SYMBOL_PLAY) == 0) {
        change_playbutton_label(LV_SYMBOL_PAUSE);
#ifndef USE_SIMULATOR
        resume_playback();
#else
        printf("resume_playback();\n");
#endif
    } else if (strcmp(current_text, LV_SYMBOL_PAUSE) == 0) {
        change_playbutton_label(LV_SYMBOL_PLAY);
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
    timer_reset_handler(e);
#ifndef USE_SIMULATOR
    skip_duration(-10000);  // Skip back 10 seconds
#else
    printf("skip_duration(-10000);n");
#endif
}

// Event handler for the fast-forward button
static void ff_event_handler(lv_event_t * e)
{
    timer_reset_handler(e);
#ifndef USE_SIMULATOR
    skip_duration(10000);  // Skip forward 10 seconds
#else
    printf("skip_duration(10000);\n");
#endif
}

// Event handler for the stop button
static void stop_event_handler(lv_event_t * e)
{
    timer_reset_handler(e);

    change_playbutton_label(LV_SYMBOL_PLAY);

#ifndef USE_SIMULATOR
    switch_pipeline_source("stream",NULL);
#else
    printf("switch_pipeline_source(\"stream\",NULL);\n");
#endif

    lv_screen_load(pp_menu_screen);
    lv_indev_set_group(indev_drv,dvr_page_group);
}

// Focus on Play/Pause on screen load
static void screen_load_default_focus(lv_event_t * e)
{
    lv_group_focus_obj(btn_play_pause);
}

void create_video_controls(lv_obj_t * parent)
{
    // Create a container for the buttons with a flex layout
    btn_container = lv_obj_create(parent);
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
    lv_obj_add_event_cb(btn,timer_reset_handler,LV_EVENT_FOCUSED, NULL);

    // Play/Pause Button
    btn_play_pause = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn_play_pause, play_pause_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn_play_pause);
    lv_label_set_text(label, LV_SYMBOL_PLAY); // Initial state is 'Play'
    lv_obj_center(btn_play_pause);
    lv_obj_add_style(btn_play_pause, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn_play_pause, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn_play_pause,timer_reset_handler,LV_EVENT_FOCUSED, NULL);

    // Fast-Forward Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, ff_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_RIGHT);
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn,timer_reset_handler,LV_EVENT_FOCUSED, NULL);

    // Stop Button
    btn = lv_button_create(btn_container);
    lv_obj_add_event_cb(btn, stop_event_handler, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_STOP);
    lv_obj_center(label);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn,timer_reset_handler,LV_EVENT_FOCUSED, NULL);
}

void dvr_player_screen_init(void)
{

    dvr_group = lv_group_create();
    lv_group_set_default(dvr_group);

    // Create the video control buttons
    create_video_controls(dvr_screen);
    lv_obj_add_event_cb(dvr_screen,timer_reset_handler,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(dvr_screen,screen_load_default_focus,LV_EVENT_SCREEN_LOADED,NULL);

    lv_group_set_default(default_group);

}