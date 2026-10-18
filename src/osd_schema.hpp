#pragma once
/*
 * What the OSD editor knows about the layout JSON: the editable fields of every
 * widget type (so the menu can offer the right control for each), a preset for
 * "Add widget", and a catalog of every fact the code publishes (so facts are
 * picked from a list, never typed). Pure data; the widgets themselves and the
 * validation of a finished widget live in osd.cpp.
 */
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace osd_schema {

using json = nlohmann::ordered_json;

enum class Kind {
	Text,      // free text, on-screen keyboard
	Template,  // text with %d/%u/%f/%s/%b placeholders, one per fact
	Int,       // integer, slider [min, max] in `step`s
	Double,    // decimal, slider [min, max] in `step`s with `precision` decimals
	Number,    // any number, typed on the on-screen keyboard
	Bool,      // switch
	Enum,      // one of `options`
	Icon,      // a PNG from the assets dir
	Color,     // {r, g, b, alpha}, 0..1 each
	Ranges,    // IconSelectorWidget's ranges_and_icons
};

struct Field {
	const char* key;
	const char* label;
	Kind kind;
	double min = 0, max = 0, step = 1;
	int precision = 0;
	const char* options = nullptr;  // Enum: '\n' separated
	double def = 0;                 // value when the key is missing
};

struct WidgetType {
	const char* type;          // JSON "type"
	const char* label;         // shown in the menu
	std::vector<Field> fields;
	int min_facts, max_facts;  // max_facts < 0: unlimited
	const char* preset;        // JSON for "Add widget" (name/x/y are added)
	bool movable;              // false: full-screen effect, x/y unused
	bool unique;               // can't be duplicated (owns a port / shm region)
};

const std::vector<WidgetType>& types();
const WidgetType* findType(const std::string& type);

struct FactInfo {
	std::string name;
	std::string type;          // "BOOL", "INT", "UINT", "DOUBLE", "STRING"
	std::string description;
	std::vector<std::string> tags;  // tag keys the source sets
};

// Every fact published by the code, in menu order.
const std::vector<FactInfo>& catalog();
const FactInfo* findFact(const std::string& name);

// Menu grouping of a fact name ("Video", "WFB-ng link", ...); same order as categories().
std::string categoryOf(const std::string& fact_name);
const std::vector<std::string>& categories();

// Template placeholder printing a fact of `type` ("INT" -> "%d", ...).
std::string placeholderFor(const std::string& type);

}  // namespace osd_schema
