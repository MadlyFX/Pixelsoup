#include "json.hpp"

#include <cstdint>

namespace ajfx::detail {

Json Json::number(std::string text) {
    Json json;
    json.type = Type::Number;
    json.text = std::move(text);
    return json;
}

Json Json::string(std::string value) {
    Json json;
    json.type = Type::String;
    json.text = std::move(value);
    return json;
}

Json Json::array() {
    Json json;
    json.type = Type::Array;
    return json;
}

Json Json::object() {
    Json json;
    json.type = Type::Object;
    return json;
}

Json Json::flag(bool value) {
    Json json;
    json.type = Type::Bool;
    json.boolean = value;
    return json;
}

Json& Json::add(std::string key, Json value) {
    members.emplace_back(std::move(key), std::move(value));
    return members.back().second;
}

const Json* Json::find(std::string_view key) const {
    for (const auto& [name, value] : members) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

void write_string(std::string& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20U) {
            out += "\\u00";
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0xFU]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

bool is_scalar(const Json& value) {
    return value.type != Json::Type::Array && value.type != Json::Type::Object;
}

// Arrays of scalars stay on one line, so a vector or colour reads as one value.
bool is_flat(const Json& value) {
    for (const Json& item : value.items) {
        if (!is_scalar(item)) {
            return false;
        }
    }
    return true;
}

// So do objects of at most two scalar or flat members: a parameter binding,
// or a curve key with its value.
bool is_inline(const Json& value) {
    if (value.members.size() > 2U) {
        return false;
    }
    for (const auto& [name, member] : value.members) {
        if (!is_scalar(member) && !(member.type == Json::Type::Array && is_flat(member))) {
            return false;
        }
    }
    return true;
}

void write_value(std::string& out, const Json& value, std::size_t depth) {
    const auto newline = [&out](std::size_t level) {
        out.push_back('\n');
        out.append(level * 2U, ' ');
    };
    switch (value.type) {
    case Json::Type::Null:
        out += "null";
        break;
    case Json::Type::Bool:
        out += value.boolean ? "true" : "false";
        break;
    case Json::Type::Number:
        out += value.text;
        break;
    case Json::Type::String:
        write_string(out, value.text);
        break;
    case Json::Type::Array:
        if (value.items.empty()) {
            out += "[]";
        } else if (is_flat(value)) {
            out.push_back('[');
            for (std::size_t i = 0U; i < value.items.size(); ++i) {
                out += i == 0U ? "" : ", ";
                write_value(out, value.items[i], depth + 1U);
            }
            out.push_back(']');
        } else {
            out.push_back('[');
            for (std::size_t i = 0U; i < value.items.size(); ++i) {
                out += i == 0U ? "" : ",";
                newline(depth + 1U);
                write_value(out, value.items[i], depth + 1U);
            }
            newline(depth);
            out.push_back(']');
        }
        break;
    case Json::Type::Object:
        if (value.members.empty()) {
            out += "{}";
            break;
        }
        if (is_inline(value)) {
            out += "{";
            for (std::size_t i = 0U; i < value.members.size(); ++i) {
                out += i == 0U ? "" : ", ";
                write_string(out, value.members[i].first);
                out += ": ";
                write_value(out, value.members[i].second, depth + 1U);
            }
            out += "}";
            break;
        }
        out.push_back('{');
        for (std::size_t i = 0U; i < value.members.size(); ++i) {
            out += i == 0U ? "" : ",";
            newline(depth + 1U);
            write_string(out, value.members[i].first);
            out += ": ";
            write_value(out, value.members[i].second, depth + 1U);
        }
        newline(depth);
        out.push_back('}');
        break;
    }
}

class Parser final {
public:
    Parser(std::string_view text, JsonError& error) : text_{text}, error_{error} {}

    std::optional<Json> document() {
        std::optional<Json> value = parse_value(0U);
        if (!value) {
            return std::nullopt;
        }
        skip_space();
        if (at_ < text_.size()) {
            return fail("unexpected text after the value");
        }
        return value;
    }

private:
    static constexpr std::size_t max_depth = 64U;

    std::nullopt_t fail(std::string message) {
        error_ = JsonError{at_, std::move(message)};
        return std::nullopt;
    }

    void skip_space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    bool consume(std::string_view token) {
        if (text_.substr(at_, token.size()) == token) {
            at_ += token.size();
            return true;
        }
        return false;
    }

    std::optional<Json> parse_value(std::size_t depth) {
        if (depth > max_depth) {
            return fail("nesting is too deep");
        }
        skip_space();
        if (at_ >= text_.size()) {
            return fail("expected a value");
        }
        const char c = text_[at_];
        if (c == '{') {
            return parse_object(depth);
        }
        if (c == '[') {
            return parse_array(depth);
        }
        if (c == '"') {
            std::optional<std::string> value = parse_string();
            if (!value) {
                return std::nullopt;
            }
            return Json::string(*std::move(value));
        }
        if (consume("true")) {
            return Json::flag(true);
        }
        if (consume("false")) {
            return Json::flag(false);
        }
        if (consume("null")) {
            return Json{};
        }
        return parse_number();
    }

    std::optional<Json> parse_number() {
        const std::size_t start = at_;
        if (at_ < text_.size() && text_[at_] == '-') {
            ++at_;
        }
        const auto digits = [this] {
            const std::size_t from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
            return at_ > from;
        };
        if (!digits()) {
            return fail("expected a value");
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digits()) {
                return fail("expected digits after '.'");
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (!digits()) {
                return fail("expected an exponent");
            }
        }
        return Json::number(std::string{text_.substr(start, at_ - start)});
    }

    std::optional<std::uint32_t> parse_hex4() {
        if (text_.size() - at_ < 4U) {
            return std::nullopt;
        }
        std::uint32_t value = 0U;
        for (std::size_t i = 0U; i < 4U; ++i) {
            const char c = text_[at_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    std::optional<std::string> parse_string() {
        ++at_; // opening quote
        std::string out;
        while (true) {
            if (at_ >= text_.size()) {
                fail("unterminated string");
                return std::nullopt;
            }
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                fail("unterminated escape");
                return std::nullopt;
            }
            const char e = text_[at_++];
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
                std::optional<std::uint32_t> code = parse_hex4();
                if (!code) {
                    fail("bad \\u escape");
                    return std::nullopt;
                }
                if (*code >= 0xD800U && *code < 0xDC00U) {
                    std::optional<std::uint32_t> low;
                    if (consume("\\u")) {
                        low = parse_hex4();
                    }
                    if (!low || *low < 0xDC00U || *low >= 0xE000U) {
                        fail("unpaired surrogate");
                        return std::nullopt;
                    }
                    code = 0x10000U + ((*code - 0xD800U) << 10U) + (*low - 0xDC00U);
                } else if (*code >= 0xDC00U && *code < 0xE000U) {
                    fail("unpaired surrogate");
                    return std::nullopt;
                }
                append_utf8(out, *code);
                break;
            }
            default:
                fail("unknown escape");
                return std::nullopt;
            }
        }
    }

    std::optional<Json> parse_array(std::size_t depth) {
        ++at_;
        Json array = Json::array();
        skip_space();
        if (consume("]")) {
            return array;
        }
        while (true) {
            std::optional<Json> item = parse_value(depth + 1U);
            if (!item) {
                return std::nullopt;
            }
            array.items.push_back(*std::move(item));
            skip_space();
            if (consume("]")) {
                return array;
            }
            if (!consume(",")) {
                return fail("expected ',' or ']'");
            }
        }
    }

    std::optional<Json> parse_object(std::size_t depth) {
        ++at_;
        Json object = Json::object();
        skip_space();
        if (consume("}")) {
            return object;
        }
        while (true) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') {
                return fail("expected a member name");
            }
            std::optional<std::string> key = parse_string();
            if (!key) {
                return std::nullopt;
            }
            if (object.find(*key) != nullptr) {
                return fail("duplicate member '" + *key + "'");
            }
            skip_space();
            if (!consume(":")) {
                return fail("expected ':'");
            }
            std::optional<Json> value = parse_value(depth + 1U);
            if (!value) {
                return std::nullopt;
            }
            object.add(*std::move(key), *std::move(value));
            skip_space();
            if (consume("}")) {
                return object;
            }
            if (!consume(",")) {
                return fail("expected ',' or '}'");
            }
        }
    }

    std::string_view text_;
    JsonError& error_;
    std::size_t at_{0U};
};

} // namespace

std::string write_json(const Json& value) {
    std::string out;
    write_value(out, value, 0U);
    out.push_back('\n');
    return out;
}

std::optional<Json> parse_json(std::string_view text, JsonError& error) {
    return Parser{text, error}.document();
}

} // namespace ajfx::detail
