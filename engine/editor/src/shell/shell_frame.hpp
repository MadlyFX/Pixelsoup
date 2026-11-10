#pragma once

// One editor frame as an object: EditorShell::frame() builds a Frame from
// its inputs and runs the phases in order. State that one phase hands to
// another lives here as members; everything else stays local to its phase.
// Internal to the editor library.

#include "shell_impl.hpp"

namespace mediaserver::editor {

// Built by aggregate initialization from the shell, its state and
// frame()'s arguments, in the order they are declared below.
struct EditorShell::Frame final {
  // Phases, in the order frame() runs them. start() is false when the
  // window is closing and the frame ends there.
  [[nodiscard]] bool start();
  void prepare_views();
  void render_views();
  void run_header();
  void adopt_model_ingests();
  void prepare_panels();
  void dock_panels();
  void apply_drops();
  void finish();

  // Draws one docked panel; the dock calls it for every visible leaf.
  void draw_panel(gui::PanelId panel, const gui::Rect &bounds);
  void draw_stage_panel(const gui::Rect &bounds);
  void draw_program_panel(const gui::Rect &bounds);
  void draw_scene_panel(const gui::Rect &bounds);
  void draw_selection_sets_panel(const gui::Rect &bounds);
  void draw_masks_panel(const gui::Rect &bounds);
  void draw_asset_viewer_panel(const gui::Rect &bounds);
  void draw_library_panel(const gui::Rect &bounds);
  void draw_effects_panel(const gui::Rect &bounds);
  void draw_compositor_panel(const gui::Rect &bounds);
  void draw_hardware_mapping_panel(const gui::Rect &bounds);
  void draw_feed_mapping_panel(const gui::Rect &bounds);
  void draw_output_panel(const gui::Rect &bounds);
  void draw_live_data_panel(const gui::Rect &bounds);
  void draw_cues_panel(const gui::Rect &bounds);
  void draw_automation_panel(const gui::Rect &bounds);
  void draw_macros_panel(const gui::Rect &bounds);
  void draw_timeline_panel(const gui::Rect &bounds);
  void draw_color_correction_panel(const gui::Rect &bounds);
  void draw_texture_panel(const gui::Rect &bounds);
  void draw_inspector_panel(const gui::Rect &bounds);

  // Edits and gestures more than one phase or panel authors.
  void request_clip_clipboard(ClipClipboardAction action);
  void request_key_clipboard(TimelineKeyClipboardEdit edit);
  void route_marker_edits(std::span<const TimelineMarkerEdit> edits);
  scene::Camera stage_view_camera(const Impl::StageViewport &view);
  scene::Canvas stage_view_canvas(const Impl::StageViewport &view);
  render::CompositorSceneExecutor &stage_renderer_of(std::size_t index);
  render::OutputTransform &stage_display_of(std::size_t index);
  core::u64 scene_selection_token() noexcept;
  core::u64 begin_semantic_nudge(const NumericNudge &nudge,
                                 HistoryContext context,
                                 core::u64 selection) noexcept;
  void delete_selection();
  void capture_group(const scene::Scene &authored_scene,
                     const scene::Scene &display_scene);
  void emit_components(doc::NodeId node, scene::TransformChannel first,
                       scene::Vec3 values, core::u64 gesture);
  void emit_transform(const TransformDelta &delta, core::u64 gesture);
  void clear_transform(TransformTool tool, const scene::Scene &authored_scene);
  void route_surface_edit(FeedSurfaceEditRequest request);
  void enqueue_model(std::string source, scene::SceneId add_scene,
                     std::optional<FeedSurfaceAddRequest> surface_add,
                     std::optional<FeedSurfaceEditRequest> surface_edit,
                     doc::NodeId folder = {}, doc::NodeId revise = {},
                     std::optional<core::u64> gesture = std::nullopt);
  void handle_viewport(const ViewportFrame &viewport,
                       const scene::Scene &authored_view,
                       const scene::Scene &display_view);
  void choose_shading(render::ViewShading shading);
  void navigate_stage_camera(const ViewportFrame &viewport, bool pointer_free,
                             bool left_available);
  void add_simple(AddKind kind);
  // Shows a native dialog off the frame thread (Impl::dialog); `apply` runs
  // on the frame that sees the operator's choice, and not at all on cancel.
  // `show` blocks its own thread until the operator answers.
  void open_file_dialog(std::function<std::optional<std::string>()> show,
                        std::function<void(Frame &, std::string)> apply);
  // A yes/no question, shown the same way; `yes` runs only on yes.
  void open_confirm_dialog(std::string title, std::string message,
                           std::function<void(Frame &)> yes);
  // Runs `then` now for a clean project, and otherwise only once the operator
  // agrees to discard the unsaved changes for `action`.
  void confirm_discard(std::string_view action,
                       std::function<void(Frame &)> then);
  // Asks once whether to open unsaved work recovered at start-up.
  void offer_recovery();
  // Saves to the project's own file, or asks for one when it has none.
  void save_project();
  void adopt_dialog();
  void apply_inspector_file(const FilePickerRequest &request,
                            std::string path);
  void handle_library_add(LibraryAddAction action);
  void import_files(std::span<const std::string> sources, doc::NodeId folder);
  void import_asset(LibraryImportAction action, doc::NodeId folder);
  void instantiate_asset(
      doc::NodeId id, std::optional<scene::Vec3> position,
      std::optional<CompositorAddIntent> graph_layer = std::nullopt,
      std::optional<scene::Vec3> rotation = std::nullopt,
      std::optional<scene::ClipTiming> timing = std::nullopt);
  void draw_placeholder(const gui::Rect &bounds, std::string_view title,
                        std::span<const std::string_view> rows);
  void queue_mask_edit(MaskEditRequest request);
  void run_color_sampler(gui::UiId id, const gui::Rect &image,
                         const rhi::Texture &target,
                         bool pointer_available = true);
  MaskViewportInput mask_viewport_input(const scene::Scene &view,
                                        const ViewportProjection *projection);
  void apply_mask_viewport_frame(const MaskViewportFrame &mask_frame);
  void open_clip_in_viewer(const TimelineViewClipRequest &request);
  void delete_active_domain();

  EditorShell &shell;
  Impl &impl;

  // What frame() was given.
  const scene::Scene &scene;
  const EditorTimeline &timeline;
  const std::vector<scene::ChannelKeyframes> &keyframes;
  const std::vector<ElementInfo> &elements;
  const std::vector<scene::EffectInstance> &effects;
  const topology::HardwareMap &hardware_map;
  const output::FeedMap &feed_map;
  const EditorSwitcherState &switcher;
  const assets::AssetRegistry &asset_registry;
  std::span<const LibraryThumbnailObservation> thumbnails;
  const compositor::CompositorGraph &compositor_graph;
  const EditorProjectState &project;
  const OutputMonitorState &output_monitor;
  const topology::HardwareRuntimeSnapshot &hardware_runtime;
  const sync::SyncPolicy &sync_policy;
  const EditorPreferencesState &preferences;
  const ProjectSettingsState &project_settings;
  std::span<const LibraryProxyRuntimeObservation> proxy_runtime;
  std::span<const scene::TimelineLane> timeline_lanes;
  std::optional<core::i64> clip_clipboard_span;
  std::span<const scene::SelectionSetSummary> selection_sets;
  std::optional<TimelineKeyScalePreview> key_scale_preview;
  std::span<const TimelineFilmstripObservation> filmstrips;
  const LiveDataState &live_data;
  std::span<const scene::TrackingSource> tracking_sources;
  std::span<const scene::TimelineMarker> timeline_markers;
  const AssetViewerInput &asset_viewer;
  const HistoryState &history_state;

  EditorFrame result{};

  // Long-lived state this frame works on.
  rhi::Device &device = impl.window.device();
  Impl::AssetViewerRender &viewer = impl.viewer;
  Impl::ModelThumbnailWork &thumbs = impl.model_thumbnails;
  gui::UiContext &ui = impl.ui;
  const scene::Scene &authored_stage =
      switcher.stage_scene != nullptr ? *switcher.stage_scene : scene;

  // Handed from one phase to the next.
  std::vector<ViewportPlate> plate_overlay{};
  std::span<const LibraryProxyRuntimeObservation> library_proxy_runtime{};
  scene::Scene stage_scene{};
  std::optional<scene::Scene> stage_view_scene{};
  scene::Scene stage_interaction_scene{};
  // Program's scene as its compositor graph places it; unset while that is
  // the scene itself.
  std::optional<scene::Scene> program_interaction_scene{};
  const output::OutputMap *program_output{};
  ProgramOutputExtent program_extent{};
  std::size_t stage_view_count{};
  std::array<PreviewExtent, 4U> stage_previews{};
  bool program_targets_ready{};
  std::array<bool, 4U> stage_targets_ready{};
  const assets::MediaAsset *viewer_media{};
  const assets::ModelAsset *viewer_model{};
  bool viewer_ready{};
  const rhi::Texture *program_texture{};
  bool program_preview_ok{};
  bool program_preview_held{};
  bool program_mapping_unavailable{};
  std::string_view program_preview_fault{};
  std::optional<render::RendererReadiness> renderer_readiness{};
  ProgramAssetStatus program_assets{};
  std::string bake_activity{};
  std::optional<ViewportProjection> program_view_projection{};
  float width{};
  float height{};
  bool preferences_blocked_pointer{};
  ApplicationMenuResult menu_result{};
  bool application_menu_blocks_pointer{};
  bool delete_consumed{};
  bool clipboard_consumed{};
  bool editor_control_editing{};
  ViewportFrame stage_viewport_frame{};
  bool stage_viewport_drawn{};
  std::optional<LibraryAssetDrop> pending_asset_drop{};
  std::optional<AssetViewerDrop> pending_viewer_drop{};
  bool viewer_drawn_now{};
  std::optional<doc::NodeId> dragging_asset{};
  bool plateset_editor_drawn{};
  std::vector<assets::DerivativeKey> plateset_thumbnail_keys{};
  std::vector<TimelineMoment> timeline_moments{};
  std::vector<doc::NodeId> dragging_assets{};
  std::optional<LibraryAddAction> dragging_generator{};
  std::optional<LibraryGeneratorDrop> pending_generator_drop{};
  bool library_took_files{};
  std::optional<EffectApplyRequest> pending_effect_drop{};
  std::optional<TimelineLayerDrop> pending_timeline_layer_drop{};
  gui::Rect compositor_panel_bounds{};
  bool compositor_panel_drawn{};
  bool timeline_panel_drawn{};
  gui::Rect timeline_panel_bounds{};
  gui::Rect inspector_panel_bounds{};
  bool inspector_panel_drawn{};
  SceneSettingsFrame live_scene_settings{};
  std::optional<InspectorPanelRequest> pending_inspector_panel{};
  bool color_sample_active{};
  MasksPanelTarget mask_target{};
  bool mask_authoring_enabled{};
  bool mask_stage_active{};
  bool mask_program_active{};
  bool compositor_locked{};
};

} // namespace mediaserver::editor
