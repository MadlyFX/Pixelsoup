#pragma once

// Keyframe command authoring: channel nodes, key edits, retimes and the
// status messages key operations report.

#include "edit_coalescing_policy.hpp"
#include "key_clipboard.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::app {

void append_key_target_report(std::string &message, std::string_view label,
                              std::span<const doc::NodeId> targets);

void append_key_incompatible_report(std::string &message,
                                    std::span<const KeyAddress> incompatible);

[[nodiscard]] std::string
key_operation_message(std::string_view operation, std::size_t accepted,
                      std::string_view accepted_label,
                      std::span<const doc::NodeId> locked,
                      std::span<const KeyAddress> incompatible);

[[nodiscard]] std::vector<KeyAddress>
key_addresses(std::span<const editor::TimelineKeyAddress> addresses);

[[nodiscard]] std::vector<scene::KeyAddress>
scene_key_addresses(std::span<const editor::TimelineKeyAddress> addresses);

[[nodiscard]] std::string
key_batch_message(editor::TimelineKeyBatchAction action,
                  const scene::KeyEditReport &report);

[[nodiscard]] doc::NodeId channel_node_id(doc::NodeId target,
                                          core::u64 component_index) noexcept;

[[nodiscard]] doc::Node make_channel_node(doc::NodeId id, doc::NodeId target,
                                          std::string_view component);

void upsert_channel_key(doc::Node &node, core::u64 frame, double value);

[[nodiscard]] bool erase_channel_key(doc::Node &node, core::u64 frame);

[[nodiscard]] bool retime_channel_key(doc::Node &node, core::i64 from,
                                      core::i64 to);

[[nodiscard]] bool schedule_key_edit(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyIntent &key,
                                     cmd::FrameNumber apply_at,
                                     EditCoalescingPlan *coalescing = nullptr);

[[nodiscard]] bool schedule_key_move(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyMove &move,
                                     cmd::FrameNumber apply_at);

[[nodiscard]] std::string_view
transform_property(scene::TransformChannel component) noexcept;

} // namespace mediaserver::app
