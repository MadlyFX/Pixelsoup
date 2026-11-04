#include "im_controls.hpp"

#include <imgui_internal.h>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace ajfx::studio::im {
namespace {

// "%.2f" with the unit after it; a percent sign in a unit is doubled.
std::string number_format(const ControlDef& control) {
    std::string format = "%." + std::to_string(std::clamp(control.decimals, 0, 6)) + "f";
    for (const char c : control.unit) {
        format += c;
        if (c == '%') {
            format += '%';
        }
    }
    if (!control.unit.empty()) {
        format.insert(format.find('f') + 1U, " ");
    }
    return format;
}

float drag_speed(const ControlDef& control) {
    if (control.decimals == 0) {
        return std::max(1.0F, (control.max - control.min) / 400.0F);
    }
    return std::clamp((control.max - control.min) / 400.0F, 0.001F, 0.1F);
}

// The limits DragFloat holds a typed or dragged value to; none when they are
// the unlimited defaults.
void limits(const ControlDef& control, float& lo, float& hi) {
    const bool bounded = control.hard_min > -1.0e8F && control.hard_max < 1.0e8F;
    lo = bounded ? control.hard_min : 0.0F;
    hi = bounded ? control.hard_max : 0.0F;
}

bool drag(const char* id, float& value, const ControlDef& control, bool mixed, const std::string& format) {
    float lo = 0.0F;
    float hi = 0.0F;
    limits(control, lo, hi);
    if (mixed) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    const bool changed =
        ImGui::DragFloat(id, &value, drag_speed(control), lo, hi, format.c_str(), ImGuiSliderFlags_AlwaysClamp);
    if (mixed) {
        ImGui::PopItemFlag();
    }
    return changed;
}

} // namespace

namespace {
std::vector<Drawn>& drawn_rows() {
    static std::vector<Drawn> rows;
    return rows;
}
ImVec4 item_rect() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return ImVec4{min.x, min.y, max.x, max.y};
}
} // namespace

const std::vector<Drawn>& drawn() { return drawn_rows(); }
void forget_drawn() { drawn_rows().clear(); }

float label_width() {
    return std::max(ImGui::GetContentRegionAvail().x * 0.38F, ImGui::GetFontSize() * 5.0F);
}

void heading(std::string_view text) {
    ImGui::Spacing();
    ImGui::SeparatorText(std::string{text}.c_str());
}

Edit row(const ControlDef& control, Value& value, bool mixed, unsigned mixed_axes) {
    Edit edit;
    ImGui::PushID(std::string{control.id}.c_str());
    const float start = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    const float reset_w = ImGui::GetFrameHeight();
    const float label_w = label_width();
    const std::string label = std::string{control.label} + (mixed ? " *" : "");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label.c_str());
    if (ImGui::IsItemHovered() && (mixed || !control.help.empty())) {
        ImGui::SetTooltip("%s", mixed ? "Mixed values. Editing applies this value to the selection."
                                      : std::string{control.help}.c_str());
    }
    ImGui::SameLine(start + label_w);
    const float box = std::max(width - label_w - reset_w - ImGui::GetStyle().ItemSpacing.x, 20.0F);
    const std::string format = number_format(control);
    Drawn record;
    record.id = std::string{control.id};
    bool first_part = true;
    const auto note_first = [&] {
        if (first_part) {
            record.control = item_rect();
            first_part = false;
        }
    };

    switch (control.type) {
    case ControlType::Number: {
        ImGui::SetNextItemWidth(box);
        float v = value.x;
        if (drag("##v", v, control, mixed, format)) {
            value.x = v;
            edit.changed = true;
        }
        note_first();
        break;
    }
    case ControlType::Vec3: {
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const float each = (box - gap * 2.0F) / 3.0F;
        float* parts[] = {&value.x, &value.y, &value.z};
        const char* axes[] = {"##x", "##y", "##z"};
        const char* prefixes[] = {"X ", "Y ", "Z "};
        for (unsigned k = 0U; k < 3U; ++k) {
            if (k != 0U) {
                ImGui::SameLine(0.0F, gap);
            }
            ImGui::SetNextItemWidth(each);
            const std::string axis_format = prefixes[k] + format;
            if (drag(axes[k], *parts[k], control, mixed && (mixed_axes & (1U << k)) != 0U, axis_format)) {
                edit.axes |= 1U << k;
                edit.changed = true;
            }
            note_first();
        }
        break;
    }
    case ControlType::Range: {
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const float each = (box - gap) * 0.5F;
        float* parts[] = {&value.x, &value.y};
        const char* ids[] = {"##low", "##high"};
        for (unsigned k = 0U; k < 2U; ++k) {
            if (k != 0U) {
                ImGui::SameLine(0.0F, gap);
            }
            ImGui::SetNextItemWidth(each);
            if (drag(ids[k], *parts[k], control, mixed, format)) {
                edit.changed = true;
            }
            note_first();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Each particle picks its own, between the two.");
        }
        break;
    }
    case ControlType::Color: {
        float rgba[4] = {value.x, value.y, value.z, value.w};
        ImGui::SetNextItemWidth(box);
        if (ImGui::ColorEdit4("##c", rgba,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_HDR |
                                  ImGuiColorEditFlags_Float)) {
            value = color(rgba[0], rgba[1], rgba[2], rgba[3]);
            edit.changed = true;
        }
        note_first();
        break;
    }
    case ControlType::Toggle: {
        bool on = value.x != 0.0F;
        if (mixed) {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        if (ImGui::Checkbox("##t", &on)) {
            value = toggle(on);
            edit.changed = true;
        }
        note_first();
        if (mixed) {
            ImGui::PopItemFlag();
        }
        break;
    }
    case ControlType::Choice: {
        std::vector<const char*> labels;
        std::vector<std::string> owned;
        owned.reserve(control.choices.size());
        for (const ChoiceDef& c : control.choices) {
            owned.emplace_back(c.label);
        }
        for (const std::string& s : owned) {
            labels.push_back(s.c_str());
        }
        int at = mixed ? -1 : static_cast<int>(value.x);
        ImGui::SetNextItemWidth(box);
        const char* shown = at >= 0 && at < static_cast<int>(labels.size()) ? labels[static_cast<std::size_t>(at)] : "-";
        if (ImGui::BeginCombo("##choice", shown)) {
            for (std::size_t k = 0U; k < labels.size(); ++k) {
                if (ImGui::Selectable(labels[k], static_cast<int>(k) == at)) {
                    value = number(static_cast<float>(k));
                    edit.changed = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case ControlType::File: {
        const std::string name = std::filesystem::path{value.text}.filename().string();
        ImGui::SetNextItemWidth(box);
        if (ImGui::Button((name.empty() ? std::string{"Choose..."} : name).c_str(), ImVec2{box, 0.0F})) {
            const char* filters[] = {"*.obj", "*.gltf", "*.glb"};
            if (const char* path = tinyfd_openFileDialog("Choose a model", "", 3, filters, "3D models", 0)) {
                value.text = std::filesystem::path{path}.generic_string();
                edit.changed = true;
            }
        }
        break;
    }
    }

    if (mixed || value != control.fallback) {
        ImGui::SameLine();
        const bool pressed = ImGui::Button("\xE2\x86\x90", ImVec2{reset_w, 0.0F});
        record.reset = item_rect();
        record.has_reset = true;
        if (pressed) {
            value = control.fallback;
            edit.axes = 7U;
            edit.reset = true;
            edit.changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset to default");
        }
    }
    ImGui::PopID();
    drawn_rows().push_back(record);
    if (edit.changed && control.type != ControlType::Vec3) {
        edit.axes = 7U;
    }
    return edit;
}

bool number_row(std::string_view id, std::string_view label, float& value, float fallback, float lo, float hi,
                std::string_view unit, int decimals) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.fallback = number(fallback);
    control.min = control.hard_min = lo;
    control.max = control.hard_max = hi;
    control.unit = unit;
    control.decimals = decimals;
    Value v = number(value);
    const Edit edit = row(control, v);
    if (edit.changed) {
        value = v.x;
    }
    return edit.changed;
}

bool toggle_row(std::string_view id, std::string_view label, bool& value, bool fallback) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Toggle;
    control.fallback = toggle(fallback);
    Value v = toggle(value);
    const Edit edit = row(control, v);
    if (edit.changed) {
        value = v.x != 0.0F;
    }
    return edit.changed;
}

bool choice_row(std::string_view id, std::string_view label, int& index, int fallback,
                std::initializer_list<std::string_view> labels) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Choice;
    control.fallback = number(static_cast<float>(fallback));
    for (const std::string_view l : labels) {
        control.choices.push_back({l, l});
    }
    Value v = number(static_cast<float>(index));
    const Edit edit = row(control, v);
    if (edit.changed) {
        index = static_cast<int>(v.x);
    }
    return edit.changed;
}

} // namespace ajfx::studio::im
