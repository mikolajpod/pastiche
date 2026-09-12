#pragma once

#include <map>
#include <string>
#include <vector>

namespace pastiche {

enum class ParamType { Int, Float, Bool, Enum, String };

// Declarative description of one algorithm parameter. The CLI parses and
// validates `-p key=value` against it, `--help` prints it, the GUI draws a
// widget from it. Algorithms never parse strings themselves.
struct ParamSpec {
    std::string key;
    ParamType type = ParamType::Float;
    std::string description;
    double def = 0.0;                  // Int / Float / Bool default (Bool: 0 or 1)
    double min = 0.0;                  // Int / Float range; min == max means unbounded
    double max = 0.0;
    std::string def_str;               // Enum / String default
    std::vector<std::string> choices;  // Enum values

    static ParamSpec Int(std::string key, int def, int min, int max, std::string desc);
    static ParamSpec Float(std::string key, double def, double min, double max, std::string desc);
    static ParamSpec Bool(std::string key, bool def, std::string desc);
    static ParamSpec Enum(std::string key, std::string def, std::vector<std::string> choices, std::string desc);
    static ParamSpec String(std::string key, std::string def, std::string desc);

    const char* type_name() const;      // "int", "float", "bool", "enum", "string"
    std::string default_text() const;   // default as the user would type it
    std::string range_text() const;     // "0..100", "a|b|c" or ""
};

struct ParamValue {
    double num = 0.0;     // Int / Float / Bool
    std::string str;      // Enum / String
};

class Params {
public:
    static Params defaults(const std::vector<ParamSpec>& specs);

    // Set one parameter from text, validated against `specs`.
    // Returns an empty string on success or a human-readable error.
    std::string set(const std::vector<ParamSpec>& specs, const std::string& key, const std::string& value);
    // Same, from "key=value".
    std::string set_kv(const std::vector<ParamSpec>& specs, const std::string& kv);

    bool has(const std::string& key) const;
    int get_int(const std::string& key, int fallback = 0) const;
    double get_float(const std::string& key, double fallback = 0.0) const;
    bool get_bool(const std::string& key, bool fallback = false) const;
    const std::string& get_str(const std::string& key) const;

    // Value as text (for logs / JSON); needs the spec to know the type.
    std::string text(const ParamSpec& spec) const;

    std::map<std::string, ParamValue>& values() { return values_; }
    const std::map<std::string, ParamValue>& values() const { return values_; }

private:
    std::map<std::string, ParamValue> values_;
};

const ParamSpec* find_spec(const std::vector<ParamSpec>& specs, const std::string& key);

// Multi-line help text, one line per parameter, indented by `indent` spaces.
std::string params_help(const std::vector<ParamSpec>& specs, int indent = 2);

// Parses text as a value of the given type; returns error or "" and fills out.
std::string parse_param_value(const ParamSpec& spec, const std::string& text, ParamValue& out);

// Minimal JSON helpers (the sidecar file is small enough to write by hand).
std::string json_escape(const std::string& s);
std::string json_quote(const std::string& s);
std::string json_number(double v);
// {"key": value, ...} with typed values (numbers, true/false, strings).
std::string params_to_json(const std::vector<ParamSpec>& specs, const Params& p);

} // namespace pastiche
