#include <stdlib.h>
#include "../../lvgl/lvgl.h"
#include "../input.h"
#include "helper.h"
#include "styles.h"
#include "ui.h"
#include "executor.h"

extern gsmenu_control_mode_t control_mode;
extern lv_obj_t * menu;
extern lv_group_t *current_group;
extern lv_indev_t * indev_drv;

lv_obj_t * create_text(lv_obj_t * parent, const char * icon, const char * txt,
                              lv_menu_builder_variant_t builder_variant)
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
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(label, 1);
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

    return obj;
}


static void slider_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    lv_obj_t * slider_label = lv_event_get_user_data(e);

    switch (code)
    {
    case LV_EVENT_FOCUSED:
        {
            printf("forcus\n");
            control_mode = GSMENU_CONTROL_MODE_SLIDER;
            break;
        }
    case LV_EVENT_DEFOCUSED:
        {
            printf("de-forcus\n");
            control_mode = GSMENU_CONTROL_MODE_NAV;
            break;  
        }
    case LV_EVENT_VALUE_CHANGED:
        {
            char buf[8];
            lv_snprintf(buf, sizeof(buf), "%d", (int)lv_slider_get_value(slider));
            lv_label_set_text(slider_label, buf);
            //lv_obj_align_to(slider_label, slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
            break;  
        }
    default:
        break;
    }
}

lv_obj_t * create_slider(lv_obj_t * parent, const char * icon, const char * txt, int32_t min, int32_t max, int32_t val,const char * parameter, menu_page_data_t* menu_page_data,bool blocking)
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

    lv_obj_t * slider_label = lv_label_create(obj);
    char s[11]; 
    sprintf(s,"%i", val);    
    lv_label_set_text(slider_label, s);

    lv_obj_t * slider = lv_slider_create(obj);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_add_style(slider, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);    
    lv_obj_add_style(slider, &style_openipc, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(slider, &style_openipc, LV_STATE_FOCUS_KEY);

    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_ALL, slider_label);

    thread_data_t* data = malloc(sizeof(thread_data_t));
    if (data) {
        memset(data, 0, sizeof(thread_data_t));
    }
    data->menu_page_data = menu_page_data;
    data->blocking = blocking;
    strcpy(data->parameter, parameter);

    lv_obj_set_user_data(slider,data);

    lv_obj_add_event_cb(slider, generic_slider_event_cb, LV_EVENT_VALUE_CHANGED,data);    

    get_slider_value(obj);

    return obj;
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

    return obj;
}

void dropdown_event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    lv_key_t key = lv_event_get_key(e);
    switch (code)
    {
    case LV_EVENT_CANCEL:
    case LV_EVENT_VALUE_CHANGED: {
        char buf[32];
        lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
        control_mode = GSMENU_CONTROL_MODE_NAV;
        break;
    }
    case LV_EVENT_READY:
        printf("Switching control mode\n");
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

    get_dropdown_value(obj);

    return obj;
}

void backbutton_event_handler(lv_event_t * e) {
    // lv_menu_set_page(menu, lv_menu_get_cur_main_page(menu));
    // lv_menu_clear_history(menu);
    // handle_sub_page_load(NULL);
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
    lv_obj_add_event_cb(back_button, backbutton_event_handler, LV_EVENT_CLICKED,NULL);    
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

    lv_obj_t * button_label = lv_label_create(button);
    lv_label_set_text(button_label,LV_SYMBOL_KEYBOARD);


    return obj;
}

// Recursive function to find the first focusable object
lv_obj_t * find_first_focusable_obj(lv_obj_t * parent) {
    // Iterate through all children of the parent
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
    if(menu_page_data != NULL && menu_page_data->page_load_callback != NULL) 
        menu_page_data->page_load_callback(page);

    // Find the first focusable object recursively
    lv_obj_t * first_obj = find_first_focusable_obj(page);

    // Focus the first focusable object
    if (first_obj) {
        lv_group_t * group = lv_group_get_default();
        if (group) {
            lv_group_focus_obj(first_obj);
        }
    }

}

char* get_paramater(lv_obj_t * page, char * param) {
    menu_page_data_t* menu_page_data = (menu_page_data_t*) lv_obj_get_user_data(page);
    char final_command[200] = "gsmenu.sh get ";
    strcat(final_command,menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,menu_page_data->page);
    strcat(final_command," "); 
    strcat(final_command,param); 
    return run_command(final_command);
}


void reload_switch_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_switch_class);
    thread_data_t * param_user_data = (thread_data_t*) lv_obj_get_user_data(obj);
    lv_obj_set_state(obj,LV_STATE_CHECKED,atoi(get_paramater(page,param_user_data->parameter)));  
}

void reload_dropdown_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_dropdown_class);
    thread_data_t * param_user_data = (thread_data_t*) lv_obj_get_user_data(obj);
    char * value = get_paramater(page, param_user_data->parameter);
    lv_dropdown_set_selected(obj,lv_dropdown_get_option_index(obj,value),LV_ANIM_OFF);
}

void reload_textarea_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_textarea_class);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
    lv_textarea_set_text(obj,get_paramater(page,param_user_data->parameter));
}

void reload_slider_value(lv_obj_t * page,lv_obj_t * parameter) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parameter,0,&lv_slider_class);
    lv_obj_t * label = lv_obj_get_child_by_type(parameter,1,&lv_label_class);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
    char * value = get_paramater(page,param_user_data->parameter);
    lv_slider_set_value(obj,atoi(value),LV_ANIM_OFF);
    lv_label_set_text(label,value);
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
    int min, max;
    if (sscanf(value_line, "%d %d", &min, &max) != 2) {
        return;
    }
    lv_slider_set_range(obj,min,max);
}

void get_dropdown_value(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_get_child_by_type(parent,0,&lv_dropdown_class);
    thread_data_t * param_user_data  = (thread_data_t*) lv_obj_get_user_data(obj);
    lv_dropdown_set_options(obj,get_values(param_user_data));
}