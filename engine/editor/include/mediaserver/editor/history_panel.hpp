#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/history.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The History strip: the operator's edits as a row of steps, oldest first,
// with a marker after the current one. Clicking a step moves the document to
// just after it - every step between undone or redone as one change - and
// clicking Start goes back before them all. It shows the scene or the
// compositor history, following whichever the operator last edited unless a
// context is pinned.
//
// Like every editor panel it authors nothing: a click becomes a
// HistoryRequest the Director turns into ordinary undo/redo commands.
namespace mediaserver::editor {

// Where clicking a listed step, or Start (nullopt), would move the history:
// the step to land after (zero for Start) and how many steps that crosses.
// Zero steps means the click is on the current position.
struct HistoryJump final {
  core::u64 target_step{};
  core::u64 steps{};

  bool operator==(const HistoryJump &) const = default;
};
[[nodiscard]] HistoryJump history_jump(const HistoryContextState &state,
                                       std::optional<std::size_t> index) noexcept;

// The chevron that hides the strip (pointing down, `shown`) or brings it
// back (pointing up). With a `label` it is a chip, as the status bar shows
// it while the strip is hidden. Returns true on the frame it is clicked.
[[nodiscard]] bool history_toggle(gui::UiContext &ctx, gui::UiId id,
                                  const gui::Rect &bounds, bool shown,
                                  std::string_view label = {});

struct HistoryPanelFrame final {
  // Undo, Redo or Jump in the context the strip shows. The shell supplies
  // the transactions.
  std::optional<HistoryRequest> request;
  // The chevron at the strip's left end was clicked.
  bool hide{};
  // The nodes the step under the pointer touched, for the Stage to outline.
  std::vector<doc::NodeId> hovered_nodes;
};

class HistoryPanel final {
public:
  explicit HistoryPanel(gui::UiId id) noexcept : id_(id) {}

  // `active` is the context the operator last edited in. `on_air` means the
  // scene being edited is the one on the wall, so a multi-step jump would
  // change the show in one go and asks for a second click.
  [[nodiscard]] HistoryPanelFrame draw(gui::UiContext &ctx,
                                       const gui::Rect &bounds,
                                       const HistoryState &state,
                                       HistoryContext active, bool on_air);

  // The context the strip is showing.
  [[nodiscard]] HistoryContext shown(HistoryContext active) const noexcept {
    return pinned_.value_or(active);
  }

  // A project switch: the steps are gone, so nothing stays armed or pinned.
  void reset() noexcept;

private:
  gui::UiId id_{};
  std::optional<HistoryContext> pinned_;
  float scroll_{};
  float content_width_{};
  // Where along the scroll bar's thumb the pointer took hold of it.
  float thumb_grab_{};
  // The position the strip last scrolled into view, so it follows new edits
  // without fighting an operator scrolling back through old ones.
  core::u64 followed_revision_{};
  HistoryContext followed_context_{HistoryContext::Scene};
  // A multi-step jump on air waiting for its second click.
  std::optional<HistoryJump> armed_;
  HistoryContext armed_context_{HistoryContext::Scene};
  double armed_until_{-1.0};
};

} // namespace mediaserver::editor
