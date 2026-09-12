#include "params.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace pastiche {

namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string fmt_num(double v)
{
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

ParamSpec ParamSpec::Int(std::string key, int def, int min, int max, std::string desc)
{
    ParamSpec s; s.key = std::move(key); s.type = ParamType::Int; s.def = def; s.min = min; s.max = max;
    s.description = std::move(desc); return s;
}
ParamSpec ParamSpec::Float(std::string key, double def, double min, double max, std::string desc)
{
    ParamSpec s; s.key = std::move(key); s.type = ParamType::Float; s.def = def; s.min = min; s.max = max;
    s.description = std::move(desc); return s;
}
ParamSpec ParamSpec::Bool(std::string key, bool def, std::string desc)
{
    ParamSpec s; s.key = std::move(key); s.type = ParamType::Bool; s.def = def ? 1.0 : 0.0;
    s.description = std::move(desc); return s;
}
ParamSpec ParamSpec::Enum(std::string key, std::string def, std::vector<std::string> choices, std::string desc)
{
    ParamSpec s; s.key = std::move(key); s.type = ParamType::Enum; s.def_str = std::move(def);
    s.choices = std::move(choices); s.description = std::move(desc); return s;
}
ParamSpec ParamSpec::String(std::string key, std::string def, std::string desc)
{
    ParamSpec s; s.key = std::move(key); s.type = ParamType::String; s.def_str = std::move(def);
    s.description = std::move(desc); return s;
}

const char* ParamSpec::type_name() const
{
    switch (type) {
        case ParamType::Int: return "int";
        case ParamType::Float: return "float";
        case ParamType::Bool: return "bool";
        case ParamType::Enum: return "enum";
        case ParamType::String: return "string";
    }
    return "?";
}

std::string ParamSpec::default_text() const
{
    switch (type) {
        case ParamType::Int: return fmt_num(def);
        case ParamType::Float: return fmt_num(def);
        case ParamType::Bool: return def != 0.0 ? "on" : "off";
        case ParamType::Enum: return def_str;
        case ParamType::String: return def_str;
    }
    return {};
}

std::string ParamSpec::range_text() const
{
    switch (type) {
        case ParamType::Int:
        case ParamType::Float:
            if (min == max) return {};
            return fmt_num(min) + ".." + fmt_num(max);
        case ParamType::Bool: return "on|off";
        case ParamType::Enum: {
            std::string r;
            for (size_t i = 0; i < choices.size(); ++i) { if (i) r += '|'; r += choices[i]; }
            return r;
        }
        case ParamType::String: return {};
    }
    return {};
}

const ParamSpec* find_spec(const std::vector<ParamSpec>& specs, const std::string& key)
{
    for (const ParamSpec& s : specs) if (s.key == key) return &s;
    return nullptr;
}

std::string parse_param_value(const ParamSpec& spec, const std::string& raw, ParamValue& out)
{
    const std::string text = trim(raw);
    switch (spec.type) {
        case ParamType::Int: {
            if (text.empty()) return "empty value for '" + spec.key + "'";
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno != 0 || !end || *end != '\0') return "'" + spec.key + "' expects an integer, got '" + text + "'";
            if (spec.min != spec.max && (v < spec.min || v > spec.max))
                return "'" + spec.key + "' must be in " + spec.range_text() + ", got " + text;
            out.num = static_cast<double>(v);
            return {};
        }
        case ParamType::Float: {
            if (text.empty()) return "empty value for '" + spec.key + "'";
            errno = 0;
            char* end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (errno != 0 || !end || *end != '\0' || !std::isfinite(v))
                return "'" + spec.key + "' expects a number, got '" + text + "'";
            if (spec.min != spec.max && (v < spec.min || v > spec.max))
                return "'" + spec.key + "' must be in " + spec.range_text() + ", got " + text;
            out.num = v;
            return {};
        }
        case ParamType::Bool: {
            const std::string t = lower(text);
            if (t == "1" || t == "on" || t == "true" || t == "yes") { out.num = 1.0; return {}; }
            if (t == "0" || t == "off" || t == "false" || t == "no") { out.num = 0.0; return {}; }
            return "'" + spec.key + "' expects on|off, got '" + text + "'";
        }
        case ParamType::Enum: {
            const std::string t = lower(text);
            for (const std::string& c : spec.choices)
                if (lower(c) == t) { out.str = c; return {}; }
            return "'" + spec.key + "' must be one of " + spec.range_text() + ", got '" + text + "'";
        }
        case ParamType::String:
            out.str = raw;
            return {};
    }
    return "unknown parameter type";
}

Params Params::defaults(const std::vector<ParamSpec>& specs)
{
    Params p;
    for (const ParamSpec& s : specs) {
        ParamValue v;
        v.num = s.def;
        v.str = s.def_str;
        p.values_[s.key] = v;
    }
    return p;
}

std::string Params::set(const std::vector<ParamSpec>& specs, const std::string& key, const std::string& value)
{
    const ParamSpec* spec = find_spec(specs, key);
    if (!spec) {
        std::string known;
        for (const ParamSpec& s : specs) { if (!known.empty()) known += ", "; known += s.key; }
        return "unknown parameter '" + key + "'" + (known.empty() ? "" : " (known: " + known + ")");
    }
    ParamValue v;
    const std::string err = parse_param_value(*spec, value, v);
    if (!err.empty()) return err;
    values_[key] = v;
    return {};
}

std::string Params::set_kv(const std::vector<ParamSpec>& specs, const std::string& kv)
{
    const size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) return "expected key=value, got '" + kv + "'";
    return set(specs, trim(kv.substr(0, eq)), kv.substr(eq + 1));
}

bool Params::has(const std::string& key) const { return values_.count(key) != 0; }

int Params::get_int(const std::string& key, int fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : static_cast<int>(std::llround(it->second.num));
}

double Params::get_float(const std::string& key, double fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.num;
}

bool Params::get_bool(const std::string& key, bool fallback) const
{
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.num != 0.0;
}

const std::string& Params::get_str(const std::string& key) const
{
    static const std::string empty;
    auto it = values_.find(key);
    return it == values_.end() ? empty : it->second.str;
}

std::string Params::text(const ParamSpec& spec) const
{
    auto it = values_.find(spec.key);
    if (it == values_.end()) return spec.default_text();
    switch (spec.type) {
        case ParamType::Int: return fmt_num(std::llround(it->second.num));
        case ParamType::Float: return fmt_num(it->second.num);
        case ParamType::Bool: return it->second.num != 0.0 ? "on" : "off";
        case ParamType::Enum:
        case ParamType::String: return it->second.str;
    }
    return {};
}

std::string params_help(const std::vector<ParamSpec>& specs, int indent)
{
    std::ostringstream os;
    const std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
    size_t w = 0;
    for (const ParamSpec& s : specs) w = std::max(w, s.key.size());
    for (const ParamSpec& s : specs) {
        os << pad << s.key << std::string(w - s.key.size() + 2, ' ');
        os << s.type_name();
        const std::string r = s.range_text();
        if (!r.empty()) os << " " << r;
        os << ", default " << (s.default_text().empty() ? std::string("\"\"") : s.default_text());
        if (!s.description.empty()) os << "\n" << pad << std::string(w + 2, ' ') << s.description;
        os << "\n";
    }
    return os.str();
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string json_quote(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string json_number(double v)
{
    if (!std::isfinite(v)) return "null";
    return fmt_num(v);
}

std::string params_to_json(const std::vector<ParamSpec>& specs, const Params& p)
{
    std::string out = "{";
    bool first = true;
    for (const ParamSpec& s : specs) {
        if (!first) out += ", ";
        first = false;
        out += json_quote(s.key) + ": ";
        switch (s.type) {
            case ParamType::Int: out += json_number(p.get_int(s.key, static_cast<int>(s.def))); break;
            case ParamType::Float: out += json_number(p.get_float(s.key, s.def)); break;
            case ParamType::Bool: out += p.get_bool(s.key, s.def != 0.0) ? "true" : "false"; break;
            case ParamType::Enum:
            case ParamType::String: out += json_quote(p.has(s.key) ? p.get_str(s.key) : s.def_str); break;
        }
    }
    out += "}";
    return out;
}

} // namespace pastiche
