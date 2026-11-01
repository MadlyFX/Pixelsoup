#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small JSON tree for Studio documents. Objects keep member order, so a
// saved page diffs cleanly and writing is deterministic.
namespace ajfx::studio::json {

struct Value final {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    [[nodiscard]] static Value of(double number);
    [[nodiscard]] static Value of(bool flag);
    [[nodiscard]] static Value of(std::string text);
    [[nodiscard]] static Value of(const char* text) { return of(std::string{text}); }
    [[nodiscard]] static Value array();
    [[nodiscard]] static Value object();

    // Appends a member to an object, or an item to an array.
    Value& add(std::string key, Value value);
    Value& push(Value value);

    [[nodiscard]] const Value* find(std::string_view key) const;
    // A member's value, or `fallback` when it is missing or of another type.
    [[nodiscard]] double number_or(std::string_view key, double fallback) const;
    [[nodiscard]] bool bool_or(std::string_view key, bool fallback) const;
    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback) const;
};

// Two-space indented, members in insertion order, trailing newline.
[[nodiscard]] std::string write(const Value& value);

struct ParseError final {
    std::size_t offset{};
    std::string message;
};

// One JSON value; anything but whitespace after it is an error.
[[nodiscard]] std::optional<Value> parse(std::string_view text, ParseError& error);

} // namespace ajfx::studio::json
