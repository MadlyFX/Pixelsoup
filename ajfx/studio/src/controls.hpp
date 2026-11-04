#pragma once

#include <ajfx/parameters.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The controls a scene item's properties are edited with (scene_properties.hpp)
// and the values they hold: one shape for every control, so storing,
// resetting, validating and drawing them is generic.
namespace ajfx::studio {

// A control's value: a number uses x; a range x (low) and y (high); a vector
// x, y and z; a colour x, y, z and w; a choice or a file `text`; a switch x
// (0 or 1).
struct Value final {
    float x{};
    float y{};
    float z{};
    float w{};
    std::string text;

    friend bool operator==(const Value&, const Value&) = default;
};

[[nodiscard]] inline Value number(float x) { return Value{x, 0.0F, 0.0F, 0.0F, {}}; }
[[nodiscard]] inline Value range(float low, float high) { return Value{low, high, 0.0F, 0.0F, {}}; }
[[nodiscard]] inline Value vec3(float x, float y, float z) { return Value{x, y, z, 0.0F, {}}; }
[[nodiscard]] inline Value color(float r, float g, float b, float a = 1.0F) { return Value{r, g, b, a, {}}; }
[[nodiscard]] inline Value choice(std::string id) { return Value{0.0F, 0.0F, 0.0F, 0.0F, std::move(id)}; }
[[nodiscard]] inline Value toggle(bool on) { return Value{on ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F, {}}; }

enum class ControlType : std::uint8_t {
    Number,
    Range,
    Vec3,
    Color,
    Choice,
    Toggle,
    // A file on disk, its path in `text`: a 3D model (`files`).
    File,
};

// What a File control picks.
enum class FileKind : std::uint8_t {
    Image, // a picture: PNG, JPEG, TGA, BMP
    Model, // a 3D model: OBJ, glTF, GLB
};

struct ChoiceDef final {
    std::string_view id;
    std::string_view label;
};

struct ControlDef final {
    std::string_view id;
    std::string_view label;
    ControlType type{ControlType::Number};
    Value fallback{};
    // The range a slider covers, and the range a typed value is held to.
    float min{0.0F};
    float max{1.0F};
    float hard_min{-1.0e9F};
    float hard_max{1.0e9F};
    std::string_view unit;
    int decimals{2};
    std::vector<ChoiceDef> choices;
    std::string_view help;
    // When the value takes effect: bake-time values are read by the
    // simulation, so a cache is taken with one value of each; playback-time
    // ones are applied as particles are drawn.
    ParameterTiming timing{ParameterTiming::Bake};
    // For a File control.
    FileKind files{FileKind::Model};
};

// Clamps to the control's hard range; a choice not on the list falls back.
[[nodiscard]] Value sanitize(const ControlDef& control, Value value);

// "0.25", "12", "1.5 m".
[[nodiscard]] std::string format_number(float value, int decimals);
// A range as words: "2 m/s" when both ends agree, else "1 to 2 m/s".
[[nodiscard]] std::string format_range(const Value& range, int decimals, std::string_view unit);
// The file name a path ends in, for reading: "rock.glb".
[[nodiscard]] std::string file_label(std::string_view path);

} // namespace ajfx::studio
