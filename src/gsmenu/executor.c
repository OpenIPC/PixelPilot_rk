#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <lvgl.h>
#include "executor.h"
#include "styles.h"

#define MAX_OUTPUT_SIZE 1024

lv_group_t * default_group;
lv_group_t * error_group;
extern lv_indev_t * indev_drv;

void error_button_callback(lv_event_t * e) {

    lv_obj_t* msgbox = (lv_obj_t*)lv_event_get_user_data(e);

    printf("Button klicked\n");
    lv_indev_set_group(indev_drv,default_group);
    lv_group_set_default(default_group);
    lv_msgbox_close(msgbox);
}

void show_error(char* result) {

    default_group = lv_group_get_default();
    error_group = lv_group_create();
    lv_group_set_default(error_group);
    lv_indev_set_group(indev_drv,error_group);

    lv_obj_t * msgbox = lv_msgbox_create(NULL);
    lv_msgbox_add_text(msgbox,result);
    lv_msgbox_add_title(msgbox, "Error");
    
    lv_obj_t * button = lv_msgbox_add_footer_button(msgbox, "Ok");
    lv_obj_add_event_cb(button, error_button_callback, LV_EVENT_CLICKED,msgbox);
    lv_obj_add_style(button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);    
}

char* run_command(const char* command) {
    char buffer[MAX_OUTPUT_SIZE];
    char* result = (char*)malloc(MAX_OUTPUT_SIZE);
    if (result == NULL) {
        perror("Failed to allocate memory");
        return NULL;
    }
    result[0] = '\0'; // Initialize the result string

    printf("Running command: %s\n",command);
    FILE* fp = popen(command, "r");
    if (fp == NULL) {
        perror("Failed to run command");
        free(result);
        return NULL;
    }

    // Read the output of the command
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        strcat(result, buffer);
    }

    int status = pclose(fp);
    if (status == -1) {
        perror("pclose failed");
    } else {
        printf("Command exited with status: %d\n", WEXITSTATUS(status));
        if (WEXITSTATUS(status) > 0)
            show_error(result);
    }
    return result;
}

void* worker_thread(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;

    run_command(data->command);
    
    data->work_complete = true;
    return NULL;
}

void check_thread_complete(lv_timer_t* timer) {
    thread_data_t* data = (thread_data_t*)lv_timer_get_user_data(timer);    
    
    if(data->work_complete) {
        // Thread has finished - clean up
        pthread_join(data->thread_id, NULL);
        lv_obj_del(data->spinner);
        lv_timer_del(timer);
        free(data);
    }
}

void run_command_and_block(lv_event_t* e,const char * command) {
    lv_obj_t* parent = lv_event_get_current_target(e);
    
    // Setup thread data
    thread_data_t* data = malloc(sizeof(thread_data_t));
    data->parent = parent;
    data->work_complete = false;
    data->command = strdup(command);
    
    // Show loading screen
    data->spinner = lv_spinner_create(lv_layer_top());
    lv_obj_add_style(data->spinner,&style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_center(data->spinner);
    
    // Create worker thread
    pthread_create(&data->thread_id, NULL, worker_thread, data);
    
    // Create timer to check for completion
    lv_timer_create(check_thread_complete, 30, data); // Check every 30ms
}

void generic_switch_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    thread_data_t * user_data = (thread_data_t *) lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");     

    if(lv_obj_has_state(target, LV_STATE_CHECKED)) {
        strcat(final_command,"on");
    } else {
        strcat(final_command,"off");
    }

    for(int i=0;i<MAX_CMD_ARGS;i++) {
        if (user_data->arguments[i]) {
            if (lv_obj_check_type(user_data->arguments[i],&lv_textarea_class)) {
                strcat(final_command," \"");
                strcat(final_command,lv_textarea_get_text(user_data->arguments[i]));
                strcat(final_command,"\"");
            }
        }
    }

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command);
}

void generic_dropdown_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    thread_data_t * user_data = (thread_data_t*) lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    char arg[100] = "";
    lv_dropdown_get_selected_str(target,arg,99);
    user_data->argument_string = strdup(arg);
    strcat(final_command,"\"");
    strcat(final_command,user_data->argument_string);
    strcat(final_command,"\"");

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command);

}

void generic_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    thread_data_t * user_data = lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    int value;
    value = lv_slider_get_value(target);
    user_data->argument_string = malloc(32);
    sprintf(user_data->argument_string, "%i", value);
    strcat(final_command,user_data->argument_string);
    strcat(final_command," ");    
    printf("final_command: %s\n",final_command);

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command);

}