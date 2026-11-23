#include "json.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <fstream>

namespace clam {

namespace {

const Json& null_value() {
    static const Json value;
    return value;
}

void escape_into(std::string& out, const std::string& input) {
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

class Parser {
public:
    Parser(const std::string& text, std::string* error) : text_(text), error_(error) {}

    bool parse(Json& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        if (pos_ != text_.size()) return fail("trailing characters");
        return true;
    }

private:
    bool fail(const char* message) {
        if (error_) {
            *error_ = std::string(message) + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool parse_value(Json& out) {
        if (pos_ >= text_.size()) return fail("unexpected end");
        switch (text_[pos_]) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string value;
                if (!parse_string(value)) return false;
                out = Json(value);
                return true;
            }
            case 't':
                if (text_.compare(pos_, 4, "true") == 0) {
                    pos_ += 4;
                    out = Json(true);
                    return true;
                }
                return fail("invalid literal");
            case 'f':
                if (text_.compare(pos_, 5, "false") == 0) {
                    pos_ += 5;
                    out = Json(false);
                    return true;
                }
                return fail("invalid literal");
            case 'n':
                if (text_.compare(pos_, 4, "null") == 0) {
                    pos_ += 4;
                    out = Json();
                    return true;
                }
                return fail("invalid literal");
            default: return parse_number(out);
        }
    }

    bool parse_number(Json& out) {
        size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                digits = true;
            }
        }
        if (digits && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (!digits) return fail("invalid number");
        out = Json(std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected string");
        ++pos_;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail("bad escape");
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return fail("bad \\u escape");
                    unsigned code = static_cast<unsigned>(std::strtoul(text_.substr(pos_, 4).c_str(), nullptr, 16));
                    pos_ += 4;
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        unsigned low = static_cast<unsigned>(std::strtoul(text_.substr(pos_ + 2, 4).c_str(), nullptr, 16));
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            pos_ += 6;
                            unsigned combined = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            append_utf8(out, combined);
                            break;
                        }
                    }
                    append_utf8(out, code);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parse_array(Json& out) {
        ++pos_;  // [
        JsonArray items;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Json(std::move(items));
            return true;
        }
        while (true) {
            Json item;
            skip_ws();
            if (!parse_value(item)) return false;
            items.push_back(std::move(item));
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = Json(std::move(items));
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool parse_object(Json& out) {
        ++pos_;  // {
        JsonObject fields;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Json(std::move(fields));
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected :");
            ++pos_;
            skip_ws();
            Json value;
            if (!parse_value(value)) return false;
            fields[key] = std::move(value);
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = Json(std::move(fields));
                return true;
            }
            return fail("expected , or }");
        }
    }

    const std::string& text_;
    std::string* error_;
    size_t pos_ = 0;
};

}  // namespace

const Json& Json::operator[](const std::string& key) const {
    if (!is_object()) return null_value();
    auto it = object_.find(key);
    return it == object_.end() ? null_value() : it->second;
}

Json& Json::operator[](const std::string& key) {
    type_ = Type::Object;
    return object_[key];
}

bool Json::has(const std::string& key) const {
    return is_object() && object_.find(key) != object_.end();
}

const Json& Json::at(size_t index) const {
    if (!is_array() || index >= array_.size()) return null_value();
    return array_[index];
}

std::string json_escape(const std::string& input) {
    std::string out;
    escape_into(out, input);
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    auto newline = [&](int level) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * level), ' ');
    };

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (std::isfinite(number_)) {
                if (number_ == std::floor(number_) && std::fabs(number_) < 1e15) {
                    char buffer[32];
                    std::snprintf(buffer, sizeof(buffer), "%.0f", number_);
                    out += buffer;
                } else {
                    char buffer[40];
                    std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
                    out += buffer;
                }
            } else {
                out += "0";
            }
            break;
        }
        case Type::String: escape_into(out, string_); break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            bool first = true;
            for (const auto& item : array_) {
                if (!first) out.push_back(',');
                first = false;
                newline(depth + 1);
                item.dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& [key, value] : object_) {
                if (!first) out.push_back(',');
                first = false;
                newline(depth + 1);
                escape_into(out, key);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                value.dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    out.reserve(512);
    dump_to(out, indent, 0);
    return out;
}

std::unique_ptr<Json> Json::parse(const std::string& text, std::string* error) {
    auto value = std::make_unique<Json>();
    Parser parser(text, error);
    if (!parser.parse(*value)) return nullptr;
    return value;
}

std::string url_encode(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string to_lower(const std::string& input) {
    std::string out = input;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();
    while (start < end && std::isspace(static_cast<unsigned char>(input[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return input.substr(start, end - start);
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

std::string format_bytes(size_t bytes) {
    char buffer[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%zu B", bytes);
    }
    return buffer;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

bool read_utf8_file(const std::string& path, std::string& out, size_t max_bytes) {
    std::ifstream file(std::filesystem::path(utf8_to_wide(path)), std::ios::binary);
    if (!file) return false;
    out.clear();
    if (max_bytes > 0) {
        out.resize(max_bytes + 1);
        file.read(out.data(), static_cast<std::streamsize>(out.size()));
        out.resize(static_cast<size_t>(file.gcount()));
    } else {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        out = buffer.str();
    }
    return true;
}

bool write_utf8_file(const std::string& path, const std::string& content, bool append) {
    std::ofstream file(std::filesystem::path(utf8_to_wide(path)),
                       std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

}  // namespace clam
