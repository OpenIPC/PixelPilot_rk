#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <cairo.h>

#include "../src/osd.hpp"

TEST_CASE("Expression tokenizer tests", "[ExpressionTree]")
{
    TestExpressionTree tree;

    REQUIRE(tree.tokenize("1") == std::vector<std::string>{"1"});
    REQUIRE(tree.tokenize("3 * x") == std::vector<std::string>{"3", "*", "x"});
    REQUIRE(tree.tokenize("3*x") == std::vector<std::string>{"3", "*", "x"});
    REQUIRE(tree.tokenize("(3 + 2) * x") ==
            std::vector<std::string>{"(", "3", "+", "2", ")", "*", "x"});
    REQUIRE(tree.tokenize("1 + 2 / 3 * 4 - ( 5 + x )") ==
            std::vector<std::string>{"1", "+", "2", "/", "3", "*", "4", "-",
                                     "(", "5", "+", "x", ")"});
    REQUIRE(tree.tokenize("1+2/3*4-(5+x)") ==
            std::vector<std::string>{"1", "+", "2", "/", "3", "*", "4", "-",
                                     "(", "5", "+", "x", ")"});
}

TEST_CASE("Expression evaluation tests", "[ExpressionTree]")
{
    TestExpressionTree tree;
    auto x = GENERATE(0.0, 1.0, 2.0, 3.0, 1000.0);

    SECTION("constant") {
        tree.parse("1");
        REQUIRE(tree.evaluate(0) == 1.0);
    }
    SECTION("Just x") {
        tree.parse("x");
        REQUIRE(tree.evaluate(x) == x);
    }
    SECTION("x + 1") {
        tree.parse("x + 1");
        REQUIRE(tree.evaluate(x) == x + 1);
    }
    SECTION("(x + 2) * 3") {
        tree.parse("(x + 2) * 3");
        REQUIRE(tree.evaluate(4) == 18.0);
        REQUIRE(tree.evaluate(x) == ((x + 2) * 3));
    }
    SECTION("x + 2 * 3") {
        tree.parse("x + 2 * 3");
        REQUIRE(tree.evaluate(4) == 10.0);
        REQUIRE(tree.evaluate(x) == (x + 2 * 3));
    }
    SECTION("x + 2 * x") {
        tree.parse("x + 2 * x");
        REQUIRE(tree.evaluate(4) == 12.0);
        REQUIRE(tree.evaluate(x) == (x + 2 * x));
    }
}

TEST_CASE("TplTextWidget supports all fact data-types and float precision", "[TplTextWidget]") {
    // Template covers: bool, int, uint, float (default), float (0/2/4 precision), string
    TestTplTextWidget widget(
        10, 50,
        "Bool: %b, Int: %i, Uint: %u, Float: %f, Float0: %.0f, Float2: %.2f, Float4: %.4f, String: %s, Undef: %s",
        9
    );
    widget.setBoolFact(0, true);                // %b
    widget.setLongFact(1, (long)-123);          // %i
    widget.setUlongFact(2, (ulong)456);          // %u
    widget.setDoubleFact(3, 3.1415926535);        // %f
    widget.setDoubleFact(4, 3.2515926535);        // %.0f
    widget.setDoubleFact(5, 3.3415926535);        // %.2f
    widget.setDoubleFact(6, 3.4415926535);        // %.4f
    widget.setStringFact(7, std::string("hello")); // %s
    // not setting 8th so it is UNDEF

    std::string result = *widget.render_tpl();

    REQUIRE(
        result ==
        "Bool: t, Int: -123, Uint: 456, Float: 3.14, Float0: 3, Float2: 3.34, Float4: 3.4416, String: hello, Undef: ?"
    );
}

int compare_surfaces_with_tolerance(cairo_surface_t* a, cairo_surface_t* b, int tolerance = 5, int max_report = 10) {
    if (!a || !b) {
        std::cerr << "One or both surfaces are null." << std::endl;
        return -1;
    }
    int width = cairo_image_surface_get_width(a);
    int height = cairo_image_surface_get_height(a);
    if (width != cairo_image_surface_get_width(b) || height != cairo_image_surface_get_height(b)) {
        std::cerr << "Surface size mismatch: " << width << "x" << height
                  << " vs " << cairo_image_surface_get_width(b) << "x"
                  << cairo_image_surface_get_height(b) << std::endl;
        return -1;
    }
    int stride = cairo_image_surface_get_stride(a);
    const unsigned char* data_a = cairo_image_surface_get_data(a);
    const unsigned char* data_b = cairo_image_surface_get_data(b);

    int diff_count = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int offset = y * stride + x * 4;
            uint8_t* pa = (uint8_t*)(data_a + offset);
            uint8_t* pb = (uint8_t*)(data_b + offset);
            int diff = 0;
            for (int c = 0; c < 4; ++c) { // ARGB
                diff += std::abs(pa[c] - pb[c]);
            }
            if (diff > tolerance * 4) {
                if (diff_count < max_report) {
                    std::cerr << "Pixel difference at (" << x << "," << y << "): "
                              << "A=(" << (int)pa[0] << "," << (int)pa[1] << "," << (int)pa[2] << "," << (int)pa[3] << ") "
                              << "B=(" << (int)pb[0] << "," << (int)pb[1] << "," << (int)pb[2] << "," << (int)pb[3] << ") "
                              << "Total diff: " << diff << std::endl;
                }
                ++diff_count;
            }
        }
    }
    if (diff_count > max_report) {
        std::cerr << "(... " << (diff_count - max_report) << " more differences ...)" << std::endl;
    }
    if (diff_count == 0) {
        std::cout << "Surfaces are visually identical (within tolerance)." << std::endl;
    } else {
        std::cerr << "Total differing pixels (with tolerance): " << diff_count << std::endl;
    }
    return diff_count;
}

TEST_CASE("Basic TplTextWidget rendering", "[TplTextWidget]")
{
    // 1. Render widget to template
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 100);
	cairo_t* cr = cairo_create(surface);
	cairo_select_font_face(cr, "Arial", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 20);
    // XXX: do we need to clear?

    TestTplTextWidget widget(10, 50, "Hello %u", 1);
    widget.setUlongFact(0, 42);
    widget.draw(cr);
	cairo_fill(cr);
	cairo_destroy(cr);

    // Load reference
    cairo_surface_t *ref_surface = cairo_image_surface_create_from_png(
                                      "tests/files/basic_tpl_text_widget.png");
    REQUIRE(cairo_surface_status(ref_surface) == CAIRO_STATUS_SUCCESS);

    // Compare
    REQUIRE(compare_surfaces_with_tolerance(surface, ref_surface, 5, 10) == 0);

    // Cleanup
    cairo_surface_destroy(surface);
    cairo_surface_destroy(ref_surface);
}
