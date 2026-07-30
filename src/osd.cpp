/**
 * A graphical OSD overlay on top of the video.
 * It receives data-points ("Facts") from other parts of the system via osd_publish_* / osd_add_*
 * and uses this data to render LVGL widgets on the screen.
 * The whole OSD is configured using a JSON config file: a list of widgets, positions, options
 * and "subscriptions" to Facts.
 *
 * OSD runs in a separate thread and receives all the facts via queue.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csetjmp>
extern "C" {
#include "drm.h"
#include "mavlink.h"
#include "menu.h"
#include "input.h"
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
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <cstdlib> //KILLME
#include <string>
#include <optional>
#include <regex>
#include <utility>
#include <filesystem>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"
#include <fmt/ranges.h>
#include "../lvgl/lvgl.h"
#include "../lvgl/src/draw/lv_image_decoder_private.h"
#include "../lvgl/src/draw/sw/lv_draw_sw_gradient.h"  // lv_gradient_init_stops() for SignalWarningWidget
#include <png.h>
#include "osd_gl.hpp"

#ifdef BUILD_TESTS
#include <catch2/catch.hpp>
#endif

#define WFB_LINK_LOST 1
#define WFB_LINK_JAMMED 2

#define PATH_MAX	4096

using json = nlohmann::json;

int enable_osd = 0;
int osd_zpos = 2;
extern uint32_t refresh_frequency_ms;
extern uint32_t frames_received;
uint32_t stats_rx_bytes = 0;
struct timespec last_timestamp = {0, 0};
float rx_rate = 0;
int hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
extern pthread_mutex_t video_mutex;
extern pthread_cond_t video_cond;
bool osd_update_ready = false;
bool menu_active = false;
bool gsmenu_enabled = false;

OsdGl osd_gl;
extern bool enable_live_colortrans;
extern float live_colortrans_offset;
extern float live_colortrans_gain;

#include "frame_processor.h"
extern FrameProcessor *frame_proc;
extern bool dvr_osd;

osd_thread_params *p;

double getTimeInterval(struct timespec* timestamp, struct timespec* last_meansure_timestamp) {
  return (timestamp->tv_sec - last_meansure_timestamp->tv_sec) +
       (timestamp->tv_nsec - last_meansure_timestamp->tv_nsec) / 1000000000.;
}


//
// Evaluation of `convert` expressions on numerical facts
//

class ExpressionException : public std::exception {
public:
    enum ErrorType {
        MISMATCHED_PARENTHESES,
        DIVISION_BY_ZERO,
        UNKNOWN_OPERATOR,
        INVALID_EXPRESSION
    };

    ExpressionException(ErrorType type, const std::string& message)
        : type_(type), msg_(message) {}

    virtual const char* what() const noexcept override {
        return msg_.c_str();
    }

    ErrorType type() const { return type_; }

private:
    ErrorType type_;
    std::string msg_;
};

/**
 * This class can parse and evaluate basic math expressions.
 * It understands the following operators and tokens:
 * - '123', '12.34' - integer and simple float numbers (negative not supported yet)
 * - '+', '-', '/', '*' - standard math operators with respect to precedence
 * - '(', ')' - parentheses to alter the precedence
 * - 'x' - the variable that is goig to be passed to `evaluate` function
 *
 * It always evaluates float math and returns float.
 */
class ExpressionTree {
public:
	ExpressionTree() : root(nullptr) {}
	ExpressionTree(const std::string &expression) : root(nullptr) {
		parse(expression);
	}
    // Move constructor
    ExpressionTree(ExpressionTree&& other) noexcept : root(std::move(other.root)) {}
    
    // Copy constructor
    ExpressionTree(const ExpressionTree& other) {
        if (other.root) {
            root = std::make_unique<Node>(*other.root); // Make a deep copy
        } else {
            root = nullptr;
        }
    }

	// Tokenize the expression string, return vector of tokens
	std::vector<std::string> tokenize(const std::string& expression) {
		std::vector<std::string> tokens;
		std::string currentToken;

		for (size_t i = 0; i < expression.length(); ++i) {
			char c = expression[i];

			// Handling digits and decimal point for numbers
			if (std::isdigit(c) || c == '.') {
				currentToken += c;
			} 
			// Handling operators, 'x' and parentheses
			else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' || c == 'x') {
				if (!currentToken.empty()) {
					tokens.push_back(currentToken);
					currentToken.clear();
				}
				tokens.push_back(std::string(1, c)); // Add the operator or parenthesis as a token
			} else if (std::isspace(c)) {
				// Ignore whitespace
				if (!currentToken.empty()) {
					tokens.push_back(currentToken);
					currentToken.clear();
				}
			} else {
				throw ExpressionException(
    					  ExpressionException::INVALID_EXPRESSION,
						  "Unexpected symbol at " + std::to_string(i) + ": '" + c + "'");
			}
		}
    
		if (!currentToken.empty()) {
			tokens.push_back(currentToken); // Add any remaining token
		}

		return tokens;
	}
	
    void parseTokens(const std::vector<std::string>& tokens) {
        std::vector<Node*> output;
        std::vector<Node*> operators;

        for (const auto& token : tokens) {
            if (isNumber(token)) {
                output.push_back(new Node(std::stod(token)));
            } else if (token == "x") {
                output.push_back(new Node('x'));
            } else if (token == "(") {
                operators.push_back(new Node('(')); // Push a dummy node for '('
            } else if (token == ")") {
                while (!operators.empty() && operators.back()->op != '(') {
                    processOperator(output, operators);
                }
                if (operators.empty()) {
                  throw ExpressionException(
                      ExpressionException::MISMATCHED_PARENTHESES,
                      "Mismatched parentheses");
                }
                operators.pop_back(); // Remove the '('
            } else {
                while (!operators.empty() && precedence(operators.back()->op) >= precedence(token[0])) {
                    processOperator(output, operators);
                }
                operators.push_back(new Node(token[0]));
            }
        }

        while (!operators.empty()) {
            processOperator(output, operators);
        }

        root.reset(output.back());
    }

	// Tokenize and parse the expression
    void parse(const std::string &expression) {
		parseTokens(tokenize(expression));
	}

    double evaluate(double xValue) {
        return evaluateNode(root.get(), xValue);
    }

	std::string treeToString() const {
		if (!root.get()) return "null";

		return nodeToString(root.get());
	}

private:
    struct Node {
        char op; // Operator: +, -, *, /, 'x' variable
        double value; // Used for numeric values
        std::unique_ptr<Node> left, right; // Left and right children

        Node(double val) : op(0), value(val), left(nullptr), right(nullptr) {}
        Node(char operation) : op(operation), value(0), left(nullptr), right(nullptr) {}
        // Copy constructor for Node
        Node(const Node& other) 
            : op(other.op), value(other.value), 
              left(other.left ? std::make_unique<Node>(*other.left) : nullptr), 
              right(other.right ? std::make_unique<Node>(*other.right) : nullptr) {}

        // Move constructor for Node
        Node(Node&& other) noexcept 
            : op(other.op), value(other.value), 
              left(std::move(other.left)), right(std::move(other.right)) {}
	};

    std::unique_ptr<Node> root;

    bool isNumber(const std::string& s) {
        char* p;
        std::strtod(s.c_str(), &p);
        return *p == 0; // Verify if p points to the end of the string
    }

    int precedence(char op) {
        if (op == '+' || op == '-') return 1;
        if (op == '*' || op == '/') return 2;
        return 0;
    }

    void processOperator(std::vector<Node*>& output, std::vector<Node*>& operators) {
        Node* right = output.back(); output.pop_back();
        Node* left = output.back(); output.pop_back();
        Node* opNode = operators.back(); operators.pop_back();
        opNode->left = std::unique_ptr<Node>(left);
        opNode->right = std::unique_ptr<Node>(right);
        output.push_back(opNode);
    }

    double evaluateNode(Node* node, double xValue) const {
        if (!node) return 0;
        if (node->op == 0) {
            return node->value; // Return numeric value
        } else if (node->op == 'x') {
            return xValue; // Return the value of variable x
        }
        double leftValue = evaluateNode(node->left.get(), xValue);
        double rightValue = evaluateNode(node->right.get(), xValue);
        switch (node->op) {
            case '+': return leftValue + rightValue;
            case '-': return leftValue - rightValue;
            case '*': return leftValue * rightValue;
            case '/':
                if (rightValue == 0) {
                  throw ExpressionException(
                      ExpressionException::DIVISION_BY_ZERO,
                      "Division by zero");
                }
                return leftValue / rightValue;
            default:
              throw ExpressionException(ExpressionException::UNKNOWN_OPERATOR,
                                        "Unknown operator");
        }
    }

	std::string nodeToString(Node *node) const {
		std::ostringstream oss;
		oss << "Node(op=" << (node->op != 0 ? std::string(1, node->op) : std::to_string(node->value)) 
			<< ", left=" << nodeToString(node->left.get()) 
			<< ", right=" << nodeToString(node->right.get()) << ")";
		return oss.str();
	}

};

//
// Facts
//

typedef std::map<std::string, std::string> FactTags;


class FactMeta {
public:
	FactMeta(): name(""), tags({}) {};
	FactMeta(std::string name): name(name), tags({}) {};
	FactMeta(std::string name, FactTags tags): name(name), tags(tags) {};
	

	std::string getName() const { return name; }
	FactTags getTags() const { return tags; }

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

	bool isDefined() const {
		return type != T_UNDEF;
	}

    operator bool() {
        switch (type) {
        case T_BOOL:
            return getBoolValue();
        case T_UINT:
            return getUintValue() != 0;
        case T_INT:
            return getIntValue() != 0;
        case T_DOUBLE:
            return getDoubleValue() != 0.0;
        case T_STRING:
            return getStrValue() != "";
        }
    }

    operator long() {
        switch (type) {
        case T_BOOL:
            return getBoolValue() ? 1 : 0;
        case T_UINT:
            return (long)getUintValue();
        case T_INT:
            return getIntValue();
        case T_DOUBLE:
            return round(getDoubleValue());
        }
    }

    operator ulong() {
        switch (type) {
        case T_BOOL:
            return getBoolValue() ? 1 : 0;
        case T_UINT:
            return getUintValue();
        case T_INT:
            return (ulong)getIntValue();
        case T_DOUBLE:
            return round(getDoubleValue());
        }
    }

    operator double() {
        switch (type) {
        case T_BOOL:
            return getBoolValue() ? 1.0 : 0.0;
        case T_UINT:
            return getUintValue() * 1.0;
        case T_INT:
            return getIntValue() * 1.0;
        case T_DOUBLE:
            return getDoubleValue();
        }
    }

	// TODO: try to cast instead of crash
	bool getBoolValue() const {
		assertType(T_BOOL);
		return std::get<bool>(value);
	}

	long getIntValue() const {
		assertType(T_INT);
		return std::get<long>(value);
	}

	ulong getUintValue() const {
		assertType(T_UINT);
		return std::get<ulong>(value);
	}

	double getDoubleValue() const {
		assertType(T_DOUBLE);
		return std::get<double>(value);
	}

	std::string getStrValue() const {
		assertType(T_STRING);
		return std::get<std::string>(value);
	}

	std::string getTypeName() const {
		return typeName(type);
	}

	Type getType() const {
		return type;
	}

	std::string getName() const {
		return meta.getName();
	}

	FactTags getTags() const {
		return meta.getTags();
	}

	std::string asString() const {
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

	std::string asVerboseString() const {
		std::ostringstream oss;
		if (!isDefined()) {
			oss << "undef";
		} else {
			oss << getName() << " (" << getTypeName() << ") {";
			for (const auto &tag : getTags()) {
				oss << tag.first << "=>" << tag.second << ", ";
			}
			oss << "} = " << asString();
		}
		return oss.str();
	}
	
private:
	Type type = T_UNDEF;
	std::string typeName(Type t) const {
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

	void assertType(Type t) const {
		if (t != type) {
			spdlog::error("'{}': requested type of {}, but the actual type is {}",
						  asVerboseString(), typeName(t), typeName(type));
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



class FactMatcher {
public:
	FactMatcher(std::string name, FactTags tags, std::string &convert_str)
		: name(name), tags(tags), converter(ExpressionTree(convert_str)) {};
	// FactMatcher(std::string name, FactTags tags, ExpressionTree converter)
	// 	: name(name), tags(tags), converter(std::move(converter)) {};
	FactMatcher(std::string name, FactTags tags): name(name), tags(tags) {};
	FactMatcher(std::string name): name(name), tags({}) {};

	
	/**
	 * Returns true if names are equal and all match_tags are defined and have equal value
	 */
	bool matches(Fact fact) {
		if(fact.getName() != name) return false;
		FactTags fact_tags = fact.getTags();
		
		for (const auto& [key, match_value] : tags) {
			if (auto value = fact_tags.find(key); value != tags.end()) {
				if (value->second != match_value) return false;
			} else {
				return false;
			}
		}
		return true;
	}

	/**
	 * Applies 'convert' expression to the fact's value.
	 * On success, a new fact is returned with `.converted` appended to its name and value converted
	 */
	Fact convert(Fact fact_in) {
		if (converter.has_value()) {
			std::string name = fact_in.getName();
			FactTags tags = fact_in.getTags();
			FactMeta new_meta(name + ".converted", tags);
			double val = 0.0;

			switch (fact_in.getType()) {
			case Fact::T_BOOL:
				val = 0.0;
				if(fact_in.getBoolValue()) {
					val = 1.0;
				}
				break;
			case Fact::T_INT:
				val = static_cast<double>(fact_in.getIntValue());
				break;
			case Fact::T_UINT:
				val = static_cast<double>(fact_in.getUintValue());
				break;
			case Fact::T_DOUBLE:
				val = fact_in.getDoubleValue();
				break;
			default:
				spdlog::warn("Attempt to apply 'convert' to unexpected datatype. Ignoring");
				return fact_in;
			}
			return Fact(new_meta, converter->evaluate(val));
		} else {
			return fact_in;
		}
	}
	
	std::string name;
	FactTags tags;
protected:
	std::optional<ExpressionTree> converter = std::nullopt;
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

	virtual void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) {}
	virtual void tick() {}

	virtual void setFact(uint idx, Fact fact) {
        if (idx >= args.size()) throw std::out_of_range("setFact index out of range");
        args[idx] = fact;
	}

	// Reset one bound fact to undefined (its initial state). Used to flush stale
	// facts, e.g. on an RX mode switch. Resets the arg directly rather than routing
	// through setFact(), because some overrides read the value unconditionally and
	// would choke on an undefined fact. Marks the widget dirty so it re-renders the
	// cleared state on the next tick.
	virtual void clearFact(uint idx) {
		if (idx >= args.size()) return;
		args[idx] = Fact();
		dirty = true;
	}

protected:
	int absX(int screen_w) const { return (screen_w + pos_x) % screen_w; }
	int absY(int screen_h) const { return (screen_h + pos_y) % screen_h; }

	int pos_x, pos_y;
	std::vector<Fact> args;
	bool dirty = false;  // widget needs a redraw; set on setFact/clearFact, cleared in tick()
};


class TextWidget: public Widget {
public:
	TextWidget(int pos_x, int pos_y, std::string text): Widget(pos_x, pos_y), text(text) {};

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		lv_obj_t* label = lv_label_create(parent);
		lv_obj_set_pos(label, absX(screen_w), absY(screen_h));
		lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
		lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
		lv_label_set_text(label, text.c_str());
	}

protected:
	std::string text;
};


class IconTextWidget: public Widget {
public:
	IconTextWidget(int pos_x, int pos_y, std::string icon_path, std::string text):
		Widget(pos_x, pos_y), text(text), icon_path(std::move(icon_path)) {};

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		int ax = absX(screen_w), ay = absY(screen_h);
		lv_icon = lv_image_create(parent);
		lv_obj_set_pos(lv_icon, ax, ay - 20);
		lv_image_set_src(lv_icon, icon_path.c_str());

		lv_label = lv_label_create(parent);
		lv_obj_set_pos(lv_label, ax + 40, ay - 20);
		lv_obj_set_style_bg_opa(lv_label, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_text_color(lv_label, lv_color_white(), LV_PART_MAIN);
		lv_label_set_text(lv_label, text.c_str());
	}

protected:
	std::string text;
	std::string icon_path;
	lv_obj_t* lv_icon = nullptr;
	lv_obj_t* lv_label = nullptr;
};


class TplTextWidget: public Widget {
public:
    TplTextWidget(int pos_x, int pos_y, std::string tpl, uint num_args):
        Widget(pos_x, pos_y, num_args), tpl(tpl), num_args(num_args) {
        _tokens = tokenize(tpl);
    };

    void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
        lv_label = lv_label_create(parent);
        lv_obj_set_pos(lv_label, absX(screen_w), absY(screen_h));
        lv_obj_set_style_bg_opa(lv_label, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(lv_label, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(lv_label, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(lv_label, "?");
    }

    void setFact(uint idx, Fact fact) override {
        Widget::setFact(idx, fact);
        dirty = true;
    }

    void tick() override {
        if (dirty) {
            updateLvLabel();
            dirty = false;
        }
    }

    virtual void updateLvLabel() {
        if (!lv_label) return;
        auto text = render_tpl();
        lv_label_set_text(lv_label, text->c_str());
    }

    std::unique_ptr<std::string> render_tpl() {
        return render_tokens(_tokens, args);
    }

    uint default_precision = 2;

protected:
    enum class TokenType {
        Literal,
        Boolean,
        Int,
        Uint,
        Float,
        String
    };

    struct Token {
        TokenType type;
        std::optional<std::string> value; // Used to hold literal
        uint precision;    // Precision for float placeholders if applicable

        Token(TokenType t, std::string v) // literal
            : type(t), value(std::move(v)), precision(0) {}
        Token(TokenType t, uint p) // float
            : type(t), value(std::nullopt), precision(p) {}
        Token(TokenType t) // other
            : type(t), value(std::nullopt), precision(0) {}
    };

    std::unique_ptr<std::string> render_tpl(const std::string& tpl, const std::vector<Fact>& facts) {
        auto tokens = tokenize(tpl);
        return render_tokens(tokens, facts);
    }

    std::unique_ptr<std::string> render_tokens(const std::vector<Token>& tokens,
                                               const std::vector<Fact>& facts) {
        std::ostringstream msg;
        size_t fact_i = 0; // To track the current index in the facts vector

        for (const Token& token : tokens) {
            if (token.type == TokenType::Literal) {
                msg << *token.value; // Append literal directly, dereference std::optional
            } else {
                // Check if we have enough facts and if the current fact is defined
                if (fact_i >= facts.size() || !facts[fact_i].isDefined()) {
                    msg << '?'; // Append '?' for undefined facts
                } else {
                    switch (token.type) {
                    case TokenType::Boolean:
                        msg << (facts[fact_i].getBoolValue() ? 't' : 'f');
                        break;
                    case TokenType::Int:
                        msg << facts[fact_i].getIntValue();
                        break;
                    case TokenType::Uint:
                        msg << facts[fact_i].getUintValue();
                        break;
                    case TokenType::Float:
                        msg << std::fixed << std::setprecision(token.precision) << facts[fact_i].getDoubleValue();
                        break;
                    case TokenType::String:
                        msg << facts[fact_i].getStrValue();
                        break;
                    }
                }
                fact_i++; // Move to the next fact for the next placeholder
            }
        }
        return std::make_unique<std::string>(msg.str());
    }

    std::vector<Token> tokenize(const std::string& tpl) {
        std::vector<Token> tokens;
        std::regex token_regex(R"(%%|%[bisu]|%(\.\d+)?f|[^%]+)"); // Match placeholders and literals
        std::sregex_iterator iter(tpl.begin(), tpl.end(), token_regex);
        std::sregex_iterator end;

        while (iter != end) {
            std::string match = iter->str();
            if (match == "%%") {
                tokens.emplace_back(TokenType::Literal, "%");
            } else if (match[0] == '%') {
                if (match.size() == 2) { // Simple placeholder like %b, %i, %u, %s, %f
                    if (match[1] == 'b') {
                        tokens.emplace_back(TokenType::Boolean);
                    } else if (match[1] == 'i' || match[1] == 'd') {
                        tokens.emplace_back(TokenType::Int);
                    } else if (match[1] == 'u') {
                        tokens.emplace_back(TokenType::Uint);
                    } else if (match[1] == 's') {
                        tokens.emplace_back(TokenType::String);
                    } else if (match[1] == 'f') {
                        tokens.emplace_back(TokenType::Float, default_precision);
                    }
                } else if (match.back() == 'f') { // Float placeholder with precision
                    uint precision = 0;
                    if (match.size() > 2 && match[1] == '.') {
                        precision = std::stoi(match.substr(2, match.size() - 3)); // Extract precision
                    }
                    tokens.emplace_back(TokenType::Float, precision); // Add float token
                }
            } else {
                tokens.emplace_back(TokenType::Literal, match); // Accumulate literal
            }
            ++iter;
        }

        return tokens;
    }

    std::string tpl;
    std::vector<Token> _tokens;
    uint num_args;
    lv_obj_t* lv_label = nullptr;
};


class IconTplTextWidget: public TplTextWidget {
public:
	IconTplTextWidget(int pos_x, int pos_y, std::string icon_path, std::string tpl, uint num_args):
		TplTextWidget(pos_x, pos_y, tpl, num_args), icon_path(std::move(icon_path)) {}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		int ax = absX(screen_w), ay = absY(screen_h);
		lv_obj_t* icon_obj = lv_image_create(parent);
		lv_obj_set_pos(icon_obj, ax, ay - 20);
		lv_image_set_src(icon_obj, icon_path.c_str());

		lv_label = lv_label_create(parent);
		lv_obj_set_pos(lv_label, ax + 40, ay - 20);
		lv_obj_set_style_bg_opa(lv_label, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_text_color(lv_label, lv_color_white(), LV_PART_MAIN);
		lv_label_set_text(lv_label, "?");
	}

protected:
	std::string icon_path;
};

class BoxWidget: public Widget {
public:
	BoxWidget(int pos_x, int pos_y, uint w, uint h, double r, double g, double b, double a):
		Widget(pos_x, pos_y), w(w), h(h), r(r), g(g), b(b), a(a) {};

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		lv_obj_t* box = lv_obj_create(parent);
		lv_obj_set_pos(box, absX(screen_w), absY(screen_h));
		lv_obj_set_size(box, w, h);
		lv_obj_set_style_bg_color(box, lv_color_make(
			(uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255)), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(box, (lv_opa_t)(a * 255), LV_PART_MAIN);
		lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
		lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
		lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	}

private:
	uint w, h;
	double r, g, b, a;
};

// Warning border that fades in proportionally as a signal drops below a
// threshold (e.g. link RSSI). Accepts one or more facts and reacts to the BEST
// (highest) value among them, so with several antennas it only starts reacting
// once even the strongest one drops below `threshold`. Four edge bars form a
// soft red gradient, opaque at the screen edge and transparent inward; the
// container's opacity is driven by how far past the threshold the best value is
// (threshold -> 0, critical -> max). Straight (non-premultiplied) alpha — relies
// on the OSD plane being composited in "Coverage" blend mode (see drm.c) on the
// display path, and on the GL shader's straight-alpha over blend (see
// frame_colorcorrect.cpp) on the DVR re-encode path.
class SignalWarningWidget: public Widget {
public:
	SignalWarningWidget(int pos_x, int pos_y, double threshold, double critical, uint num_facts,
	                    int average_ms = 0, bool rate_mode = false, double divisor = 1.0):
		Widget(pos_x, pos_y, num_facts), threshold(threshold), critical(critical),
		rate_divisor(divisor > 0.0 ? divisor : 1.0) {
		int window_ms = average_ms > 0 ? average_ms : 1000;
		int bucket_ms = std::max(1, window_ms / 20);
		if (rate_mode) {
			// Facts are byte/packet deltas; derive a per-second rate per fact
			// (like VideoBitrateWidget) and warn on that.
			rate_window_ms = window_ms;
			for (uint i = 0; i < num_facts; i++)
				rate_ras.emplace_back(window_ms, bucket_ms);
		} else if (average_ms > 0) {
			avg = std::make_unique<RunningAverage>(average_ms, bucket_ms);
		}
	};

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		// Transparent full-screen container; the four edge bars are its children
		// so one opacity value scales the whole border.
		vig = lv_obj_create(parent);
		lv_obj_remove_style_all(vig);
		lv_obj_set_pos(vig, 0, 0);
		lv_obj_set_size(vig, screen_w, screen_h);
		lv_obj_clear_flag(vig, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_clear_flag(vig, LV_OBJ_FLAG_CLICKABLE);

		int b = (int)(screen_h * kBorderPct / 100.0);  // border band (px)
		if (b < 1) b = 1;
		//     x            y            w          h          grad      direction        edge@start
		mkBar(0,           0,           b,         screen_h,  &grad[0], LV_GRAD_DIR_HOR, true);   // left
		mkBar(screen_w-b,  0,           b,         screen_h,  &grad[1], LV_GRAD_DIR_HOR, false);  // right
		mkBar(0,           0,           screen_w,  b,         &grad[2], LV_GRAD_DIR_VER, true);   // top
		mkBar(0,           screen_h-b,  screen_w,  b,         &grad[3], LV_GRAD_DIR_VER, false);  // bottom

		lv_obj_move_background(vig);
		lv_obj_add_flag(vig, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_opa(vig, 0, LV_PART_MAIN);
	}

	void setFact(uint idx, Fact fact) override {
		if (!rate_ras.empty() && idx < rate_ras.size()) {
			// Treat the fact as a byte/packet delta and store its per-second rate
			// (divided by `rate_divisor`, e.g. 125000 for Mbit/s), mirroring
			// VideoBitrateWidget. The aggregation/threshold logic runs on rates.
			rate_ras[idx].add(std::lround((double)fact));
			double rate = rate_ras[idx].rate_per_second_over_last_ms(rate_window_ms) / rate_divisor;
			args[idx] = Fact(FactMeta("signal_rate"), rate);
		} else {
			Widget::setFact(idx, fact);
		}
		dirty = true;
	}

	void tick() override {
		if (!vig || !dirty) return;
		dirty = false;

		// Direction is inferred from where 'critical' sits relative to 'threshold':
		//   critical < threshold  -> lower is worse (e.g. RSSI -75/-85)
		//   critical > threshold  -> higher is worse (e.g. fec_recovered 10/20)
		// React to the *most optimistic* reading among all bound facts (the highest
		// when lower-is-worse, the lowest when higher-is-worse), so with multiple
		// antennas the border only appears once even the best one is in warning range.
		const bool higher_is_worse = (critical > threshold);

		bool have = false;
		double best = 0.0;
		for (auto& a : args) {
			if (!a.isDefined()) continue;
			double v = (double)a;
			if (!have) { best = v; have = true; }
			else if (higher_is_worse ? (v < best) : (v > best)) { best = v; }
		}

		// Optional smoothing: feed the aggregated value through a time-windowed
		// running average so brief spikes don't flash the border. RunningAverage
		// works in integers, which is fine for RSSI / fec-style metrics.
		if (have && avg) {
			best = (double)avg->add(std::lround(best));
		}

		double s = 0.0;  // severity 0..1
		if (have) {
			if (critical == threshold) {
				// No ramp: full intensity once past the threshold, either direction.
				bool warn = higher_is_worse ? (best >= threshold) : (best <= threshold);
				s = warn ? 1.0 : 0.0;
			} else {
				// Ramps 0 at threshold -> 1 at critical; the clamp also suppresses
				// the warning entirely while 'best' is on the healthy side.
				s = (best - threshold) / (critical - threshold);
				if (s < 0.0) s = 0.0; else if (s > 1.0) s = 1.0;
			}
		}

		lv_opa_t opa = (lv_opa_t)(s * kMaxOpa);
		int q = opa / 8;  // quantise to avoid churn on small fluctuations
		if (q == last_q) return;
		last_q = q;
		if (opa == 0) {
			lv_obj_add_flag(vig, LV_OBJ_FLAG_HIDDEN);
		} else {
			setHalo(s);  // yellow -> red with severity, alongside the opacity ramp
			lv_obj_set_style_opa(vig, opa, LV_PART_MAIN);
			lv_obj_clear_flag(vig, LV_OBJ_FLAG_HIDDEN);
		}
	}

private:
	// One edge bar: an alpha gradient, opaque at the screen edge (frac 0 when
	// edge_at_start) fading to transparent toward the centre.
	void mkBar(int x, int y, int w, int h, lv_grad_dsc_t* g, lv_grad_dir_t dir, bool edge_at_start) {
		lv_obj_t* bar = lv_obj_create(vig);
		lv_obj_remove_style_all(bar);
		lv_obj_set_pos(bar, x, y);
		lv_obj_set_size(bar, w, h);
		lv_color_t start = lv_color_hex(0xffff00);  // seed yellow; recoloured per severity
		lv_color_t colors[2] = { start, start };
		lv_opa_t   opas[2]   = { edge_at_start ? LV_OPA_COVER : LV_OPA_TRANSP,
		                         edge_at_start ? LV_OPA_TRANSP : LV_OPA_COVER };
		uint8_t    fracs[2]  = { 0, 255 };
		lv_gradient_init_stops(g, colors, opas, fracs, 2);
		g->dir = dir;
		bars[g - grad] = bar;
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_grad(bar, g, LV_PART_MAIN);
	}

	// Fade the halo hue with severity to mirror the opacity ramp: yellow at the
	// threshold (s~0) through to red at critical (s~1). The SW gradient is recomputed
	// from the descriptor on every draw, so mutating the stops in place is enough.
	void setHalo(double s) {
		uint8_t green = (uint8_t)(255.0 * (1.0 - s) + 0.5);
		lv_color_t c = lv_color_make(255, green, 0);
		for (int i = 0; i < 4; i++) {
			grad[i].stops[0].color = c;
			grad[i].stops[1].color = c;
			if (bars[i]) lv_obj_invalidate(bars[i]);
		}
	}

	static constexpr double   kBorderPct = 5.0;  // gradient band, % of screen height
	static constexpr lv_opa_t kMaxOpa    = 216;   // ~0.85 opacity at full severity

	double threshold, critical;
	std::unique_ptr<RunningAverage> avg;   // optional smoothing of the aggregated value (value mode)
	std::vector<RunningAverage> rate_ras;  // per-fact byte-delta -> rate (rate mode); empty otherwise
	uint rate_window_ms = 0;
	double rate_divisor = 1.0;

	lv_grad_dsc_t grad[4]{};   // one persistent descriptor per edge bar
	lv_obj_t* bars[4] = {};    // the four edge bars, indexed like grad[]
	lv_obj_t* vig = nullptr;
	int last_q = -1;
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
		dirty = true;
	}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		lv_chart = lv_chart_create(parent);
		lv_obj_set_size(lv_chart, w, h);
		lv_obj_set_pos(lv_chart, absX(screen_w), absY(screen_h));
		lv_chart_set_type(lv_chart, LV_CHART_TYPE_BAR);
		lv_chart_set_point_count(lv_chart, num_buckets);
		lv_chart_set_range(lv_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
		lv_obj_set_style_bg_opa(lv_chart, LV_OPA_TRANSP, LV_PART_MAIN);

		lv_chart_series_t* ser = lv_chart_add_series(lv_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
		lv_chart_set_next_value(lv_chart, ser, 0);
	}

	void tick() override {
		if (!dirty || !lv_chart) return;
		updateChart();
		dirty = false;
	}

private:
	void updateChart() {
		// Get stats for the entire window
		Stats overall_stats = stats.get_stats_over_last_ms_result(window_ms);
		double value = 0;

		switch(stats_field) {
			case STATS_MIN:
				value = overall_stats.min;
				break;
			case STATS_MAX:
				value = overall_stats.max;
				break;
			case STATS_SUM:
				value = overall_stats.sum;
				break;
			case STATS_COUNT:
				value = overall_stats.count;
				break;
			case STATS_AVG:
				value = overall_stats.average;
				break;
		}

		// Update chart range
		double max_val = value > 1000 ? value : 1000;
		lv_chart_set_range(lv_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)max_val * 1.1);

		// Update chart with current value
		lv_chart_series_t* ser = lv_chart_get_series_next(lv_chart, nullptr);
		if (ser) {
			lv_chart_set_next_value(lv_chart, ser, (int32_t)value);
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
	lv_obj_t* lv_chart = nullptr;
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
		Widget(pos_x, pos_y, num_args), timeout_ms(timeout_ms) {}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		lv_parent = parent;
		ax = absX(screen_w);
		ay = absY(screen_h);
	}

	void setFact(uint /*idx*/, Fact fact) override {
		if (!lv_parent) return;
		std::string msg = fact.getStrValue();
		if (msg.empty()) return;

		lv_obj_t* label = lv_label_create(lv_parent);
		lv_label_set_text(label, msg.c_str());
		lv_obj_set_pos(label, ax, ay);
		lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
		lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(label, LV_OPA_50, LV_PART_MAIN);
		lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
		lv_obj_set_style_radius(label, 4, LV_PART_MAIN);
		lv_obj_set_style_pad_all(label, 8, LV_PART_MAIN);

		// Slide upward
		lv_anim_t a_y;
		lv_anim_init(&a_y);
		lv_anim_set_exec_cb(&a_y, [](void* obj, int32_t v) {
			lv_obj_set_y((lv_obj_t*)obj, v);
		});
		lv_anim_set_var(&a_y, label);
		lv_anim_set_values(&a_y, ay, ay - 150);
		lv_anim_set_duration(&a_y, timeout_ms);
		lv_anim_set_completed_cb(&a_y, [](lv_anim_t* a) {
			lv_obj_delete((lv_obj_t*)a->var);
		});
		lv_anim_start(&a_y);

		// Fade out (animate background and text opacity)
		lv_anim_t a_opa;
		lv_anim_init(&a_opa);
		lv_anim_set_exec_cb(&a_opa, [](void* obj, int32_t v) {
			lv_obj_set_style_bg_opa((lv_obj_t*)obj, (uint8_t)v, LV_PART_MAIN);
			lv_obj_set_style_text_opa((lv_obj_t*)obj, (uint8_t)v, LV_PART_MAIN);
		});
		lv_anim_set_var(&a_opa, label);
		lv_anim_set_values(&a_opa, 255, 0);  // 255=opaque, 0=transparent
		lv_anim_set_duration(&a_opa, timeout_ms);
		lv_anim_start(&a_opa);
	}

private:
	lv_obj_t* lv_parent = nullptr;
	int ax = 0, ay = 0;
	uint timeout_ms;
};

//
// Specific widgets
//

class DvrStatusWidget: public IconTextWidget {
public:
	DvrStatusWidget(int pos_x, int pos_y, std::string icon_path, std::string text) :
		IconTextWidget(pos_x, pos_y, std::move(icon_path), std::move(text)) {
		args.push_back(Fact());
	}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		IconTextWidget::createLvObjects(parent, screen_w, screen_h);
		// Red text, initially hidden
		lv_obj_set_style_text_color(lv_label, lv_color_make(255, 0, 0), LV_PART_MAIN);
		lv_obj_add_flag(lv_icon, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(lv_label, LV_OBJ_FLAG_HIDDEN);
	}

	void setFact(uint idx, Fact fact) override {
		Widget::setFact(idx, fact);
		dirty = true;
	}

	void tick() override {
		if (!dirty || !lv_icon) return;
		updateStatus();
		dirty = false;
	}

private:
	void updateStatus() {
		bool recording = args[0].isDefined() && args[0].getBoolValue();
		if (recording) {
			lv_obj_clear_flag(lv_icon, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(lv_label, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(lv_icon, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(lv_label, LV_OBJ_FLAG_HIDDEN);
		}
	}
};

class VideoWidget: public IconTplTextWidget {
public:
  VideoWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
              std::string icon_path, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, std::move(icon_path), tpl, num_args),
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
		dirty = true;
	}

private:
	RunningAverage fps;
};

class VideoBitrateWidget: public IconTplTextWidget {
public:
  VideoBitrateWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
					 std::string icon_path, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, std::move(icon_path), tpl, num_args),
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
		dirty = true;
	}

private:
	RunningAverage bps;
};

class VideoDecodeLatencyWidget: public IconTplTextWidget {
public:
  VideoDecodeLatencyWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
					 std::string icon_path, std::string tpl, uint num_args) :
		IconTplTextWidget(pos_x, pos_y, std::move(icon_path), tpl, 3),  // 3 args: min/max/avg
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
		dirty = true;
	}

private:
	RunningAverage timing;
};


class GPSWidget: public Widget {
public:
	GPSWidget(int pos_x, int pos_y, uint num_args) :
		Widget(pos_x, pos_y, num_args) {
		assert(num_args == 3);
	}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		lv_label = lv_label_create(parent);
		lv_obj_set_pos(lv_label, absX(screen_w), absY(screen_h));
		lv_obj_set_style_bg_opa(lv_label, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(lv_label, 0, LV_PART_MAIN);
		lv_obj_set_style_text_color(lv_label, lv_color_white(), LV_PART_MAIN);
		lv_label_set_text(lv_label, "GPS: --");
	}

	void setFact(uint idx, Fact fact) override {
		Widget::setFact(idx, fact);
		dirty = true;
	}

	void tick() override {
		if (!dirty) return;
		updateLabel();
		dirty = false;
	}

private:
	void updateLabel() {
		if (!lv_label) return;
		if (!(args[0].isDefined() && args[1].isDefined() && args[2].isDefined())) return;
		const char* fix_type = "undef";
		switch (args[0].getUintValue()) {
		case 0: fix_type = "no GPS"; break;
		case 1: fix_type = "no fix"; break;
		case 2: fix_type = "2D fix"; break;
		case 3: fix_type = "3D fix"; break;
		case 4: fix_type = "DGPS/SBAS 3D"; break;
		case 5: fix_type = "RTK float 3D"; break;
		case 6: fix_type = "RTK Fixed 3D"; break;
		case 7: fix_type = "Static fixed"; break;
		case 8: fix_type = "PPP 3D"; break;
		}
		double lat = args[1].getIntValue() * 1.0e-7;
		double lon = args[2].getIntValue() * 1.0e-7;
		char buf[64];
		snprintf(buf, sizeof(buf), "%s Lat:%f, Lon:%f", fix_type, lat, lon);
		lv_label_set_text(lv_label, buf);
	}

	lv_obj_t* lv_label = nullptr;
};

/**
 * Widget that shows approximate voltage of a battery cell.
 * If the number of cells is 0, it estimates it from the pack voltage based on max_voltage_mv;
 * If the number of cells is -1, it estimates only even cell numbers (2, 4, 6, 8, ...) - this
 * fixes the situation when, eg, discharged to 20v 6s LiIon would be recognized as 5s.
 *
 * Widget's text is drawn in white when battery is above 20% from critical. And below 20% it
 * gradually transitions from yellow through orange to red.
 */
class BatteryCellWidget: public TplTextWidget {
public:
    float warn_percentage = 0.2;

    BatteryCellWidget(int pos_x, int pos_y,
                      int critical_voltage_mv, int max_voltage_mv, int num_cells,
                      std::string tpl, uint num_args) :
        TplTextWidget(pos_x, pos_y, tpl, num_args), critical_voltage_mv(critical_voltage_mv),
        max_voltage_mv(max_voltage_mv), num_cells(num_cells) {
        assert(num_args == 1);
    };

    virtual void setFact(uint idx, Fact fact) {
        assert(idx == 0);
        // replace the pack value with per-cell value
        long voltage_mv = fact.getIntValue();
        int cells;
        if (num_cells > 0) {
            cells = num_cells;
        } else if (num_cells == 0) {
            // estimate any number of cells
            cells = (voltage_mv / max_voltage_mv) + 1;
        } else {
            // estimate even number of cells
            cells = (voltage_mv / max_voltage_mv) + 1;
            if (cells % 2 != 0) {
                cells++;
            }
        }
        long cell_voltage_mv = voltage_mv / cells;
        args[0] = Fact(FactMeta("volts"), (double)cell_voltage_mv / 1000.0);
        dirty = true;
    }


    void updateLvLabel() override {
        TplTextWidget::updateLvLabel();
        if (!lv_label || !args[0].isDefined()) return;
        double cell_voltage_mv = args[0].getDoubleValue() * 1000.0;
        lv_color_t color;
        if (cell_voltage_mv <= critical_voltage_mv) {
            color = lv_color_make(255, 0, 0);
        } else {
            float remaining = (float)(cell_voltage_mv - critical_voltage_mv) /
                              (float)(max_voltage_mv - critical_voltage_mv);
            if (remaining < warn_percentage) {
                uint8_t green = (uint8_t)(255.0f * (remaining / warn_percentage));
                color = lv_color_make(255, green, 0);
            } else {
                color = lv_color_white();
            }
        }
        lv_obj_set_style_text_color(lv_label, color, LV_PART_MAIN);
    }
protected:
    int critical_voltage_mv;
    int max_voltage_mv;
    int num_cells;
};

class DebugWidget: public Widget {
public:
	DebugWidget(int pos_x, int pos_y, uint num_args) :
		Widget(pos_x, pos_y, num_args) {}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		int ax = absX(screen_w), ay = absY(screen_h);
		for (uint i = 0; i < args.size(); i++) {
			lv_obj_t* label = lv_label_create(parent);
			lv_obj_set_pos(label, ax, ay + (int)(i * 36));
			lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
			lv_obj_set_style_bg_opa(label, LV_OPA_50, LV_PART_MAIN);
			lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
			lv_obj_set_style_radius(label, 4, LV_PART_MAIN);
			lv_obj_set_style_pad_all(label, 8, LV_PART_MAIN);
			lv_obj_set_style_text_color(label, lv_color_make(255, 50, 50), LV_PART_MAIN);
			lv_label_set_text(label, "...");
			lv_labels.push_back(label);
		}
	}

	void setFact(uint idx, Fact fact) override {
		Widget::setFact(idx, fact);
		dirty = true;
	}

	void tick() override {
		if (!dirty) return;
		updateLabels();
		dirty = false;
	}

private:
	void updateLabels() {
		for (uint i = 0; i < args.size() && i < lv_labels.size(); i++) {
			if (lv_labels[i]) {
				std::string text = args[i].asVerboseString();
				SPDLOG_INFO("dbg draw {}", text);
				lv_label_set_text(lv_labels[i], text.c_str());
			}
		}
	}

	std::vector<lv_obj_t*> lv_labels;
};

class IconSelectorWidget : public Widget {
public:
    IconSelectorWidget(int pos_x, int pos_y,
                       const std::vector<std::pair<std::pair<int, int>, std::filesystem::path>>& ranges_and_icons,
                       const std::filesystem::path& assets_dir)
        : Widget(pos_x, pos_y)
    {
        args.push_back(Fact());
        for (const auto& [range, icon_path] : ranges_and_icons) {
            std::filesystem::path full = icon_path.is_absolute() ? icon_path : assets_dir / icon_path;
            lv_icon_paths[range] = "A:" + full.string();
        }
    }

    void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
        lv_img = lv_image_create(parent);
        lv_obj_set_pos(lv_img, absX(screen_w), absY(screen_h));
        lv_obj_add_flag(lv_img, LV_OBJ_FLAG_HIDDEN);
    }

    void setFact(uint idx, Fact fact) override {
        assert(idx == 0);
        args[idx] = fact;
        dirty = true;
    }

    void tick() override {
        if (!dirty || !lv_img) return;
        updateIcon();
        dirty = false;
    }

private:
    void updateIcon() {
        if (!args[0].isDefined()) {
            lv_obj_add_flag(lv_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        long value = 0;
        switch (args[0].getType()) {
            case Fact::T_BOOL:   value = args[0].getBoolValue() ? 1 : 0; break;
            case Fact::T_INT:    value = args[0].getIntValue(); break;
            case Fact::T_UINT:   value = static_cast<long>(args[0].getUintValue()); break;
            case Fact::T_DOUBLE: value = static_cast<long>(args[0].getDoubleValue()); break;
            case Fact::T_STRING:
                try { value = std::stol(args[0].getStrValue()); } catch (...) { value = 0; }
                break;
            default:
                lv_obj_add_flag(lv_img, LV_OBJ_FLAG_HIDDEN);
                return;
        }

        for (const auto& [range, path] : lv_icon_paths) {
            if (value >= range.first && value <= range.second) {
                if (current_path != path) {
                    current_path = path;
                    lv_image_set_src(lv_img, path.c_str());
                }
                lv_obj_clear_flag(lv_img, LV_OBJ_FLAG_HIDDEN);
                return;
            }
        }
        lv_obj_add_flag(lv_img, LV_OBJ_FLAG_HIDDEN);
    }

    std::map<std::pair<int, int>, std::string> lv_icon_paths;
    lv_obj_t* lv_img = nullptr;
    std::string current_path;
};

struct DisplayInfo {
	uint8_t char_width;
	uint8_t char_height;
	uint8_t font_width;
	uint8_t font_height;
	uint16_t num_chars;
};

class MspDisplayPortWidget: public Widget {
public:
	MspDisplayPortWidget(int pos_x, int pos_y, const std::string& font_path, uint udp_port = 14551)
		: Widget(pos_x, pos_y), font_path(font_path), udp_port(udp_port),
		  // Betaflight/INAV HD DisplayPort grid (matches msposd's current_display_info).
		  // font_width/height are recomputed from the frame in createLvObjects; the 36x54
		  // seed keeps the native 2:3 glyph aspect ratio.
		  display_info({53, 20, 36, 54, 256}) {}  // Will be scaled to screen in createLvObjects

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		spdlog::info("creating MSP DisplayPort widget on UDP port {}", udp_port);

		// Create UDP socket
		udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
		if (udp_socket < 0) {
			SPDLOG_ERROR("Failed to create UDP socket: {}", strerror(errno));
			return;
		}

		struct sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(udp_port);

		if (bind(udp_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
			SPDLOG_ERROR("Failed to bind UDP socket: {}", strerror(errno));
			close(udp_socket);
			udp_socket = -1;
			return;
		}

		// Socket stays in blocking mode for reader thread

		// Remember the frame size so the grid can be re-sized later when an MSP
		// SET_OPTIONS message selects a different canvas (INAV/Ardupilot/HDZero).
		screen_w_ = screen_w;
		screen_h_ = screen_h;

		// Create the canvas and size it for the current grid.
		canvas = lv_canvas_create(parent);
		configureCanvas();

		// Initialize character map
		memset(character_map, 0, sizeof(character_map));
		last_refresh = std::chrono::steady_clock::now();

		// Font preview: lay out every glyph of every page, the way msposd previews
		// the whole font for a few seconds on startup. Each page (256 glyphs) fills
		// its own block of rows; on the 53x20 grid that is ceil(256/53)=5 rows per
		// page, so all 4 pages tile the screen exactly. Wiped by the first MSP CLEAR
		// once real OSD data starts arriving.
		const int cols = display_info.char_width;
		const int rows = display_info.char_height;
		const int rows_per_page = (256 + cols - 1) / cols;
		for (int page = 0; page < 4; page++) {
			for (int idx = 0; idx < 256; idx++) {
				int row = page * rows_per_page + idx / cols;
				int col = idx % cols;
				if (row >= rows) break;  // ran out of screen
				character_map[row][col] = (uint16_t)((page << 8) | idx);
			}
		}
		// Commit the preview to the front buffer so it presents like a real frame.
		memcpy(render_map, character_map, sizeof(render_map));
		frame_ready.store(true, std::memory_order_release);

		// Load font image
		font_image = lv_image_create(parent);
		lv_image_set_src(font_image, font_path.c_str());
		lv_obj_add_flag(font_image, LV_OBJ_FLAG_HIDDEN);  // Don't display it directly

		// "No data" overlay, centered, shown by tick() when the port goes quiet.
		// Created after the canvas so it draws on top; hidden until needed.
		no_data_label = lv_label_create(parent);
		lv_label_set_text_fmt(no_data_label, "NO MSP DATA\nUDP PORT %u", udp_port);
		lv_obj_set_style_text_font(no_data_label, &lv_font_montserrat_26, 0);
		lv_obj_set_style_text_color(no_data_label, lv_color_white(), 0);
		lv_obj_set_style_text_align(no_data_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_bg_opa(no_data_label, LV_OPA_60, 0);
		lv_obj_set_style_bg_color(no_data_label, lv_color_black(), 0);
		lv_obj_set_style_pad_all(no_data_label, 16, 0);
		lv_obj_set_style_radius(no_data_label, 8, 0);
		lv_obj_align(no_data_label, LV_ALIGN_CENTER, 0, 0);
		lv_obj_add_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);

		// Seed the data clock so the font preview shows for NO_DATA_TIMEOUT_MS before
		// the "no data" message appears if nothing ever arrives.
		last_data_ms.store(nowMs(), std::memory_order_relaxed);

		// Start UDP reader and MSP parser threads
		running = true;
		reader_thread = std::thread(&MspDisplayPortWidget::udpReaderLoop, this);
		parser_thread = std::thread(&MspDisplayPortWidget::mspParserLoop, this);
	}

	void tick() override {
		if (!canvas || !display_buffer) return;

		// Apply a grid change requested by the parser thread (SET_OPTIONS).
		// Canvas resize/realloc touches LVGL objects, so it must run here on the
		// LVGL thread, not in the MSP parser.
		if (pending_grid_change.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> lock(char_map_mutex);
			display_info.char_width  = pending_cols.load();
			display_info.char_height = pending_rows.load();
			memset(character_map, 0, sizeof(character_map));  // dims changed; drop stale glyphs
			memset(render_map, 0, sizeof(render_map));
			configureCanvas();
			pending_grid_change.store(false, std::memory_order_release);
		}

		// Render at ~60Hz independently of main OSD tick
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refresh);
		if (elapsed.count() < 16) return;  // ~60Hz
		last_refresh = now;

		// Show a "no data" message when nothing has been received on the UDP port
		// for a while (never connected, or link lost). Toggled only on transitions.
		bool no_data = (nowMs() - last_data_ms.load(std::memory_order_relaxed)) > NO_DATA_TIMEOUT_MS;
		if (no_data) {
			if (!showing_no_data) {
				showing_no_data = true;
				if (no_data_label) lv_obj_clear_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);
				// Blank the OSD once so the font preview / last frame isn't left behind.
				std::lock_guard<std::mutex> lock(char_map_mutex);
				uint32_t bytes = (uint32_t)display_info.char_width * display_info.font_width
				               * display_info.char_height * display_info.font_height * 4;
				memset(display_buffer, 0, bytes);
				lv_obj_invalidate(canvas);
			}
			return;  // nothing to render until data resumes
		}

		if (showing_no_data) {
			showing_no_data = false;
			if (no_data_label) lv_obj_add_flag(no_data_label, LV_OBJ_FLAG_HIDDEN);
		}

		// Present a committed frame. Commits happen only on frame boundaries
		// (DRAW_SCREEN, or the next CLEAR), so this always draws a whole frame.
		// Between frames nothing is redrawn; the last full frame stays on screen.
		if (frame_ready.exchange(false, std::memory_order_acq_rel)) {
			{
				std::lock_guard<std::mutex> lock(char_map_mutex);
				renderDisplay();
			}
			lv_obj_invalidate(canvas);
		}
	}

	~MspDisplayPortWidget() {
		running = false;
		if (reader_thread.joinable()) reader_thread.join();
		if (parser_thread.joinable()) parser_thread.join();
		if (udp_socket >= 0) close(udp_socket);
		if (display_buffer) free(display_buffer);
		if (canvas) lv_obj_del(canvas);
		if (font_image) lv_obj_del(font_image);
		if (no_data_label) lv_obj_del(no_data_label);
	}

private:
	static int64_t nowMs() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	// (Re)size the canvas for the current grid. The overlay fills the whole frame
	// like msposd's OVERLAY: each cell is font_width x font_height and the glyph is
	// drawn 1:1. Deriving both cell dims from the frame keeps the native ~2:3 glyph
	// aspect (e.g. 1920/53 x 1080/20 = 36x54), so nothing is squeezed.
	// Must run on the LVGL thread.
	void configureCanvas() {
		display_info.font_width  = screen_w_ / display_info.char_width;
		display_info.font_height = screen_h_ / display_info.char_height;

		uint32_t display_width  = display_info.char_width  * display_info.font_width;
		uint32_t display_height = display_info.char_height * display_info.font_height;

		if (display_buffer) free(display_buffer);
		display_buffer = (uint32_t*)malloc(display_width * display_height * 4);
		memset(display_buffer, 0, display_width * display_height * 4);

		lv_canvas_set_buffer(canvas, display_buffer, display_width, display_height, LV_COLOR_FORMAT_ARGB8888);
		int canvas_x = absX(screen_w_);
		int canvas_y = absY(screen_h_);
		lv_obj_set_pos(canvas, canvas_x, canvas_y);
		lv_obj_set_size(canvas, display_width, display_height);

		spdlog::info("MSP canvas: grid {}x{} cell {}x{} -> {}x{} @ ({},{}) screen {}x{}",
			display_info.char_width, display_info.char_height,
			display_info.font_width, display_info.font_height,
			display_width, display_height, canvas_x, canvas_y, screen_w_, screen_h_);
	}

	// Map an MSP DisplayPort SET_OPTIONS resolution (msp_hd_options_e) to a char
	// grid. Betaflight never sends SET_OPTIONS and stays on the 53x20 default;
	// INAV/Ardupilot/HDZero use this to pick their canvas.
	static bool gridForOption(uint8_t opt, uint8_t& cols, uint8_t& rows) {
		switch (opt) {
			case 0: cols = 30; rows = 16; return true;  // MSP_SD_OPTION_30_16 (analog)
			case 1: cols = 50; rows = 18; return true;  // MSP_HD_OPTION_50_18 (HDZero)
			case 2: cols = 30; rows = 16; return true;  // MSP_HD_OPTION_30_16
			case 3: cols = 60; rows = 22; return true;  // MSP_HD_OPTION_60_22
			default: return false;
		}
	}

	void udpReaderLoop() {
		pthread_setname_np(pthread_self(), "MSP-UDP-Reader");
		spdlog::info("MSP UDP reader thread started");
		uint8_t buffer[1024];
		struct sockaddr_in src_addr;
		socklen_t src_len = sizeof(src_addr);

		while (running) {
			int recv_len = recvfrom(udp_socket, buffer, sizeof(buffer), 0,
									 (struct sockaddr*)&src_addr, &src_len);
			if (recv_len > 0) {
				last_data_ms.store(nowMs(), std::memory_order_relaxed);  // link is alive
				static uint32_t packet_count = 0;
				packet_count++;
				if (packet_count <= 5) {
					SPDLOG_DEBUG("Packet #{}: {} bytes", packet_count, recv_len);
				} else if (packet_count % 100 == 0) {
					spdlog::debug("MSP: received {} bytes (packet #{})", recv_len, packet_count);
				}

				std::vector<uint8_t> packet(buffer, buffer + recv_len);
				{
					std::lock_guard<std::mutex> lock(packet_queue_mutex);
					packet_queue.push(packet);
				}
			}
		}
		spdlog::info("MSP UDP reader thread stopped");
	}

	void mspParserLoop() {
		pthread_setname_np(pthread_self(), "MSP-Parser");
		spdlog::info("MSP parser thread started");
		while (running) {
			std::vector<uint8_t> packet;
			{
				std::unique_lock<std::mutex> lock(packet_queue_mutex);
				if (packet_queue.empty()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				packet = packet_queue.front();
				packet_queue.pop();
			}

			for (uint8_t byte : packet) {
				processMspByte(byte);
			}
		}
		spdlog::info("MSP parser thread stopped");
	}

	void processMspByte(uint8_t byte) {
		// Simple MSP frame parser
		// MSP v1: $ M < len cmd payload chk
		enum { SYNC1, SYNC2, DIR, LEN, CMD, PAYLOAD, CHK };

		switch (msp_state) {
			case SYNC1:
				if (byte == '$') {
					msp_state = SYNC2;
				}
				break;
			case SYNC2:
				if (byte == 'M') {
					msp_state = DIR;
				} else {
					msp_state = SYNC1;
				}
				break;
			case DIR:
				// Accept both $M< (inbound) and $M> (outbound)
				if (byte == '<' || byte == '>') {
					msp_state = LEN;
				} else {
					msp_state = SYNC1;
				}
				break;
			case LEN:
				msp_len = byte;
				msp_checksum = msp_len ^ 0;  // Start checksum
				msp_state = CMD;
				break;
			case CMD:
				msp_cmd = byte;
				msp_checksum ^= msp_cmd;
				msp_payload_idx = 0;
				msp_state = (msp_len > 0) ? PAYLOAD : CHK;
				break;
			case PAYLOAD:
				msp_payload[msp_payload_idx++] = byte;
				msp_checksum ^= byte;
				if (msp_payload_idx >= msp_len) msp_state = CHK;
				break;
			case CHK:
				if (msp_cmd == 182) {  // MSP_CMD_DISPLAYPORT
					if (msp_checksum == byte) {
						handleDisplayPort(msp_payload, msp_len);
					} else {
						spdlog::warn("MSP: Checksum mismatch (expected {} got {})", msp_checksum, byte);
					}
				}
				msp_state = SYNC1;
				break;
		}
	}

	void handleDisplayPort(uint8_t* payload, uint8_t len) {
		if (len < 1) return;

		uint8_t subcmd = payload[0];
		switch (subcmd) {
			case 0:  // HEARTBEAT / KEEPALIVE - no frame action
			case 1:  // RELEASE / CLOSE - no frame action
				break;
			case 2:  // CLEAR: a new frame starts here, so the previous frame is now
				// complete. Commit it, then wipe. This is the frame boundary for
				// senders that never emit DRAW_SCREEN (e.g. the SITL harness, which
				// only sends CLEAR + DRAW_STRING + HEARTBEAT).
				spdlog::debug("MSP: CLEAR screen");
				{
					std::lock_guard<std::mutex> lock(char_map_mutex);
					memcpy(render_map, character_map, sizeof(render_map));
					frame_ready.store(true, std::memory_order_release);
					memset(character_map, 0, sizeof(character_map));
				}
				break;
			case 3:  // DRAW_STRING: write into the working buffer
				if (len >= 4) {
					uint8_t row = payload[1];
					uint8_t col = payload[2];
					uint8_t attrs = payload[3];
					uint8_t page = attrs & 0x3;
					uint8_t str_len = 0;

					for (uint8_t i = 4; i < len; i++) {
						if (payload[i] == '\0') break;
						uint16_t char_code = payload[i] | (page << 8);

						if (col < display_info.char_width && row < display_info.char_height) {
							character_map[row][col] = char_code;
							str_len++;
						}
						col++;
					}
					if (str_len > 0) {
						spdlog::debug("MSP: DRAW_STRING row={} col={} page={} chars={}", row, col-str_len, page, str_len);
					}
				}
				break;
			case 4: {  // DRAW_SCREEN: frame finished -> commit working buffer to the
				// front buffer so tick() presents a whole frame, not a half-drawn one.
				std::lock_guard<std::mutex> lock(char_map_mutex);
				memcpy(render_map, character_map, sizeof(render_map));
				frame_ready.store(true, std::memory_order_release);
				break;
			}
			case 5:  // SET_OPTIONS (INAV/Ardupilot/HDZero; Betaflight never sends this)
				if (len >= 3) {
					uint8_t font = payload[1];
					uint8_t is_hd = payload[2];
					uint8_t cols = 0, rows = 0;
					if (gridForOption(is_hd, cols, rows)) {
						// Stage the resize; tick() applies it on the LVGL thread.
						if (cols != display_info.char_width || rows != display_info.char_height) {
							spdlog::info("MSP: SET_OPTIONS font={} opt={} -> grid {}x{}", font, is_hd, cols, rows);
							pending_cols.store(cols);
							pending_rows.store(rows);
							pending_grid_change.store(true, std::memory_order_release);
						}
					} else {
						spdlog::debug("MSP: SET_OPTIONS font={} opt={} (unknown resolution, ignored)", font, is_hd);
					}
				}
				break;
			default:
				spdlog::debug("MSP: Unknown/unhandled DisplayPort subcmd={} len={}", subcmd, len);
				break;
		}
	}

	void loadFontAtlas() {
		if (font_data) return;  // Already loaded

		spdlog::info("Loading font atlas from: {}", font_path);

		// Open PNG file
		FILE* fp = fopen(font_path.c_str(), "rb");
		if (!fp) {
			spdlog::warn("Failed to open font file: {}", font_path);
			return;
		}

		// Create PNG read structure
		png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
		if (!png) {
			fclose(fp);
			spdlog::warn("Failed to create PNG read struct");
			return;
		}

		png_infop info = png_create_info_struct(png);
		if (!info) {
			png_destroy_read_struct(&png, nullptr, nullptr);
			fclose(fp);
			spdlog::warn("Failed to create PNG info struct");
			return;
		}

		// Set error handler (use setjmp for libpng error handling)
		if (setjmp(png_jmpbuf(png))) {
			png_destroy_read_struct(&png, &info, nullptr);
			fclose(fp);
			spdlog::warn("PNG read error");
			return;
		}

		png_init_io(png, fp);
		png_read_info(png, info);

		uint32_t width = png_get_image_width(png, info);
		uint32_t height = png_get_image_height(png, info);

		// Get color type before transformations
		png_byte color_type = png_get_color_type(png, info);
		png_byte bit_depth = png_get_bit_depth(png, info);

		spdlog::debug("PNG color_type={} bit_depth={}", (int)color_type, (int)bit_depth);

		// Transform to RGBA8
		if (bit_depth == 16) png_set_strip_16(png);
		if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
		if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
		if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
		if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
		// Add alpha channel if not present
		if (color_type == PNG_COLOR_TYPE_RGB) png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);

		png_read_update_info(png, info);

		// Check actual rowbytes after transformation
		uint32_t rowbytes = png_get_rowbytes(png, info);
		spdlog::debug("After transforms: rowbytes={} (expected={})", rowbytes, width * 4);

		// Allocate row pointers
		png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
		for (uint32_t y = 0; y < height; y++) {
			row_pointers[y] = (png_byte*)malloc(rowbytes);
		}

		// Read PNG data
		png_read_image(png, row_pointers);

		// Convert to uint32_t ARGB buffer
		uint32_t num_pixels = width * height;
		font_data = (uint32_t*)malloc(num_pixels * sizeof(uint32_t));
		if (font_data) {
			for (uint32_t y = 0; y < height; y++) {
				uint8_t* row = row_pointers[y];
				for (uint32_t x = 0; x < width; x++) {
					uint32_t idx = x * 4;
					if (idx + 3 < rowbytes) {
						uint8_t r = row[idx + 0];
						uint8_t g = row[idx + 1];
						uint8_t b = row[idx + 2];
						uint8_t a = row[idx + 3];
						font_data[y * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
					}
				}
			}
			font_width_atlas = width;
			font_height_atlas = height;

			// Debug: sample pixels from different rows and columns to verify
			spdlog::debug("Font atlas pixel samples (ARGB):");
			uint32_t test_indices[] = {0, width/4, width/2, 3*width/4, width-1};
			for (uint32_t idx : test_indices) {
				if (idx < num_pixels) {
					uint32_t pixel = font_data[idx];
					uint8_t a = (pixel >> 24) & 0xFF;
					uint8_t r = (pixel >> 16) & 0xFF;
					uint8_t g = (pixel >> 8) & 0xFF;
					uint8_t b = pixel & 0xFF;
					spdlog::debug("  [{}] ARGB={:02x}{:02x}{:02x}{:02x}", idx, a, r, g, b);
				}
			}

			spdlog::info("Font atlas loaded: {}x{} pixels, {} bytes",
				font_width_atlas, font_height_atlas, num_pixels * sizeof(uint32_t));
		}

		// Clean up
		for (uint32_t y = 0; y < height; y++) {
			free(row_pointers[y]);
		}
		free(row_pointers);
		png_destroy_read_struct(&png, &info, nullptr);
		fclose(fp);
	}

	void renderDisplay() {
		if (!canvas || !display_buffer) {
			SPDLOG_WARN("renderDisplay: canvas={} buffer={}", (void*)canvas, (void*)display_buffer);
			return;
		}

		uint32_t display_width = display_info.char_width * display_info.font_width;
		uint32_t display_height = display_info.char_height * display_info.font_height;

		// Clear buffer with transparent background so video shows through
		uint32_t* buf = display_buffer;
		uint32_t transparent = 0x00000000;  // ARGB8888: fully transparent
		for (uint32_t i = 0; i < display_width * display_height; i++) {
			buf[i] = transparent;
		}

		// Load font atlas if not loaded yet
		if (!font_data) {
			loadFontAtlas();
			if (!font_data) {
				SPDLOG_WARN("renderDisplay: font_data still null after loadFontAtlas()");
			} else {
				SPDLOG_DEBUG("renderDisplay: font atlas loaded, {}x{}", font_width_atlas, font_height_atlas);
			}
		}

		// Count and draw characters from the committed front buffer.
		int char_count = 0;
		for (uint8_t row = 0; row < display_info.char_height; row++) {
			for (uint8_t col = 0; col < display_info.char_width; col++) {
				uint16_t char_code = render_map[row][col];
				if (char_code == 0) continue;

				uint8_t page = (char_code >> 8) & 0x3;
				uint8_t char_idx = char_code & 0xFF;

				// Atlas is organized as N pages (columns) × 256 chars (rows).
				// Derive the page count from the atlas geometry instead of
				// hardcoding it, the way msposd does (font_pages = width / glyph_w).
				// BF/INAV/HDZero/Ardu glyphs are all ~2:3 (w:h), so the native glyph
				// width is atlas_char_height * 2/3 regardless of the on-screen cell
				// size. This gives 4 pages for btfl, 2 for inav, 1 for ardu, avoiding
				// horizontal squeeze on 1- and 2-page fonts.
				uint32_t atlas_char_height = font_height_atlas / 256;
				uint32_t glyph_w = atlas_char_height * 2 / 3;
				uint32_t font_pages = (glyph_w > 0) ? (font_width_atlas / glyph_w) : 4;
				if (font_pages == 0) font_pages = 1;
				uint32_t atlas_char_width = font_width_atlas / font_pages;

				// Calculate source position in font atlas
				int src_x = page * atlas_char_width;
				int src_y = char_idx * atlas_char_height;

				// Debug first character
				if (char_count == 0) {
					SPDLOG_DEBUG("First char: code=0x{:04x} page={} idx={} atlas_char_size={}x{}",
						char_code, page, char_idx, atlas_char_width, atlas_char_height);
				}

				// Calculate destination position on canvas
				int dst_x = col * display_info.font_width;
				int dst_y = row * display_info.font_height;

				// Copy character from atlas to canvas buffer with scaling
				if (font_data && font_width_atlas > 0 && font_height_atlas > 0) {
					for (int y = 0; y < (int)display_info.font_height && dst_y + y < (int)display_height; y++) {
						for (int x = 0; x < (int)display_info.font_width && dst_x + x < (int)display_width; x++) {
							// Scale from display coordinates back to atlas coordinates
							int atlas_x = src_x + (x * atlas_char_width) / display_info.font_width;
							int atlas_y = src_y + (y * atlas_char_height) / display_info.font_height;

							// Bounds check on atlas
							if (atlas_x >= 0 && atlas_x < (int)font_width_atlas &&
								atlas_y >= 0 && atlas_y < (int)font_height_atlas) {
								// Copy pixel from atlas to canvas
								uint32_t pixel = font_data[atlas_y * font_width_atlas + atlas_x];
								buf[(dst_y + y) * display_width + (dst_x + x)] = pixel;
							}
						}
					}
					char_count++;
				} else if (!font_data) {
					// Fallback: draw placeholder rectangle
					uint8_t brightness = 200 + ((char_code & 0x3F) >> 1);
					uint32_t color = (0xFF << 24) | (brightness << 16) | (brightness << 8) | brightness;
					for (int y = 0; y < (int)display_info.font_height && dst_y + y < (int)display_height; y++) {
						for (int x = 0; x < (int)display_info.font_width && dst_x + x < (int)display_width; x++) {
							buf[(dst_y + y) * display_width + (dst_x + x)] = color;
						}
					}
					char_count++;
				}
			}
		}

		if (char_count > 0) {
			SPDLOG_DEBUG("renderDisplay: {} characters drawn", char_count);
		}
	}

	int udp_socket = -1;
	lv_obj_t* canvas = nullptr;
	lv_obj_t* font_image = nullptr;
	uint32_t* display_buffer = nullptr;
	uint32_t* font_data = nullptr;  // Decoded font atlas pixels
	uint32_t font_width_atlas = 0;
	uint32_t font_height_atlas = 0;
	std::string font_path;
	std::vector<uint8_t> png_buffer;  // Keep PNG file in memory for decoder
	uint udp_port;

	DisplayInfo display_info;
	int screen_w_ = 0;
	int screen_h_ = 0;

	// "No data" overlay shown when the UDP port goes quiet.
	static constexpr int64_t NO_DATA_TIMEOUT_MS = 10000;
	lv_obj_t* no_data_label = nullptr;
	std::atomic<int64_t> last_data_ms{0};
	bool showing_no_data = false;

	// Grid change requested by the parser thread (SET_OPTIONS), applied in tick().
	std::atomic<bool> pending_grid_change{false};
	std::atomic<uint8_t> pending_cols{0};
	std::atomic<uint8_t> pending_rows{0};

	uint16_t character_map[32][64];  // Working buffer (parser thread): CLEAR/DRAW_STRING
	uint16_t render_map[32][64];     // Front buffer: committed on DRAW_SCREEN, read by renderDisplay

	// Frame commit: only present whole frames, committed on a frame boundary
	// (DRAW_SCREEN, or the next CLEAR), to avoid the top-to-bottom tearing you get
	// when rendering the working buffer mid-update.
	std::atomic<bool> frame_ready{false};
	std::chrono::steady_clock::time_point last_refresh;

	// Threading
	std::thread reader_thread;
	std::thread parser_thread;
	std::atomic<bool> running = false;
	std::queue<std::vector<uint8_t>> packet_queue;
	std::mutex packet_queue_mutex;
	std::mutex char_map_mutex;

	// MSP parser state (per-instance, not static)
	int msp_state = 0;  // SYNC1
	uint8_t msp_len = 0;
	uint8_t msp_cmd = 0;
	uint8_t msp_checksum = 0;
	uint8_t msp_payload[256];
	uint8_t msp_payload_idx = 0;
};


class ExternalSurfaceWidget: public Widget {
public:
	ExternalSurfaceWidget(int pos_x, int pos_y, std::string shm_name, uint refresh_interval_ms = 16)
		: Widget(pos_x, pos_y), shm_name(shm_name), refresh_interval_ms(refresh_interval_ms) {}

	void createLvObjects(lv_obj_t* parent, int screen_w, int screen_h) override {
		SPDLOG_INFO("creating shm region {} (refresh {}ms)", shm_name, refresh_interval_ms);

		shm_size = sizeof(SharedMemoryRegion) + (screen_w * screen_h * 4);

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

		auto *shm_region = static_cast<SharedMemoryRegion*>(
			mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
		);
		if (shm_region == MAP_FAILED) {
			perror("Failed to map shared memory");
			shm_unlink(shm_name.c_str());
			return;
		}

		shm_region->width = screen_w;
		shm_region->height = screen_h;

		canvas = lv_canvas_create(parent);
		lv_canvas_set_buffer(canvas, (void*)shm_region->data, screen_w, screen_h, LV_COLOR_FORMAT_ARGB8888);
		lv_obj_set_pos(canvas, absX(screen_w), absY(screen_h));

		shm_data = reinterpret_cast<unsigned char*>(shm_region);
		last_refresh = std::chrono::steady_clock::now();
	}

	void tick() override {
		if (!canvas) return;

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refresh);

		if (elapsed.count() >= refresh_interval_ms) {
			lv_obj_invalidate(canvas);
			last_refresh = now;
		}
	}

	~ExternalSurfaceWidget() {
		SPDLOG_INFO("bye, bye, shm region {}", shm_name);
		if (canvas) {
			lv_obj_del(canvas);
		}
		if (shm_data) {
			munmap(shm_data, shm_size);
		}
		shm_unlink(shm_name.c_str());
	}

protected:
	lv_obj_t *canvas = nullptr;
	unsigned char *shm_data = nullptr;
	size_t shm_size = 0;
	std::string shm_name;
	uint refresh_interval_ms;
	std::chrono::steady_clock::time_point last_refresh;
};


class Osd {
public:
	void createWidgets(lv_obj_t* parent, int screen_w, int screen_h) {
		for (auto& widget : widgets)
			widget->createLvObjects(parent, screen_w, screen_h);
	}

	void tick() {
		for (auto& widget : widgets)
			widget->tick();
	}

	void loadConfig(json cfg) {
		json obj;
		if (cfg.contains("format")) {
			auto cfg_format = cfg.at("format").template get<std::string>();
			if (cfg_format != "0.0.1" && cfg_format != "0.0.2") {
				spdlog::warn("Unexpected OSD config format: {}. OSD may look wrong", cfg_format);
			}
		} else {
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
				if (matcher_j.contains("convert")) {
					auto expression_str = matcher_j.at("convert").template get<std::string>();
					try {
						matchers.push_back(FactMatcher(matcher_name, tags, expression_str));
					} catch (const ExpressionException& e) {
						spdlog::error("Invalid convert expression {}: {}",
									  expression_str, e.what());
					}
				} else {
					matchers.push_back(FactMatcher(matcher_name, tags));
				}
			}
			if (type == "TextWidget") {
				addWidget(new TextWidget(x, y, widget_j.at("text").template get<std::string>()),
						  matchers);
			} else if (type == "ExternalSurfaceWidget") {
				if (!widget_j.contains("shm_name")) {
					spdlog::error("ExternalSurfaceWidget '{}' missing required 'shm_name'", name);
					continue;
				}
				auto shm_name = widget_j.at("shm_name").template get<std::string>();
				uint refresh_ms = 16;  // Default ~60Hz
				if (widget_j.contains("refresh_interval_ms")) {
					refresh_ms = widget_j.at("refresh_interval_ms").template get<uint>();
				}
				addWidget(new ExternalSurfaceWidget(x, y, shm_name, refresh_ms), matchers);
			} else if (type == "MspDisplayPortWidget") {
				if (!widget_j.contains("font_path")) {
					spdlog::error("MspDisplayPortWidget '{}' missing required 'font_path'", name);
					continue;
				}
				auto font_path = widget_j.at("font_path").template get<std::string>();
				uint udp_port = 14551;  // Default MSP DisplayPort UDP port
				if (widget_j.contains("udp_port")) {
					udp_port = widget_j.at("udp_port").template get<uint>();
				}
				addWidget(new MspDisplayPortWidget(x, y, font_path, udp_port), matchers);
			} else if (type == "IconSelectorWidget") {
				std::vector<std::pair<std::pair<int, int>, std::filesystem::path>> ranges_and_icons;
				for (const auto& range_icon : widget_j.at("ranges_and_icons")) {
					int range_start = range_icon.at("range")[0];
					int range_end = range_icon.at("range")[1];
					std::filesystem::path icon_path = range_icon.at("icon_path");
					ranges_and_icons.push_back({{range_start, range_end}, icon_path});
				}
				addWidget(new IconSelectorWidget(x, y, ranges_and_icons, assets_dir), matchers);
			} else if (type == "TplTextWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				addWidget(new TplTextWidget(x, y, tpl, (uint)matchers.size()), matchers);
			} else if(type == "IconTplTextWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				addWidget(new IconTplTextWidget(x, y, lvIconPath(assets_dir, icon_path),
				                               tpl, (uint)matchers.size()), matchers);
			} else if(type == "DvrStatusWidget") {
				auto text = widget_j.at("text").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				addWidget(new DvrStatusWidget(x, y, lvIconPath(assets_dir, icon_path), text), matchers);
			} else if(type == "VideoWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();
				addWidget(new VideoWidget(x, y, window_size_s * 1000, bucket_size_ms,
				                         lvIconPath(assets_dir, icon_path), tpl, (uint)matchers.size()),
						  matchers);
			} else if(type == "VideoBitrateWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();
				addWidget(new VideoBitrateWidget(x, y, window_size_s * 1000, bucket_size_ms,
				                                lvIconPath(assets_dir, icon_path), tpl, (uint)matchers.size()),
						  matchers);
			} else if(type == "VideoDecodeLatencyWidget") {
				auto tpl = widget_j.at("template").template get<std::string>();
				auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
				uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
				uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();
				addWidget(new VideoDecodeLatencyWidget(x, y, window_size_s * 1000, bucket_size_ms,
				                                      lvIconPath(assets_dir, icon_path), tpl, 1),
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
			} else if(type == "SignalWarningWidget") {
				if (matchers.empty()) {
					spdlog::error("SignalWarningWidget '{}' needs at least one fact", name);
					continue;
				}
				double threshold = widget_j.at("threshold").template get<double>();
				// Value at which the border reaches full intensity. Omit for an
				// on/off effect (full intensity once past threshold).
				double critical = threshold;
				if (widget_j.contains("critical")) {
					critical = widget_j.at("critical").template get<double>();
				}
				// Optional: smooth the aggregated value over this many ms (0 = off).
				// In rate mode this is the rate window instead (default 1000).
				int average_ms = 0;
				if (widget_j.contains("average_ms")) {
					average_ms = widget_j.at("average_ms").template get<int>();
				}
				// Optional: treat facts as byte/packet deltas and warn on their
				// per-second rate (like VideoBitrateWidget). 'rate_divisor' scales
				// the rate, e.g. 125000 for bytes/s -> Mbit/s.
				bool rate_mode = false;
				if (widget_j.contains("rate")) {
					rate_mode = widget_j.at("rate").template get<bool>();
				}
				double rate_divisor = 1.0;
				if (widget_j.contains("rate_divisor")) {
					rate_divisor = widget_j.at("rate_divisor").template get<double>();
				}
				addWidget(new SignalWarningWidget(x, y, threshold, critical, (uint)matchers.size(),
				                                  average_ms, rate_mode, rate_divisor), matchers);
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
            } else if (type == "BatteryCellWidget") {
                int critical_mv = 3500;
                int max_mv = 4200;
                int num_cells = -1;
				auto tpl = widget_j.at("template").template get<std::string>();
                if (widget_j.contains("critical_voltage")) {
                    critical_mv = (int)(widget_j.at("critical_voltage").template get<float>() * 1000);
                }
                if (widget_j.contains("max_voltage")) {
                    max_mv = (int)(widget_j.at("max_voltage").template get<float>() * 1000);
                }
                if (widget_j.contains("num_cells")) {
                    std::string cells = widget_j["num_cells"];
                    if (cells == "auto") {
                        num_cells = 0;
                    } else if (cells == "even") {
                        num_cells = -1;
                    } else {
                        num_cells = widget_j["num_cells"].get<int>();
                    }
                }
                assert(critical_mv < max_mv);
                addWidget(new BatteryCellWidget(x, y, critical_mv, max_mv, num_cells,
                                                tpl, (uint)matchers.size()),
                          matchers);
			} else if (type == "PopupWidget") {
				auto timeout_ms = widget_j.at("timeout_ms").template get<uint>();
				addWidget(new PopupWidget(x, y, timeout_ms, (uint)matchers.size()),
						  matchers);
			} else if (type == "DebugWidget") {
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

	// Reset the bound facts whose name starts with one of `prefixes` to undefined.
	// The sources owning them re-publish right after, so this is a clean slate for
	// stale facts (e.g. the previous RX mode's link stats). Scoped by name rather
	// than clearing every widget: facts published only once per stream, like
	// video.width/height (only on a decoder frame-info change) or video.codec, have
	// nobody to re-publish them and their widgets would render '?' for good.
	// Must run on the fact-processor thread (under lvgl_mutex), like setFact.
	void flushFacts(const std::vector<std::string>& prefixes) {
		for (auto& [matcher, widget, arg_idx] : matchers) {
			for (const auto& prefix : prefixes) {
				if (matcher.name.rfind(prefix, 0) == 0) {
					widget->clearFact(arg_idx);
					break;
				}
			}
		}
	}

	void setFact(Fact fact) {
		for (auto& [matcher, widget, arg_idx] : matchers) {
			if (matcher.matches(fact)) {
				try {
					Fact converted_fact = matcher.convert(fact);
					widget->setFact(arg_idx, converted_fact);
				} catch (const ExpressionException& e) {
					spdlog::error("Failed to evaluate 'convert' expression for {}: {}",
								  fact.asVerboseString(), e.what());
				}
			}
		}
	};

private:

	static std::string lvIconPath(const std::filesystem::path& base, std::filesystem::path icon) {
		if (icon.is_relative()) icon = base / icon;
		return "A:" + icon.string();
	}

	std::vector<Widget *> widgets;
	std::vector<std::tuple<FactMatcher, Widget *, uint>> matchers;
};


std::queue<Fact> fact_queue;
std::vector<std::string> pending_flush_prefixes;  // guarded by mtx; set by osd_flush_facts()
std::mutex mtx;
std::mutex lvgl_mutex;  // Protects all LVGL operations
std::condition_variable cv;
pthread_mutex_t osd_mutex;

int osd_thread_signal;

typedef struct png_closure
{
	unsigned char * iter;
	unsigned int bytes_left;
} png_closure_t;

/* LVGL renders into this cached (normal RAM) shadow buffer instead of the
 * write-combined DRM dumb buffers. Alpha blending is read-modify-write, and
 * uncached reads on ARM are extremely slow — rendering directly into the dumb
 * buffers is what made the menu sluggish. The shadow always holds the complete
 * current UI; my_flush_cb copies it to the off-screen DRM buffer and flips. */
static uint8_t * lvgl_shadow;

void my_flush_cb(lv_display_t * display, const lv_area_t * area, uint8_t * px_map)
{
	(void)area; (void)px_map;

	/* Direct mode calls flush once per invalidated area. Acting on the
	 * intermediate calls used to flip the visible buffer mid-frame — the
	 * display thread commits on every video frame, so scanout could catch a
	 * half-rendered frame (rows blinking out during navigation, stale fade
	 * ghosts). Only publish once the frame is complete. */
	if (!lv_display_flush_is_last(display)) {
		lv_display_flush_ready(display);
		return;
	}

	/* osd_buf_switch is only ever flipped on this thread, so the back buffer
	 * is stable: copy without the lock. */
	int back = p->out->osd_buf_switch ^ 1;
	struct modeset_buf *dst = &p->out->osd_bufs[back];
	memcpy(dst->map, lvgl_shadow, dst->size);

	int ret = pthread_mutex_lock(&osd_mutex);
	assert(!ret);
	p->out->osd_buf_switch = back;
	if (enable_live_colortrans) {
		dst->gl_fb_id = osd_gl_process(dst);
	}
	ret = pthread_mutex_unlock(&osd_mutex);
	assert(!ret);

	if (dvr_osd && frame_proc)
		frame_proc->set_osd_blend(dst->prime_fd, dst->width, dst->height,
		                         dst->stride / 4);

	// tell the display thread that we have a update
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);
	osd_update_ready = true;
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);

    /* IMPORTANT!!!
     * Inform LVGL that flushing is complete so buffer can be modified again. */
    lv_display_flush_ready(display);
}

uint32_t my_get_milliseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

lv_display_t * display;
static lv_draw_buf_t lvgl_draw_buf1;

void setup_lvgl(osd_thread_params *p) {

	/* Initialize LVGL. */
    lv_init();

	struct modeset_buf *buf1 = &p->out->osd_bufs[0];

	display = lv_display_create(buf1->width, buf1->height);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);

	/* Single cached shadow buffer as the one draw buffer (see my_flush_cb).
	 * With one buffer LVGL renders in place and never runs its dual-buffer
	 * dirty-area sync (which read back from uncached DRM memory every frame).
	 * Keep the DRM pitch (buf->stride) so the shadow's row layout matches the
	 * hardware-aligned scanline stride and the flush copy is one flat memcpy. */
	size_t shadow_sz = ((size_t)buf1->size + 63) & ~(size_t)63;
	lvgl_shadow = (uint8_t *)aligned_alloc(64, shadow_sz);
	assert(lvgl_shadow);
	memset(lvgl_shadow, 0, shadow_sz);
	lv_draw_buf_init(&lvgl_draw_buf1, buf1->width, buf1->height,
	                 LV_COLOR_FORMAT_ARGB8888, buf1->stride, lvgl_shadow, buf1->size);
	lv_display_set_draw_buffers(display, &lvgl_draw_buf1, NULL);
	lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_DIRECT);

	lv_display_set_flush_cb(display, my_flush_cb);

	lv_tick_set_cb(my_get_milliseconds);

    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, LV_PART_MAIN);

}

static Osd *g_osd = nullptr;  // Global for fact processor thread

void factProcessorLoop() {
	pthread_setname_np(pthread_self(), "OSD-FactProc");
	spdlog::info("OSD fact processor thread started");

	while (!osd_thread_signal) {
		std::vector<Fact> fact_buf;
		std::vector<std::string> flush_prefixes;
		{
			std::unique_lock<std::mutex> lock(mtx);
			// Wait for facts with 1ms timeout (continuous processing)
			cv.wait_for(lock, std::chrono::milliseconds(1),
				[/*fact_queue*/] { return !fact_queue.empty(); });

			// Collect all available facts
			while (!fact_queue.empty()) {
				SPDLOG_DEBUG("got fact {}", fact_queue.front().asVerboseString());
				fact_buf.push_back(fact_queue.front());
				fact_queue.pop();
			}
			flush_prefixes.swap(pending_flush_prefixes);
		}

		// Process facts with LVGL lock protection
		if (g_osd) {
			std::lock_guard<std::mutex> lock(lvgl_mutex);
			// Flush first, so facts published after the flush request survive.
			if (!flush_prefixes.empty()) {
				g_osd->flushFacts(flush_prefixes);
			}
			for (const Fact& fact : fact_buf) {
				g_osd->setFact(fact);
			}
		}
	}
	spdlog::info("OSD fact processor thread stopped");
}

void *__OSD_THREAD__(void *param) {
	p = (osd_thread_params *)param;
	Osd *osd = new Osd;
	g_osd = osd;  // Store for fact processor thread
	pthread_setname_np(pthread_self(), "__OSD");

	osd->loadConfig(p->config);
	auto last_lvgl_tick_at = std::chrono::steady_clock::now();

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane,
								  buf->fb, buf->width, buf->height, osd_zpos);

	if (!osd_gl.init(p->fd, buf->width, buf->height,
						live_colortrans_gain, live_colortrans_offset)) {
		spdlog::warn("OSD GL: init failed");
	}

	// LVGL is always initialized: OSD rendering now uses LVGL widgets
	setup_lvgl(p);

	// Full-screen transparent container on layer_bottom (below menu layer)
	{
		int screen_w = buf->width;
		int screen_h = buf->height;
		lv_obj_t* osd_cont = lv_obj_create(lv_layer_bottom());
		lv_obj_set_pos(osd_cont, 0, 0);
		lv_obj_set_size(osd_cont, screen_w, screen_h);
		lv_obj_set_style_bg_opa(osd_cont, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(osd_cont, 0, LV_PART_MAIN);
		lv_obj_set_style_radius(osd_cont, 0, LV_PART_MAIN);
		lv_obj_set_style_pad_all(osd_cont, 0, LV_PART_MAIN);
		lv_obj_clear_flag(osd_cont, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_text_font(osd_cont, &lv_font_montserrat_20, LV_PART_MAIN);
		osd->createWidgets(osd_cont, screen_w, screen_h);
	}

	// Force an initial render so the video thread receives an OSD buffer signal
	lv_task_handler();

	// Start fact processor thread (runs continuously, independent of rendering)
	std::thread fact_processor(factProcessorLoop);
	fact_processor.detach();  // Let it run independently

	if (gsmenu_enabled) {
		pp_menu_main();
	}

	// Main loop: focus purely on rendering
	while (!osd_thread_signal) {

		if (gsmenu_enabled) {
			handle_keyboard_input();
		}

		{
			std::lock_guard<std::mutex> lock(lvgl_mutex);

			// Update time-based widgets at fixed intervals
			auto now = std::chrono::steady_clock::now();
			auto since_last_tick = now - last_lvgl_tick_at;
			if (since_last_tick >= std::chrono::milliseconds(refresh_frequency_ms)) {
				osd->tick();
				last_lvgl_tick_at = now;
			}

			// Render frequently for smooth animations (~60Hz)
			lv_task_handler();
		}

		// Sleep briefly to avoid busy loop, ~60Hz
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

	spdlog::info("OSD thread done.");
	g_osd = nullptr;
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
			for (const Fact& fact : *facts) {
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

// Reset the facts whose name starts with one of `prefixes` to undefined. The source
// owning them re-publishes, so this is the clean way to drop a subsystem's stale
// facts on an RX mode switch instead of retracting each one by hand. Requests are
// accumulated and applied on the fact-processor thread.
void osd_flush_facts(const char *const *prefixes, int n_prefixes) {
	if (!prefixes || n_prefixes <= 0) return;
	{
		std::lock_guard<std::mutex> lock(mtx);
		for (int i = 0; i < n_prefixes; i++) {
			if (prefixes[i]) pending_flush_prefixes.emplace_back(prefixes[i]);
		}
	}
	cv.notify_one();  // wake the fact processor so the flush is applied promptly
};

uint32_t osd_gl_process(struct modeset_buf* buf){
	return osd_gl.process(buf);
}

#ifdef __cplusplus
}
#endif


//
// Code below is only for unit-tests!
//
#ifdef TEST

TestExpressionTree::TestExpressionTree() {
    tree = new ExpressionTree();
}

TestExpressionTree::TestExpressionTree(const std::string& expression) {
    tree = new ExpressionTree(expression);
}

TestExpressionTree::~TestExpressionTree() {
    delete tree;
}

std::vector<std::string> TestExpressionTree::tokenize(const std::string& input) {
    return tree->tokenize(input);
}

void TestExpressionTree::parse(const std::string &expression) {
    tree->parse(expression);
}

double TestExpressionTree::evaluate(double xValue) {
    return tree->evaluate(xValue);
}



TestTplTextWidget::TestTplTextWidget(int pos_x, int pos_y, std::string tpl, uint n_args) {
    widget = new TplTextWidget(pos_x, pos_y, tpl, n_args);
}
TestTplTextWidget::~TestTplTextWidget() {
    delete widget;
}
void TestTplTextWidget::setBoolFact(uint idx, bool v) {
    Fact fact = Fact(FactMeta("bool"), v);
    widget->setFact(idx, fact);
};
void TestTplTextWidget::setLongFact(uint idx, long v) {
    Fact fact = Fact(FactMeta("long"), v);
    widget->setFact(idx, fact);
};
void TestTplTextWidget::setUlongFact(uint idx, ulong v) {
    Fact fact = Fact(FactMeta("ulong"), v);
    widget->setFact(idx, fact);
};
void TestTplTextWidget::setDoubleFact(uint idx, double v) {
    Fact fact = Fact(FactMeta("double"), v);
    widget->setFact(idx, fact);
};
void TestTplTextWidget::setStringFact(uint idx, std::string v) {
    Fact fact = Fact(FactMeta("string"), v);
    widget->setFact(idx, fact);
};

std::unique_ptr<std::string> TestTplTextWidget::render_tpl() {
    return widget->render_tpl();
}

#endif
