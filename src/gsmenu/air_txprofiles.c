#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "styles.h"
#include "ui.h"
#include "helper.h"
#include "../input.h"
#include "executor.h"


/* Example txprofiles input 
# <ra - nge> <gi> <mcs> <fecK> <fecN> <bitrate> <gop> <Pwr> <roiQP> <bandwidth> <qpDelta>
999 - 999 long 0 2 3 1000 10 40 0,0,0,0 20 -12 
1000 - 1100 long 0 6 9 2000 10 40 0,0,0,0 20 -12 
1101 - 1200 long 1 8 12 4000 10 35 0,0,0,0 20 -12 
1201 - 1300 long 2 8 12 8000 10 30 12,8,8,12 20 -12 
1301 - 1400 long 2 8 12 10000 10 30 2,1,1,2 20 -12 
1401 - 1600 long 3 8 12 13000 10 30 2,1,1,2 20 -12 
1601 - 1800 long 4 10 15 16000 10 30 0,0,0,0 20 -12 
1801 - 2001 long 4 11 15 19000 10 30 0,0,0,0 20 -12
*/

// Define the number of columns based on the new structure (Range is split into Min/Max)
#define TABLE_COLS 12
#define MAX_LINE_LENGTH 256
#define MAX_CELL_LENGTH 32

extern gsmenu_control_mode_t control_mode;
extern lv_indev_t * indev_drv;
extern lv_group_t * default_group;
extern lv_obj_t * pp_menu_screen;
lv_group_t * tx_profile_group;
extern lv_obj_t * sub_air_alink_page;

lv_obj_t * input_box;
lv_obj_t * input_cont;
lv_obj_t * input_textarea;
lv_obj_t * air_txprofiles_save_button;
lv_obj_t * kb;

// Global reference to the table
lv_obj_t * table;

static void draw_event_cb(lv_event_t * e) {
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc = lv_draw_task_get_draw_dsc(draw_task);
    if (base_dsc == NULL || base_dsc->part != LV_PART_ITEMS) return;

    lv_area_t draw_task_area;
    lv_draw_task_get_area(draw_task, &draw_task_area);

    uint32_t row = base_dsc->id1;
    uint32_t col = base_dsc->id2;

    // Get existing data (if any)
    lv_point_t *point = lv_table_get_cell_user_data(table, row, col);

    if (!point) {
        point = lv_malloc(sizeof(lv_point_t));  // Allocate only if missing
        lv_table_set_cell_user_data(table, row, col, point);  // Assign immediately
    }

    // Update coordinates
    point->x = draw_task_area.x1;
    point->y = draw_task_area.y1;

    // printf("row: %i, col: %i, x: %i, y: %i\n", row, col, point->x, point->y);
}


// Helper function to calculate text width
static int32_t calculate_text_width(const char *txt, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// Function to calculate and set dynamic column widths
static void set_dynamic_column_widths(lv_obj_t *t) {
    uint16_t col_count = lv_table_get_col_cnt(t);
    uint16_t row_count = lv_table_get_row_cnt(t);
    
    // Calculate total available width
    lv_coord_t table_width = lv_obj_get_content_width(lv_obj_get_parent(t));
    lv_coord_t total_content_width = 0;
    lv_coord_t *col_widths = lv_malloc(sizeof(lv_coord_t) * col_count);
    const lv_font_t *font = lv_obj_get_style_text_font(t, LV_PART_ITEMS);
    
    if(!font) {
        LV_LOG_WARN("No font set for table");
        lv_free(col_widths);
        return;
    }

    // First pass: calculate minimum required widths
    for(uint16_t col = 0; col < col_count; col++) {
        lv_coord_t max_width = 60; // Set minimum width
        
        // Check all cells in column
        for(uint16_t row = 0; row < row_count; row++) {
            const char *cell = lv_table_get_cell_value(t, row, col);
            if(cell) {
                lv_coord_t width = calculate_text_width(cell, font) + 10; // Add padding
                if(width > max_width) max_width = width;
            }
        }
        
        col_widths[col] = max_width;
        total_content_width += max_width;
    }
    
    // Second pass: distribute extra space proportionally
    if(total_content_width < table_width) {
        lv_coord_t extra_space = table_width - total_content_width;
        lv_coord_t space_per_col = extra_space / col_count;
        
        for(uint16_t col = 0; col < col_count; col++) {
            col_widths[col] += space_per_col;
        }
        
        // Distribute any remaining pixels
        lv_coord_t remaining = extra_space % col_count;
        for(uint16_t col = 0; col < remaining; col++) {
            col_widths[col]++;
        }
    }
    
    // Apply the calculated widths
    for(uint16_t col = 0; col < col_count; col++) {
        lv_table_set_col_width(t, col, col_widths[col]);
    }
    
    lv_free(col_widths);
}

// Function to load data from file to table
void load_table_data(lv_event_t * e) {
    FILE * fp = fopen("/tmp/gsmenu_cache/txprofiles.conf", "r");
    if (!fp) {
        LV_LOG_ERROR("Failed to open file");
        return;
    }

    char line[MAX_LINE_LENGTH];
    uint32_t row = 0;
    
    while (fgets(line, sizeof(line), fp) != NULL && row < 20) { // Limit to 20 rows for safety
        // Skip empty lines and lines starting with #
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#') {
            continue;
        }
        char values[TABLE_COLS-1][MAX_CELL_LENGTH];
        
        // Use sscanf to parse the line correctly, capturing the two range values
        int items_scanned = sscanf(line, "%31s - %31s %31s %31s %31s %31s %31s %31s %31s %31s %31s %31s",
                                   values[0], values[1], values[2], values[3], values[4],
                                   values[5], values[6], values[7], values[8], values[9],
                                   values[10], values[11]);

        lv_table_set_cell_value(table, row, 0, "..."); // ... to indicate a menu

        if (items_scanned >= 11) { // We expect at least 11 values (range min/max + 9 others)
            // Add the parsed data to the table
            lv_table_set_cell_value(table, row, 1, values[0]); // Range Min
            lv_table_set_cell_value(table, row, 2, values[1]); // Range Max
            
            // The rest of the values
            for (int i = 3; i < items_scanned + 1 ; i++) {
                lv_table_set_cell_value(table, row, i, values[i - 1]);
            }
            row++;
        }
    }

    lv_table_set_row_count(table, row);
    fclose(fp);
}

// Function to insert a new row at a specified position
static void insert_table_row(uint32_t row) {
    uint16_t row_cnt = lv_table_get_row_cnt(table);
    
    // Don't exceed maximum rows
    if (row_cnt >= 20) {
        LV_LOG_WARN("Maximum row count reached");
        return;
    }

    // Shift rows down to make space
    for (uint32_t r = row_cnt; r > row; r--) {
        for (uint32_t c = 0; c < TABLE_COLS + 1; c++) {
            const char *value = lv_table_get_cell_value(table, r - 1, c);
            lv_table_set_cell_value(table, r, c, value ? value : "");
        }
    }
    
    // Initialize new row with default values
    lv_table_set_cell_value(table, row + 1, 0, "..."); // Menu indicator
    for (int i = 1 ; i <= TABLE_COLS; i++) {
        lv_table_set_cell_value(table, row + 1, i, lv_table_get_cell_value(table,row,i)); // Default range min
    }
    
    // Update row count
    lv_table_set_row_cnt(table, row_cnt + 1);
}

// Function to delete a row at a specified position
static void delete_table_row(uint32_t row) {
    uint16_t row_cnt = lv_table_get_row_cnt(table);
    
    // Can't delete if only one row remains
    if (row_cnt <= 1) {
        LV_LOG_WARN("Cannot delete the last row");
        return;
    }
    
    // Shift rows up to fill the gap
    for (uint32_t r = row; r < row_cnt - 1; r++) {
        for (uint32_t c = 0; c < TABLE_COLS + 1; c++) {
            const char *value = lv_table_get_cell_value(table, r + 1, c);
            lv_table_set_cell_value(table, r, c, value ? value : "");
        }
    }
    
    // Clear the last row (now duplicate)
    for (uint32_t c = 0; c < TABLE_COLS + 1; c++) {
        lv_table_set_cell_value(table, row_cnt - 1, c, "");
    }
    
    // Update row count
    lv_table_set_row_cnt(table, row_cnt - 1);
}

static void line_edit_dropdown_callback(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    
    switch (code) {
    case LV_EVENT_CANCEL:
        printf("Cancel\n");
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
        lv_obj_del_async(obj);  // Use async delete instead
        break;
        
    case LV_EVENT_VALUE_CHANGED: {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
        char selection[100] = "";
        lv_dropdown_get_selected_str(obj, selection, 99);
        printf("Selection: %s\n", selection);
        
        // Get the current row
        uint32_t row, col;
        lv_table_get_selected_cell(table, &row, &col);
        
        if (strcmp(selection, "Duplicate Row") == 0) {
            printf("Insert above row %d\n", row);
            insert_table_row(row);
        }
        else if (strcmp(selection, "Delete") == 0) {
            printf("Delete row %d\n", row);
            delete_table_row(row);
        }
        
        lv_obj_del_async(obj);  // Use async delete
        lv_group_focus_obj(table);  // Refocus the table
        break;
    }
    default:
        break;
    }
}

// Callback for table edit events
static void table_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_key_t key = lv_event_get_key(e);
    lv_obj_t *target = lv_event_get_target(e);
    uint32_t row, col;
    lv_table_get_selected_cell(table, &row, &col);

    switch (code)
    {
    case LV_EVENT_KEY:
        if (key == LV_KEY_UP && row == 0) {
            lv_group_focus_obj(air_txprofiles_save_button);
            control_mode = GSMENU_CONTROL_MODE_NAV;
        }
        if (key == LV_KEY_DOWN && row == lv_table_get_row_count(table) -1 ) {
            lv_group_focus_next(tx_profile_group);
            control_mode = GSMENU_CONTROL_MODE_NAV;
        }
        break;
    case LV_EVENT_CLICKED:
                if (col == 0 ) { // Line edit mode
                    lv_obj_t * dropdown = lv_dropdown_create(lv_layer_top());

                    lv_point_t * point = lv_table_get_cell_user_data(table,row,col);
                    lv_obj_set_pos(dropdown,point->x,point->y);

                    lv_dropdown_set_options(dropdown,"\nDuplicate Row\nDelete");
                    lv_dropdown_set_text(dropdown,"");
                    lv_obj_set_width(dropdown,0);

                    lv_dropdown_set_dir(dropdown, LV_DIR_RIGHT);
                    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_RIGHT);

                    lv_obj_set_style_outline_width(dropdown,0,LV_PART_MAIN| LV_STATE_FOCUS_KEY);
                    lv_obj_t * list = lv_dropdown_get_list(dropdown);
                    lv_obj_add_style(list, &style_openipc, LV_PART_SELECTED | LV_STATE_CHECKED);
                    lv_obj_add_style(list, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
                    
                    lv_group_add_obj(tx_profile_group,dropdown);
                    lv_group_focus_obj(dropdown);
                    
                    lv_obj_add_event_cb(dropdown,line_edit_dropdown_callback,LV_EVENT_ALL,NULL);
                    
                    lv_dropdown_open(dropdown);
                    control_mode = GSMENU_CONTROL_MODE_EDIT;
                } else if (col == 3) { // GI only allows long or short
                    const char * current = lv_table_get_cell_value(table, row, col);
                    if (strcmp(current, "long") == 0) {
                        lv_table_set_cell_value(table, row, col, "short");
                    } else {
                        lv_table_set_cell_value(table, row, col, "long");
                    }
                } else {
                    lv_textarea_set_text(input_textarea,lv_table_get_cell_value(table,row,col));
                    lv_obj_remove_flag(input_box, LV_OBJ_FLAG_HIDDEN);
                    lv_indev_wait_release(lv_event_get_param(e));
                    lv_group_focus_obj(kb);
                }
    default:
        break;
    }
}


static void txprofiles_back_button_callback(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if(code == LV_EVENT_CLICKED) {
        lv_screen_load(pp_menu_screen);
        menu_page_data_t * menu_page_data = (menu_page_data_t *) lv_obj_get_user_data(sub_air_alink_page);
        lv_indev_set_group(indev_drv,menu_page_data->indev_group);
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
}

static void table_focus_callback(lv_event_t * e) {
    control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
}

static void txprofiles_save_callback() {
    lv_screen_load(pp_menu_screen);
}

// Function to save table data back to file
static void txprofiles_save_button_callback(lv_event_t * e) {
    FILE * fp = fopen("/tmp/gsmenu_cache/txprofiles.conf", "w");
    if (!fp) {
        LV_LOG_ERROR("Failed to open file for writing");
        return;
    }
    
    // Write header comment
    fprintf(fp, "# <ra - nge> <gi> <mcs> <fecK> <fecN> <bitrate> <gop> <Pwr> <roiQP> <bandwidth> <qpDelta>\n");
    
    uint16_t row_cnt = lv_table_get_row_cnt(table);
    
    for(uint16_t row = 0; row < row_cnt; row++) {
        // Get Range Min and Max from the first two columns
        const char * range_min = lv_table_get_cell_value(table, row, 1);
        const char * range_max = lv_table_get_cell_value(table, row, 2);
        
        // Reconstruct the "min - max" format
        if (range_min && range_max) {
             fprintf(fp, "%s - %s ", range_min, range_max);
        }
        
        // Write the rest of the columns
        for(uint16_t col = 3; col < TABLE_COLS+1; col++) {
            const char * cell = lv_table_get_cell_value(table, row, col);
            if(cell && strlen(cell) > 0) {
                fprintf(fp, "%s ", cell);
            }
        }
        fprintf(fp, "\n");
    }
    
    fclose(fp);
    run_command_and_block(e,"gsmenu.sh set air alink txprofiles",txprofiles_save_callback);

}

static void kb_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e);
    uint32_t row, col;
    lv_table_get_selected_cell(table, &row, &col);

    if (code == LV_EVENT_FOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
    else if(code == LV_EVENT_READY) {
        lv_table_set_cell_value(table,row,col,lv_textarea_get_text(input_textarea));
        lv_obj_add_flag(input_box, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(NULL, ta);   /*To forget the last clicked object to make it focusable again*/
        lv_group_focus_obj(table);
    }
    else if(code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(input_box, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(NULL, ta);   /*To forget the last clicked object to make it focusable again*/
        lv_group_focus_obj(table);
    }


}

static void table_scoll_callback(lv_event_t *e) {
    lv_obj_t * header = lv_event_get_user_data(e);
    lv_obj_t * main_table = lv_event_get_target(e);
    
    // Only sync on scroll events
    if(lv_event_get_code(e) == LV_EVENT_SCROLL) {
        // Get current scroll position of main table
        lv_coord_t scroll_x = lv_obj_get_scroll_x(main_table);
        
        // Scroll header to same position
        lv_obj_scroll_to_x(header, scroll_x, LV_ANIM_OFF);
    }
}

void create_table(lv_obj_t * parent) {

    tx_profile_group = lv_group_create();
    lv_group_set_default(tx_profile_group);

    // Make a overall container
    lv_obj_t * table_cont = lv_obj_create(parent);
    lv_obj_set_size(table_cont,lv_obj_get_width(parent) / 4 * 3,
                                lv_obj_get_height(parent)/ 4 * 3);
    lv_obj_center(table_cont);
    lv_obj_set_flex_flow(table_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(table_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_border_side(table_cont, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(table_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(table_cont, &style_openipc_dark_background, LV_PART_MAIN);

    // Button Content
    lv_obj_t * button_cont = lv_obj_create(table_cont);
    lv_obj_set_width(button_cont,LV_SIZE_CONTENT);
    lv_obj_set_height(button_cont,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(button_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(button_cont,0);
    lv_obj_add_style(button_cont, &style_openipc_dark_background, LV_PART_MAIN);
    lv_obj_set_style_border_width(button_cont,0,LV_PART_MAIN);

    // Back Button
    lv_obj_t * air_txprofiles_back_button = lv_button_create(button_cont);
    lv_obj_add_style(air_txprofiles_back_button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(air_txprofiles_back_button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    lv_obj_t * label = lv_label_create(air_txprofiles_back_button);
    lv_label_set_text(label, "Back");

    lv_obj_add_event_cb(air_txprofiles_back_button, txprofiles_back_button_callback,LV_EVENT_CLICKED,NULL);

    // Save Button
    air_txprofiles_save_button = lv_button_create(button_cont);
    lv_obj_add_style(air_txprofiles_save_button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(air_txprofiles_save_button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    label = lv_label_create(air_txprofiles_save_button);
    lv_label_set_text(label, "Save");

    lv_obj_add_event_cb(air_txprofiles_save_button,txprofiles_save_button_callback,LV_EVENT_CLICKED,NULL);

    // Create a table header
    lv_obj_t * header = lv_table_create(table_cont);
    
    // Set size and position
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_align(header, LV_ALIGN_CENTER, 0, 0);
    
    // Set column count
    lv_table_set_col_cnt(header, TABLE_COLS + 1 ); // + 1 for line edit "button"
    
    // Set row count (will be adjusted when loading data)
    lv_table_set_row_cnt(header, 1);
    
    // Set column widths (adjust as needed)
    // Set column headers
    const char * headers[] = {
        "   ","Range Min", "Range Max", "GI", "MCS", "fecK", "fecN", 
        "Bitrate", "GOP", "Pwr", "roiQP", "Bandwidth", "qpDelta"
    };

    for(int i = 0; i < TABLE_COLS + 1 ; i++) {
        lv_table_set_cell_value(header, 0, i, headers[i]);
    }
    lv_obj_add_state(header, LV_STATE_DISABLED);
    lv_obj_add_style(header, &style_openipc, LV_PART_ITEMS| LV_STATE_DEFAULT);

    set_dynamic_column_widths(header);

    // Create a table
    table = lv_table_create(table_cont);
    
    // Set size and position
    lv_obj_set_flex_grow(table, 1);
    lv_obj_set_width(table, LV_PCT(100));
    lv_obj_align(table, LV_ALIGN_CENTER, 0, 0);

    // Set column count
    lv_table_set_col_cnt(table, TABLE_COLS + 1);
    
    // Set row count (will be adjusted when loading data)
    lv_table_set_row_cnt(table, 21); // Max 20 data rows + 1 header
    
    // Set column widths aligned to header
    for(int i = 0; i < TABLE_COLS + 1; i++) {
        lv_table_set_col_width(table, i, lv_table_get_col_width(header,i));
    }
    
    // Make the table interactive
    lv_obj_add_flag(table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(table, LV_OBJ_FLAG_SCROLLABLE);

    // Style the table
    lv_obj_add_style(table, &style_openipc, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(table, &style_openipc_dark_background, LV_PART_ITEMS| LV_STATE_DEFAULT);
    lv_obj_add_style(table, &style_openipc_dark_background, LV_PART_MAIN);
    lv_obj_add_style(table, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_all(table,2,LV_PART_ITEMS);

    // Add event handler
    lv_obj_add_event_cb(table, table_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(table, table_focus_callback, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(table, table_scoll_callback, LV_EVENT_SCROLL, header);

    /*Add an event callback to to apply some custom drawing*/
    lv_obj_add_event_cb(table, draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);

    input_box = lv_msgbox_create(parent);
    lv_obj_set_width(input_box,300);
    lv_obj_set_height(input_box,250);
    lv_obj_add_flag(input_box, LV_OBJ_FLAG_HIDDEN);
    lv_msgbox_add_title(input_box,"Edit");

    input_cont = lv_msgbox_get_content(input_box);
    lv_obj_add_flag(input_cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_style(input_cont, &style_openipc_dark_background, LV_PART_MAIN);

    input_textarea = lv_textarea_create(input_box);
    lv_textarea_set_one_line(input_textarea,true);
    lv_obj_add_state(input_textarea, LV_STATE_DISABLED);
    lv_obj_add_style(input_textarea, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(input_textarea, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(input_textarea, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(input_textarea, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(input_textarea, lv_color_hex(0xff4c60d8), LV_PART_CURSOR | LV_STATE_DEFAULT);

    kb = lv_keyboard_create(input_box);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, input_textarea);

    lv_obj_set_style_outline_width(kb,0,LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_dark_background, LV_PART_ITEMS| LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_textcolor, LV_PART_ITEMS| LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL,kb);

    lv_obj_set_height(input_box,lv_obj_get_height(input_box) + lv_obj_get_height(kb)+ 30 );
    
    // Load data
    lv_obj_add_event_cb(parent,load_table_data,LV_EVENT_SCREEN_LOAD_START,NULL);

    lv_group_set_default(default_group);
}
