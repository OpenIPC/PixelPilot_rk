#pragma once
#include <pthread.h>
#include "../../lvgl/lvgl.h"

/* Per-page metadata. These lived in ui.h with the old lv_menu; the executor is
 * now their only real user, so they live here. */
typedef void (*ReloadFunc)(lv_obj_t * page, lv_obj_t * target);

typedef struct {
    const char *caption;
    lv_obj_t *target;
    ReloadFunc reload;
} PageEntry;

typedef struct {
    char type[100];
    char page[100];
    void (*page_load_callback)(lv_obj_t * page);
    lv_group_t *indev_group;
    size_t entry_count;
    PageEntry *page_entries;
} menu_page_data_t;

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
    int precision;
    callback_fn callback_fn;
} thread_data_t;

char* run_command(const char* command);
void run_command_and_block(lv_event_t* e,const char * command, callback_fn callback);
