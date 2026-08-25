#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/scene/math.hpp>

#include <string_view>

// What a scene element looks like to the editor's panels.
//
// This is the vocabulary the Inspector, the Timeline, and the shell all share:
// a node, what kind of thing it is, and its *authored* values - the base
// properties as the document holds them, before any keyframe channel overrides
// them. The Director reads them out of the document once a frame and hands them
// down; nothing here reaches back.
//
// It sits in its own header rather than in the shell's because a panel that had
// to include the shell to name its own input would be upside down: the panels
// are the parts that get built and tested on their own, and the shell is what
// composes them.
namespace mediaserver::editor {

enum class ElementKind : core::u8 {
    Object,
    Media,
    Model,
    DirectionalLight,
    PointLight,
};

// The word an operator sees for a kind - what the Inspector titles a selection
// with and what the Timeline names a layer.
[[nodiscard]] constexpr std::string_view kind_name(ElementKind kind) noexcept {
    switch (kind) {
    case ElementKind::Media:
        return "clip";
    case ElementKind::Model:
        return "model";
    case ElementKind::DirectionalLight:
        return "sun";
    case ElementKind::PointLight:
        return "point light";
    case ElementKind::Object:
        break;
    }
    return "object";
}

// The swatch a kind is marked with, in the layer list and beside the Inspector's
// title. One hue per kind, held back far enough that a screen full of them still
// reads as chrome rather than as content.
[[nodiscard]] constexpr gui::UiColor kind_color(ElementKind kind) noexcept {
    switch (kind) {
    case ElementKind::Media:
        return gui::UiColor{0.376F, 0.647F, 0.980F, 1.0F};
    case ElementKind::Model:
        return gui::UiColor{0.549F, 0.824F, 0.588F, 1.0F};
    case ElementKind::DirectionalLight:
    case ElementKind::PointLight:
        return gui::UiColor{0.961F, 0.804F, 0.353F, 1.0F};
    case ElementKind::Object:
        break;
    }
    return gui::UiColor{0.627F, 0.627F, 0.675F, 1.0F};
}

[[nodiscard]] constexpr bool is_light(ElementKind kind) noexcept {
    return kind == ElementKind::DirectionalLight
        || kind == ElementKind::PointLight;
}

// A scene element the Inspector shows and edits, with its authored values (the
// base, before channels) read from the document by the Director. Objects and
// clips use the transform; lights use the light fields.
struct ElementInfo final {
    doc::NodeId node{};
    ElementKind kind{ElementKind::Object};
    scene::Vec3 position{};
    scene::Vec3 rotation{}; // yaw, pitch, roll (turns)
    scene::Vec3 scale{1.0F, 1.0F, 1.0F};
    scene::Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    scene::Vec3 direction{0.0F, -1.0F, 0.0F};
    float range{10.0F};

    bool operator==(const ElementInfo&) const = default;
};

} // namespace mediaserver::editor
