#pragma once
/*
 * Editor access to the running OSD (implemented in osd.cpp, where the widgets
 * live). The OSD layout JSON is the single source of truth: every edit goes into
 * it and the affected widget is rebuilt from it; save() writes it back to the
 * --osd-config file.
 *
 * Everything here must run on the LVGL thread with the OSD lock held, as menu
 * callbacks do; never call it from a worker thread.
 */
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../lvgl/lvgl.h"

namespace osd_model {

using json = nlohmann::ordered_json;

struct WidgetInfo {
	uint32_t id;        // stable for the widget's lifetime (indexes shift on edits)
	std::string name;
	std::string type;   // without the "---" prefix of a disabled widget
	std::string error;  // why the widget could not be built ("" = fine)
	bool enabled;       // false: type is prefixed with "---" in the file
};

// A fact the OSD has received since startup (its latest value).
struct SeenFact {
	std::string name;
	std::map<std::string, std::string> tags;
	std::string type;   // "BOOL", "INT", "UINT", "DOUBLE", "STRING"
	std::string value;
};

// Widgets in draw order (bottom-most first).
std::vector<WidgetInfo> list();
int index(uint32_t id);                       // -1 if gone
json get(uint32_t id);                        // the widget's JSON (null if gone)
// Replace a widget's JSON and rebuild it; returns the build error ("" = ok).
std::string set(uint32_t id, const json& widget);
uint32_t insert(const json& widget, int at = -1);  // at < 0: on top; returns the id
void remove(uint32_t id);
int moveZ(uint32_t id, int delta);            // move in draw order; returns the new index

// Moving: draw the widget shifted by (dx, dy) from its configured position, without
// a rebuild. commitOffset() writes the shifted position into x/y.
void setOffset(uint32_t id, int dx, int dy);
void getOffset(uint32_t id, int& dx, int& dy);
void commitOffset(uint32_t id);
// Re-express x/y from the other screen edge, keeping the widget where it is.
// Negative x/y count from the right/bottom edge, so they follow the screen size.
void setAnchor(uint32_t id, bool right, bool bottom);
// Screen extent of what the widget currently draws (incl. offset); false if
// nothing is visible (e.g. a hidden DVR indicator).
bool bbox(uint32_t id, lv_area_t& out);
// The configured x/y on screen (incl. offset).
void anchorPoint(uint32_t id, int& x, int& y);
// Make the widget show something even without data (DVR indicator, icons,
// pop-ups), for placing it; 0 = no widget.
void setPreview(uint32_t id);

void screen(int& w, int& h);
std::vector<SeenFact> seen();
std::string assetsDir();
std::string configPath();
bool dirty();                                 // edited since load/save
std::string save();                           // write to configPath(); "" = ok
std::string reload();                         // discard edits, re-read the file; "" = ok
// Check a 'convert' expression; "" = ok, else the parse error.
std::string checkExpression(const std::string& expr);

}  // namespace osd_model
