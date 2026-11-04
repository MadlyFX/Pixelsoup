#pragma once

#include "document.hpp"
#include "package_builder.hpp"

#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <ajfx/actions.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The preview: the effect on a small stage, drawn by the media server's own
// renderer - the Advanced scene renderer, its particle pass, bloom and the
// display transform - so what an artist sees is what the wall will show.
namespace ajfx::studio {

namespace ms = mediaserver;

// The preview camera's vertical field of view, in turns (about 40 degrees).
inline constexpr float preview_field_of_view = 0.11F;

// The instance seed the renderer gives the preview's placement, so a CPU run
// with it plays the very particles the preview draws.
[[nodiscard]] std::uint32_t preview_instance_seed();

// Where the camera looks from, orbiting a point.
struct Orbit final {
    float yaw{0.1F};    // turns about +Y
    float pitch{0.06F}; // turns above the horizon
    float distance{4.5F};
    ms::scene::Vec3 target{0.0F, 0.9F, 0.0F};
};

// A view that fits `bounds` (or a person-sized default without them).
[[nodiscard]] Orbit frame_bounds(const Orbit& from, const Bounds& bounds, float aspect);

struct PreviewFrame final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    Backdrop backdrop{Backdrop::Night};
    float glow{0.6F};
    bool figure{true};
    // Scene workspaces draw their authored Ground; legacy pages use the stage.
    bool staging_floor{true};
    Orbit orbit{};
    // The package to play, empty for none, and its generation: a new
    // generation reads the file again.
    std::string package;
    std::uint64_t package_generation{0U};
    // Show frame at 60 frames a second, one simulation step each.
    std::uint64_t frame{0U};
    // Changes when the page does, so temporal history restarts on an edit.
    std::uint64_t revision{0U};
    // A paused preview holds its show frame, and a held frame repeats its
    // last resolve; moving the camera then has to restart that history.
    bool paused{false};
    // Values for the effect's published parameters, as a placement's own;
    // anything left out takes the package's default.
    std::vector<ms::scene::ParticleParameter> parameters;
    // What the artist did to the running effect (the scene's performance),
    // and the first tick at which it differs from the last frame's.
    std::shared_ptr<const ajfx::ActionProvider> actions;
    std::uint64_t actions_from{0U};
};

// Steps the preview's rigid physics runs ahead while it is acted on
// (ParticleSettings::acting_rigid_lead). It plays one step a frame, so two
// keep each step ready; an act stamped at acting_tick of the frame's step
// lands just past them.
inline constexpr std::uint32_t preview_acting_lead = 2U;

struct PreviewStatus final {
    bool drawing{false};
    // Loading the package, or catching up with the playhead.
    bool waiting{false};
    // The picture returned is the last complete one, held while this frame
    // waits or fails: a moment's catching up never shows as a gap.
    bool held{false};
    std::string issue;
};

class Preview final {
public:
    [[nodiscard]] static std::unique_ptr<Preview> create(ms::rhi::Device& device, std::string& error);

    // Records the frame into `commands` and returns the picture to show, or
    // null when nothing could be drawn (the status says why). A frame that
    // waits on the effect, or fails, briefly returns the last complete
    // picture instead, as a wall holds its last frame.
    [[nodiscard]] const ms::rhi::Texture* render(ms::rhi::CommandList& commands, const PreviewFrame& frame);
    [[nodiscard]] const PreviewStatus& status() const noexcept { return status_; }

private:
    Preview(ms::rhi::Device& device, ms::render::CompositorSceneExecutor executor, ms::render::OutputTransform display);

    [[nodiscard]] ms::scene::Scene scene_for(const PreviewFrame& frame) const;

    ms::rhi::Device* device_;
    ms::render::CompositorSceneExecutor executor_;
    ms::render::OutputTransform display_;
    PreviewStatus status_;
    // The display target holds a complete picture of this size, and how many
    // frames in a row it has been shown in place of an incomplete one.
    bool complete_shown_{false};
    std::uint32_t held_frames_{0U};
};

} // namespace ajfx::studio
