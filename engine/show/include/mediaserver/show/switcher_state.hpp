#pragma once

#include <optional>
#include <string_view>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/canvas.hpp>

namespace mediaserver::show {

// Program/rehearsal selection is replicated beside transport. Transition
// progress is derived from the ordered control-frame stream, so a dissolve
// continues while the content timeline is paused and every render node mixes
// the same two scenes by the same amount.
inline constexpr doc::NodeId switcher_node_id{2U};
inline constexpr std::string_view switcher_node_type = "show.switcher";

namespace switcher_property {
inline constexpr std::string_view program_scene = "program_scene";
inline constexpr std::string_view rehearsal_scene = "rehearsal_scene";
inline constexpr std::string_view transition_from = "transition_from";
inline constexpr std::string_view transition_to = "transition_to";
inline constexpr std::string_view anchor_control_frame =
    "anchor_control_frame";
inline constexpr std::string_view duration_control_frames =
    "duration_control_frames";
} // namespace switcher_property

struct SwitcherAnchor final {
    doc::NodeId program_scene{};
    doc::NodeId rehearsal_scene{};
    doc::NodeId transition_from{};
    doc::NodeId transition_to{};
    cmd::FrameNumber anchor_control_frame{};
    core::u64 duration_control_frames{};

    bool operator==(const SwitcherAnchor&) const = default;
};

struct SwitcherProgress final {
    doc::NodeId from{};
    doc::NodeId to{};
    core::u64 elapsed{};
    core::u64 duration{};
    bool active{};

    [[nodiscard]] double mix() const noexcept {
        if (duration == 0U) {
            return 1.0;
        }
        return static_cast<double>(elapsed) / static_cast<double>(duration);
    }

    bool operator==(const SwitcherProgress&) const = default;
};

[[nodiscard]] bool valid_switcher_anchor(
    const SwitcherAnchor& anchor) noexcept;

[[nodiscard]] SwitcherProgress derive_switcher_progress(
    const SwitcherAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept;

// A dissolve renders both inputs through one shared output slice. It is only
// defined when Program and Rehearsal describe the same valid canvas. Cuts do
// not need this check because they never render both scenes together.
[[nodiscard]] bool dissolve_canvases_compatible(
    scene::Canvas program,
    scene::Canvas rehearsal) noexcept;

[[nodiscard]] doc::Node make_switcher_node(const SwitcherAnchor& anchor);
[[nodiscard]] std::optional<SwitcherAnchor> read_switcher(
    const doc::Root& root) noexcept;

} // namespace mediaserver::show
