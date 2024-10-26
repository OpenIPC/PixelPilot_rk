
extern "C" {
#include "drm.h"
#include "mavlink.h"
#include "icons/icons.h"
}
#include "osd.h"

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
#include <cstdlib> //KILLME
#include <string>
#include <cairo.h>
#include "spdlog/spdlog.h"
#include <fmt/ranges.h>

#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

#define PATH_MAX	4096

struct osd_vars osd_vars;

extern uint32_t frames_received;
uint32_t stats_rx_bytes = 0;
struct timespec last_timestamp = {0, 0};
float rx_rate = 0;
int hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
char custom_msg[80];
u_int custom_msg_refresh_count = 0;


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
	Fact(): meta(FactMeta("", {})), type(T_UNDEF) {};
	Fact(FactMeta meta, bool val): meta(meta), value(val), type(T_BOOL) {};
	Fact(FactMeta meta, int val): meta(meta), value(val), type(T_INT) {};
	Fact(FactMeta meta, uint val): meta(meta), value(val), type(T_UINT) {};
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

	int getIntValue() {
		assertType(T_INT);
		return std::get<int>(value);
	}

	uint getUintValue() {
		assertType(T_UINT);
		return std::get<uint>(value);
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

	std::string getName() {
		return meta.getName();
	}

	FactTags getTags() {
		return meta.getTags();
	}
	
private:
	enum Type {
		T_UNDEF,
		T_BOOL,
		T_INT,
		T_UINT,
		T_DOUBLE,
		T_STRING
	} type = T_UNDEF;
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
		int,
		uint,
		double,
		std::string
		> value;
};

std::queue<Fact> fact_queue;
std::mutex mtx;
std::condition_variable cv;

//
// Widgets
//

class Widget {
public:
	Widget(int pos_x, int pos_y): pos_x(pos_x), pos_y(pos_y) {};

	virtual void draw(cairo_t *cr) {};
	virtual void setFact(uint idx, Fact fact) {};

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
private:
	std::string text;
};


class IconTextWidget: Widget {
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

private:
	std::string text;
	cairo_surface_t *icon;
};


class TplTextWidget: public Widget {
public:
	TplTextWidget(int pos_x, int pos_y, std::string tpl, uint num_args):
		Widget(pos_x, pos_y), tpl(tpl), num_args(num_args) {
		for (auto i=0; i < num_args; i++) {
			args.push_back(Fact());
		}
		// todo: validate num_args matches number of placeholders
	};

	virtual void setFact(uint idx, Fact fact) {
		args[idx] = fact;
	}

	virtual void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		std::unique_ptr<std::string> msg = render_tpl();
		cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, msg->c_str());
	}

protected:
	std::unique_ptr<std::string> render_tpl() {
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
						msg->append(std::to_string(fact->getDoubleValue()));
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

private:
	std::string tpl;
	uint num_args;
	std::vector<Fact> args;
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

private:
	cairo_surface_t *icon;
};


class Osd {
public:
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
	std::vector<Widget *> widgets;
	std::vector<std::tuple<FactMatcher, Widget *, uint>> matchers;
};


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
			sprintf(msg, "WFB %3d F%d L%d", osd_vars.wfb_rssi, osd_vars.wfb_fec_fixed, osd_vars.wfb_errors);
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
	if (osd_custom_message) {
		FILE *file = fopen("/run/pixelpilot.msg", "r");
		if (file != NULL) {

			if (fgets(custom_msg, sizeof(custom_msg), file) == NULL) {
				perror("Error reading from file");
				fclose(file);
			}
			fclose(file);
			if (unlink("/run/pixelpilot.msg") != 0) {
				perror("Error deleting the file");
			}
			custom_msg_refresh_count = 1;
		}
		if (custom_msg_refresh_count > 0) {

			if (custom_msg_refresh_count++ > 5) custom_msg_refresh_count=0;

			size_t msg_length = strlen(custom_msg);

			// Ensure null termination at the 80th position to prevent overflow
			custom_msg[79] = '\0';

			// Find the first newline character, if it exists
			char *newline_pos = strchr(custom_msg, '\n');
			if (newline_pos != NULL) {
				*newline_pos = '\0';  // Null-terminate at the newline
			}

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
	}	

	if (!osd_vars.enable_telemetry){
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

	auto last_display_at = std::chrono::steady_clock::now();

	fps_icon = surface_from_embedded_png(framerate_icon, framerate_icon_length);
	lat_icon = surface_from_embedded_png(latency_icon, latency_icon_length);
	net_icon = surface_from_embedded_png(bandwidth_icon, bandwidth_icon_length);
	sdcard_icon = surface_from_embedded_png(sdcard_white_icon, sdcard_white_icon_length);

	Fact fact = Fact();

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
				SPDLOG_DEBUG("got fact {}", fact_queue.front().getName());
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
			uint rnd = std::rand();
			int buf_idx = p->out->osd_buf_switch ^ 1;
			struct modeset_buf *buf = &p->out->osd_bufs[buf_idx];
			switch (rnd % 2) {
			case 0:
				{
					SPDLOG_DEBUG("rand 0");
					osd->setFact(Fact(FactMeta("latency.avg"), (double)osd_vars.latency_avg));
					osd->setFact(Fact(FactMeta("latency.min"), (double)osd_vars.latency_min));
					osd->setFact(Fact(FactMeta("latency.max"), (double)osd_vars.latency_max));
					osd->setFact(Fact(FactMeta("video.current_framerate"), (int)osd_vars.current_framerate));
					// osd->setFact(Fact(FactMeta("video.width"), (int)osd_vars.video_width));
					// osd->setFact(Fact(FactMeta("video.height"), (int)osd_vars.video_height));
					osd->setFact(Fact(FactMeta("wfb.rssi"), (int)osd_vars.wfb_rssi));
					osd->setFact(Fact(FactMeta("wfb.fec_fixed"), (int)osd_vars.wfb_fec_fixed));
					osd->setFact(Fact(FactMeta("wfb.errors"), (int)osd_vars.wfb_errors));
					break;
				}
			case 1:
				{
					SPDLOG_DEBUG("rnd 1");
					osd->setFact(Fact(FactMeta("latency.avg"), (double)(rnd % 101) ));
					osd->setFact(Fact(FactMeta("latency.min"), (double)(rnd % 102) ));
					osd->setFact(Fact(FactMeta("latency.max"), (double)(rnd % 103) ));
					osd->setFact(Fact(FactMeta("video.current_framerate"), (int)(rnd % 104) ));
					// osd->setFact(Fact(FactMeta("video.width"), (int)640));
					// osd->setFact(Fact(FactMeta("video.height"), (int)480));
					osd->setFact(Fact(FactMeta("wfb.rssi"), (int)(-rnd % 110) ));
					osd->setFact(Fact(FactMeta("wfb.fec_fixed"), (int)(rnd % 30) ));
					osd->setFact(Fact(FactMeta("wfb.errors"), (int)(rnd % 25) ));
					break;
				}
			}
			modeset_paint_buffer(buf, osd);

			int ret = pthread_mutex_lock(&osd_mutex);
			assert(!ret);	
			p->out->osd_buf_switch = buf_idx;
			ret = pthread_mutex_unlock(&osd_mutex);
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
	SPDLOG_DEBUG("publish {}({})", fact.getName(), fact.getTags());
	{
		std::lock_guard<std::mutex> lock(mtx);
		fact_queue.push(fact);
	}
	cv.notify_one();
}

#ifdef __cplusplus
extern "C" {
#endif

void osd_publish_bool_fact(char const *name, osd_tag *tags, int n_tags, bool value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_int_fact(char const *name, osd_tag *tags, int n_tags, int value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_uint_fact(char const *name, osd_tag *tags, int n_tags, uint value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_double_fact(char const *name, osd_tag *tags, int n_tags, double value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), value));
};

void osd_publish_str_fact(char const *name, osd_tag *tags, int n_tags, char *value) {
	FactTags fact_tags;
	mk_tags(tags, n_tags, &fact_tags);
	publish(Fact(FactMeta(std::string(name), fact_tags), std::string(value)));
};

#ifdef __cplusplus
}
#endif
