#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <unordered_map>

// A reusable visual RGB selector for the bespoke UI.
//
// The authored value stays RGB, while the interaction is the conventional HSV
// model: drag in the large field for saturation/value and along the rail for
// hue. The widget owns no document or editor state, so lights, materials,
// effects, and future panels can all consume the same result.
namespace mediaserver::gui {

struct ColorPickerFrame final {
    UiColor color{};
    bool changed{};
    // True while either direct-manipulation surface owns the pointer, including
    // its release frame. Callers can use this to coalesce one drag into one
    // undo gesture.
    bool editing{};
};

class ColorPicker final {
public:
    [[nodiscard]] ColorPickerFrame draw(
        UiContext& ctx, UiId id, const Rect& bounds, UiColor color);

    void clear() noexcept { remembered_hues_.clear(); }

private:
    // RGB cannot encode a hue when saturation is zero. Remember it per widget
    // so an operator can choose hue on gray, then add saturation in the field.
    std::unordered_map<UiId, float> remembered_hues_;
};

} // namespace mediaserver::gui
