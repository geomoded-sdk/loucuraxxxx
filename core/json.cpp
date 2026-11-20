#include "core/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lx {

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    // Maximum container nesting the parser will descend into. A configuration
    // file is a handful of levels deep; anything past this is either corrupt or
    // a deliberate stack-exhaustion attempt, so it is rejected instead of being
    // recursed into until the process crashes.
    static constexpr int kMaxDepth = 64;

    Result<Json> parse() {
        skip_ws();
        auto value = parse_value();
        if (!value.ok()) return value.status();
        skip_ws();
        if (pos_ != text_.size()) {
            return Status::error(ErrorCode::ParseError,
                                 "trailing characters at offset " + std::to_string(pos_));
        }
        return value;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    Status fail(const std::string& message) {
        return Status::error(ErrorCode::ParseError,
                             message + " at offset " + std::to_string(pos_));
    }

    // Decrements the depth counter on every exit path, including the early
    // returns scattered through parse_object/parse_array.
    struct DepthGuard {
        int& depth;
        ~DepthGuard() { --depth; }
    };
    DepthGuard make_depth_guard() { return DepthGuard{depth_}; }

    Result<Json> parse_value() {
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        const char c = text_[pos_];
        switch (c) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string_value();
            case 't': return parse_literal("true", Json(true));
            case 'f': return parse_literal("false", Json(false));
            case 'n': return parse_literal("null", Json());
            default:
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
                    return parse_number();
                }
                return fail("unexpected character");
        }
    }

    Result<Json> parse_literal(const char* literal, Json value) {
        const std::size_t len = std::string(literal).size();
        if (text_.compare(pos_, len, literal) != 0) return fail("invalid literal");
        pos_ += len;
        return value;
    }

    Result<Json> parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ == start || (pos_ == start + 1 && text_[start] == '-')) {
            return fail("invalid number");
        }
        const std::string token = text_.substr(start, pos_ - start);
        return Json(std::strtod(token.c_str(), nullptr));
    }

    Result<Json> parse_string_raw(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return Json();
            if (c == '\\') {
                if (pos_ >= text_.size()) return fail("truncated escape");
                const char e = text_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = text_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("invalid \\u escape");
                        }
                        // Encode as UTF-8 (BMP only; surrogates emitted as-is).
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("invalid escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    Result<Json> parse_string_value() {
        std::string out;
        auto r = parse_string_raw(out);
        if (!r.ok()) return r.status();
        return Json(std::move(out));
    }

    Result<Json> parse_object() {
        if (depth_ >= kMaxDepth) return fail("maximum nesting depth exceeded");
        ++depth_;
        const auto guard = make_depth_guard();
        ++pos_;  // consume '{'
        Json obj = Json::object();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return obj;
        }
        while (true) {
            skip_ws();
            std::string key;
            auto key_res = parse_string_raw(key);
            if (!key_res.ok()) return key_res.status();
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            auto value = parse_value();
            if (!value.ok()) return value.status();
            obj[key] = std::move(value.value());
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; break; }
            return fail("expected ',' or '}'");
        }
        return obj;
    }

    Result<Json> parse_array() {
        if (depth_ >= kMaxDepth) return fail("maximum nesting depth exceeded");
        ++depth_;
        const auto guard = make_depth_guard();
        ++pos_;  // consume '['
        Json arr = Json::array();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return arr;
        }
        while (true) {
            skip_ws();
            auto value = parse_value();
            if (!value.ok()) return value.status();
            arr.push_back(std::move(value.value()));
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; break; }
            return fail("expected ',' or ']'");
        }
        return arr;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

void escape_string(const std::string& in, std::string& out) {
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string number_to_string(double value) {
    if (std::isfinite(value) && value == std::floor(value) &&
        std::abs(value) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

}  // namespace

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && object_.count(key) > 0;
}

const Json& Json::at(const std::string& key) const {
    static const Json kNull;
    if (type_ != Type::Object) return kNull;
    auto it = object_.find(key);
    return it == object_.end() ? kNull : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        array_.clear();
    }
    return object_[key];
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    const std::string pad(indent > 0 ? static_cast<std::size_t>(indent) * depth : 0, ' ');
    const std::string pad_inner(
        indent > 0 ? static_cast<std::size_t>(indent) * (depth + 1) : 0, ' ');
    const char* newline = indent > 0 ? "\n" : "";

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: out += number_to_string(number_); break;
        case Type::String: escape_string(string_, out); break;
        case Type::Array: {
            if (array_.empty()) { out += "[]"; break; }
            out += "[";
            out += newline;
            for (std::size_t i = 0; i < array_.size(); ++i) {
                out += pad_inner;
                array_[i].dump_to(out, indent, depth + 1);
                if (i + 1 < array_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "]";
            break;
        }
        case Type::Object: {
            if (object_.empty()) { out += "{}"; break; }
            out += "{";
            out += newline;
            std::size_t i = 0;
            for (const auto& [key, value] : object_) {
                out += pad_inner;
                escape_string(key, out);
                out += indent > 0 ? ": " : ":";
                value.dump_to(out, indent, depth + 1);
                if (++i < object_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "}";
            break;
        }
    }
}

Result<Json> Json::parse(const std::string& text) {
    Parser parser(text);
    return parser.parse();
}

}  // namespace lx
