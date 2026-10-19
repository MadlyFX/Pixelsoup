#pragma once

#include "edit_coalescing_policy.hpp"

#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <string>
#include <string_view>

namespace mediaserver::app {

// The texture-preparation worker publishes an immutable package; the Director
// changes only the standalone model's source reference, with the previous node
// as its undo inverse. This is also the shipping GenericEdit source adapter.
[[nodiscard]] inline bool schedule_model_source_edit(
    cmd::DirectorScheduler& scheduler, doc::NodeId node, std::string_view source,
    cmd::TransactionId transaction, cmd::FrameNumber apply_at,
    EditCoalescingPlan* coalescing = nullptr) {
    const auto* existing = scheduler.projected_document().get(node);
    if (existing == nullptr || existing->type != scene::schema::model_type ||
        scene::is_linked_occurrence(*existing) || source.empty() || source.size() > 1024U ||
        source.find('\0') != std::string_view::npos) return true;
    const auto lock = existing->properties.find(scene::schema::locked);
    if (lock != existing->properties.end()) {
        if (const auto* locked = std::get_if<bool>(&lock->second); locked && *locked) return true;
    }
    auto updated = *existing;
    updated.properties.insert_or_assign(std::string{scene::schema::source}, std::string{source});
    if (updated == *existing) return true;
    return static_cast<bool>(coalescing
        ? schedule_editor_coalesced_edit(scheduler, cmd::PutNode{std::move(updated)},
            cmd::PutNode{*existing}, *coalescing, apply_at)
        : scheduler.schedule_edit(cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
            cmd::UndoContextId{2U}, transaction, apply_at));
}

} // namespace mediaserver::app
