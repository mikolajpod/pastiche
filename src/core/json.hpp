#pragma once

// Minimal read-only JSON parser, enough for models.json (D8): the model
// catalogue with URLs, checksums and licences. Writing JSON is already handled
// by sidecar.cpp and params.cpp, which render it directly; only reading was
// missing, and a whole dependency for one 100-line file would be out of
// proportion.
//
// Object members keep their file order, so diagnostics can quote the catalogue
// back in the order the user wrote it.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pastiche {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    int64_t as_int(int64_t fallback = 0) const;
    // Returns "" for anything that is not a string, so callers can treat a
    // missing and an empty field the same way when that is what they want.
    const std::string& as_string() const;

    // Array elements. Empty for non-arrays.
    const std::vector<JsonValue>& items() const { return items_; }

    // Object member by key, or nullptr. Returns nullptr for non-objects too.
    const JsonValue* find(const std::string& key) const;
    // Shorthands that fall back when the member is missing or the wrong type.
    std::string string_or(const std::string& key, const std::string& fallback = {}) const;
    int64_t int_or(const std::string& key, int64_t fallback = 0) const;
    bool bool_or(const std::string& key, bool fallback = false) const;

    const std::vector<std::pair<std::string, JsonValue>>& members() const { return members_; }

private:
    friend class JsonParser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

// Parses `text`. On failure returns a Null value and sets `error` to a message
// that names the line and column, because a hand-edited catalogue is exactly
// the kind of file that gets a trailing comma.
JsonValue json_parse(const std::string& text, std::string& error);

// Serialises back to compact JSON. Needed because safetensors files carry a
// JSON header that has to be rewritten in place (see safetensors.hpp); the
// hand-rolled rendering used for sidecars cannot do arbitrary values.
std::string json_dump(const JsonValue& value);

} // namespace pastiche
