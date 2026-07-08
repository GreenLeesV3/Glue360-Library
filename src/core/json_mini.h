// core/json_mini.h — a tiny JSON parser/serializer for state.json.
//
// We avoid a full JSON library: state persistence needs only flat objects of
// string/int/bool values plus arrays of strings. This is internal to core.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace recomp::json {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value {
public:
    Value() : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(long long i) : type_(Type::Int), i_(i) {}
    Value(int i) : type_(Type::Int), i_(i) {}
    Value(double d) : type_(Type::Double), d_(d) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }

    bool as_bool() const { return b_; }
    long long as_int() const { return i_; }
    double as_double() const { return d_; }
    const std::string& as_string() const { return s_; }
    const Array& as_array() const { return *arr_; }
    const Object& as_object() const { return *obj_; }

    const Value& get(const std::string& key) const;
    std::string get_string(const std::string& key, const std::string& def = "") const;
    long long get_int(const std::string& key, long long def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;

    static const Value& null_value();

private:
    Type type_ = Type::Null;
    bool b_ = false;
    long long i_ = 0;
    double d_ = 0.0;
    std::string s_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

Value parse(const std::string& text);
std::string dump(const Value& v);

} // namespace recomp::json
