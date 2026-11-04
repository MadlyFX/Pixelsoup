#pragma once

#include "document.hpp"

#include <functional>
#include <span>

namespace ajfx::studio {
// One registry drives persistence, inspector, reset defaults and validation.
// The setter addresses a typed owner; there are no compiler string keys here.
struct SceneProperty final {
    std::string_view section;
    ControlDef control;
    std::function<Value(const SceneItem &)> read;
    std::function<void(SceneItem &, const Value &)> write;
};
[[nodiscard]] std::span<const SceneProperty> scene_properties(const SceneItem &item);
[[nodiscard]] std::string validate_scene_properties(const SceneItem &item);
} // namespace ajfx::studio
