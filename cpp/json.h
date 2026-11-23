// Minimal JSON value type + parser/serializer, no external dependencies.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clam {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::map<std::string, Json>;

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(const char* value) : type_(Type::String), string_(value ? value : "") {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(JsonArray value) : type_(Type::Array), array_(std::move(value)) {}
    Json(JsonObject value) : type_(Type::Object), object_(std::move(value)) {}

    static Json make_array() { return Json(JsonArray{}); }
    static Json make_object() { return Json(JsonObject{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return is_number() ? number_ : fallback; }
    int as_int(int fallback = 0) const {
        return is_number() ? static_cast<int>(number_) : fallback;
    }
    const std::string& as_string() const { return string_; }
    std::string string_or(const std::string& fallback) const {
        return is_string() ? string_ : fallback;
    }
    const JsonArray& array() const { return array_; }
    JsonArray& array() { return array_; }
    const JsonObject& object() const { return object_; }
    JsonObject& object() { return object_; }

    // Object helpers. Missing keys yield a shared null value.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);
    bool has(const std::string& key) const;
    const Json& at(size_t index) const;

    void push_back(Json value) {
        type_ = Type::Array;
        array_.push_back(std::move(value));
    }
    void set(const std::string& key, Json value) {
        type_ = Type::Object;
        object_[key] = std::move(value);
    }

    std::string dump(int indent = -1) const;

    // Returns nullptr on failure.
    static std::unique_ptr<Json> parse(const std::string& text, std::string* error = nullptr);

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    JsonArray array_;
    JsonObject object_;
};

std::string json_escape(const std::string& input);
std::string url_encode(const std::string& input);
std::string to_lower(const std::string& input);
std::string trim(const std::string& input);
bool contains_ci(const std::string& haystack, const std::string& needle);
std::string format_bytes(size_t bytes);

// UTF-8 <-> UTF-16 conversion for the Win32 API boundary.
std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

// Reading/writing files with UTF-8 paths (MinGW stream constructors need wide paths).
bool read_utf8_file(const std::string& path, std::string& out, size_t max_bytes = 0);
bool write_utf8_file(const std::string& path, const std::string& content, bool append = false);

}  // namespace clam
