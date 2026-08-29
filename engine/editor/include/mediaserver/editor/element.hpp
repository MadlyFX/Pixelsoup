#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <string>
#include <string_view>
#include <optional>

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
    Group,
    // A timeline track and the screen it owns. Selectable and inspectable like
    // any drawable because that is what it is: a plane in the scene, with the
    // clips filed under it playing on it.
    Track,
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
    case ElementKind::Group:
        return "group";
    case ElementKind::Track:
        return "track";
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
    case ElementKind::Group:
        return gui::UiColor{0.745F, 0.565F, 0.941F, 1.0F};
    case ElementKind::Track:
        return gui::UiColor{0.906F, 0.596F, 0.400F, 1.0F};
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
    bool locked{};
    scene::Vec3 position{};
    scene::Vec3 rotation{}; // yaw, pitch, roll (turns)
    scene::Vec3 scale{1.0F, 1.0F, 1.0F};
    scene::Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    scene::Vec3 direction{0.0F, -1.0F, 0.0F};
    float range{10.0F};
    // Drawable-only authored values. Lights keep their defaults and the
    // Inspector omits these sections for them.
    scene::Material material{};
    scene::LayerStyle style{};
    std::string source;
    core::u32 sequence_start{};
    core::u32 sequence_count{1U};
    // Clip-only native source cadence. Timeline filmstrips use the same exact
    // rational show-to-source mapping as trim/slip authoring; an absent rate
    // retains the image-sequence one-source-frame-per-show-frame contract.
    std::optional<scene::MediaFrameRate> source_frame_rate;
    // Clip-only: where the placement sits on the show timeline. An unplaced
    // clip (zero duration) is the legacy always-on form and the Timeline draws
    // no bar for it. Other kinds leave this default.
    scene::ClipTiming timing{};
    // Clip-only authored lane membership and deterministic overlap order.
    // Node id zero denotes the implicit lane and is never authored.
    doc::NodeId timeline_lane{};
    core::i64 clip_order{};
    // False for a clip the current show frame does not include. It is still a
    // real, editable placement - the Timeline needs its row and its bar - so it
    // is marked rather than omitted.
    bool on_air{true};
    // Authored hierarchy metadata. Groups carry their persisted display name;
    // drawables carry the same parent/order pair so the Scene panel can expose
    // a newly authored hierarchy without reaching back into the document.
    std::string display_name;
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};
    bool visible{true};
    bool effective_visible{true};
    // Track-only. The screen's authored format, and whether its plane's height
    // follows it. `scale` above is already the locked extent, so a panel can
    // show it without re-deriving anything.
    core::u32 raster_width{scene::default_raster_width};
    core::u32 raster_height{scene::default_raster_height};
    bool aspect_locked{true};

    // Clip-only: the name of the track whose screen it is drawn on, so a
    // panel can say which screen without holding the track list too.
    std::string screen_name;

    // A clip drawn on a track's screen rather than on its own transform. The
    // clip's transform properties still exist and are still what it returns
    // to when it leaves the track; they are simply not what it renders with.
    [[nodiscard]] bool on_screen() const noexcept {
        return kind == ElementKind::Media && timeline_lane != doc::NodeId{};
    }

    bool operator==(const ElementInfo&) const = default;
};

// The value one transform component holds in an element's authored transform -
// what a keyframe on that component captures, and what a field shows.
[[nodiscard]] constexpr double component_value(
    const ElementInfo& element, scene::TransformChannel component) noexcept {
    switch (component) {
    case scene::TransformChannel::PositionX:
        return element.position.x;
    case scene::TransformChannel::PositionY:
        return element.position.y;
    case scene::TransformChannel::PositionZ:
        return element.position.z;
    case scene::TransformChannel::RotationYaw:
        return element.rotation.x;
    case scene::TransformChannel::RotationPitch:
        return element.rotation.y;
    case scene::TransformChannel::RotationRoll:
        return element.rotation.z;
    case scene::TransformChannel::ScaleX:
        return element.scale.x;
    case scene::TransformChannel::ScaleY:
        return element.scale.y;
    case scene::TransformChannel::ScaleZ:
        return element.scale.z;
    case scene::PropertyChannel::Opacity:
        return element.style.opacity;
    case scene::PropertyChannel::Brightness:
        return element.style.effects.brightness;
    case scene::PropertyChannel::Saturation:
        return element.style.effects.saturation;
    case scene::PropertyChannel::BlurRadius:
        return element.style.effects.blur_radius;
    case scene::PropertyChannel::FilmGrainAmount:
    case scene::PropertyChannel::BoxBlurRadius:
    case scene::PropertyChannel::DirectionalBlurLength:
    case scene::PropertyChannel::ChromaKeyMix:
    case scene::PropertyChannel::LumaKeyMix:
    case scene::TransformChannel::Count:
        break;
    }
    return 0.0;
}

} // namespace mediaserver::editor
