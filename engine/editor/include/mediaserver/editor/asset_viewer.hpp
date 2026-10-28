#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <string>

// The Asset Viewer's half of the editor/Director boundary.
//
// The viewer shows one Library asset outside every scene. Everything here is
// editor-local: the frame it shows, its marks, its zoom and the one-clip scene
// that draws it are never replicated, digested, saved or sent to a node. The
// only thing that leaves the viewer as an edit is a marked range cut into a
// track, which reaches the Director as an ordinary undoable intent.
namespace mediaserver::editor {

// How a marked source range goes into a track. Insert pushes what is at and
// after the edit point along; Overwrite covers it, keeping both uncovered
// remainders of a clip it lands inside; Append lands after the track's last
// clip and moves nothing.
enum class SourceEditMode : core::u8 {
  Insert,
  Overwrite,
  Append,
};

// What the viewer shows of the picture. Display-only: the wall never sees it.
enum class AssetViewerChannel : core::u8 {
  Rgb,
  Red,
  Green,
  Blue,
  Luma,
};

// What the viewer asks to see on the next frame.
struct AssetViewerRequest final {
  doc::NodeId asset{};
  core::u32 source_frame{};

  bool operator==(const AssetViewerRequest &) const = default;
};

// Which file the picture comes from, as the Director resolved it.
enum class AssetViewerRendition : core::u8 {
  // Nothing requested, or an asset the viewer cannot show yet.
  None,
  // The asset's own file, required by the project and found on this machine.
  Source,
  // The asset's own file. Nothing in the project uses the asset, so nothing
  // has checked that the file is there; a missing one draws the placeholder.
  Unchecked,
  // Its proxy, by the asset's Prefer or Require Proxy policy.
  Proxy,
  // Require Proxy with no proxy ready: nothing is decoded, as on the wall.
  ProxyUnready,
  // The file is missing. The grey placeholder draws, as on the wall.
  Offline,
};

// The asset's filmstrip derivative, for the viewer's scrub bar.
struct AssetViewerFilmstrip final {
  assets::DerivativeKey key;
  assets::DerivativeState state{assets::DerivativeState::Missing};
  const rhi::Texture *texture{};

  bool operator==(const AssetViewerFilmstrip &) const = default;
};

// The Director's answer to the last request: the scene that shows it,
// resolved to the file this machine decodes exactly as the Stage is, and
// which file that turned out to be.
struct AssetViewerInput final {
  const scene::Scene *scene{};
  AssetViewerRequest shown{};
  AssetViewerRendition rendition{AssetViewerRendition::None};
  std::optional<AssetViewerFilmstrip> filmstrip;
};

// How many frames the viewer can step through: the probed frame count, or one
// for a still or an asset not yet probed.
[[nodiscard]] core::u32
asset_viewer_frame_count(const assets::MediaAsset &asset) noexcept;

// The picture's width over its height, 16:9 until it has been probed.
[[nodiscard]] float asset_viewer_aspect(const assets::MediaAsset &asset) noexcept;

// The picture's pixel size, 1920x1080 until it has been probed.
[[nodiscard]] assets::PixelDimensions
asset_viewer_pixels(const assets::MediaAsset &asset) noexcept;

// The rate the viewer plays and counts timecode at: the source's own, or the
// show's for an image sequence that has none.
[[nodiscard]] assets::MediaFrameRate
asset_viewer_rate(const assets::MediaAsset &asset,
                  scene::MediaFrameRate show_rate) noexcept;

// `asset` alone, as one always-on clip on a quad one unit tall and as wide as
// its picture, standing at the origin facing +Z and showing `source_frame`.
// The surround is black. Only the project's backend, quality and colour
// settings carry over, so the picture goes through the same decode and colour
// path the wall's does, without the wall's bloom, fog or temporal history.
[[nodiscard]] scene::Scene
asset_viewer_media_scene(const assets::MediaAsset &asset,
                         core::u32 source_frame,
                         const scene::RendererSettings &project);

// Where the viewer looks. `scale` is screen pixels per source pixel; zero
// fits the picture to the image area. `center` is the world point at the
// image area's centre, in the units of asset_viewer_media_scene: the picture
// spans x in [-aspect/2, aspect/2] and y in [-1/2, 1/2].
struct AssetViewerFraming final {
  float scale{};
  float center_x{};
  float center_y{};

  bool operator==(const AssetViewerFraming &) const = default;
};

// The scale a fit shows `source` at in an image area of `width` x `height`
// screen pixels.
[[nodiscard]] float asset_viewer_fit_scale(assets::PixelDimensions source,
                                           float width, float height) noexcept;

// The framing's scale, with zero (fit) resolved.
[[nodiscard]] float asset_viewer_scale(const AssetViewerFraming &framing,
                                       assets::PixelDimensions source,
                                       float width, float height) noexcept;

// The world height the image area spans.
[[nodiscard]] float asset_viewer_view_height(const AssetViewerFraming &framing,
                                             assets::PixelDimensions source,
                                             float width,
                                             float height) noexcept;

// The orthographic camera the image area is drawn through. It reaches the
// renderer as an explicit view; the scene's own camera stays perspective.
[[nodiscard]] scene::Camera
asset_viewer_camera(const AssetViewerFraming &framing,
                    assets::PixelDimensions source, float width,
                    float height) noexcept;

// The picture's rectangle inside an image area of `width` x `height` whose
// top-left is (0, 0), in the same units. Parts may lie outside the area.
struct AssetViewerPictureRect final {
  float x{};
  float y{};
  float width{};
  float height{};
};
[[nodiscard]] AssetViewerPictureRect
asset_viewer_picture_rect(const AssetViewerFraming &framing,
                          assets::PixelDimensions source, float width,
                          float height) noexcept;

// The framing after zooming by `factor` about the image-area point
// (`at_x`, `at_y`), so the point under the pointer stays put.
[[nodiscard]] AssetViewerFraming
asset_viewer_zoom(const AssetViewerFraming &framing,
                  assets::PixelDimensions source, float width, float height,
                  float factor, float at_x, float at_y) noexcept;

// The framing after dragging the picture by (`dx`, `dy`) image-area units.
[[nodiscard]] AssetViewerFraming
asset_viewer_pan(const AssetViewerFraming &framing,
                 assets::PixelDimensions source, float width, float height,
                 float dx, float dy) noexcept;

// The source's own timecode for `frame`: its start timecode plus the frame,
// at its own rate, drop-frame when its start timecode is. Empty without a rate.
[[nodiscard]] std::string asset_viewer_timecode(const assets::MediaAsset &asset,
                                                scene::MediaFrameRate show_rate,
                                                core::u64 frame);

} // namespace mediaserver::editor
