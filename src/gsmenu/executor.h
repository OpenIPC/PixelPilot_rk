#pragma once
#include "ui.h"

#define MAX_CMD_ARGS 5

typedef void (*callback_fn)(void);

typedef struct {
    lv_event_t * event;
    lv_obj_t* parent;
    bool blocking;
    bool work_complete;
    pthread_t thread_id;
    menu_page_data_t * menu_page_data;
    char* argument_string;
    char* command;
    lv_obj_t* arguments[MAX_CMD_ARGS];
    lv_obj_t* spinner;
    char parameter[100];
    callback_fn callback_fn;
} thread_data_t;


char* run_command(const char* command);
void run_command_and_block(lv_event_t* e,const char * command, callback_fn callback);
void generic_switch_event_cb(lv_event_t * e);
void generic_dropdown_event_cb(lv_event_t * e);
void generic_slider_event_cb(lv_event_t * e);