
extern "C" {
#include "drm.h"
#include "mavlink.h"
#include "icons/icons.h"
}
#include "osd.h"
#include "osd.hpp"

#include <pthread.h>
#include <map>
#include <vector>
#include <ranges>
#include <memory>
#include <variant>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <cstdlib> //KILLME
#include <string>
#include <filesystem>
#include <cairo.h>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"
#include <fmt/ranges.h>

#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

#define PATH_MAX	4096

using json = nlohmann::json;

struct osd_vars osd_vars;
int enable_osd = 0;

extern uint32_t frames_received;
uint32_t stats_rx_bytes = 0;
struct timespec last_timestamp = {0, 0};
float rx_rate = 0;
int hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
char custom_msg[80];
u_int custom_msg_refresh_count = 0;
extern pthread_mutex_t video_mutex;
extern pthread_cond_t video_cond;
bool osd_update_ready = false;


double getTimeInterval(struct timespec* timestamp, struct timespec* last_meansure_timestamp) {
  return (timestamp->tv_sec - last_meansure_timestamp->tv_sec) +
       (timestamp->tv_nsec - last_meansure_timestamp->tv_nsec) / 1000000000.;
}

//
// Facts
//

typedef std::map<std::string, std::string> FactTags;


class FactMatcher {
public:
	FactMatcher(std::string name, FactTags tags): name(name), tags(tags) {};
	FactMatcher(std::string name): name(name), tags({}) {};
	std::string name;
	FactTags tags;
};


class FactMeta {
public:
	FactMeta(): name(""), tags({}) {};
	FactMeta(std::string name): name(name), tags({}) {};
	FactMeta(std::string name, FactTags tags): name(name), tags(tags) {};


	std::string getName() { return name; }
	FactTags getTags() { return tags; }

	/**
	 * Returns true if names are equal and all match_tags are defined and have equal value
	 */
	bool match(FactMatcher matcher) {
		if(matcher.name != name) return false;
		for (const auto& [key, match_value] : matcher.tags) {
			if (auto value = tags.find(key); value != tags.end()) {
				if (value->second != match_value) return false;
			} else {
				return false;
			}
		}
		return true;
	}

private:
	std::string name;
	FactTags tags;
};


class Fact {
public:
	enum Type {
		T_UNDEF,
		T_BOOL,
		T_INT,
		T_UINT,
		T_DOUBLE,
		T_STRING
	};

	Fact(): meta(FactMeta("", {})), type(T_UNDEF) {};
	Fact(FactMeta meta, bool val): meta(meta), value(val), type(T_BOOL) {};
	Fact(FactMeta meta, long val): meta(meta), value(val), type(T_INT) {};
	Fact(FactMeta meta, ulong val): meta(meta), value(val), type(T_UINT) {};
	Fact(FactMeta meta, double val): meta(meta), value(val), type(T_DOUBLE) {};
	Fact(FactMeta meta, std::string val): meta(meta), value(val), type(T_STRING) {};

	bool isDefined() {
		return type != T_UNDEF;
	}

	// TODO: try to cast instead of crash
	bool getBoolValue() {
		assertType(T_BOOL);
		return std::get<bool>(value);
	}

	long getIntValue() {
		assertType(T_INT);
		return std::get<long>(value);
	}

	ulong getUintValue() {
		assertType(T_UINT);
		return std::get<ulong>(value);
	}

	double getDoubleValue() {
		assertType(T_DOUBLE);
		return std::get<double>(value);
	}

	std::string getStrValue() {
		assertType(T_STRING);
		return std::get<std::string>(value);
	}

	bool matches(FactMatcher matcher) {
		return meta.match(matcher);
	}

	std::string getTypeName() {
		return typeName(type);
	}

	Type getType() {
		return type;
	}

	std::string getName() {
		return meta.getName();
	}

	FactTags getTags() {
		return meta.getTags();
	}

	std::string asString() {
		switch(type) {
		case T_UNDEF:
			return "(undefined)";
		case T_BOOL:
			if (getBoolValue()) {
				return "true";
			} else {
				return "false";
			};
		case T_INT:
			return std::to_string(getIntValue());
		case T_UINT:
			return std::to_string(getUintValue());
		case T_DOUBLE:
			return std::to_string(getDoubleValue());
		case T_STRING:
			return getStrValue();
		}
		return "(unknown)";
	}
	
private:
	Type type = T_UNDEF;
	std::string typeName(Type t) {
		switch(t) {
		case T_UNDEF:
			return "UNDEF";
		case T_BOOL:
			return "BOOL";
		case T_INT:
			return "INT";
		case T_UINT:
			return "UINT";
		case T_DOUBLE:
			return "DOUBLE";
		case T_STRING:
			return "STRING";
		}
		return "UNKNOWN";
	}

	void assertType(Type t) {
		if (t != type) {
			spdlog::error("'{}': requested type of {}, but the actual type is {}",
						  meta.getName(), typeName(t), typeName(type));
			assert(type == t);
		}
	}
	FactMeta meta;
	// TODO: timestamp
	std::variant<
		bool,
		long,
		ulong,
		double,
		std::string
		> value;
};



struct Bucket {
	long long timestamp;
	long sum;
	int count;
	long min_value;
	long max_value;

	Bucket(long long ts, long value)
		: timestamp(ts), sum(value), count(1), min_value(value), max_value(value) {}
};

// Struct to hold extended statistics
struct Stats {
	long min;
	long max;
	double average;
	long sum;
	int count;

	Stats(long min_value, long max_value, double avg, long total_sum, int total_count)
		: min(min_value), max(max_value), average(avg), sum(total_sum), count(total_count) {}
};

/**
 * Calculates the running average/rate-per-second/min/max over a sliding time window.
 * @param window_size_ms the size of the sliding window in milliseconds
 * @param bucket_size_ms the size of the bucket; structure uses amount of memory
 *		  of O(window_size_ms / bucket_size_ms), however large bucket size decreases the precision.
 * NOTE: the code was mostly generated by ChatGPT
 */
class RunningAverage {
public:
	RunningAverage(int window_size_ms, int bucket_size_ms)
		: window_size(window_size_ms), bucket_size(bucket_size_ms), sum(0), count(0) {
		assert(window_size_ms >= bucket_size_ms);
	}

	long add(long value) {
		auto now = std::chrono::steady_clock::now();
		auto current_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

		// Remove outdated buckets
		while (!buckets.empty() && (current_time - buckets.front().timestamp > window_size)) {
			sum -= buckets.front().sum;
			count -= buckets.front().count;
			buckets.pop_front();
		}

		// Add the value to the current bucket
		if (!buckets.empty() && (current_time - buckets.back().timestamp < bucket_size)) {
			buckets.back().sum += value;
			buckets.back().count += 1;
			buckets.back().min_value = std::min(buckets.back().min_value, value);
			buckets.back().max_value = std::max(buckets.back().max_value, value);
		} else {
			buckets.emplace_back(current_time, value);
		}

		// Update the running sum and count
		sum += value;
		count++;

		return count > 0 ? sum / count : 0;
	}

	double average_over_last_ms(uint last_ms) const {
		long min = std::numeric_limits<long>::max();
		long max = std::numeric_limits<long>::min();
		long last_sum;
		int last_count;
		calculate_stats_in_window(last_ms, last_sum, last_count, min, max);

		return last_count > 0 ? static_cast<double>(last_sum) / last_count : 0.0;
	}

	double rate_per_second_over_last_ms(uint last_ms) const {
		long min = std::numeric_limits<long>::max();
		long max = std::numeric_limits<long>::min();
		long last_sum;
		int last_count;
		calculate_stats_in_window(last_ms, last_sum, last_count, min, max);

		double elapsed_seconds = static_cast<double>(last_ms) / 1000.0;
		return elapsed_seconds > 0 ? static_cast<double>(last_sum) / elapsed_seconds : 0.0;
	}

	void get_stats_over_last_ms(uint last_ms, long& min, long& max, double& average) const {
		long last_sum;
		int last_count;

		min = std::numeric_limits<long>::max();
		max = std::numeric_limits<long>::min();

		calculate_stats_in_window(last_ms, last_sum, last_count, min, max);

		average = last_count > 0 ? static_cast<double>(last_sum) / last_count : 0.0;
	}

	// New method to return Stats struct with sum and count
	Stats get_stats_over_last_ms_result(uint last_ms) const {
		long min = std::numeric_limits<long>::max();
		long max = std::numeric_limits<long>::min();
		long last_sum = 0;
		int last_count = 0;

		calculate_stats_in_window(last_ms, last_sum, last_count, min, max);

		double average = last_count > 0 ? static_cast<double>(last_sum) / last_count : 0.0;
		return Stats(min, max, average, last_sum, last_count);
	}

	std::vector<long> get_bucket_sums() const {
		std::vector<long> sums;
		sums.reserve(buckets.size());
		for (const auto& bucket : buckets) {
			sums.push_back(bucket.sum);
		}
		return sums;
	}

	std::vector<Stats> get_bucket_stats() const {
		std::vector<Stats> stats;
		stats.reserve(buckets.size());
		for (const auto& bucket : buckets) {
			double average = bucket.count > 0 ? static_cast<double>(bucket.sum) / bucket.count : 0.0;
			stats.push_back(Stats(bucket.min_value, bucket.max_value, average, bucket.sum, bucket.count));
		}
		return stats;
	}

private:
	void calculate_stats_in_window(uint last_ms, long& sum_out, int& count_out, long& min_out, long& max_out) const {
		auto now = std::chrono::steady_clock::now();
		auto current_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

		sum_out = 0;
		count_out = 0;

		for (auto it = buckets.rbegin(); it != buckets.rend(); ++it) {
			if (current_time - it->timestamp <= last_ms) {
				sum_out += it->sum;
				count_out += it->count;
				min_out = std::min(min_out, it->min_value);
				max_out = std::max(max_out, it->max_value);
			} else {
				break;	// Exit loop once we're outside the time window
			}
		}
	}

	int window_size;
	int bucket_size;
	std::deque<Bucket> buckets;
	long sum;
	int count;
};

//
// Widgets
//

class Widget {
public:
	Widget(int pos_x, int pos_y): pos_x(pos_x), pos_y(pos_y) {};
	Widget(int pos_x, int pos_y, uint num_args): pos_x(pos_x), pos_y(pos_y) {
		for (auto i=0; i < num_args; i++) {
			args.push_back(Fact());
		}
	};

	virtual void draw(cairo_t *cr) {};

	virtual void setFact(uint idx, Fact fact) {
		args[idx] = fact;
	}

	int x(cairo_t *cr) {
		cairo_surface_t *target = cairo_get_target(cr);
		int w = cairo_image_surface_get_width(target);
		//int h = cairo_image_surface_get_height(target);
		return (w + pos_x) % w;
	}
	int y(cairo_t *cr) {
		cairo_surface_t *target = cairo_get_target(cr);
		//int w = cairo_image_surface_get_width(target);
		int h = cairo_image_surface_get_height(target);
		return (h + pos_y) % h;
	}
	std::pair<int, int> xy(cairo_t *cr) {
		cairo_surface_t *target = cairo_get_target(cr);
		int w = cairo_image_surface_get_width(target);
		int h = cairo_image_surface_get_height(target);
		return std::pair((w + pos_x) % w, (h + pos_y) % h);
	}

protected:
	int pos_x, pos_y;
	std::vector<Fact> args;
};


class TextWidget: public Widget {
public:
	TextWidget(int pos_x, int pos_y, std::string text): Widget(pos_x, pos_y), text(text) {};

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, text.c_str());
	}
protected:
	std::string text;
};


class IconTextWidget: public Widget {
public:
	IconTextWidget(int pos_x, int pos_y, cairo_surface_t *icon, std::string text):
		Widget(pos_x, pos_y), text(text), icon(icon) {};

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		cairo_set_source_surface(cr, icon, x, y - 20);
		cairo_paint(cr);
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x + 40, y);
		cairo_show_text(cr, text.c_str());
	}

protected:
	std::string text;
	cairo_surface_t *icon;
};


class TplTextWidget: public Widget {
public:
	TplTextWidget(int pos_x, int pos_y, std::string tpl, uint num_args):
		Widget(pos_x, pos_y, num_args), tpl(tpl), num_args(num_args) {};

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		std::unique_ptr<std::string> msg = render_tpl();
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, msg->c_str());
	}

protected:
	std::unique_ptr<std::string> render_tpl() {
		return render_tpl(tpl, args);
	}
	std::unique_ptr<std::string> render_tpl(std::string tpl, std::vector<Fact> args) {
		bool at_placeholder = false;
		int fact_i = 0;
		Fact *fact;
		std::unique_ptr<std::string> msg(new std::string);
		for(char& c : tpl) {
			if (c == '%') {
				at_placeholder = true;
			} else if (!at_placeholder) {
				msg->push_back(c);
			} else if (at_placeholder && c == '%') {
				msg->push_back('%');
				at_placeholder = false;
			} else if (at_placeholder) {
				at_placeholder = false;
				fact = &args[fact_i];
				if (!fact->isDefined()) {
					msg->push_back('?');
					fact_i++;
					continue;
				}
				switch (c) {
				case 'b':
					{
						msg->push_back(fact->getBoolValue() ? 't' : 'f');
						break;
					}
				case 'd':
				case 'i':
					{
						msg->append(std::to_string(fact->getIntValue()));
						break;
					}
				case 'u':
					{
						msg->append(std::to_string(fact->getUintValue()));
						break;
					}
				case 'f':
					{
						char buf[32];
						std::snprintf(buf, sizeof(buf), "%.2f", fact->getDoubleValue());
						msg->append(std::string(buf));
						break;
					}
				case 's':
					{
						msg->append(fact->getStrValue());
						break;
					}
				default:
					{
						msg->push_back('?');
					}
				}
				fact_i++;
			}
		}
		return msg;
	}

protected:
	std::string tpl;
	uint num_args;
};


class IconTplTextWidget: public TplTextWidget {
public:
	IconTplTextWidget(int pos_x, int pos_y, cairo_surface_t *icon, std::string tpl, uint num_args):
		TplTextWidget(pos_x, pos_y, tpl, num_args), icon(icon) {};

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		std::unique_ptr<std::string> msg = render_tpl();
		cairo_set_source_surface(cr, icon, x, y - 20);
		cairo_paint(cr);
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x + 40, y);
		cairo_show_text(cr, msg->c_str());
	}

protected:
	cairo_surface_t *icon;
};

class BoxWidget: public Widget {
public:
	BoxWidget(int pos_x, int pos_y, uint w, uint h, double r, double g, double b, double a):
		Widget(pos_x, pos_y), w(w), h(h), r(r), g(g), b(b), a(a) {};

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		cairo_set_source_rgba(cr, r, g, b, a);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
	}

private:
	uint w, h;
	double r, g, b, a;
};

class BarChartWidget: public Widget {
public:
	enum StatsField {
		STATS_MIN,
		STATS_MAX,
		STATS_SUM,
		STATS_COUNT,
		STATS_AVG
	};

	BarChartWidget(int pos_x, int pos_y, uint w, uint h, uint window_s, uint num_buckets, BarChartWidget::StatsField stats_field):
		Widget(pos_x, pos_y, 0), w(w), h(h), window_ms(window_s * 1000), num_buckets(num_buckets), stats_field(stats_field),
		stats(window_s * 1000, window_s * 1000 / num_buckets) {};

	virtual void setFact(uint idx, Fact fact) {
		assert(idx == 0);
		switch (fact.getType()) {
		case Fact::T_INT:
			stats.add(fact.getIntValue());
			break;
		case Fact::T_UINT:
			stats.add(static_cast<long>(fact.getUintValue()));
		}
	}

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		// box
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);

		std::vector<Stats> all_stats = stats.get_bucket_stats();
		if (all_stats.size() < 3) {
			SPDLOG_DEBUG("Can't draw bar chart - too few values");
			return;
		}
		all_stats.pop_back(); // drop last bucket, because it is usually still not full
		std::vector<double> stats = select_stats(all_stats);
		double min = *std::min_element(stats.begin(), stats.end());
		double max = *std::max_element(stats.begin(), stats.end());

		// legend
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x + 2, y + 15);
		cairo_show_text(cr, shorten(max).c_str());

		cairo_move_to(cr, x + 2, y + h);
		cairo_show_text(cr, shorten(min).c_str());

		// bars
		cairo_set_source_rgba(cr, 200.0, 200.0, 200.0, 0.8);

		double scale = max - min;
		SPDLOG_TRACE("Scale: {}, min {}, max {}", scale, min, max);
		uint legend_w = 65;
		uint chart_w = w - legend_w;

		uint bar_pad = 4;
		uint bar_w = (chart_w - (bar_pad * num_buckets)) / num_buckets;
		uint bar_x = x + legend_w;
		SPDLOG_TRACE(
					 "chart_w {} bar_w {}, bar_x {}",
					 chart_w, bar_w, bar_x
                    );
		for (auto val : stats) {
			double normalized = val - min;
			double bar_h = -1.0 * (normalized * (h - 10)) / scale;
			// h -> max-min
			// ? -> normalized
			SPDLOG_TRACE("val {}, cairo_rectangle(cr, {}, {}, {}, {})",
						 val, bar_x, y + h, bar_w, bar_h);
			cairo_rectangle(cr, bar_x, y + h, bar_w, bar_h - 2);
			cairo_fill(cr);
			bar_x += (bar_pad + bar_w);
		}
	}

private:
	/**
	 * function that takes ulong and returns string with short form of the number:
	 * up to 3 digits and "giga" / "mega" / "kilo" suffix
	 * made by ChatGPT
	 */
	std::string shorten(long num) {
		double value = num;
		std::string suffix;

		if (num >= 1'000'000'000) {  // Giga
			value = num / 1'000'000'000.0;
			suffix = "G";
		} else if (num >= 1'000'000) {  // Mega
			value = num / 1'000'000.0;
			suffix = "M";
		} else if (num >= 1'000) {  // Kilo
			value = num / 1'000.0;
			suffix = "K";
		} else {
			suffix = "";  // No suffix needed
		}

		// Format to 3 significant digits
		std::ostringstream oss;
		oss << std::fixed << std::setprecision(3 - static_cast<int>(std::log10(value) + 1)) << value;
		return oss.str() + " " + suffix;
	}
	std::vector<double> select_stats(std::vector<Stats> stats) {
		std::vector<double> res;
		res.reserve(stats.size());
		for (auto stat : stats) {
			switch(stats_field) {
			case STATS_MIN:
				res.push_back(static_cast<double>(stat.min));
				break;
			case STATS_MAX:
				res.push_back(static_cast<double>(stat.max));
				break;
			case STATS_SUM:
				res.push_back(static_cast<double>(stat.sum));
				break;
			case STATS_COUNT:
				res.push_back(static_cast<double>(stat.count));
				break;
			case STATS_AVG:
				res.push_back(stat.average);
				break;
			}
		}
		return res;
	}
	uint w, h;
	uint window_ms, num_buckets;
	StatsField stats_field = STATS_SUM;
	RunningAverage stats;
};

/**
 * Displays text facts for a period of time, stacking them one after another; fading-out opacity.
 * Convenient for warnings, custom messages and pop-ups.
 *
 * @param timeout_ms stop displaying the fact after this many milliseconds since it was received
 */
class PopupWidget: public Widget {
public:
	PopupWidget(int pos_x, int pos_y, uint timeout_ms, uint num_args) :
		Widget(pos_x, pos_y, num_args), timeout(timeout_ms) {};

	virtual void setFact(uint _idx, Fact fact) {
		auto now = std::chrono::steady_clock::now();
		std::string msg = fact.getStrValue();
		msgs.push_back(std::pair(now, msg));
	}

	void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		auto now = std::chrono::steady_clock::now();

		// Remove outdated messages
		while (!msgs.empty() && (now - msgs.front().first > timeout)) {
			msgs.pop_front();
		}
		uint y_offset = y;
		for (auto [time, msg] : msgs) {
			auto past = std::chrono::duration_cast<std::chrono::milliseconds>(now - time);
			double fade_fraction = 1.0 - static_cast<double>(past.count()) / static_cast<double>(timeout.count());

			cairo_text_extents_t extents;
			cairo_text_extents(cr, msg.c_str(), &extents);

			// Draw popup box
			double padding = 5.0;
			cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, fade_fraction / 3.0);
			cairo_rectangle(cr,
							x - padding,
							y_offset + padding,
							extents.width + (padding * 2), -(extents.height + (padding * 2)));
			cairo_fill(cr);

			// Draw popup text
			cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, fade_fraction);
			cairo_move_to(cr, x, y_offset);
			cairo_show_text(cr, msg.c_str());
			y_offset += extents.height + (padding * 2) + 2;
		}
	}

private:
	std::deque<std::pair<
				   std::chrono::time_point<std::chrono::steady_clock>,
				   std::string
				   >> msgs;
	std::chrono::milliseconds timeout;
};

//
// Specific widgets
//

class DvrStatusWidget: public IconTextWidget {
public:
	DvrStatusWidget(int pos_x, int pos_y, cairo_surface_t *icon, std::string text) :
		IconTextWidget(pos_x, pos_y, icon, text) {
		args.push_back(Fact());
	};

	void draw(cairo_t *cr) {
		if(args[0].isDefined() && args[0].getBoolValue()) {
			auto [x, y] = xy(cr);
			cairo_save(cr);
			cairo_set_source_surface(cr, icon, x, y - 20);
			cairo_paint(cr);
			cairo_set_source_rgba(cr, 255.0, 0.0, 0.0, 1);
			cairo_move_to(cr, x + 40, y);
			cairo_show_text(cr, text.c_str());
			cairo_restore(cr);
		}
	}
};

class VideoWidget: public IconTplTextWidget {
public:
  VideoWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
              cairo_surface_t *icon, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, icon, tpl, num_args),
		fps(window_size_ms, bucket_size_ms) {};

	virtual void setFact(uint idx, Fact fact) {
		if (idx == 0) {
			// replace the value with its increment rate per-second
			ulong num_frames = fact.getUintValue(); // should be always '1'
			fps.add(num_frames);
			args[idx] = Fact(FactMeta("video_fps"), (ulong)fps.rate_per_second_over_last_ms(1000));
		} else {
			args[idx] = fact;
		}
	}

private:
	RunningAverage fps;
};

class VideoBitrateWidget: public IconTplTextWidget {
public:
  VideoBitrateWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
					 cairo_surface_t *icon, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, icon, tpl, num_args),
		bps(window_size_ms, bucket_size_ms) {
	  assert(num_args == 1);
  };

	virtual void setFact(uint idx, Fact fact) {
		assert(idx == 0);
		// replace the value with its increment rate per-second
		ulong num_bytes = fact.getUintValue();
		bps.add(num_bytes);
		// 125000 is 1_000_000 / 8 (megabits, not megabytes)
		args[idx] = Fact(FactMeta("video_mbps"), bps.rate_per_second_over_last_ms(1000) / 125000.0);
	}

private:
	RunningAverage bps;
};

class VideoDecodeLatencyWidget: public IconTplTextWidget {
public:
  VideoDecodeLatencyWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
					 cairo_surface_t *icon, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, icon, tpl, 3),  // 3 args, because we calculate min/max/avg
		timing(window_size_ms, bucket_size_ms) {
	  assert(num_args == 1);
  };

	virtual void setFact(uint idx, Fact fact) {
		assert(idx == 0);
		ulong decode_time = fact.getUintValue();
		timing.add(decode_time);
		Stats stats = timing.get_stats_over_last_ms_result(1000);
		args[0] = Fact(FactMeta("video_avg"), stats.average);
		args[1] = Fact(FactMeta("video_min"), stats.min);
		args[2] = Fact(FactMeta("video_max"), stats.max);
	}

private:
	RunningAverage timing;
};


class GPSWidget: public Widget {
public:
	GPSWidget(int pos_x, int pos_y, uint num_args) :
		Widget(pos_x, pos_y, num_args) {
		assert(num_args == 3);
	};

	void draw(cairo_t *cr) {
		if( !(args[0].isDefined() && args[1].isDefined() && args[2].isDefined()) ) return;
		auto [x, y] = xy(cr);
		std::string fix_type = "undef";
		char buf[64];
		switch (args[0].getUintValue()) {
		case 0:
			fix_type = "no GPS";
			break;
		case 1:
			fix_type = "no fix";
			break;
		case 2:
			fix_type = "2D fix";
			break;
		case 3:
			fix_type = "3D fix";
			break;
		case 4:
			fix_type = "DGPS/SBAS 3D";
			break;
		case 5:
			fix_type = "RTK float 3D";
			break;
		case 6:
			fix_type = "RTK Fixed 3D";
			break;
		case 7:
			fix_type = "Static fixed";
			break;
		case 8:
			fix_type = "PPP 3D";
			break;
		}
		double lat = args[1].getIntValue() * 1.0e-7;
		double lon = args[2].getIntValue() * 1.0e-7;
		snprintf(buf, sizeof(buf), "%s Lat:%f, Lon:%f", fix_type.c_str(), lat, lon);
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, buf);
	}
};


class DebugWidget: public Widget {
public:
	DebugWidget(int pos_x, int pos_y, uint num_args) :
		Widget(pos_x, pos_y, num_args) {};

	void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		auto y_offset = y;
		for (Fact &fact : args) {
			std::ostringstream oss;
			if (!fact.isDefined()) {
				oss << "undef";
			} else {
				oss << fact.getName() << " (" << fact.getTypeName() << ") {";
				for (const auto &tag : fact.getTags()) {
					oss << tag.first << "=>" << tag.second << ", ";
				}
				oss << "} = " << fact.asString();
			}
			std::string text =  oss.str();
			cairo_set_source_rgba(cr, 255.0, 50.0, 50.0, 1);
			cairo_move_to(cr, x, y_offset);
			cairo_show_text(cr, text.c_str());
			y_offset += 20;
			SPDLOG_INFO("dbg draw {}", text);
		}
	}
};

class ExternalSurfaceWidget: public Widget {
public:
	ExternalSurfaceWidget(int pos_x, int pos_y, std::string shm_name ): Widget(pos_x, pos_y), shm_name(shm_name)  {};

	virtual void init_shm(cairo_t *cr) {
		SPDLOG_INFO("creating shm region {}", shm_name);

		cairo_surface_t *target = cairo_get_target(cr);
		int width = cairo_image_surface_get_width(target);
		int height = cairo_image_surface_get_height(target);

		// Calculate total shared memory size
		shm_size = sizeof(SharedMemoryRegion) + (width * height * 4); // Metadata + Image data

		// Create shared memory region
		int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
		if (shm_fd == -1) {
			perror("Failed to create shared memory");
			return;
		}

		if (ftruncate(shm_fd, shm_size) == -1) {
			perror("Failed to set shared memory size");
			shm_unlink(shm_name.c_str());
			return;
		}

		// Map shared memory to process address space
		auto *shm_region = static_cast<SharedMemoryRegion*>(
			mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
		);
		if (shm_region == MAP_FAILED) {
			perror("Failed to map shared memory");
			shm_unlink(shm_name.c_str());
			return;
		}

		// Write metadata
		shm_region->width = width;
		shm_region->height = height;

		// Create Cairo surface for the image data
		shm_surface = cairo_image_surface_create_for_data(
			shm_region->data, CAIRO_FORMAT_ARGB32, width, height, width * 4
		);

		// Store pointer for cleanup
		shm_data = reinterpret_cast<unsigned char*>(shm_region);
	}


	virtual void draw(cairo_t *cr) {

		if (! shm_surface) 
			init_shm(cr);
		auto [x, y] = xy(cr);
		cairo_set_source_surface(cr, shm_surface, x, y); // Position at (0, 0)
    	cairo_paint(cr); // Paint shm_surface onto base_surface
	}

	~ExternalSurfaceWidget() {
		SPDLOG_INFO("bye, bye, shm region {}", shm_name);
		if (shm_surface) {
			cairo_surface_destroy(shm_surface);
		}
		if (shm_data) {
			munmap(shm_data, shm_size);
		}
		shm_unlink(shm_name.c_str());
	}

protected:
	cairo_surface_t *shm_surface = nullptr;
	unsigned char *shm_data = nullptr;
	size_t shm_size;
	std::string shm_name;
};

class Osd {
public:
	void loadConfig(json cfg) {
		json obj;
		if (!cfg.contains("format")) {
			spdlog::error("OSD config doesn't have 'format' key");
			return;
		}
		if (!cfg.contains("widgets")) {
			//|| cfg["widgets"].type() != json::value_t::array)
			spdlog::error("OSD config doesn't have 'widgets' key");
			return;
		}
		std::filesystem::path assets_dir(".");
		if (cfg.contains("assets_dir")) {
			assets_dir = cfg.at("assets_dir").template get<std::filesystem::path>();
		}
		json widgets_j = cfg.at("widgets");
		for (json widget_j : widgets_j) {
			if(!(widget_j.contains("name") || widget_j.contains("type") || widget_j.contains("x") ||
				 widget_j.contains("y") || widget_j.contains("facts"))) {
				spdlog::error("Missing required key name/type/x/y/facts");
				return;
			}
			auto name = widget_j.at("name").template get<std::string>();
			auto type = widget_j.at("type").template get<std::string>();
			auto x = widget_j.at("x").template get<int>();
			auto y = widget_j.at("y").template get<int>();
			std::vector<FactMatcher> matchers;
			for(json matcher_j : widget_j.at("facts")) {
				auto matcher_name = matcher_j.at("name").template get<std::string>();
				FactTags tags;
				if (matcher_j.contains("tags")) {
					for (auto& [key, value] : matcher_j.at("tags").items()) {
						tags.insert({key, value});
					}
				}
				matchers.push_back(FactMatcher(matcher_name, tags));
			}
			if (type == "TextWidget") {
				addWidget(new TextWidget(x, y, widget_j.at("text").template get<std::string>()),
						  matchers);
			}
			else if (type == "ExternalSurfaceWidget") {
				addWidget(new ExternalSurfaceWidget(x, y, name), matchers);
			} else if (type == "TplTextWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				addWidget(new TplTextWidget(x, y, tpl, (uint)matchers.size()), matchers);
			} else if(type == "IconTplTextWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
				if (icon == NULL) break;
				addWidget(new IconTplTextWidget(x, y, icon, tpl, (uint)matchers.size()), matchers);
			} else if(type == "DvrStatusWidget") {
				auto text = widget_j.at("text").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
				if (icon == NULL) break;
				addWidget(new DvrStatusWidget(x, y, icon, text), matchers);
			} else if(type == "VideoWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();;
				cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
				if (icon == NULL) break;
				addWidget(new VideoWidget(x, y, window_size_s * 1000, bucket_size_ms,
										  icon, tpl, (uint)matchers.size()),
						  matchers);
			} else if(type == "VideoBitrateWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();;
				cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
				if (icon == NULL) break;
				addWidget(new VideoBitrateWidget(x, y, window_size_s * 1000, bucket_size_ms,
												 icon, tpl, (uint)matchers.size()),
						  matchers);
			} else if(type == "VideoDecodeLatencyWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();;
				cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
				if (icon == NULL) break;
				addWidget(new VideoDecodeLatencyWidget(x, y, window_size_s * 1000, bucket_size_ms,
													   icon, tpl, 1),
						  matchers);
			} else if(type == "BoxWidget") {
				auto width = widget_j.at("width").template get<uint>();
				auto height = widget_j.at("height").template get<uint>();
				json color_j = widget_j.at("color");
				auto r = color_j.at("r").template get<double>();
				auto g = color_j.at("g").template get<double>();
				auto b = color_j.at("b").template get<double>();
				auto a = color_j.at("alpha").template get<double>();
				addWidget(new BoxWidget(x, y, width, height, r, g, b, a), matchers);
			} else if(type == "BarChartWidget") {
				auto width = widget_j.at("width").template get<uint>();
				auto height = widget_j.at("height").template get<uint>();
				auto window_s = widget_j.at("window_s").template get<uint>();
				auto num_buckets = widget_j.at("num_buckets").template get<uint>();
				auto stats_kind_str = widget_j.at("stats_kind").template get<std::string>();
				BarChartWidget::StatsField stats_kind;
				if (stats_kind_str == "sum") {
					stats_kind = BarChartWidget::STATS_SUM;
				} else if (stats_kind_str == "min") {
					stats_kind = BarChartWidget::STATS_MIN;
				} else if (stats_kind_str == "max") {
					stats_kind = BarChartWidget::STATS_MAX;
				} else if (stats_kind_str == "count") {
					stats_kind = BarChartWidget::STATS_COUNT;
				} else if (stats_kind_str == "avg") {
					stats_kind = BarChartWidget::STATS_AVG;
				} else {
					SPDLOG_WARN("{}: invalid stats_kind {}", name, stats_kind_str);
					break;
				}
				addWidget(new BarChartWidget(x, y, width, height, window_s, num_buckets, stats_kind),
						  matchers);
			} else if (type == "GPSWidget") {
				addWidget(new GPSWidget(x, y, (uint)matchers.size()), matchers);
			} else if(type == "PopupWidget") {
				auto timeout_ms = widget_j.at("timeout_ms").template get<uint>();
				addWidget(new PopupWidget(x, y, timeout_ms, (uint)matchers.size()),
						  matchers);
			} else if(type == "DebugWidget") {
				addWidget(new DebugWidget(x, y, (uint)matchers.size()), matchers);
			} else {
				spdlog::warn("Widget '{}': unknown type: {}", name, type);
			}
		}
	}

	Osd *addWidget(Widget *widget, std::vector<FactMatcher> param_matchers) {
		uint arg_idx = 0;
		widgets.push_back(widget);
		for (auto matcher : param_matchers) {
			matchers.push_back(std::make_tuple(matcher, widget, arg_idx));
			arg_idx++;
		}
		return this;
	};

	void draw(cairo_t *cr) {
		for(auto &widget : widgets)
			widget->draw(cr);
	};

	void setFact(Fact fact) {
		for (auto [matcher, widget, arg_idx] : matchers) {
			if (fact.matches(matcher)) {
				widget->setFact(arg_idx, fact);
			}
		}
	};

private:

	cairo_surface_t *openIcon(std::string widget_name, std::filesystem::path base_path,
							  std::filesystem::path icon_path) {
		if (icon_path.is_relative()) {
			icon_path = base_path / icon_path;
		}
		cairo_surface_t *icon = cairo_image_surface_create_from_png(icon_path.c_str());
		if (cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS) {
			std::string status("OTHER_ERROR");
			switch (cairo_surface_status(icon)) {
			case CAIRO_STATUS_NULL_POINTER:
				status = "NULL_POINTER";
				break;
			case CAIRO_STATUS_NO_MEMORY:
				status = "NO_MEMORY";
				break;
			case CAIRO_STATUS_READ_ERROR:
				status = "READ_ERROR";
				break;
			case CAIRO_STATUS_INVALID_CONTENT:
				status = "INVALID_CONTENT";
				break;
			case CAIRO_STATUS_INVALID_FORMAT:
				status = "INVALID_FORMAT";
				break;
			case CAIRO_STATUS_INVALID_VISUAL:
				status = "INVALID_VISUAL";
				break;
			};
			spdlog::error("Widget '{}': Can't open icon '{}': {}",
						  widget_name, icon_path.string(), status);
			return NULL;
		}
		return icon;
	}

	std::vector<Widget *> widgets;
	std::vector<std::tuple<FactMatcher, Widget *, uint>> matchers;
};


std::queue<Fact> fact_queue;
std::mutex mtx;
std::condition_variable cv;


cairo_surface_t *fps_icon;
cairo_surface_t *lat_icon;
cairo_surface_t* net_icon;
cairo_surface_t* sdcard_icon;

pthread_mutex_t osd_mutex;

void modeset_paint_buffer(struct modeset_buf *buf, Osd *osd) {
	unsigned int j,k,off;
	cairo_t* cr;
	cairo_surface_t *surface;
	char msg[80];
	memset(msg, 0x00, sizeof(msg));

	//check custom message
	//TODO: move this code to the main thread's main loop (read_gstreamerpipe_stream, sleep(10))
	if (osd_custom_message) {
		std::string filename = "/run/pixelpilot.msg";
		FILE *file = fopen(filename.c_str(), "r");
		osd_tag tag;
		if (file != NULL) {

			if (fgets(custom_msg, sizeof(custom_msg), file) == NULL) {
				perror("Error reading from file");
				fclose(file);
			}
			fclose(file);
			if (unlink(filename.c_str()) != 0) {
				perror("Error deleting the file");
			}
			// Ensure null termination at the 80th position to prevent overflow
			custom_msg[79] = '\0';

			// Find the first newline character, if it exists
			char *newline_pos = strchr(custom_msg, '\n');
			if (newline_pos != NULL) {
				*newline_pos = '\0';  // Null-terminate at the newline
			}
			FactTags fact_tags = { {"file", filename} };
			osd->setFact(Fact(FactMeta("osd.custom_message", fact_tags), std::string(custom_msg)));
			//osd_publish_str_fact("osd.custom_message", &tag, 1, std::string(custom_msg))
			custom_msg_refresh_count = 1;
		}
	}

	int osd_x = buf->width - 300;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	// https://www.cairographics.org/FAQ/#clear_a_surface
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);

	cairo_select_font_face (cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 20);

	osd->draw(cr);

	if (osd_vars.enable_video || osd_vars.enable_wfbng ) {
		// stats height
		int stats_top_margin = 5;
		int stats_row_height = 33;
		int stats_height = 30;
		int row_count = 0;
		if (osd_vars.enable_recording) {
			stats_height+=stats_row_height;
		}
		if (osd_vars.enable_video) {
			stats_height+=stats_row_height*2;
			if (osd_vars.enable_latency) {
				stats_height+=stats_row_height;
			}
		}
		if (osd_vars.enable_wfbng) {
			stats_height+=stats_row_height;
		} 

		cairo_set_source_rgba(cr, 0, 0, 0, 0.4); // R, G, B, A
		cairo_rectangle(cr, osd_x, 0, 300, stats_height); 
		cairo_fill(cr);
		

		if (osd_vars.enable_video) {
			row_count++;
			cairo_set_source_surface (cr, fps_icon, osd_x+22, stats_top_margin+stats_row_height-19);
			cairo_paint(cr);
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height);
			sprintf(msg, "%d fps | %dx%d", osd_vars.current_framerate, osd_vars.video_width, osd_vars.video_height);
			cairo_show_text (cr, msg);

			if (osd_vars.enable_latency) {
				row_count++;
				cairo_set_source_surface (cr, lat_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
				cairo_paint (cr);
				cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
				cairo_move_to (cr,osd_x+60, stats_top_margin+stats_row_height*2);
				sprintf(msg, "%.2f ms (%.f, %.f)", osd_vars.latency_avg, osd_vars.latency_min, osd_vars.latency_max);
				cairo_show_text (cr, msg);
			}
			
			// Video Link Elements
			double avg_bw = 0;
			int avg_cnt = 0;
			for (int i = osd_vars.bw_curr; i<(osd_vars.bw_curr+10); ++i) {
				int h = osd_vars.bw_stats[i%10]/10000 * 1.2;
				if (h<0) {
					h = 0;
				}
				if (osd_vars.bw_stats[i%10]>0) {
					avg_bw += osd_vars.bw_stats[i%10];
					avg_cnt++;
				}
			}
			avg_bw = avg_bw / avg_cnt;
			if (avg_bw < 1000) {
				sprintf(msg, "%.2f Kbps", avg_bw / 125 );
			} else {
				sprintf(msg, "%.2f Mbps", avg_bw / 125000 );
			}
			row_count++;
			cairo_set_source_surface (cr, net_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
			cairo_paint (cr);
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to (cr, osd_x+60, stats_top_margin+stats_row_height*row_count);
			cairo_show_text (cr, msg);
		}

		// WFB-ng Elements
		if (osd_vars.enable_wfbng) {
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			sprintf(msg, "RSSI: %3d FEC: %d Lost: %d", osd_vars.wfb_rssi, osd_vars.wfb_fec_fixed, osd_vars.wfb_errors);
			// //TODO (gehee) Only getting WFB_LINK_LOST when testing.
			// if (osd_vars.wfb_flags & WFB_LINK_LOST) {
			// 		sprintf(msg, "%s (LOST)", msg);
			// } else if (osd_vars.wfb_flags & WFB_LINK_JAMMED) {
			// 		sprintf(msg, "%s (JAMMED)", msg);
			// }
			row_count++;
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			cairo_move_to(cr, osd_x+25, stats_top_margin+stats_row_height*row_count);
			cairo_show_text(cr, msg);
		}

		// Recording
		if (osd_vars.enable_recording) {
			
			// Sets the source pattern within cr to a translucent color. This color will then be used for any subsequent drawing operation until a new source pattern is set.
			// did not change anything
			cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
			sprintf(msg, "Recording");
						
			row_count++;
			// This is a convenience function for creating a pattern from surface and setting it as the source in cr with cairo_set_source().
			// if when we have an icon
			cairo_set_source_surface (cr, sdcard_icon, osd_x+22, stats_top_margin+row_count*stats_row_height-19);
			cairo_paint (cr);

			// // set to red font
			cairo_set_source_rgba (cr, 255.0, 0.0, 0.0, 1);

			// Begin a new sub-path. After this call the current point will be (x, y)
			cairo_move_to (cr, osd_x+60, stats_top_margin+stats_row_height*row_count);
			cairo_show_text (cr, msg);
		}
	}

	//display custom message
	if (osd_custom_message && custom_msg_refresh_count > 0) {
		if (custom_msg_refresh_count++ > 5) custom_msg_refresh_count=0;

		// Measure the text width
		cairo_text_extents_t extents;
		cairo_text_extents(cr, custom_msg, &extents);

		// Calculate the position to center the text horizontally
		double x = (buf->width / 2) - (extents.width / 2);
		double y = (buf->height / 2);

		// Set the position and draw the text
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, custom_msg);
	}	

	if (!osd_vars.enable_telemetry){
		cairo_destroy(cr);
	    cairo_surface_destroy(surface);
		return;
	}

	cairo_set_source_rgba (cr, 255.0, 255.0, 255.0, 1);
	// Mavlink elements
	uint32_t x_center = buf->width / 2;
	if (osd_vars.telemetry_level > 1){
		// OSD telemetry
		sprintf(msg, "ALT:%.00fM", osd_vars.telemetry_altitude);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "SPD:%.00fKM/H", osd_vars.telemetry_gspeed);
		cairo_move_to(cr, x_center - (16 * 3) - 360, buf->height / 2 - 8);
		cairo_show_text(cr, msg);
		sprintf(msg, "VSPD:%.00fM/S", osd_vars.telemetry_vspeed);
		cairo_move_to(cr, x_center + (20) + 260, buf->height / 2 + 22);
		cairo_show_text(cr, msg);
	}

    sprintf(msg, "BAT:%.02fV", osd_vars.telemetry_battery / 1000);
    cairo_move_to(cr, 40, buf->height - 30);
    cairo_show_text(cr, msg);
    sprintf(msg, "CONS:%.00fmAh", osd_vars.telemetry_current_consumed);
    cairo_move_to(cr, 40, buf->height - 60);
    cairo_show_text(cr, msg);
    sprintf(msg, "CUR:%.02fA", osd_vars.telemetry_current / 100);
    cairo_move_to(cr, 40, buf->height - 90);
    cairo_show_text(cr, msg);
    sprintf(msg, "THR:%.00f%%", osd_vars.telemetry_throttle);
    cairo_move_to(cr, 40, buf->height - 120);
    cairo_show_text(cr, msg);
    sprintf(msg, "TEMP:%.00fC", osd_vars.telemetry_raw_imu/100);
    cairo_move_to(cr, 40, buf->height - 150);
    cairo_show_text(cr, msg);
    
	if (osd_vars.telemetry_level > 1){
		sprintf(msg, "SATS:%.00f", osd_vars.telemetry_sats);
		cairo_move_to(cr,buf->width - 140, buf->height - 30);
		cairo_show_text(cr, msg);
		sprintf(msg, "HDG:%.00f", osd_vars.telemetry_hdg);
		cairo_move_to(cr,buf->width - 140, buf->height - 120);
		cairo_show_text(cr, msg);
		sprintf(osd_vars.c1, "%.00f", osd_vars.telemetry_lat);

		if (osd_vars.telemetry_lat < 10000000) {
			insertString(osd_vars.c1, "LAT:0.", 0);
		}

		if (osd_vars.telemetry_lat > 9999999) {
			if (numOfChars(osd_vars.c1) == 8) {
				insertString(osd_vars.c1, ".", 1);
			} else {
				insertString(osd_vars.c1, ".", 2);
			}
			insertString(osd_vars.c1, "LAT:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 90);
		cairo_show_text(cr,  osd_vars.c1);

		sprintf(osd_vars.c2, "%.00f", osd_vars.telemetry_lon);
		if (osd_vars.telemetry_lon < 10000000) {
			insertString(osd_vars.c2, "LON:0.", 0);
		}
		if (osd_vars.telemetry_lon > 9999999) {
			if (numOfChars(osd_vars.c2) == 8) {
				insertString(osd_vars.c2, ".", 1);
			} else {
				insertString(osd_vars.c2, ".", 2);
			}
			insertString(osd_vars.c2, "LON:", 0);
		}
		cairo_move_to(cr, buf->width - 240, buf->height - 60);
		cairo_show_text(cr,  osd_vars.c2);
		sprintf(msg, "PITCH:%.00f", osd_vars.telemetry_pitch);
		cairo_move_to(cr, x_center + 440, buf->height - 140);
		sprintf(msg, "ROLL:%.00f", osd_vars.telemetry_roll);
		cairo_move_to(cr, x_center + 440, buf->height - 170);
		sprintf(msg, "DIST:%.03fM", osd_vars.telemetry_distance);
		cairo_move_to(cr, x_center - 350, buf->height - 30);
		cairo_show_text(cr, msg);
	}
    
	sprintf(msg, "RSSI:%.00f", osd_vars.telemetry_rssi);
	cairo_move_to(cr, x_center - 50, buf->height - 30);
	cairo_show_text(cr,  msg);

	struct timespec current_timestamp;
	if (!clock_gettime(CLOCK_MONOTONIC_COARSE, &current_timestamp)) {
		double interval = getTimeInterval(&current_timestamp, &last_timestamp);
		if (osd_vars.telemetry_arm > 1700){
			seconds = seconds + interval;
		}
	}

	sprintf(msg, "TIME:%.2d:%.2d", minutes,seconds);
	cairo_move_to(cr, buf->width - 300, buf->height - 90);
	cairo_show_text(cr, msg);
	if(seconds > 59){
		seconds = 0;
		++minutes;  
	}
	if(minutes > 59){
		seconds = 0;
		minutes = 0;
	}

	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

int osd_thread_signal;

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;

cairo_status_t on_read_png_stream(png_closure_t * closure, unsigned char * data, unsigned int length)
{
	if(length > closure->bytes_left) return CAIRO_STATUS_READ_ERROR;
	
	memcpy(data, closure->iter, length);
	closure->iter += length;
	closure->bytes_left -= length;
	return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t * surface_from_embedded_png(const char * png, size_t length)
{
	int rc = -1;
	png_closure_t closure[1] = {{
		.iter = (unsigned char *)png,
		.bytes_left = (unsigned int)length,
	}};
	return cairo_image_surface_create_from_png_stream(
		(cairo_read_func_t)on_read_png_stream,
		closure);
}

void *__OSD_THREAD__(void *param) {
	osd_thread_params *p = (osd_thread_params *)param;
	Osd *osd = new Osd;
	pthread_setname_np(pthread_self(), "__OSD");

	osd->loadConfig(p->config);
	auto last_display_at = std::chrono::steady_clock::now();

	fps_icon = surface_from_embedded_png(framerate_icon, framerate_icon_length);
	lat_icon = surface_from_embedded_png(latency_icon, latency_icon_length);
	net_icon = surface_from_embedded_png(bandwidth_icon, bandwidth_icon_length);
	sdcard_icon = surface_from_embedded_png(sdcard_white_icon, sdcard_white_icon_length);

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane,
								  buf->fb, buf->width, buf->height, osd_vars.plane_zpos);
	assert(ret >= 0);
	while (!osd_thread_signal) {
		std::unique_lock<std::mutex> lock(mtx);
		std::vector<Fact> fact_buf;
		auto since_last_display = std::chrono::steady_clock::now() - last_display_at;
		auto wait = std::chrono::milliseconds(osd_vars.refresh_frequency_ms) - since_last_display;
		bool got_fact = cv.wait_for(
					lock,
					wait,
					[/*fact_queue*/] {
						return !fact_queue.empty();
					});
		if (got_fact) {
			// thread woke up because we got a new fact(s)
			// copy all the facts to the temporary buffer to unlock the queue ASAP
			for(; !fact_queue.empty(); fact_queue.pop()) {
				SPDLOG_DEBUG("got fact {}({})", fact_queue.front().getName(), fact_queue.front().getTags());
				fact_buf.push_back(fact_queue.front());
			}
			lock.unlock();
			for (Fact fact : fact_buf) {
				osd->setFact(fact);
			}
			fact_buf.clear();
		} else {
			// thread woke up because of refresh timeout
			lock.unlock();
			SPDLOG_DEBUG("refresh OSD");
			int buf_idx = p->out->osd_buf_switch ^ 1;
			struct modeset_buf *buf = &p->out->osd_bufs[buf_idx];
			modeset_paint_buffer(buf, osd);

			int ret = pthread_mutex_lock(&osd_mutex);
			assert(!ret);	
			p->out->osd_buf_switch = buf_idx;
			ret = pthread_mutex_unlock(&osd_mutex);
			assert(!ret);

			// tell the display thread that we have a update
			ret = pthread_mutex_lock(&video_mutex);
			assert(!ret);
			osd_update_ready = true;
			ret = pthread_cond_signal(&video_cond);
			assert(!ret);
			ret = pthread_mutex_unlock(&video_mutex);
			assert(!ret);

			last_display_at = std::chrono::steady_clock::now();
		}
    }
	spdlog::info("OSD thread done.");
	return nullptr;
}

void mk_tags(osd_tag *tags, int n_tags, FactTags *fact_tags) {
	osd_tag tag;
	for (int i = 0; i < n_tags; i++) {
		tag = *tags++;
		fact_tags->emplace(tag.key, tag.val);
	}
}

void publish(Fact fact) {
	if (!enable_osd) return;
	//SPDLOG_DEBUG("post fact {}({})", fact.getName(), fact.getTags());
	{
		std::lock_guard<std::mutex> lock(mtx);
		fact_queue.push(fact);
	}
	cv.notify_one();
}

#ifdef __cplusplus
extern "C" {
#endif

// Batch APIs

void *osd_batch_init(uint n) {
	auto batch = new std::vector<Fact>;
	batch->reserve(n);
	return batch;
}
void osd_publish_batch(void *batch) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	if (enable_osd) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			for (Fact fact : *facts) {
				// SPDLOG_DEBUG("batch post fact {}({})", fact.getName(), fact.getTags());
				fact_queue.push(fact);
			}
		}
		cv.notify_one();
	}
	delete facts;
};

void osd_add_bool_fact(void *batch, char const *name, osd_tag *tags, int n_tags, bool value) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	facts->push_back(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_add_int_fact(void *batch, char const *name, osd_tag *tags, int n_tags, long value) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	facts->push_back(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_add_uint_fact(void *batch, char const *name, osd_tag *tags, int n_tags, ulong value) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	facts->push_back(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_add_double_fact(void *batch, char const *name, osd_tag *tags, int n_tags, double value) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	facts->push_back(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_add_str_fact(void *batch, char const *name, osd_tag *tags, int n_tags, const char *value) {
	std::vector<Fact> *facts = static_cast<std::vector<Fact> *>(batch);
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	facts->push_back(Fact(FactMeta(std::string(name), fact_tags), std::string(value)));
};


// Individual APIs

void osd_publish_bool_fact(char const *name, osd_tag *tags, int n_tags, bool value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_int_fact(char const *name, osd_tag *tags, int n_tags, long value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_uint_fact(char const *name, osd_tag *tags, int n_tags, ulong value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_double_fact(char const *name, osd_tag *tags, int n_tags, double value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_str_fact(char const *name, osd_tag *tags, int n_tags, const char *value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), std::string(value)));
};

#ifdef __cplusplus
}
#endif
