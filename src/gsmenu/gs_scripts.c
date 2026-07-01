#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "../input.h"
#include "helper.h"
#include "gs_scripts.h"
#include "styles.h"

#define SCRIPT_DIR "/media/dvr/scripts"
#define MAX_SCRIPT_COUNT 128
#define MAX_OUTPUT_KEEP 32768
#define OUTPUT_CHUNK 512

extern lv_indev_t * indev_drv;
extern lv_group_t * default_group;

typedef struct {
    pid_t pid;
    int fd;
    bool running;
    lv_timer_t *timer;
    lv_obj_t *msgbox;
    lv_obj_t *output_ta;
    lv_obj_t *close_btn;
    lv_obj_t *stop_btn;
    char *output;
    size_t output_len;
    lv_group_t *group;
} script_runner_t;

static lv_obj_t *g_parent_page;
static lv_obj_t *g_scripts_section;
static menu_page_data_t *g_menu_page_data;
static char *g_script_names[MAX_SCRIPT_COUNT];
static int g_script_count;
static lv_obj_t *g_confirm_msgbox;
static lv_group_t *g_confirm_group;
static char g_pending_script[256];

static void build_script_list(void);
static script_runner_t g_runner = {
    .pid = -1,
    .fd = -1,
    .running = false,
    .timer = NULL,
    .msgbox = NULL,
    .output_ta = NULL,
    .close_btn = NULL,
    .stop_btn = NULL,
    .output = NULL,
    .output_len = 0,
    .group = NULL,
};

static int cmp_string_ptr(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool ends_with_sh(const char *name)
{
    size_t len = strlen(name);
    return len > 3 && strcmp(name + len - 3, ".sh") == 0;
}

static int collect_scripts(char **scripts, int max_scripts)
{
    DIR *dir = opendir(SCRIPT_DIR);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_scripts) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!ends_with_sh(entry->d_name)) {
            continue;
        }

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", SCRIPT_DIR, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        scripts[count] = strdup(entry->d_name);
        if (scripts[count]) {
            count++;
        }
    }

    closedir(dir);
    qsort(scripts, count, sizeof(char *), cmp_string_ptr);
    return count;
}

static void clear_script_names(void)
{
    for (int i = 0; i < g_script_count; i++) {
        free(g_script_names[i]);
        g_script_names[i] = NULL;
    }
    g_script_count = 0;
}

static void runner_append(const char *text)
{
    if (!text) {
        return;
    }

    size_t add = strlen(text);
    if (add == 0) {
        return;
    }

    size_t new_len = g_runner.output_len + add;
    if (new_len > MAX_OUTPUT_KEEP) {
        size_t drop = new_len - MAX_OUTPUT_KEEP;
        if (drop > g_runner.output_len) {
            drop = g_runner.output_len;
        }
        if (drop > 0 && g_runner.output) {
            memmove(g_runner.output, g_runner.output + drop, g_runner.output_len - drop);
            g_runner.output_len -= drop;
            g_runner.output[g_runner.output_len] = '\0';
        }
    }

    size_t cap = g_runner.output_len + add + 1;
    char *new_buf = realloc(g_runner.output, cap);
    if (!new_buf) {
        return;
    }
    g_runner.output = new_buf;
    memcpy(g_runner.output + g_runner.output_len, text, add);
    g_runner.output_len += add;
    g_runner.output[g_runner.output_len] = '\0';

    if (g_runner.output_ta && lv_obj_is_valid(g_runner.output_ta)) {
        lv_textarea_set_text(g_runner.output_ta, g_runner.output);
        lv_textarea_set_cursor_pos(g_runner.output_ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void runner_finish_message(int status)
{
    char line[96];
    if (WIFEXITED(status)) {
        snprintf(line, sizeof(line), "\n\nProcess exited with code %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(line, sizeof(line), "\n\nProcess terminated by signal %d\n", WTERMSIG(status));
    } else {
        snprintf(line, sizeof(line), "\n\nProcess ended\n");
    }
    runner_append(line);
}

static void script_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (g_runner.fd >= 0) {
        char chunk[OUTPUT_CHUNK + 1];
        ssize_t r;
        while ((r = read(g_runner.fd, chunk, OUTPUT_CHUNK)) > 0) {
            chunk[r] = '\0';
            runner_append(chunk);
        }
    }

    if (g_runner.running && g_runner.pid > 0) {
        int status = 0;
        pid_t ret = waitpid(g_runner.pid, &status, WNOHANG);
        if (ret == g_runner.pid) {
            g_runner.running = false;
            runner_finish_message(status);
            if (g_runner.fd >= 0) {
                close(g_runner.fd);
                g_runner.fd = -1;
            }
            if (g_runner.stop_btn && lv_obj_is_valid(g_runner.stop_btn)) {
                lv_obj_add_state(g_runner.stop_btn, LV_STATE_DISABLED);
            }
        }
    }
}

static void close_runner_ui(void)
{
    if (g_runner.timer) {
        lv_timer_del(g_runner.timer);
        g_runner.timer = NULL;
    }

    if (g_runner.fd >= 0) {
        close(g_runner.fd);
        g_runner.fd = -1;
    }

    if (g_runner.running && g_runner.pid > 0) {
        kill(g_runner.pid, SIGTERM);
        waitpid(g_runner.pid, NULL, 0);
    }

    g_runner.running = false;
    g_runner.pid = -1;

    if (g_runner.msgbox && lv_obj_is_valid(g_runner.msgbox)) {
        lv_msgbox_close(g_runner.msgbox);
    }
    g_runner.msgbox = NULL;
    g_runner.output_ta = NULL;
    g_runner.close_btn = NULL;
    g_runner.stop_btn = NULL;

    free(g_runner.output);
    g_runner.output = NULL;
    g_runner.output_len = 0;

    if (g_runner.group) {
        lv_group_del(g_runner.group);
        g_runner.group = NULL;
    }

    /* Restore focus to the scripts page. */
    if (g_menu_page_data && g_menu_page_data->indev_group) {
        lv_indev_set_group(indev_drv, g_menu_page_data->indev_group);
    }
}

static void script_close_cb(lv_event_t *e)
{
    (void)e;
    close_runner_ui();
}

static void script_stop_cb(lv_event_t *e)
{
    (void)e;
    if (g_runner.running && g_runner.pid > 0) {
        kill(g_runner.pid, SIGTERM);
        runner_append("\nStopping script...\n");
    }
}

static void start_script_runner(const char *script_name)
{
    if (!script_name || strchr(script_name, '/')) {
        return;
    }

    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", SCRIPT_DIR, script_name);

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
        mode_t new_mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
        if (new_mode != st.st_mode) {
            if (chmod(fullpath, new_mode) != 0) {
                perror("chmod");
            }
        }
    }

    close_runner_ui();

    /* Use a pseudoterminal so the child sees a tty on stdout/stderr.
     * This causes shells and most programs to flush output line by line,
     * giving continuous log output instead of large buffered chunks.
     * Use the Linux-native /dev/ptmx approach to avoid needing posix_openpt()
     * which may not be available on musl-based cross-compilers. */
    int master_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        return;
    }
    /* Unlock the slave pty (equivalent to unlockpt()). */
    {
        int unlock = 0;
        ioctl(master_fd, TIOCSPTLCK, &unlock);
    }
    /* Get the slave pty device number (equivalent to ptsname()). */
    unsigned int ptnum = 0;
    if (ioctl(master_fd, TIOCGPTN, &ptnum) != 0) {
        close(master_fd);
        return;
    }
    char slave_path[64];
    snprintf(slave_path, sizeof(slave_path), "/dev/pts/%u", ptnum);

    /* Disable echo and LF->CRLF translation so output reads as plain text. */
    {
        int slave_fd_temp = open(slave_path, O_RDWR | O_NOCTTY);
        if (slave_fd_temp >= 0) {
            struct termios tios;
            if (tcgetattr(slave_fd_temp, &tios) == 0) {
                tios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
                tios.c_oflag &= ~ONLCR;
                tcsetattr(slave_fd_temp, TCSANOW, &tios);
            }
            close(slave_fd_temp);
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        return;
    }

    if (pid == 0) {
        /* Become a new session leader so the pty becomes the controlling
         * terminal, which forces line-buffered output from the child. */
        setsid();
        int slave_fd = open(slave_path, O_RDWR);
        if (slave_fd < 0) {
            _exit(127);
        }
        ioctl(slave_fd, TIOCSCTTY, 0);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);
        close(master_fd);

        if (chdir(SCRIPT_DIR) != 0) {
            perror("chdir");
            _exit(127);
        }

        execl("/bin/sh", "sh", script_name, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    g_runner.pid = pid;
    g_runner.fd = master_fd;
    g_runner.running = true;

    g_runner.msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(g_runner.msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_title(g_runner.msgbox, script_name);

    lv_obj_t *content = lv_msgbox_get_content(g_runner.msgbox);
    lv_obj_add_style(content, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(content, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_runner.output_ta = lv_textarea_create(content);
    /* The textarea must be a child of the msgbox content container, otherwise
     * LVGL lays it out beside the built-in header/content/footer regions and
     * most of the modal ends up unused. */
    lv_obj_set_size(g_runner.output_ta, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(g_runner.output_ta, 1);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_textcolor, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_textarea_set_one_line(g_runner.output_ta, false);
    lv_textarea_set_password_mode(g_runner.output_ta, false);
    lv_textarea_set_cursor_click_pos(g_runner.output_ta, false);
    lv_obj_set_scrollbar_mode(g_runner.output_ta, LV_SCROLLBAR_MODE_ACTIVE);

    g_runner.stop_btn = lv_msgbox_add_footer_button(g_runner.msgbox, "Stop");
    g_runner.close_btn = lv_msgbox_add_footer_button(g_runner.msgbox, "Close");
    lv_obj_add_event_cb(g_runner.stop_btn, script_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_runner.close_btn, script_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_style(g_runner.stop_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.stop_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(g_runner.close_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.close_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    theme_msgbox(g_runner.msgbox);

    /* Fixed size: triggers msgbox_size_changed_event_cb which sets flex_grow=1
     * on the content area, making the textarea fill the available height. */
    lv_obj_set_size(g_runner.msgbox, lv_pct(98), lv_pct(92));

    /* Create a dedicated input group for this dialog so key navigation
     * reaches Stop/Close and not the script list behind the overlay. */
    g_runner.group = lv_group_create();
    lv_group_add_obj(g_runner.group, g_runner.stop_btn);
    lv_group_add_obj(g_runner.group, g_runner.close_btn);
    lv_indev_set_group(indev_drv, g_runner.group);

    char header[512];
    snprintf(header, sizeof(header), "Running %s/%s\n\n", SCRIPT_DIR, script_name);
    runner_append(header);

    g_runner.timer = lv_timer_create(script_poll_timer_cb, 10, NULL);
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (g_confirm_msgbox && lv_obj_is_valid(g_confirm_msgbox)) {
        lv_msgbox_close(g_confirm_msgbox);
    }
    g_confirm_msgbox = NULL;
    if (g_confirm_group) {
        lv_group_del(g_confirm_group);
        g_confirm_group = NULL;
    }
    /* Restore focus to the scripts page. */
    if (g_menu_page_data && g_menu_page_data->indev_group) {
        lv_indev_set_group(indev_drv, g_menu_page_data->indev_group);
    }
}

static void confirm_execute_cb(lv_event_t *e)
{
    (void)e;
    if (g_confirm_msgbox && lv_obj_is_valid(g_confirm_msgbox)) {
        lv_msgbox_close(g_confirm_msgbox);
    }
    g_confirm_msgbox = NULL;
    if (g_confirm_group) {
        lv_group_del(g_confirm_group);
        g_confirm_group = NULL;
    }
    start_script_runner(g_pending_script);
}

static void run_script_cb(lv_event_t *e)
{
    const char *script_name = (const char *)lv_event_get_user_data(e);
    if (!script_name || strchr(script_name, '/')) {
        return;
    }

    if (g_confirm_msgbox && lv_obj_is_valid(g_confirm_msgbox)) {
        lv_msgbox_close(g_confirm_msgbox);
        g_confirm_msgbox = NULL;
    }

    snprintf(g_pending_script, sizeof(g_pending_script), "%s", script_name);

    g_confirm_msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(g_confirm_msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_title(g_confirm_msgbox, "Confirm Execution");

    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "Are you sure you want to execute\n%s?\n\n"
             "Check the source of this script first. "
             "It can modify and brick your system.",
             g_pending_script);
    lv_msgbox_add_text(g_confirm_msgbox, prompt);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(g_confirm_msgbox, "Cancel");
    lv_obj_t *exec_btn = lv_msgbox_add_footer_button(g_confirm_msgbox, "Execute");

    lv_obj_add_style(cancel_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(cancel_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(exec_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(exec_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    lv_obj_add_event_cb(cancel_btn, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(exec_btn, confirm_execute_cb, LV_EVENT_CLICKED, NULL);

    theme_msgbox(g_confirm_msgbox);

    lv_obj_set_width(g_confirm_msgbox, lv_pct(75));

    /* Create a dedicated input group so key navigation reaches Cancel/Execute
     * and not the script list behind the overlay. */
    g_confirm_group = lv_group_create();
    lv_group_add_obj(g_confirm_group, cancel_btn);
    lv_group_add_obj(g_confirm_group, exec_btn);
    lv_indev_set_group(indev_drv, g_confirm_group);
}

static void build_script_list(void)
{
    if (!g_parent_page || !g_menu_page_data) {
        return;
    }

    if (g_scripts_section && lv_obj_is_valid(g_scripts_section)) {
        lv_obj_del(g_scripts_section);
        g_scripts_section = NULL;
    }

    /* Set the page group as default so every new focusable widget (the inner
     * lv_btn created by create_button) auto-registers into the correct group.
     * This also ensures generic_back_event_handler receives key events so
     * HOME / back navigation works. */
    lv_group_set_default(g_menu_page_data->indev_group);

    g_scripts_section = lv_menu_section_create(g_parent_page);
    lv_obj_add_style(g_scripts_section, &style_openipc_section, 0);

    g_script_count = collect_scripts(g_script_names, MAX_SCRIPT_COUNT);

    if (g_script_count == 0) {
        create_text(g_scripts_section, LV_SYMBOL_WARNING, "No scripts found", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
        create_text(g_scripts_section, NULL, "Place .sh files in " SCRIPT_DIR, NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    }

    for (int i = 0; i < g_script_count; i++) {
        /* Build a display label with a play icon; user_data keeps the raw name
         * so the execute path is not affected. */
        char display[300];
        snprintf(display, sizeof(display), LV_SYMBOL_PLAY "  %s", g_script_names[i]);
        lv_obj_t *row = create_button(g_scripts_section, display);
        lv_obj_t *btn = lv_obj_get_child_by_type(row, 0, &lv_button_class);
        lv_obj_add_event_cb(btn, run_script_cb, LV_EVENT_CLICKED, g_script_names[i]);
    }

    /* Restore the global default group. */
    lv_group_set_default(default_group);
}

void gs_scripts_page_refresh(lv_obj_t *page)
{
    (void)page;
    clear_script_names();
    if (g_scripts_section && lv_obj_is_valid(g_scripts_section)) {
        lv_obj_del(g_scripts_section);
        g_scripts_section = NULL;
    }
    build_script_list();
}

void gs_scripts_init_in_page(lv_obj_t *parent, menu_page_data_t *menu_page_data)
{
    g_parent_page = parent;
    g_menu_page_data = menu_page_data;

    create_text(parent, LV_SYMBOL_DIRECTORY, "Custom Scripts", NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    create_text(parent, NULL, "Scripts are loaded from " SCRIPT_DIR, NULL, NULL, false, LV_MENU_ITEM_BUILDER_VARIANT_1);

    build_script_list();
}
