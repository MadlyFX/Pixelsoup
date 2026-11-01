#include "json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ajfx::studio::json {

Value Value::of(double number) {
    Value value;
    value.type = Type::Number;
    value.number = number;
    return value;
}

Value Value::of(bool flag) {
    Value value;
    value.type = Type::Bool;
    value.boolean = flag;
    return value;
}

Value Value::of(std::string text) {
    Value value;
    value.type = Type::String;
    value.text = std::move(text);
    return value;
}

Value Value::array() {
    Value value;
    value.type = Type::Array;
    return value;
}

Value Value::object() {
    Value value;
    value.type = Type::Object;
    return value;
}

Value& Value::add(std::string key, Value value) {
    members.emplace_back(std::move(key), std::move(value));
    return members.back().second;
}

Value& Value::push(Value value) {
    items.push_back(std::move(value));
    return items.back();
}

const Value* Value::find(std::string_view key) const {
    for (const auto& [name, value] : members) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

double Value::number_or(std::string_view key, double fallback) const {
    const Value* found = find(key);
    return found != nullptr && found->type == Type::Number ? found->number : fallback;
}

bool Value::bool_or(std::string_view key, bool fallback) const {
    const Value* found = find(key);
    return found != nullptr && found->type == Type::Bool ? found->boolean : fallback;
}

std::string Value::string_or(std::string_view key, std::string fallback) const {
    const Value* found = find(key);
    return found != nullptr && found->type == Type::String ? found->text : std::move(fallback);
}

namespace {

void write_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                std::array<char, 8> escaped{};
                std::snprintf(escaped.data(), escaped.size(), "\\u%04x", static_cast<unsigned>(c));
                out += escaped.data();
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_number(std::string& out, double number) {
    if (!std::isfinite(number)) {
        out += '0';
        return;
    }
    // The shortest text that reads back as the same double.
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    out.append(buffer.data(), error == std::errc{} ? end : buffer.data());
}

void write_value(std::string& out, const Value& value, int depth) {
    const auto indent = [&](int level) { out.append(static_cast<std::size_t>(level) * 2U, ' '); };
    switch (value.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += value.boolean ? "true" : "false"; break;
    case Value::Type::Number: write_number(out, value.number); break;
    case Value::Type::String: write_string(out, value.text); break;
    case Value::Type::Array: {
        // Short arrays of numbers stay on one line: vectors and colours.
        bool flat = value.items.size() <= 4U;
        for (const Value& item : value.items) {
            flat = flat && item.type == Value::Type::Number;
        }
        if (value.items.empty() || flat) {
            out += '[';
            for (std::size_t i = 0U; i < value.items.size(); ++i) {
                out += i == 0U ? "" : ", ";
                write_value(out, value.items[i], depth + 1);
            }
            out += ']';
            break;
        }
        out += "[\n";
        for (std::size_t i = 0U; i < value.items.size(); ++i) {
            indent(depth + 1);
            write_value(out, value.items[i], depth + 1);
            out += i + 1U < value.items.size() ? ",\n" : "\n";
        }
        indent(depth);
        out += ']';
        break;
    }
    case Value::Type::Object: {
        if (value.members.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (std::size_t i = 0U; i < value.members.size(); ++i) {
            indent(depth + 1);
            write_string(out, value.members[i].first);
            out += ": ";
            write_value(out, value.members[i].second, depth + 1);
            out += i + 1U < value.members.size() ? ",\n" : "\n";
        }
        indent(depth);
        out += '}';
        break;
    }
    }
}

class Parser final {
public:
    Parser(std::string_view text, ParseError& error) : text_{text}, error_{error} {}

    std::optional<Value> document() {
        std::optional<Value> value = parse_value(0);
        if (!value) {
            return std::nullopt;
        }
        skip_space();
        if (at_ < text_.size()) {
            return fail("unexpected text after the document");
        }
        return value;
    }

private:
    std::optional<Value> fail(std::string message) {
        if (error_.message.empty()) {
            error_.offset = at_;
            error_.message = std::move(message);
        }
        return std::nullopt;
    }

    void skip_space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) == word) {
            at_ += word.size();
            return true;
        }
        return false;
    }

    std::optional<Value> parse_value(int depth) {
        if (depth > 64) {
            return fail("nested too deeply");
        }
        skip_space();
        if (at_ >= text_.size()) {
            return fail("unexpected end of the document");
        }
        const char c = text_[at_];
        if (c == '{') {
            return parse_object(depth);
        }
        if (c == '[') {
            return parse_array(depth);
        }
        if (c == '"') {
            std::optional<std::string> text = parse_string();
            if (!text) {
                return std::nullopt;
            }
            return Value::of(std::move(*text));
        }
        if (literal("true")) {
            return Value::of(true);
        }
        if (literal("false")) {
            return Value::of(false);
        }
        if (literal("null")) {
            return Value{};
        }
        return parse_number();
    }

    std::optional<Value> parse_number() {
        const std::size_t start = at_;
        while (at_ < text_.size() && (std::string_view{"+-0123456789.eE"}.find(text_[at_]) != std::string_view::npos)) {
            ++at_;
        }
        double number = 0.0;
        const auto [end, error] = std::from_chars(text_.data() + start, text_.data() + at_, number);
        if (start == at_ || error != std::errc{} || end != text_.data() + at_) {
            at_ = start;
            return fail("expected a value");
        }
        return Value::of(number);
    }

    std::optional<std::string> parse_string() {
        ++at_; // the opening quote
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) {
                break;
            }
            const char escape = text_[at_++];
            switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (at_ + 4U > text_.size()) {
                    fail("a short \\u escape");
                    return std::nullopt;
                }
                unsigned code = 0U;
                const auto [end, error] = std::from_chars(text_.data() + at_, text_.data() + at_ + 4U, code, 16);
                if (error != std::errc{} || end != text_.data() + at_ + 4U) {
                    fail("a bad \\u escape");
                    return std::nullopt;
                }
                at_ += 4U;
                // UTF-8 for the basic plane; Studio writes nothing else.
                if (code < 0x80U) {
                    out += static_cast<char>(code);
                } else if (code < 0x800U) {
                    out += static_cast<char>(0xC0U | (code >> 6U));
                    out += static_cast<char>(0x80U | (code & 0x3FU));
                } else {
                    out += static_cast<char>(0xE0U | (code >> 12U));
                    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
                    out += static_cast<char>(0x80U | (code & 0x3FU));
                }
                break;
            }
            default: fail("an unknown escape"); return std::nullopt;
            }
        }
        fail("an unterminated string");
        return std::nullopt;
    }

    std::optional<Value> parse_array(int depth) {
        ++at_;
        Value array = Value::array();
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return array;
        }
        while (true) {
            std::optional<Value> item = parse_value(depth + 1);
            if (!item) {
                return std::nullopt;
            }
            array.push(std::move(*item));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return array;
            }
            return fail("expected ',' or ']'");
        }
    }

    std::optional<Value> parse_object(int depth) {
        ++at_;
        Value object = Value::object();
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
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
            skip_space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return fail("expected ':'");
            }
            ++at_;
            std::optional<Value> value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            object.add(std::move(*key), std::move(*value));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return object;
            }
            return fail("expected ',' or '}'");
        }
    }

    std::string_view text_;
    ParseError& error_;
    std::size_t at_{0U};
};

} // namespace

std::string write(const Value& value) {
    std::string out;
    write_value(out, value, 0);
    out += '\n';
    return out;
}

std::optional<Value> parse(std::string_view text, ParseError& error) {
    error = {};
    return Parser{text, error}.document();
}

} // namespace ajfx::studio::json
