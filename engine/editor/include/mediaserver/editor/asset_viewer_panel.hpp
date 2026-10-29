#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/asset_viewer.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/plateset_editor.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/render/model_report.hpp>
#include <mediaserver/scene/scene_model.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::editor {

// A marked range for the target track.
struct AssetViewerSourceEdit final {
  SourceEditMode mode{SourceEditMode::Insert};
  core::u32 source_in{};
  core::u32 source_frames{};

  bool operator==(const AssetViewerSourceEdit &) const = default;
};

// The picture let go outside the viewer, carrying the asset and the range it
// had marked (the whole source when nothing was marked).
struct AssetViewerDrop final {
  doc::NodeId asset{};
  core::u32 source_in{};
  core::u32 source_frames{};
  float screen_x{};
  float screen_y{};

  bool operator==(const AssetViewerDrop &) const = default;
};

// The model the viewer shows, as the editor's model cache has it.
enum class AssetViewerModelState : core::u8 {
  Loading,
  Ready,
  Failed,
};

struct AssetViewerModelInput final {
  AssetViewerModelState state{AssetViewerModelState::Loading};
  // Why it did not load, from the importer.
  std::string_view diagnostic;
  // The package's facts, once the worker has them.
  std::shared_ptr<const render::ModelPackageFacts> facts;
  render::ModelCheckContext checks;
  // The loaded package's own box, once it has loaded.
  std::optional<WorldBounds> bounds;
  // The project renders Advanced, which an HDRI and shadows need.
  bool advanced{};
};

struct AssetViewerDrawInput final {
  const assets::AssetRegistry *assets{};
  // The Director's answer for the last request. Null before the first.
  const AssetViewerInput *director{};
  // The rendered picture, sized to the image area the last draw reported.
  const rhi::Texture *image{};
  // The picture is still waiting on decoded frames, or a model's geometry
  // and textures; `loading_detail` says which, as the Stage does.
  bool decoding{};
  std::string_view loading_detail;
  // A model's assets failed on the GPU, with the reason in loading_detail.
  bool load_failed{};
  // Why there is no picture; empty when there is one.
  std::string_view image_fault;
  scene::MediaFrameRate show_rate{50U, 1U};
  // Where Insert, Overwrite and Append land, by the Timeline's targeting
  // rule: a track, or doc::NodeId{} for outside every track. Empty when the
  // rule names a locked track.
  std::optional<doc::NodeId> target_lane;
  std::string_view target_name;
  // Scene edits are refused while a transition runs.
  bool edits_locked{};
  // The Stage scene's elements and tracks, for the Used In list.
  std::span<const ElementInfo> elements;
  std::span<const scene::TimelineLane> lanes;
  // Screen pixels per interface unit, so 100% is one source pixel per pixel.
  float pixels_per_unit{1.0F};
  // Set while the open asset is a model or projection mesh.
  const AssetViewerModelInput *model{};
  // For a plateset: the Library's thumbnails for its slot tiles, whether a
  // Library drag is in flight, and what motion analysis has found.
  std::span<const LibraryThumbnailObservation> thumbnails;
  bool dragging_assets{};
  const PlatesetAnalysisView *plateset_analysis{};
};

struct AssetViewerFrame final {
  // What to show next frame. Empty while no media asset is open.
  std::optional<AssetViewerRequest> request;
  // What to draw of the open model next frame; empty unless one is open.
  std::optional<AssetViewerModelView> model_view;
  // The operator asked to light the model with an image of their choosing.
  bool choose_hdri{};
  std::optional<AssetViewerSourceEdit> source_edit;
  // The picture is being dragged out; and where it was let go.
  bool dragging{};
  std::optional<AssetViewerDrop> drop;
  // A Used In row was chosen: select that clip.
  std::optional<doc::NodeId> select_clip;
  // The image area in screen pixels, which the picture is rendered at.
  core::u32 surface_width{};
  core::u32 surface_height{};
  // The screen pixel under the pointer inside that area, for the probe.
  std::optional<std::pair<core::u32, core::u32>> probe;
  // The viewer took this frame's Space or arrow keys.
  bool consumed_keys{};
  // What the plateset page asked for, while a plateset is open.
  std::optional<PlatesetEditorFrame> plateset;

  bool operator==(const AssetViewerFrame &) const = default;
};

// A source monitor for one Library asset at a time. Double-clicking an asset
// in the Library opens it here; Space, J, K and L play it at its own rate;
// I and O mark a range; and `,`, `.` or the buttons cut the range into the
// target track at the Stage playhead. The picture zooms about the pointer
// with the wheel, pans with a middle drag, and drags out onto the Timeline
// or the Stage carrying the range. Everything the viewer holds - frame,
// marks, zoom, channel - is editor-local and resets with the project.
class AssetViewerPanel final {
public:
  explicit AssetViewerPanel(gui::UiId id) noexcept
      : id_(id), plateset_editor_(id ^ 0x5A17E5E7ULL) {}

  // Shows `asset` from `frame`, with In and Out at `marks` when given, and
  // adds it to the history the back and forward buttons step through.
  void open(doc::NodeId asset, core::u32 frame = 0U,
            std::optional<std::pair<core::u32, core::u32>> marks =
                std::nullopt);

  [[nodiscard]] AssetViewerFrame draw(gui::UiContext &ctx,
                                      const gui::Rect &bounds,
                                      const AssetViewerDrawInput &input);

  // The picture under the pointer while it is dragged out, drawn above the
  // panels it is dragged over.
  void draw_drag_ghost(gui::UiContext &ctx,
                       const rhi::Texture *image) const;

  // The probed display colour under the pointer, a frame or two late.
  void set_probe_value(std::optional<gui::UiColor> value) noexcept {
    probe_value_ = value;
  }
  // A short note in the edit row, such as why a cut was refused.
  void show_message(std::string message, double now);
  // Lights the model with the equirectangular image at `path`.
  void set_hdri(std::string path);

  // A new project's ids mean nothing here: forget the asset and history.
  void reset_project_state() noexcept;

  [[nodiscard]] doc::NodeId asset() const noexcept { return asset_; }
  [[nodiscard]] const PlatesetEditor &plateset_editor() const noexcept {
    return plateset_editor_;
  }
  [[nodiscard]] PlatesetEditor &plateset_editor() noexcept {
    return plateset_editor_;
  }
  [[nodiscard]] core::u32 frame() const noexcept { return frame_; }
  [[nodiscard]] bool focused() const noexcept { return focused_; }
  [[nodiscard]] bool playing() const noexcept { return direction_ != 0; }
  [[nodiscard]] bool dragging() const noexcept { return dragging_; }
  [[nodiscard]] std::optional<core::u32> mark_in() const noexcept {
    return mark_in_;
  }
  [[nodiscard]] std::optional<core::u32> mark_out() const noexcept {
    return mark_out_;
  }
  [[nodiscard]] AssetViewerChannel channel() const noexcept {
    return channel_;
  }
  [[nodiscard]] float exposure_stops() const noexcept { return exposure_; }
  [[nodiscard]] const AssetViewerFraming &framing() const noexcept {
    return framing_;
  }
  [[nodiscard]] const StageOrbit &orbit() const noexcept { return orbit_; }
  [[nodiscard]] render::ViewShading shading() const noexcept {
    return shading_;
  }
  void set_shading(render::ViewShading shading) noexcept { shading_ = shading; }
  [[nodiscard]] const AssetViewerLookDev &look() const noexcept {
    return look_;
  }
  [[nodiscard]] std::optional<core::u32> isolated_part() const noexcept {
    return part_;
  }
  [[nodiscard]] std::optional<core::u32> clip() const noexcept {
    return clip_;
  }
  [[nodiscard]] bool turntable() const noexcept { return turntable_; }

  // The model view's three tabs below the picture.
  enum class ModelTab : core::u8 {
    Details,
    Parts,
    Animation,
  };

private:
  struct Pass;

  void play(int direction, double now) noexcept;
  void stop() noexcept;
  void seek(core::u32 frame, core::u32 count) noexcept;
  void advance(Pass &p);
  void draw_header(Pass &p);
  // The < and > that step back and forward through the assets viewed.
  void history_buttons(gui::UiContext &ctx, const gui::Rect &back,
                       const gui::Rect &forward);
  void draw_image(Pass &p);
  void draw_scrub(Pass &p);
  void draw_transport(Pass &p);
  void draw_edit_row(Pass &p);
  void handle_keys(Pass &p);
  void draw_model(Pass &p);
  void draw_model_header(Pass &p);
  void draw_model_image(Pass &p);
  void draw_model_tools(Pass &p);
  void draw_model_info(Pass &p);
  void draw_model_details(Pass &p, gui::FormLayout &rows);
  void draw_model_parts(Pass &p, gui::FormLayout &rows);
  void draw_model_animation(Pass &p, gui::FormLayout &rows);
  void handle_model_keys(Pass &p);
  void advance_model(Pass &p);
  void play_clip(double now) noexcept;
  void request_edit(Pass &p, SourceEditMode mode);
  [[nodiscard]] std::pair<core::u32, core::u32> marked_range(
      core::u32 count) const noexcept;

  gui::UiId id_{};
  PlatesetEditor plateset_editor_;
  doc::NodeId asset_{};
  std::vector<doc::NodeId> history_;
  std::size_t history_index_{};
  core::u32 frame_{};
  // -1, 0 or 1. Playing runs from origin_frame_ at origin_time_.
  int direction_{};
  double origin_time_{};
  core::u32 origin_frame_{};
  std::optional<core::u32> mark_in_;
  std::optional<core::u32> mark_out_;
  bool loop_{};
  bool in_to_out_{};
  AssetViewerFraming framing_{};
  AssetViewerChannel channel_{AssetViewerChannel::Rgb};
  bool full_resolution_{};
  float exposure_{};
  bool focused_{};
  bool scrubbing_{};
  bool panning_{};
  bool drag_armed_{};
  bool dragging_{};
  float press_x_{};
  float press_y_{};
  std::optional<gui::UiColor> probe_value_;
  std::string message_;
  double message_until_{};
  std::vector<std::string> use_labels_;
  std::vector<doc::NodeId> use_clips_;

  // Model mode. Like everything above, editor-local and reset with the
  // project.
  bool model_fresh_{true};
  bool navigated_{};
  bool framed_from_package_{};
  StageOrbit orbit_{};
  render::ViewShading shading_{render::ViewShading::Lit};
  AssetViewerLookDev look_{};
  bool turntable_{};
  // Seconds a turntable turn takes.
  float turntable_period_{20.0F};
  scene::Turns turntable_angle_{};
  // When the model view last advanced; empty before its first frame.
  std::optional<double> last_time_;
  std::optional<core::u32> part_;
  std::optional<core::u32> clip_;
  bool clip_playing_{};
  bool clip_loop_{true};
  double clip_origin_time_{};
  core::u64 clip_origin_frame_{};
  core::u64 clip_frame_{};
  // The pointer button steering the model view, while one is held.
  enum class ModelDrag : core::u8 { None, Orbit, Rig, Track, Dolly };
  ModelDrag model_drag_{ModelDrag::None};
  ModelTab tab_{ModelTab::Details};
  gui::ScrollRegion info_scroll_;
  std::shared_ptr<const render::ModelPackageFacts> checked_facts_;
  render::ModelCheckContext checked_context_{};
  std::vector<render::ModelCheck> checks_;
};

} // namespace mediaserver::editor
