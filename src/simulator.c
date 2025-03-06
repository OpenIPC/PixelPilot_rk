#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include "lvgl/lvgl.h"
#include "menu.h"
#include "input.h"


int dvr_enabled = 0;

void my_log_cb(lv_log_level_t level, const char * buf)
{
  printf("%s\n",buf);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_sdl_window_create(1920,1080);

    // lv_log_register_print_cb(my_log_cb);
    pp_menu_main();
    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }

}