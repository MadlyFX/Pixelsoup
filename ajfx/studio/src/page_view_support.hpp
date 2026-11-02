#pragma once

#include "page_view.hpp"

#include "catalog.hpp"
#include "raw_modules.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// What the page's drawing code (page_view*.cpp) shares: the sizes it lays
// rows out by and the small helpers it reads values with. Internal to the
// page; nothing else includes this.
namespace ajfx::studio {
namespace page_ui {

namespace editor = mediaserver::editor;

constexpr float page_max_width = 720.0F;
constexpr float header_height = 34.0F;
constexpr float row_height = 26.0F;
constexpr float row_gap = 4.0F;
constexpr float label_width = 112.0F;
constexpr float hood_line = 17.0F;
constexpr float layer_header = 36.0F;
// A scope heading over a rigid layer's blocks, and the air after its group.
constexpr float scope_heading = 24.0F;
constexpr float scope_gap = 8.0F;
// How far an Object's own blocks sit in from it, so a page reads as each
// thing in the layer and what becomes of it.
constexpr float object_indent = 16.0F;
// How much of a hidden row's ink is left.
constexpr float hidden_dim = 0.45F;
// The starter tiles of a layer with nothing born in it: room for a tile's
// icon, the gap between tiles, and the page width from which two sit side by
// side.
constexpr float starter_icon_room = 46.0F;
constexpr float starter_gap = 8.0F;
constexpr float starter_two_columns = 440.0F;
// A starter tile is one line: its icon and its name.
constexpr float starter_height = 36.0F;
// A next-step chip: its height, the gap between chips, and the room beside its
// words for the plus and the padding.
constexpr float next_chip_height = 26.0F;
constexpr float next_chip_gap = 6.0F;
constexpr float next_chip_room = 46.0F;

inline gui::UiColor to_ui(const Value& v) {
    return gui::UiColor{v.x, v.y, v.z, v.w};
}

// Greys out a hidden row's controls: they stay editable under a veil of the
// page's own colour.
inline void grey_out(gui::UiContext& ctx, const gui::Rect& area) {
    ctx.rounded(area, gui::fade(ctx.theme().panel, 1.0F - hidden_dim), ctx.theme().radius);
}

// "Rigid physics" -> "rigid physics", to sit inside a sentence.
inline std::string lower_words(std::string_view text) {
    std::string out{text};
    if (!out.empty()) {
        out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    }
    return out;
}

// What choosing a tile or chip adds, by the blocks' names in the picker:
// "Adds Stream, Gravity and Ground".
inline std::string suggestion_adds(const Suggestion& suggestion) {
    std::string adds = "Adds ";
    for (std::size_t b = 0U; b < suggestion.blocks.size(); ++b) {
        const BlockDef* def = find_block_def(suggestion.blocks[b].kind);
        adds += (b == 0U ? "" : b + 1U == suggestion.blocks.size() ? " and " : ", ") +
                std::string{def != nullptr ? def->title : block_kind_id(suggestion.blocks[b].kind)};
    }
    return adds;
}

inline std::string readable(std::string_view field) {
    std::string out{field};
    std::replace(out.begin(), out.end(), '_', ' ');
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

inline std::string default_text(const ControlDef& control) {
    const Value& v = control.fallback;
    switch (control.type) {
    case ControlType::Number: return format_number(v.x, control.decimals) + std::string{control.unit.empty() ? "" : " "} + std::string{control.unit};
    case ControlType::Range: return format_number(v.x, control.decimals) + " to " + format_number(v.y, control.decimals);
    case ControlType::Vec3:
        return format_number(v.x, 2) + ", " + format_number(v.y, 2) + ", " + format_number(v.z, 2);
    case ControlType::Color: return "the starting colour";
    case ControlType::File: return "no file";
    case ControlType::SolidLayer: return "every solid layer";
    case ControlType::Toggle: return v.x != 0.0F ? "on" : "off";
    case ControlType::Choice:
        for (const ChoiceDef& option : control.choices) {
            if (option.id == v.text) {
                return std::string{option.label};
            }
        }
        return v.text;
    }
    return {};
}

inline std::vector<std::string> hood_lines_for(const PageInputs& inputs, Uid layer, Uid origin, bool with_renderer) {
    std::vector<std::string> lines;
    if (!inputs.expert || inputs.compiled == nullptr) {
        return lines;
    }
    const LayerReport* report = inputs.compiled->layer(layer);
    if (report == nullptr) {
        return lines;
    }
    for (const ModuleLine& line : report->modules) {
        if (line.origin == origin) {
            lines.push_back(std::string{raw_stage(line.module)} + ": " + std::string{raw_kind(line.module)} + "   " +
                            describe_module(line.module));
        }
    }
    if (with_renderer && report->error.empty()) {
        lines.push_back("draw: " + describe_renderer(report->renderer));
        lines.push_back("budget: " + thousands(report->capacity) + " particles, " +
                        thousands((report->bytes + 1023U) / 1024U) + " KB of state");
    }
    return lines;
}

// Rows the Module block's fields take.
struct RawRows final {
    float rows{0.0F};
    template <class T>
    void operator()(std::string_view, const T& field) {
        if constexpr (std::is_same_v<T, std::vector<ColorKey>> || std::is_same_v<T, std::vector<CurveKey>>) {
            rows += static_cast<float>(field.size()) + 1.0F;
        } else {
            rows += 1.0F;
        }
    }
};
} // namespace page_ui
} // namespace ajfx::studio
