#pragma once

// The Project Settings dialog and the document settings it edits.

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace mediaserver::editor {

// The document half of the same idea: how this project's effects are computed.
//
// Split from Preferences deliberately. Preferences describe the machine and are
// never replicated; these change the picture, so they are an ordinary document
// edit that undoes, saves, and reaches every render node. Nothing here belongs
// beside an individual effect in the Inspector - a kernel width is set once per
// show and then left alone, and putting it on every blur node would be five
// controls an operator scrolls past forever.
// What became of the authored environment map, from the renderer's side.
enum class EnvironmentStatus : core::u8 {
  // No map authored. The ordinary case, and not a fault: the hemisphere is a
  // real answer, so this must not read as an error.
  NotAuthored,
  // Authored and still being read or decoded on the residency worker.
  Loading,
  // Read or decode failed. Sticky until the path changes.
  Failed,
  // Authored, and the renderer this project selected cannot use it. Image-
  // based lighting is Advanced only, and a Basic project shows the hemisphere
  // however good the map is.
  BackendUnsupported,
  // There is no stage node to store it on, so Apply has nowhere to write and
  // the field reverts. Its own state because that is a project-structure
  // problem rather than anything about the map, and the two look identical
  // from the operator's side - the value simply does not stick.
  NoStage,
  // Lighting the scene.
  Ready,
};

[[nodiscard]] std::string_view
environment_status_text(EnvironmentStatus status) noexcept;

// What became of the authored colour, from the output transform's side.
//
// Every way it can fail is otherwise silent: a look file that does not exist,
// a table still baking, and a project running Basic - where colour is never
// applied because the display transform lives on the scene-linear path - all
// show the same picture. Naming the state is the difference between "the look
// does nothing" and one specific thing to fix.
enum class ColorStatus : core::u8 {
  // The table for the authored transform and look is still baking on the
  // residency worker. The previous table stays on the output meanwhile.
  Loading,
  // Baked and live on the output.
  Ready,
  // The look file could not be read or is not a 3D `.cube`. The transform is
  // live without it, so the wall is predictable while the path is fixed.
  Failed,
  // The project is running Basic, which is display-referred already: nothing
  // here reaches the wall until Advanced is selected and available.
  BackendUnsupported,
};

[[nodiscard]] std::string_view color_status_text(ColorStatus status) noexcept;
// The residency state, in the operator's terms.
[[nodiscard]] ColorStatus
color_status_for(render::DisplayLutState state) noexcept;

struct ProjectSettingsState final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  // The lighting environment, which lives on the stage node rather than in the
  // renderer policy: it is content, not a capability. Edited here because it is
  // one per scene and is selected through the Environment scene row.
  scene::Environment environment{};
  doc::NodeId stage_node{};
  // What the renderer is actually doing with the authored map.
  //
  // Reported because every way this can fail is otherwise silent: a path that
  // does not resolve, a file that is not Radiance, and a project still on the
  // Basic backend all produce exactly the same thing on screen - the
  // hemisphere - and an operator has no way to tell which. Naming the state is
  // the difference between "it does not work" and one specific thing to fix.
  EnvironmentStatus environment_status{EnvironmentStatus::NotAuthored};
  // The document node the values came from, or zero when the project has none
  // and the authoring boundary must create one.
  doc::NodeId node{};
  // Lens calibration is camera-owned document state, unlike the project-wide
  // renderer policy above. Project Settings edits the active stage camera when
  // one is available; a zero source deliberately keeps the controls read-only.
  std::optional<scene::LensProfile> lens_profile;
  doc::NodeId camera_node{};
  // Which authored passes the renderer actually ran on the last frame.
  //
  // Last in the struct because callers brace-initialise the fields above
  // positionally, and because it is observed rather than edited - Apply never
  // writes it back.
  render::RendererPassStatus passes{};
  // What the output transform did with the authored colour. Observed, like
  // the passes above, and filled by the editor shell that owns the transform.
  ColorStatus color_status{ColorStatus::Ready};
  bool bake_running{};
  std::string bake_status;
  doc::NodeId reflection_anchor{};
  // Program is a published revision drawn with renderer settings other than
  // these; the next PUBLISH carries these to Program.
  bool program_renderer_differs{};
};

struct ProjectSettingsValues final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  scene::Environment environment{};
  std::optional<scene::LensProfile> lens_profile;

  bool operator==(const ProjectSettingsValues &) const = default;
};

struct ProjectSettingsFrame final {
  std::optional<scene::BakedLighting> bake;
  std::string bake_destination;
  bool cancel_bake{};
  // A committed modal draft or an immediate edit from the scene Inspector.
  std::optional<ProjectSettingsValues> apply;
};

class ProjectSettingsPanel final {
public:
  explicit ProjectSettingsPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const scene::EffectSettings &settings,
            const scene::RendererSettings &renderer,
            const std::optional<scene::LensProfile> &lens_profile = {},
            const scene::Environment &environment = {});
  void close() noexcept { open_ = false; }
  void adopt_bake(const scene::BakedLighting& baked) { environment_draft_.baked = baked; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // `bounds` is the whole window, as for Preferences: this is a modal card and
  // the caller must have denied the pointer to everything drawn before it.
  [[nodiscard]] ProjectSettingsFrame draw(gui::UiContext &ctx,
                                          const gui::Rect &bounds,
                                          const ProjectSettingsState &state);

private:
  enum class Category : core::u8 { Tracking, Lens };

  gui::UiId id_{};
  bool open_{};
  Category category_{Category::Tracking};
  scene::EffectSettings draft_{};
  scene::RendererSettings renderer_draft_{};
  scene::Environment environment_draft_{};
  std::optional<scene::LensProfile> lens_draft_;
  NumberField tracking_fields_;
  gui::ScrollRegion scroll_;
};

} // namespace mediaserver::editor
