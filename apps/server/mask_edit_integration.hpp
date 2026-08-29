#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/scene/mask_authoring.hpp>

#include <optional>

namespace mediaserver::app {

// One editor mask action projected into a complete replacement document. The
// Director schedules this root once, with MaskEditIntent::transaction, so a
// multi-point Stage translation remains one replicated undo step.
struct MaskEditIntegrationPlan final {
    doc::Root document;
    core::u64 next_node{};
    doc::NodeId mask{};
    std::optional<doc::NodeId> point;
};

using MaskEditIntegrationResult = core::Result<
    MaskEditIntegrationPlan, scene::MaskAuthoringError>;

[[nodiscard]] MaskEditIntegrationResult plan_mask_edit_integration(
    const doc::Root& document, const editor::MaskEditIntent& edit,
    core::u64 next_node);

} // namespace mediaserver::app
