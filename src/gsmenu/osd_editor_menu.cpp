/*
 * OSD editor: System Settings → OSD.
 *
 *   OSD             widget list (bottom layer first), Add widget, Save, Revert
 *   └ <widget>      Enabled, Name, Position (move with the arrow keys), anchors,
 *     │             the type's fields (from osd_schema), Facts, order, Duplicate, Delete
 *     └ Facts       one row per bound fact, Add fact
 *       └ <fact>    which fact (picked from a list), tag filters, convert, Remove
 *         └ category → fact → source   (the fact picker)
 *
 * Every change is applied to the running OSD at once (osd_model rebuilds the
 * widget from its edited JSON); Save writes the layout file. The levels are built
 * with the column engine directly, because each one needs to know which widget
 * (and fact) it is about; that context rides along as the rows' builder data.
 *
 * Everything here runs in LVGL callbacks, i.e. on the OSD thread with the OSD
 * lock held, as osd_model requires.
 */
#include "osd_editor_menu.h"
#include "column_stack.h"
#include "../input.h"
#include "../menu.h"
#include "../osd_internal.hpp"
#include "../osd_schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

extern "C" {
extern lv_indev_t * indev_drv;
extern lv_obj_t * pp_menu_screen;
extern lv_obj_t * pp_osd_screen;
}
extern gsmenu_control_mode_t control_mode;

using json = osd_model::json;
namespace sch = osd_schema;
using Tags = std::map<std::string, std::string>;

static colstack_t * cs() { return colmenu_cs(); }

/* ── small helpers ─────────────────────────────────────────────────────────── */

/* What a row is about; owned by the row (deleted with it). */
struct RowCtx {
    uint32_t id = 0;      /* widget */
    int index = -1;       /* fact / range index; -1 = a new one */
    std::string key;      /* field / tag key, category, fact name, widget type */
};

static RowCtx * own(lv_obj_t * row, RowCtx * rc)
{
    lv_obj_add_event_cb(row, [](lv_event_t * e) { delete static_cast<RowCtx *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, rc);
    return rc;
}

static std::string str(const json & j, const char * key, const std::string & def = "")
{
    if(!j.is_object()) return def;
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

static double num(const json & j, const char * key, double def)
{
    if(!j.is_object()) return def;
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

/* The range item `index` of w[key] as an object to edit in place, or nullptr if
 * the widget or the item is gone. */
static json * range_item(json & w, const std::string & key, int index)
{
    if(!w.is_object() || !w.contains(key) || !w[key].is_array()) return nullptr;
    if(index < 0 || index >= (int)w[key].size()) return nullptr;
    json & item = w[key][index];
    if(!item.is_object()) item = json::object();
    return &item;
}

/* w[key] if it is a list, else an empty one (w may be null: the widget is gone) */
static json list_of(const json & w, const char * key)
{
    if(!w.is_object()) return json::array();
    auto it = w.find(key);
    return it != w.end() && it->is_array() ? *it : json::array();
}

static std::string fmt_num(double v, int precision = -1)
{
    char buf[64];
    if(precision < 0) snprintf(buf, sizeof(buf), "%g", v);
    else              snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return buf;
}

static std::string type_of(const json & w)
{
    std::string t = str(w, "type");
    return t.rfind("---", 0) == 0 ? t.substr(3) : t;
}

static std::string type_label(const std::string & type)
{
    const sch::WidgetType * t = sch::findType(type);
    return t ? t->label : type;
}

static osd_model::WidgetInfo info(uint32_t id)
{
    for(auto & wi : osd_model::list())
        if(wi.id == id) return wi;
    return {0, "", "", "", false};
}

static const sch::Field * field_of(uint32_t id, const std::string & key)
{
    const sch::WidgetType * t = sch::findType(type_of(osd_model::get(id)));
    if(!t) return nullptr;
    for(auto & f : t->fields)
        if(key == f.key) return &f;
    return nullptr;
}

static std::string tags_text(const Tags & tags)
{
    std::string s;
    for(auto & [k, v] : tags) s += (s.empty() ? "" : ", ") + k + "=" + v;
    return s;
}

static Tags tags_of(const json & fact)
{
    Tags tags;
    if(fact.is_object() && fact.contains("tags") && fact["tags"].is_object())
        for(auto & [k, v] : fact["tags"].items())
            if(v.is_string()) tags[k] = v.get<std::string>();
    return tags;
}

/* "wfbcli.rx.ant_stats.rssi_avg" → "rx.ant_stats.rssi_avg": the category says the rest */
static std::string short_name(const std::string & name)
{
    size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

static void finish(colstack_t * c)
{
    colstack_autofit(c);
    colstack_ensure_escapable(c);
}

/* Refresh the top column (deferred, the edit usually comes from one of its rows). */
static void refresh_later()
{
    lv_async_call([](void *) { colstack_rebuild_current_keep_focus(cs()); }, nullptr);
}

static void reveal_refresh(void *) { colstack_rebuild_current_keep_focus(cs()); }

static void message(const char * title, const std::string & text)
{
    colmenu_dialog(title, text.c_str(), nullptr, nullptr, nullptr);
}

/* Replace a widget's JSON. If the new version can't be built while the old one
 * could, put the old one back and say why (quiet: just revert, e.g. live slider). */
static bool apply_widget(uint32_t id, const json & w, bool quiet = false)
{
    json old = osd_model::get(id);
    std::string old_err = info(id).error;
    std::string err = osd_model::set(id, w);
    if(err.empty() || !old_err.empty()) return true;
    osd_model::set(id, old);
    if(!quiet) message("Can't apply this change", err);
    return false;
}

/* The fact placeholders in a template (positions, "%%" skipped). */
static std::vector<std::pair<size_t, size_t>> placeholders(const std::string & tpl)
{
    std::vector<std::pair<size_t, size_t>> out;
    static const std::regex re(R"(%%|%[bisud]|%(\.\d+)?f)");
    for(auto it = std::sregex_iterator(tpl.begin(), tpl.end(), re); it != std::sregex_iterator(); ++it)
        if(it->str() != "%%") out.push_back({(size_t)it->position(), (size_t)it->length()});
    return out;
}

/* Widgets whose template has one placeholder per fact, typed like the fact.
 * (VideoWidget turns its first fact into fps; the bitrate/latency/battery
 * templates show computed values, not the facts.) */
static bool template_follows_facts(const std::string & type, int fact_index)
{
    return type == "TplTextWidget" || type == "IconTplTextWidget" || (type == "VideoWidget" && fact_index > 0);
}

/* ── seen facts / catalog ──────────────────────────────────────────────────── */

static std::vector<osd_model::SeenFact> seen_of(const std::string & name)
{
    std::vector<osd_model::SeenFact> out;
    for(auto & f : osd_model::seen())
        if(f.name == name) out.push_back(f);
    std::sort(out.begin(), out.end(), [](auto & a, auto & b) { return a.tags < b.tags; });
    return out;
}

/* Latest value matching a widget's fact entry (name + all its tags), or "". */
static std::string live_value(const json & fact)
{
    std::string name = str(fact, "name");
    Tags want = tags_of(fact);
    for(auto & f : seen_of(name)) {
        bool ok = true;
        for(auto & [k, v] : want) {
            auto it = f.tags.find(k);
            if(it == f.tags.end() || it->second != v) { ok = false; break; }
        }
        if(ok) return f.value.size() > 14 ? f.value.substr(0, 14) + "..." : f.value;
    }
    return "";
}

static std::string type_of_fact(const std::string & name, const Tags & tags)
{
    for(auto & f : seen_of(name))
        if(tags.empty() || f.tags == tags) return f.type;
    const sch::FactInfo * fi = sch::findFact(name);
    return fi ? fi->type : "STRING";
}

/* Fact names of a category: catalog order first, then ones only seen at runtime. */
static std::vector<std::string> fact_names(const std::string & category)
{
    std::vector<std::string> out;
    std::set<std::string> have;
    for(auto & f : sch::catalog())
        if(sch::categoryOf(f.name) == category && have.insert(f.name).second) out.push_back(f.name);
    std::set<std::string> extra;
    for(auto & f : osd_model::seen())
        if(sch::categoryOf(f.name) == category && !have.count(f.name)) extra.insert(f.name);
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

static std::vector<std::string> icon_files()
{
    std::vector<std::string> out;
    std::string dir = osd_model::assetsDir();
    if(DIR * d = opendir(dir.c_str())) {
        while(struct dirent * e = readdir(d)) {
            std::string n = e->d_name;
            if(n.size() > 4 && n.compare(n.size() - 4, 4, ".png") == 0) out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/* ── highlight of the widget being looked at ───────────────────────────────── */

static lv_obj_t * g_hl;
static lv_timer_t * g_hl_timer;
static uint32_t g_hl_id;
static lv_obj_t * g_root_body;   /* the editor's list column (set while the editor is open) */
static struct MoveState * g_move;

static int32_t overlap(const lv_area_t & a, const lv_area_t & b)
{
    int32_t w = std::min(a.x2, b.x2) - std::max(a.x1, b.x1) + 1;
    int32_t h = std::min(a.y2, b.y2) - std::max(a.y1, b.y1) + 1;
    return w > 0 && h > 0 ? w * h : 0;
}

/* Keep the menu off the selected widget: if the columns cover it and would cover
 * less of it on the other side of the screen, dock them there. (Only switches when
 * it helps, so the menu doesn't jump around while browsing.) */
static void dock_update(const lv_area_t & widget)
{
    if(!g_root_body || g_move) return;
    lv_area_t menu, view;
    if(!colstack_visible_area(cs(), &menu, &view)) return;
    int32_t mw = lv_area_get_width(&menu);
    bool right = colstack_dock_right(cs());
    if(mw > lv_area_get_width(&view)) {          /* too wide to choose a side */
        if(right) colstack_set_dock_right(cs(), false);
        return;
    }
    lv_area_t left_side  = { view.x1, menu.y1, view.x1 + mw - 1, menu.y2 };
    lv_area_t right_side = { view.x2 - mw + 1, menu.y1, view.x2, menu.y2 };
    int32_t cur = overlap(widget, right ? right_side : left_side);
    int32_t alt = overlap(widget, right ? left_side : right_side);
    if(cur > 0 && alt < cur) colstack_set_dock_right(cs(), !right);
}

static void hl_place()
{
    if(!g_hl || !g_hl_id) return;
    if(osd_model::index(g_hl_id) < 0) {
        if(!lv_obj_has_flag(g_hl, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(g_hl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_area_t a;
    if(osd_model::bbox(g_hl_id, a)) {
        lv_obj_set_pos(g_hl, a.x1 - 4, a.y1 - 4);
        lv_obj_set_size(g_hl, lv_area_get_width(&a) + 8, lv_area_get_height(&a) + 8);
    } else {
        /* draws nothing right now (e.g. DVR indicator while not recording): mark x/y */
        int x, y;
        osd_model::anchorPoint(g_hl_id, x, y);
        lv_obj_set_pos(g_hl, x - 12, y - 12);
        lv_obj_set_size(g_hl, 24, 24);
        a = { x - 12, y - 12, x + 12, y + 12 };
    }
    dock_update(a);
    /* (unhiding always invalidates, i.e. a redraw on every refresh tick) */
    if(lv_obj_has_flag(g_hl, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(g_hl, LV_OBJ_FLAG_HIDDEN);
}

static void hl_show(uint32_t id)
{
    if(!g_hl) {
        /* on the top layer (above the menu), but beneath any dialog opened later */
        g_hl = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(g_hl);
        lv_obj_set_style_border_width(g_hl, 3, 0);
        lv_obj_set_style_border_color(g_hl, lv_color_hex(0xffcc00), 0);
        lv_obj_set_style_border_opa(g_hl, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g_hl, 4, 0);
        lv_obj_remove_flag(g_hl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(g_hl, LV_OBJ_FLAG_SCROLLABLE);
        /* follow the widget as it changes size (text updates, pop-ups) */
        g_hl_timer = lv_timer_create([](lv_timer_t *) { hl_place(); }, 250, nullptr);
    }
    g_hl_id = id;
    osd_model::setPreview(id);
    lv_obj_move_background(g_hl);
    hl_place();
}

static void hl_hide()
{
    g_hl_id = 0;
    osd_model::setPreview(0);
    if(g_hl) lv_obj_add_flag(g_hl, LV_OBJ_FLAG_HIDDEN);
}

static void hl_destroy()
{
    g_hl_id = 0;
    osd_model::setPreview(0);
    if(g_hl_timer) lv_timer_delete(g_hl_timer);
    if(g_hl) lv_obj_delete(g_hl);
    g_hl_timer = nullptr;
    g_hl = nullptr;
}

static void no_hl_on_focus(lv_obj_t * row)
{
    lv_obj_add_event_cb(row, [](lv_event_t *) { hl_hide(); }, LV_EVENT_FOCUSED, nullptr);
}

/* ── move mode: position a widget with the arrow keys ─────────────────────── */

struct MoveState {
    uint32_t id;
    int start_dx, start_dy;      /* offset before the move (restored on cancel)   */
    int dx, dy;
    lv_group_t * grp;
    lv_group_t * restore;
    lv_obj_t * catcher;          /* invisible focus target receiving the keys     */
    lv_obj_t * readout;
    uint32_t started;
    uint32_t last_key, last_tick;
    int streak;
    bool closing;
};

static void move_readout(MoveState * m, int step)
{
    json w = osd_model::get(m->id);
    int sw, sh, x, y;
    osd_model::screen(sw, sh);
    osd_model::anchorPoint(m->id, x, y);
    /* as it will be saved: negative = from the right / bottom edge */
    if(num(w, "x", 0) < 0) x -= sw;
    if(num(w, "y", 0) < 0) y -= sh;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s   x %d  y %d   step %d px\nCenter: done  |  hold center: cancel",
             str(w, "name").c_str(), x, y, step);
    lv_label_set_text(m->readout, buf);
}

/* How far `a` sticks out of the screen, horizontally / vertically. */
static int outside_x(const lv_area_t & a, int sw) { return std::max(0, -(int)a.x1) + std::max(0, (int)a.x2 - (sw - 1)); }
static int outside_y(const lv_area_t & a, int sh) { return std::max(0, -(int)a.y1) + std::max(0, (int)a.y2 - (sh - 1)); }

static void move_by(MoveState * m, int ddx, int ddy)
{
    int sw, sh;
    osd_model::screen(sw, sh);
    lv_area_t a;
    /* Don't push a widget further off-screen than it already is (icons at y=0
     * already start a bit above the top edge, so it's not forced fully inside). */
    if(osd_model::bbox(m->id, a)) {
        lv_area_t b = a;
        b.x1 += ddx; b.x2 += ddx;
        if(ddx && outside_x(b, sw) > outside_x(a, sw)) ddx = 0;
        b = a;
        b.y1 += ddy; b.y2 += ddy;
        if(ddy && outside_y(b, sh) > outside_y(a, sh)) ddy = 0;
    } else {
        int x, y;
        osd_model::anchorPoint(m->id, x, y);
        ddx = std::clamp(x + ddx, 0, sw - 1) - x;
        ddy = std::clamp(y + ddy, 0, sh - 1) - y;
    }
    m->dx += ddx;
    m->dy += ddy;
    osd_model::setOffset(m->id, m->dx, m->dy);
}

static void move_end_async(void * p)
{
    MoveState * m = static_cast<MoveState *>(p);
    lv_screen_load(pp_menu_screen);
    lv_indev_set_group(indev_drv, m->restore);
    control_mode = GSMENU_CONTROL_MODE_NAV;
    lv_group_delete(m->grp);
    lv_obj_delete(m->catcher);
    lv_obj_delete(m->readout);
    uint32_t id = m->id;
    delete m;
    g_move = nullptr;
    colstack_rebuild_current_keep_focus(cs());   /* the Position value changed */
    hl_show(id);
}

static void move_key_cb(lv_event_t * e)
{
    MoveState * m = static_cast<MoveState *>(lv_event_get_user_data(e));
    if(m->closing) return;
    uint32_t k = lv_event_get_key(e);
    uint32_t now = lv_tick_get();

    if(k == LV_KEY_ENTER || k == LV_KEY_ESC) {
        /* a center button held down when the move started repeats ENTER; don't
         * take that for "done" */
        if(lv_tick_elaps(m->started) < 400) return;
        if(k == LV_KEY_ENTER) osd_model::commitOffset(m->id);
        else                  osd_model::setOffset(m->id, m->start_dx, m->start_dy);
        m->closing = true;
        lv_async_call(move_end_async, m);
        return;
    }

    int ddx = 0, ddy = 0;
    switch(k) {
        case LV_KEY_UP:    ddy = -1; break;
        case LV_KEY_DOWN:  ddy = 1;  break;
        case LV_KEY_LEFT:  ddx = -1; break;
        case LV_KEY_RIGHT: ddx = 1;  break;
        default: return;
    }
    /* 1 px per press; accelerate while the key repeats (held button) */
    m->streak = (k == m->last_key && lv_tick_elaps(m->last_tick) < 180) ? m->streak + 1 : 0;
    m->last_key = k;
    m->last_tick = now;
    int step = m->streak < 3 ? 1 : m->streak < 8 ? 4 : m->streak < 16 ? 8 : 16;
    move_by(m, ddx * step, ddy * step);
    hl_place();
    move_readout(m, step);
}

static void move_begin(uint32_t id)
{
    if(g_move) return;
    MoveState * m = new MoveState{};
    m->id = id;
    osd_model::getOffset(id, m->start_dx, m->start_dy);
    m->dx = m->start_dx;
    m->dy = m->start_dy;
    m->started = lv_tick_get();
    m->restore = colstack_cur_group(cs());

    /* Show the plain OSD; menu_active stays true, so the keys keep coming here
     * instead of reopening the menu. */
    lv_screen_load(pp_osd_screen);

    m->catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(m->catcher);
    lv_obj_set_size(m->catcher, 0, 0);
    lv_obj_remove_flag(m->catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m->catcher, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_remove_flag(m->catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->catcher, move_key_cb, LV_EVENT_KEY, m);

    m->readout = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(m->readout, lv_color_hex(0x0b0e14), 0);
    lv_obj_set_style_bg_opa(m->readout, 200, 0);
    lv_obj_set_style_text_color(m->readout, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_text_align(m->readout, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(m->readout, 12, 0);
    lv_obj_set_style_radius(m->readout, 8, 0);
    lv_obj_align(m->readout, LV_ALIGN_BOTTOM_MID, 0, -40);

    m->grp = lv_group_create();
    lv_group_add_obj(m->grp, m->catcher);
    lv_indev_set_group(indev_drv, m->grp);
    control_mode = GSMENU_CONTROL_MODE_MOVE;
    g_move = m;

    hl_show(id);
    move_readout(m, 1);
}

/* ── text prompts (one at a time) ──────────────────────────────────────────── */

enum class PromptKind { Name, Field, Number, Convert };
static struct {
    PromptKind kind;
    uint32_t id;
    int index;
    std::string key;
} g_prompt;

static void prompt_ok(const char * text, void *)
{
    json w = osd_model::get(g_prompt.id);
    if(w.is_null()) return;
    std::string t = text ? text : "";
    switch(g_prompt.kind) {
        case PromptKind::Name:
            w["name"] = t;
            break;
        case PromptKind::Field:
            w[g_prompt.key] = t;
            break;
        case PromptKind::Number: {
            char * end = nullptr;
            double v = strtod(t.c_str(), &end);
            if(t.empty() || *end) {
                message("Not a number", "'" + t + "' is not a number.");
                return;
            }
            const sch::Field * f = field_of(g_prompt.id, g_prompt.key);
            if(f && f->precision == 0 && v == std::floor(v) && std::fabs(v) < 1e15) w[g_prompt.key] = (long long)v;
            else                                                                       w[g_prompt.key] = v;
            break;
        }
        case PromptKind::Convert: {
            json & fact = w["facts"][g_prompt.index];
            if(t.empty()) {
                fact.erase("convert");
            } else {
                std::string err = osd_model::checkExpression(t);
                if(!err.empty()) {
                    message("Invalid expression", "'" + t + "': " + err +
                            "\n\nUse x for the value, numbers, + - * / and ( ), e.g. x / 1000");
                    return;
                }
                fact["convert"] = t;
            }
            break;
        }
    }
    apply_widget(g_prompt.id, w);
    refresh_later();
}

static void prompt(PromptKind kind, uint32_t id, int index, const std::string & key,
                   const std::string & title, const std::string & initial)
{
    g_prompt.kind = kind;
    g_prompt.id = id;
    g_prompt.index = index;
    g_prompt.key = key;
    colmenu_prompt_text(title.c_str(), initial.c_str(), prompt_ok, nullptr);
}

/* ── forward declarations of the levels ────────────────────────────────────── */

static void build_widget(colstack_t * c, lv_obj_t * body, void * user);
static void build_facts(colstack_t * c, lv_obj_t * body, void * user);
static void build_fact(colstack_t * c, lv_obj_t * body, void * user);
static void build_categories(colstack_t * c, lv_obj_t * body, void * user);
static void build_color(colstack_t * c, lv_obj_t * body, void * user);
static void build_ranges(colstack_t * c, lv_obj_t * body, void * user);
static void build_add(colstack_t * c, lv_obj_t * body, void * user);

/* ── OSD: the widget list ──────────────────────────────────────────────────── */

static std::vector<std::pair<uint32_t, lv_obj_t *>> g_root_rows;

static lv_obj_t * root_row_of(uint32_t id)
{
    for(auto & [rid, row] : g_root_rows)
        if(rid == id) return row;
    return nullptr;
}

static void pop_once(void *) { colstack_pop(cs()); }

static void save_now()
{
    std::string err = osd_model::save();
    if(!err.empty()) message("Saving the OSD layout failed", err);
}

/* Back out of the editor with unsaved changes: ask first. */
static bool root_on_back(void *)
{
    if(!osd_model::dirty()) return false;
    colmenu_dialog("Unsaved OSD changes", "Save them to the OSD layout file?", "Save\nDiscard\nCancel",
                   [](int choice, void *) {
                       if(choice == 0) {
                           std::string err = osd_model::save();
                           if(!err.empty()) { message("Saving the OSD layout failed", err); return; }
                       } else if(choice == 1) {
                           std::string err = osd_model::reload();
                           if(!err.empty()) message("Reloading the OSD layout failed", err);
                       } else {
                           return;
                       }
                       colstack_pop(cs());
                   },
                   nullptr);
    return true;
}

static void root_body_deleted(lv_event_t *)
{
    g_root_body = nullptr;
    g_root_rows.clear();
    hl_destroy();
    pp_menu_set_header_visible(true);
}

static void build_root(colmenu_emit_t * e)
{
    colstack_t * c = colmenu_emit_cs(e);
    lv_obj_t * body = colmenu_emit_body(e);
    if(body != g_root_body) {
        /* entering the editor: show the whole OSD (no logo bar) until we leave,
         * and keep the menu narrow - only the editor's last two columns, without
         * the Menu / System Settings columns - so it can dock away from the
         * selected widget (dock_update) */
        g_root_body = body;
        lv_obj_add_event_cb(body, root_body_deleted, LV_EVENT_DELETE, nullptr);
        pp_menu_set_header_visible(false);
        colstack_limit_visible(c, colstack_depth(c) - 1, 2);
    }
    colstack_set_column_hooks(c, root_on_back, reveal_refresh, nullptr);
    g_root_rows.clear();

    int sw, sh;
    osd_model::screen(sw, sh);
    std::string path = osd_model::configPath();
    std::string file = path.empty() ? "no --osd-config file" : path.substr(path.find_last_of('/') + 1);
    char caption[256];
    snprintf(caption, sizeof(caption), "%dx%d  |  %s%s", sw, sh, file.c_str(),
             osd_model::dirty() ? "  |  unsaved" : "");
    colstack_add_title(c, body, caption);

    auto list = osd_model::list();
    if(list.empty()) colstack_add_title(c, body, "No widgets yet");
    for(auto & wi : list) {
        const char * icon = !wi.error.empty() ? LV_SYMBOL_WARNING : !wi.enabled ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_IMAGE;
        RowCtx * rc = new RowCtx{wi.id};
        lv_obj_t * row = colstack_add_submenu(c, body, icon, wi.name.empty() ? "(unnamed)" : wi.name.c_str(),
                                              build_widget, rc);
        own(row, rc);
        colstack_set_row_value(row, type_label(wi.type).c_str());
        lv_obj_add_event_cb(row, [](lv_event_t * ev) {
            hl_show(static_cast<RowCtx *>(lv_event_get_user_data(ev))->id);
        }, LV_EVENT_FOCUSED, rc);
        g_root_rows.push_back({wi.id, row});
    }

    lv_obj_t * row = colstack_add_submenu(c, body, LV_SYMBOL_PLUS, "Add widget", build_add, nullptr);
    no_hl_on_focus(row);

    row = colstack_add_action(c, body, LV_SYMBOL_SAVE, "Save");
    colstack_set_row_value(row, path.empty() ? "no file" : osd_model::dirty() ? "unsaved changes" : "saved");
    colstack_set_action(row, [](void *) { save_now(); refresh_later(); }, nullptr);
    no_hl_on_focus(row);

    row = colstack_add_action(c, body, LV_SYMBOL_REFRESH, "Revert to saved");
    colstack_set_action(row, [](void *) {
        colmenu_dialog("Revert", "Discard all changes since the last save?", "Revert\nCancel",
                       [](int choice, void *) {
                           if(choice != 0) return;
                           hl_hide();
                           std::string err = osd_model::reload();
                           if(!err.empty()) message("Reloading the OSD layout failed", err);
                           colstack_rebuild_current_keep_focus(cs());
                       },
                       nullptr);
    }, nullptr);
    no_hl_on_focus(row);

    /* the first row got focus before its handler existed */
    lv_obj_t * f = lv_group_get_focused(colstack_cur_group(c));
    hl_hide();
    for(auto & [id, r] : g_root_rows)
        if(r == f) hl_show(id);
}

extern "C" const colmenu_page_t osd_editor_page = { .title = "OSD", .build = build_root };

/* ── Add widget ────────────────────────────────────────────────────────────── */

static std::string unique_name(const std::string & base)
{
    std::set<std::string> names;
    for(auto & wi : osd_model::list()) names.insert(wi.name);
    if(!names.count(base)) return base;
    for(int n = 2;; n++) {
        std::string s = base + " " + std::to_string(n);
        if(!names.count(s)) return s;
    }
}

/* Open a widget's page from the list (as if its row was entered) */
static void open_widget(uint32_t id)
{
    colstack_rebuild_current(cs());
    lv_obj_t * row = root_row_of(id);
    if(!row) return;
    lv_group_focus_obj(row);
    uint32_t key = LV_KEY_ENTER;
    lv_obj_send_event(row, LV_EVENT_KEY, &key);
}

static void add_widget_async(void * p)
{
    std::string * type = static_cast<std::string *>(p);
    const sch::WidgetType * t = sch::findType(*type);
    delete type;
    if(!t) return;

    int sw, sh;
    osd_model::screen(sw, sh);
    json w = json::object();
    w["name"] = unique_name(t->label);
    w["type"] = t->type;
    w["x"] = t->movable ? sw / 2 - 100 : 0;
    w["y"] = t->movable ? sh / 2 : 0;
    json preset = json::parse(t->preset);   /* (not a temporary: items() refers into it) */
    for(auto & [k, v] : preset.items()) w[k] = v;
    uint32_t id = osd_model::insert(w);

    /* back to the list, open the new widget and let the user place it */
    colstack_pop(cs());
    open_widget(id);
    if(t->movable) move_begin(id);
}

static void build_add(colstack_t * c, lv_obj_t * body, void *)
{
    colstack_add_title(c, body, "New widget type");
    for(auto & t : sch::types()) {
        lv_obj_t * row = colstack_add_action(c, body, NULL, t.label);
        RowCtx * rc = own(row, new RowCtx{0, -1, t.type});
        colstack_set_action(row, [](void * ctx) {
            lv_async_call(add_widget_async, new std::string(static_cast<RowCtx *>(ctx)->key));
        }, rc);
    }
    finish(c);
}

/* ── <widget> ──────────────────────────────────────────────────────────────── */

static void on_enabled(void * ctx, const char * value)
{
    uint32_t id = static_cast<RowCtx *>(ctx)->id;
    json w = osd_model::get(id);
    std::string type = type_of(w);
    w["type"] = strcmp(value, "on") == 0 ? type : "---" + type;
    apply_widget(id, w);
    refresh_later();
}

static void on_anchor(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    json w = osd_model::get(rc->id);
    bool right = num(w, "x", 0) < 0, bottom = num(w, "y", 0) < 0;
    bool far = strstr(value, "right") || strstr(value, "bottom");
    if(rc->key == "x") right = far;
    else               bottom = far;
    osd_model::setAnchor(rc->id, right, bottom);
    refresh_later();
}

static void set_number(uint32_t id, const std::string & key, const char * value, bool quiet)
{
    json w = osd_model::get(id);
    const sch::Field * f = field_of(id, key);
    double v = atof(value);
    if(f && f->precision == 0) w[key] = (long long)llround(v);
    else                       w[key] = v;
    apply_widget(id, w, quiet);
}

static void on_number(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    set_number(rc->id, rc->key, value, false);
    refresh_later();
}

static void on_number_live(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    set_number(rc->id, rc->key, value, true);
}

static void on_choice(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    json w = osd_model::get(rc->id);
    std::string v = value;
    const sch::Field * f = field_of(rc->id, rc->key);
    char * end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if(f && f->kind == sch::Kind::Enum && !v.empty() && !*end) w[rc->key] = n;   /* e.g. num_cells 4 */
    else                                                       w[rc->key] = v;
    apply_widget(rc->id, w);
}

static void on_bool(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    json w = osd_model::get(rc->id);
    w[rc->key] = strcmp(value, "on") == 0;
    apply_widget(rc->id, w);
}

static void add_slider(colstack_t * c, lv_obj_t * body, uint32_t id, const char * key, const char * label,
                       double v, double lo, double hi, double step, int precision)
{
    double scale = std::pow(10, precision);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    lv_obj_t * row = colstack_add_slider(c, body, NULL, label, (int32_t)llround(lo * scale),
                                         (int32_t)llround(hi * scale), (int32_t)llround(v * scale), precision, 0);
    colstack_set_slider_step(row, std::max<int32_t>(1, (int32_t)llround(step * scale)));
    RowCtx * rc = own(row, new RowCtx{id, -1, key});
    colstack_set_binding(row, on_number, rc);
    colstack_set_slider_live(row, on_number_live, rc);
}

static void add_field_row(colstack_t * c, lv_obj_t * body, uint32_t id, const json & w, const sch::Field & f)
{
    lv_obj_t * row = nullptr;
    switch(f.kind) {
        case sch::Kind::Text:
        case sch::Kind::Template: {
            row = colstack_add_action(c, body, LV_SYMBOL_EDIT, f.label);
            std::string v = str(w, f.key);
            colstack_set_row_value(row, v.empty() ? "(empty)" : v.c_str());
            colstack_set_action(row, [](void * ctx) {
                RowCtx * rc = static_cast<RowCtx *>(ctx);
                const sch::Field * fd = field_of(rc->id, rc->key);
                prompt(PromptKind::Field, rc->id, -1, rc->key, fd ? fd->label : rc->key,
                       str(osd_model::get(rc->id), rc->key.c_str()));
            }, own(row, new RowCtx{id, -1, f.key}));
            break;
        }
        case sch::Kind::Number: {
            row = colstack_add_action(c, body, LV_SYMBOL_EDIT, f.label);
            colstack_set_row_value(row, fmt_num(num(w, f.key, f.def)).c_str());
            colstack_set_action(row, [](void * ctx) {
                RowCtx * rc = static_cast<RowCtx *>(ctx);
                const sch::Field * fd = field_of(rc->id, rc->key);
                prompt(PromptKind::Number, rc->id, -1, rc->key, fd ? fd->label : rc->key,
                       fmt_num(num(osd_model::get(rc->id), rc->key.c_str(), fd ? fd->def : 0)));
            }, own(row, new RowCtx{id, -1, f.key}));
            break;
        }
        case sch::Kind::Int:
        case sch::Kind::Double:
            add_slider(c, body, id, f.key, f.label, num(w, f.key, f.def), f.min, f.max, f.step, f.precision);
            break;
        case sch::Kind::Bool: {
            bool on = w.is_object() && w.contains(f.key) && w[f.key].is_boolean() ? w[f.key].get<bool>() : f.def != 0;
            row = colstack_add_switch(c, body, NULL, f.label, on);
            colstack_set_binding(row, on_bool, own(row, new RowCtx{id, -1, f.key}));
            break;
        }
        case sch::Kind::Enum: {
            std::string cur = w.is_object() && w.contains(f.key)
                ? (w[f.key].is_string() ? w[f.key].get<std::string>() : w[f.key].dump()) : "";
            std::string opts = f.options;
            int sel = 0, i = 0;
            size_t pos = 0;
            while(pos <= opts.size()) {
                size_t nl = opts.find('\n', pos);
                if(opts.substr(pos, nl - pos) == cur) sel = i;
                if(nl == std::string::npos) break;
                pos = nl + 1;
                i++;
            }
            row = colstack_add_dropdown(c, body, NULL, f.label, opts.c_str(), sel);
            colstack_set_binding(row, on_choice, own(row, new RowCtx{id, -1, f.key}));
            break;
        }
        case sch::Kind::Icon: {
            std::string cur = str(w, f.key);
            auto icons = icon_files();
            if(!cur.empty() && std::find(icons.begin(), icons.end(), cur) == icons.end()) icons.insert(icons.begin(), cur);
            std::string opts;
            int sel = 0;
            for(size_t i = 0; i < icons.size(); i++) {
                opts += (i ? "\n" : "") + icons[i];
                if(icons[i] == cur) sel = (int)i;
            }
            row = colstack_add_dropdown(c, body, LV_SYMBOL_IMAGE, f.label, opts.c_str(), sel);
            colstack_set_binding(row, on_choice, own(row, new RowCtx{id, -1, f.key}));
            break;
        }
        case sch::Kind::Color: {
            RowCtx * rc = new RowCtx{id, -1, f.key};
            row = colstack_add_submenu(c, body, NULL, f.label, build_color, rc);
            own(row, rc);
            json col = w.is_object() && w.contains(f.key) ? w[f.key] : json::object();
            char buf[64];
            snprintf(buf, sizeof(buf), "#%02x%02x%02x  %d%%", (int)llround(num(col, "r", 0) * 255),
                     (int)llround(num(col, "g", 0) * 255), (int)llround(num(col, "b", 0) * 255),
                     (int)llround(num(col, "alpha", 1) * 100));
            colstack_set_row_value(row, buf);
            break;
        }
        case sch::Kind::Ranges: {
            RowCtx * rc = new RowCtx{id, -1, f.key};
            row = colstack_add_submenu(c, body, NULL, f.label, build_ranges, rc);
            own(row, rc);
            size_t n = w.is_object() && w.contains(f.key) && w[f.key].is_array() ? w[f.key].size() : 0;
            colstack_set_row_value(row, std::to_string(n).c_str());
            break;
        }
    }
}

static void duplicate_async(void * p)
{
    uint32_t id = (uint32_t)(uintptr_t)p;
    json w = osd_model::get(id);
    if(w.is_null()) return;
    w["name"] = unique_name(str(w, "name") + " copy");
    /* 20 px away from the edge each coordinate counts from, so a right/bottom
     * anchored (negative) one stays negative instead of flipping sides */
    auto shift = [](int v) { return v < 0 ? v - 20 : v + 20; };
    w["x"] = shift((int)num(w, "x", 0));
    w["y"] = shift((int)num(w, "y", 0));
    uint32_t copy = osd_model::insert(w, osd_model::index(id) + 1);
    colstack_pop(cs());
    open_widget(copy);
}

static void build_widget(colstack_t * c, lv_obj_t * body, void * user)
{
    uint32_t id = static_cast<RowCtx *>(user)->id;
    json w = osd_model::get(id);
    if(w.is_null()) {
        colstack_add_title(c, body, "This widget is gone");
        finish(c);
        return;
    }
    colstack_set_column_hooks(c, nullptr, reveal_refresh, nullptr);
    hl_show(id);

    osd_model::WidgetInfo wi = info(id);
    std::string type = type_of(w);
    const sch::WidgetType * t = sch::findType(type);
    colstack_add_title(c, body, type_label(type).c_str());
    if(!wi.error.empty()) colstack_add_title(c, body, (std::string(LV_SYMBOL_WARNING " ") + wi.error).c_str());

    lv_obj_t * row = colstack_add_switch(c, body, LV_SYMBOL_EYE_OPEN, "Enabled", wi.enabled);
    colstack_set_binding(row, on_enabled, own(row, new RowCtx{id}));

    row = colstack_add_action(c, body, LV_SYMBOL_EDIT, "Name");
    colstack_set_row_value(row, str(w, "name").c_str());
    colstack_set_action(row, [](void * ctx) {
        uint32_t wid = static_cast<RowCtx *>(ctx)->id;
        prompt(PromptKind::Name, wid, -1, "name", "Name", str(osd_model::get(wid), "name"));
    }, own(row, new RowCtx{id}));

    if(!t || t->movable) {
        int x = (int)num(w, "x", 0), y = (int)num(w, "y", 0);
        row = colstack_add_action(c, body, LV_SYMBOL_SHUFFLE, "Position");
        colstack_set_row_value(row, ("x " + std::to_string(x) + ", y " + std::to_string(y)).c_str());
        colstack_set_action(row, [](void * ctx) { move_begin(static_cast<RowCtx *>(ctx)->id); }, own(row, new RowCtx{id}));

        /* x/y count from the left/top edge, or (negative) from the right/bottom
         * one, which keeps a widget in its corner on other screen sizes */
        row = colstack_add_dropdown(c, body, NULL, "Horizontal", "from left\nfrom right", x < 0);
        colstack_set_binding(row, on_anchor, own(row, new RowCtx{id, -1, "x"}));
        row = colstack_add_dropdown(c, body, NULL, "Vertical", "from top\nfrom bottom", y < 0);
        colstack_set_binding(row, on_anchor, own(row, new RowCtx{id, -1, "y"}));
    }

    if(t)
        for(auto & f : t->fields) add_field_row(c, body, id, w, f);

    if(!t || t->max_facts != 0) {
        RowCtx * rc = new RowCtx{id};
        row = colstack_add_submenu(c, body, LV_SYMBOL_LIST, "Facts", build_facts, rc);
        own(row, rc);
        size_t n = w.contains("facts") && w["facts"].is_array() ? w["facts"].size() : 0;
        colstack_set_row_value(row, std::to_string(n).c_str());
    }

    int idx = osd_model::index(id), count = (int)osd_model::list().size();
    std::string layer = "layer " + std::to_string(idx + 1) + " of " + std::to_string(count);
    row = colstack_add_action(c, body, LV_SYMBOL_UP, "Move up the list (below)");
    colstack_set_row_value(row, layer.c_str());
    colstack_set_action(row, [](void * ctx) {
        osd_model::moveZ(static_cast<RowCtx *>(ctx)->id, -1);
        refresh_later();
    }, own(row, new RowCtx{id}));
    row = colstack_add_action(c, body, LV_SYMBOL_DOWN, "Move down the list (on top)");
    colstack_set_action(row, [](void * ctx) {
        osd_model::moveZ(static_cast<RowCtx *>(ctx)->id, 1);
        refresh_later();
    }, own(row, new RowCtx{id}));

    if(!t || !t->unique) {
        row = colstack_add_action(c, body, LV_SYMBOL_COPY, "Duplicate");
        colstack_set_action(row, [](void * ctx) {
            lv_async_call(duplicate_async, (void *)(uintptr_t) static_cast<RowCtx *>(ctx)->id);
        }, own(row, new RowCtx{id}));
    }

    row = colstack_add_action(c, body, LV_SYMBOL_TRASH, "Delete");
    colstack_set_action(row, [](void * ctx) {
        uint32_t wid = static_cast<RowCtx *>(ctx)->id;
        std::string text = "Delete '" + str(osd_model::get(wid), "name") + "' from the OSD?";
        colmenu_dialog("Delete widget", text.c_str(), "Delete\nCancel", [](int choice, void * user) {
            if(choice == 0) {
                hl_hide();
                osd_model::remove((uint32_t)(uintptr_t)user);
                colstack_pop(cs());
            }
        }, (void *)(uintptr_t)wid);
    }, own(row, new RowCtx{id}));

    finish(c);
}

/* ── Color ─────────────────────────────────────────────────────────────────── */

static const struct { const char * name; double r, g, b; } kColors[] = {
    {"black", 0, 0, 0}, {"white", 1, 1, 1}, {"gray", 0.5, 0.5, 0.5}, {"red", 1, 0, 0},
    {"green", 0, 0.8, 0}, {"blue", 0, 0.3, 1}, {"yellow", 1, 0.85, 0}, {"orange", 1, 0.55, 0},
};

static void set_color(RowCtx * rc, const std::string & part, double v, bool quiet)
{
    json w = osd_model::get(rc->id);
    json & col = w[rc->key];
    if(!col.is_object()) col = {{"r", 0.0}, {"g", 0.0}, {"b", 0.0}, {"alpha", 1.0}};
    col[part] = std::round(v * 100) / 100;
    apply_widget(rc->id, w, quiet);
}

static void build_color(colstack_t * c, lv_obj_t * body, void * user)
{
    RowCtx * owner = static_cast<RowCtx *>(user);
    colstack_set_column_hooks(c, nullptr, reveal_refresh, nullptr);
    json col = osd_model::get(owner->id)[owner->key];

    lv_obj_t * row = colstack_add_dropdown(c, body, NULL, "Preset",
                                           "-\nblack\nwhite\ngray\nred\ngreen\nblue\nyellow\norange", 0);
    colstack_set_binding(row, [](void * ctx, const char * value) {
        RowCtx * rc = static_cast<RowCtx *>(ctx);
        for(auto & k : kColors) {
            if(strcmp(k.name, value)) continue;
            json w = osd_model::get(rc->id);
            json & cl = w[rc->key];
            double a = num(cl, "alpha", 1);
            cl = {{"r", k.r}, {"g", k.g}, {"b", k.b}, {"alpha", a}};
            apply_widget(rc->id, w);
            refresh_later();
        }
    }, own(row, new RowCtx{owner->id, -1, owner->key}));

    static const struct { const char * part; const char * label; double def; } parts[] = {
        {"r", "Red %", 0}, {"g", "Green %", 0}, {"b", "Blue %", 0}, {"alpha", "Opacity %", 1},
    };
    for(auto & p : parts) {
        int pct = (int)llround(num(col, p.part, p.def) * 100);
        row = colstack_add_slider(c, body, NULL, p.label, 0, 100, pct, 0, 0);
        colstack_set_slider_step(row, 5);
        RowCtx * rc = own(row, new RowCtx{owner->id, -1, owner->key + "\n" + p.part});
        colstack_set_binding(row, [](void * ctx, const char * value) {
            RowCtx * r = static_cast<RowCtx *>(ctx);
            size_t nl = r->key.find('\n');
            RowCtx target{r->id, -1, r->key.substr(0, nl)};
            set_color(&target, r->key.substr(nl + 1), atof(value) / 100, false);
        }, rc);
        colstack_set_slider_live(row, [](void * ctx, const char * value) {
            RowCtx * r = static_cast<RowCtx *>(ctx);
            size_t nl = r->key.find('\n');
            RowCtx target{r->id, -1, r->key.substr(0, nl)};
            set_color(&target, r->key.substr(nl + 1), atof(value) / 100, true);
        }, rc);
    }
    finish(c);
}

/* ── Ranges (IconSelectorWidget) ───────────────────────────────────────────── */

static void build_range(colstack_t * c, lv_obj_t * body, void * user)
{
    RowCtx * owner = static_cast<RowCtx *>(user);
    json w = osd_model::get(owner->id);
    json list = list_of(w, owner->key.c_str());
    if(owner->index < 0 || owner->index >= (int)list.size()) {
        colstack_add_title(c, body, "This range is gone");
        finish(c);
        return;
    }
    const json & r = list[owner->index];
    json ends = list_of(r, "range");
    int lo = ends.size() == 2 && ends[0].is_number() ? ends[0].get<int>() : 0;
    int hi = ends.size() == 2 && ends[1].is_number() ? ends[1].get<int>() : 0;

    for(int end = 0; end < 2; end++) {
        int v = end ? hi : lo;
        lv_obj_t * row = colstack_add_slider(c, body, NULL, end ? "To" : "From", std::min(-200, v - 100),
                                             std::max(200, v + 100), v, 0, 0);
        colstack_set_slider_step(row, 1);
        RowCtx * rc = own(row, new RowCtx{owner->id, owner->index, owner->key + (end ? "\n1" : "\n0")});
        colstack_set_binding(row, [](void * ctx, const char * value) {
            RowCtx * x = static_cast<RowCtx *>(ctx);
            size_t nl = x->key.find('\n');
            json wj = osd_model::get(x->id);
            json * item = range_item(wj, x->key.substr(0, nl), x->index);
            if(!item) return;
            json & rr = (*item)["range"];
            if(!rr.is_array() || rr.size() != 2) rr = json::array({0, 0});
            rr[atoi(x->key.c_str() + nl + 1)] = atoi(value);
            apply_widget(x->id, wj);
        }, rc);
    }

    std::string cur = str(r, "icon_path");
    auto icons = icon_files();
    if(!cur.empty() && std::find(icons.begin(), icons.end(), cur) == icons.end()) icons.insert(icons.begin(), cur);
    std::string opts;
    int sel = 0;
    for(size_t i = 0; i < icons.size(); i++) {
        opts += (i ? "\n" : "") + icons[i];
        if(icons[i] == cur) sel = (int)i;
    }
    lv_obj_t * row = colstack_add_dropdown(c, body, LV_SYMBOL_IMAGE, "Icon", opts.c_str(), sel);
    colstack_set_binding(row, [](void * ctx, const char * value) {
        RowCtx * x = static_cast<RowCtx *>(ctx);
        json wj = osd_model::get(x->id);
        json * item = range_item(wj, x->key, x->index);
        if(!item) return;
        (*item)["icon_path"] = value;
        apply_widget(x->id, wj);
    }, own(row, new RowCtx{owner->id, owner->index, owner->key}));

    row = colstack_add_action(c, body, LV_SYMBOL_TRASH, "Remove range");
    colstack_set_action(row, [](void * ctx) {
        RowCtx * x = static_cast<RowCtx *>(ctx);
        json wj = osd_model::get(x->id);
        if(range_item(wj, x->key, x->index)) {
            wj[x->key].erase(wj[x->key].begin() + x->index);
            apply_widget(x->id, wj);
        }
        lv_async_call(pop_once, nullptr);
    }, own(row, new RowCtx{owner->id, owner->index, owner->key}));
    finish(c);
}

static void build_ranges(colstack_t * c, lv_obj_t * body, void * user)
{
    RowCtx * owner = static_cast<RowCtx *>(user);
    colstack_set_column_hooks(c, nullptr, reveal_refresh, nullptr);
    json w = osd_model::get(owner->id);
    json list = list_of(w, owner->key.c_str());
    colstack_add_title(c, body, "Shows the icon of the range the value is in");
    for(size_t i = 0; i < list.size(); i++) {
        const json & r = list[i];
        std::string text = r.contains("range") ? r["range"].dump() : "[?]";
        RowCtx * rc = new RowCtx{owner->id, (int)i, owner->key};
        lv_obj_t * row = colstack_add_submenu(c, body, NULL, text.c_str(), build_range, rc);
        own(row, rc);
        colstack_set_row_value(row, str(r, "icon_path").c_str());
    }
    lv_obj_t * row = colstack_add_action(c, body, LV_SYMBOL_PLUS, "Add range");
    colstack_set_action(row, [](void * ctx) {
        RowCtx * x = static_cast<RowCtx *>(ctx);
        json wj = osd_model::get(x->id);
        json & l = wj[x->key];
        if(!l.is_array()) l = json::array();
        json r = l.empty() ? json{{"range", {0, 0}}, {"icon_path", "signal1.png"}} : l.back();
        l.push_back(r);
        apply_widget(x->id, wj);
        refresh_later();
    }, own(row, new RowCtx{owner->id, -1, owner->key}));
    finish(c);
}

/* ── Facts ─────────────────────────────────────────────────────────────────── */

static void build_facts(colstack_t * c, lv_obj_t * body, void * user)
{
    uint32_t id = static_cast<RowCtx *>(user)->id;
    colstack_set_column_hooks(c, nullptr, reveal_refresh, nullptr);
    json w = osd_model::get(id);
    const sch::WidgetType * t = sch::findType(type_of(w));
    json facts = list_of(w, "facts");

    if(w.contains("template")) colstack_add_title(c, body, ("Text: " + str(w, "template")).c_str());
    for(size_t i = 0; i < facts.size(); i++) {
        std::string name = str(facts[i], "name");
        std::string tags = tags_text(tags_of(facts[i]));
        std::string label = std::to_string(i + 1) + ". " + name + (tags.empty() ? "" : "\n" + tags);
        RowCtx * rc = new RowCtx{id, (int)i};
        lv_obj_t * row = colstack_add_submenu(c, body, NULL, label.c_str(), build_fact, rc);
        own(row, rc);
        std::string v = live_value(facts[i]);
        colstack_set_row_value(row, v.empty() ? "no data" : v.c_str());
    }
    if(t && (int)facts.size() < t->min_facts)
        colstack_add_title(c, body, (std::string(LV_SYMBOL_WARNING " needs ") + std::to_string(t->min_facts) +
                                     " fact" + (t->min_facts > 1 ? "s" : "")).c_str());
    if(!t || t->max_facts < 0 || (int)facts.size() < t->max_facts) {
        RowCtx * rc = new RowCtx{id, -1};
        lv_obj_t * row = colstack_add_submenu(c, body, LV_SYMBOL_PLUS, "Add fact", build_categories, rc);
        own(row, rc);
    }
    finish(c);
}

static void on_tag(void * ctx, const char * value)
{
    RowCtx * rc = static_cast<RowCtx *>(ctx);
    json w = osd_model::get(rc->id);
    json & fact = w["facts"][rc->index];
    if(strcmp(value, "(any)") == 0) {
        if(fact.contains("tags")) fact["tags"].erase(rc->key);
    } else {
        fact["tags"][rc->key] = value;
    }
    if(fact.contains("tags") && fact["tags"].empty()) fact.erase("tags");
    apply_widget(rc->id, w);
}

static void remove_fact(uint32_t id, int index)
{
    json w = osd_model::get(id);
    json & facts = w["facts"];
    if(!facts.is_array() || index >= (int)facts.size()) return;
    facts.erase(facts.begin() + index);
    std::string type = type_of(w);
    if(template_follows_facts(type, index) && w.contains("template")) {
        std::string tpl = str(w, "template");
        auto ph = placeholders(tpl);
        if(index < (int)ph.size()) {
            size_t pos = ph[index].first, len = ph[index].second;
            if(pos > 0 && tpl[pos - 1] == ' ') { pos--; len++; }
            tpl.erase(pos, len);
            w["template"] = tpl;
        }
    }
    apply_widget(id, w);
}

static void build_fact(colstack_t * c, lv_obj_t * body, void * user)
{
    RowCtx * owner = static_cast<RowCtx *>(user);
    uint32_t id = owner->id;
    int index = owner->index;
    colstack_set_column_hooks(c, nullptr, reveal_refresh, nullptr);
    json w = osd_model::get(id);
    json facts = list_of(w, "facts");
    if(index < 0 || index >= (int)facts.size()) {
        colstack_add_title(c, body, "This fact is gone");
        finish(c);
        return;
    }
    const json & fact = facts[index];
    std::string name = str(fact, "name");

    RowCtx * rc = new RowCtx{id, index};
    lv_obj_t * row = colstack_add_submenu(c, body, LV_SYMBOL_LIST, "Fact", build_categories, rc);
    own(row, rc);
    colstack_set_row_value(row, name.c_str());
    if(const sch::FactInfo * fi = sch::findFact(name)) colstack_add_title(c, body, fi->description.c_str());

    /* tag filters: keys the source sets, with the values seen so far */
    Tags cur = tags_of(fact);
    std::set<std::string> keys;
    std::map<std::string, std::set<std::string>> values;
    if(const sch::FactInfo * fi = sch::findFact(name))
        for(auto & k : fi->tags) keys.insert(k);
    for(auto & f : seen_of(name))
        for(auto & [k, v] : f.tags) { keys.insert(k); values[k].insert(v); }
    for(auto & [k, v] : cur) { keys.insert(k); values[k].insert(v); }
    if(!keys.empty()) colstack_add_title(c, body, "Only from");
    for(auto & k : keys) {
        std::string opts = "(any)";
        int sel = 0, i = 1;
        for(auto & v : values[k]) {
            opts += "\n" + v;
            if(cur.count(k) && cur[k] == v) sel = i;
            i++;
        }
        row = colstack_add_dropdown(c, body, NULL, k.c_str(), opts.c_str(), sel);
        colstack_set_binding(row, on_tag, own(row, new RowCtx{id, index, k}));
    }

    row = colstack_add_action(c, body, LV_SYMBOL_EDIT, "Convert");
    std::string conv = str(fact, "convert");
    colstack_set_row_value(row, conv.empty() ? "none" : conv.c_str());
    colstack_set_action(row, [](void * ctx) {
        RowCtx * x = static_cast<RowCtx *>(ctx);
        json f = osd_model::get(x->id)["facts"][x->index];
        prompt(PromptKind::Convert, x->id, x->index, "convert", "Convert (x = value, e.g. x / 1000; empty = none)",
               str(f, "convert"));
    }, own(row, new RowCtx{id, index}));

    row = colstack_add_action(c, body, LV_SYMBOL_TRASH, "Remove fact");
    colstack_set_action(row, [](void * ctx) {
        RowCtx * x = static_cast<RowCtx *>(ctx);
        remove_fact(x->id, x->index);
        lv_async_call(pop_once, nullptr);
    }, own(row, new RowCtx{id, index}));
    finish(c);
}

/* ── the fact picker ───────────────────────────────────────────────────────── */

/* Which widget/fact the picker fills in, and the column to return to. */
static struct {
    uint32_t id;
    int index;     /* -1: add a new fact */
    int depth;
} g_pick;

static void pick_done(void *)
{
    while(colstack_depth(cs()) > g_pick.depth) colstack_pop(cs());
}

static void pick(const std::string & name, const Tags & tags)
{
    json w = osd_model::get(g_pick.id);
    if(w.is_null()) return;
    if(!w.contains("facts") || !w["facts"].is_array()) w["facts"] = json::array();
    json & facts = w["facts"];
    std::string type = type_of(w);
    std::string ph = sch::placeholderFor(type_of_fact(name, tags));
    int index = g_pick.index;

    if(index < 0 || index >= (int)facts.size()) {
        json f = {{"name", name}};
        if(!tags.empty()) f["tags"] = tags;
        facts.push_back(f);
        index = (int)facts.size() - 1;
        /* one placeholder per fact: append one that prints this fact's type */
        if(template_follows_facts(type, index) && w.contains("template")) {
            std::string tpl = str(w, "template");
            w["template"] = tpl + (tpl.empty() || tpl.back() == ' ' ? "" : " ") + ph;
        }
    } else {
        json & f = facts[index];
        f["name"] = name;
        if(tags.empty()) f.erase("tags");
        else             f["tags"] = tags;
        /* and keep its placeholder printing the new fact's type */
        if(template_follows_facts(type, index) && w.contains("template")) {
            std::string tpl = str(w, "template");
            auto phs = placeholders(tpl);
            if(index < (int)phs.size() && tpl.substr(phs[index].first, phs[index].second) != ph &&
               !(ph == "%.2f" && tpl[phs[index].first + phs[index].second - 1] == 'f')) {
                tpl.replace(phs[index].first, phs[index].second, ph);
                w["template"] = tpl;
            }
        }
    }
    apply_widget(g_pick.id, w);
    lv_async_call(pick_done, nullptr);
}

static void pick_row(colstack_t * c, lv_obj_t * body, const std::string & label, const std::string & value,
                     const std::string & name, const Tags & tags)
{
    lv_obj_t * row = colstack_add_action(c, body, NULL, label.c_str());
    colstack_set_row_value(row, value.c_str());
    /* name + tags ride in the key, '\n' separated: name\nk=v\nk=v */
    std::string key = name;
    for(auto & [k, v] : tags) key += "\n" + k + "=" + v;
    colstack_set_action(row, [](void * ctx) {
        std::string s = static_cast<RowCtx *>(ctx)->key;
        size_t nl = s.find('\n');
        std::string n = s.substr(0, nl);
        Tags t;
        while(nl != std::string::npos) {
            size_t next = s.find('\n', nl + 1);
            std::string kv = s.substr(nl + 1, next == std::string::npos ? std::string::npos : next - nl - 1);
            size_t eq = kv.find('=');
            if(eq != std::string::npos) t[kv.substr(0, eq)] = kv.substr(eq + 1);
            nl = next;
        }
        pick(n, t);
    }, own(row, new RowCtx{0, -1, key}));
}

static void build_fact_sources(colstack_t * c, lv_obj_t * body, void * user)
{
    std::string name = static_cast<RowCtx *>(user)->key;
    colstack_add_title(c, body, name.c_str());
    for(auto & f : seen_of(name)) pick_row(c, body, tags_text(f.tags), f.value, name, f.tags);
    pick_row(c, body, "any source", "", name, {});
    finish(c);
}

static void build_fact_names(colstack_t * c, lv_obj_t * body, void * user)
{
    std::string category = static_cast<RowCtx *>(user)->key;
    for(auto & name : fact_names(category)) {
        auto sources = seen_of(name);
        const sch::FactInfo * fi = sch::findFact(name);
        std::string desc = fi ? fi->description : "";
        if(sources.size() > 1) {
            /* several sources (antennas, sensors, ...): choose one next */
            RowCtx * rc = new RowCtx{0, -1, name};
            std::string label = short_name(name) + (desc.empty() ? "" : "\n" + desc);
            lv_obj_t * row = colstack_add_submenu(c, body, NULL, label.c_str(), build_fact_sources, rc);
            own(row, rc);
            colstack_set_row_value(row, (std::to_string(sources.size()) + " sources").c_str());
        } else if(sources.size() == 1) {
            std::string tags = tags_text(sources[0].tags);
            pick_row(c, body, short_name(name) + "\n" + (tags.empty() ? desc : tags), sources[0].value, name,
                     sources[0].tags);
        } else {
            pick_row(c, body, short_name(name) + (desc.empty() ? "" : "\n" + desc), "no data", name, {});
        }
    }
    finish(c);
}

static void build_categories(colstack_t * c, lv_obj_t * body, void * user)
{
    RowCtx * owner = static_cast<RowCtx *>(user);
    g_pick.id = owner->id;
    g_pick.index = owner->index;
    g_pick.depth = colstack_depth(c) - 1;   /* back to the column that opened the picker */

    for(auto & cat : sch::categories()) {
        auto names = fact_names(cat);
        if(names.empty()) continue;
        int live = 0;
        for(auto & n : names) live += !seen_of(n).empty();
        RowCtx * rc = new RowCtx{0, -1, cat};
        lv_obj_t * row = colstack_add_submenu(c, body, NULL, cat.c_str(), build_fact_names, rc);
        own(row, rc);
        colstack_set_row_value(row, (std::to_string(live) + " live").c_str());
    }
    finish(c);
}
