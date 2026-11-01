#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A minimal JSON tree for effect.json. Numbers keep their source text so a
// float reads back with exactly the bits it was written with, and objects
// keep member order so writing is deterministic.
namespace ajfx::detail {

struct Json final {
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
    // A number's text, or a string's decoded value.
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    [[nodiscard]] static Json number(std::string text);
    [[nodiscard]] static Json string(std::string value);
    [[nodiscard]] static Json array();
    [[nodiscard]] static Json object();
    [[nodiscard]] static Json flag(bool value);

    // Appends a member to an object; returns the new value.
    Json& add(std::string key, Json value);
    [[nodiscard]] const Json* find(std::string_view key) const;
};

// Two-space indented, keys in insertion order, trailing newline.
[[nodiscard]] std::string write_json(const Json& value);

struct JsonError final {
    std::size_t offset{};
    std::string message;
};

// Parses one JSON value; anything but whitespace after it is an error.
[[nodiscard]] std::optional<Json> parse_json(std::string_view text, JsonError& error);

} // namespace ajfx::detail
