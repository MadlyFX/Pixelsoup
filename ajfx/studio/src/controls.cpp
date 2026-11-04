#include "controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ajfx::studio {

Value sanitize(const ControlDef& control, Value value) {
    const auto clamp = [&](float v) {
        return std::isfinite(v) ? std::clamp(v, control.hard_min, control.hard_max) : control.fallback.x;
    };
    switch (control.type) {
    case ControlType::Number:
        return number(clamp(value.x));
    case ControlType::Range: {
        const float low = clamp(value.x);
        return range(low, std::max(low, clamp(value.y)));
    }
    case ControlType::Vec3:
        return vec3(clamp(value.x), clamp(value.y), clamp(value.z));
    case ControlType::Color:
        return color(std::clamp(value.x, 0.0F, 1.0F), std::clamp(value.y, 0.0F, 1.0F),
                     std::clamp(value.z, 0.0F, 1.0F), std::clamp(value.w, 0.0F, 1.0F));
    case ControlType::Choice:
        for (const ChoiceDef& option : control.choices) {
            if (option.id == value.text) {
                return choice(std::move(value.text));
            }
        }
        return control.fallback;
    case ControlType::Toggle:
        return toggle(value.x != 0.0F);
    case ControlType::File:
        return choice(std::move(value.text));
    }
    return value;
}

std::string format_number(float value, int decimals) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(decimals, 0), static_cast<double>(value));
    std::string text = buffer;
    // Trailing zeros say nothing: "1.50" reads "1.5", "2.00" reads "2".
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? std::string{"0"} : text;
}

std::string format_range(const Value& range, int decimals, std::string_view unit) {
    std::string text = format_number(range.x, decimals);
    if (format_number(range.y, decimals) != text) {
        text += " to " + format_number(range.y, decimals);
    }
    if (!unit.empty()) {
        if (unit != "x" && unit != "/s" && unit != "%") {
            text += ' ';
        }
        text += unit;
    }
    return text;
}

std::string file_label(std::string_view path) {
    if (path.empty()) {
        return "no file";
    }
    return std::filesystem::path{std::string{path}}.filename().string();
}

} // namespace ajfx::studio
