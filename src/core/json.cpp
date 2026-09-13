#include "json.hpp"

#include <cmath>
#include <cstdlib>

namespace pastiche {

namespace {

const std::string kEmptyString;

} // namespace

int64_t JsonValue::as_int(int64_t fallback) const
{
    if (type_ != Type::Number) return fallback;
    return static_cast<int64_t>(number_);
}

const std::string& JsonValue::as_string() const
{
    return type_ == Type::String ? string_ : kEmptyString;
}

const JsonValue* JsonValue::find(const std::string& key) const
{
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : members_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

std::string JsonValue::string_or(const std::string& key, const std::string& fallback) const
{
    const JsonValue* v = find(key);
    return v && v->is_string() ? v->as_string() : fallback;
}

int64_t JsonValue::int_or(const std::string& key, int64_t fallback) const
{
    const JsonValue* v = find(key);
    return v && v->is_number() ? v->as_int(fallback) : fallback;
}

bool JsonValue::bool_or(const std::string& key, bool fallback) const
{
    const JsonValue* v = find(key);
    return v && v->is_bool() ? v->as_bool(fallback) : fallback;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class JsonParser {
public:
    JsonParser(const std::string& text, std::string& error) : s_(text), error_(error) {}

    JsonValue parse()
    {
        skip_whitespace();
        JsonValue v;
        if (!parse_value(v, 0)) return JsonValue{};
        skip_whitespace();
        if (pos_ != s_.size()) {
            fail("trailing content after the top-level value");
            return JsonValue{};
        }
        return v;
    }

private:
    // Guards against a hand-written catalogue nesting deep enough to blow the
    // stack; the real files are three levels deep.
    static const int kMaxDepth = 64;

    bool fail(const std::string& what)
    {
        if (!error_.empty()) return false;  // keep the first, most specific error
        std::size_t line = 1, col = 1;
        for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        error_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + what;
        return false;
    }

    void skip_whitespace()
    {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool at(char c) const { return pos_ < s_.size() && s_[pos_] == c; }

    bool literal(const char* word)
    {
        const std::size_t len = std::char_traits<char>::length(word);
        if (s_.compare(pos_, len, word) != 0) return false;
        pos_ += len;
        return true;
    }

    bool parse_value(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) return fail("nested too deeply");
        skip_whitespace();
        if (pos_ >= s_.size()) return fail("unexpected end of input");

        switch (s_[pos_]) {
        case '{': return parse_object(out, depth);
        case '[': return parse_array(out, depth);
        case '"':
            out.type_ = JsonValue::Type::String;
            return parse_string(out.string_);
        case 't':
            if (!literal("true")) return fail("invalid literal");
            out.type_ = JsonValue::Type::Bool;
            out.bool_ = true;
            return true;
        case 'f':
            if (!literal("false")) return fail("invalid literal");
            out.type_ = JsonValue::Type::Bool;
            out.bool_ = false;
            return true;
        case 'n':
            if (!literal("null")) return fail("invalid literal");
            out.type_ = JsonValue::Type::Null;
            return true;
        default:
            return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out, int depth)
    {
        out.type_ = JsonValue::Type::Object;
        ++pos_;  // '{'
        skip_whitespace();
        if (at('}')) { ++pos_; return true; }

        while (true) {
            skip_whitespace();
            if (!at('"')) return fail("expected a quoted member name");
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (!at(':')) return fail("expected ':' after the member name");
            ++pos_;

            JsonValue value;
            if (!parse_value(value, depth + 1)) return false;
            out.members_.emplace_back(std::move(key), std::move(value));

            skip_whitespace();
            if (at(',')) { ++pos_; continue; }
            if (at('}')) { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(JsonValue& out, int depth)
    {
        out.type_ = JsonValue::Type::Array;
        ++pos_;  // '['
        skip_whitespace();
        if (at(']')) { ++pos_; return true; }

        while (true) {
            JsonValue value;
            if (!parse_value(value, depth + 1)) return false;
            out.items_.push_back(std::move(value));

            skip_whitespace();
            if (at(',')) { ++pos_; continue; }
            if (at(']')) { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    // Appends a code point to `out` as UTF-8.
    static void append_utf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool parse_hex4(uint32_t& out)
    {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out)
    {
        ++pos_;  // opening quote
        out.clear();
        while (true) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) return fail("raw control character in string");
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(cp)) return false;
                // A surrogate pair encodes one code point above U+FFFF.
                if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 1 < s_.size() &&
                    s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!parse_hex4(low)) return false;
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else {
                        // Not a valid pair; emit both as-is rather than failing,
                        // this only affects text we display.
                        append_utf8(out, cp);
                        cp = low;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return fail("unknown escape sequence");
            }
        }
    }

    bool parse_number(JsonValue& out)
    {
        const std::size_t start = pos_;
        if (at('-')) ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (at('.')) {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ == start) return fail("expected a value");

        const std::string text = s_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return fail("malformed number");
        out.type_ = JsonValue::Type::Number;
        out.number_ = value;
        return true;
    }

    const std::string& s_;
    std::string& error_;
    std::size_t pos_ = 0;
};

JsonValue json_parse(const std::string& text, std::string& error)
{
    error.clear();
    JsonParser parser(text, error);
    JsonValue v = parser.parse();
    if (!error.empty()) return JsonValue{};
    return v;
}

} // namespace pastiche
