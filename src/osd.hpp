#ifndef OSDPP_H
#define OSDPP_H

extern "C" {
#include "drm.h"
}
#include <nlohmann/json.hpp>

typedef struct {
	struct modeset_output *out;
	int fd;
	nlohmann::json config;
} osd_thread_params;

extern int osd_thread_signal;

struct SharedMemoryRegion {
    uint16_t width;       // Image width
    uint16_t height;      // Image height
    unsigned char data[]; // Flexible array member for image data
};

void *__OSD_THREAD__(void *param);

#ifdef TEST
class ExpressionTree;

class TestExpressionTree {
public:
    TestExpressionTree();
    TestExpressionTree(const std::string& expression);
    ~TestExpressionTree();

    std::vector<std::string> tokenize(const std::string& input);
    void parse(const std::string &expression);
    double evaluate(double xValue);

private:
    ExpressionTree *tree;
};

class TplTextWidget;

class TestTplTextWidget {
public:
    TestTplTextWidget(int pos_x, int pos_y, std::string tpl, uint n_args);
    ~TestTplTextWidget();

    void draw(void *cr);

    std::unique_ptr<std::string> render_tpl();

    void setBoolFact(uint idx, bool v);
    void setLongFact(uint idx, long v);
    void setUlongFact(uint idx, ulong v);
    void setDoubleFact(uint idx, double v);
    void setStringFact(uint idx, std::string v);

private:
    TplTextWidget *widget;
};

#endif

#endif
