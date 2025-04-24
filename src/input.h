// input.h
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include "../../lvgl/lvgl.h"
#include "../../lvgl/src/core/lv_global.h"

typedef enum {
    GSMENU_CONTROL_MODE_NAV = 0,
    GSMENU_CONTROL_MODE_EDIT,
    GSMENU_CONTROL_MODE_SLIDER,
    GSMENU_CONTROL_MODE_KEYBOARD,
    GSMENU_CONTROL_MODE_LVGL_ISSUE_8093
} gsmenu_control_mode_t;


#ifdef __cplusplus
extern "C" {
#endif

void simulate_key_press(uint32_t key_code);

// Function to make stdin non-blocking
void set_stdin_nonblock(void);
// Function to restore terminal settings
void restore_stdin(void);

// Handle WASD input and convert to LVGL key codes
void handle_keyboard_input(void);

void toggle_rec_enabled(void);

// Custom function to simulate keyboard input
static void virtual_keyboard_read(lv_indev_t * indev, lv_indev_data_t * data);

// Function to create the virtual keyboard
lv_indev_t * create_virtual_keyboard();

void cleanup_gpio(void);

#ifdef __cplusplus
}
#endif
#endif // INPUT_H