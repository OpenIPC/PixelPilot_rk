/**
 * A graphical OSD overlay on top of the video.
 * It receives a ton of various data-points ("Facts") other parts of the system publish using
 * osd_publish_* and osd_add_* functions and uses this data to draw a graphical and/or textual
 * widgets on the screen.
 * The whole OSD is configured using config-file (JSON) which currently is basically a list of
 * widgets, their positions, additional options and "subscriptions" to the "Facts".
 *
 * OSD runs in a separate thread and receives all the facts via queue.
 *
 * We also have `ExternalSurfaceWidget` which is a bit special - it doesn't read any facts but
 * displays a surface that is provided via shm by external program. Right now it is used to display
 * MSP/Displayport OSD.
 */
#include <cmath>
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
#include <condition_variable>
#include <chrono>
#include <deque>
#include <cstdlib> //KILLME
#include <string>
#include <optional>
#include <regex>
#include <utility>
#include <filesystem>
#include <cairo.h>
#include <time.h>
#include <stdint.h>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"
#include <fmt/ranges.h>
#include "lvgl/lvgl.h"

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

	std::string asVerboseString() {
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
        Widget(pos_x, pos_y, num_args), tpl(tpl), num_args(num_args) {
        _tokens = tokenize(tpl);
    };

    virtual void draw(cairo_t *cr) {
        auto [x, y] = xy(cr);
        std::unique_ptr<std::string> msg = render_tpl();
        cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, msg->c_str());
    }

    uint default_precision = 2;

protected:
    enum class TokenType {
        Literal,
        Bool,
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

    std::unique_ptr<std::string> render_tpl() {
        return render_tokens(_tokens, args);
    }

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
                    case TokenType::Bool:
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
                if (match.size() == 2) { // Simple placeholder like %b, %i, %u, %s
                    if (match[1] == 'b') {
                        tokens.emplace_back(TokenType::Bool);
                    } else if (match[1] == 'i' || match[1] == 'd') {
                        tokens.emplace_back(TokenType::Int);
                    } else if (match[1] == 'u') {
                        tokens.emplace_back(TokenType::Uint);
                    } else if (match[1] == 's') {
                        tokens.emplace_back(TokenType::String);
                    }
                } else if (match.back() == 'f') { // Float placeholder
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
    }


    virtual void draw(cairo_t *cr) {
        auto [x, y] = xy(cr);
        const Fact& fact = args[0];
        auto cell_voltage = fact.getDoubleValue();
        auto cell_voltage_mv = cell_voltage * 1000;

        std::unique_ptr<std::string> msg = render_tpl();

        if (cell_voltage_mv <= critical_voltage_mv) {
            // Draw in red
            cairo_set_source_rgba(cr, 255.0, 0, 0, 1);
        } else {
            // Now we know voltage is above critical
            float remaining_percentage =
                (float)(cell_voltage_mv - critical_voltage_mv) /
                (max_voltage_mv - critical_voltage_mv);

            if (remaining_percentage < warn_percentage) {
                // Calculate green based on remaining percentage (0--warn_percentage% range)
                double green_value = 255.0 * (remaining_percentage / warn_percentage);
                // Transition from yellow through orange to red
                cairo_set_source_rgba(cr, 255.0, green_value, 0, 1);
            } else {
                // White when above 20%
                cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
            }
        }
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, msg->c_str());
    }
protected:
    int critical_voltage_mv;
    int max_voltage_mv;
    int num_cells;
};

class DebugWidget: public Widget {
public:
	DebugWidget(int pos_x, int pos_y, uint num_args) :
		Widget(pos_x, pos_y, num_args) {};

	void draw(cairo_t *cr) {
		auto [x, y] = xy(cr);
		auto y_offset = y;
		for (Fact &fact : args) {
			std::string text = fact.asVerboseString();
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

class IconSelectorWidget : public Widget {
public:
    IconSelectorWidget(int pos_x, int pos_y, const std::vector<std::pair<std::pair<int, int>, std::filesystem::path>>& ranges_and_icons, const std::filesystem::path& assets_dir)
        : Widget(pos_x, pos_y), assets_dir(assets_dir) {
        args.push_back(Fact()); // Expect one fact as input

        // Load and cache all icons during initialization
        for (const auto& [range, icon_path] : ranges_and_icons) {
            cairo_surface_t* icon = openIcon(icon_path);
            if (icon) {
                icon_cache[range] = icon;
            }
        }
    }

    virtual ~IconSelectorWidget() {
        // Clean up cached icons
        for (auto& [range, icon] : icon_cache) {
            if (icon) {
                cairo_surface_destroy(icon);
            }
        }
    }

    virtual void setFact(uint idx, Fact fact) override {
        assert(idx == 0);
        args[idx] = fact;
        current_icon = selectIcon(fact);
    }

    virtual void draw(cairo_t *cr) override {
        if (!current_icon) return;

        auto [x, y] = xy(cr);
        cairo_set_source_surface(cr, current_icon, x, y);
        cairo_paint(cr);
    }

private:
    cairo_surface_t* selectIcon(Fact& fact) {
        if (!fact.isDefined()) return nullptr;

        long value = 0;
        
        // Convert all fact types to comparable integer values
        switch (fact.getType()) {
            case Fact::T_BOOL:
                value = fact.getBoolValue() ? 1 : 0;
                break;
            case Fact::T_INT:
                value = fact.getIntValue();
                break;
            case Fact::T_UINT:
                value = static_cast<long>(fact.getUintValue());
                break;
            case Fact::T_DOUBLE:
                value = static_cast<long>(fact.getDoubleValue());
                break;
            case Fact::T_STRING:
                try {
                    value = std::stol(fact.getStrValue());
                } catch (...) {
                    // If string can't be converted to number, use 0
                    value = 0;
                }
                break;
            case Fact::T_UNDEF:
            default:
                return nullptr;
        }

        // Iterate through the configured ranges and select the appropriate icon
        for (const auto& [range, icon] : icon_cache) {
            if (value >= range.first && value <= range.second) {
                return icon;
            }
        }

        return nullptr; // No icon selected
    }

    cairo_surface_t* openIcon(const std::filesystem::path& icon_path) {
        std::filesystem::path full_path = assets_dir / icon_path;
        cairo_surface_t* icon = cairo_image_surface_create_from_png(full_path.c_str());
        if (cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS) {
            spdlog::error("Failed to open icon: {}", full_path.string());
            return nullptr;
        }
        return icon;
    }

    std::map<std::pair<int, int>, cairo_surface_t*> icon_cache; // Cache of loaded icons
    std::filesystem::path assets_dir;
    cairo_surface_t* current_icon = nullptr; // Currently selected icon
};

class Osd {
public:
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
			}
			else if (type == "ExternalSurfaceWidget") {
				addWidget(new ExternalSurfaceWidget(x, y, name), matchers);
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

	void draw(cairo_t *cr) {
		for(auto &widget : widgets)
			widget->draw(cr);
	};

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


void my_flush_cb(lv_display_t * display, const lv_area_t * area, uint8_t * px_map)
{

    struct modeset_buf *buf1 = &p->out->osd_bufs[0];
    struct modeset_buf *buf2 = &p->out->osd_bufs[1];
	int ret = pthread_mutex_lock(&osd_mutex);
	assert(!ret);	
    if (px_map == buf1->map) {
		p->out->osd_buf_switch = 0;
    } else if (px_map == buf2->map) {
		p->out->osd_buf_switch = 1;
    } else {
        spdlog::error("Unknown buffer being flushed");
    }
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
void setup_lvgl(osd_thread_params *p) {

	/* Initialize LVGL. */
    lv_init();

	// create the display
    struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	display = lv_display_create(buf->width, buf->height);

	// Get the first two buffers from the OSD buffers
	struct modeset_buf *buf1 = &p->out->osd_bufs[0];
	struct modeset_buf *buf2 = &p->out->osd_bufs[1];

	// Set the buffers in LVGL
	lv_display_set_buffers(display, buf1->map, buf2->map, buf1->size, LV_DISPLAY_RENDER_MODE_DIRECT);

	lv_display_set_flush_cb(display, my_flush_cb);

	lv_tick_set_cb(my_get_milliseconds);

	lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, LV_PART_MAIN);
    
}

void *__OSD_THREAD__(void *param) {
	p = (osd_thread_params *)param;
	Osd *osd = new Osd;
	pthread_setname_np(pthread_self(), "__OSD");

	osd->loadConfig(p->config);
	auto last_display_at = std::chrono::steady_clock::now();

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane,
								  buf->fb, buf->width, buf->height, osd_zpos);

	if (gsmenu_enabled) {
		setup_lvgl(p);
		pp_menu_main();
	}

	while (!osd_thread_signal) {

		if (gsmenu_enabled) {
			handle_keyboard_input();
			lv_task_handler();
		}

		std::unique_lock<std::mutex> lock(mtx);
		std::vector<Fact> fact_buf;
		auto since_last_display = std::chrono::steady_clock::now() - last_display_at;
		auto wait = std::chrono::milliseconds(refresh_frequency_ms) - since_last_display;
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
				SPDLOG_DEBUG("got fact {}", fact_queue.front().asVerboseString());
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

			if (! menu_active ) {
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
			} else {
				usleep(5000);
			}
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
