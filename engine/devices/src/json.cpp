#include <mediaserver/devices/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <utility>

namespace mediaserver::devices {
namespace {

class Parser final {
public:
  Parser(std::string_view text, const JsonLimits &limits) noexcept
      : text_(text), limits_(limits) {}

  [[nodiscard]] std::optional<JsonValue> document() {
    if (text_.size() > limits_.max_bytes) {
      return std::nullopt;
    }
    skip_space();
    // A UTF-8 byte order mark is tolerated at the very start only.
    if (text_.substr(position_, 3U) == "\xEF\xBB\xBF") {
      position_ += 3U;
      skip_space();
    }
    JsonValue value;
    if (!parse_value(value, 0U)) {
      return std::nullopt;
    }
    skip_space();
    if (position_ != text_.size()) {
      return std::nullopt;
    }
    return value;
  }

private:
  std::string_view text_;
  JsonLimits limits_;
  std::size_t position_{};
  std::size_t values_{};

  void skip_space() noexcept {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return;
      }
      ++position_;
    }
  }

  [[nodiscard]] bool literal(std::string_view word) noexcept {
    if (text_.substr(position_, word.size()) != word) {
      return false;
    }
    position_ += word.size();
    return true;
  }

  [[nodiscard]] bool parse_value(JsonValue &out, core::u32 depth) {
    if (depth > limits_.max_depth || ++values_ > limits_.max_values) {
      return false;
    }
    skip_space();
    if (position_ >= text_.size()) {
      return false;
    }
    switch (text_[position_]) {
    case '{':
      return parse_object(out, depth);
    case '[':
      return parse_array(out, depth);
    case '"': {
      std::string text;
      if (!parse_string(text)) {
        return false;
      }
      out = JsonValue::string(std::move(text));
      return true;
    }
    case 't':
      if (!literal("true")) {
        return false;
      }
      out = JsonValue::boolean(true);
      return true;
    case 'f':
      if (!literal("false")) {
        return false;
      }
      out = JsonValue::boolean(false);
      return true;
    case 'n':
      if (!literal("null")) {
        return false;
      }
      out = JsonValue{};
      return true;
    default:
      return parse_number(out);
    }
  }

  [[nodiscard]] bool parse_object(JsonValue &out, core::u32 depth) {
    ++position_;
    out = JsonValue::object();
    skip_space();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return true;
    }
    while (true) {
      skip_space();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return false;
      }
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      skip_space();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return false;
      }
      ++position_;
      JsonValue member;
      if (!parse_value(member, depth + 1U)) {
        return false;
      }
      out.set(std::move(key), std::move(member));
      skip_space();
      if (position_ >= text_.size()) {
        return false;
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  [[nodiscard]] bool parse_array(JsonValue &out, core::u32 depth) {
    ++position_;
    out = JsonValue::array();
    skip_space();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      JsonValue item;
      if (!parse_value(item, depth + 1U)) {
        return false;
      }
      out.push(std::move(item));
      skip_space();
      if (position_ >= text_.size()) {
        return false;
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return true;
      }
      return false;
    }
  }

  [[nodiscard]] static int hex(char c) noexcept {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  [[nodiscard]] bool read_hex4(core::u32 &code) noexcept {
    if (position_ + 4U > text_.size()) {
      return false;
    }
    code = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      const int digit = hex(text_[position_ + index]);
      if (digit < 0) {
        return false;
      }
      code = (code << 4U) | static_cast<core::u32>(digit);
    }
    position_ += 4U;
    return true;
  }

  static void append_utf8(std::string &out, core::u32 code) {
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

  [[nodiscard]] bool parse_string(std::string &out) {
    ++position_;
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) {
        return false;
      }
      const char escape = text_[position_++];
      switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        core::u32 code{};
        if (!read_hex4(code)) {
          return false;
        }
        if (code >= 0xD800U && code <= 0xDBFFU) {
          core::u32 low{};
          if (text_.substr(position_, 2U) != "\\u") {
            return false;
          }
          position_ += 2U;
          if (!read_hex4(low) || low < 0xDC00U || low > 0xDFFFU) {
            return false;
          }
          code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
        } else if (code >= 0xDC00U && code <= 0xDFFFU) {
          return false;
        }
        append_utf8(out, code);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  [[nodiscard]] bool parse_number(JsonValue &out) noexcept {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') {
      ++position_;
    }
    const auto digits = [this] {
      const std::size_t first = position_;
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
      return position_ - first;
    };
    const std::size_t whole_start = position_;
    if (digits() == 0U) {
      return false;
    }
    // No leading zeros, as JSON requires.
    if (text_[whole_start] == '0' && position_ - whole_start > 1U) {
      return false;
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      if (digits() == 0U) {
        return false;
      }
    }
    if (position_ < text_.size() &&
        (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() &&
          (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      if (digits() == 0U) {
        return false;
      }
    }
    double value{};
    const char *const first = text_.data() + start;
    const char *const last = text_.data() + position_;
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) {
      return false;
    }
    out = JsonValue::number(value);
    return true;
  }
};

void write(std::string &out, const JsonValue &value);

void write_string(std::string &out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20U) {
        std::array<char, 8U> escaped{};
        std::snprintf(escaped.data(), escaped.size(), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        out += escaped.data();
      } else {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
}

void write(std::string &out, const JsonValue &value) {
  switch (value.kind()) {
  case JsonValue::Kind::Null:
    out += "null";
    return;
  case JsonValue::Kind::Boolean:
    out += value.as_bool() ? "true" : "false";
    return;
  case JsonValue::Kind::Number: {
    const double number = value.as_number();
    std::array<char, 40U> text{};
    if (std::abs(number) < 9.0e15 && std::floor(number) == number) {
      std::snprintf(text.data(), text.size(), "%lld",
                    static_cast<long long>(number));
    } else {
      std::snprintf(text.data(), text.size(), "%.9g", number);
    }
    out += text.data();
    return;
  }
  case JsonValue::Kind::String:
    write_string(out, value.as_string());
    return;
  case JsonValue::Kind::Array: {
    out.push_back('[');
    bool first = true;
    for (const JsonValue &item : value.items()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      write(out, item);
    }
    out.push_back(']');
    return;
  }
  case JsonValue::Kind::Object: {
    out.push_back('{');
    const auto keys = value.keys();
    const auto items = value.items();
    for (std::size_t index = 0U; index < items.size(); ++index) {
      if (index != 0U) {
        out.push_back(',');
      }
      write_string(out, keys[index]);
      out.push_back(':');
      write(out, items[index]);
    }
    out.push_back('}');
    return;
  }
  }
}

} // namespace

JsonValue JsonValue::boolean(bool value) {
  JsonValue result;
  result.kind_ = Kind::Boolean;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::number(double value) {
  JsonValue result;
  result.kind_ = Kind::Number;
  result.number_ = value;
  return result;
}

JsonValue JsonValue::string(std::string value) {
  JsonValue result;
  result.kind_ = Kind::String;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::array() {
  JsonValue result;
  result.kind_ = Kind::Array;
  return result;
}

JsonValue JsonValue::object() {
  JsonValue result;
  result.kind_ = Kind::Object;
  return result;
}

bool JsonValue::as_bool(bool fallback) const noexcept {
  if (kind_ == Kind::Boolean) {
    return boolean_;
  }
  if (kind_ == Kind::Number) {
    return number_ != 0.0;
  }
  return fallback;
}

double JsonValue::as_number(double fallback) const noexcept {
  if (kind_ == Kind::Number) {
    return number_;
  }
  if (kind_ == Kind::Boolean) {
    return boolean_ ? 1.0 : 0.0;
  }
  return fallback;
}

std::string_view JsonValue::as_string(std::string_view fallback) const noexcept {
  return kind_ == Kind::String ? std::string_view{string_} : fallback;
}

const JsonValue *JsonValue::find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  for (std::size_t index = 0U; index < keys_.size(); ++index) {
    if (keys_[index] == key) {
      return &children_[index];
    }
  }
  return nullptr;
}

double JsonValue::number_at(std::string_view key,
                            double fallback) const noexcept {
  const JsonValue *const member = find(key);
  return member != nullptr ? member->as_number(fallback) : fallback;
}

bool JsonValue::bool_at(std::string_view key, bool fallback) const noexcept {
  const JsonValue *const member = find(key);
  return member != nullptr ? member->as_bool(fallback) : fallback;
}

std::string_view JsonValue::string_at(std::string_view key,
                                      std::string_view fallback) const noexcept {
  const JsonValue *const member = find(key);
  return member != nullptr ? member->as_string(fallback) : fallback;
}

void JsonValue::push(JsonValue value) {
  if (kind_ == Kind::Array) {
    children_.push_back(std::move(value));
  }
}

void JsonValue::set(std::string key, JsonValue value) {
  if (kind_ != Kind::Object) {
    return;
  }
  keys_.push_back(std::move(key));
  children_.push_back(std::move(value));
}

std::optional<JsonValue> parse_json(std::string_view text,
                                    const JsonLimits &limits) {
  Parser parser{text, limits};
  return parser.document();
}

std::string to_json(const JsonValue &value) {
  std::string out;
  write(out, value);
  return out;
}

} // namespace mediaserver::devices
