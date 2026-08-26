#include <mediaserver/show/switcher_state.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace mediaserver::show {
namespace {

inline constexpr core::u64 max_document_integer =
    static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] bool read_u64(const doc::PropertyMap& properties,
                            std::string_view key,
                            core::u64& value) noexcept {
    const auto found = properties.find(key);
    if (found == properties.end()) {
        return false;
    }
    const auto* const integer = std::get_if<std::int64_t>(&found->second);
    if (integer == nullptr || *integer < 0) {
        return false;
    }
    value = static_cast<core::u64>(*integer);
    return true;
}

[[nodiscard]] std::int64_t document_integer(core::u64 value) noexcept {
    return static_cast<std::int64_t>(std::min(value, max_document_integer));
}

} // namespace

bool valid_switcher_anchor(const SwitcherAnchor& anchor) noexcept {
    return anchor.program_scene != doc::NodeId{} &&
           anchor.rehearsal_scene != doc::NodeId{} &&
           anchor.program_scene != anchor.rehearsal_scene &&
           anchor.transition_from != doc::NodeId{} &&
           anchor.transition_to != doc::NodeId{} &&
           anchor.anchor_control_frame.value() <= max_document_integer &&
           anchor.duration_control_frames <= max_document_integer;
}

SwitcherProgress derive_switcher_progress(const SwitcherAnchor& anchor,
                                           cmd::FrameNumber control_frame) noexcept {
    SwitcherProgress progress{};
    progress.from = anchor.transition_from;
    progress.to = anchor.transition_to;
    progress.duration = anchor.duration_control_frames;
    if (!valid_switcher_anchor(anchor) ||
        anchor.duration_control_frames == 0U) {
        progress.elapsed = progress.duration;
        return progress;
    }
    if (control_frame <= anchor.anchor_control_frame) {
        progress.active = true;
        return progress;
    }
    progress.elapsed = std::min(
        control_frame.value() - anchor.anchor_control_frame.value(),
        anchor.duration_control_frames);
    progress.active = progress.elapsed < progress.duration;
    return progress;
}

bool dissolve_canvases_compatible(scene::Canvas program,
                                  scene::Canvas rehearsal) noexcept {
    return scene::valid(program) && program == rehearsal;
}

doc::Node make_switcher_node(const SwitcherAnchor& anchor) {
    doc::Node node{};
    node.id = switcher_node_id;
    node.type = std::string{switcher_node_type};
    node.properties.emplace(std::string{switcher_property::program_scene},
                            document_integer(anchor.program_scene.value()));
    node.properties.emplace(std::string{switcher_property::rehearsal_scene},
                            document_integer(anchor.rehearsal_scene.value()));
    node.properties.emplace(std::string{switcher_property::transition_from},
                            document_integer(anchor.transition_from.value()));
    node.properties.emplace(std::string{switcher_property::transition_to},
                            document_integer(anchor.transition_to.value()));
    node.properties.emplace(
        std::string{switcher_property::anchor_control_frame},
        document_integer(anchor.anchor_control_frame.value()));
    node.properties.emplace(
        std::string{switcher_property::duration_control_frames},
        document_integer(anchor.duration_control_frames));
    return node;
}

std::optional<SwitcherAnchor> read_switcher(const doc::Root& root) noexcept {
    const doc::Node* const node = root.get(switcher_node_id);
    if (node == nullptr || std::string_view{node->type} != switcher_node_type) {
        return std::nullopt;
    }

    core::u64 program{};
    core::u64 rehearsal{};
    core::u64 from{};
    core::u64 to{};
    core::u64 at{};
    SwitcherAnchor anchor{};
    if (!read_u64(node->properties, switcher_property::program_scene, program) ||
        !read_u64(node->properties, switcher_property::rehearsal_scene,
                  rehearsal) ||
        !read_u64(node->properties, switcher_property::transition_from, from) ||
        !read_u64(node->properties, switcher_property::transition_to, to) ||
        !read_u64(node->properties, switcher_property::anchor_control_frame,
                  at) ||
        !read_u64(node->properties,
                  switcher_property::duration_control_frames,
                  anchor.duration_control_frames)) {
        return std::nullopt;
    }
    anchor.program_scene = doc::NodeId{program};
    anchor.rehearsal_scene = doc::NodeId{rehearsal};
    anchor.transition_from = doc::NodeId{from};
    anchor.transition_to = doc::NodeId{to};
    anchor.anchor_control_frame = cmd::FrameNumber{at};
    if (!valid_switcher_anchor(anchor)) {
        return std::nullopt;
    }
    return anchor;
}

} // namespace mediaserver::show
