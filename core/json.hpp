// LoucuraXXXX GPU - minimal dependency-free JSON.
//
// Used for configuration files and shader manifests. Deliberately small: only
// the subset of JSON needed by the project, with strict parsing so malformed
// input is reported rather than silently coerced.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"

namespace lx {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(unsigned value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(unsigned long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(const char* value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    bool as_bool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }
    double as_number(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    long long as_int(long long fallback = 0) const {
        if (type_ != Type::Number) return fallback;
        // A double outside the representable integer range (or NaN) makes the
        // cast UB, so clamp before converting.
        const double v = number_;
        if (v != v) return fallback;  // NaN
        if (v >= 9223372036854775807.0) return 9223372036854775807LL;
        if (v <= -9223372036854775808.0) return -9223372036854775807LL - 1;
        return static_cast<long long>(v);
    }
    std::string as_string(const std::string& fallback = "") const {
        return type_ == Type::String ? string_ : fallback;
    }

    // Object access. has() checks presence; operator[] auto-creates for writes.
    bool has(const std::string& key) const;
    const Json& at(const std::string& key) const;
    Json& operator[](const std::string& key);

    // Array access.
    const std::vector<Json>& items() const { return array_; }
    void push_back(Json value) { array_.push_back(std::move(value)); }
    std::size_t size() const {
        return type_ == Type::Array ? array_.size()
                                    : (type_ == Type::Object ? object_.size() : 0);
    }

    std::string dump(int indent = 2) const;

    // Parses text; returns the value or a Status describing the failure.
    static Result<Json> parse(const std::string& text);

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

}  // namespace lx
