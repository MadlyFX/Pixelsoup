#pragma once

#include "controls.hpp"

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

// Studio's property rows in Dear ImGui: a label, the control for its type, and
// a reset arrow where the value is not the default. One row for every
// ControlType the scene's properties use, so the inspector is drawn from the
// same registry that persists them.
namespace ajfx::studio::im {

struct Edit final {
    bool changed{false};
    // Vec3: the axes edited (bit 0 x, 1 y, 2 z); all of them for a reset.
    unsigned axes{0U};
    bool reset{false};
};

// Draws `control` as a row and edits `value` in place. `mixed` shows the
// value as mixed (a selection that disagrees), `mixed_axes` which axes of a
// Vec3 do. Call inside a pushed ID that is unique to the owner.
Edit row(const ControlDef& control, Value& value, bool mixed = false, unsigned mixed_axes = 0U);

// A section heading.
void heading(std::string_view text);

// A number row for a value that is not a registered property.
bool number_row(std::string_view id, std::string_view label, float& value, float fallback, float lo, float hi,
                std::string_view unit = {}, int decimals = 2);
bool toggle_row(std::string_view id, std::string_view label, bool& value, bool fallback);
// A choice by index among `labels`.
bool choice_row(std::string_view id, std::string_view label, int& index, int fallback,
                std::initializer_list<std::string_view> labels);

// Where each row's control was drawn (the first of its parts) and its reset
// arrow, in pixels, for the rows drawn since forget_drawn(): what a test aims at.
struct Drawn final {
    std::string id;
    ImVec4 control;
    ImVec4 reset;
    bool has_reset{false};
};
[[nodiscard]] const std::vector<Drawn>& drawn();
void forget_drawn();

// The width of the label column.
[[nodiscard]] float label_width();

} // namespace ajfx::studio::im
