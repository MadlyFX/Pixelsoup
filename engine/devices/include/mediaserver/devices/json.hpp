#pragma once

#include <mediaserver/core/types.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A small JSON reader for the answers device control APIs send.
//
// A device is outside the trust boundary, so the reader is bounded in size,
// nesting and value count, and refuses anything that is not well-formed JSON
// rather than guessing. It builds a plain tree: the answers are a few hundred
// kilobytes at most and are read once, off the control thread.
namespace mediaserver::devices {

class JsonValue final {
public:
  enum class Kind : core::u8 {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
  };

  JsonValue() = default;

  [[nodiscard]] static JsonValue boolean(bool value);
  [[nodiscard]] static JsonValue number(double value);
  [[nodiscard]] static JsonValue string(std::string value);
  [[nodiscard]] static JsonValue array();
  [[nodiscard]] static JsonValue object();

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_object() const noexcept {
    return kind_ == Kind::Object;
  }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }

  // The value, or `fallback` when it is of another kind. A boolean reads as
  // a number (0 or 1) and a numeric string does not: devices disagree on the
  // first and the second is a bug worth noticing.
  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
  [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
  [[nodiscard]] std::string_view as_string(
      std::string_view fallback = {}) const noexcept;

  // An array's items, or an object's values in document order.
  [[nodiscard]] std::span<const JsonValue> items() const noexcept {
    return children_;
  }
  // An object's keys, parallel to items().
  [[nodiscard]] std::span<const std::string> keys() const noexcept {
    return keys_;
  }
  // The first member called `key`, or null when this is not an object or
  // has none.
  [[nodiscard]] const JsonValue *find(std::string_view key) const noexcept;

  // Shorthands for a member's value with a fallback.
  [[nodiscard]] double number_at(std::string_view key,
                                 double fallback = 0.0) const noexcept;
  [[nodiscard]] bool bool_at(std::string_view key,
                             bool fallback = false) const noexcept;
  [[nodiscard]] std::string_view string_at(
      std::string_view key, std::string_view fallback = {}) const noexcept;

  void push(JsonValue value);
  void set(std::string key, JsonValue value);

private:
  Kind kind_{Kind::Null};
  bool boolean_{};
  double number_{};
  std::string string_;
  std::vector<std::string> keys_;
  std::vector<JsonValue> children_;
};

struct JsonLimits final {
  std::size_t max_bytes{8U * 1024U * 1024U};
  core::u32 max_depth{48U};
  std::size_t max_values{2'000'000U};
};

// The document, or nothing for text that is not one complete JSON value
// within the limits.
[[nodiscard]] std::optional<JsonValue> parse_json(std::string_view text,
                                                  const JsonLimits &limits = {});

// Serializes a value compactly. Numbers that are whole print without a
// decimal point, so a request body reads {"width":3840}.
[[nodiscard]] std::string to_json(const JsonValue &value);

} // namespace mediaserver::devices
