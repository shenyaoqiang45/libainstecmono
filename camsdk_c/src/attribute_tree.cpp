#include "attribute_tree.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace acam {
namespace {

// Minimal JSON DOM sufficient for GenICam attribute trees and the flat ABI.
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* get(const char* key) const {
        if (kind != Object)
            return nullptr;
        for (const auto& kv : o) {
            if (kv.first == key)
                return &kv.second;
        }
        return nullptr;
    }
    bool has(const char* key) const { return get(key) != nullptr; }
    std::string as_string() const {
        if (kind == String)
            return s;
        return {};
    }
    bool as_bool(bool fallback = false) const {
        if (kind == Bool)
            return b;
        if (kind == Number)
            return n != 0.0;
        return fallback;
    }
    bool as_number(double& out) const {
        if (kind != Number)
            return false;
        out = n;
        return true;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : t_(text) {}

    bool parse(Json& out) {
        skip();
        if (!parse_value(out))
            return false;
        skip();
        return i_ == t_.size();
    }

private:
    const std::string& t_;
    size_t i_ = 0;

    void skip() {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_])))
            ++i_;
    }
    bool match(char c) {
        skip();
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool parse_value(Json& out) {
        skip();
        if (i_ >= t_.size())
            return false;
        const char c = t_[i_];
        if (c == '{')
            return parse_object(out);
        if (c == '[')
            return parse_array(out);
        if (c == '"')
            return parse_string(out);
        if (c == 't' || c == 'f')
            return parse_bool(out);
        if (c == 'n')
            return parse_null(out);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number(out);
        return false;
    }
    bool parse_null(Json& out) {
        if (t_.compare(i_, 4, "null") != 0)
            return false;
        i_ += 4;
        out = Json{};
        return true;
    }
    bool parse_bool(Json& out) {
        if (t_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            out.kind = Json::Bool;
            out.b = true;
            return true;
        }
        if (t_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            out.kind = Json::Bool;
            out.b = false;
            return true;
        }
        return false;
    }
    bool parse_number(Json& out) {
        const size_t start = i_;
        if (t_[i_] == '-')
            ++i_;
        if (i_ >= t_.size() || !std::isdigit(static_cast<unsigned char>(t_[i_])))
            return false;
        while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_])))
            ++i_;
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_])))
                ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-'))
                ++i_;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_])))
                ++i_;
        }
        try {
            out.kind = Json::Number;
            out.n = std::stod(t_.substr(start, i_ - start));
            return true;
        } catch (...) {
            return false;
        }
    }
    bool parse_string(Json& out) {
        if (!match('"'))
            return false;
        std::string s;
        while (i_ < t_.size()) {
            const char c = t_[i_++];
            if (c == '"') {
                out.kind = Json::String;
                out.s = std::move(s);
                return true;
            }
            if (c == '\\') {
                if (i_ >= t_.size())
                    return false;
                const char e = t_[i_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    s.push_back(e);
                    break;
                case 'b':
                    s.push_back('\b');
                    break;
                case 'f':
                    s.push_back('\f');
                    break;
                case 'n':
                    s.push_back('\n');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case 'u':
                    // Skip 4 hex digits; attribute trees are ASCII keys.
                    if (i_ + 4 > t_.size())
                        return false;
                    i_ += 4;
                    s.push_back('?');
                    break;
                default:
                    return false;
                }
            } else {
                s.push_back(c);
            }
        }
        return false;
    }
    bool parse_array(Json& out) {
        if (!match('['))
            return false;
        out.kind = Json::Array;
        skip();
        if (match(']'))
            return true;
        for (;;) {
            Json v;
            if (!parse_value(v))
                return false;
            out.a.push_back(std::move(v));
            skip();
            if (match(']'))
                return true;
            if (!match(','))
                return false;
        }
    }
    bool parse_object(Json& out) {
        if (!match('{'))
            return false;
        out.kind = Json::Object;
        skip();
        if (match('}'))
            return true;
        for (;;) {
            Json key;
            if (!parse_string(key) || key.kind != Json::String)
                return false;
            if (!match(':'))
                return false;
            Json val;
            if (!parse_value(val))
                return false;
            out.o.emplace_back(std::move(key.s), std::move(val));
            skip();
            if (match('}'))
                return true;
            if (!match(','))
                return false;
        }
    }
};

struct FlatAttr {
    std::string key;
    std::string type; // int | float | bool
    bool has_min = false, has_max = false, has_step = false, has_default = false;
    double min = 0, max = 0, step = 0, def = 0;
    bool def_bool = false;
};

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            o.push_back(c);
            break;
        }
    }
    return o;
}

std::string format_number(double v, bool as_int) {
    if (as_int) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

void collect_enum_bounds(const Json& leaf, double& mn, double& mx, bool& have) {
    const Json* entries = leaf.get("EnumEntry");
    if (entries == nullptr || entries->kind != Json::Array)
        return;
    for (const Json& item : entries->a) {
        if (item.kind != Json::Object || item.o.size() != 1)
            continue;
        const Json& body = item.o.front().second;
        const Json* val = body.get("Value");
        double v = 0;
        if (val == nullptr || !val->as_number(v))
            continue;
        if (!have) {
            mn = mx = v;
            have = true;
        } else {
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
    }
}

bool emit_leaf(const std::string& key, const Json& leaf, std::vector<FlatAttr>& out) {
    const Json* type_j = leaf.get("Type");
    if (type_j == nullptr || type_j->kind != Json::String)
        return false;
    const std::string& type = type_j->s;
    FlatAttr attr;
    attr.key = key;
    if (type == "Integer" || type == "Enumeration") {
        attr.type = "int";
    } else if (type == "Float") {
        attr.type = "float";
    } else if (type == "Boolean") {
        attr.type = "bool";
    } else {
        return false; // Category / Command / String / unknown
    }

    double v = 0;
    if (const Json* mn = leaf.get("Min"); mn && mn->as_number(v)) {
        attr.has_min = true;
        attr.min = v;
    }
    if (const Json* mx = leaf.get("Max"); mx && mx->as_number(v)) {
        attr.has_max = true;
        attr.max = v;
    }
    if (const Json* st = leaf.get("Step"); st && st->as_number(v)) {
        attr.has_step = true;
        attr.step = v;
    } else if (const Json* inc = leaf.get("Inc"); inc && inc->as_number(v)) {
        attr.has_step = true;
        attr.step = v;
    }
    if (type == "Enumeration" && !(attr.has_min && attr.has_max)) {
        double emin = 0, emax = 0;
        bool have = false;
        collect_enum_bounds(leaf, emin, emax, have);
        if (have) {
            attr.has_min = attr.has_max = true;
            attr.min = emin;
            attr.max = emax;
        }
    }
    if (!attr.has_step && attr.type == "int" && attr.has_min && attr.has_max) {
        attr.has_step = true;
        attr.step = 1.0; // ROS IntegerRange rejects step 0
    }

    if (const Json* def = leaf.get("DefaultValue")) {
        if (attr.type == "bool") {
            attr.has_default = true;
            attr.def_bool = def->as_bool(false);
        } else if (def->as_number(v)) {
            attr.has_default = true;
            attr.def = v;
        }
    }

    out.push_back(std::move(attr));
    return true;
}

void walk_features(const Json& features, std::vector<FlatAttr>& out,
                   std::map<std::string, size_t>& seen) {
    if (features.kind != Json::Array)
        return;
    for (const Json& item : features.a) {
        if (item.kind != Json::Object || item.o.size() != 1)
            continue;
        const std::string& name = item.o.front().first;
        const Json& body = item.o.front().second;
        if (body.has("Feature")) {
            walk_features(*body.get("Feature"), out, seen);
            continue;
        }
        const size_t before = out.size();
        if (!emit_leaf(name, body, out))
            continue;
        if (seen.count(name)) {
            out.resize(before); // drop duplicate
            continue;
        }
        seen.emplace(name, before);
    }
}

std::string serialize_flat(const std::vector<FlatAttr>& attrs) {
    std::ostringstream os;
    os << "{\"attributes\":[";
    for (size_t i = 0; i < attrs.size(); ++i) {
        const FlatAttr& a = attrs[i];
        if (i)
            os << ',';
        os << "{\"key\":\"" << json_escape(a.key) << "\",\"type\":\"" << a.type << '"';
        if (a.type == "bool") {
            if (a.has_default)
                os << ",\"default\":" << (a.def_bool ? "true" : "false");
        } else {
            const bool as_int = (a.type == "int");
            if (a.has_min)
                os << ",\"min\":" << format_number(a.min, as_int);
            if (a.has_max)
                os << ",\"max\":" << format_number(a.max, as_int);
            if (a.has_step)
                os << ",\"step\":" << format_number(a.step, as_int);
            if (a.has_default)
                os << ",\"default\":" << format_number(a.def, as_int);
        }
        os << '}';
    }
    os << "]}";
    return os.str();
}

} // namespace

std::string flatten_attribute_tree_json(const std::string& input) {
    Parser parser(input);
    Json root;
    if (!parser.parse(root) || root.kind != Json::Object)
        return input;

    // Already ABI-flat (mock backend): leave untouched.
    if (const Json* attrs = root.get("attributes"); attrs && attrs->kind == Json::Array)
        return input;

    const Json* feature = root.get("Feature");
    if (feature == nullptr)
        return input;

    std::vector<FlatAttr> flat;
    std::map<std::string, size_t> seen;
    walk_features(*feature, flat, seen);
    if (flat.empty())
        return input;
    return serialize_flat(flat);
}

} // namespace acam
