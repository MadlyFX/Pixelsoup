#pragma once

#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/mask_authoring.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace mediaserver::editor {

// The active target establishes the coordinate-space contract before a tool
// emits anything: drawable masks are SourceLayer, group masks AuthoredCanvas.
struct MasksPanelTarget final {
  scene::SceneId scene{};
  doc::NodeId target{};
  scene::MaskCoordinateSpace coordinate_space{
      scene::MaskCoordinateSpace::SourceLayer};
  // False when the target has no one flat surface to cut - a box, sphere,
  // imported model, particle effect or group. Its existing masks still list,
  // so they can be turned off or deleted, but no new one can be made.
  bool flat_surface{true};

  bool operator==(const MasksPanelTarget &) const = default;
};

// Mask/point selection is an editor-local typed domain shared with the Stage.
// It never aliases or mutates ordinary Scene selection.
struct MasksPanelSelection final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  std::optional<doc::NodeId> point;

  bool operator==(const MasksPanelSelection &) const = default;
};

enum class MaskCreateTool : core::u8 {
  Rectangle,
  Ellipse,
  Bezier,
};

struct MaskCreateModifiers final {
  // Familiar shape-tool semantics: Shift constrains aspect; Alt draws from
  // centre. The Stage consumes these while the panel remains geometry-free.
  bool constrain_aspect{};
  bool from_center{};

  bool operator==(const MaskCreateModifiers &) const = default;
};

struct MaskCreateIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  scene::MaskCoordinateSpace coordinate_space{
      scene::MaskCoordinateSpace::SourceLayer};
  MaskCreateTool tool{MaskCreateTool::Rectangle};
  MaskCreateModifiers modifiers{};

  bool operator==(const MaskCreateIntent &) const = default;
};

struct MaskRenameIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  std::string name;

  bool operator==(const MaskRenameIntent &) const = default;
};

struct MaskEnabledIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  bool enabled{};

  bool operator==(const MaskEnabledIntent &) const = default;
};

struct MaskCombineIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  scene::MaskCombine combine{scene::MaskCombine::Add};

  bool operator==(const MaskCombineIntent &) const = default;
};

struct MaskInvertIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  bool invert{};

  bool operator==(const MaskInvertIntent &) const = default;
};

enum class MaskScalarProperty : core::u8 {
  FeatherCanvasPx,
  ExpansionCanvasPx,
  Opacity,
};

struct MaskScalarEditIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  MaskScalarProperty property{MaskScalarProperty::Opacity};
  float value{};
  NumberFieldEdit edit{};

  bool operator==(const MaskScalarEditIntent &) const = default;
};

enum class MaskBoundsComponent : core::u8 {
  Left,
  Top,
  Right,
  Bottom,
};

struct MaskBoundsEditIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  scene::MaskBounds bounds{};
  MaskBoundsComponent component{MaskBoundsComponent::Left};
  NumberFieldEdit edit{};

  bool operator==(const MaskBoundsEditIntent &) const = default;
};

struct MaskReorderIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  std::size_t destination_index{};

  bool operator==(const MaskReorderIntent &) const = default;
};

struct MaskDeleteIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};

  bool operator==(const MaskDeleteIntent &) const = default;
};

struct MaskPointInsertIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  std::size_t index{};
  scene::MaskPointGeometry geometry{};

  bool operator==(const MaskPointInsertIntent &) const = default;
};

enum class MaskPointComponent : core::u8 {
  X,
  Y,
  InX,
  InY,
  OutX,
  OutY,
};

struct MaskPointEditIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  doc::NodeId point{};
  scene::MaskPointPatch patch{};
  MaskPointComponent component{MaskPointComponent::X};
  NumberFieldEdit edit{};

  bool operator==(const MaskPointEditIntent &) const = default;
};

struct MaskPointDeleteIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  doc::NodeId mask{};
  doc::NodeId point{};

  bool operator==(const MaskPointDeleteIntent &) const = default;
};

// A shape drawn on the picture with a shape tool: a mask of exactly that
// shape on `target`, where the panel's own Create makes a default one.
struct MaskDrawIntent final {
  scene::SceneId scene{};
  doc::NodeId target{};
  scene::MaskCoordinateSpace coordinate_space{
      scene::MaskCoordinateSpace::SourceLayer};
  scene::MaskShape shape{scene::RectangleMaskShape{}};

  bool operator==(const MaskDrawIntent &) const = default;
};

using MasksPanelIntent = std::variant<
    MaskCreateIntent, MaskRenameIntent, MaskEnabledIntent, MaskCombineIntent,
    MaskInvertIntent, MaskScalarEditIntent, MaskBoundsEditIntent,
    MaskReorderIntent, MaskDeleteIntent, MaskPointInsertIntent,
    MaskPointEditIntent, MaskPointDeleteIntent>;

struct MasksPanelFrame final {
  bool selection_changed{};
  MasksPanelSelection selection{};
  std::optional<MasksPanelIntent> intent;
};

[[nodiscard]] std::string_view mask_diagnostic_message(
    scene::MaskDiagnosticKind kind) noexcept;

class MasksPanel final {
public:
  explicit MasksPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] MasksPanelFrame draw(
      gui::UiContext &ctx, const gui::Rect &bounds,
      const MasksPanelTarget &target,
      std::span<const scene::MaskInstance> masks,
      std::span<const scene::MaskDiagnostic> diagnostics,
      const MasksPanelSelection &selection,
      bool authoring_enabled = true);

  [[nodiscard]] std::string_view edit_name() const noexcept {
    return rename_draft_;
  }

private:
  gui::UiId id_{};
  NumberField number_field_;
  // A mask name is opened by a second click on the row that is already
  // selected, the way a track header opens its name, so the list needs no
  // separate field and no button to commit one.
  doc::NodeId rename_node_{};
  std::string rename_draft_;
  doc::NodeId click_node_{};
  double click_time_{};
  // The row under a held pointer. Reordering is the drag, so the panel carries
  // no up/down controls.
  doc::NodeId drag_node_{};
  gui::ScrollRegion scroll_;
};

} // namespace mediaserver::editor
