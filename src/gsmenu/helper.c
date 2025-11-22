#include <stdlib.h>
#include <pthread.h>
#include <math.h>
#include "../../lvgl/lvgl.h"
#include "gs_system.h"
#include "../input.h"
#include "helper.h"
#include "styles.h"
#include "ui.h"
#include "executor.h"
#include "../WiFiRSSIMonitor.h"

extern enum RXMode RXMODE;

extern gsmenu_control_mode_t control_mode;
extern lv_obj_t * menu;
extern lv_indev_t * indev_drv;
extern lv_obj_t * sub_gs_main_page;

extern lv_obj_t * air_presets_cont;
extern lv_obj_t * air_wfbng_cont;
extern lv_obj_t * air_alink_cont;
extern lv_obj_t * air_aalink_cont;
extern lv_obj_t * air_camera_cont;
extern lv_obj_t * air_telemetry_cont;
extern lv_obj_t * air_actions_cont;
extern lv_obj_t * gs_dvr_cont;
extern lv_obj_t * gs_wfbng_cont;
extern lv_obj_t * gs_apfpv_cont;
extern lv_obj_t * gs_system_cont;
extern lv_obj_t * gs_wlan_cont;
extern lv_obj_t * gs_actions_cont;
extern lv_group_t *main_group;
extern lv_group_t * error_group;
extern lv_obj_t * size; // air camera size setting wfb-ng only
extern lv_obj_t * fps; // air camera fps setting wfb-ng only
extern lv_obj_t * bitrate; // air camera bitrate setting wfb-ng only
extern lv_obj_t * video_mode; // air camera video_mode setting apfpv only
extern lv_obj_t * router;
extern lv_obj_t * osd_fps;
extern lv_obj_t * air_gs_rendering;
extern lv_obj_t * air_telemetry_msposd_text;
extern lv_obj_t * air_telemetry_msposd_section;


extern lv_obj_t * msgbox;

lv_group_t *loader_group;
pthread_t loader_thread;
lv_obj_t *loader_msgbox = NULL;

void on_focus(lv_event_t* e)
{
    lv_obj_t* obj = lv_event_get_current_target(e);
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(obj), LV_ANIM_ON);
}

void loader_cancel_button_cb(lv_event_t * e) {
    printf("Cancelling loader thread\n");
    
    // Cancel the loader thread
    pthread_cancel(loader_thread);

    menu_page_data_t *menu_page_data = lv_event_get_user_data(e);
    
    // Close the message box
    lv_lock();
    if (loader_msgbox)
        lv_msgbox_close(loader_msgbox);
    lv_unlock();
    
    // Restore input device group
    if (!error_group) {
        lv_indev_set_group(indev_drv, menu_page_data->indev_group);
    } else {
        // Find the first focusable object recursively
        lv_obj_t * first_obj = find_first_focusable_obj(msgbox);
        lv_group_focus_obj(first_obj);
        lv_indev_set_group(indev_drv, error_group);
    }

    lv_group_del(loader_group);
    loader_group = NULL;
}

void* generic_page_load_thread(void *arg) {
    // Set cancel type to deferred to allow cleanup
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    lv_obj_t *page = (lv_obj_t*)arg;
    menu_page_data_t *menu_page_data = lv_obj_get_user_data(page);
    PageEntry *entries = menu_page_data->page_entries;

    lv_lock(); // Lock LVGL before any GUI operations

    // Create progress UI
    loader_msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(loader_msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(loader_msgbox);
    lv_obj_t *bar = lv_bar_create(loader_msgbox);
    lv_obj_add_style(bar, &style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(bar, &style_openipc_dropdown, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_bar_set_range(bar, 0, menu_page_data->entry_count);
    lv_obj_center(bar);

    lv_obj_t * cancel_button =  lv_msgbox_add_footer_button(loader_msgbox, "Cancel");
    lv_group_add_obj(loader_group,cancel_button);
    lv_obj_add_style(cancel_button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(cancel_button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(cancel_button,loader_cancel_button_cb,LV_EVENT_CLICKED,menu_page_data);
    lv_obj_center(cancel_button);

    // lv_obj_set_width(loader_msgbox, LV_SIZE_CONTENT);
    // lv_obj_set_flex_grow(cancel_button, 1);
    // lv_obj_align_to(bar, cancel_button, LV_ALIGN_OUT_BOTTOM_MID, 0, -50);
    // lv_obj_set_height(loader_msgbox, LV_SIZE_CONTENT);


    lv_unlock();

    // Process entries with cancellation point
    for (int i = 0; i < menu_page_data->entry_count; i++) {
        // Add cancellation point
        pthread_testcancel();
        
        PageEntry *entry = &entries[i];
        if (entry->caption && entry->reload) {
            lv_lock();
            lv_label_set_text(label, entry->caption);
            lv_bar_set_value(bar, i, LV_ANIM_OFF);
            lv_unlock();
            entry->reload(page, entry->target);
        }
    }

    lv_lock();
    lv_msgbox_close(loader_msgbox);
    lv_unlock();

    if (! error_group)
        lv_indev_set_group(indev_drv,menu_page_data->indev_group);
    else
        lv_indev_set_group(indev_drv,error_group);

    lv_group_del(loader_group);
    loader_group = NULL;

    return NULL;
}

void generic_page_load_callback(lv_obj_t *page) {

    if (!loader_group)
        loader_group = lv_group_create();
    lv_indev_set_group(indev_drv,loader_group);
    pthread_create(&loader_thread, NULL, generic_page_load_thread, page);
    pthread_detach(loader_thread); // Don't wait for thread to finish
}

void generic_back_event_handler(lv_event_t * e) {
    lv_key_t key = lv_event_get_key(e);
    if (key == LV_KEY_HOME) {
        lv_menu_set_page(menu,NULL);
        lv_obj_remove_state(air_presets_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_wfbng_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_alink_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_aalink_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_camera_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_telemetry_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(air_actions_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_dvr_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_wfbng_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_apfpv_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_system_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_wlan_cont, LV_STATE_CHECKED);
        lv_obj_remove_state(gs_actions_cont, LV_STATE_CHECKED);
        lv_indev_set_group(indev_drv,main_group);
    }
}

lv_obj_t * create_text(lv_obj_t * parent, const char * icon, const char * txt, const char * parameter, menu_page_data_t* menu_page_data,bool blocking,lv_menu_builder_variant_t builder_variant)
{
    lv_obj_t * obj = lv_menu_cont_create(parent);
    //lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * img = NULL;
    lv_obj_t * label = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }

    if(txt) {
        label = lv_label_create(obj);
        lv_label_set_text(label, txt);
        // lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_user_data(obj,(void *)txt); // ugly but where else to store
    }

    if(builder_variant == LV_MENU_ITEM_BUILDER_VARIANT_2 && icon && txt) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        lv_obj_swap(img, label);
    }

    lv_obj_add_style(obj, &style_openipc, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_style(obj, &style_openipc, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &style_openipc, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(obj, &style_openipc_textcolor, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_style(obj, &style_openipc_disabled, LV_PART_MAIN | LV_STATE_DISABLED );

    if (menu_page_data) {
        thread_data_t* data = malloc(sizeof(thread_data_t));
        if (data) {
            memset(data, 0, sizeof(thread_data_t));
        }
        data->menu_page_data = menu_page_data;
        data->blocking = blocking;
        strcpy(data->parameter, parameter);

        lv_obj_set_user_data(label,data);
    }

    lv_obj_add_event_cb(label, on_focus, LV_EVENT_FOCUSED, NULL);

    return obj;
}

static void slider_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    lv_obj_t * slider_label = lv_event_get_user_data(e);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(slider);

    switch (code)
    {
    case LV_EVENT_CLICKED:
        {
            if (control_mode == GSMENU_CONTROL_MODE_NAV) {
                printf("GSMENU_CONTROL_MODE_SLIDER\n");
                lv_obj_add_style(slider, &style_openipc_outline, LV_PART_KNOB | LV_STATE_DEFAULT);
                lv_obj_remove_style(slider, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

                // Free previous user data if it exists
                int32_t *old_value = lv_obj_get_user_data(slider_label);
                if (old_value) free(old_value);
                lv_obj_set_user_data(slider_label,NULL);

                int32_t *start_value = malloc(sizeof(int32_t));
                *start_value = lv_slider_get_value(slider);
                lv_obj_set_user_data(slider_label, start_value);

                control_mode = GSMENU_CONTROL_MODE_SLIDER;
            } else {
                lv_obj_remove_style(slider, &style_openipc_outline, LV_PART_KNOB | LV_STATE_DEFAULT);
                lv_obj_add_style(slider, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
                
                control_mode = GSMENU_CONTROL_MODE_NAV;
            }
            break;
        }
    case LV_EVENT_CANCEL:
        {
            lv_obj_remove_style(slider, &style_openipc_outline, LV_PART_KNOB | LV_STATE_DEFAULT);
            lv_obj_add_style(slider, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

            int32_t *start_value = lv_obj_get_user_data(slider_label);
            if (start_value) {
                if (*start_value != lv_slider_get_value(slider)) {
                    char buf[32];
                    char format[16];
                    snprintf(format, sizeof(format), "%%.%df", param_user_data->precision);
                    float value = *start_value / powf(10, param_user_data->precision);
                    snprintf(buf, sizeof(buf), format, value);
                    lv_label_set_text(slider_label, buf);
                    lv_slider_set_value(slider, *start_value, LV_ANIM_OFF);
                }
                free(start_value);
                lv_obj_set_user_data(slider_label, NULL);
            }

            control_mode = GSMENU_CONTROL_MODE_NAV;
            break;  
        }
    case LV_EVENT_VALUE_CHANGED:
        {
            char buf[32];
            char format[16];
            snprintf(format, sizeof(format), "%%.%df", param_user_data->precision);
            float value = (float)lv_slider_get_value(slider) / powf(10, param_user_data->precision);
            snprintf(buf, sizeof(buf), format, value);
            lv_label_set_text(slider_label, buf);
            break;
        }
    default:
        break;
    }
}

lv_obj_t * create_slider(lv_obj_t * parent, const char * icon, const char * txt, const char * parameter, menu_page_data_t* menu_page_data, bool blocking, int precision)
{
    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * img = NULL;
    lv_obj_t * label = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }

    if(txt) {
        label = lv_label_create(obj);
        lv_label_set_text(label, txt);
        //lv_obj_set_flex_grow(label, 1);
    }    

    // Create a buffer for the float value display
    char format[16];
    snprintf(format, sizeof(format), "%%.%df", precision);
    
    lv_obj_t * slider_label = lv_label_create(obj);
    char s[32];
    snprintf(s, sizeof(s), format, 0.0f); // Initialize with 0.0
    lv_label_set_text(slider_label, s);

    lv_obj_t * slider = lv_slider_create(obj);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_add_style(slider, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);    
    lv_obj_add_style(slider, &style_openipc, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc, LV_STATE_FOCUS_KEY);

    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_ALL, slider_label);
    lv_obj_add_event_cb(slider, on_focus, LV_EVENT_FOCUSED, NULL);

    thread_data_t* data = malloc(sizeof(thread_data_t));
    if (data) {
        memset(data, 0, sizeof(thread_data_t));
    }
    data->menu_page_data = menu_page_data;
    data->blocking = blocking;
    data->precision = precision;
    strcpy(data->parameter, parameter);

    lv_obj_set_user_data(slider,data);

    lv_obj_add_event_cb(slider, generic_slider_event_cb, LV_EVENT_CLICKED,data);
    lv_obj_add_event_cb(slider, generic_back_event_handler, LV_EVENT_KEY,NULL);

    get_slider_value(obj);

    return obj;
}

void generic_button_callback(lv_event_t * e) {
    lv_obj_t * target = lv_event_get_target(e);
    lv_obj_t * button = lv_obj_get_child_by_type(target,0,&lv_button_class);
    lv_obj_t * button_label = lv_obj_get_child_by_type(target,0,&lv_label_class);
    menu_page_data_t* menu_page_data = (menu_page_data_t*) lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh button ";
    strcat(final_command,menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,menu_page_data->page);
    strcat(final_command," \"");
    strcat(final_command,lv_label_get_text(button_label));
    strcat(final_command,"\"");
    run_command(final_command);
}

lv_obj_t * create_button(lv_obj_t * parent, const char * txt)
{
    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * label = NULL;

    lv_obj_t * btn = lv_btn_create(obj);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);

    if(txt) {
        label = lv_label_create(btn);
        lv_label_set_text(label, txt);
    }    

    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(btn, generic_back_event_handler, LV_EVENT_KEY,NULL);
    lv_obj_add_event_cb(btn, on_focus, LV_EVENT_FOCUSED, NULL);


    return obj;
}

static void lv_spinbox_increment_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_SHORT_CLICKED || code  == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_increment(lv_event_get_user_data(e));
    }
}

static void lv_spinbox_decrement_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        lv_spinbox_decrement(lv_event_get_user_data(e));
    }
}

lv_obj_t * create_spinbox(lv_obj_t * parent, const char * icon, const char * txt, int32_t min, int32_t max,
                                int32_t val)
{
    lv_obj_t * obj = lv_menu_cont_create(parent);
    lv_obj_t * img = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }    

    lv_obj_t * label = lv_label_create(obj);
    lv_label_set_text(label, txt);
    lv_label_set_text(label, txt);

    lv_obj_t * spinbox = lv_spinbox_create(obj);
    lv_obj_add_state(spinbox, LV_STATE_DISABLED);
    lv_spinbox_set_digit_format(spinbox, 2, 0);
    lv_group_remove_obj(spinbox);
    lv_spinbox_set_value(spinbox, val);
    lv_spinbox_set_range(spinbox, min, max);
    lv_obj_t * btn = lv_button_create(obj);
    lv_obj_set_style_bg_image_src(btn, LV_SYMBOL_PLUS, 0);
    lv_obj_add_event_cb(btn, lv_spinbox_increment_event_cb, LV_EVENT_ALL,  spinbox);

    btn = lv_button_create(obj);
    lv_obj_set_style_bg_image_src(btn, LV_SYMBOL_MINUS, 0);
    lv_obj_add_event_cb(btn, lv_spinbox_decrement_event_cb, LV_EVENT_ALL, spinbox);


    return obj;
}

lv_obj_t * create_switch(lv_obj_t * parent, const char * icon, const char * txt,const char * parameter, menu_page_data_t* menu_page_data,bool blocking)
{
    lv_obj_t * obj = lv_menu_cont_create(parent);
    lv_obj_t * img = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }

    lv_obj_t * label = lv_label_create(obj);
    lv_label_set_text(label, txt);

    lv_obj_t * sw = lv_switch_create(obj);
    lv_obj_add_state(sw, false);
    lv_obj_add_style(sw, &style_openipc, LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(sw, &style_openipc, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(sw, &style_openipc_outline, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(sw, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(sw, &style_openipc_dark_background, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_style(sw, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (menu_page_data) { 
        thread_data_t* data = malloc(sizeof(thread_data_t));
        if (data) {
            memset(data, 0, sizeof(thread_data_t));
        }
        data->menu_page_data = menu_page_data;
        data->blocking = blocking;
        strcpy(data->parameter, parameter);

        lv_obj_set_user_data(sw,data);

        lv_obj_add_event_cb(sw, generic_switch_event_cb, LV_EVENT_VALUE_CHANGED,data);
    }

    lv_obj_add_event_cb(sw, generic_back_event_handler, LV_EVENT_KEY,NULL);
    lv_obj_add_event_cb(sw, on_focus, LV_EVENT_FOCUSED, NULL);


    return obj;
}

void dropdown_event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    switch (code)
    {
    case LV_EVENT_CANCEL:
    case LV_EVENT_VALUE_CHANGED: {
        control_mode = GSMENU_CONTROL_MODE_NAV;
        break;
    }
    case LV_EVENT_READY:
        control_mode = GSMENU_CONTROL_MODE_EDIT;
    default:
        break;
    }
}

lv_obj_t * create_dropdown(lv_obj_t * parent, const char * icon, const char * label_txt, const char * txt,const char * parameter, menu_page_data_t* menu_page_data,bool blocking)
{

    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * img = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }    

    lv_obj_t * label = lv_label_create(obj);
    lv_label_set_text(label, label_txt);

    lv_obj_t * dd = lv_dropdown_create(obj); 
    lv_dropdown_set_dir(dd, LV_DIR_RIGHT);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_RIGHT);
    lv_obj_set_width(dd,300); // someone got a better idea ?

    lv_obj_add_style(dd, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(dd, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * list = lv_dropdown_get_list(dd);
    lv_obj_add_style(list, &style_openipc, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_style(list, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(dd, dropdown_event_handler, LV_EVENT_ALL, NULL);

    thread_data_t* data = malloc(sizeof(thread_data_t));
    if (data) {
        memset(data, 0, sizeof(thread_data_t));
    }
    data->menu_page_data = menu_page_data;
    data->blocking = blocking;
    strcpy(data->parameter, parameter);

    lv_obj_set_user_data(dd,data);

    lv_obj_add_event_cb(dd, generic_dropdown_event_cb, LV_EVENT_VALUE_CHANGED,data);
    lv_obj_add_event_cb(dd, generic_back_event_handler, LV_EVENT_KEY,NULL);
    lv_obj_add_event_cb(dd, on_focus, LV_EVENT_FOCUSED, NULL);

    get_dropdown_value(obj);

    return obj;
}


lv_obj_t * create_checkbox(lv_obj_t * parent, const char * icon, const char * label_txt, const char * parameter, menu_page_data_t* menu_page_data,bool blocking)
{

    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * img = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }    

    lv_obj_t * cb;
    cb = lv_checkbox_create(obj);
    lv_checkbox_set_text(cb, label_txt);

    lv_obj_add_style(cb, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xff4c60d8), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0xff4c60d8), LV_PART_INDICATOR | LV_STATE_CHECKED);

    thread_data_t* data = malloc(sizeof(thread_data_t));
    if (data) {
        memset(data, 0, sizeof(thread_data_t));
    }
    data->menu_page_data = menu_page_data;
    data->blocking = blocking;
    strcpy(data->parameter, parameter);

    lv_obj_set_user_data(cb,data);

    lv_obj_add_event_cb(cb, generic_checkbox_event_cb, LV_EVENT_VALUE_CHANGED,data);
    lv_obj_add_event_cb(cb, generic_back_event_handler, LV_EVENT_KEY,NULL);

    return obj;
}

lv_obj_t * create_backbutton(lv_obj_t * parent, const char * icon, const char * label_txt)
{

    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * img = NULL;

    if(icon) {
        img = lv_image_create(obj);
        lv_image_set_src(img, icon);
    }    

    lv_obj_t *back_button = lv_btn_create(obj);
    lv_obj_add_event_cb(back_button, generic_back_event_handler, LV_EVENT_CLICKED,NULL);    
    lv_obj_t * label = lv_label_create(back_button);
    lv_label_set_text(label, label_txt);
    return obj;
}

lv_obj_t * create_textarea(lv_obj_t * parent, char * text, const char * label_txt, const char * parameter, menu_page_data_t* menu_page_data, bool password)
{

    lv_obj_t * obj = lv_menu_cont_create(parent);

    lv_obj_t * label = lv_label_create(obj);
    lv_label_set_text(label, label_txt);    

    lv_obj_t * ta = lv_textarea_create(obj);
    //lv_textarea_set_placeholder_text(ta_ssid, "SSID");
    lv_textarea_set_one_line(ta,true);
    lv_textarea_set_text(ta,text);
    lv_obj_add_state(ta, LV_STATE_DISABLED);
    lv_obj_add_style(ta, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(ta, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    thread_data_t* data = malloc(sizeof(thread_data_t));
    if (data) {
        memset(data, 0, sizeof(thread_data_t));
    }
    data->menu_page_data = menu_page_data;
    data->blocking = false;
    strcpy(data->parameter, parameter);

    lv_obj_set_user_data(ta,data);    

    lv_obj_t * button = lv_button_create(obj);
    lv_obj_set_user_data(button, ta); // Associate button with text area
    lv_obj_add_style(button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(button, generic_back_event_handler, LV_EVENT_KEY,NULL);

    lv_obj_t * button_label = lv_label_create(button);
    lv_label_set_text(button_label,LV_SYMBOL_KEYBOARD);


    return obj;
}

// Recursive function to find the first focusable object
lv_obj_t * find_first_focusable_obj(lv_obj_t * parent) {
    // Iterate through all children of the parent
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return NULL;
    for (int i = 0; i < lv_obj_get_child_cnt(parent); i++) {
        lv_obj_t * child = lv_obj_get_child(parent, i);

        // Check if the child is focusable
        if ((lv_obj_has_flag(child, LV_OBJ_FLAG_CHECKABLE) || lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) && ! lv_obj_has_state(child, LV_STATE_DISABLED)) {
            lv_group_t * group = lv_obj_get_group(child);
            if (group != NULL) {
                return child; // Return the first focusable object
            } else {
                // The object is not part of any group
            }            
        }

        // Recursively check the child's children
        lv_obj_t * result = find_first_focusable_obj(child);
        if (result) {
            return result; // Return the first focusable object found in the subtree
        }
    }

    return NULL; // No focusable object found
}

void handle_sub_page_load(lv_event_t *e) {

    lv_obj_t * menu = lv_event_get_current_target(e); // Get the menu object
    lv_obj_t * page = lv_menu_get_cur_main_page(menu); // Get the current main page

    if ( ! page )
        return;

    menu_page_data_t * menu_page_data = (menu_page_data_t *) lv_obj_get_user_data(page);
    // set the indev to the pages group
    lv_indev_set_group(indev_drv,menu_page_data->indev_group);

    if(menu_page_data != NULL && menu_page_data->page_load_callback != NULL) 
        menu_page_data->page_load_callback(page);

    // Find the first focusable object recursively
    lv_obj_t * first_obj = find_first_focusable_obj(page);
    lv_group_focus_obj(first_obj);
}

char* get_paramater(lv_obj_t * page, char * param) {
    menu_page_data_t* menu_page_data = (menu_page_data_t*) lv_obj_get_user_data(page);
    char final_command[200] = "gsmenu.sh get ";
    strcat(final_command,menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,menu_page_data->page);
    strcat(final_command," "); 
    strcat(final_command,param);
    char * result = run_command(final_command);
    size_t len = strlen(result);
    if (len > 0 && result[len - 1] == '\n') {
        result[len - 1] = '\0';
    }
    return result;
}


void reload_label_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t *obj = lv_obj_get_child_by_type(parameter, 0, &lv_label_class);
    thread_data_t *param_user_data = (thread_data_t*)lv_obj_get_user_data(obj);
    
    // Get the parameter value
    const char *param_value = get_paramater(page, param_user_data->parameter);

    // Original label
    const char *org_txt = (const char*)lv_obj_get_user_data(parameter);
    
    // Create the combined string
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", org_txt, param_value);
    
    // Set the label text
    lv_lock();
    lv_label_set_text(obj, buffer);
    lv_unlock();
}

void reload_switch_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_switch_class);
    if ( !lv_obj_has_state(obj, LV_STATE_DISABLED) && ! lv_obj_has_flag(parameter,LV_OBJ_FLAG_HIDDEN)) {
        thread_data_t * param_user_data = (thread_data_t*) lv_obj_get_user_data(obj);
        bool value = atoi(get_paramater(page,param_user_data->parameter));
        lv_lock();
        lv_obj_set_state(obj,LV_STATE_CHECKED,value);
        lv_unlock();
    }
}

void reload_dropdown_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_dropdown_class);
    if ( !lv_obj_has_state(obj, LV_STATE_DISABLED) && ! lv_obj_has_flag(parameter,LV_OBJ_FLAG_HIDDEN)) {
        thread_data_t * param_user_data = (thread_data_t*) lv_obj_get_user_data(obj);
        char * value = get_paramater(page, param_user_data->parameter);
        lv_lock();
        lv_dropdown_set_selected(obj,lv_dropdown_get_option_index(obj,value));
        lv_unlock();
    }
}

void reload_checkbox_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_checkbox_class);
    if ( !lv_obj_has_state(obj, LV_STATE_DISABLED) && ! lv_obj_has_flag(parameter,LV_OBJ_FLAG_HIDDEN)) {
        thread_data_t * param_user_data = (thread_data_t*) lv_obj_get_user_data(obj);
        bool value = atoi(get_paramater(page,param_user_data->parameter));
        lv_lock();
        lv_obj_set_state(obj,LV_STATE_CHECKED,value);
        lv_unlock();
    }
}

void reload_textarea_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_textarea_class);
    // if ( !lv_obj_has_state(obj, LV_STATE_DISABLED) && ! lv_obj_has_flag(parameter,LV_OBJ_FLAG_HIDDEN)) { // ToDo: This need rework
        thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
        const char * value = get_paramater(page,param_user_data->parameter);
        lv_lock();
        lv_textarea_set_text(obj,value);
        lv_unlock();
    // }
}

void reload_slider_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_slider_class);
    lv_obj_t * label = lv_obj_get_child_by_type(parameter,1,&lv_label_class);
    if ( !lv_obj_has_state(obj, LV_STATE_DISABLED) && ! lv_obj_has_flag(parameter,LV_OBJ_FLAG_HIDDEN)) {
        thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
        char * value = get_paramater(page,param_user_data->parameter);
        float current_value;
        if (sscanf(value, "%f", &current_value) != 1) {
            return;
        }
        int32_t scaled_value = (int32_t)(current_value * powf(10, param_user_data->precision));
        // Create a buffer for the float value display
        char format[16];
        snprintf(format, sizeof(format), "%%.%df", param_user_data->precision);
        char s[32];
        snprintf(s, sizeof(s), format, current_value);
        lv_lock();
        lv_slider_set_value(obj,scaled_value,LV_ANIM_OFF);
        lv_label_set_text(label,s);
        lv_unlock();
    }
}

char* get_values(thread_data_t * data) {
    menu_page_data_t* menu_page_data = data->menu_page_data;
    char final_command[200] = "gsmenu.sh values ";
    strcat(final_command,menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,menu_page_data->page);
    strcat(final_command," "); 
    strcat(final_command,data->parameter);
    return run_command(final_command);
}

void get_slider_value(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parent,0,&lv_slider_class);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
    char * value_line = get_values(param_user_data);
    float min, max;
    if (sscanf(value_line, "%f %f", &min, &max) != 2) {
        return;
    }
    
    // Scale the min/max values by the precision
    int32_t scaled_min = (int32_t)(min * powf(10, param_user_data->precision));
    int32_t scaled_max = (int32_t)(max * powf(10, param_user_data->precision));
    lv_slider_set_range(obj, scaled_min, scaled_max);
}


void update_dropdown_width(lv_obj_t * dropdown) {
    const char * options = lv_dropdown_get_options(dropdown);
    uint16_t option_cnt = lv_dropdown_get_option_cnt(dropdown);
    
    lv_coord_t max_width = 0;
    lv_point_t p;
    
    // Get the style to use for font info
    const lv_font_t * font = lv_obj_get_style_text_font(dropdown, LV_PART_MAIN);
    
    // Iterate through options (split by \n)
    const char * opt = options;
    for(uint16_t i = 0; i < option_cnt; i++) {
        const char * next_opt = strchr(opt, '\n');
        size_t len = next_opt ? (size_t)(next_opt - opt) : strlen(opt);
        
        // Get text width
        lv_txt_get_size(&p, opt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        
        if(p.x > max_width) max_width = p.x;
        
        if(!next_opt) break;
        opt = next_opt + 1;
    }
    
    // Add some padding (adjust as needed)
    max_width += lv_obj_get_style_pad_left(dropdown, LV_PART_MAIN) + 
                 lv_obj_get_style_pad_right(dropdown, LV_PART_MAIN) + 
                 20; // Extra space for the arrow
    
    lv_obj_set_width(dropdown, max_width);
}

void get_dropdown_value(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parent,0,&lv_dropdown_class);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
    lv_dropdown_set_options(obj,get_values(param_user_data));
    update_dropdown_width(obj);
}

bool file_exists(const char *path) {
    lv_fs_file_t f;
    lv_fs_res_t res = lv_fs_open(&f, path, LV_FS_MODE_RD);
    if(res == LV_FS_RES_OK) {
        lv_fs_close(&f);
        return true;
    }
    return false;
}

const char* find_resource_file(const char* relative_path) {

    static char path[256];

    // Try installed locations
    const char* prefixes[] = {
        "/usr/local/share/pixelpilot",
        "/usr/share/pixelpilot",
        "./src/icons",
    };

    for(size_t i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "A:%s/%s", prefixes[i], relative_path);
        if(file_exists(path)) {
            return path;
        }
    }

    return NULL; // Not found
}

void gsmenu_toggle_rxmode() {


    switch (RXMODE)
    {
    case APFPV:
        lv_obj_add_flag(air_wfbng_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(router, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(osd_fps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(air_gs_rendering, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(air_telemetry_msposd_text,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(air_telemetry_msposd_section,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(air_alink_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(gs_wfbng_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(size, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bitrate, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(gs_apfpv_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(video_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(air_aalink_cont, LV_OBJ_FLAG_HIDDEN);
        setenv("REMOTE_IP" , "192.168.0.1", 1);
        setenv("AIR_FIRMWARE_TYPE" , "apfpv", 1);
        break;
    case WFB:
        lv_obj_remove_flag(air_wfbng_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(router, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(osd_fps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(air_gs_rendering, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(air_telemetry_msposd_text,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(air_telemetry_msposd_section,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(air_alink_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(gs_wfbng_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(size, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(fps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bitrate, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(gs_apfpv_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(video_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(air_aalink_cont, LV_OBJ_FLAG_HIDDEN);
        setenv("REMOTE_IP" , "10.5.0.10", 1);
        setenv("AIR_FIRMWARE_TYPE" , "wfb", 1);
#ifndef USE_SIMULATOR
        wifi_rssi_monitor_reset();
#endif
        break;

    default:
        break;
    }
}

void add_entry_to_menu_page(menu_page_data_t *menu_page_data, const char* text, lv_obj_t* obj, ReloadFunc reload_func) {
    // Increase entry count
    menu_page_data->entry_count++;
    
    // Reallocate memory for entries
    PageEntry *new_entries = realloc(menu_page_data->page_entries, 
                                    sizeof(PageEntry) * menu_page_data->entry_count);
    
    if (new_entries) {
        menu_page_data->page_entries = new_entries;
        
        // Add new entry at the end with string copy
        menu_page_data->page_entries[menu_page_data->entry_count - 1] = 
            (PageEntry){ strdup(text), obj, reload_func };
    } else {
        // Handle allocation failure
        menu_page_data->entry_count--; // Revert count on failure
    }
}

void delete_menu_page_entry_by_obj(menu_page_data_t *menu_page_data, lv_obj_t* obj) {
    if (!menu_page_data || !menu_page_data->page_entries || menu_page_data->entry_count == 0) {
        return;
    }
    
    // Find the index of the entry with matching object
    int found_index = -1;
    for (size_t i = 0; i < menu_page_data->entry_count; i++) {
        if (menu_page_data->page_entries[i].target == obj) {
            found_index = i;
            break;
        }
    }
    
    if (found_index == -1) {
        return;
    }
    
    // Free the duplicated string
    if (menu_page_data->page_entries[found_index].caption) {
        free((void*)menu_page_data->page_entries[found_index].caption);
    }
    
    // Shift entries
    for (size_t i = found_index; i < menu_page_data->entry_count - 1; i++) {
        menu_page_data->page_entries[i] = menu_page_data->page_entries[i + 1];
    }
    
    menu_page_data->entry_count--;
    
    // Only reallocate if we have entries left
    if (menu_page_data->entry_count > 0) {
        PageEntry *new_entries = realloc(menu_page_data->page_entries, 
                                        sizeof(PageEntry) * menu_page_data->entry_count);
        // If realloc succeeds, use the new pointer. If it fails, keep the old one.
        if (new_entries) {
            menu_page_data->page_entries = new_entries;
        }
    } else {
        free(menu_page_data->page_entries);
        menu_page_data->page_entries = NULL;
    }
}