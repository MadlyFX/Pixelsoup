#pragma once

#include "document.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The blocks an artist builds a layer from. Each entry says what the block is
// for in a sentence, which controls it has and what they default to. The
// controls an artist needs first are Essential; the rest sit under "More".
// What a block turns into is the compiler's business (compile.cpp).
namespace ajfx::studio {

// The Module block's kind: one raw ajfx module.
inline constexpr std::string_view raw_block_kind = "module";

enum class Category : std::uint8_t {
    Birth,   // how many particles, where they start and how they leave
    Motion,  // the forces on them
    Look,    // how they are drawn
    Contact, // what they hit
    Physics, // solid pieces that collide with each other
    Light,   // light they give the scene
    Expert,  // raw modules
};

[[nodiscard]] std::string_view category_name(Category category) noexcept;

enum class ControlType : std::uint8_t {
    Number,
    Range,
    Vec3,
    Color,
    Choice,
    Toggle,
    // A file on disk, its path in `text`: an image or a 3D model (`files`).
    File,
};

// What a File control picks.
enum class FileKind : std::uint8_t {
    Image, // a picture for sprites: PNG, JPEG, TGA, BMP
    Model, // a 3D model: OBJ, glTF, GLB
};

enum class Level : std::uint8_t {
    Essential,
    Detail,
};

struct ChoiceDef final {
    std::string_view id;
    std::string_view label;
};

// Shown only while another control of the block holds (or, with `negate`,
// does not hold) one of `values`. A switch reads as "on" or "off".
struct Condition final {
    std::string_view control;
    std::array<std::string_view, 3> values{};
    bool negate{false};
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
    Level level{Level::Essential};
    std::string_view help;
    std::optional<Condition> shown_if;
    // Can be published for a host to set (see Published): the control maps
    // onto one runtime input, so a host's value means what the control does.
    bool publishable{false};
    // A range that was once one number: a page saved then gives the number
    // as its high end, from 0 (a spread of 20 is 0 to 20), rather than as
    // both ends.
    bool number_is_high{false};
    // For a File control.
    FileKind files{FileKind::Image};
};

struct BlockDef final {
    std::string_view kind;
    std::string_view title;
    // One sentence: what the block does, in the artist's words.
    std::string_view description;
    Category category{Category::Birth};
    std::vector<ControlDef> controls;
    // Extra words the block picker matches.
    std::string_view keywords;
    // At most one of these in a layer.
    bool unique{false};
    // Can be turned into the Module blocks it compiles to.
    bool detachable{true};
};

[[nodiscard]] const std::vector<BlockDef>& block_catalog();
[[nodiscard]] const BlockDef* find_block_def(std::string_view kind);
[[nodiscard]] const ControlDef* find_control(const BlockDef& def, std::string_view id);

// A control's value: what the block stores, else its default.
[[nodiscard]] Value value_of(const Block& block, const ControlDef& control);
[[nodiscard]] float number_of(const Block& block, std::string_view control);
[[nodiscard]] Value get(const Block& block, std::string_view control);
[[nodiscard]] std::string choice_of(const Block& block, std::string_view control);
[[nodiscard]] bool toggle_of(const Block& block, std::string_view control);
// A File control's path, empty when none is picked.
[[nodiscard]] std::string file_of(const Block& block, std::string_view control);
// The file name a path ends in, for reading on the page: "rock.glb".
[[nodiscard]] std::string file_label(std::string_view path);

// Clamps to the control's hard range; a choice not on the list falls back.
[[nodiscard]] Value sanitize(const ControlDef& control, Value value);

[[nodiscard]] bool control_visible(const Block& block, const BlockDef& def, const ControlDef& control);

// A short reading of the block's values, shown beside its title.
[[nodiscard]] std::string block_summary(const Block& block);
[[nodiscard]] std::string block_title(const Block& block);

// "0.25", "12", "1.5 m".
[[nodiscard]] std::string format_number(float value, int decimals);
// A range as words: "2 m/s" when both ends agree, else "1 to 2 m/s".
[[nodiscard]] std::string format_range(const Value& range, int decimals, std::string_view unit);

} // namespace ajfx::studio
