#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "lvgl/lvgl.h"
#include "../main.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"
#include "gs_dvrplayer.h"

extern char* dvr_template;
extern lv_group_t * default_group;
extern lv_obj_t * dvr_screen;
extern lv_indev_t * indev_drv;
extern lv_group_t * dvr_group;
lv_group_t * dvr_page_group;


char path[256];
lv_obj_t* rec_list = NULL;

// Structure to hold file information
typedef struct {
    char* filepath;
} file_data_t;

// Button event handler
void button_event_handler(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t* btn = lv_event_get_target(e);
        file_data_t* data = (file_data_t*)lv_obj_get_user_data(btn);
        printf("Selected video: %s\n", data->filepath);
#ifndef USE_SIMULATOR
        switch_pipeline_source("file",data->filepath);
#else
        printf("switch_pipeline_source(\"file\",data->filepath);\n");
#endif
    lv_screen_load(dvr_screen);
    lv_indev_set_group(indev_drv,dvr_group);
    change_playbutton_label(LV_SYMBOL_PAUSE);
    }
    
    else if (code == LV_EVENT_DELETE) {
        lv_obj_t* btn = lv_event_get_target(e);
        file_data_t* data = (file_data_t*)lv_obj_get_user_data(btn);
        if (data) {
            free(data->filepath);
            free(data);
        }
    }
}


void dvr_menu_load_callback(lv_obj_t * page) {

    menu_page_data_t * menu_page_data = (menu_page_data_t *) lv_obj_get_user_data(page);
    lv_indev_set_group(indev_drv,menu_page_data->indev_group);
    lv_group_set_default(menu_page_data->indev_group);

    if (rec_list) {
        lv_obj_clean(rec_list);
    } else {
        rec_list = lv_list_create(page);
        lv_obj_set_size(rec_list, LV_PCT(100), LV_SIZE_CONTENT); // Make the list fill the parent
        lv_obj_center(rec_list);
        lv_obj_add_style(rec_list, &style_openipc, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_add_style(rec_list, &style_openipc_section, LV_PART_MAIN);
    }

    DIR* dir;
    struct dirent* ent;
    
    // Open directory and add file names to the list
    if ((dir = opendir(path)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            // Check for .mp4 files
            char* ext = strrchr(ent->d_name, '.');
            if (ext && strcmp(ext, ".mp4") == 0) {
                // Add a new button to the list with the filename as its text
                lv_obj_t* btn = lv_list_add_btn(rec_list, LV_SYMBOL_VIDEO, ent->d_name);
                
                // Store the full file path in the button's user data
                size_t path_len = strlen(path) + strlen(ent->d_name) + 1; // +1 for '\0'
                file_data_t* data = malloc(sizeof(file_data_t));
                data->filepath = malloc(path_len);
                snprintf(data->filepath, path_len, "%s%s", path, ent->d_name); // Path already has a trailing slash
                lv_obj_set_user_data(btn, data);

                lv_obj_add_event_cb(btn, button_event_handler, LV_EVENT_ALL, NULL);
                lv_obj_add_event_cb(btn, generic_back_event_handler, LV_EVENT_KEY,NULL);

                lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
                lv_obj_add_style(btn, &style_openipc_section, LV_PART_MAIN);
            }
        }
        closedir(dir);
    } else {
        // Handle case where directory cannot be opened
        lv_obj_t* btn = lv_list_add_btn(rec_list, LV_SYMBOL_WARNING, "Could not open directory");
        lv_obj_add_event_cb(btn, generic_back_event_handler, LV_EVENT_KEY,NULL);
        lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    }
    if (lv_obj_get_child_count(rec_list) == 0) {
        lv_obj_t* btn = lv_list_add_btn(rec_list, LV_SYMBOL_VIDEO, "No recordings found");
        lv_obj_add_event_cb(btn, generic_back_event_handler, LV_EVENT_KEY,NULL);
        lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    }
    lv_group_set_default(default_group);
}

void create_gs_dvr_menu(lv_obj_t * parent) {

    menu_page_data_t* menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "dvr");
    menu_page_data->page_load_callback = dvr_menu_load_callback;
    menu_page_data->indev_group = lv_group_create();
    dvr_page_group = menu_page_data->indev_group;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    // Extract directory path from dvr_template
    const char *last_slash = strrchr(dvr_template, '/');
    if (last_slash) {
        size_t path_len = last_slash - dvr_template + 1;
        strncpy(path, dvr_template, path_len);
        path[path_len] = '\0';  // Null-terminate
    } else {
        strcpy(path, "./"); // Fallback to current directory if no slash found
    }
    printf("Extracted path: \"%s\"\n", path);

    dvr_menu_load_callback(parent);

    lv_group_set_default(default_group);
}
