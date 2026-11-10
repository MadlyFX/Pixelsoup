#pragma once

// The editor shell's state between frames: the window, its renderers,
// every panel, and the selection and gesture state the panels share.
// Internal to the editor library.

#include "shell_chrome.hpp"

#include <mediaserver/editor/async_dialog.hpp>

#include <functional>
#include <memory>
#include <mutex>

namespace mediaserver::editor {

struct EditorShell::Impl final {
  // Declare the device owner first so every GPU texture and submission,
  // including the asset-activation fence, is released before its device.
  gui::UiWindow window;
  struct PendingModelIngest {
    std::shared_ptr<render::ModelIngestJob> job;
    std::string display_name;
    core::u64 gesture{};
    core::u64 project_generation{};
    scene::SceneId add_scene{};
    std::optional<FeedSurfaceAddRequest> surface_add;
    std::optional<FeedSurfaceEditRequest> surface_edit;
    doc::NodeId texture_target{};
    std::string expected_source;
    doc::NodeId folder{};
    // The Library asset this ingest re-reads for an accepted change.
    doc::NodeId revise{};
  };
  std::vector<PendingModelIngest> pending_model_ingests;
  // Re-probed assets waiting for the rest of their gesture, so everything
  // one Accept asked for reaches the Director - and undoes - together.
  std::vector<AssetRevisionIntent> pending_revisions;
  render::ModelCache inspector_model_cache;
  // What each selected particle package publishes, for the Inspector.
  render::ParticlePackageCache inspector_particle_cache{ajfx::gpu::ShaderFormat::Dxil};
  core::AssetPublicationGate asset_gate;
  core::AssetSetReadiness assets{};
  std::optional<rhi::Texture> visible_program;
  struct PendingRecording {
    rhi::SubmittedWork work;
    core::u64 asset_digest{}, record_ns{}, profile_digest{}, sequence{};
  };
  // The RHI supports at most four contexts. Retaining their tickets preserves
  // timestamp ownership without reducing the existing recording concurrency.
  std::array<PendingRecording, 4U> pending_recordings{};
  core::FrameTiming frame_timing{};
  core::u64 next_recording_sequence{1U};
  core::u64 uploaded_asset_digest{};
  std::shared_ptr<render::BakeJob> bake_job;
  scene::BakedLighting bake_settings;
  doc::NodeId bake_stage;
  core::u64 bake_snapshot_digest{};
  std::optional<core::u64> look_document_digest;
  core::u64 look_asset_generation{};
  doc::NodeId look_stage;
  bool look_observed{}, look_stale{};
  std::string bake_message;
  std::string completed_bake_content;
  std::chrono::steady_clock::time_point program_wait_started{};
  StageAssetStatus stage_assets{true, "LOADING STAGE ASSETS...",
                                "Preparing the first Stage image."};
  std::string ingest_message;

  // Every native dialog - file pickers, folder pickers and yes/no confirms -
  // runs here, off the frame thread, so the Director's frame loop that draws
  // this editor keeps sealing frames while the operator answers.
  AsyncDialog<EditorShell::Frame> dialog;
  // Whether the recovery on offer has been put to the operator.
  bool recovery_asked{};

  enum class PendingMediaAction : core::u8 { Add, Import, Revise };

  struct PendingMediaProbe final {
    PendingMediaAction action{};
    std::string source;
    std::future<render::MediaProbe> future;
    bool launch_failed{};
    core::u64 gesture{};
    scene::SceneId scene{};
    core::u64 project_generation{};
    // The Library folder an import lands in.
    doc::NodeId folder{};
    // The Library asset a Revise probe re-reads.
    doc::NodeId revise{};
  };

  gui::FontAtlas font;
  gui::InputState input;
  gui::UiDrawList draw_list;
  gui::UiContext ui;
  LibraryThumbnailTextureCache library_thumbnail_textures;
  // The Hardware Map's pixel-art sheet, uploaded once when the editor opens.
  std::optional<rhi::Texture> hardware_sprites;
  TimelineFilmstripTextureCache timeline_filmstrip_textures;
  // Program, transition-to, and Stage are recorded into one command list.
  // Their media inputs share immutable source textures while their render
  // targets, playheads, and compositor state remain independent.
  render::MediaCache frame_media_cache;

  render::CompositorSceneExecutor program_renderer;
  render::CompositorSceneExecutor program_to_renderer;
  render::ProjectionMeshWarp program_warp;
  render::ProjectionMeshWarp program_to_warp;
  render::SceneMixer program_mixer;
  render::OutputTransform program_display;
  render::CompositorSceneExecutor stage_renderer;
  render::OutputTransform stage_display;
  render::OutputProjectionCache program_projection_cache;
  // A missed non-blocking command acquisition holds this last complete image.
  // The operator keeps a stale picture instead of a black panel while the GPU
  // drains; an actual record/submit fault invalidates it explicitly below.
  bool program_preview_valid{};
  core::u32 program_preview_width{};
  core::u32 program_preview_height{};

  gui::DockLayout docks;
  gui::DockHost dock_host;
  WorkspaceLayoutBytes saved_workspace_layout;
  core::u32 workspace_window_width{};
  core::u32 workspace_window_height{};
  // Operator interface zoom, in percent of the display's own scale.
  core::u16 ui_zoom_percent{100U};
  // The show bar's night dim: a calmer palette for the small hours.
  bool night{};
  std::optional<InspectorColorSampleRequest> color_sample_request;
  ViewportPanel program_panel{gui::ui_id("editor.viewport.program"),
                              ViewportRole::Program};
  // The Stage's views. The first fills the Stage alone; in four views it is
  // the bottom-right one, beside Top, Front and Side. Each keeps its own
  // camera, panel (its own picking, gizmo and measurement) and render
  // target. The one toolbar sets the tools for all of them.
  struct StageViewport final {
    StageViewport(const char *id, StageProjection projection) noexcept
        : panel{gui::ui_id(id), ViewportRole::Stage} {
      orbit.projection = projection;
    }
    StageOrbit orbit{};
    // An authoring aid for this view alone; Program always renders lit.
    render::ViewShading shading{render::ViewShading::Lit};
    ViewportPanel panel;
    // Every view past the first renders through its own executor and
    // display, made the first time four views are shown.
    std::optional<render::CompositorSceneExecutor> renderer;
    std::optional<render::OutputTransform> display;
    const rhi::Texture *texture{};
    // The view follows its pane's actual content aspect, one frame behind a
    // resize, so it fills its pane rather than letterboxing.
    core::u32 surface_width{};
    core::u32 surface_height{};
    StageAssetStatus assets{true, "LOADING STAGE ASSETS...",
                            "Preparing the first Stage image."};
    ViewportFrame frame{};
  };
  std::array<StageViewport, 4U> stage_views{
      {{"editor.viewport.stage", StageProjection::Perspective},
       {"editor.viewport.stage.top", StageProjection::Top},
       {"editor.viewport.stage.front", StageProjection::Front},
       {"editor.viewport.stage.side", StageProjection::Side}}};
  // The operator asked for four views. They are drawn once their renders
  // exist, and the first view stands alone while masks are authored or a
  // colour is being sampled.
  bool stage_four{};
  bool stage_four_failed{};
  // The other three views have been set up to look where the first did.
  bool stage_four_shown{};
  bool stage_single_only{};
  // The view the Stage code is acting for right now: each in turn while the
  // views are drawn, then the one under the pointer or holding a gesture.
  std::size_t stage_current{};
  std::size_t navigation_view{};
  std::size_t stage_bookmark_view{};
  [[nodiscard]] StageViewport &stage() noexcept {
    return stage_views[stage_current];
  }
  InspectorPanel inspector_panel{gui::ui_id("editor.inspector")};
  TimelinePanel timeline_panel{gui::ui_id("editor.timeline")};
  CuesPanel cues_panel{gui::ui_id("editor.cues")};
  HistoryPanel history_panel{gui::ui_id("editor.history")};
  // The strip is put away; the status bar offers it back. Saved with the
  // workspace.
  bool history_hidden{};
  // What the hovered History step touched: set while the strip draws, and
  // outlined on Stage the next frame, whichever panel draws first.
  std::vector<doc::NodeId> history_hover;
  std::vector<doc::NodeId> stage_history_highlight;
  ScenePanel scene_panel{gui::ui_id("editor.scene")};
  SelectionSetsPanel selection_sets_panel{
      gui::ui_id("editor.selection-sets")};
  MasksPanel masks_panel{gui::ui_id("editor.masks")};
  LibraryPanel library_panel{gui::ui_id("editor.library")};
  EffectsPanel effects_panel{gui::ui_id("editor.effects")};
  ColorCorrectionPanel color_correction_panel{
      gui::ui_id("editor.color-correction")};
  TexturePanel texture_panel{gui::ui_id("editor.texture")};
  CompositorPanel compositor_panel{gui::ui_id("editor.compositor")};
  OutputMonitorPanel output_panel{gui::ui_id("editor.output-monitor")};
  LiveDataPanel live_data_panel{gui::ui_id("editor.live-data")};
  PreferencesPanel preferences_panel{gui::ui_id("editor.preferences")};
  SceneSettingsPanel scene_settings_panel;
  SceneSettingsTarget scene_settings_selection{SceneSettingsTarget::None};
  ProjectSettingsPanel project_settings_panel{
      gui::ui_id("editor.project-settings")};
  HardwareMappingPanel hardware_mapping_panel{
      gui::ui_id("editor.hardware-mapping")};
  FeedMappingPanel feed_mapping_panel{gui::ui_id("editor.feed-mapping")};
  AssetViewerPanel asset_viewer_panel{gui::ui_id("editor.asset-viewer")};
  // The Asset Viewer's picture. Its executor keeps its own media cache, so a
  // scrub's pending decodes never count against Program's asset readiness,
  // and it shares the window's device so device decoding stays available.
  // Everything here is made the first time the panel shows an asset and
  // released after the panel has been hidden for a few seconds.
  struct AssetViewerRender final {
    std::optional<render::CompositorSceneExecutor> renderer;
    std::optional<render::OutputTransform> display;
    std::optional<render::InspectionView> inspection;
    bool failed{};
    // The picture the panel draws, or null.
    const rhi::Texture *texture{};
    // The image area the panel last asked for, in screen pixels.
    core::u32 width{};
    core::u32 height{};
    bool drawn{};
    core::u32 hidden_frames{};
    bool decoding{};
    // Why a model's picture is not complete yet, as the Stage says it.
    StageAssetStatus model_status;
    std::string fault;
    // What the current picture shows, so an unchanged one is not redrawn.
    struct Shown final {
      AssetViewerRequest request{};
      std::string source;
      AssetViewerRendition rendition{AssetViewerRendition::None};
      AssetViewerFraming framing{};
      AssetViewerChannel channel{AssetViewerChannel::Rgb};
      float exposure{};
      core::u32 width{};
      core::u32 height{};
      scene::ColorSettings color{};
      // Model mode: the view, the box it framed, and the Stage lighting it
      // borrowed, which can change under an unchanged view.
      std::optional<AssetViewerModelView> model;
      std::optional<WorldBounds> model_bounds;
      std::optional<scene::Environment> stage_environment;
      std::vector<scene::DirectionalLight> stage_lights;

      bool operator==(const Shown &) const = default;
    };
    std::optional<Shown> shown;
    // The model view the panel last asked for, drawn one frame behind like
    // the pane size, and the loaded package's box.
    std::optional<AssetViewerModelView> model_request;
    std::optional<WorldBounds> model_bounds;
    // Model packages come from the process-wide model cache the renderer
    // reads, observed without waiting; their facts are worked out on a
    // worker made the first time a model is shown.
    render::ModelCache model_cache;
    std::unique_ptr<ModelFactsWorker> facts_worker;
    // The pixel probe: one download of the display image in flight at a
    // time, read when the GPU has finished it, never waited for.
    std::optional<rhi::Readback> readback;
    rhi::SubmittedWork probe_work;
    bool probe_pending{};
    core::u32 probe_x{};
    core::u32 probe_y{};
    core::u32 probe_width{};
    core::u32 probe_height{};
    double probe_time{};
    std::optional<std::pair<core::u32, core::u32>> probe_at;
  } viewer;

  // Library thumbnails of models, drawn one at a time at the profile's
  // 320 x 180 in the viewer's studio look-dev scene, in a command list of
  // their own after the frame's. The targets are made for the first and
  // released once nothing has been queued for a while.
  struct ModelThumbnailWork final {
    struct Job final {
      assets::DerivativeKey key;
      std::string source;
      core::u32 waited{};
      core::u32 drawn{};
      bool settled{};
    };
    std::deque<Job> queue;
    std::optional<render::CompositorSceneExecutor> renderer;
    std::optional<render::OutputTransform> display;
    std::optional<rhi::Readback> readback;
    rhi::SubmittedWork download;
    bool downloading{};
    core::u32 idle_frames{};
    std::vector<RenderedModelThumbnail> finished;
  } model_thumbnails;

  // Scene-node selection is editor-local and canonical. Compositor, physical
  // surface, and mapping selections remain in their distinct carriers below.
  NodeSelection scene_selection;
  // A saved-set row is a distinct typed panel selection. It never becomes a
  // Scene node selection and therefore cannot accidentally enter Inspector,
  // viewport, keyboard Delete, or clip/key clipboard operations.
  doc::NodeId selection_set_selection{};
  // Mask and Bezier-point selection is intentionally disjoint from canonical
  // Scene selection. The latter chooses the target; this carrier chooses the
  // operation/point within that target and is shared only by Masks + Stage.
  MasksPanelSelection mask_selection{};
  bool mask_coverage_visible{};
  // Group ids are allocated at the Director boundary. Keep the exact child
  // set briefly so the first projected frame containing their new parent can
  // promote that parent to the canonical selection.
  std::vector<doc::NodeId> pending_group_members;
  core::u32 pending_group_frames{};
  // A new empty group has no members to recognise it by, so keep the groups
  // that existed when it was asked for: the first projected frame with one
  // more under `pending_new_group_parent` (zero: the top level) selects it
  // and opens its name.
  std::optional<doc::NodeId> pending_new_group_parent;
  std::set<doc::NodeId> pending_new_group_known;
  core::u32 pending_new_group_frames{};
  std::optional<doc::NodeId> stage_surface_selection;
  std::optional<doc::NodeId> program_output_map_node;
  scene::SceneId displayed_scene{};
  std::size_t scene_list_offset{};
  std::optional<scene::SceneId> pending_scene_delete;
  // The PROJECT SCENES row whose right-click menu is open.
  std::optional<scene::SceneId> scene_menu_scene;
  // In-place renaming of a project scene's row.
  InlineRename scene_rename{gui::ui_id("editor.scene.rename")};
  ClickTracker scene_clicks;
  core::u64 project_generation{};
  DeleteDomain delete_domain{DeleteDomain::Scene};
  ApplicationMenu open_menu{ApplicationMenu::None};
  bool recent_projects_submenu_open{};
  bool scene_link_details_open{};
  bool blender_connection_open{};
  std::string blender_selection;
  std::size_t blender_page{};
  std::string publication_storage_draft;
  bool publication_storage_editing{};
  double hardware_warning_until{};
  double compositor_warning_until{};

  // The Stage cameras (StageViewport::orbit) are editor-local and never enter
  // the document. They are navigated as in the Unreal Editor (see
  // stage_navigation_for): a button joins a camera gesture only when it was
  // pressed on a Stage view, and the gesture then follows whichever of those
  // buttons are still held, in that view.
  // Ctrl+1..9 camera bookmarks for the free view, and the note that
  // confirms one was kept or recalled.
  std::array<std::optional<StageOrbit>, 9U> stage_bookmarks{};
  std::string stage_bookmark_note;
  double stage_bookmark_note_until{};
  bool navigate_left{};
  bool navigate_middle{};
  bool navigate_right{};
  // The gesture has moved the camera, so the cursor is hidden and locked.
  bool navigation_moved{};
  float fly_speed{stage_default_fly_speed};
  double fly_speed_shown_until{};
  double navigation_time{};
  // This frame's share of a held fly, measured once for all the views.
  float navigation_seconds{};

  // A viewport drag captures every selected element's starting local position.
  // The panel reports one displayed-world delta; each node maps that through
  // its compositor and scene-group parents so the selection moves rigidly even
  // while the committed scene lags behind the drag.
  std::vector<doc::NodeId> group_nodes;
  std::vector<scene::Vec3> group_start;
  // Maps a displayed world-space drag vector back into each node's authored
  // parent space. This includes both scene-group ancestry and any compositor
  // spatial parent (for example an LED car-process plate slot).
  std::vector<scene::Mat4> group_delta_to_local;
  // What a rotate or scale needs as well: each node's evaluated rotation and
  // scale, and where and how it is drawn, parallel to group_nodes.
  std::vector<TransformStart> group_transform;
  // Blender-linked objects are placed by one world matrix: their authored
  // world when the gesture began, and how the Stage's display differs from it.
  std::vector<bool> group_linked;
  std::vector<scene::Mat4> group_authored_world;
  std::vector<scene::Mat4> group_display_adjustment;

  core::u64 edit_gesture{};
  core::u64 hardware_edit_gesture{};
  // Processors whose reported screens have been offered as wall cards
  // this session, so a wall the operator deletes does not come back.
  std::vector<doc::NodeId> walls_offered;
  core::u64 feed_mapping_edit_gesture{};
  core::u64 compositor_edit_gesture{};
  HistoryContext history_context{HistoryContext::Scene};
  // Held for the whole of one clip drag so that the per-frame placements a
  // move or a trim authors collapse into a single undo step.
  core::u64 clip_edit_gesture{};
  core::u64 lane_edit_gesture{};
  core::u64 next_edit_gesture{1U};
  // A plateset placement waiting on the operator's answer about the rig.
  std::optional<PlatesetPlaceIntent> pending_plateset_place;
  // Videos added while their frame rate differs from the timeline's, held
  // back until the operator says whether the timeline should follow them.
  // `changing` is set once the rate change was requested; the clips are then
  // released only after the timeline reports the new rate, so their lengths
  // are measured against it.
  struct PendingRateMatch final {
    std::vector<AddIntent> adds;
    ShowRateRequest rate;
    ShowRateRequest previous;
    bool changing{};
    core::u32 frames_waited{};
  };
  std::optional<PendingRateMatch> pending_rate_match;
  // The Library is carrying assets right now (as of its last draw), so the
  // plateset page can light its slots as drop targets.
  bool library_dragging{};
  std::vector<PendingMediaProbe> pending_media_probes;
  bool manipulating{};
  bool semantic_boundary{};
  gui::UiId semantic_focus{};

  Impl(gui::UiWindow &&created_window, gui::FontAtlas &&created_font,
       render::CompositorSceneExecutor &&program,
       render::CompositorSceneExecutor &&program_to,
       render::ProjectionMeshWarp &&program_projection,
       render::ProjectionMeshWarp &&program_to_projection,
       render::SceneMixer &&mixer,
       render::OutputTransform &&program_output,
       render::CompositorSceneExecutor &&stage,
       render::OutputTransform &&stage_output,
       std::span<const std::byte> workspace_layout)
      : window(std::move(created_window)), font(std::move(created_font)),
        ui(input, draw_list, font, editor_theme()),
        library_thumbnail_textures(window.device()),
        timeline_filmstrip_textures(window.device()),
        program_renderer(std::move(program)),
        program_to_renderer(std::move(program_to)),
        program_warp(std::move(program_projection)),
        program_to_warp(std::move(program_to_projection)),
        program_mixer(std::move(mixer)),
        program_display(std::move(program_output)),
        stage_renderer(std::move(stage)),
        stage_display(std::move(stage_output)),
        dock_host(docks) {
    workspace_window_width = window.pixel_width();
    workspace_window_height = window.pixel_height();
    reset_workspace();
    if (!workspace_layout.empty()) {
      auto decoded = decode_workspace_layout(workspace_layout);
      if (decoded) {
        ui_zoom_percent = decoded.value().ui_zoom_percent;
        compositor_panel.set_parameter_panel_width(
            decoded.value().compositor_parameter_width);
        compositor_panel.show_minimap(decoded.value().compositor_minimap);
        night = decoded.value().night_dim;
        history_hidden = decoded.value().history_hidden;
      }
      if (decoded && docks.restore(decoded.value().docks)) {
        for (StageViewport &view : stage_views) {
          view.panel.set_resolution(decoded.value().stage_resolution);
          view.panel.set_media(decoded.value().stage_media);
        }
        program_panel.set_resolution(decoded.value().program_resolution);
        program_panel.set_media(decoded.value().program_media);
        saved_workspace_layout.assign(workspace_layout.begin(),
                                      workspace_layout.end());
      }
    }
  }

  void reset_workspace() {
    ui.clear_focus();
    docks = gui::DockLayout{};
    dock_host = gui::DockHost{docks};
    // Keep the picture and common edits visible; specialized tools share
    // dock tabs or open from Window. Every region remains rearrangeable.
    const gui::DockId stage_dock = docks.add(panel_stage, "Stage");
    const gui::DockId timeline_dock = docks.dock(
        panel_timeline, "Timeline", stage_dock, gui::DockSide::Bottom, 0.34F);
    static_cast<void>(
        docks.dock(panel_cues, "Cues", timeline_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_automation, "Automation", timeline_dock,
                                 gui::DockSide::Center));
    docks.set_active(timeline_dock, panel_timeline);
    static_cast<void>(docks.dock(panel_compositor, "Compositor", timeline_dock,
                                gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_output, "Output Monitor", timeline_dock,
                                gui::DockSide::Center));
    docks.set_active(timeline_dock, panel_timeline);

    const float workspace_width = static_cast<float>(window.width());
    const float properties_width = std::clamp(workspace_width * 0.25F, 320.0F, 440.0F);
    const float scene_width = std::clamp(workspace_width * 0.18F, 200.0F, 300.0F);

    const gui::DockId scene_dock = docks.dock(
        panel_scene, "Scene", docks.root(), gui::DockSide::Left,
        scene_width / std::max(workspace_width - properties_width - 6.0F, 1.0F));
    docks.set_active(scene_dock, panel_scene);
    const gui::DockId library_dock = docks.dock(
        panel_library, "Library", scene_dock, gui::DockSide::Bottom, 0.40F);
    static_cast<void>(docks.dock(panel_effects, "Effects", library_dock,
                                 gui::DockSide::Center));
    docks.set_active(library_dock, panel_library);

    const gui::DockId inspector_dock =
        docks.dock(panel_inspector, "Properties", docks.root(),
                   gui::DockSide::Right, properties_width / std::max(workspace_width, 1.0F));
    static_cast<void>(docks.dock(panel_color_correction, "Color",
                                 inspector_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_texture, "Texture", inspector_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_masks, "Masks", inspector_dock,
                                 gui::DockSide::Center));
    docks.set_active(inspector_dock, panel_inspector);

    // Program is a dock tab alongside Stage; it does not obscure the canvas
    // until the operator chooses to detach it. The Asset Viewer is the third
    // tab there: a source monitor beside the picture it feeds.
    static_cast<void>(docks.dock(panel_program, "Program Output", stage_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_asset_viewer, "Asset Viewer",
                                 stage_dock, gui::DockSide::Center));
    docks.set_active(stage_dock, panel_stage);

    // Register less frequently used panels so Window can open them on demand.
    for (const WindowPanelEntry &entry : window_panel_entries()) {
      if (docks.title_of(entry.panel).empty()) {
        docks.register_panel(entry.panel, entry.title);
      }
    }
  }

  [[nodiscard]] static std::string_view panel_title(gui::PanelId panel) {
    const auto found = std::find_if(window_panel_entries().begin(),
                                    window_panel_entries().end(),
                                    [panel](const WindowPanelEntry &entry) {
                                      return entry.panel == panel;
                                    });
    return found == window_panel_entries().end() ? std::string_view{"Panel"}
                                                 : found->title;
  }

  // Hardware Map and Feed Mapping are setup surfaces with a lot to show at
  // once: a map, a palette and a details rail side by side. They open as one
  // large floating frame over the workspace, most of the window, rather than
  // as a tab squeezed into whichever dock comes first. A floating frame never
  // blocks what lies outside it, so the rest of the editor stays usable.
  [[nodiscard]] gui::Rect setup_frame() const noexcept {
    const gui::Rect area =
        editor_dock_area(static_cast<float>(window.width()),
                         static_cast<float>(window.height()), !history_hidden);
    const float margin_x = std::max(area.width * 0.03F, 12.0F);
    const float margin_y = std::max(area.height * 0.03F, 10.0F);
    const float width = std::max(area.width - 2.0F * margin_x, 220.0F);
    const float height = std::max(area.height - 2.0F * margin_y, 140.0F);
    return gui::Rect{area.x + margin_x, area.y + margin_y, width, height};
  }

  // Opens a setup panel in its floating frame, and raises it. A panel found
  // docked - restored from an older workspace, or dropped there by hand - is
  // lifted back out. One already floating keeps the size the operator gave
  // its frame. An open companion joins the same frame as a tab, so the two
  // views of the wall never end up in different corners.
  void show_mapping_panel(gui::PanelId wanted) {
    const gui::PanelId companion = wanted == panel_hardware_mapping
                                       ? panel_feed_mapping
                                       : panel_hardware_mapping;
    // The map opens framed on the devices it shows.
    if (!docks.is_open(panel_hardware_mapping)) {
      hardware_mapping_panel.frame_devices();
    }
    gui::DockId frame = docks.floating_frame_of(wanted);
    if (frame == gui::no_dock) {
      frame = docks.floating_frame_of(companion);
    }
    gui::DockId leaf = gui::no_dock;
    if (frame != gui::no_dock) {
      // Join the frame that is already floating.
      const gui::PanelId anchor = docks.floating_frame_of(wanted) == frame
                                      ? wanted
                                      : companion;
      leaf = docks.leaf_of(anchor);
      if (anchor != wanted) {
        leaf = docks.is_open(wanted)
                   ? docks.move(wanted, leaf, gui::DockSide::Center)
                   : docks.dock(wanted, panel_title(wanted), leaf,
                                gui::DockSide::Center);
      }
    } else {
      // Introduce the panel through the ordinary tree, then detach it as a
      // real floating DockLayout root.
      static_cast<void>(docks.add(wanted, panel_title(wanted)));
      leaf = docks.detach(wanted, setup_frame());
      frame = docks.floating_frame_of(wanted);
    }
    if (leaf == gui::no_dock) {
      return;
    }
    if (docks.is_open(companion) &&
        docks.floating_frame_of(companion) != frame) {
      static_cast<void>(
          docks.move(companion, docks.leaf_of(wanted), gui::DockSide::Center));
    }
    docks.set_active(docks.leaf_of(wanted), wanted);
    if (const gui::DockId owner = docks.floating_frame_of(wanted);
        owner != gui::no_dock) {
      docks.raise(owner);
    }
  }

  void show_setup() {
    show_mapping_panel(panel_hardware_mapping);
    show_mapping_panel(panel_feed_mapping);
    show_mapping_panel(panel_hardware_mapping);
  }

  void show_panel(gui::PanelId panel) {
    if (panel == panel_hardware_mapping || panel == panel_feed_mapping) {
      show_mapping_panel(panel);
      return;
    }
    if (docks.leaf_of(panel) != gui::no_dock) {
      const gui::DockId leaf = docks.leaf_of(panel);
      docks.set_active(leaf, panel);
      return;
    }

    // A viewer opened from a workspace saved without one joins the Stage's
    // tabs, where the starting layout keeps it, rather than splitting the
    // window.
    if (panel == panel_asset_viewer && !docks.has_saved_placement(panel) &&
        docks.leaf_of(panel_stage) != gui::no_dock) {
      const gui::DockId tabbed = docks.dock(panel, panel_title(panel),
                                            docks.leaf_of(panel_stage),
                                            gui::DockSide::Center);
      if (tabbed != gui::no_dock) {
        docks.set_active(tabbed, panel);
        return;
      }
    }
    gui::DockId leaf = docks.add(panel, panel_title(panel));
    if (panel == panel_program && leaf != gui::no_dock) {
      const float monitor_width = std::clamp(
          static_cast<float>(window.width()) * 0.24F, 300.0F, 420.0F);
      const float monitor_height = monitor_width * 9.0F / 16.0F + 68.0F;
      leaf = docks.detach(panel_program,
                          gui::Rect{static_cast<float>(window.width()) * 0.58F,
                                    static_cast<float>(window.height()) * 0.26F,
                                    monitor_width, monitor_height});
    }
    if (leaf != gui::no_dock) {
      docks.set_active(leaf, panel);
    }
  }

  void toggle_panel(gui::PanelId panel) {
    if (docks.leaf_of(panel) == gui::no_dock) {
      show_panel(panel);
    } else {
      docks.remove(panel);
    }
  }

  [[nodiscard]] bool has_selection() const noexcept {
    return !scene_selection.empty();
  }

  [[nodiscard]] bool is_selected(doc::NodeId node) const noexcept {
    return scene_selection.contains(node);
  }

  void select_only(doc::NodeId node) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.replace(node);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void toggle(doc::NodeId node) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.toggle(node);
    delete_domain = scene_selection.empty() ? DeleteDomain::Neutral
                                            : DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void select_range(doc::NodeId node,
                    std::span<const doc::NodeId> visible_order) {
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.select_range(node, visible_order);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void clear_scene_selection(bool activate_scene_history) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.clear();
    delete_domain = DeleteDomain::Neutral;
    if (activate_scene_history) {
      history_context = HistoryContext::Scene;
    }
    semantic_boundary = true;
  }

  [[nodiscard]] doc::NodeId anchor() const noexcept {
    return scene_selection.primary();
  }

  [[nodiscard]] core::u64 begin_gesture() noexcept {
    if (!manipulating) {
      manipulating = true;
      edit_gesture = next_edit_gesture++;
    }
    return edit_gesture;
  }
};

} // namespace mediaserver::editor
