#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/library_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The Library's geometry, top down: a 25-unit title, the 25-unit toolbar
// (search from x=6, then New Folder, IMPORT and RESCAN at the right), the
// 20-unit column header or folder path at y=50, the content from y=70, and
// a 22-unit footer. In the List view the Generators section's heading is
// 70..92 and, open, its eight rows follow at 22 units each (92..268);
// folders and then assets come after. Asset rows are 33 units tall at the
// default thumbnail size. The empty test font measures every string as 0.
constexpr float content_top = 70.0F;
constexpr float generator_row(int index) {
  return 92.0F + 22.0F * static_cast<float>(index) + 11.0F;
}
constexpr float after_generators = 268.0F;
// Generators, folders and assets are separate groups with 10 units between
// them at the top level (6 inside a folder).
constexpr float first_group = after_generators + 10.0F;
constexpr float asset_row_height = 33.0F;

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LibraryPanel panel{gui::ui_id("library.test")};
  gui::Rect bounds{0.0F, 0.0F, 360.0F, 520.0F};
  assets::AssetRegistry registry;
  std::vector<editor::LibraryThumbnailObservation> thumbnails;
  std::vector<editor::LibraryProxyRuntimeObservation> runtime;
  editor::LibraryThumbnailWindow window{};
  bool thumbnail_aware{};

  editor::LibraryFrame frame(float x, float y, bool down,
                             gui::KeyMods mods = gui::KeyMods::None,
                             std::string_view text = {}, float wheel = 0.0F,
                             std::optional<gui::Key> key = std::nullopt,
                             bool right = false) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Right, right);
    input.set_mods(mods);
    input.add_text(text);
    input.add_wheel(wheel);
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame();
    editor::LibraryFrame result =
        thumbnail_aware || !runtime.empty()
            ? panel.draw(ctx, bounds, registry, thumbnails, window, runtime)
            : panel.draw(ctx, bounds, registry);
    ctx.draw_overlays(gui::Rect{0.0F, 0.0F, 1600.0F, 900.0F});
    return result;
  }

  editor::LibraryFrame click(float x, float y,
                             gui::KeyMods mods = gui::KeyMods::None) {
    static_cast<void>(frame(x, y, false, mods));
    static_cast<void>(frame(x, y, true, mods));
    return frame(x, y, false, mods);
  }

  editor::LibraryFrame key(gui::Key pressed) {
    return frame(-1.0F, -1.0F, false, gui::KeyMods::None, {}, 0.0F, pressed);
  }

  // A press at one point, a held move to another, and a release there.
  std::pair<editor::LibraryFrame, editor::LibraryFrame>
  drag(float from_x, float from_y, float to_x, float to_y) {
    static_cast<void>(frame(from_x, from_y, false));
    static_cast<void>(frame(from_x, from_y, true));
    editor::LibraryFrame held = frame(to_x, to_y, true);
    editor::LibraryFrame released = frame(to_x, to_y, false);
    return {std::move(held), std::move(released)};
  }

  editor::LibraryFrame right_click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    editor::LibraryFrame opened =
        frame(x, y, false, gui::KeyMods::None, {}, 0.0F, std::nullopt, true);
    static_cast<void>(frame(x, y, false));
    return opened;
  }

  // Entry `index` of a menu opened at (x, anchor_y): menus open 6 below the
  // pointer in menu_row_height rows, showing ten at a time, and with the
  // empty test font they are 40 units wide.
  editor::LibraryFrame choose(float x, float anchor_y, std::size_t index) {
    const float y = anchor_y + 6.0F +
                    gui::menu_row_height * (static_cast<float>(index) + 0.5F);
    return click(x + 10.0F, y);
  }

  void collapse_generators() { static_cast<void>(click(80.0F, 81.0F)); }
};

[[nodiscard]] assets::ContentFingerprint test_fingerprint(core::u64 index) {
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 1'000U + index;
  fingerprint.sha256.bytes[0U] = static_cast<core::u8>(index & 0xffU);
  fingerprint.sha256.bytes[1U] = static_cast<core::u8>((index >> 8U) & 0xffU);
  return fingerprint;
}

// Two assets, listed by name: Hero Car (a model), then Opening Plate.
[[nodiscard]] assets::AssetRegistry project_assets() {
  assets::MediaAsset media{};
  media.id = doc::NodeId{20U};
  media.display_name = "Opening Plate";
  media.source = "C:/show/media/opening_plate.mov";
  media.dimensions = assets::PixelDimensions{3840U, 2160U};
  media.frame_count = 900U;
  media.frame_rate = assets::MediaFrameRate{25U, 1U};

  assets::ModelAsset model{};
  model.id = doc::NodeId{30U};
  model.display_name = "Hero Car";
  model.source = "C:/show/models/hero_car.glb";
  model.triangle_count = 128'000U;
  model.material_count = 7U;

  assets::AssetRegistry registry;
  registry.assets.emplace_back(std::move(media));
  registry.assets.emplace_back(std::move(model));
  return registry;
}

constexpr float hero_car_y = first_group + asset_row_height * 0.5F;
constexpr float opening_plate_y = first_group + asset_row_height * 1.5F;

[[nodiscard]] assets::AssetRegistry
fingerprinted_media_assets(std::size_t count) {
  assets::AssetRegistry registry;
  for (std::size_t index = 0U; index < count; ++index) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{100U + static_cast<core::u64>(index)};
    // Zero-padded so the name order is the index order.
    asset.display_name = "Plate " + std::string(index < 10U ? "0" : "") +
                         std::to_string(index);
    asset.source = "C:/show/media/plate_" + std::to_string(index) + ".exr";
    asset.fingerprint = test_fingerprint(static_cast<core::u64>(index));
    registry.assets.emplace_back(std::move(asset));
  }
  return registry;
}

[[nodiscard]] assets::DerivativeKey
thumbnail_key(const assets::AssetRegistry &registry, std::size_t index) {
  const assets::Asset &asset = registry.assets.at(index);
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Thumbnail320x180Srgb);
  UI_CHECK(profile != nullptr);
  return assets::make_derivative_key(
      assets::asset_id(asset), *assets::asset_fingerprint(asset), *profile);
}

[[nodiscard]] bool contains(std::span<const assets::DerivativeKey> keys,
                            const assets::DerivativeKey &expected) {
  return std::find(keys.begin(), keys.end(), expected) != keys.end();
}

// --------------------------------------------------------------- generators

void a_generator_click_reports_its_add_action() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 260.0F, 320.0F};
  UI_CHECK(harness.click(80.0F, generator_row(0)).add ==
           editor::LibraryAddAction::Box);
  // Box, Plane, Sphere, Background, Camera, then the lights.
  UI_CHECK(harness.click(80.0F, generator_row(3)).add ==
           editor::LibraryAddAction::Background);
  UI_CHECK(harness.click(80.0F, generator_row(4)).add ==
           editor::LibraryAddAction::Camera);
  UI_CHECK(harness.click(80.0F, generator_row(7)).add ==
           editor::LibraryAddAction::SpotLight);
  // Below the last generator there is nothing to add.
  UI_CHECK(!harness.click(80.0F, after_generators + 10.0F).add.has_value());
}

void a_surface_generator_drags_out_to_stage() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 260.0F, 320.0F};
  // Carried past the threshold, the Background is reported as dragging,
  // and let go outside the Library it is dropped where it was let go - not
  // also added as a click.
  const auto [held, released] =
      harness.drag(80.0F, generator_row(3), 420.0F, 150.0F);
  UI_CHECK(held.dragging_generator == editor::LibraryAddAction::Background);
  UI_CHECK(!held.generator_drop.has_value());
  UI_CHECK(released.generator_drop.has_value());
  UI_CHECK(released.generator_drop->action ==
           editor::LibraryAddAction::Background);
  UI_CHECK(released.generator_drop->screen_x == 420.0F);
  UI_CHECK(released.generator_drop->screen_y == 150.0F);
  UI_CHECK(!released.add.has_value());
  UI_CHECK(!released.dragging_generator.has_value());

  // Let go back over the Library it is nothing at all.
  const auto [unused, back] =
      harness.drag(80.0F, generator_row(0), 120.0F, generator_row(5));
  static_cast<void>(unused);
  UI_CHECK(!back.generator_drop.has_value());
  UI_CHECK(!back.add.has_value());

  // A light is placed where it is added, so it never starts a drag.
  const auto [light, light_released] =
      harness.drag(80.0F, generator_row(5), 420.0F, 150.0F);
  UI_CHECK(!light.dragging_generator.has_value());
  UI_CHECK(!light_released.generator_drop.has_value());
}

void the_generators_section_collapses() {
  Harness harness;
  harness.collapse_generators();
  // The first row after the heading is now empty space, not Box.
  UI_CHECK(!harness.click(80.0F, generator_row(0)).add.has_value());
}

// ---------------------------------------------------------------- selection

void a_click_selects_and_a_double_click_opens_the_viewer() {
  Harness harness;
  harness.registry = project_assets();

  const editor::LibraryFrame first = harness.click(120.0F, opening_plate_y);
  UI_CHECK(!first.view_asset.has_value());
  UI_CHECK(harness.panel.selection().size() == 1U);
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{20U});

  // The test clock stands still, so the next click is a double-click.
  // It opens the asset in the Asset Viewer. Nothing is placed: an asset
  // reaches a scene only by being dragged there.
  const editor::LibraryFrame second = harness.click(120.0F, opening_plate_y);
  UI_CHECK(second.view_asset == doc::NodeId{20U});
  UI_CHECK(!second.add.has_value() && !second.import_asset.has_value());
  UI_CHECK(!second.asset_drop.has_value());

  // Enter opens the selection's asset the same way.
  const editor::LibraryFrame entered =
      harness.frame(-1.0F, -1.0F, false, gui::KeyMods::None, {}, 0.0F,
                    gui::Key::Enter);
  UI_CHECK(entered.view_asset == doc::NodeId{20U});
}

void shift_extends_and_ctrl_toggles_the_selection() {
  Harness harness;
  harness.registry = project_assets();
  static_cast<void>(harness.click(120.0F, hero_car_y));
  static_cast<void>(
      harness.click(120.0F, opening_plate_y, gui::KeyMods::Shift));
  UI_CHECK(harness.panel.selection().size() == 2U);

  static_cast<void>(harness.click(120.0F, hero_car_y, gui::KeyMods::Ctrl));
  UI_CHECK(harness.panel.selection().size() == 1U);
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{20U});
}

void search_filters_assets_by_name_and_metadata() {
  Harness harness;
  harness.registry = project_assets();
  static_cast<void>(harness.click(70.0F, 37.0F));
  static_cast<void>(
      harness.frame(70.0F, 37.0F, false, gui::KeyMods::None, "car"));
  UI_CHECK(harness.panel.search() == "car");

  // Only Hero Car is left, in the first content row.
  static_cast<void>(harness.click(120.0F, content_top + 16.0F));
  const editor::LibraryFrame placed =
      harness.click(120.0F, content_top + 16.0F);
  UI_CHECK(placed.view_asset == doc::NodeId{30U});

  // A resolution is something to search for too.
  Harness resolution;
  resolution.registry = project_assets();
  static_cast<void>(resolution.click(70.0F, 37.0F));
  static_cast<void>(
      resolution.frame(70.0F, 37.0F, false, gui::KeyMods::None, "3840"));
  static_cast<void>(resolution.click(120.0F, content_top + 16.0F));
  UI_CHECK(resolution.click(120.0F, content_top + 16.0F).view_asset ==
           doc::NodeId{20U});
}

// ------------------------------------------------------------------ columns

void a_header_click_sorts_by_that_column() {
  Harness harness;
  harness.registry = project_assets();
  UI_CHECK(harness.panel.sort_column() == editor::LibraryColumn::Name);
  // Name again: descending, so Opening Plate comes first.
  static_cast<void>(harness.click(60.0F, 60.0F));
  UI_CHECK(harness.panel.sort_descending());
  static_cast<void>(harness.click(120.0F, hero_car_y));
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{20U});

  // Resolution (x=294..394): the model has none, so it sorts last.
  static_cast<void>(harness.click(330.0F, 60.0F));
  UI_CHECK(harness.panel.sort_column() == editor::LibraryColumn::Resolution);
  UI_CHECK(!harness.panel.sort_descending());
  static_cast<void>(harness.click(120.0F, opening_plate_y));
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{30U});
}

void a_header_edge_drag_resizes_its_column() {
  Harness harness;
  harness.registry = project_assets();
  // Widen Name from 220 to 300: Resolution's header moves from 294..394 to
  // 374..474, so a click at 330 now lands on Type.
  static_cast<void>(harness.drag(220.0F, 60.0F, 300.0F, 60.0F));
  static_cast<void>(harness.click(330.0F, 60.0F));
  UI_CHECK(harness.panel.sort_column() == editor::LibraryColumn::Type);
}

// The columns are wider than the panel, so a horizontal scrollbar takes its
// own 10-unit strip under the rows (488..498). Dragging its thumb scrolls
// the columns and the header together, and a press there never reaches a
// row beneath it.
void the_horizontal_scrollbar_drags_without_touching_rows() {
  Harness harness;
  harness.registry = project_assets();
  // Fill the list down to the bar, so a row would be there if the bar
  // were drawn over the rows.
  for (core::u64 index = 0U; index < 8U; ++index) {
    assets::MediaAsset extra{};
    extra.id = doc::NodeId{200U + index};
    extra.display_name = "Zz " + std::to_string(index);
    extra.source = "C:/show/zz_" + std::to_string(index) + ".mov";
    harness.registry.assets.emplace_back(std::move(extra));
  }
  const auto [held, released] = harness.drag(5.0F, 493.0F, 300.0F, 493.0F);
  UI_CHECK(!held.dragging_asset.has_value());
  UI_CHECK(released == editor::LibraryFrame{});
  UI_CHECK(harness.panel.selection().empty());
  // Scrolled, x=60 in the header is no longer Name.
  static_cast<void>(harness.click(60.0F, 60.0F));
  UI_CHECK(harness.panel.sort_column() != editor::LibraryColumn::Name);
}

void the_header_menu_shows_and_hides_columns() {
  Harness harness;
  harness.registry = project_assets();
  UI_CHECK(!harness.panel.column_visible(editor::LibraryColumn::Frames));
  UI_CHECK(harness.panel.column_visible(editor::LibraryColumn::Codec));
  // One entry per column after Name, in column order: Frames is the 7th.
  static_cast<void>(harness.right_click(60.0F, 60.0F));
  static_cast<void>(harness.choose(60.0F, 60.0F, 6U));
  UI_CHECK(harness.panel.column_visible(editor::LibraryColumn::Frames));
  static_cast<void>(harness.right_click(60.0F, 60.0F));
  static_cast<void>(harness.choose(60.0F, 60.0F, 7U));
  UI_CHECK(!harness.panel.column_visible(editor::LibraryColumn::Codec));
}

void columns_read_authored_metadata() {
  assets::MediaAsset video{};
  video.id = doc::NodeId{1U};
  video.display_name = "Interview A";
  video.source = "C:/show/interview_a.mov";
  video.dimensions = assets::PixelDimensions{3840U, 2160U};
  video.frame_count = 250U;
  video.frame_rate = assets::MediaFrameRate{25U, 1U};
  video.codec = "prores";
  video.start_timecode = "10:00:00:00";
  video.fingerprint = assets::ContentFingerprint{1'500'000'000U, {}};
  const assets::Asset media{video};
  const auto text = [](const assets::Asset &asset, editor::LibraryColumn c) {
    return editor::library_column_text(asset, c);
  };
  UI_CHECK(text(media, editor::LibraryColumn::Type) == "Video");
  UI_CHECK(text(media, editor::LibraryColumn::Resolution) == "3840 x 2160");
  UI_CHECK(text(media, editor::LibraryColumn::FrameRate) == "25");
  UI_CHECK(text(media, editor::LibraryColumn::Duration) == "00:00:10:00");
  UI_CHECK(text(media, editor::LibraryColumn::StartTimecode) ==
           "10:00:00:00");
  // The last frame, as an editor's Media End reads.
  UI_CHECK(text(media, editor::LibraryColumn::EndTimecode) == "10:00:09:24");
  UI_CHECK(text(media, editor::LibraryColumn::Frames) == "250");
  UI_CHECK(text(media, editor::LibraryColumn::Codec) == "PRORES");
  UI_CHECK(text(media, editor::LibraryColumn::FileSize) == "1.40 GB");
  UI_CHECK(text(media, editor::LibraryColumn::Proxy) == "SRC");
  UI_CHECK(text(media, editor::LibraryColumn::Vertices).empty());

  // A file with no timecode of its own starts at zero.
  video.start_timecode.clear();
  video.frame_rate = assets::MediaFrameRate{24'000U, 1'001U};
  const assets::Asset untimed{video};
  UI_CHECK(text(untimed, editor::LibraryColumn::FrameRate) == "23.976");
  UI_CHECK(text(untimed, editor::LibraryColumn::StartTimecode) ==
           "00:00:00:00");
  UI_CHECK(text(untimed, editor::LibraryColumn::EndTimecode) ==
           "00:00:10:09");

  assets::MediaAsset still{};
  still.id = doc::NodeId{2U};
  still.display_name = "Logo";
  still.source = "C:/show/logo.png";
  still.dimensions = assets::PixelDimensions{512U, 256U};
  still.frame_count = 1U;
  const assets::Asset image{still};
  UI_CHECK(text(image, editor::LibraryColumn::Type) == "Still");
  UI_CHECK(text(image, editor::LibraryColumn::Duration).empty());
  UI_CHECK(text(image, editor::LibraryColumn::StartTimecode).empty());

  assets::ModelAsset car{};
  car.id = doc::NodeId{3U};
  car.display_name = "Car";
  car.source = "C:/show/car.glb";
  car.vertex_count = 64'000U;
  car.triangle_count = 1'250'000U;
  car.material_count = 7U;
  car.texture_count = 12U;
  car.largest_texture = assets::PixelDimensions{4096U, 2048U};
  const assets::Asset model{car};
  UI_CHECK(text(model, editor::LibraryColumn::Type) == "3D Model");
  UI_CHECK(text(model, editor::LibraryColumn::Vertices) == "64,000");
  UI_CHECK(text(model, editor::LibraryColumn::Triangles) == "1,250,000");
  UI_CHECK(text(model, editor::LibraryColumn::Materials) == "7");
  UI_CHECK(text(model, editor::LibraryColumn::Textures) == "12");
  UI_CHECK(text(model, editor::LibraryColumn::TextureSize) == "4096 x 2048");
  UI_CHECK(text(model, editor::LibraryColumn::Resolution).empty());
  UI_CHECK(text(model, editor::LibraryColumn::Proxy).empty());

  assets::ParticleEffectAsset embers{};
  embers.id = doc::NodeId{4U};
  embers.display_name = "Embers";
  embers.source = "C:/show/embers.ajfx";
  const assets::Asset effect{embers};
  UI_CHECK(text(effect, editor::LibraryColumn::Type) == "Particle effect");
  UI_CHECK(text(effect, editor::LibraryColumn::Name) == "Embers");
  UI_CHECK(text(effect, editor::LibraryColumn::Triangles).empty());
  UI_CHECK(text(effect, editor::LibraryColumn::Resolution).empty());
}

void drop_frame_timecode_counts_like_an_editor() {
  const assets::MediaFrameRate ntsc{30'000U, 1'001U};
  UI_CHECK(editor::timecode_frames("01:00:00;00", ntsc) == 107'892);
  UI_CHECK(editor::timecode_text(107'892, ntsc, true) == "01:00:00;00");
  // Frames ;00 and ;01 do not exist at the top of a minute...
  UI_CHECK(editor::timecode_text(1'799, ntsc, true) == "00:00:59;29");
  UI_CHECK(editor::timecode_text(1'800, ntsc, true) == "00:01:00;02");
  // ...except every tenth minute.
  UI_CHECK(editor::timecode_text(17'982, ntsc, true) == "00:10:00;00");
  // The same count without drop-frame.
  UI_CHECK(editor::timecode_text(1'800, ntsc, false) == "00:01:00:00");
  UI_CHECK(editor::timecode_frames("00:01:00:00", ntsc) == 1'800);
  // A drop-frame mark at a rate that has none counts every frame.
  const assets::MediaFrameRate film{24U, 1U};
  UI_CHECK(editor::timecode_frames("00:00:01;00", film) == 24);
  UI_CHECK(!editor::timecode_frames("00:00:01:24", film).has_value());
  UI_CHECK(!editor::timecode_frames("not a timecode", film).has_value());
}

void files_import_by_extension() {
  UI_CHECK(editor::library_import_kind("C:/shoot/A001.MOV") ==
           editor::LibraryImportAction::Media);
  UI_CHECK(editor::library_import_kind("C:/shoot/plate.exr") ==
           editor::LibraryImportAction::Media);
  UI_CHECK(editor::library_import_kind("C:/models/car.glb") ==
           editor::LibraryImportAction::Model);
  UI_CHECK(editor::library_import_kind("C:/fx/Embers.AJFX") ==
           editor::LibraryImportAction::ParticleEffect);
  UI_CHECK(!editor::library_import_kind("C:/notes.txt").has_value());
  UI_CHECK(!editor::library_import_kind("C:/takes.v2/readme").has_value());
}

// ------------------------------------------------------------ proxy, status

void the_proxy_chip_cycles_its_policy_without_placing() {
  Harness harness;
  // Wide enough to show every default column: Proxy's chip is 1144..1188.
  harness.bounds = gui::Rect{0.0F, 0.0F, 1400.0F, 520.0F};
  harness.registry = project_assets();
  auto *const media =
      std::get_if<assets::MediaAsset>(&harness.registry.assets[0U]);
  UI_CHECK(media != nullptr);

  editor::LibraryFrame result = harness.click(1164.0F, opening_plate_y);
  UI_CHECK(result.proxy_policy.has_value());
  UI_CHECK(result.proxy_policy->asset == media->id);
  UI_CHECK(result.proxy_policy->policy ==
           assets::ProxyPlaybackPolicy::PreferProxy);
  UI_CHECK(result.proxy_policy->requested_profile ==
           assets::DerivativeProfileId::Proxy720pCfrH264);

  media->proxy_playback_policy = assets::ProxyPlaybackPolicy::PreferProxy;
  media->requested_proxy_profile =
      assets::DerivativeProfileId::Proxy720pCfrH264;
  // A second click at once is no double-click: the chip is its own control.
  result = harness.click(1164.0F, opening_plate_y);
  UI_CHECK(!result.view_asset.has_value());
  UI_CHECK(result.proxy_policy->policy ==
           assets::ProxyPlaybackPolicy::RequireProxy);

  media->proxy_playback_policy = assets::ProxyPlaybackPolicy::RequireProxy;
  result = harness.click(1164.0F, opening_plate_y);
  UI_CHECK(result.proxy_policy->policy == assets::ProxyPlaybackPolicy::Source);
  UI_CHECK(!result.proxy_policy->requested_profile.has_value());
  UI_CHECK(harness.panel.selection().empty());
}

void cpu_only_media_asks_for_a_full_resolution_proxy_and_warns() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 1400.0F, 520.0F};
  harness.registry = project_assets();
  harness.runtime.push_back(editor::LibraryProxyRuntimeObservation{
      doc::NodeId{20U}, std::nullopt, editor::LibraryProxyRuntimeState::Source,
      assets::ProxyPlaybackReason::SourcePolicy,
      editor::LibraryProxyRuntimeIssue::None, std::nullopt,
      editor::LibraryMediaDecodeState::CpuOnly});

  const editor::LibraryFrame result = harness.click(1164.0F, opening_plate_y);
  // The warning leads the Status cell, which starts at x=1198.
  const bool red_warning = std::any_of(
      harness.list.vertices().begin(), harness.list.vertices().end(),
      [](const gui::UiVertex &vertex) {
        return vertex.x >= 1196.0F && vertex.x <= 1212.0F &&
               vertex.y >= opening_plate_y - 8.0F &&
               vertex.y <= opening_plate_y + 8.0F && vertex.r > 0.8F &&
               vertex.g < 0.6F;
      });
  UI_CHECK(red_warning);
  UI_CHECK(result.proxy_policy.has_value());
  UI_CHECK(result.proxy_policy->requested_profile ==
           assets::DerivativeProfileId::ProxySourceCfrHevcMain10);
}

void proxy_runtime_status_labels_explain_resolution_and_failure() {
  editor::LibraryProxyRuntimeObservation observation{};
  observation.asset = doc::NodeId{100U};
  UI_CHECK(editor::library_proxy_runtime_status(observation) == "SOURCE");

  observation.state = editor::LibraryProxyRuntimeState::ProxyReady;
  observation.reason = assets::ProxyPlaybackReason::None;
  UI_CHECK(editor::library_proxy_runtime_status(observation) == "PROXY READY");

  observation.state = editor::LibraryProxyRuntimeState::PreferFallback;
  observation.reason = assets::ProxyPlaybackReason::CacheGenerating;
  UI_CHECK(editor::library_proxy_runtime_status(observation) ==
           "SOURCE (PROXY GENERATING)");

  observation.transcode_progress =
      editor::LibraryProxyTranscodeProgress{42U, 100U, false};
  UI_CHECK(editor::library_proxy_runtime_status(observation) ==
           "TRANSCODING PROXY 42%");
  observation.transcode_progress->queued = true;
  UI_CHECK(editor::library_proxy_runtime_status(observation) == "PROXY QUEUED");
  observation.transcode_progress.reset();

  observation.state = editor::LibraryProxyRuntimeState::RequiredUnready;
  observation.reason = assets::ProxyPlaybackReason::CacheMissing;
  UI_CHECK(editor::library_proxy_runtime_status(observation) ==
           "REQUIRED - UNREADY (MISSING)");

  observation.state = editor::LibraryProxyRuntimeState::Failed;
  observation.reason = assets::ProxyPlaybackReason::FrameMapInvalid;
  observation.issue = editor::LibraryProxyRuntimeIssue::FrameMappingFailed;
  UI_CHECK(editor::library_proxy_runtime_status(observation) ==
           "PROXY FAILED (FRAME MAPPING FAILED)");
}

// The editor's own proxy reads in the operator's words, rounding down so
// 100% only ever means made.
void editor_proxy_status_says_how_much_is_made() {
  using State = editor::LibraryEditorProxyState;
  editor::LibraryEditorProxy proxy{State::Queued, 0U, 2'928U};
  UI_CHECK(editor::library_editor_proxy_status(proxy) ==
           "Editor proxy queued");
  UI_CHECK(editor::library_editor_proxy_fraction(proxy) == 0.0F);
  proxy = {State::Making, 1'171U, 2'928U};
  UI_CHECK(editor::library_editor_proxy_status(proxy) ==
           "Editor proxy 39%");
  proxy.made_frames = 2'927U;
  UI_CHECK(editor::library_editor_proxy_status(proxy) ==
           "Editor proxy 99%");
  proxy = {State::Making, 5U, 0U};
  UI_CHECK(editor::library_editor_proxy_status(proxy) ==
           "Editor proxy 0%");
  proxy = {State::Ready, 0U, 0U};
  UI_CHECK(editor::library_editor_proxy_status(proxy) == "Editor proxy ready");
  UI_CHECK(editor::library_editor_proxy_fraction(proxy) == 1.0F);
  proxy.state = State::Failed;
  UI_CHECK(editor::library_editor_proxy_status(proxy) ==
           "Editor proxy failed");
}

// ---------------------------------------------------------- changed sources

// After a RESCAN finds files that changed on disk, the Library says so in its
// title row - a chip that accepts them all - and on each asset's menu.
void changed_sources_are_offered_for_acceptance() {
  Harness harness;
  harness.registry = project_assets();
  const std::array<doc::NodeId, 2U> changed{
      {doc::NodeId{20U}, doc::NodeId{999U}}};
  harness.panel.set_changed_sources(changed);

  // The chip leads the title row (6..22 with the empty font). An id that
  // is no longer in the Library is not offered.
  const editor::LibraryFrame all = harness.click(12.0F, 12.0F);
  UI_CHECK(all.accept_sources == std::vector<doc::NodeId>{doc::NodeId{20U}});

  // Its menu leads with Accept Changed Source.
  static_cast<void>(harness.right_click(60.0F, opening_plate_y));
  const editor::LibraryFrame one = harness.choose(60.0F, opening_plate_y, 0U);
  UI_CHECK(one.accept_sources == std::vector<doc::NodeId>{doc::NodeId{20U}});
  UI_CHECK(!one.view_asset.has_value());

  // An unchanged asset's menu has no such entry: its first is Open in Asset
  // Viewer. There is no Add to Scene; dragging out places an asset.
  static_cast<void>(harness.right_click(60.0F, hero_car_y));
  const editor::LibraryFrame viewed = harness.choose(60.0F, hero_car_y, 0U);
  UI_CHECK(viewed.accept_sources.empty());
  UI_CHECK(viewed.view_asset == doc::NodeId{30U});

  // Nothing changed, nothing offered: the title row is inert.
  harness.panel.set_changed_sources({});
  UI_CHECK(harness.click(12.0F, 12.0F).accept_sources.empty());
}

// ------------------------------------------------------------ drag and drop

void a_drag_out_carries_the_whole_selection_under_a_thumbnail() {
  Harness harness;
  harness.registry = project_assets();
  static_cast<void>(harness.click(120.0F, hero_car_y));
  static_cast<void>(
      harness.click(120.0F, opening_plate_y, gui::KeyMods::Shift));

  static_cast<void>(harness.frame(120.0F, opening_plate_y, false));
  static_cast<void>(harness.frame(120.0F, opening_plate_y, true));
  const editor::LibraryFrame held = harness.frame(900.0F, 250.0F, true);
  UI_CHECK(held.dragging_asset == doc::NodeId{20U});
  UI_CHECK(!held.view_asset.has_value());
  // The ghost is drawn behind the pointer, above everything else.
  const std::size_t before = harness.list.vertices().size();
  harness.panel.draw_drag_ghost(harness.ctx);
  const bool ghost = std::any_of(
      harness.list.vertices().begin() + static_cast<std::ptrdiff_t>(before),
      harness.list.vertices().end(), [](const gui::UiVertex &vertex) {
        return vertex.x >= 914.0F && vertex.y >= 264.0F;
      });
  UI_CHECK(ghost);

  const editor::LibraryFrame dropped = harness.frame(900.0F, 250.0F, false);
  UI_CHECK(dropped.asset_drop.has_value());
  UI_CHECK(dropped.asset_drop->asset == doc::NodeId{20U});
  UI_CHECK(dropped.asset_drop->screen_x == 900.0F);
  UI_CHECK(dropped.asset_drop->screen_y == 250.0F);
  UI_CHECK((dropped.asset_drop->assets ==
            std::vector<doc::NodeId>{doc::NodeId{20U}, doc::NodeId{30U}}));
  UI_CHECK(dropped.library_edits.empty());
  // Nothing rides the pointer once it is let go.
  const std::size_t after = harness.list.vertices().size();
  harness.panel.draw_drag_ghost(harness.ctx);
  UI_CHECK(harness.list.vertices().size() == after);
}

[[nodiscard]] assets::AssetRegistry with_folder() {
  assets::AssetRegistry registry = project_assets();
  registry.folders.push_back(
      assets::AssetFolder{doc::NodeId{40U}, "Plates", doc::NodeId{}});
  return registry;
}

// With a folder at the top level, the List reads: Plates (22 units), a gap,
// then Hero Car and Opening Plate.
constexpr float plates_y = first_group + 11.0F;
constexpr float foldered_hero_y = first_group + 22.0F + 10.0F + 16.0F;
constexpr float foldered_plate_y = first_group + 22.0F + 10.0F + 49.0F;

void dragging_onto_a_folder_files_the_asset_there() {
  Harness harness;
  harness.registry = with_folder();
  const auto [held, dropped] =
      harness.drag(120.0F, foldered_plate_y, 120.0F, plates_y);
  UI_CHECK(!held.dragging_asset.has_value() ||
           held.dragging_asset == doc::NodeId{20U});
  UI_CHECK(dropped.library_edits.size() == 1U);
  const assets::LibraryEdit &edit = dropped.library_edits.front();
  UI_CHECK(edit.kind == assets::LibraryEdit::Kind::Move);
  UI_CHECK(edit.destination == doc::NodeId{40U});
  UI_CHECK(edit.items == std::vector<doc::NodeId>{doc::NodeId{20U}});
  UI_CHECK(!dropped.asset_drop.has_value());
}

void a_folder_opens_on_its_twirl_and_files_back_to_the_top() {
  Harness harness;
  harness.registry = with_folder();
  std::get<assets::MediaAsset>(harness.registry.assets[0U]).folder =
      doc::NodeId{40U};
  // Closed, the folder hides Opening Plate: only Hero Car follows it.
  static_cast<void>(harness.click(13.0F, plates_y));
  // Open: Plates, then Opening Plate inside it, then a gap and Hero Car.
  const float inside_y = first_group + 22.0F + 16.0F;
  static_cast<void>(harness.click(120.0F, inside_y));
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{20U});
  // Let go on empty space below the list: back to the top level.
  const auto [held, dropped] = harness.drag(120.0F, inside_y, 120.0F, 470.0F);
  static_cast<void>(held);
  UI_CHECK(dropped.library_edits.size() == 1U);
  UI_CHECK(dropped.library_edits.front().destination == doc::NodeId{});
  UI_CHECK(dropped.library_edits.front().items ==
           std::vector<doc::NodeId>{doc::NodeId{20U}});
}

void a_folder_is_never_filed_inside_itself() {
  Harness harness;
  harness.registry = with_folder();
  const auto [held, dropped] = harness.drag(120.0F, plates_y, 120.0F,
                                            plates_y + 4.0F);
  static_cast<void>(held);
  UI_CHECK(dropped.library_edits.empty());
}

void files_from_the_desktop_import_into_the_folder_under_them() {
  Harness harness;
  harness.registry = with_folder();
  harness.input.new_frame();
  harness.input.set_mouse_position(-1.0F, -1.0F);
  harness.input.add_dropped_file("C:/shoot/A001.mov", 120.0F, plates_y);
  harness.input.add_dropped_file("C:/shoot/A002.mov", 120.0F, plates_y);
  harness.ctx.new_frame();
  const editor::LibraryFrame into =
      harness.panel.draw(harness.ctx, harness.bounds, harness.registry);
  UI_CHECK((into.import_files ==
            std::vector<std::string>{"C:/shoot/A001.mov", "C:/shoot/A002.mov"}));
  UI_CHECK(into.import_folder == doc::NodeId{40U});

  harness.input.new_frame();
  harness.input.add_dropped_file("C:/shoot/A003.mov", 120.0F, 470.0F);
  harness.ctx.new_frame();
  const editor::LibraryFrame top =
      harness.panel.draw(harness.ctx, harness.bounds, harness.registry);
  UI_CHECK(top.import_files.size() == 1U);
  UI_CHECK(top.import_folder == doc::NodeId{});

  // Let go outside the Library, they are not the Library's to import.
  harness.input.new_frame();
  harness.input.add_dropped_file("C:/shoot/A004.mov", 900.0F, 200.0F);
  harness.ctx.new_frame();
  UI_CHECK(harness.panel.draw(harness.ctx, harness.bounds, harness.registry)
               .import_files.empty());
}

// ------------------------------------------------------------------ folders

void new_folder_is_created_then_named_in_place() {
  Harness harness;
  harness.registry = project_assets();
  const editor::LibraryFrame created = harness.click(220.0F, 37.0F);
  UI_CHECK(created.library_edits.size() == 1U);
  const assets::LibraryEdit &edit = created.library_edits.front();
  UI_CHECK(edit.kind == assets::LibraryEdit::Kind::CreateFolder);
  UI_CHECK(edit.name == "New Folder");
  UI_CHECK(edit.destination == doc::NodeId{});

  // The Director files it with an id of its own; the Library selects it and
  // opens its name for typing.
  harness.registry.folders.push_back(
      assets::AssetFolder{doc::NodeId{41U}, "New Folder", doc::NodeId{}});
  static_cast<void>(harness.frame(-1.0F, -1.0F, false));
  UI_CHECK(harness.panel.selection().size() == 1U);
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{41U});
  static_cast<void>(
      harness.frame(-1.0F, -1.0F, false, gui::KeyMods::None, "Shots"));
  const editor::LibraryFrame named =
      harness.frame(-1.0F, -1.0F, false, gui::KeyMods::None, {}, 0.0F,
                    gui::Key::Enter);
  UI_CHECK(named.library_edits.size() == 1U);
  UI_CHECK(named.library_edits.front().kind ==
           assets::LibraryEdit::Kind::Rename);
  UI_CHECK(named.library_edits.front().target == doc::NodeId{41U});
  UI_CHECK(named.library_edits.front().name == "Shots");
}

void delete_removes_selected_assets_and_folders() {
  Harness harness;
  harness.registry = with_folder();
  static_cast<void>(harness.click(120.0F, plates_y));
  static_cast<void>(
      harness.click(120.0F, foldered_plate_y, gui::KeyMods::Ctrl));
  const editor::LibraryFrame removed = harness.key(gui::Key::Delete);
  UI_CHECK(removed.consume_delete);
  UI_CHECK(removed.remove_assets == std::vector<doc::NodeId>{doc::NodeId{20U}});
  UI_CHECK(removed.library_edits.size() == 1U);
  UI_CHECK(removed.library_edits.front().kind ==
           assets::LibraryEdit::Kind::DeleteFolder);
  UI_CHECK(removed.library_edits.front().target == doc::NodeId{40U});

  // Delete is the Library's only while it was the last panel clicked.
  static_cast<void>(harness.click(120.0F, foldered_plate_y));
  static_cast<void>(harness.click(900.0F, 300.0F));
  UI_CHECK(!harness.key(gui::Key::Delete).consume_delete);
}

void an_asset_menu_removes_it_from_the_library() {
  Harness harness;
  harness.registry = project_assets();
  static_cast<void>(harness.right_click(60.0F, opening_plate_y));
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{20U});
  // Add to Scene, Rename, New Folder, Remove from Library.
  const editor::LibraryFrame removed =
      harness.choose(60.0F, opening_plate_y, 3U);
  UI_CHECK(removed.remove_assets == std::vector<doc::NodeId>{doc::NodeId{20U}});
}

// ---------------------------------------------------------------- grid view

void the_grid_view_browses_one_folder_at_a_time() {
  Harness harness;
  harness.registry = with_folder();
  harness.panel.set_view(editor::LibraryView::Grid);
  // Tiles are 142 wide and 116 tall, two to a row 8 apart from (8, 78):
  // Generators and Plates; then, on a row of their own 10 further down
  // (212..328), Hero Car and Opening Plate.
  static_cast<void>(harness.click(229.0F, 130.0F));
  static_cast<void>(harness.click(229.0F, 130.0F));
  UI_CHECK(harness.panel.folder() == doc::NodeId{40U});

  // Inside, the path reads Library > Plates; Library is 6..16.
  static_cast<void>(harness.click(10.0F, 60.0F));
  UI_CHECK(harness.panel.folder() == doc::NodeId{});

  // A tile dragged onto a folder tile is filed there.
  const auto [held, dropped] = harness.drag(229.0F, 250.0F, 229.0F, 130.0F);
  static_cast<void>(held);
  UI_CHECK(dropped.library_edits.size() == 1U);
  UI_CHECK(dropped.library_edits.front().destination == doc::NodeId{40U});
  UI_CHECK(dropped.library_edits.front().items ==
           std::vector<doc::NodeId>{doc::NodeId{20U}});
}

void the_grid_view_files_back_up_through_its_path() {
  Harness harness;
  harness.registry = with_folder();
  std::get<assets::MediaAsset>(harness.registry.assets[0U]).folder =
      doc::NodeId{40U};
  harness.panel.set_view(editor::LibraryView::Grid);
  harness.panel.open_folder(doc::NodeId{40U});
  // Opening Plate is the only tile; drag it onto "Library" in the path.
  const auto [held, dropped] = harness.drag(79.0F, 120.0F, 10.0F, 60.0F);
  static_cast<void>(held);
  UI_CHECK(dropped.library_edits.size() == 1U);
  UI_CHECK(dropped.library_edits.front().destination == doc::NodeId{});
}

void the_size_slider_and_ctrl_wheel_resize_thumbnails() {
  Harness harness;
  harness.registry = project_assets();
  const float list_size = harness.panel.icon_size();
  static_cast<void>(harness.frame(120.0F, 300.0F, false, gui::KeyMods::Ctrl,
                                  {}, 2.0F));
  UI_CHECK(harness.panel.icon_size() > list_size);

  // The view switch is in the footer; each view keeps its own size.
  static_cast<void>(harness.click(42.0F, 509.0F));
  UI_CHECK(harness.panel.view() == editor::LibraryView::Grid);
  const float grid_size = harness.panel.icon_size();
  // The slider runs 74..184: its right end is the largest size.
  static_cast<void>(harness.drag(100.0F, 509.0F, 184.0F, 509.0F));
  UI_CHECK(harness.panel.icon_size() > grid_size);
  static_cast<void>(harness.click(16.0F, 509.0F));
  UI_CHECK(harness.panel.view() == editor::LibraryView::List);
  UI_CHECK(harness.panel.icon_size() > list_size);
}

// --------------------------------------------------------------- thumbnails

// 300x240: the content is 70..218. With the generators closed, asset rows
// start at 92 and four of them are at least partly in view.
void thumbnails_request_visible_rows_then_a_bounded_prefetch() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 240.0F};
  harness.registry = fingerprinted_media_assets(8U);
  harness.thumbnail_aware = true;
  harness.window = editor::LibraryThumbnailWindow{2U, 8U};
  harness.collapse_generators();

  const editor::LibraryFrame result = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(result.thumbnail_retain.has_value());
  UI_CHECK(result.thumbnail_retain->keys.size() == 6U);
  UI_CHECK(result.thumbnail_requests.size() == 6U);
  for (std::size_t index = 0U; index < 6U; ++index) {
    UI_CHECK(result.thumbnail_requests[index].key ==
             thumbnail_key(harness.registry, index));
    UI_CHECK(result.thumbnail_requests[index].priority ==
             (index < 4U ? editor::LibraryThumbnailRequestPriority::Visible
                         : editor::LibraryThumbnailRequestPriority::Prefetch));
  }
}

void thumbnail_observations_suppress_duplicate_requests() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 240.0F};
  harness.registry = fingerprinted_media_assets(6U);
  harness.thumbnail_aware = true;
  constexpr std::array<editor::LibraryThumbnailState, 6U> states{{
      editor::LibraryThumbnailState::Queued,
      editor::LibraryThumbnailState::Generating,
      editor::LibraryThumbnailState::Ready,
      editor::LibraryThumbnailState::Stale,
      editor::LibraryThumbnailState::Failed,
      editor::LibraryThumbnailState::Placeholder,
  }};
  for (std::size_t index = 0U; index < states.size(); ++index) {
    harness.thumbnails.push_back(editor::LibraryThumbnailObservation{
        thumbnail_key(harness.registry, index), states[index], nullptr});
  }
  harness.collapse_generators();

  const editor::LibraryFrame result = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(result.thumbnail_retain->keys.size() == states.size());
  UI_CHECK(result.thumbnail_requests.size() == 2U);
  UI_CHECK(result.thumbnail_requests[0U].key ==
           thumbnail_key(harness.registry, 3U));
  UI_CHECK(result.thumbnail_requests[0U].priority ==
           editor::LibraryThumbnailRequestPriority::Visible);
  UI_CHECK(result.thumbnail_requests[1U].key ==
           thumbnail_key(harness.registry, 5U));
  UI_CHECK(result.thumbnail_requests[1U].priority ==
           editor::LibraryThumbnailRequestPriority::Prefetch);
}

void thumbnail_observations_must_match_the_exact_content_revision() {
  Harness harness;
  harness.registry = fingerprinted_media_assets(1U);
  harness.thumbnail_aware = true;
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Thumbnail320x180Srgb);
  UI_CHECK(profile != nullptr);
  const assets::DerivativeKey wrong_revision = assets::make_derivative_key(
      doc::NodeId{100U}, test_fingerprint(999U), *profile);
  harness.thumbnails.push_back(editor::LibraryThumbnailObservation{
      wrong_revision, editor::LibraryThumbnailState::Queued, nullptr});

  const editor::LibraryFrame result = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(result.thumbnail_retain->keys ==
           std::vector<assets::DerivativeKey>{
               thumbnail_key(harness.registry, 0U)});
  UI_CHECK(result.thumbnail_requests.size() == 1U);
  UI_CHECK(result.thumbnail_requests[0U].key != wrong_revision);
}

void scrolling_replaces_the_thumbnail_retain_set() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 240.0F};
  harness.registry = fingerprinted_media_assets(12U);
  harness.thumbnail_aware = true;
  harness.window = editor::LibraryThumbnailWindow{2U, 16U};
  harness.collapse_generators();

  const editor::LibraryFrame first = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(contains(first.thumbnail_retain->keys,
                    thumbnail_key(harness.registry, 0U)));
  static_cast<void>(harness.frame(120.0F, 150.0F, false, gui::KeyMods::None,
                                  {}, -100.0F));
  const editor::LibraryFrame scrolled = harness.frame(120.0F, 150.0F, false);
  UI_CHECK(!contains(scrolled.thumbnail_retain->keys,
                     thumbnail_key(harness.registry, 0U)));
  UI_CHECK(contains(scrolled.thumbnail_retain->keys,
                    thumbnail_key(harness.registry, 11U)));
}

void thumbnail_identity_is_never_invented_for_unfingerprinted_assets() {
  Harness harness;
  harness.registry = project_assets();
  harness.thumbnail_aware = true;
  const editor::LibraryFrame aware = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(aware.thumbnail_retain.has_value());
  UI_CHECK(aware.thumbnail_retain->keys.empty());
  UI_CHECK(aware.thumbnail_requests.empty());

  harness.thumbnail_aware = false;
  const editor::LibraryFrame plain = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(!plain.thumbnail_retain.has_value());
}

void the_grid_view_requests_thumbnails_for_its_tiles() {
  Harness harness;
  harness.registry = fingerprinted_media_assets(3U);
  harness.thumbnail_aware = true;
  harness.panel.set_view(editor::LibraryView::Grid);
  const editor::LibraryFrame result = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(result.thumbnail_retain->keys.size() == 3U);
  UI_CHECK(std::all_of(result.thumbnail_requests.begin(),
                       result.thumbnail_requests.end(),
                       [](const editor::LibraryThumbnailRequestIntent &intent) {
                         return intent.priority ==
                                editor::LibraryThumbnailRequestPriority::Visible;
                       }));
}

// -------------------------------------------------------------- containment

void long_lists_scroll_and_clip_interaction_to_the_list() {
  Harness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 220.0F};
  for (core::u64 index = 0U; index < 30U; ++index) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{100U + index};
    asset.display_name = "Plate " + std::to_string(index);
    asset.source = "C:/show/plate_" + std::to_string(index) + ".exr";
    harness.registry.assets.emplace_back(std::move(asset));
  }
  // Scroll to the end, then click the title. Rows whose logical coordinates
  // pass behind the title must neither paint nor receive that click.
  static_cast<void>(harness.frame(120.0F, 100.0F, false, gui::KeyMods::None,
                                  {}, -100.0F));
  static_cast<void>(harness.frame(120.0F, 10.0F, true));
  const editor::LibraryFrame result = harness.frame(120.0F, 10.0F, false);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(harness.panel.selection().empty());
  for (const gui::UiBatch &batch : harness.list.batches()) {
    UI_CHECK(batch.clip_min_x >= harness.bounds.x);
    UI_CHECK(batch.clip_min_y >= harness.bounds.y);
    UI_CHECK(batch.clip_max_x <= 1600.0F);
    UI_CHECK(batch.clip_max_y <= 900.0F);
  }
}

void empty_space_does_nothing() {
  Harness harness;
  const editor::LibraryFrame result = harness.click(110.0F, 400.0F);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(!harness.list.vertices().empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

// ---------------------------------------------------------------- platesets

[[nodiscard]] assets::AssetRegistry take_plates() {
  assets::AssetRegistry registry;
  const std::array<std::pair<core::u64, const char *>, 3U> plates{{
      {31U, "Take 7 FRONT.mp4"}, {32U, "Take 7 LEFT.mp4"},
      {33U, "Take 7 RIGHT.mp4"}}};
  for (const auto &[id, name] : plates) {
    assets::MediaAsset media{};
    media.id = doc::NodeId{id};
    media.display_name = name;
    media.source = std::string{"C:/show/plates/"} + name;
    media.frame_count = 2'400U;
    media.frame_rate = assets::MediaFrameRate{24U, 1U};
    registry.assets.emplace_back(std::move(media));
  }
  return registry;
}

// Selecting a take's plates and choosing New Plateset from Selection makes
// one set with each plate in the slot its name gives, the smallest rig that
// holds them, and the name the files share; the set opens in the Asset
// Viewer once it exists.
void a_plateset_is_made_from_the_selected_plates() {
  Harness harness;
  harness.registry = take_plates();
  harness.collapse_generators();
  const float first_row = 102.0F + asset_row_height * 0.5F;
  const float third_row = 102.0F + asset_row_height * 2.5F;
  static_cast<void>(harness.click(120.0F, first_row));
  static_cast<void>(harness.click(120.0F, third_row, gui::KeyMods::Shift));
  UI_CHECK(harness.panel.selection().size() == 3U);
  static_cast<void>(harness.right_click(120.0F, first_row));
  // Open, Rename, New Folder, Remove, New Plateset from Selection.
  const editor::LibraryFrame chosen = harness.choose(120.0F, first_row, 4U);
  UI_CHECK(chosen.library_edits.size() == 1U);
  const assets::LibraryEdit &edit = chosen.library_edits.front();
  UI_CHECK(edit.kind == assets::LibraryEdit::Kind::CreatePlateset);
  UI_CHECK(edit.name == "Take 7");
  UI_CHECK(edit.rig == assets::PlatesetRig::Three);
  UI_CHECK(edit.slots[static_cast<std::size_t>(assets::PlatesetSlot::Front)] ==
           doc::NodeId{31U});
  UI_CHECK(edit.slots[static_cast<std::size_t>(assets::PlatesetSlot::Left)] ==
           doc::NodeId{32U});
  UI_CHECK(edit.slots[static_cast<std::size_t>(assets::PlatesetSlot::Right)] ==
           doc::NodeId{33U});

  // The Director answers with the new set.
  assets::PlatesetAsset plateset{};
  plateset.id = doc::NodeId{90U};
  plateset.display_name = "Take 7";
  plateset.rig = assets::PlatesetRig::Three;
  harness.registry.assets.emplace_back(plateset);
  const editor::LibraryFrame arrived = harness.frame(-1.0F, -1.0F, false);
  UI_CHECK(arrived.view_asset == doc::NodeId{90U});
  UI_CHECK(harness.panel.selection().size() == 1U);
  UI_CHECK(harness.panel.selection()[0] == doc::NodeId{90U});
}

void an_empty_plateset_comes_from_the_background_menu() {
  Harness harness;
  harness.registry = take_plates();
  harness.collapse_generators();
  static_cast<void>(harness.right_click(120.0F, 400.0F));
  // New Folder, New Plateset.
  const editor::LibraryFrame chosen = harness.choose(120.0F, 400.0F, 1U);
  UI_CHECK(chosen.library_edits.size() == 1U);
  UI_CHECK(chosen.library_edits.front().kind ==
           assets::LibraryEdit::Kind::CreatePlateset);
  UI_CHECK(chosen.library_edits.front().name == "Plateset");
  UI_CHECK(chosen.library_edits.front().slots ==
           (std::array<doc::NodeId, assets::plateset_slot_count>{}));
  UI_CHECK(editor::library_column_text(
               assets::Asset{assets::PlatesetAsset{
                   doc::NodeId{91U}, "Night", assets::PlatesetRig::Nine,
                   {{{doc::NodeId{31U}, 0}, {}, {doc::NodeId{33U}, 4}}}}},
               editor::LibraryColumn::Type) == "Plateset (2)");
}

constexpr std::array<Case, 39U> cases{{
    {"changed sources are offered for acceptance",
     &changed_sources_are_offered_for_acceptance},
    {"the horizontal scrollbar drags without touching rows",
     &the_horizontal_scrollbar_drags_without_touching_rows},
    {"a generator click reports its add action",
     &a_generator_click_reports_its_add_action},
    {"a surface generator drags out to Stage",
     &a_surface_generator_drags_out_to_stage},
    {"the generators section collapses", &the_generators_section_collapses},
    {"a click selects and a double-click opens the viewer",
     &a_click_selects_and_a_double_click_opens_the_viewer},
    {"Shift extends and Ctrl toggles the selection",
     &shift_extends_and_ctrl_toggles_the_selection},
    {"search filters assets by name and metadata",
     &search_filters_assets_by_name_and_metadata},
    {"a header click sorts by that column",
     &a_header_click_sorts_by_that_column},
    {"a header edge drag resizes its column",
     &a_header_edge_drag_resizes_its_column},
    {"the header menu shows and hides columns",
     &the_header_menu_shows_and_hides_columns},
    {"columns read authored metadata", &columns_read_authored_metadata},
    {"drop-frame timecode counts like an editor",
     &drop_frame_timecode_counts_like_an_editor},
    {"files import by extension", &files_import_by_extension},
    {"the proxy chip cycles its policy without placing",
     &the_proxy_chip_cycles_its_policy_without_placing},
    {"cpu-only media asks for a full-resolution proxy and warns",
     &cpu_only_media_asks_for_a_full_resolution_proxy_and_warns},
    {"proxy runtime status labels explain resolution and failure",
     &proxy_runtime_status_labels_explain_resolution_and_failure},
    {"editor proxy status says how much is made",
     &editor_proxy_status_says_how_much_is_made},
    {"a drag out carries the whole selection under a thumbnail",
     &a_drag_out_carries_the_whole_selection_under_a_thumbnail},
    {"dragging onto a folder files the asset there",
     &dragging_onto_a_folder_files_the_asset_there},
    {"a folder opens on its twirl and files back to the top",
     &a_folder_opens_on_its_twirl_and_files_back_to_the_top},
    {"a folder is never filed inside itself",
     &a_folder_is_never_filed_inside_itself},
    {"files from the desktop import into the folder under them",
     &files_from_the_desktop_import_into_the_folder_under_them},
    {"a new folder is created then named in place",
     &new_folder_is_created_then_named_in_place},
    {"Delete removes selected assets and folders",
     &delete_removes_selected_assets_and_folders},
    {"an asset menu removes it from the Library",
     &an_asset_menu_removes_it_from_the_library},
    {"the Grid view browses one folder at a time",
     &the_grid_view_browses_one_folder_at_a_time},
    {"the Grid view files back up through its path",
     &the_grid_view_files_back_up_through_its_path},
    {"the size slider and Ctrl+wheel resize thumbnails",
     &the_size_slider_and_ctrl_wheel_resize_thumbnails},
    {"thumbnails request visible rows then a bounded prefetch",
     &thumbnails_request_visible_rows_then_a_bounded_prefetch},
    {"thumbnail observations suppress duplicate requests",
     &thumbnail_observations_suppress_duplicate_requests},
    {"thumbnail observations must match the exact content revision",
     &thumbnail_observations_must_match_the_exact_content_revision},
    {"scrolling replaces the thumbnail retain set",
     &scrolling_replaces_the_thumbnail_retain_set},
    {"thumbnail identity is never invented for unfingerprinted assets",
     &thumbnail_identity_is_never_invented_for_unfingerprinted_assets},
    {"the Grid view requests thumbnails for its tiles",
     &the_grid_view_requests_thumbnails_for_its_tiles},
    {"long lists scroll and clip interaction to the list",
     &long_lists_scroll_and_clip_interaction_to_the_list},
    {"empty space does nothing", &empty_space_does_nothing},
    {"a plateset is made from the selected plates",
     &a_plateset_is_made_from_the_selected_plates},
    {"an empty plateset comes from the background menu",
     &an_empty_plateset_comes_from_the_background_menu},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << std::endl;
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
