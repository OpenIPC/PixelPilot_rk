#include "colmenu.h"
#include "column_stack.h"
#include "styles.h"        /* dark theme styles for the keyboard */
#include "../input.h"      /* control_mode for text entry */
#include "../menu.h"       /* menu_is_recording() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

extern gsmenu_control_mode_t control_mode;
extern lv_indev_t * indev_drv;

extern int  audio_get_enabled(void);
extern void audio_set_enabled(int enabled);

static colstack_t * g_cs;

/* True while the command-error dialog is up, so async teardowns (e.g. the text
 * overlay close) don't steal the keypad back from its Close button. */
static bool g_err_active;

/* First genuine `gsmenu.sh get` failure (non-zero exit that is NOT the "Unknown"
 * = unimplemented sentinel), captured for a deferred error dialog. Gets run on the
 * page-load worker thread, so the failure is recorded here and shown later on the
 * main thread (page_load_poll, or an async flush for a main-thread get). */
static char          g_gerr_cmd[256];
static char          g_gerr_out[512];
static char          g_gerr_err[512];
static int           g_gerr_rc;
static volatile bool g_gerr_pending;
static volatile bool g_prefetching;      /* true while the load worker is fetching */
static char *        read_all(const char * path);
static void          flush_get_error(void * unused);

static void report_get_error(const char * cmd, int rc, const char * out, const char * err)
{
    if(__atomic_load_n(&g_gerr_pending, __ATOMIC_ACQUIRE)) return;   /* keep the first */
    snprintf(g_gerr_cmd, sizeof(g_gerr_cmd), "%s", cmd ? cmd : "");
    snprintf(g_gerr_out, sizeof(g_gerr_out), "%s", out ? out : "");
    snprintf(g_gerr_err, sizeof(g_gerr_err), "%s", err ? err : "");
    g_gerr_rc = rc;
    __atomic_store_n(&g_gerr_pending, true, __ATOMIC_RELEASE);
    /* Worker-thread gets are flushed by page_load_poll; a main-thread get defers
     * its own flush (lv_async_call is only safe to call from the LVGL thread). */
    if(!g_prefetching) lv_async_call(flush_get_error, NULL);
}

/* True from when a sub-page enter is requested until its rows are built. The slow
 * gsmenu.sh reads happen on a worker thread meanwhile (see the page loader below),
 * so stack-mutating callbacks (drone detection) must defer their work. */
static volatile bool g_loading;
static volatile bool g_pending_close;   /* drone lost mid-load: close after build */
static void close_drone_subtree(void);

/* Values pre-fetched by the loader worker, consulted by colmenu_get during the
 * (main-thread) build so no gsmenu.sh reads happen while creating widgets. */
typedef struct { char * param; char * value; char * opts; } prefetch_entry_t;
typedef struct { prefetch_entry_t * items; int count; } prefetch_cache_t;
static prefetch_cache_t * g_prefetch;

struct colmenu_emit { colstack_t * cs; lv_obj_t * body; };

/* ── gsmenu.sh backend ─────────────────────────────────────────────────────── */

static void strip_nl(char * s) { size_t n = s ? strlen(s) : 0; while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]='\0'; }

/* Read `gsmenu.sh get <type> <page> <param>`; returns malloc'd current value
 * (NULL if the script doesn't implement it), and *opts = malloc'd option/range
 * blob after the 0x1e separator. */
char * colmenu_get(const char * type, const char * page, const char * param, char ** opts)
{
    if(opts) *opts = NULL;
    if(!type || !param) return NULL;

    /* During a page build a loader worker has already fetched the slow reads; serve
     * them from the cache so we never shell out from the main (render) thread —
     * for static value items and for the gets a dynamic build() makes (the WiFi
     * scan). The worker itself runs with g_prefetch == NULL, so it fetches for real. */
    if(g_prefetch) {
        for(int i = 0; i < g_prefetch->count; i++) {
            if(g_prefetch->items[i].param && strcmp(g_prefetch->items[i].param, param) == 0) {
                if(opts) *opts = g_prefetch->items[i].opts ? strdup(g_prefetch->items[i].opts) : NULL;
                return g_prefetch->items[i].value ? strdup(g_prefetch->items[i].value) : NULL;
            }
        }
    }

    /* rec_enabled is runtime state (are we recording RIGHT NOW), not config.
     * Read it directly from the app instead of the shell script. Return the
     * switch's 0/1 convention (the builder does atoi(v)) — NOT "on"/"off",
     * which atoi()s to 0 and would leave the switch stuck off while recording. */
    if(strcmp(param, "rec_enabled") == 0) {
        return strdup(menu_is_recording() ? "1" : "0");
    }

    /* Audio on/off is live runtime state on the receiver, not a config value —
     * read it straight from the app (switch builder atoi()s this: 0/1, not on/off). */
    if(strcmp(param, "audio") == 0) {
        return strdup(audio_get_enabled() ? "1" : "0");
    }

    char errf[] = "/tmp/gsmenu_gerr_XXXXXX";
    int efd = mkstemp(errf);
    if(efd >= 0) close(efd);
    char cmd[300];
    printf("Running command: gsmenu.sh get %s %s %s\n", type, page, param);
    fflush(stdout);
    snprintf(cmd, sizeof(cmd), "gsmenu.sh get %s %s %s 2>%s", type, page, param, errf);
    FILE * fp = popen(cmd, "r");
    if(!fp) { unlink(errf); return NULL; }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    int status = pclose(fp);
    int rc = (status == -1) ? -1 : WEXITSTATUS(status);
    bool unknown = (strncmp(buf, "Unknown", 7) == 0);
    /* Non-zero exit that isn't the "Unknown"/unimplemented sentinel is a real
     * failure — surface its command, stdout and stderr. */
    if(rc != 0 && !unknown) {
        char * eout = read_all(errf);
        char disp[300];
        snprintf(disp, sizeof(disp), "gsmenu.sh get %s %s %s", type, page, param);
        report_get_error(disp, rc, buf, eout);
        free(eout);
    }
    unlink(errf);
    if(rc != 0 || n == 0) return NULL;   /* Unknown / error / empty → no value */
    char * rs = strchr(buf, '\x1e');
    char * val;
    if(rs) { *rs = '\0'; if(opts) { *opts = strdup(rs + 1); strip_nl(*opts); } val = strdup(buf); }
    else   { val = strdup(buf); }
    strip_nl(val);
    return val;
}

/* ── command execution + error reporting ───────────────────────────────────── */

static char * read_all(const char * path)
{
    FILE * f = fopen(path, "r");
    if(!f) return NULL;
    char * buf = malloc(4096);
    size_t n = fread(buf, 1, 4095, f);
    buf[n] = '\0';
    fclose(f);
    strip_nl(buf);
    return buf;
}

typedef struct { lv_obj_t * backdrop; lv_group_t * grp; lv_group_t * restore; } err_dlg_t;

/* Deferred teardown — the close arrives from the button's own event, so deleting
 * it inline would be a use-after-free. */
static void err_close_async(void * p)
{
    err_dlg_t * d = p;
    lv_indev_set_group(indev_drv, d->restore);
    lv_obj_delete(d->backdrop);
    lv_group_delete(d->grp);
    free(d);
}

static void err_close_cb(lv_event_t * e)
{
    /* Activate on the ENTER key press, not CLICKED (which fires on release) — the
     * release of the ENTER that triggered the failing action would else close it. */
    if(lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) != LV_KEY_ENTER) return;
    if(!g_err_active) return;             /* already closing */
    g_err_active = false;
    lv_async_call(err_close_async, lv_event_get_user_data(e));
}

/* Modal error dialog showing a failed command's exit code, stdout and stderr. */
static void show_cmd_error(const char * cmd, int rc, const char * out, const char * err)
{
    err_dlg_t * d = calloc(1, sizeof(*d));
    d->restore = colstack_cur_group(g_cs);

    lv_obj_t * bd = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bd, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_50, 0);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    d->backdrop = bd;

    lv_obj_t * panel = lv_obj_create(bd);
    lv_obj_add_style(panel, &style_openipc_lightdark_background, LV_PART_MAIN);
    lv_obj_set_width(panel, 1000);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LV_PCT(80), 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(panel);

    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, "Command failed");
    lv_obj_set_style_text_color(title, lv_color_hex(0xff6b6b), 0);
    lv_obj_set_style_pad_bottom(title, 12, 0);

    lv_obj_t * body = lv_label_create(panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_pad_bottom(body, 16, 0);
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "$ %s\nexit status: %d\n\nstdout:\n%s\n\nstderr:\n%s",
             cmd, rc, (out && *out) ? out : "(none)", (err && *err) ? err : "(none)");
    lv_label_set_text(body, buf);

    lv_obj_t * btn = lv_button_create(panel);
    lv_obj_add_style(btn, &style_openipc, LV_PART_MAIN);
    lv_obj_add_style(btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, "Close");

    d->grp = lv_group_create();
    lv_group_add_obj(d->grp, btn);
    lv_indev_set_group(indev_drv, d->grp);
    lv_group_focus_obj(btn);
    lv_obj_add_event_cb(btn, err_close_cb, LV_EVENT_KEY, d);
    g_err_active = true;
}

/* Async command runner: the (possibly slow) command runs on a worker thread while
 * a spinner blocks the menu, matching the old menu's write behaviour. On finish a
 * failure shows the error dialog; `done` (optional) runs the caller's follow-up. */
typedef struct {
    char *       cmd;
    char         outf[32];
    char         errf[32];
    lv_obj_t *   spinner;
    lv_group_t * loader_group;
    lv_timer_t * poll;
    void (*done)(void * ctx, int rc);
    void *       done_ctx;
    volatile int  rc;
    volatile bool finished;
} exec_ctx_t;

static void * exec_thread(void * arg)
{
    exec_ctx_t * x = arg;
    char full[700];
    snprintf(full, sizeof(full), "%s >%s 2>%s", x->cmd, x->outf, x->errf);
    int status = system(full);
    x->rc = (status == -1) ? -1 : WEXITSTATUS(status);
    __atomic_store_n(&x->finished, true, __ATOMIC_RELEASE);
    return NULL;
}

static void exec_poll(lv_timer_t * t)
{
    exec_ctx_t * x = lv_timer_get_user_data(t);
    if(!__atomic_load_n(&x->finished, __ATOMIC_ACQUIRE)) return;

    lv_obj_delete(x->spinner);
    int rc = x->rc;

    /* Hand the keypad back to the column and drop the loader group BEFORE the
     * follow-up: a follow-up may open its own dialog that captures the current
     * group to return to (e.g. the restart notice), and that must be a real
     * group, not our about-to-be-deleted temporary one. */
    lv_indev_set_group(indev_drv, colstack_cur_group(g_cs));
    lv_group_delete(x->loader_group);

    /* caller's follow-up (reload / rebuild / restart notice / close overlay) —
     * only runs its effect on success; it may claim the keypad for its own UI. */
    if(x->done) x->done(x->done_ctx, rc);

    if(rc != 0) {
        char * out = read_all(x->outf);
        char * err = read_all(x->errf);
        show_cmd_error(x->cmd, rc, out, err);   /* grabs the keypad */
        free(out);
        free(err);
    }

    unlink(x->outf);
    unlink(x->errf);
    lv_timer_delete(x->poll);
    free(x->cmd);
    free(x);
}

static void colmenu_exec_cb(const char * cmd, void (*done)(void * ctx, int rc), void * ctx)
{
    printf("Running command: %s\n", cmd);
    fflush(stdout);

    exec_ctx_t * x = calloc(1, sizeof(*x));
    x->cmd = strdup(cmd);
    x->done = done;
    x->done_ctx = ctx;
    strcpy(x->outf, "/tmp/gsmenu_out_XXXXXX");
    strcpy(x->errf, "/tmp/gsmenu_err_XXXXXX");
    int ofd = mkstemp(x->outf);
    int efd = mkstemp(x->errf);
    if(ofd >= 0) close(ofd);
    if(efd >= 0) close(efd);

    x->spinner = openipc_spinner_create(lv_layer_top());

    /* freeze the menu on an empty group until the command finishes */
    x->loader_group = lv_group_create();
    lv_indev_set_group(indev_drv, x->loader_group);

    x->poll = lv_timer_create(exec_poll, 20, x);

    pthread_t th;
    if(pthread_create(&th, NULL, exec_thread, x) != 0) {
        /* thread spawn failed — run it synchronously */
        lv_timer_delete(x->poll);
        lv_obj_delete(x->spinner);
        lv_indev_set_group(indev_drv, colstack_cur_group(g_cs));
        lv_group_delete(x->loader_group);
        char full[700];
        snprintf(full, sizeof(full), "%s >%s 2>%s", x->cmd, x->outf, x->errf);
        int status = system(full);
        int rc = (status == -1) ? -1 : WEXITSTATUS(status);
        if(x->done) x->done(x->done_ctx, rc);
        if(rc != 0) {
            char * out = read_all(x->outf); char * err = read_all(x->errf);
            show_cmd_error(x->cmd, rc, out, err); free(out); free(err);
        }
        unlink(x->outf); unlink(x->errf); free(x->cmd); free(x);
        return;
    }
    pthread_detach(th);
}

void colmenu_exec(const char * cmd) { colmenu_exec_cb(cmd, NULL, NULL); }

/* Show a pending `gsmenu.sh get` failure (recorded by report_get_error). Runs on
 * the main thread — from page_load_poll after a load, or via lv_async_call. */
static void flush_get_error(void * unused)
{
    (void)unused;
    if(!__atomic_load_n(&g_gerr_pending, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_gerr_pending, false, __ATOMIC_RELEASE);
    if(g_err_active) return;   /* another error dialog is already up */
    show_cmd_error(g_gerr_cmd, g_gerr_rc, g_gerr_out, g_gerr_err);
}

/* ── change binding ────────────────────────────────────────────────────────── */

typedef struct {
    const char * type;
    const char * page;
    const char * param;
    int          display_scale;    /* multiply slider display value by this (e.g., 100) */
    void (*on_change)(const char * value);
    void (*on_live)(const char * value);
} bind_ctx_t;

/* on_change runs after the async set completes (only on success). */
typedef struct { void (*on_change)(const char * value); char * value; } setdone_t;

static void do_set_done(void * ctx, int rc)
{
    setdone_t * s = ctx;
    if(rc == 0 && s->on_change) s->on_change(s->value);
    free(s->value);
    free(s);
}

static void do_set(void * ctx, const char * value)
{
    bind_ctx_t * b = ctx;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "gsmenu.sh set %s %s %s \"%s\"",
             b->type, b->page, b->param, value ? value : "");
    setdone_t * s = malloc(sizeof(*s));
    s->on_change = b->on_change;
    s->value = strdup(value ? value : "");
    colmenu_exec_cb(cmd, do_set_done, s);
}

static void free_bind_cb(lv_event_t * e) { free(lv_event_get_user_data(e)); }

/* Slider live-preview: apply the effect (on_live) as the slider moves, without
 * writing gsmenu.sh — the persist happens once on commit via do_set. */
static void do_live(void * ctx, const char * value)
{
    bind_ctx_t * b = ctx;
    if(b->on_live) b->on_live(value);
}

static void bind_row(lv_obj_t * row, const colmenu_page_t * pg, const colmenu_item_t * it)
{
    bind_ctx_t * b = malloc(sizeof(bind_ctx_t));
    b->type = pg->type; b->page = pg->page; b->param = it->param;
    b->display_scale = it->display_scale;
    b->on_change = it->on_change; b->on_live = it->on_live;
    colstack_set_binding(row, do_set, b);
    colstack_set_row_param(row, it->param);   /* for external reflect (see colmenu_reflect_switch) */
    if(it->on_live) colstack_set_slider_live(row, do_live, b);
    lv_obj_add_event_cb(row, free_bind_cb, LV_EVENT_DELETE, b);
}

static int option_index(const char * options, const char * value)
{
    if(!options || !value) return 0;
    char * tmp = strdup(options); char * save = NULL; int i = 0;
    for(char * t = strtok_r(tmp, "\n", &save); t; t = strtok_r(NULL, "\n", &save), i++)
        if(strcmp(t, value) == 0) { free(tmp); return i; }
    free(tmp);
    return 0;
}

/* ── page builder ──────────────────────────────────────────────────────────── */

static void build_page(colstack_t * cs, lv_obj_t * body, const colmenu_page_t * pg);

/* A page is worth a loading spinner if entering it hits the backend: dynamic
 * pages (may scan/list) or any item that reads a value via gsmenu.sh. Pages made
 * only of labels/submenus/actions build instantly, so skip the spinner+thread. */
static bool page_needs_load(const colmenu_page_t * pg)
{
    if(pg->build) return true;
    for(int i = 0; i < pg->count; i++) {
        switch(pg->items[i].kind) {
        case COLMENU_VALUE: case COLMENU_SWITCH: case COLMENU_SLIDER:
        case COLMENU_DROPDOWN: case COLMENU_TEXT:
            return true;
        default:
            break;
        }
    }
    return false;
}

static bool item_reads_value(colmenu_kind_t kind)
{
    switch(kind) {
    case COLMENU_VALUE: case COLMENU_SWITCH: case COLMENU_SLIDER:
    case COLMENU_DROPDOWN: case COLMENU_TEXT:
        return true;
    default:
        return false;
    }
}

typedef struct {
    colstack_t * cs;
    lv_obj_t * body;
    const colmenu_page_t * pg;
    lv_obj_t * panel;            /* loading overlay (label + progress bar)       */
    lv_obj_t * bar;
    lv_obj_t * label;
    int          total;          /* number of value items to fetch               */
    lv_group_t * loader_group;   /* empty group: freezes nav while loading       */
    lv_group_t * target_group;   /* the new column's group to focus when done    */
    lv_timer_t * poll;           /* main-thread timer that updates + finishes     */
    prefetch_cache_t * cache;    /* filled by the worker, read by the build      */
    int          prog_done;      /* items fetched so far (worker → poll)         */
    const char * prog_label;     /* label of the item being fetched (static str) */
    volatile bool cancel;        /* set by the Cancel button (poll → abort)      */
    volatile bool done;          /* set by the worker when the cache is ready    */
} page_load_t;

static void free_cache(prefetch_cache_t * c)
{
    if(!c) return;
    for(int i = 0; i < c->count; i++) {
        free(c->items[i].param); free(c->items[i].value); free(c->items[i].opts);
    }
    free(c->items); free(c);
}

/* Worker thread: touches NO LVGL — only runs the (slow) gsmenu.sh reads for the
 * page's value items into a cache, reporting progress as it goes, then flags
 * itself done. The main-thread poll timer draws the progress bar and, once the
 * cache is ready, builds the rows — keeping all UI work on one thread. */
static void * page_load_thread(void * arg)
{
    page_load_t * pl = arg;
    const colmenu_page_t * pg = pl->pg;

    prefetch_cache_t * c = calloc(1, sizeof(*c));
    c->items = calloc(pg->count > 0 ? pg->count : 1, sizeof(prefetch_entry_t));
    g_prefetching = true;   /* colmenu_get records get failures instead of showing */
    for(int i = 0; i < pg->count; i++) {
        const colmenu_item_t * it = &pg->items[i];
        if(!item_reads_value(it->kind) || !it->param) continue;
        if(__atomic_load_n(&pl->cancel, __ATOMIC_ACQUIRE)) break;   /* user cancelled */
        /* publish what's loading before the (slow) read so the poll can show it */
        __atomic_store_n(&pl->prog_label, it->label ? it->label : it->param, __ATOMIC_RELEASE);
        char * opts = NULL;
        char * v = colmenu_get(pg->type, pg->page, it->param, &opts);
        c->items[c->count].param = strdup(it->param);
        c->items[c->count].value = v;
        c->items[c->count].opts  = opts;
        c->count++;
        __atomic_store_n(&pl->prog_done, c->count, __ATOMIC_RELEASE);
    }

    g_prefetching = false;
    pl->cache = c;
    /* Release store pairs with the acquire load in page_load_poll so the cache
     * write is guaranteed visible before `done` reads true (matters on ARM). */
    __atomic_store_n(&pl->done, true, __ATOMIC_RELEASE);
    return NULL;
}

/* Runs on the main thread every tick: advances the progress bar, and once the
 * worker has finished fetching, builds the page from the cache. */
static void page_load_poll(lv_timer_t * t)
{
    page_load_t * pl = lv_timer_get_user_data(t);

    /* reflect fetch progress */
    int done_n = __atomic_load_n(&pl->prog_done, __ATOMIC_ACQUIRE);
    lv_bar_set_value(pl->bar, done_n, LV_ANIM_OFF);
    const char * lab = __atomic_load_n(&pl->prog_label, __ATOMIC_ACQUIRE);
    if(lab) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Loading %s ...", lab);
        lv_label_set_text(pl->label, buf);
    }

    if(!__atomic_load_n(&pl->done, __ATOMIC_ACQUIRE)) return;

    lv_obj_delete(pl->panel);
    g_loading = false;

    if(__atomic_load_n(&pl->cancel, __ATOMIC_ACQUIRE)) {
        /* user cancelled: discard the fetched values and back out of the
         * half-built column, returning focus to the row that opened it. */
        free_cache(pl->cache);
        colstack_pop(pl->cs);
        if(g_pending_close) { g_pending_close = false; close_drone_subtree(); }
    } else {
        /* Attach the keypad to the column BEFORE building so the first row added
         * gets LV_STATE_FOCUS_KEY (the visible focus), matching a normal (sync)
         * page open. Building while input is elsewhere leaves it merely FOCUSED. */
        lv_indev_set_group(indev_drv, pl->target_group);
        g_prefetch = pl->cache;
        build_page(pl->cs, pl->body, pl->pg);   /* value items hit the cache */
        g_prefetch = NULL;
        free_cache(pl->cache);

        if(g_pending_close) {
            /* drone was lost mid-load: close the (now built) sub-page. This pops
             * the column and its target_group, and re-homes the focus itself. */
            g_pending_close = false;
            close_drone_subtree();
        } else {
            /* Focusing the first row while the column was still being populated
             * queues a SCROLL_ON_FOCUS animation toward a stale target, which then
             * drifts the (now full) column off-screen. Cancel it and pin to top. */
            lv_anim_delete(pl->body, NULL);
            lv_obj_scroll_to_y(pl->body, 0, LV_ANIM_OFF);
            /* if any value's gsmenu.sh get failed during the load, report it now */
            flush_get_error(NULL);
        }
    }
    lv_group_delete(pl->loader_group);
    lv_timer_delete(pl->poll);
    free(pl);
}

/* Cancel button: flag the load; page_load_poll aborts once the worker unwinds.
 * Fires on the ENTER key press (not CLICKED, which fires on release) so the very
 * release of the ENTER that opened this page can't instantly cancel it. */
static void loader_cancel_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) != LV_KEY_ENTER) return;
    page_load_t * pl = lv_event_get_user_data(e);
    __atomic_store_n(&pl->cancel, true, __ATOMIC_RELEASE);
    lv_label_set_text(pl->label, "Cancelling ...");
}

/* ── Dynamic-page prefetch (spinner) ───────────────────────────────────────────
 * A dynamic page that declares a `prefetch` list (e.g. the WiFi Networks scan)
 * gets its slow gsmenu.sh reads warmed on a worker thread behind a spinner, then
 * build() runs on the main thread reading those values from the cache. Used for
 * both opening the page and re-scanning it (both go through submenu_builder). */
typedef struct {
    colstack_t *           cs;
    lv_obj_t *             body;
    const colmenu_page_t * pg;
    lv_obj_t *             spinner;
    lv_group_t *           loader_group;   /* empty group: freezes nav while loading */
    lv_group_t *           target_group;   /* column's group to focus once built     */
    lv_timer_t *           poll;
    prefetch_cache_t *     cache;          /* filled by the worker, read by the build */
    volatile bool          done;
} dyn_load_t;

/* Worker: warm the page's prefetch params into a cache (touches no LVGL). */
static void * dyn_load_thread(void * arg)
{
    dyn_load_t * dl = arg;
    const colmenu_page_t * pg = dl->pg;
    int n = 0;
    for(const char * const * p = pg->prefetch; *p; p++) n++;

    prefetch_cache_t * c = calloc(1, sizeof(*c));
    c->items = calloc(n > 0 ? n : 1, sizeof(prefetch_entry_t));
    g_prefetching = true;   /* colmenu_get records get failures instead of showing */
    for(const char * const * p = pg->prefetch; *p; p++) {
        char * opts = NULL;
        char * v = colmenu_get(pg->type, pg->page, *p, &opts);
        c->items[c->count].param = strdup(*p);
        c->items[c->count].value = v;
        c->items[c->count].opts  = opts;
        c->count++;
    }
    g_prefetching = false;
    dl->cache = c;
    __atomic_store_n(&dl->done, true, __ATOMIC_RELEASE);
    return NULL;
}

/* Main-thread poll: once the worker is done, build the page from the cache. */
static void dyn_load_poll(lv_timer_t * t)
{
    dyn_load_t * dl = lv_timer_get_user_data(t);
    if(!__atomic_load_n(&dl->done, __ATOMIC_ACQUIRE)) return;

    lv_obj_delete(dl->spinner);
    g_loading = false;

    /* Attach the keypad to the column BEFORE building so the first row added gets
     * the visible focus (matches page_load_poll). */
    lv_indev_set_group(indev_drv, dl->target_group);
    lv_group_delete(dl->loader_group);

    g_prefetch = dl->cache;
    build_page(dl->cs, dl->body, dl->pg);   /* build()'s colmenu_get hits the cache */
    g_prefetch = NULL;
    free_cache(dl->cache);

    /* pin the freshly-built column to the top (see page_load_poll) */
    lv_anim_delete(dl->body, NULL);
    lv_obj_scroll_to_y(dl->body, 0, LV_ANIM_OFF);
    flush_get_error(NULL);   /* report any read that failed during the load */

    lv_timer_delete(dl->poll);
    free(dl);
}

static void dyn_load_start(colstack_t * cs, lv_obj_t * body, const colmenu_page_t * pg)
{
    dyn_load_t * dl = calloc(1, sizeof(*dl));
    dl->cs = cs; dl->body = body; dl->pg = pg;
    dl->target_group = colstack_cur_group(cs);

    dl->spinner = openipc_spinner_create(lv_layer_top());

    /* freeze the menu on an empty group until the scan finishes */
    dl->loader_group = lv_group_create();
    lv_indev_set_group(indev_drv, dl->loader_group);

    g_loading = true;
    dl->poll = lv_timer_create(dyn_load_poll, 20, dl);

    pthread_t th;
    if(pthread_create(&th, NULL, dyn_load_thread, dl) != 0) {
        /* thread spawn failed — fall back to a synchronous build */
        lv_timer_delete(dl->poll);
        lv_obj_delete(dl->spinner);
        lv_indev_set_group(indev_drv, dl->target_group);
        lv_group_delete(dl->loader_group);
        g_loading = false;
        build_page(cs, body, pg);
        free(dl);
        return;
    }
    pthread_detach(th);
}

static void submenu_builder(colstack_t * cs, lv_obj_t * body, void * user)
{
    const colmenu_page_t * pg = (const colmenu_page_t *)user;

    /* Dynamic pages build their own rows (interleaving reads + widgets) so they
     * can't be prefetched item-by-item; a page that declares a `prefetch` list
     * (e.g. the WiFi scan) warms those reads behind a spinner first. Trivial pages
     * have nothing slow to fetch. Only static pages with value items get the async
     * progress bar. */
    if(pg->build) {
        if(pg->prefetch) dyn_load_start(cs, body, pg);
        else             build_page(cs, body, pg);
        return;
    }
    if(!page_needs_load(pg)) { build_page(cs, body, pg); return; }

    int total = 0;
    for(int i = 0; i < pg->count; i++)
        if(item_reads_value(pg->items[i].kind) && pg->items[i].param) total++;
    if(total == 0) { build_page(cs, body, pg); return; }

    page_load_t * pl = calloc(1, sizeof(*pl));
    pl->cs = cs; pl->body = body; pl->pg = pg; pl->total = total;
    pl->target_group = colstack_cur_group(cs);   /* the just-pushed column's group */

    /* loading overlay: a caption + progress bar, centred over everything */
    pl->panel = lv_obj_create(lv_layer_top());
    lv_obj_add_style(pl->panel, &style_openipc_lightdark_background, LV_PART_MAIN);
    lv_obj_set_size(pl->panel, 460, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(pl->panel, 24, 0);
    lv_obj_set_flex_flow(pl->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pl->panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pl->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(pl->panel);

    pl->label = lv_label_create(pl->panel);
    lv_label_set_text(pl->label, "Loading ...");
    lv_obj_set_style_text_color(pl->label, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_pad_bottom(pl->label, 18, 0);

    pl->bar = lv_bar_create(pl->panel);
    lv_obj_set_size(pl->bar, 400, 14);
    lv_obj_add_style(pl->bar, &style_openipc_dropdown, LV_PART_MAIN);
    lv_obj_add_style(pl->bar, &style_openipc, LV_PART_INDICATOR);
    lv_bar_set_range(pl->bar, 0, total);
    lv_bar_set_value(pl->bar, 0, LV_ANIM_OFF);

    /* input goes to the loader group holding just the Cancel button */
    pl->loader_group = lv_group_create();
    lv_indev_set_group(indev_drv, pl->loader_group);

    lv_obj_t * cancel = lv_button_create(pl->panel);
    lv_obj_add_style(cancel, &style_openipc, LV_PART_MAIN);
    lv_obj_add_style(cancel, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_margin_top(cancel, 18, 0);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_group_add_obj(pl->loader_group, cancel);
    lv_group_focus_obj(cancel);
    lv_obj_add_event_cb(cancel, loader_cancel_cb, LV_EVENT_KEY, pl);

    g_loading = true;
    pl->poll = lv_timer_create(page_load_poll, 20, pl);

    pthread_t th;
    if(pthread_create(&th, NULL, page_load_thread, pl) != 0) {
        /* thread spawn failed — fall back to a synchronous build */
        lv_timer_delete(pl->poll);
        lv_obj_delete(pl->panel);
        lv_indev_set_group(indev_drv, pl->target_group);
        lv_group_delete(pl->loader_group);
        g_loading = false;
        build_page(cs, body, pg);
        free(pl);
        return;
    }
    pthread_detach(th);
}

static void static_action_cb(void * ctx)
{
    const colmenu_item_t * it = ctx;
    if(it->on_activate) it->on_activate();
}

static void add_value_row(lv_obj_t * body, const char * icon, const char * label, const char * value)
{
    lv_obj_t * row = lv_obj_create(body);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    /* a card, like the focusable rows but dimmer, so read-only info is still
     * clearly legible against the column instead of faint floating text */
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161b24), 0);
    lv_obj_set_style_bg_opa(row, 140, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_margin_bottom(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if(icon) {
        lv_obj_t * ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_color(ic, lv_color_hex(0xf2f4f8), 0);
        lv_obj_set_style_pad_right(ic, 8, 0);
    }
    lv_obj_t * l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t * v = lv_label_create(row);
    lv_label_set_text(v, value ? value : "-");
    lv_obj_set_style_text_color(v, lv_color_hex(0xc7cfdd), 0);   /* brighter value */
}

/* ── text entry (floating textarea + on-screen keyboard overlay) ───────────── */

typedef struct {
    const char * type;
    const char * page;
    const char * param;
    void (*on_change)(const char * value);
    lv_obj_t *   value_lbl;   /* row's value label to refresh after save     */
    lv_obj_t *   ta;          /* live only while the overlay is open          */
    lv_obj_t *   overlay;     /* dim backdrop + panel, floated on the top layer */
    lv_group_t * group;       /* the keyboard's own input group               */
    lv_group_t * prev_group;  /* group to restore on close                    */
    /* prompt mode (colmenu_prompt): a standalone text entry with a custom OK
     * handler instead of the gsmenu.sh set — used e.g. for a wifi password. */
    const char * title;                              /* overlay caption (else param) */
    void (*on_ok)(const char * text, void * user);   /* custom OK; NULL = generic set */
    void *       user;
    bool         owned;                              /* free this ctx on close       */
} text_ctx_t;

/* Deferred teardown — ok/cancel arrive from the keyboard's own event, so
 * deleting it there would be a use-after-free. */
static void text_close_cb(void * p)
{
    text_ctx_t * t = p;
    /* If a command-error dialog popped from the save, leave the keypad with its
     * Close button; err_close_cb restores it to the same page group afterwards. */
    if(!g_err_active) lv_indev_set_group(indev_drv, t->prev_group);
    if(t->group)   { lv_group_delete(t->group); t->group = NULL; }
    if(t->overlay) { lv_obj_delete(t->overlay); t->overlay = NULL; }
    control_mode = GSMENU_CONTROL_MODE_NAV;
    if(t->owned) free(t);   /* standalone prompt ctx (row-owned ctx frees itself) */
}

/* After the async set: on success update the row's value + fire on_change; either
 * way close the keyboard overlay (text_close_cb defers to the error dialog). */
static void text_set_done(void * ctx, int rc)
{
    text_ctx_t * t = ctx;
    if(rc == 0) {
        const char * val = lv_textarea_get_text(t->ta);
        if(t->value_lbl) lv_label_set_text(t->value_lbl, val);
        if(t->on_change) t->on_change(val);
    }
    lv_async_call(text_close_cb, t);
}

static void text_ok_cb(lv_event_t * e)
{
    text_ctx_t * t = lv_event_get_user_data(e);
    if(t->on_ok) {                       /* prompt mode: hand the text to the caller */
        t->on_ok(lv_textarea_get_text(t->ta), t->user);
        lv_async_call(text_close_cb, t);
        return;
    }
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "gsmenu.sh set %s %s %s \"%s\"",
             t->type, t->page, t->param, lv_textarea_get_text(t->ta));
    colmenu_exec_cb(cmd, text_set_done, t);
}
static void text_cancel_cb(lv_event_t * e)
{
    lv_async_call(text_close_cb, lv_event_get_user_data(e));
}

static void open_text(void * ctx)
{
    text_ctx_t * t = ctx;

    /* dim backdrop over the whole menu */
    lv_obj_t * overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    t->overlay = overlay;

    /* centred floating panel: title + textarea + full-size keyboard */
    lv_obj_t * panel = lv_obj_create(overlay);
    lv_obj_set_width(panel, 780);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0b0e14), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x4c60d8), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, t->title ? t->title : t->param);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8a93a6), 0);

    lv_obj_t * ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_add_style(ta, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* prompt mode starts empty; a bound field preloads its current value */
    char * cur = t->on_ok ? NULL : colmenu_get(t->type, t->page, t->param, NULL);
    lv_textarea_set_text(ta, cur ? cur : "");
    free(cur);
    t->ta = ta;

    lv_obj_t * kb = lv_keyboard_create(panel);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, 320);
    /* match the dark menu theme instead of the bright default keyboard */
    lv_obj_add_style(kb, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_dark_background,      LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_add_style(kb, &style_openipc_dark_background,      LV_PART_ITEMS | LV_STATE_CHECKED);   /* control keys */
    lv_obj_add_style(kb, &style_openipc,                      LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(kb, &style_openipc_outline,              LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, text_ok_cb, LV_EVENT_READY, t);
    lv_obj_add_event_cb(kb, text_cancel_cb, LV_EVENT_CANCEL, t);

    /* own group so the keypad drives the floating keyboard (WASD→arrows,
     * ENTER→press key); restore the column's group on close. */
    t->prev_group = colstack_cur_group(g_cs);
    t->group = lv_group_create();
    lv_group_add_obj(t->group, kb);
    lv_indev_set_group(indev_drv, t->group);
    lv_group_focus_obj(kb);
    control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
}

/* Standalone on-screen-keyboard prompt: on OK, `on_ok` is called with the entered
 * text (nothing is written to gsmenu.sh). Used e.g. for a wifi network password. */
void colmenu_prompt(const char * title, void (*on_ok)(const char * text, void * user), void * user)
{
    text_ctx_t * t = calloc(1, sizeof(text_ctx_t));
    t->title = title;
    t->on_ok = on_ok;
    t->user  = user;
    t->owned = true;
    open_text(t);
}

static void build_page(colstack_t * cs, lv_obj_t * body, const colmenu_page_t * pg)
{
    if(pg->build) {                       /* dynamic page: emit rows at runtime */
        colmenu_emit_t e = { cs, body };
        pg->build(&e);
        colstack_autofit(cs);
        colstack_ensure_escapable(cs);    /* empty list (only captions) → keep Back working */
        return;
    }
    for(int i = 0; i < pg->count; i++) {
        const colmenu_item_t * it = &pg->items[i];

        /* Skip items that don't match the current RX mode. */
        if(it->mode_mask) {
            int mode_bit = (RXMODE == APFPV) ? COLMENU_MODE_APFPV : COLMENU_MODE_WFB;
            if(!(it->mode_mask & mode_bit)) continue;
        }

        switch(it->kind) {
        case COLMENU_LABEL:
            colstack_add_title(cs, body, it->label);
            break;

        case COLMENU_VALUE: {
            char * v = colmenu_get(pg->type, pg->page, it->param, NULL);
            add_value_row(body, it->icon, it->label, v);
            free(v);
            break;
        }

        case COLMENU_SUBMENU:
            colstack_add_submenu(cs, body, it->icon, it->label, submenu_builder, (void *)it->sub);
            break;

        case COLMENU_SWITCH: {
            char * v = colmenu_get(pg->type, pg->page, it->param, NULL);
            lv_obj_t * row = colstack_add_switch(cs, body, it->icon, it->label, v && atoi(v));
            free(v);
            bind_row(row, pg, it);
            break;
        }
        case COLMENU_SLIDER: {
            char * opts = NULL;
            char * v = colmenu_get(pg->type, pg->page, it->param, &opts);
            double dv = v ? atof(v) : 0, dmin = 0, dmax = 100;
            if(opts) sscanf(opts, "%lf %lf", &dmin, &dmax);
            double f = pow(10, it->precision);
            lv_obj_t * row = colstack_add_slider(cs, body, it->icon, it->label,
                                                 (int32_t)(dmin * f), (int32_t)(dmax * f),
                                                 (int32_t)(dv * f), it->precision, it->display_scale);
            free(v); free(opts);
            bind_row(row, pg, it);
            break;
        }
        case COLMENU_DROPDOWN: {
            char * opts = NULL;
            char * v = colmenu_get(pg->type, pg->page, it->param, &opts);
            int sel = option_index(opts, v);
            lv_obj_t * row = colstack_add_dropdown(cs, body, it->icon, it->label,
                                                   opts ? opts : "", sel);
            free(v); free(opts);
            bind_row(row, pg, it);
            break;
        }
        case COLMENU_TEXT: {
            char * v = colmenu_get(pg->type, pg->page, it->param, NULL);
            lv_obj_t * row = colstack_add_action(cs, body, it->icon, it->label);
            lv_obj_t * val = lv_label_create(row);
            lv_label_set_text(val, v ? v : "");
            lv_obj_set_style_text_color(val, lv_color_hex(0x4c60d8), 0);
            free(v);
            text_ctx_t * t = calloc(1, sizeof(text_ctx_t));
            t->type = pg->type; t->page = pg->page; t->param = it->param;
            t->on_change = it->on_change; t->value_lbl = val;
            colstack_set_action(row, open_text, t);
            lv_obj_add_event_cb(row, free_bind_cb, LV_EVENT_DELETE, t);
            break;
        }
        case COLMENU_ACTION: {
            lv_obj_t * row = colstack_add_action(cs, body, it->icon, it->label);
            colstack_set_action(row, static_action_cb, (void *)it);
            break;
        }
        }
    }
    colstack_autofit(cs);
    colstack_ensure_escapable(cs);        /* an all-caption static page mustn't strand Back */
}

/* ── public ────────────────────────────────────────────────────────────────── */

static const colmenu_page_t * g_root;

/* Drone (air) submenu rows are "gated" — greyed/disabled until the VTX is
 * detected. Tracked so the detection timer can toggle them without a rebuild. */
#define COLMENU_MAX_GATED 16
static lv_obj_t * g_gated[COLMENU_MAX_GATED];
static int        g_gated_n;
static bool       g_drone_detected;

static bool row_is_gated(lv_obj_t * r)
{
    for(int i = 0; i < g_gated_n; i++) if(g_gated[i] == r) return true;
    return false;
}

/* Drone lost while inside a drone submenu: close those columns and jump focus to
 * the first (non-gated) GS row so the user isn't left on stale/disabled pages. */
static void close_drone_subtree(void)
{
    if(colstack_depth(g_cs) <= 1) return;
    if(!row_is_gated(colstack_column_origin(g_cs, 1))) return;   /* in a GS subtree */

    while(colstack_depth(g_cs) > 1) colstack_pop(g_cs);

    lv_obj_t * body = colstack_root(g_cs);
    uint32_t n = lv_obj_get_child_count(body);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t * ch = lv_obj_get_child(body, i);
        if(lv_obj_check_type(ch, &lv_label_class)) continue;   /* section caption */
        if(!row_is_gated(ch)) { lv_group_focus_obj(ch); break; }
    }
}

void colmenu_set_drone_detected(bool detected)
{
    bool was = g_drone_detected;
    g_drone_detected = detected;
    for(int i = 0; i < g_gated_n; i++) colstack_set_enabled(g_gated[i], detected);
    if(was && !detected) {
        /* Defer the column pop if a page is mid-load — the loader would build into
         * the very column we'd destroy. page_load_poll runs the close on finish. */
        if(g_loading) g_pending_close = true;
        else          close_drone_subtree();
    }
}
bool colmenu_drone_detected(void) { return g_drone_detected; }

void colmenu_init(lv_obj_t * parent) { g_cs = colstack_create(parent); }

void colmenu_show(const colmenu_page_t * root)
{
    g_root = root;
    build_page(g_cs, colstack_root(g_cs), root);
}

/* Reset to the root column and rebuild it — used when the menu structure itself
 * changes (e.g. receiver mode switched). Deferred so it's safe to call from a
 * row's event/binding. */
static void rebuild_async(void * p)
{
    (void)p;
    while(colstack_depth(g_cs) > 1) colstack_pop(g_cs);
    g_gated_n = 0;                       /* stale rows about to be deleted */
    lv_obj_clean(colstack_root(g_cs));
    if(g_root) build_page(g_cs, colstack_root(g_cs), g_root);
}
void colmenu_rebuild(void) { lv_async_call(rebuild_async, NULL); }

/* ── emit API (used by dynamic build() functions) ──────────────────────────── */

typedef struct { void (*on)(void *); void * ctx; } emit_action_t;

static void emit_action_invoke(void * p)
{
    emit_action_t * a = p;
    if(a->on) a->on(a->ctx);
}
static void emit_action_delete(lv_event_t * e)
{
    emit_action_t * a = lv_event_get_user_data(e);
    free(a->ctx);
    free(a);
}

void colmenu_emit_label(colmenu_emit_t * e, const char * text)
{
    colstack_add_title(e->cs, e->body, text);
}

void colmenu_emit_value(colmenu_emit_t * e, const char * icon, const char * label, const char * value)
{
    add_value_row(e->body, icon, label, value);
}

/* bind_ctx for dynamically-emitted rows owns its strings (freed on row delete). */
static void free_dyn_bind_cb(lv_event_t * e)
{
    bind_ctx_t * b = lv_event_get_user_data(e);
    free((void *)b->type); free((void *)b->page); free((void *)b->param);
    free(b);
}

void colmenu_emit_switch(colmenu_emit_t * e, const char * icon, const char * label,
                         const char * type, const char * page, const char * param,
                         void (*on_change)(const char * value))
{
    char * v = colmenu_get(type, page, param, NULL);
    lv_obj_t * row = colstack_add_switch(e->cs, e->body, icon, label, v && atoi(v));
    free(v);
    bind_ctx_t * b = malloc(sizeof(bind_ctx_t));
    b->type = strdup(type); b->page = strdup(page); b->param = strdup(param);
    b->on_change = on_change; b->on_live = NULL;
    colstack_set_binding(row, do_set, b);
    lv_obj_add_event_cb(row, free_dyn_bind_cb, LV_EVENT_DELETE, b);
}

void colmenu_emit_submenu(colmenu_emit_t * e, const char * icon, const char * label,
                          const colmenu_page_t * sub)
{
    colstack_add_submenu(e->cs, e->body, icon, label, submenu_builder, (void *)sub);
}

void colmenu_emit_submenu_gated(colmenu_emit_t * e, const char * icon, const char * label,
                                const colmenu_page_t * sub)
{
    lv_obj_t * row = colstack_add_submenu(e->cs, e->body, icon, label, submenu_builder, (void *)sub);
    if(g_gated_n < COLMENU_MAX_GATED) g_gated[g_gated_n++] = row;
    colstack_set_enabled(row, g_drone_detected);
}

void colmenu_emit_action(colmenu_emit_t * e, const char * icon, const char * label,
                         void (*on_activate)(void * ctx), void * ctx)
{
    lv_obj_t * row = colstack_add_action(e->cs, e->body, icon, label);
    emit_action_t * a = malloc(sizeof(emit_action_t));
    a->on = on_activate;
    a->ctx = ctx;
    colstack_set_action(row, emit_action_invoke, a);
    lv_obj_add_event_cb(row, emit_action_delete, LV_EVENT_DELETE, a);
}

/* Deferred so it's safe to call from a row's own event (the rebuild deletes it). */
static void rescan_async(void * p) { (void)p; colstack_rebuild_current(g_cs); }
void colmenu_rescan(void) { lv_async_call(rescan_async, NULL); }

void colmenu_reflect_switch(const char * param, bool on)
{
    if(g_cs) colstack_reflect_switch(g_cs, param, on);
}

lv_group_t * colmenu_root_group(void) { return colstack_root_group(g_cs); }
void colmenu_set_root_back(void (*cb)(void * ctx), void * ctx) { colstack_set_root_back(g_cs, cb, ctx); }
