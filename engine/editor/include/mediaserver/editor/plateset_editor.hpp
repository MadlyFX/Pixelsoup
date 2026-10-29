#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/library_edit.hpp>
#include <mediaserver/assets/plateset.hpp>
#include <mediaserver/editor/library_panel.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/plateset_analysis.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The Asset Viewer's page for a vehicle process plateset: the rig seen from
// above with a tile per slot, and what stands between the set and the stage.
// Like every leaf panel it reports what the operator asked for and leaves the
// document to the Director.
namespace mediaserver::editor {

struct PlatesetEditorInput final {
  const assets::PlatesetAsset *plateset{};
  const assets::AssetRegistry *assets{};
  std::span<const LibraryThumbnailObservation> thumbnails;
  // Library assets are being dragged over the editor right now.
  bool dragging_assets{};
  const PlatesetAnalysisView *analysis{};
};


struct PlatesetEditorFrame final {
  std::vector<assets::LibraryEdit> edits;
  std::vector<LibraryProxyPolicyIntent> proxy_edits;
  // Start (or restart) one of the analyses of the open set.
  std::optional<PlatesetAnalysisPathway> analyse;
  // Thumbnails the slot tiles want kept and made.
  std::vector<assets::DerivativeKey> thumbnails;
  // A one-line answer to the last action, such as why a sync was refused.
  std::optional<std::string> message;

  bool operator==(const PlatesetEditorFrame &) const = default;
};

class PlatesetEditor final {
public:
  explicit PlatesetEditor(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] PlatesetEditorFrame draw(gui::UiContext &ctx,
                                         const gui::Rect &bounds,
                                         const PlatesetEditorInput &input);

  // The slot under a screen point as last drawn, for a Library drop.
  [[nodiscard]] std::optional<assets::PlatesetSlot>
  slot_at(float x, float y) const noexcept;
  // The whole rig diagram as last drawn: several plates dropped anywhere on
  // it fill the empty slots their names propose.
  [[nodiscard]] bool rig_contains(float x, float y) const noexcept;

  void show_message(std::string message) { message_ = std::move(message); }

private:
  gui::UiId id_{};
  std::array<gui::Rect, assets::plateset_slot_count> slot_rects_{};
  gui::Rect rig_rect_{};
  NumberField offset_field_;
  // An offset being scrubbed or typed: shown at once, authored once when
  // the field lets go, so a drag is one undo step.
  std::optional<assets::PlatesetSlot> offset_slot_;
  core::i64 offset_value_{};
  std::string message_;
  // The set the message is about; another set opens without it.
  doc::NodeId message_set_{};
  float scroll_{};
};

} // namespace mediaserver::editor
