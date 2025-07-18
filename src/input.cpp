#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <gpiod.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <yaml-cpp/yaml.h>
#include <glob.h>
#include "main.h"
#include "../../lvgl/lvgl.h"
#include "../../lvgl/src/core/lv_global.h"
#include "input.h"
#include "gsmenu/gs_system.h"

extern YAML::Node config;
extern lv_group_t *main_group;
extern lv_indev_t * indev_drv;


struct Dvr;
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
#define MAX_GPIO_BUTTONS 6  // Adjust based on your hardware
#define DEBOUNCE_DELAY_MS 50 // Debounce delay in milliseconds
#define INITIAL_REPEAT_DELAY_MS 500  // Time before repeat starts
#define REPEAT_RATE_MS 100           // Time between repeated events

// gpio_button_t structure
typedef struct {
    const char *name;        // Button name ("up", "down", etc.)
    int pin_number;          // Physical pin number from YAML
    const char *chip_name;   // GPIO chip path (e.g., "/dev/gpiochip0")
    const char *chip_label;  // GPIO chip label (e.g., "gpiochip3")
    int line_num;            // GPIO line number
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int last_state;
    long last_time;
    long repeat_time;
    bool is_holding;
} gpio_button_t;

// Global array of GPIO buttons
gpio_button_t gpio_buttons[MAX_GPIO_BUTTONS] = {0};
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
// Function to find GPIO chip and line for a given pin number
bool find_gpio_mapping(int pin, const char** chip_name, int* line_num) {
    glob_t globbuf;
    struct gpiod_chip *chip = NULL;
    bool found = false;

    if (glob("/dev/gpiochip*", 0, NULL, &globbuf) != 0) {
        perror("Failed to find GPIO chips");
        return false;
    }

    for (size_t i = 0; i < globbuf.gl_pathc && !found; i++) {
        chip = gpiod_chip_open(globbuf.gl_pathv[i]);
        if (!chip) continue;

        // Check chip label first
        const char *label = gpiod_chip_label(chip);
        if (label) {
            // If we were looking for a specific chip label, we'd check here
        }

        // For libgpiod v1.x
        int num_lines = gpiod_chip_num_lines(chip);
        for (int offset = 0; offset < num_lines && !found; offset++) {
            struct gpiod_line *line = gpiod_chip_get_line(chip, offset);
            if (!line) continue;

            const char *name = gpiod_line_name(line);
            if (name) {
                int extracted_pin = 0;
                if (sscanf(name, "PIN_%d", &extracted_pin) == 1 || 
                    sscanf(name, "GPIO%d", &extracted_pin) == 1 ||
                    sscanf(name, "%d", &extracted_pin) == 1) {
                    if (extracted_pin == pin) {
                        *chip_name = strdup(globbuf.gl_pathv[i]);
                        *line_num = offset;
                        found = true;
                    }
                }
            }
            gpiod_line_release(line);
        }
        gpiod_chip_close(chip);
    }
    globfree(&globbuf);
    return found;
}

void init_button_from_config(YAML::Node& gpio_config, const char* button_name, int& button_index) {
    if (!gpio_config[button_name] || gpio_config[button_name].IsNull()) {
        printf("Omitting GPIO mapping for button %s\n", button_name);
        return;
    }

    gpio_buttons[button_index].name = button_name;

    // Check if the button config is a simple pin number or a map
    if (gpio_config[button_name].IsScalar()) {
        // Simple format: just a pin number
        gpio_buttons[button_index].pin_number = gpio_config[button_name].as<int>();
        if (!find_gpio_mapping(gpio_buttons[button_index].pin_number, 
                             &gpio_buttons[button_index].chip_name,
                             &gpio_buttons[button_index].line_num)) {
            fprintf(stderr, "Failed to find GPIO mapping for pin %d (%s)\n", 
                    gpio_buttons[button_index].pin_number, button_name);
            return;
        }
    } else {
        // Complex format: chip and pin specified
        YAML::Node button_config = gpio_config[button_name];
        if (!button_config["chip"] || !button_config["pin"]) {
            fprintf(stderr, "Invalid GPIO config for button %s - missing chip or pin\n", button_name);
            return;
        }

        gpio_buttons[button_index].chip_label = strdup(button_config["chip"].as<std::string>().c_str());
        gpio_buttons[button_index].pin_number = button_config["pin"].as<int>();
        
        // Construct chip path from label
        std::string chip_path = "/dev/" + std::string(gpio_buttons[button_index].chip_label);
        gpio_buttons[button_index].chip_name = strdup(chip_path.c_str());
        
        // For direct chip/pin mapping, we can use the pin number directly as line_num
        gpio_buttons[button_index].line_num = gpio_buttons[button_index].pin_number;
    }

    button_index++;
}

void init_gpio_buttons_from_config(YAML::Node& config) {
    YAML::Node gpio_config = config["gsmenu"]["gpio"];
    int button_index = 0;
    
    init_button_from_config(gpio_config, "up", button_index);
    init_button_from_config(gpio_config, "down", button_index);
    init_button_from_config(gpio_config, "left", button_index);
    init_button_from_config(gpio_config, "right", button_index);
    init_button_from_config(gpio_config, "center", button_index);
    init_button_from_config(gpio_config, "rec", button_index);
}

// Function to initialize GPIO buttons
void setup_gpio(YAML::Node& config) {
    // Initialize buttons from config first
    init_gpio_buttons_from_config(config);
    
    // Then setup the GPIO lines
    for (size_t i = 0; i < sizeof(gpio_buttons) / sizeof(gpio_buttons[0]); i++) {
        if (gpio_buttons[i].chip_name == NULL) continue;
        
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

        // Create the consumer name with "pixelpilot_" prefix
        char consumer_name[32];
        snprintf(consumer_name, sizeof(consumer_name), "pixelpilot_%s", gpio_buttons[i].name);

        if (gpiod_line_request_input(gpio_buttons[i].line, consumer_name) < 0) {
            perror("Failed to request GPIO input");
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
    }
}

void send_button_event(size_t button_index) {
    if (gpio_buttons[button_index].name == NULL) return;

    // Adjust for control_mode
    switch (control_mode) {
        case GSMENU_CONTROL_MODE_NAV:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_PREV;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_NEXT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_HOME;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0) {
                next_key = LV_KEY_ENTER;
            }
            else if (strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            else if (strcmp(gpio_buttons[button_index].name, "rec") == 0) {
                #ifdef USE_SIMULATOR
                                dvr_enabled ^= 1;
                #endif
                            toggle_rec_enabled();
            }
            break;
            
        case GSMENU_CONTROL_MODE_EDIT:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_UP;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_DOWN;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_ESC;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0 ||
                     strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        case GSMENU_CONTROL_MODE_SLIDER:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_RIGHT;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_LEFT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_ESC;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0 ||
                     strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        case GSMENU_CONTROL_MODE_KEYBOARD:
            if (strcmp(gpio_buttons[button_index].name, "up") == 0) {
                next_key = LV_KEY_UP;
            } 
            else if (strcmp(gpio_buttons[button_index].name, "down") == 0) {
                next_key = LV_KEY_DOWN;
            }
            else if (strcmp(gpio_buttons[button_index].name, "left") == 0) {
                next_key = LV_KEY_LEFT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "right") == 0) {
                next_key = LV_KEY_RIGHT;
            }
            else if (strcmp(gpio_buttons[button_index].name, "center") == 0) {
                next_key = LV_KEY_ENTER;
            }
            break;
            
        default:
            break;
    }
    
    if (next_key != LV_KEY_END) {
        next_key_pressed = true;
        printf("GPIO %s: %s (Pin: %d, Chip: %s)\n", 
               gpio_buttons[button_index].is_holding ? "Holding" : "Pressed", 
               gpio_buttons[button_index].name,
               gpio_buttons[button_index].pin_number,
               gpio_buttons[button_index].chip_name);
    }
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
    for (int i = 0; i < MAX_GPIO_BUTTONS; i++) {
        if (gpio_buttons[i].chip) {
            gpiod_chip_close(gpio_buttons[i].chip);
            gpio_buttons[i].chip = NULL;
            gpio_buttons[i].line = NULL;
        }
        if (gpio_buttons[i].chip_name) {
            free((void*)gpio_buttons[i].chip_name);
            gpio_buttons[i].chip_name = NULL;
        }
        if (gpio_buttons[i].chip_label) {
            free((void*)gpio_buttons[i].chip_label);
            gpio_buttons[i].chip_label = NULL;
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
        lv_indev_set_group(indev_drv,main_group);
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
                exit(0);
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

        if (next_key != LV_KEY_ENTER)
            toggle_screen();

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
    setup_gpio(config);          // Initialize GPIO
#endif
    lv_indev_t * indev_drv = lv_indev_create();
    lv_indev_set_type(indev_drv, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_drv, virtual_keyboard_read);

    lv_indev_enable(indev_drv, true);

    return indev_drv;
}
