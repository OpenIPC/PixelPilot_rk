#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <gpiod.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include "main.h"
#include "../../lvgl/lvgl.h"
#include "../../lvgl/src/core/lv_global.h"
#include "input.h"
#include "gsmenu/gs_system.h"

typedef struct Dvr* Dvr; // Forward declaration
void dvr_start_recording(Dvr* dvr);
void dvr_stop_recording(Dvr* dvr);
extern Dvr *dvr;
extern int dvr_enabled;

#ifdef USE_SIMULATOR
bool menu_active;
lv_timer_t * timer = NULL;
#endif
#ifndef USE_SIMULATOR
extern bool menu_active;
#define MAX_GPIO_BUTTONS 10  // Adjust based on your hardware
#define MAX_GPIO_CHIPS 5     // Max number of GPIO chips to scan
#define GPIO_PATH "/dev/"    // Base path for GPIO chips
#define DEBOUNCE_DELAY_MS 50 // Debounce delay in milliseconds
#define INITIAL_REPEAT_DELAY_MS 500  // Time before repeat starts
#define REPEAT_RATE_MS 100           // Time between repeated events

// Add this to the gpio_button_t struct
typedef struct {
    const char *chip_name;
    int line_num;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int last_state;
    long last_time;
    long repeat_time;       // Time when next repeat should occur
    bool is_holding;        // Whether button is being held down
} gpio_button_t;

// Define all GPIO buttons
gpio_button_t gpio_buttons[] = {

    // Ruby
    // {"/dev/gpiochip3", 9,  NULL, NULL, -1, 0},  // Up   PIN_16
    // {"/dev/gpiochip3", 10,  NULL, NULL, -1, 0}, // Down PIN_18
    // {"/dev/gpiochip3", 2,  NULL, NULL, -1, 0},  // Left PIN_13
    // {"/dev/gpiochip3", 1,  NULL, NULL, -1, 0}, // Right PIN_11
    // {"/dev/gpiochip3", 18,  NULL, NULL, -1, 0}, // OK   PIN_32

    // RunCam VRX
    {"/dev/gpiochip3", 9,  NULL, NULL, -1, 0},  // Up   PIN_16
    {"/dev/gpiochip3", 10,  NULL, NULL, -1, 0}, // Down PIN_18
    {"/dev/gpiochip3", 2,  NULL, NULL, -1, 0},  // Left PIN_13
    {"/dev/gpiochip3", 6,  NULL, NULL, -1, 0}, // Right PIN_38
    {"/dev/gpiochip3", 1,  NULL, NULL, -1, 0}, // OK   PIN_11
    {"/dev/gpiochip3", 18,  NULL, NULL, -1, 0}, // OK   PIN_18
};
#endif

extern lv_obj_t * pp_menu_screen;

// Global or static variable to store the next key state
static lv_key_t next_key = LV_KEY_END;  // Default to no key
static bool next_key_pressed = false;    // Indicates if the next key should be pressed or released
gsmenu_control_mode_t control_mode = GSMENU_CONTROL_MODE_NAV;

extern uint64_t gtotal_tunnel_data;
void simulate_traffic(lv_timer_t *t)
{
    gtotal_tunnel_data++;
}

#ifndef USE_SIMULATOR
// Function to initialize GPIO buttons
void setup_gpio(void) {
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        gpio_buttons[i].chip = gpiod_chip_open(gpio_buttons[i].chip_name);
        if (!gpio_buttons[i].chip) {
            perror("Failed to open GPIO chip");
            continue;
        }

        gpio_buttons[i].line = gpiod_chip_get_line(gpio_buttons[i].chip, gpio_buttons[i].line_num);
        if (!gpio_buttons[i].line) {
            perror("Failed to get GPIO line");
            gpiod_chip_close(gpio_buttons[i].chip);
            continue;
        }

        if (gpiod_line_request_input(gpio_buttons[i].line, "lvgl_input") < 0) {
            perror("Failed to request GPIO input");
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
    }
}

void send_button_event(size_t button_index) {
    // Adjust for control_mode
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_LVGL_ISSUE_8093:
            switch (gpio_buttons[button_index].line_num) {
                case 9:  // Up
                    next_key = LV_KEY_PREV;
                    break;
                case 10: // Down
                    next_key = LV_KEY_NEXT;
                    break;
                case 2:  // Left
                    break;
                case 1:  // Right
                    next_key = menu_active ? LV_KEY_ENTER : LV_KEY_RIGHT;
                    break;
                case 18: // OK
                    next_key = LV_KEY_ENTER;
                    break;
            }
            break;
        case GSMENU_CONTROL_MODE_NAV:
            switch (gpio_buttons[button_index].line_num) {
                case 9:  // Up
                    next_key = LV_KEY_PREV;
                    break;
                case 10: // Down
                    next_key = LV_KEY_NEXT;
                    break;
                case 2:  // Left
                    next_key = LV_KEY_HOME;
                    break;
                case 1:  // Right
                    next_key = menu_active ? LV_KEY_ENTER : LV_KEY_RIGHT;
                    break;
                case 18: // OK
                    next_key = LV_KEY_ENTER;
                    break;
            }
            break;
        case GSMENU_CONTROL_MODE_EDIT:
            switch (gpio_buttons[button_index].line_num) {
                case 9:  // Up
                    next_key = LV_KEY_UP;
                    break;
                case 10: // Down
                    next_key = LV_KEY_DOWN;
                    break;
                case 2:  // Left
                    next_key = LV_KEY_ESC;
                    break;
                case 1:  // Right
                    next_key = LV_KEY_ENTER;
                    break;
                case 18: // OK
                    next_key = LV_KEY_ENTER;
                    break;
            }
            break;
        case GSMENU_CONTROL_MODE_SLIDER:
            switch (gpio_buttons[button_index].line_num) {
                case 9:  // Up
                    next_key = LV_KEY_RIGHT;
                    break;
                case 10: // Down
                    next_key = LV_KEY_LEFT;
                    break;
                case 2:  // Left
                    next_key = LV_KEY_ESC;
                    break;
                case 1:  // Right
                    next_key = LV_KEY_ENTER;
                    break;
                case 18: // OK
                    next_key = LV_KEY_ENTER;
                    break;
            }
            break;
        case GSMENU_CONTROL_MODE_KEYBOARD:
            switch (gpio_buttons[button_index].line_num) {
                case 9:  // Up
                    next_key = LV_KEY_UP;
                    break;
                case 10: // Down
                    next_key = LV_KEY_DOWN;
                    break;
                case 2:  // Left
                    next_key = LV_KEY_LEFT;
                    break;
                case 1:  // Right
                    next_key = LV_KEY_RIGHT;
                    break;
                case 18: // OK
                    next_key = LV_KEY_ENTER;
                    break;
            }
            break;
        default:
            break;
    }
    next_key_pressed = true;
    printf("GPIO %s: %d (Chip: %s)\n", 
           gpio_buttons[button_index].is_holding ? "Holding" : "Pressed", 
           gpio_buttons[button_index].line_num, 
           gpio_buttons[button_index].chip_name);
}

void handle_gpio_input(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long current_time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        if (gpio_buttons[i].chip && gpio_buttons[i].line) {
            int current_state = gpiod_line_get_value(gpio_buttons[i].line);
            
            // Check for state change (with debounce)
            if (current_state != gpio_buttons[i].last_state &&
                (current_time - gpio_buttons[i].last_time) > DEBOUNCE_DELAY_MS) {
                
                gpio_buttons[i].last_state = current_state;
                gpio_buttons[i].last_time = current_time;
                
                if (current_state == 1) { // Button pressed
                    gpio_buttons[i].is_holding = true;
                    gpio_buttons[i].repeat_time = current_time + INITIAL_REPEAT_DELAY_MS;
                    send_button_event(i); // Send initial press event
                } else { // Button released
                    gpio_buttons[i].is_holding = false;
                    next_key_pressed = false; // Send release event
                }
            }
            
            // Handle repeat for held buttons
            if (gpio_buttons[i].is_holding && current_state == 1 && 
                current_time >= gpio_buttons[i].repeat_time) {
                send_button_event(i);
                gpio_buttons[i].repeat_time = current_time + REPEAT_RATE_MS;
            }
        }
    }
}


// Cleanup function for GPIO
void cleanup_gpio(void) {
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        if (gpio_buttons[i].chip) {
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
    }
}
#endif

// Function to make stdin non-blocking
void set_stdin_nonblock(void) {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    // Disable canonical mode and echo
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    term.c_cc[VMIN] = 0;
    term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

// Function to restore terminal settings
void restore_stdin(void) {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

void toggle_screen(void) {
    if( ! menu_active ) {
        lv_scr_load(pp_menu_screen);
        lv_obj_invalidate(pp_menu_screen);
        menu_active = true;
    }
}

// Handle WASD input and convert to LVGL key codes
void handle_keyboard_input(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) > 0) {
        switch(c) {
            case 'w':
            case 'W':
                switch (control_mode)
                {
                case GSMENU_CONTROL_MODE_LVGL_ISSUE_8093:
                case GSMENU_CONTROL_MODE_NAV:
                    next_key = LV_KEY_PREV;
                    break;
                case GSMENU_CONTROL_MODE_SLIDER:
                    next_key = LV_KEY_RIGHT;
                    break;
                case GSMENU_CONTROL_MODE_EDIT:
                    next_key = LV_KEY_UP;
                    break;
                case GSMENU_CONTROL_MODE_KEYBOARD:
                    next_key = LV_KEY_UP;
                    break;
                default:
                    break;
                }
                next_key_pressed = true;
                printf("Up\n");
                break;
            case 's':
            case 'S':
                switch (control_mode)
                {
                case GSMENU_CONTROL_MODE_SLIDER:
                    next_key = LV_KEY_LEFT;
                    break;
                case GSMENU_CONTROL_MODE_LVGL_ISSUE_8093:
                case GSMENU_CONTROL_MODE_NAV:
                    next_key = LV_KEY_NEXT;
                    break;
                case GSMENU_CONTROL_MODE_EDIT:
                    next_key = LV_KEY_DOWN;
                    break;
                case GSMENU_CONTROL_MODE_KEYBOARD:
                    next_key = LV_KEY_DOWN;
                    break;
                default:
                    break;
                } 
                next_key_pressed = true;
                printf("Down\n");
                break;
            case 'a':
            case 'A':
                switch (control_mode)
                {
                case GSMENU_CONTROL_MODE_SLIDER:
                case GSMENU_CONTROL_MODE_EDIT:
                    next_key = LV_KEY_ESC;
                    break;
                case GSMENU_CONTROL_MODE_LVGL_ISSUE_8093:
                    break;
                case GSMENU_CONTROL_MODE_NAV:
                    next_key = LV_KEY_HOME;
                    break;
                case GSMENU_CONTROL_MODE_KEYBOARD:
                    next_key = LV_KEY_LEFT;
                    break;
                default:
                    break;
                }
                next_key_pressed = true;
                printf("Left\n");
                break;
            case 'd':
            case 'D':
                switch (control_mode)
                {
                case GSMENU_CONTROL_MODE_LVGL_ISSUE_8093:
                case GSMENU_CONTROL_MODE_NAV:
                    next_key = menu_active ? LV_KEY_ENTER : LV_KEY_RIGHT;
                    break;
                case GSMENU_CONTROL_MODE_SLIDER:
                    next_key = LV_KEY_ENTER;
                    break;
                case GSMENU_CONTROL_MODE_EDIT:
                    next_key = LV_KEY_ENTER;
                    break;
                case GSMENU_CONTROL_MODE_KEYBOARD:
                    next_key = LV_KEY_RIGHT;
                    break;
                default:
                    break;
                }        
                next_key_pressed = true;
                printf("Right\n");
                break;
            case '\n':
                next_key = LV_KEY_ENTER;
                next_key_pressed = true;
                printf("Enter\n");
                break;
#ifdef USE_SIMULATOR
            case 't':
            case 'T':
                if (timer) {
                    lv_timer_delete(timer);
                    timer = NULL;
                }
                else
                    timer = lv_timer_create(simulate_traffic, 50, NULL);
                break;
#endif
            case 'q':
            case 'Q':
                printf("Exiting...\n");
                restore_stdin();
#ifndef USE_SIMULATOR
                sig_handler(2);
#else
                exit(0);
#endif
                break;
        }
    }
}

// Custom function to simulate keyboard input
static void virtual_keyboard_read(lv_indev_t * indev, lv_indev_data_t * data) {
    static bool key_sent = false;  // Track if a key event was sent

#ifndef USE_SIMULATOR
    handle_gpio_input(); // Check GPIO state separately from keyboard input
#endif

    if (next_key != LV_KEY_END) {
        data->key = next_key;
        data->state = next_key_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

        next_key_pressed = !next_key_pressed;  // Toggle state

        if ( next_key != LV_KEY_ENTER && ! menu_active )
            toggle_screen();
        if ( next_key == LV_KEY_ENTER && ! menu_active ) {
            data->key = LV_KEY_END;
#ifdef USE_SIMULATOR
                dvr_enabled ^= 1;
#endif
            toggle_rec_enabled();
        }

        if (!next_key_pressed) {  
            next_key = LV_KEY_END;  // Reset key after release event
        }

        key_sent = true;  // Mark that a key was sent
    } else if (key_sent) {
        data->state = LV_INDEV_STATE_REL;  // Ensure release event is sent
        key_sent = false;  // Reset the flag
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// Function to create the virtual keyboard
lv_indev_t * create_virtual_keyboard() {

    set_stdin_nonblock(); // setup keyboard input from stdin
#ifndef USE_SIMULATOR 
    setup_gpio();          // Initialize GPIO
#endif
    lv_indev_t * indev_drv = lv_indev_create();
    lv_indev_set_type(indev_drv, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_drv, virtual_keyboard_read);

    lv_indev_enable(indev_drv, true);

    return indev_drv;
}
