#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/library_edit.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Library panel: the project's persistent assets, the folders they are
// filed in, and the built-in generators. Like the other leaf panels it keeps
// document semantics out of the UI and reports what the operator asked for.
namespace mediaserver::editor {

// Intentionally separate from AddKind in editor_shell.hpp: this header is a
// leaf panel API and can therefore be included by the shell without a cycle.
// Media and Model remain only for the transitional draw() wrapper used by the
// current shell. The asset-aware LibraryFrame never returns either through
// `add`; imports and existing-asset instantiation have distinct fields below.
enum class LibraryAddAction : core::u8 {
  Box,
  Plane,
  Sphere,
  Background,
  Media,
  Model,
  DirectionalLight,
  PointLight,
  SpotLight,
  Camera,
};

enum class LibraryImportAction : core::u8 {
  Media,
  Model,
};

// Runtime-only projection of the app/server thumbnail service. These values
// are observations, never document state and never an alternate Asset node.
enum class LibraryThumbnailState : core::u8 {
  Placeholder,
  Queued,
  Generating,
  Ready,
  Stale,
  Failed,
};

struct LibraryThumbnailObservation final {
  // Contains the persistent Asset NodeId plus its expected content fingerprint
  // and the frozen thumbnail profile/generator versions.
  assets::DerivativeKey key;
  LibraryThumbnailState state{LibraryThumbnailState::Placeholder};
  // Borrowed for this draw only. Ready without a texture remains a typed Ready
  // status while the app finishes its CPU-to-GPU upload.
  const rhi::Texture *texture{};

  bool operator==(const LibraryThumbnailObservation &) const = default;
};

// Machine-local playback truth for one persistent Library MediaAsset. The
// optional key names the exact requested source revision/profile/generator;
// it remains a derivative cache key and never becomes another Asset identity.
enum class LibraryProxyRuntimeState : core::u8 {
  Source,
  ProxyReady,
  PreferFallback,
  RequiredUnready,
  Failed,
};

// Editor-owned mirror of app/server failures. Keeping this type in the editor
// prevents the reusable panel API from depending on the executable layer.
enum class LibraryProxyRuntimeIssue : core::u8 {
  None,
  MissingAssetIdentity,
  AssetNotFound,
  AssetKindMismatch,
  PlacementSourceMismatch,
  AvailabilityMissing,
  CacheInspectionFailed,
  ArtifactLoadFailed,
  ArtifactMetadataMismatch,
  FrameMappingFailed,
  MaterializationFailed,
};

struct LibraryProxyTranscodeProgress final {
  core::u64 completed_frames{};
  core::u64 total_frames{};
  // A serialized proxy worker can have several visible jobs waiting behind
  // the active encode. Queued jobs retain a zero-width determinate track and
  // an explicit label instead of pretending that encoding has begun.
  bool queued{};

  bool operator==(const LibraryProxyTranscodeProgress &) const = default;
};

enum class LibraryMediaDecodeState : core::u8 {
  Unknown,
  HardwareAccelerated,
  CpuOnly,
  Unavailable,
};

struct LibraryProxyRuntimeObservation final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeKey> key;
  LibraryProxyRuntimeState state{LibraryProxyRuntimeState::Source};
  assets::ProxyPlaybackReason reason{assets::ProxyPlaybackReason::SourcePolicy};
  LibraryProxyRuntimeIssue issue{LibraryProxyRuntimeIssue::None};
  std::optional<LibraryProxyTranscodeProgress> transcode_progress;
  LibraryMediaDecodeState media_decode{LibraryMediaDecodeState::Unknown};

  bool operator==(const LibraryProxyRuntimeObservation &) const = default;
};

// Stable, concise text used by the Library row and by non-visual tests. The
// authored SRC/AUTO/REQ control remains separate from this runtime status.
[[nodiscard]] std::string
library_proxy_runtime_status(const LibraryProxyRuntimeObservation &observation);

enum class LibraryThumbnailRequestPriority : core::u8 {
  Visible,
  Prefetch,
};

struct LibraryThumbnailRequestIntent final {
  assets::DerivativeKey key;
  LibraryThumbnailRequestPriority priority{
      LibraryThumbnailRequestPriority::Visible};

  bool operator==(const LibraryThumbnailRequestIntent &) const = default;
};

// The exact desired set for this frame. Applying it to the service's retain()
// API makes cancellation explicit when filtered, collapsed, or scrolled rows
// leave both the visible region and bounded prefetch window.
struct LibraryThumbnailRetainIntent final {
  std::vector<assets::DerivativeKey> keys;

  bool operator==(const LibraryThumbnailRetainIntent &) const = default;
};

struct LibraryThumbnailWindow final {
  core::u32 prefetch_asset_rows{4U};
  core::u32 maximum_request_intents{16U};

  bool operator==(const LibraryThumbnailWindow &) const = default;
};

inline constexpr core::u32 maximum_library_thumbnail_prefetch_rows = 32U;
inline constexpr core::u32 maximum_library_thumbnail_request_intents = 64U;

// Library assets let go outside the Library. `asset` is the one the drag
// was started on; `assets` is everything dragged, `asset` first, so a drop
// on Stage can place each.
// A surface generator - Box, Plane, Sphere or Background - dragged out of
// the Library and let go at a screen point, for the shell to place on Stage
// where it landed.
struct LibraryGeneratorDrop final {
  LibraryAddAction action{LibraryAddAction::Box};
  float screen_x{};
  float screen_y{};

  bool operator==(const LibraryGeneratorDrop &) const = default;
};

// Whether a generator can be dragged out and rested on a Stage surface: the
// ones that are a shape. Lights and cameras are placed where they are added.
[[nodiscard]] constexpr bool library_generator_placeable(
    LibraryAddAction action) noexcept {
  return action == LibraryAddAction::Box ||
         action == LibraryAddAction::Plane ||
         action == LibraryAddAction::Sphere ||
         action == LibraryAddAction::Background;
}

struct LibraryAssetDrop final {
  doc::NodeId asset{};
  float screen_x{};
  float screen_y{};
  std::vector<doc::NodeId> assets;

  bool operator==(const LibraryAssetDrop &) const = default;
};

// One explicit authored proxy preference change. Availability and generation
// remain machine-local observations; this carries only the profile request and
// Source/Prefer/Require policy stored on the existing MediaAsset node.
struct LibraryProxyPolicyIntent final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeProfileId> requested_profile;
  assets::ProxyPlaybackPolicy policy{assets::ProxyPlaybackPolicy::Source};

  bool operator==(const LibraryProxyPolicyIntent &) const = default;
};


// How the Library lays out what it holds: a sortable table of every asset
// and its metadata, or a grid of thumbnails for one folder at a time.
enum class LibraryView : core::u8 {
  List,
  Grid,
};

// The List view's columns, in their display order. Name is always shown;
// the rest can be hidden from the header's right-click menu.
enum class LibraryColumn : core::u8 {
  Name,
  Type,
  Resolution,
  FrameRate,
  Duration,
  StartTimecode,
  EndTimecode,
  Frames,
  Codec,
  Vertices,
  Triangles,
  Materials,
  Textures,
  TextureSize,
  FileSize,
  Proxy,
  Status,
  Path,
  Count,
};

inline constexpr std::size_t library_column_count =
    static_cast<std::size_t>(LibraryColumn::Count);

[[nodiscard]] std::string_view library_column_title(LibraryColumn column);

// What a column shows for `asset`, from authored probe metadata only.
// Empty when the column does not apply to this kind of asset or the value
// was never probed. Status is runtime state and is composed by the panel.
[[nodiscard]] std::string library_column_text(const assets::Asset &asset,
                                              LibraryColumn column);

// Which kind of Library asset a file on disk would import as, from its
// extension; nothing for a file that is neither media nor a model.
[[nodiscard]] std::optional<LibraryImportAction>
library_import_kind(std::string_view path);

// SMPTE timecode at `rate`. `timecode_frames` reads "HH:MM:SS:FF" (a ';'
// or '.' before the frames marks drop-frame) as a frame number, or nothing
// when it is not a timecode at that rate; `timecode_text` writes a frame
// number back. Drop-frame applies only at 29.97 and 59.94 and their
// multiples; any other rate counts every frame.
[[nodiscard]] std::optional<core::i64>
timecode_frames(std::string_view timecode, assets::MediaFrameRate rate);
[[nodiscard]] std::string timecode_text(core::i64 frame,
                                        assets::MediaFrameRate rate,
                                        bool drop_frame);

// One library interaction. Importing adds a persistent project asset,
// instantiating places an already-imported asset in the scene, and the
// organisation edits only change where an asset is listed. Generator and
// light actions continue to author scene elements directly.
struct LibraryFrame final {
  std::optional<LibraryAddAction> add;
  std::optional<LibraryImportAction> import_asset;
  // Files dragged in from the desktop and let go over the Library.
  std::vector<std::string> import_files;
  // The folder an import - from the dialog or dropped files - lands in.
  doc::NodeId import_folder{};
  std::optional<doc::NodeId> instantiate_asset;
  // Removes only the persistent Library records. Existing scene placements
  // keep their authored source fallback and therefore continue to render.
  std::vector<doc::NodeId> remove_assets;
  // Changed files the operator accepted: re-probe each and record its new
  // revision on the same asset.
  std::vector<doc::NodeId> accept_sources;
  // Folders made, renamed, removed, and assets filed under them.
  std::vector<assets::LibraryEdit> library_edits;
  // Assets dragged beyond the click threshold keep their capture outside
  // the Library. The shell resolves this screen-space release against
  // Stage, the Compositor and the Timeline so a persistent asset can be
  // placed where it was dropped.
  std::optional<doc::NodeId> dragging_asset;
  // Every asset in that drag, `dragging_asset` first.
  std::vector<doc::NodeId> dragging_assets;
  std::optional<LibraryAssetDrop> asset_drop;
  // A placeable generator carried beyond the click threshold, and where it
  // was let go outside the Library. A generator released without moving is
  // a click and reports `add` instead.
  std::optional<LibraryAddAction> dragging_generator;
  std::optional<LibraryGeneratorDrop> generator_drop;
  std::optional<LibraryProxyPolicyIntent> proxy_policy;
  // Look at the sources on disk again, reading their bytes. Nothing does this
  // on a timer, so a file that arrived after the project opened is noticed
  // when the operator says to look.
  bool rescan_assets{};
  // The Library took this frame's Delete key for its own selection.
  bool consume_delete{};
  std::vector<LibraryThumbnailRequestIntent> thumbnail_requests;
  // Disengaged on the source-compatible draw overload. Engaged, including an
  // empty key vector, on every thumbnail-aware draw.
  std::optional<LibraryThumbnailRetainIntent> thumbnail_retain;

  bool operator==(const LibraryFrame &) const = default;
};

// The project's Library, laid out the way editorial and engine asset
// browsers are: a toolbar to search, import and make folders; a List view
// whose columns carry timecode, resolution, length, codec, geometry and
// texture size and sort on a header click; a Grid view of thumbnails one
// folder at a time; and a footer slider that sizes the thumbnails in
// either. Click selects (Ctrl toggles, Shift extends), double-click places
// an asset in the scene or opens a folder, and a drag files the selection
// into a folder or carries it out to Stage, the Compositor or the Timeline
// under a thumbnail that follows the pointer. Files dragged in from the
// desktop import into the folder they are let go over.
class LibraryPanel final {
public:
  explicit LibraryPanel(gui::UiId id) noexcept;

  [[nodiscard]] LibraryFrame draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                  const assets::AssetRegistry &assets);

  [[nodiscard]] LibraryFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const assets::AssetRegistry &assets,
       std::span<const LibraryThumbnailObservation> thumbnails,
       LibraryThumbnailWindow window = {});

  [[nodiscard]] LibraryFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const assets::AssetRegistry &assets,
       std::span<const LibraryThumbnailObservation> thumbnails,
       LibraryThumbnailWindow window,
       std::span<const LibraryProxyRuntimeObservation> proxy_runtime);

  // Transitional wrapper for the current shell. It draws an empty registry
  // and maps IMPORT MEDIA/MODEL back to the old Media/Model actions. New
  // integrations should use the frame overload so import and instantiate
  // can never be mistaken for the same operation.
  [[nodiscard]] std::optional<LibraryAddAction> draw(gui::UiContext &ctx,
                                                     const gui::Rect &bounds);

  // The thumbnail of what is being dragged, under the pointer. The shell
  // draws it after every panel so it rides above them all; it draws nothing
  // unless this frame's draw() is carrying a drag.
  void draw_drag_ghost(gui::UiContext &ctx);

  [[nodiscard]] std::string_view search() const noexcept { return search_; }
  [[nodiscard]] LibraryView view() const noexcept { return view_; }
  void set_view(LibraryView view) noexcept { view_ = view; }
  // The current view's thumbnail size, 0 (smallest) to 1 (largest).
  [[nodiscard]] float icon_size() const noexcept {
    return view_ == LibraryView::List ? list_zoom_ : grid_zoom_;
  }
  void set_icon_size(float size) noexcept;
  // The folder the Grid view is showing; zero is the top level.
  [[nodiscard]] doc::NodeId folder() const noexcept { return folder_; }
  void open_folder(doc::NodeId folder) noexcept;
  [[nodiscard]] std::span<const doc::NodeId> selection() const noexcept {
    return selection_;
  }
  [[nodiscard]] bool column_visible(LibraryColumn column) const noexcept {
    return column_visible_[static_cast<std::size_t>(column)];
  }
  void set_column_visible(LibraryColumn column, bool visible) noexcept;
  [[nodiscard]] LibraryColumn sort_column() const noexcept {
    return sort_column_;
  }
  [[nodiscard]] bool sort_descending() const noexcept {
    return sort_descending_;
  }
  void sort_by(LibraryColumn column, bool descending) noexcept {
    sort_column_ = column;
    sort_descending_ = descending;
  }
  // A project load: forget the selection, open folders and any edit.
  void reset_project_state() noexcept;
  // The assets whose files changed since they were recorded, as the last
  // RESCAN found them. Marked in both views and offered for acceptance.
  void set_changed_sources(std::span<const doc::NodeId> changed) {
    changed_.assign(changed.begin(), changed.end());
  }

private:
  [[nodiscard]] LibraryFrame
  draw_impl(gui::UiContext &ctx, const gui::Rect &bounds,
            const assets::AssetRegistry &assets,
            std::span<const LibraryThumbnailObservation> thumbnails,
            const LibraryThumbnailWindow *thumbnail_window,
            std::span<const LibraryProxyRuntimeObservation> proxy_runtime);

  gui::UiId id_{};
  std::string search_;
  LibraryView view_{LibraryView::List};
  float list_zoom_{0.2F};
  float grid_zoom_{0.35F};
  float scroll_{};
  float scroll_x_{};
  // Where on a scrollbar's thumb the drag took hold, so the thumb stays
  // under the pointer rather than jumping to centre on it.
  float scrollbar_grip_{};
  doc::NodeId folder_{};
  // The Grid view is showing the built-in generators rather than a folder.
  bool generators_open_{};
  // List view: the generators section, and the folders the operator opened.
  bool generators_expanded_{true};
  std::set<doc::NodeId> expanded_;
  std::vector<doc::NodeId> selection_;
  std::vector<doc::NodeId> changed_;
  doc::NodeId anchor_{};
  std::array<float, library_column_count> column_width_{};
  std::array<bool, library_column_count> column_visible_{};
  LibraryColumn sort_column_{LibraryColumn::Name};
  bool sort_descending_{};
  // A column edge being dragged, and where its width started.
  std::optional<LibraryColumn> resizing_;
  float resize_start_width_{};
  InlineRename rename_;
  ClickTracker clicks_;
  // The Library was the last panel clicked, so F2 and Delete are for it.
  bool focused_{};
  // A drag of the selection: what it carries and where it began.
  std::vector<doc::NodeId> drag_items_;
  doc::NodeId drag_primary_{};
  float drag_start_x_{};
  float drag_start_y_{};
  bool drag_moved_{};
  bool drag_pressed_selected_{};
  // A press on a placeable generator, which becomes a drag once it moves.
  std::optional<LibraryAddAction> generator_drag_;
  bool generator_drag_moved_{};
  // What draw_drag_ghost() shows. The texture is borrowed for this frame
  // only, like every thumbnail; the ghost is drawn only on a frame whose
  // draw() set `ghost_live_`.
  bool ghost_live_{};
  std::string ghost_label_;
  const rhi::Texture *ghost_texture_{};
  bool ghost_model_{};
  // The generator a ghost stands for, drawn as its icon.
  std::optional<LibraryAddAction> ghost_generator_;
  std::size_t ghost_count_{};
  // A folder this panel asked for, opened for naming once it appears.
  std::optional<std::string> pending_folder_name_;
  doc::NodeId pending_folder_parent_{};
  std::set<doc::NodeId> known_folders_;
  // The open context menu: what it was opened on and what each entry does,
  // fixed when it opened so what the operator reads is what a choice does.
  doc::NodeId menu_target_{};
  bool menu_open_{};
  std::vector<std::string> menu_entries_;
  std::vector<core::u8> menu_commands_;
};

} // namespace mediaserver::editor
