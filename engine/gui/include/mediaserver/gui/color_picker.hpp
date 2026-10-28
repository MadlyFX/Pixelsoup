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
//
// Most callers want UiContext::color_well(), which shows the colour as a
// swatch and opens this picker - with hex and RGB entry, the colour before
// the edit, and recent colours - in a popover under it. ColorPicker is the
// same field and rail laid straight into a panel.
namespace mediaserver::gui {

struct Hsv final {
    float hue{};
    float saturation{};
    float value{};
};

[[nodiscard]] Hsv rgb_to_hsv(UiColor color) noexcept;
[[nodiscard]] UiColor hsv_to_rgb(Hsv value, float alpha) noexcept;

// The saturation/value field over the hue rail, filling `bounds`: the rail
// is the bottom 14 units, 6 below the field. Dragging either writes `hsv`.
// `ground` is the colour behind the widget, which rounds off the field's and
// the rail's corners. Returns whether either surface holds the pointer.
bool hsv_fields(UiContext& ctx, UiId id, const Rect& bounds, Hsv& hsv,
                UiColor ground);

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
