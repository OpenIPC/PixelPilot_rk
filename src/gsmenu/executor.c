#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <lvgl.h>
#include <math.h>
#include "executor.h"
#include "helper.h"
#include "styles.h"

#define MAX_OUTPUT_SIZE 4096
#define BUFFER_SIZE MAX_OUTPUT_SIZE * 3

typedef struct {
    char* command;
    char *stdout_output;
    char *stderr_output;
    int exit_status;
} CommandResult;


lv_group_t * current_group;
lv_group_t * error_group = NULL;
extern lv_obj_t * menu;
extern lv_indev_t * indev_drv;
lv_obj_t * msgbox = NULL;
lv_obj_t * msgbox_label = NULL;
char buffer[BUFFER_SIZE];
lv_group_t *loader_group = NULL;  /* was helper.c; only used as a NULL-check now */
extern lv_group_t * default_group;


void error_button_callback(lv_event_t * e) {
    lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
    menu_page_data_t* menu_page_data = lv_obj_get_user_data(current_page);
    lv_group_set_default(menu_page_data->indev_group);
    lv_indev_set_group(indev_drv,menu_page_data->indev_group);
    lv_obj_del(msgbox_label);
    lv_group_del(error_group);
    error_group = NULL;
    msgbox_label = NULL;
    msgbox = NULL;
    buffer[0] = '\0';
}


void build_output_string(char *buffer, const char *msgbox_text, CommandResult result ) {
    buffer[0] = '\0';
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "%s########\n", msgbox_text);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "command: %s\n", result.command);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "exit_status: %d\n", result.exit_status);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "stdout: %s\n", result.stdout_output);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "stderr: %s\n", result.stderr_output);
}

void show_error(CommandResult result) {
    lv_lock();

    if (!error_group) {
        error_group = lv_group_create();
        lv_group_set_default(error_group);
        if (loader_group)
            lv_indev_set_group(indev_drv,loader_group);
        else
            lv_indev_set_group(indev_drv,error_group);
    }

    if ( ! lv_obj_is_valid(msgbox)) {
        lv_obj_t * top = lv_layer_top();
        msgbox = lv_msgbox_create(top);
        lv_obj_t * backdrop = lv_obj_get_child_by_type(top,0,&lv_msgbox_backdrop_class);
        if (backdrop)
            lv_obj_swap(backdrop, msgbox);
        lv_obj_set_style_max_height(msgbox,lv_pct(80),LV_PART_MAIN);
        lv_msgbox_add_title(msgbox, "Error");
        lv_obj_t * button = lv_msgbox_add_close_button(msgbox);
        lv_obj_add_event_cb(button, error_button_callback, LV_EVENT_DELETE,NULL);
        lv_obj_add_style(button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        msgbox_label = lv_msgbox_add_text(msgbox,"");
        // lv_label_set_long_mode(msgbox_label, LV_LABEL_LONG_MODE_SCROLL);
        theme_msgbox(msgbox);
    };

    build_output_string(
        buffer,
        lv_label_get_text(msgbox_label),  // LVGL label text
        result
    );

    lv_label_set_text(msgbox_label,buffer);
    lv_obj_set_width(msgbox, lv_pct(80));
    lv_unlock();

}

char* run_command(const char* command) {
    CommandResult result = { NULL, NULL, NULL, -1 };

    result.command = strdup(command);

    // Create temporary files for stdout and stderr
    char stdout_file[] = "/tmp/stdout_XXXXXX";
    char stderr_file[] = "/tmp/stderr_XXXXXX";

    int stdout_fd = mkstemp(stdout_file);
    int stderr_fd = mkstemp(stderr_file);

    if (stdout_fd == -1 || stderr_fd == -1) {
        perror("Failed to create temporary files");
        free(result.command);
        return "";
    }

    // Construct the full command with redirections
    char full_command[MAX_OUTPUT_SIZE * 2];
    snprintf(full_command, sizeof(full_command),
        "%s > %s 2> %s",
        command, stdout_file, stderr_file);

    printf("Running command: %s\n", command);

    // Execute the command
    int status = system(full_command);
    result.exit_status = WEXITSTATUS(status);

    // Read stdout
    FILE *stdout_fp = fdopen(stdout_fd, "r");
    if (stdout_fp) {
        char buffer[MAX_OUTPUT_SIZE];
        result.stdout_output = malloc(MAX_OUTPUT_SIZE);
        result.stdout_output[0] = '\0';

        while (fgets(buffer, sizeof(buffer), stdout_fp)) {
            strcat(result.stdout_output, buffer);
        }
        fclose(stdout_fp);
    }

    // Read stderr
    FILE *stderr_fp = fdopen(stderr_fd, "r");
    if (stderr_fp) {
        char buffer[MAX_OUTPUT_SIZE];
        result.stderr_output = malloc(MAX_OUTPUT_SIZE);
        result.stderr_output[0] = '\0';

        while (fgets(buffer, sizeof(buffer), stderr_fp)) {
            strcat(result.stderr_output, buffer);
        }
        fclose(stderr_fp);
    }

    // Clean up temp files
    unlink(stdout_file);
    unlink(stderr_file);

    if (result.exit_status > 0) {
        show_error(result);
    }

    free(result.command);
    free(result.stderr_output);
    return result.stdout_output;
}

void* worker_thread(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;

    char * dummy = run_command(data->command);
    free(dummy);
    
    data->work_complete = true;
    return NULL;
}

void check_thread_complete(lv_timer_t* timer) {
    thread_data_t* data = (thread_data_t*)lv_timer_get_user_data(timer);    
    
    if(data->work_complete) {
        // Save callback BEFORE cleanup
        callback_fn cb = data->callback_fn;
        
        // Clean up resources
        pthread_join(data->thread_id, NULL);
        lv_obj_del(data->spinner);
        lv_timer_del(timer);
        
        // Handle error group if needed
        if (error_group) {
            lv_indev_set_group(indev_drv, error_group);
        } else {
            lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
            menu_page_data_t* menu_page_data = lv_obj_get_user_data(current_page);
            lv_indev_set_group(indev_drv,menu_page_data->indev_group);
        }

        // Free the command string if it exists
        if (data->command) {
            free(data->command);
        }
        
        // Call callback if it exists (after cleanup but before freeing data)
        if (cb != NULL) {  // Explicit NULL check
            cb();
        }
        
        // Finally free the data structure
        free(data);
    }
}

void run_command_and_block(lv_event_t* e, const char *command, callback_fn callback) {
    lv_obj_t* parent = lv_event_get_current_target(e);

    // disable input
    lv_indev_set_group(indev_drv,default_group);

    // Use calloc to zero-initialize the memory
    thread_data_t* data = calloc(1, sizeof(thread_data_t));
    if (!data) return;  // Always check allocation
    
    data->parent = parent;
    data->work_complete = false;
    data->command = strdup(command);
    data->callback_fn = callback;
    
    // Show loading screen
    data->spinner = openipc_spinner_create(lv_layer_top());
    
    // Create worker thread
    pthread_create(&data->thread_id, NULL, worker_thread, data);
    
    // Create timer to check for completion
    lv_timer_create(check_thread_complete, 30, data); // Check every 30ms
}