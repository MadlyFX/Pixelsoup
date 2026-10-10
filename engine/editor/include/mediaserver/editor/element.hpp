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
    // A camera on the stage. Selectable so an operator can find the one they
    // just added, place it, and bind a tracker feed to it; it draws nothing
    // itself.
    //
    // A feed is deliberately *not* a kind here. A tracker has no place on the
    // stage and nothing to select - it is a socket and a lens - so it is
    // project state, authored in Project Settings and named by the camera that
    // uses it, the way a camera names a lens rather than the lens being an
    // object in the scene.
    Camera,
    Environment,
};

// The word an operator sees for a kind - what the Inspector titles a selection
// with and what the Timeline names a layer.
// Names an element the way an operator asked for it.
//
// A spot and a point are the same `ElementKind` because they are the same
// document node, so naming from the enum alone calls every spot a point light
// - which is what an operator sees a second after adding one from a menu entry
// that said Spot Light.
[[nodiscard]] constexpr std::string_view kind_name(ElementKind kind) noexcept {
    switch (kind) {
    case ElementKind::Media:
        return "clip";
    case ElementKind::Model:
        return "model";
    case ElementKind::DirectionalLight:
        return "sun";
    case ElementKind::PointLight:
        // The local-light default. A spot is the same element kind with a
        // different authored `kind`, so the overload below is what names one.
        return "point light";
    case ElementKind::Group:
        return "group";
    case ElementKind::Track:
        return "track";
    case ElementKind::Camera:
        return "camera";
    case ElementKind::Environment:
        return "environment";
    case ElementKind::Object:
        break;
    }
    return "object";
}

// Neutral object marks keep attention on selection and the rendered content.
[[nodiscard]] constexpr gui::UiColor kind_color(ElementKind) noexcept {
    return gui::UiColor{0.48F, 0.53F, 0.58F, 1.0F};
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
    // Which flavour of local light this is, and the cone if it is a spot.
    // Carried on the element rather than inferred from the node type, because
    // point and spot are the same node type with a different `kind`.
    scene::LightUnit light_unit{scene::LightUnit::Unitless};
    float color_temperature_kelvin{6500.0F};
    scene::Turns angular_radius{};
    std::string ies_source;
    scene::Turns ies_rotation{};
    float light_width{1}, light_height{1}, light_radius{0.5F};
    scene::Turns area_rotation{};
    bool two_sided{};
    bool use_color_temperature{};
    scene::LocalLightKind local_kind{scene::LocalLightKind::Point};
    scene::Turns inner_cone{0.0625F};
    scene::Turns outer_cone{0.125F};
    bool bake_static{true};
    scene::ModelAnimation animation{};
    bool linked_animation{};
    bool bake_indirect{true};
    bool casts_shadow{};
    scene::ShadowTier shadow_tier{scene::ShadowTier::Medium};
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
    bool linked{}, linked_held{};
    core::u32 linked_batch_instances{};
    doc::NodeId linked_instance{};
    scene::Vec3 linked_world{}, linked_alignment{};
    bool linked_collection{}, linked_paused{}, linked_placement_override{}, linked_visibility_override{};
    bool linked_listening{}, linked_connected{}, linked_pending{}, linked_failed{};
    doc::NodeId linked_parent{};
    core::u32 linked_material_overrides{};
    std::string linked_source_uuid, linked_revision, linked_status;
    // Track-only. The screen's authored format, and whether its plane's height
    // follows it. `scale` above is already the locked extent, so a panel can
    // show it without re-deriving anything.
    core::u32 raster_width{scene::default_raster_width};
    core::u32 raster_height{scene::default_raster_height};
    bool aspect_locked{true};

    // Camera-only: where it stands and what it looks at. A camera is aimed
    // rather than rotated, so it carries a target instead of the Euler
    // rotation every other element uses, and the Inspector shows it that way.
    scene::Vec3 target{0.0F, 0.0F, -1.0F};
    float field_of_view{0.125F};
    float shutter_seconds{1.0F / 48.0F};
    float aperture_f_number{2.8F};
    float focus_distance_m{};
    // Camera-only: the tracker feed bound to it, or zero for a camera that
    // stays exactly where it was authored. Binding one is what makes a camera
    // the tracked camera, so this single field is the whole distinction.
    doc::NodeId tracking_source{};
    // Camera-only: how it draws its plate, as authored.
    scene::IcvfxCameraSettings icvfx{};

    // Clip-only: the name of the track whose screen it is drawn on, so a
    // panel can say which screen without holding the track list too.
    std::string screen_name;
    scene::Environment environment{};

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
    case scene::PropertyChannel::EnvironmentIntensity:
    case scene::PropertyChannel::EnvironmentRotation:
    case scene::PropertyChannel::FogDensity:
    case scene::PropertyChannel::FogHeight:
    case scene::PropertyChannel::FogFalloff:
    case scene::PropertyChannel::FogAnisotropy:
    case scene::PropertyChannel::SkyHaze:
    case scene::PropertyChannel::SkyIlluminance:
    case scene::PropertyChannel::SkyTimeOfDay:
    case scene::PropertyChannel::LumaKeyMix:
    case scene::TransformChannel::Count:
        break;
    }
    return 0.0;
}

inline std::string_view kind_name(const ElementInfo& element) noexcept {
    if (element.kind == ElementKind::PointLight
        && element.local_kind == scene::LocalLightKind::Spot) {
        return "spot light";
    }
    if (element.kind == ElementKind::PointLight && element.local_kind == scene::LocalLightKind::Rectangle) return "rectangle light";
    if (element.kind == ElementKind::PointLight && element.local_kind == scene::LocalLightKind::Sphere) return "sphere light";
    return kind_name(element.kind);
}

} // namespace mediaserver::editor
