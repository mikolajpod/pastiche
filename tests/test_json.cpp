#include <doctest/doctest.h>

#include "core/json.hpp"

#include <string>

using namespace pastiche;

TEST_CASE("json parses the shape models.json actually uses")
{
    const std::string text = R"({
        "version": 1,
        "models": [
            {
                "name": "sd15",
                "url": "https://example.invalid/sd15.safetensors",
                "sha256": "abc123",
                "bytes": 2132625432,
                "licence": "CreativeML OpenRAIL-M",
                "needs_acceptance": true
            },
            {
                "name": "ip-adapter",
                "url": "https://example.invalid/ip.bin",
                "bytes": 44642621,
                "needs_acceptance": false
            }
        ]
    })";

    std::string error;
    const JsonValue root = json_parse(text, error);
    REQUIRE(error.empty());
    REQUIRE(root.is_object());
    CHECK(root.int_or("version") == 1);

    const JsonValue* models = root.find("models");
    REQUIRE(models != nullptr);
    REQUIRE(models->is_array());
    REQUIRE(models->items().size() == 2);

    const JsonValue& first = models->items()[0];
    CHECK(first.string_or("name") == "sd15");
    CHECK(first.string_or("sha256") == "abc123");
    CHECK(first.int_or("bytes") == 2132625432LL);  // must not lose precision above 2^31
    CHECK(first.bool_or("needs_acceptance") == true);

    const JsonValue& second = models->items()[1];
    // Present and false: the explicit false must win over a true fallback.
    CHECK(second.bool_or("needs_acceptance", true) == false);
    // Absent: the fallback is what comes back.
    CHECK(second.string_or("sha256", "missing") == "missing");
}

TEST_CASE("json object members keep their file order")
{
    std::string error;
    const JsonValue v = json_parse(R"({"z": 1, "a": 2, "m": 3})", error);
    REQUIRE(error.empty());
    REQUIRE(v.members().size() == 3);
    CHECK(v.members()[0].first == "z");
    CHECK(v.members()[1].first == "a");
    CHECK(v.members()[2].first == "m");
}

TEST_CASE("json parses scalars and nesting")
{
    std::string error;

    CHECK(json_parse("true", error).as_bool() == true);
    CHECK(json_parse("null", error).is_null());
    CHECK(json_parse("-12.5e2", error).as_number() == doctest::Approx(-1250.0));
    CHECK(json_parse("[]", error).items().empty());
    CHECK(json_parse("{}", error).is_object());

    const JsonValue nested = json_parse(R"({"a": {"b": [1, 2, {"c": "deep"}]}})", error);
    REQUIRE(error.empty());
    const JsonValue* a = nested.find("a");
    REQUIRE(a != nullptr);
    const JsonValue* b = a->find("b");
    REQUIRE(b != nullptr);
    REQUIRE(b->items().size() == 3);
    CHECK(b->items()[2].string_or("c") == "deep");
}

TEST_CASE("json decodes string escapes including surrogate pairs")
{
    std::string error;
    const JsonValue v = json_parse(R"({"s": "tab:\t quote:\" slash:\/ backslash:\\ e:\u00e9 pl:\u0142"})", error);
    REQUIRE(error.empty());
    CHECK(v.string_or("s") == "tab:\t quote:\" slash:/ backslash:\\ e:\xc3\xa9 pl:\xc5\x82");

    // U+1F600, encoded as a surrogate pair, must come out as four UTF-8 bytes.
    const JsonValue emoji = json_parse(R"({"s": "\ud83d\ude00"})", error);
    REQUIRE(error.empty());
    CHECK(emoji.string_or("s") == "\xf0\x9f\x98\x80");
}

TEST_CASE("json rejects malformed input with a located message")
{
    // A hand-edited catalogue gets exactly these mistakes, so each must be an
    // error rather than something quietly accepted.
    const char* bad[] = {
        R"({"a": 1,})",          // trailing comma in an object
        R"([1, 2,])",            // trailing comma in an array
        R"({"a" 1})",            // missing colon
        R"({a: 1})",             // unquoted key
        R"({"a": "unterminated)",
        R"({"a": tru})",         // truncated literal
        R"({"a": 1} extra)",     // trailing content
        R"({"a": 0x10})",        // hex is not JSON
        "",                      // empty input
    };
    for (const char* text : bad) {
        std::string error;
        const JsonValue v = json_parse(text, error);
        CAPTURE(text);
        CHECK_FALSE(error.empty());
        CHECK(v.is_null());
    }
}

TEST_CASE("json error messages carry a line number")
{
    std::string error;
    json_parse("{\n  \"a\": 1,\n  \"b\": oops\n}", error);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("line 3") != std::string::npos);
}

TEST_CASE("json accessors are safe on the wrong type")
{
    std::string error;
    const JsonValue v = json_parse(R"({"n": 5, "s": "text", "arr": [1]})", error);
    REQUIRE(error.empty());

    // Asking for the wrong type must fall back, never crash.
    CHECK(v.string_or("n", "fallback") == "fallback");
    CHECK(v.int_or("s", -1) == -1);
    CHECK(v.find("missing") == nullptr);
    CHECK(v.find("n")->items().empty());
    CHECK(v.find("arr")->as_string().empty());
    CHECK(v.find("arr")->find("anything") == nullptr);
}
