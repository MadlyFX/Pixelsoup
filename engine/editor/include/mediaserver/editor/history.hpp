#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <cstddef>
#include <string>
#include <vector>

// The operator's edit history as the editor sees it: requests to move through
// it, and the Director's description of each step. The editor never reads the
// scheduler; the Director rebuilds this state only when a context's history
// revision changes.
namespace mediaserver::editor {

enum class HistoryAction : core::u8 {
  Undo,
  Redo,
  // Move to just after `HistoryRequest::target_step`, crossing any number of
  // steps as one atomic change.
  Jump,
};

enum class HistoryContext : core::u8 {
  Scene,
  Compositor,
};

// One navigation through the edit history. Transactions are freshly allocated
// from the same gesture counter as ordinary edits, so the commands authored by
// undo, redo or a jump cannot collide with an existing gesture. A jump crosses
// `transaction_count` steps and uses `transaction` up to
// `transaction + transaction_count - 1`, one per step.
struct HistoryRequest final {
  HistoryAction action{HistoryAction::Undo};
  HistoryContext context{HistoryContext::Scene};
  core::u64 transaction{};
  // Jump only. Zero targets the state before every listed step.
  core::u64 target_step{};
  core::u64 transaction_count{1U};

  bool operator==(const HistoryRequest &) const = default;
};

// What a step did to the document, judged by each node's state before and
// after the whole step.
enum class HistoryStepKind : core::u8 {
  Add,
  Change,
  Delete,
  Mixed,
};

struct HistoryStep final {
  // Stable while the step is listed; undo and redo do not change it.
  core::u64 step{};
  // Sentence case, e.g. "Change position of 3 objects".
  std::string label;
  HistoryStepKind kind{HistoryStepKind::Change};
  // At or before the current position. Steps after it are redoable.
  bool applied{};
  // Nodes the step added, changed or deleted, for highlighting.
  std::vector<doc::NodeId> nodes;

  bool operator==(const HistoryStep &) const = default;
};

struct HistoryContextState final {
  // Oldest first. Applied steps form a prefix.
  std::vector<HistoryStep> steps;
  // Older steps no longer listed because the history is bounded.
  std::size_t retired{};
  core::u64 revision{};

  bool operator==(const HistoryContextState &) const = default;
};

struct HistoryState final {
  HistoryContextState scene;
  HistoryContextState compositor;

  [[nodiscard]] const HistoryContextState &
  context(HistoryContext which) const noexcept {
    return which == HistoryContext::Compositor ? compositor : scene;
  }

  bool operator==(const HistoryState &) const = default;
};

} // namespace mediaserver::editor
