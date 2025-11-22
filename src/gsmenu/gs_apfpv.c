#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "../input.h"
#include "helper.h"
#include "executor.h"
#include "styles.h"
#include "executor.h"
#include "gs_wifi.h"

extern lv_obj_t * menu;
extern gsmenu_control_mode_t control_mode;
extern lv_group_t * default_group;

extern lv_obj_t * rx_mode;

lv_obj_t *this_page;
lv_obj_t * ap_fpv_ssid;
lv_obj_t * ap_fpv_password;
lv_obj_t * reset_apfpv;
lv_obj_t * autoconnect_cont;

static int adapter_count = 0;

#define MAX_DEVICES 32
#define MAX_NAME_LENGTH 64
#define MAX_PATH_LENGTH 256

typedef struct {
    char device_name[MAX_NAME_LENGTH];
    char usb_port[MAX_NAME_LENGTH];
} NetworkDevice;

int get_usb_port_udev(const char* device_name, char* usb_port, size_t port_size) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), 
             "udevadm info -q path /sys/class/net/%s 2>/dev/null | grep -o 'usb[0-9]*/.*' | head -1", 
             device_name);
    
    char* result = run_command(cmd);
    if (result == NULL || strlen(result) == 0) {
        free(result);
        return -1;
    }
    
    // Remove newline and extract relevant portion
    result[strcspn(result, "\n")] = '\0';
    
    // Find the USB port part (typically after usbX/)
    char* usb_part = strstr(result, "usb");
    if (usb_part != NULL) {
        char* port_part = strchr(usb_part, '/');
        if (port_part != NULL) {
            port_part++; // Move past the slash
            
            // Copy up to next slash or end of string
            char* next_slash = strchr(port_part, '/');
            if (next_slash != NULL) {
                size_t len = next_slash - port_part;
                if (len < port_size) {
                    strncpy(usb_port, port_part, len);
                    usb_port[len] = '\0';
                    free(result);
                    return 0;
                }
            } else {
                // No more slashes, use the remaining string
                if (strlen(port_part) < port_size) {
                    strcpy(usb_port, port_part);
                    free(result);
                    return 0;
                }
            }
        }
    }
    
    free(result);
    return -1;
}

// Main function to get the list of network devices with USB ports
NetworkDevice* get_wireless_devices(int* count) {
    NetworkDevice* devices = malloc(MAX_DEVICES * sizeof(NetworkDevice));
    if (devices == NULL) {
        *count = 0;
        return NULL;
    }
    
    // Get wireless device names using ip command
    char* ip_output = run_command("ip -o link show | awk -F': ' '{print $2}' | grep '^wlx'");
    if (ip_output == NULL) {
        *count = 0;
        free(devices);
        return NULL;
    }
    
    *count = 0;
    char* line = ip_output;
    char* next_line;
    
    while (*count < MAX_DEVICES && (next_line = strchr(line, '\n')) != NULL) {
        // Extract device name
        size_t name_len = next_line - line;
        if (name_len > 0 && name_len < MAX_NAME_LENGTH) {
            strncpy(devices[*count].device_name, line, name_len);
            devices[*count].device_name[name_len] = '\0';
            
            // Remove trailing whitespace
            devices[*count].device_name[strcspn(devices[*count].device_name, " \t\n")] = '\0';
            
            // Get USB port information
            devices[*count].usb_port[0] = '\0';
            
            // Try udev method first
            get_usb_port_udev(devices[*count].device_name,
                                devices[*count].usb_port,
                                MAX_NAME_LENGTH);
            
            (*count)++;
        }
        
        line = next_line + 1;
    }
    
    free(ip_output);
    return devices;
}

// Example usage and cleanup function
void free_devices(NetworkDevice* devices) {
    free(devices);
}

void focus_label(lv_event_t* e)
{
    lv_obj_t* obj = lv_event_get_current_target(e);
    lv_obj_t* label = lv_event_get_user_data(e);
    lv_obj_scroll_to_view_recursive(label, LV_ANIM_ON);
}

static void btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e); 
    lv_obj_t * target_ta = lv_obj_get_user_data(btn); // Retrieve associated textarea

    if(code == LV_EVENT_CLICKED) {
        if (target_ta) {
            lv_keyboard_set_textarea(kb, target_ta);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_view_recursive(target_ta, LV_ANIM_OFF);
            lv_indev_wait_release(lv_event_get_param(e));
            lv_group_focus_obj(kb);
        }
    }
}

static void change_textarea_value(lv_event_t *e ,lv_obj_t * ta) {

    thread_data_t * user_data = (thread_data_t*) lv_obj_get_user_data(ta);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    user_data->argument_string = strdup(lv_textarea_get_text(ta));
    strcat(final_command,"\"");
    strcat(final_command,user_data->argument_string);
    strcat(final_command,"\"");

    run_command_and_block(e,final_command,NULL);
}

static void kb_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e);

    if (code == LV_EVENT_FOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;

    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
    else if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(NULL, ta);   /*To forget the last clicked object to make it focusable again*/
        lv_group_focus_obj(lv_obj_get_child_by_type(lv_obj_get_parent(lv_keyboard_get_textarea(kb)),0,&lv_button_class));
        lv_obj_update_layout(lv_obj_get_parent(kb));
    }

    if(code == LV_EVENT_READY) {
        change_textarea_value(e,lv_keyboard_get_textarea(kb));
    }
}

void checkbox_event_cb(void)
{
    generic_page_load_callback(this_page);
}

void apfpv_page_load_callback(lv_obj_t *page) {

    this_page = page;
    menu_page_data_t *menu_page_data = lv_obj_get_user_data(page);
    PageEntry *entries = menu_page_data->page_entries;

    if (adapter_count == 0 ) {
        NetworkDevice* devices = get_wireless_devices(&adapter_count);
        
        if (devices == NULL) {
            printf("Failed to get device list\n");
        }
        
        printf("Found %d wireless devices:\n", adapter_count);
        for (int i = 0; i < adapter_count; i++) {
            printf("Device: %s -> USB Port: %s\n", 
                devices[i].device_name, 
                devices[i].usb_port[0] ? devices[i].usb_port : "Unknown");
            lv_group_set_default(menu_page_data->indev_group);

            // Create dynamic strings
            char checkbox_label[128];
            char loading_text[128];
            char param_text[128];
            
            snprintf(checkbox_label, sizeof(checkbox_label), "Device: %s USB: %s", 
                    devices[i].device_name, 
                    devices[i].usb_port[0] ? devices[i].usb_port : "Unknown");
            
            snprintf(loading_text, sizeof(loading_text), "Loading device %s...", 
                    devices[i].device_name);

            snprintf(param_text, sizeof(param_text), "status %s", 
                    devices[i].device_name);
            
            lv_obj_t * adapter = create_checkbox(autoconnect_cont, LV_SYMBOL_WIFI, checkbox_label, devices[i].device_name, menu_page_data, false);
            thread_data_t* data = lv_obj_get_user_data(lv_obj_get_child_by_type(adapter,0,&lv_checkbox_class));
            data->callback_fn = checkbox_event_cb;
            lv_obj_t * adapter_status = create_text(autoconnect_cont, LV_SYMBOL_SETTINGS, "Status", param_text, menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);

            lv_obj_add_event_cb(lv_obj_get_child_by_type(adapter,0,&lv_checkbox_class), focus_label, LV_EVENT_FOCUSED, adapter_status);

            add_entry_to_menu_page(menu_page_data, loading_text, adapter, reload_checkbox_value);
            add_entry_to_menu_page(menu_page_data, loading_text, adapter_status, reload_label_value);

            lv_group_set_default(default_group);
        }
        
        free_devices(devices);

    }

    generic_page_load_callback(page);
}

void gs_actions_reset_apfpv(lv_event_t * event)
{   
    lv_textarea_set_text(lv_obj_get_child_by_type(ap_fpv_ssid,0,&lv_textarea_class),"OpenIPC");
    lv_textarea_set_text(lv_obj_get_child_by_type(ap_fpv_password,0,&lv_textarea_class),"12345678");
    run_command_and_block(event,"gsmenu.sh set gs apfpv reset",NULL);
    lv_obj_send_event(lv_obj_get_child_by_type(rx_mode,0,&lv_dropdown_class),LV_EVENT_VALUE_CHANGED,NULL);

    int child_count = lv_obj_get_child_count(autoconnect_cont);
    for (int i = child_count - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(autoconnect_cont, i);
        int grand_child_count = lv_obj_get_child_count(child);
        
        for (int j = grand_child_count - 1; j >= 0; j--) {
            lv_obj_t *grand_child = lv_obj_get_child(child, j);
            thread_data_t* user_data = lv_obj_get_user_data(grand_child);
            
            if (user_data != NULL) {
                delete_menu_page_entry_by_obj(user_data->menu_page_data,child);
            }
        }
        lv_obj_del(child);
    }
    adapter_count = 0;
}

void create_apfpv_menu(lv_obj_t * parent) {

    menu_page_data_t *menu_page_data = malloc(sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "apfpv");
    menu_page_data->page_load_callback = apfpv_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    menu_page_data->entry_count = 0;
    menu_page_data->page_entries = NULL;
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent,menu_page_data);

    create_text(parent, NULL, "Connection Settings", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    ap_fpv_ssid = create_textarea(cont, "", "SSID", "ssid", menu_page_data, false);
    ap_fpv_password = create_textarea(cont, "", "Password", "password", menu_page_data, true);

    reset_apfpv = create_button(section, "Reset APFPV");
    lv_obj_add_event_cb(lv_obj_get_child_by_type(reset_apfpv,0,&lv_button_class),gs_actions_reset_apfpv,LV_EVENT_CLICKED,NULL);

    lv_obj_t * kb = lv_keyboard_create(lv_obj_get_parent(section));
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_style(kb, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_dark_background, LV_PART_ITEMS| LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_textcolor, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);    
    
    lv_obj_add_event_cb(lv_obj_get_child_by_type(ap_fpv_ssid,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(ap_fpv_password,0,&lv_button_class), btn_event_cb, LV_EVENT_ALL, kb);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL,kb);
    lv_keyboard_set_textarea(kb, NULL);

    create_text(parent, NULL, "Autoconnect", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    autoconnect_cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(autoconnect_cont, LV_FLEX_FLOW_COLUMN);

    add_entry_to_menu_page(menu_page_data,"Loading SSID ...", ap_fpv_ssid, reload_textarea_value );
    add_entry_to_menu_page(menu_page_data,"Loading Password ...", ap_fpv_password, reload_textarea_value );

    lv_group_set_default(default_group);
}
