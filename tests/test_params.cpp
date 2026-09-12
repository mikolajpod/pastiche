#include <doctest/doctest.h>

#include "core/params.hpp"

using namespace pastiche;

namespace {

std::vector<ParamSpec> specs()
{
    return {
        ParamSpec::Int("steps", 20, 1, 100, "Number of steps"),
        ParamSpec::Float("alpha", 1.0, 0.0, 1.0, "Blend"),
        ParamSpec::Bool("tile", false, "Tiling"),
        ParamSpec::Enum("mode", "fast", {"fast", "Quality"}, "Mode"),
        ParamSpec::String("prompt", "", "Text prompt"),
        ParamSpec::Float("seed", 0, 0, 0, "Unbounded"),
    };
}

} // namespace

TEST_CASE("defaults come from the spec")
{
    Params p = Params::defaults(specs());
    CHECK(p.get_int("steps") == 20);
    CHECK(p.get_float("alpha") == doctest::Approx(1.0));
    CHECK(p.get_bool("tile") == false);
    CHECK(p.get_str("mode") == "fast");
    CHECK(p.get_str("prompt") == "");
    CHECK(p.has("seed"));
    CHECK_FALSE(p.has("nope"));
    CHECK(p.get_int("nope", 7) == 7);
}

TEST_CASE("int parsing and range")
{
    Params p = Params::defaults(specs());
    CHECK(p.set_kv(specs(), "steps=42") == "");
    CHECK(p.get_int("steps") == 42);
    CHECK(p.set_kv(specs(), "steps= 5 ") == "");
    CHECK(p.get_int("steps") == 5);
    CHECK(p.set_kv(specs(), "steps=0") != "");
    CHECK(p.set_kv(specs(), "steps=101") != "");
    CHECK(p.set_kv(specs(), "steps=1.5") != "");
    CHECK(p.set_kv(specs(), "steps=abc") != "");
    CHECK(p.set_kv(specs(), "steps=") != "");
    CHECK(p.get_int("steps") == 5);  // failed sets do not modify
}

TEST_CASE("float parsing and unbounded range")
{
    Params p = Params::defaults(specs());
    CHECK(p.set_kv(specs(), "alpha=0.25") == "");
    CHECK(p.get_float("alpha") == doctest::Approx(0.25));
    CHECK(p.set_kv(specs(), "alpha=1.01") != "");
    CHECK(p.set_kv(specs(), "alpha=nan") != "");
    CHECK(p.set_kv(specs(), "alpha=1e") != "");
    CHECK(p.set_kv(specs(), "seed=123456789") == "");
    CHECK(p.set_kv(specs(), "seed=-3.5") == "");
    CHECK(p.get_float("seed") == doctest::Approx(-3.5));
}

TEST_CASE("bool spellings")
{
    Params p = Params::defaults(specs());
    for (const char* t : {"on", "true", "1", "yes", "ON", "True"}) {
        CHECK(p.set(specs(), "tile", t) == "");
        CHECK(p.get_bool("tile"));
    }
    for (const char* f : {"off", "false", "0", "no", "OFF"}) {
        CHECK(p.set(specs(), "tile", f) == "");
        CHECK_FALSE(p.get_bool("tile"));
    }
    CHECK(p.set(specs(), "tile", "maybe") != "");
}

TEST_CASE("enum is case-insensitive and normalised")
{
    Params p = Params::defaults(specs());
    CHECK(p.set_kv(specs(), "mode=quality") == "");
    CHECK(p.get_str("mode") == "Quality");
    CHECK(p.set_kv(specs(), "mode=FAST") == "");
    CHECK(p.get_str("mode") == "fast");
    CHECK(p.set_kv(specs(), "mode=slow") != "");
}

TEST_CASE("string keeps everything after the first '='")
{
    Params p = Params::defaults(specs());
    CHECK(p.set_kv(specs(), "prompt=a = b, c") == "");
    CHECK(p.get_str("prompt") == "a = b, c");
}

TEST_CASE("unknown keys and malformed pairs are errors")
{
    Params p = Params::defaults(specs());
    const std::string e = p.set_kv(specs(), "bogus=1");
    CHECK(e.find("unknown parameter 'bogus'") != std::string::npos);
    CHECK(e.find("steps") != std::string::npos);  // lists known keys
    CHECK(p.set_kv(specs(), "steps") != "");
    CHECK(p.set_kv(specs(), "=5") != "");
}

TEST_CASE("help and json rendering")
{
    Params p = Params::defaults(specs());
    p.set_kv(specs(), "prompt=he said \"hi\"\n");
    p.set_kv(specs(), "tile=on");
    const std::string json = params_to_json(specs(), p);
    CHECK(json == "{\"steps\": 20, \"alpha\": 1, \"tile\": true, \"mode\": \"fast\", "
                  "\"prompt\": \"he said \\\"hi\\\"\\n\", \"seed\": 0}");
    const std::string help = params_help(specs(), 2);
    CHECK(help.find("steps") != std::string::npos);
    CHECK(help.find("1..100") != std::string::npos);
    CHECK(help.find("fast|Quality") != std::string::npos);
    CHECK(help.find("default off") != std::string::npos);
    CHECK(json_escape("a\\b\t") == "a\\\\b\\t");
    CHECK(json_number(2.5) == "2.5");
    CHECK(json_number(3.0) == "3");
}
