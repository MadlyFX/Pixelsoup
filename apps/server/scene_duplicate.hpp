#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>

#include <optional>
#include <span>
#include <vector>

namespace mediaserver::app {

struct SceneDuplicatePlan final {
    doc::Root document;
    // The copies of the selected elements, in the order they were selected.
    std::vector<doc::NodeId> created;
    core::u64 next_node{};
};

// Copies each unlocked selected scene element - an object, clip, model,
// light or camera - exactly where it is: same scene, parent group, transform
// and timing. Its effects, masks and their points, and the animation
// channels on any of them come along under fresh ids with their references
// rewritten. Groups and Blender-linked occurrences are left out: a group's
// copy would need its children, and a linked object belongs to Blender.
// Empty when nothing selected can be copied.
[[nodiscard]] std::optional<SceneDuplicatePlan>
duplicate_in_place(const doc::Root& document,
                   std::span<const doc::NodeId> selection,
                   core::u64 next_node);

} // namespace mediaserver::app
