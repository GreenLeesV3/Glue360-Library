// core/json_mini.cpp — minimal JSON parser/serializer for state.json.
#include "core/json_mini.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace recomp::json {

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), i_(0) {}

    Value parse_value() {
        skip_ws();
        if (i_ >= s_.size()) throw std::runtime_error("JSON: unexpected end");
        char c = s_[i_];
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        return parse_number();
    }

private:
    const std::string& s_;
    std::size_t i_;

    void skip_ws() {
        while (i_ < s_.size() &&
               std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }

    Value parse_string() {
        ++i_; // skip opening quote
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                char e = s_[i_ + 1];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    default: out += e; break;
                }
                i_ += 2;
            } else {
                out += s_[i_++];
            }
        }
        if (i_ >= s_.size()) throw std::runtime_error("JSON: unterminated string");
        ++i_; // skip closing quote
        return Value(out);
    }

    Value parse_object() {
        ++i_; // {
        Object obj;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return Value(obj); }
        while (true) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"')
                throw std::runtime_error("JSON: expected key string");
            Value key = parse_string();
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':')
                throw std::runtime_error("JSON: expected ':'");
            ++i_;
            Value val = parse_value();
            obj[key.as_string()] = val;
            skip_ws();
            if (i_ >= s_.size()) throw std::runtime_error("JSON: unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; break; }
            throw std::runtime_error("JSON: expected ',' or '}'");
        }
        return Value(obj);
    }

    Value parse_array() {
        ++i_; // [
        Array arr;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return Value(arr); }
        while (true) {
            Value v = parse_value();
            arr.push_back(v);
            skip_ws();
            if (i_ >= s_.size()) throw std::runtime_error("JSON: unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; break; }
            throw std::runtime_error("JSON: expected ',' or ']'");
        }
        return Value(arr);
    }

    Value parse_bool() {
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; return Value(true); }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; return Value(false); }
        throw std::runtime_error("JSON: invalid literal");
    }

    Value parse_null() {
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return Value(); }
        throw std::runtime_error("JSON: invalid literal");
    }

    Value parse_number() {
        std::size_t start = i_;
        bool is_double = false;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[i_])) ||
                s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                s_[i_] == '+' || s_[i_] == '-')) {
            if (s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E') is_double = true;
            ++i_;
        }
        std::string num = s_.substr(start, i_ - start);
        if (is_double) return Value(std::stod(num));
        return Value(static_cast<long long>(std::stoll(num)));
    }
};

void dump_string(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\t': os << "\\t"; break;
            case '\r': os << "\\r"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

void dump_value(std::ostringstream& os, const Value& v) {
    switch (v.type()) {
        case Type::Null:   os << "null"; break;
        case Type::Bool:   os << (v.as_bool() ? "true" : "false"); break;
        case Type::Int:    os << v.as_int(); break;
        case Type::Double: os << v.as_double(); break;
        case Type::String: dump_string(os, v.as_string()); break;
        case Type::Array: {
            os << '[';
            bool first = true;
            for (const auto& e : v.as_array()) {
                if (!first) os << ',';
                dump_value(os, e);
                first = false;
            }
            os << ']';
            break;
        }
        case Type::Object: {
            os << '{';
            bool first = true;
            for (const auto& [k, val] : v.as_object()) {
                if (!first) os << ',';
                dump_string(os, k);
                os << ':';
                dump_value(os, val);
                first = false;
            }
            os << '}';
            break;
        }
    }
}

} // namespace

const Value& Value::get(const std::string& key) const {
    if (type_ != Type::Object) return null_value();
    auto it = obj_->find(key);
    if (it == obj_->end()) return null_value();
    return it->second;
}

std::string Value::get_string(const std::string& key, const std::string& def) const {
    const Value& v = get(key);
    if (v.type_ != Type::String) return def;
    return v.s_;
}

long long Value::get_int(const std::string& key, long long def) const {
    const Value& v = get(key);
    if (v.type_ != Type::Int) return def;
    return v.i_;
}

bool Value::get_bool(const std::string& key, bool def) const {
    const Value& v = get(key);
    if (v.type_ != Type::Bool) return def;
    return v.b_;
}

const Value& Value::null_value() {
    static const Value n;
    return n;
}

Value parse(const std::string& text) {
    Parser p(text);
    return p.parse_value();
}

std::string dump(const Value& v) {
    std::ostringstream os;
    dump_value(os, v);
    return os.str();
}

} // namespace recomp::json
