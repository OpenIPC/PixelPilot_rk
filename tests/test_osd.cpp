#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

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
