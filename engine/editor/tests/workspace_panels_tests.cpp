#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
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
namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] std::vector<editor::ElementInfo> elements() {
  editor::ElementInfo object{};
  object.node = doc::NodeId{7U};
  object.kind = editor::ElementKind::Object;
  editor::ElementInfo media{};
  media.node = doc::NodeId{9U};
  media.kind = editor::ElementKind::Media;
  editor::ElementInfo light{};
  light.node = doc::NodeId{11U};
  light.kind = editor::ElementKind::PointLight;
  return {object, media, light};
}

struct SceneHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ScenePanel panel{gui::ui_id("scene.test")};
  std::vector<editor::ElementInfo> items{elements()};
  std::vector<doc::NodeId> selection;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 300.0F};

  std::optional<editor::SceneSelectionRequest>
  frame(float x, float y, bool down, gui::KeyMods mods = gui::KeyMods::None,
        std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mods(mods);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds, items, selection,
                      selection.empty() ? doc::NodeId{} : selection.back());
  }
};

void a_scene_row_reports_the_clicked_node() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 101.0F, false));
  static_cast<void>(harness.frame(80.0F, 101.0F, true));
  const auto request = harness.frame(80.0F, 101.0F, false);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

void a_ctrl_scene_click_requests_an_exact_selection_toggle() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 101.0F, false));
  static_cast<void>(harness.frame(80.0F, 101.0F, true, gui::KeyMods::Ctrl));
  const auto request = harness.frame(80.0F, 101.0F, false, gui::KeyMods::Ctrl);

  UI_CHECK(request.has_value());
  UI_CHECK(request->action == editor::SelectionAction::Toggle);
  UI_CHECK(request->visible_order.empty());
}

void a_shift_scene_click_carries_the_visible_element_order() {
  SceneHarness harness;
  // The light is the third visible element row after Visuals and Media.
  static_cast<void>(harness.frame(80.0F, 183.0F, false));
  static_cast<void>(harness.frame(80.0F, 183.0F, true, gui::KeyMods::Shift));
  const auto request = harness.frame(80.0F, 183.0F, false, gui::KeyMods::Shift);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{11U});
  UI_CHECK(request->action == editor::SelectionAction::Range);
  UI_CHECK(request->visible_order ==
           std::vector<doc::NodeId>{doc::NodeId{7U}, doc::NodeId{9U},
                                    doc::NodeId{11U}});
}

void bare_scene_list_space_requests_a_clear() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 240.0F, false));
  const auto request = harness.frame(80.0F, 240.0F, true);

  UI_CHECK(request.has_value());
  UI_CHECK(request->action == editor::SelectionAction::Clear);
  UI_CHECK(request->node == doc::NodeId{});
}

void a_locked_scene_row_only_offers_its_unlock_control() {
  SceneHarness harness;
  harness.items.front().locked = true;

  static_cast<void>(harness.frame(80.0F, 101.0F, false));
  static_cast<void>(harness.frame(80.0F, 101.0F, true));
  UI_CHECK(!harness.frame(80.0F, 101.0F, false).has_value());

  constexpr float lock_x = 220.0F;
  static_cast<void>(harness.frame(lock_x, 101.0F, false));
  static_cast<void>(harness.frame(lock_x, 101.0F, true));
  const auto request = harness.frame(lock_x, 101.0F, false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->lock_changed);
  UI_CHECK(!request->locked);
}

void scene_search_filters_the_hierarchy() {
  SceneHarness harness;
  // Focus the search well, then type. The filtered tree contains only the
  // Lights group and its matching point-light row at the original first-row
  // coordinate.
  static_cast<void>(harness.frame(70.0F, 37.0F, false));
  static_cast<void>(harness.frame(70.0F, 37.0F, true));
  static_cast<void>(harness.frame(70.0F, 37.0F, false));
  static_cast<void>(
      harness.frame(70.0F, 37.0F, false, gui::KeyMods::None, "point"));
  UI_CHECK(harness.panel.search() == "point");

  static_cast<void>(harness.frame(80.0F, 101.0F, true));
  const auto request = harness.frame(80.0F, 101.0F, false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{11U});
}

void a_scene_group_is_visible_selectable_and_uses_its_authored_name() {
  SceneHarness harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Opening Look";
  harness.items.push_back(group);

  // Groups are a first-class final Scene category, after the existing
  // visuals/media/lights rows.
  static_cast<void>(harness.frame(80.0F, 224.0F, false));
  static_cast<void>(harness.frame(80.0F, 224.0F, true));
  const auto request = harness.frame(80.0F, 224.0F, false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{13U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

void scene_group_and_ungroup_actions_are_truthfully_enabled() {
  SceneHarness group_harness;
  group_harness.selection = {doc::NodeId{7U}, doc::NodeId{9U}};
  static_cast<void>(group_harness.frame(145.0F, 289.0F, false));
  static_cast<void>(group_harness.frame(145.0F, 289.0F, true));
  const auto grouped = group_harness.frame(145.0F, 289.0F, false);
  UI_CHECK(grouped.has_value());
  UI_CHECK(grouped->structure ==
           editor::SceneSelectionRequest::StructureAction::Group);

  SceneHarness ungroup_harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Opening Look";
  ungroup_harness.items.push_back(group);
  ungroup_harness.selection = {group.node};
  static_cast<void>(ungroup_harness.frame(205.0F, 289.0F, false));
  static_cast<void>(ungroup_harness.frame(205.0F, 289.0F, true));
  const auto ungrouped = ungroup_harness.frame(205.0F, 289.0F, false);
  UI_CHECK(ungrouped.has_value());
  UI_CHECK(ungrouped->structure ==
           editor::SceneSelectionRequest::StructureAction::Ungroup);

  // A light is not accepted by the core grouping planner, and the disabled
  // button must not fabricate an action.
  SceneHarness light_harness;
  light_harness.selection = {doc::NodeId{11U}};
  static_cast<void>(light_harness.frame(145.0F, 289.0F, false));
  static_cast<void>(light_harness.frame(145.0F, 289.0F, true));
  UI_CHECK(!light_harness.frame(145.0F, 289.0F, false).has_value());
}

struct LibraryHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LibraryPanel panel{gui::ui_id("library.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 260.0F, 320.0F};

  std::optional<editor::LibraryAddAction> frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  }
};

[[nodiscard]] assets::AssetRegistry project_assets() {
  assets::MediaAsset media{};
  media.id = doc::NodeId{20U};
  media.display_name = "Opening Plate";
  media.source = "media/opening_plate.exr";
  media.dimensions = assets::PixelDimensions{3840U, 2160U};
  media.frame_count = 900U;

  assets::ModelAsset model{};
  model.id = doc::NodeId{30U};
  model.display_name = "Hero Car";
  model.source = "models/hero_car.glb";
  model.triangle_count = 128'000U;
  model.material_count = 7U;

  assets::AssetRegistry registry;
  registry.assets.emplace_back(std::move(media));
  registry.assets.emplace_back(std::move(model));
  return registry;
}

struct AssetLibraryHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LibraryPanel panel{gui::ui_id("library.assets.test")};
  gui::Rect bounds{0.0F, 0.0F, 360.0F, 520.0F};

  editor::LibraryFrame frame(const assets::AssetRegistry &registry, float x,
                             float y, bool down, std::string_view text = {},
                             float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, registry);
  }

  editor::LibraryFrame thumbnail_frame(
      const assets::AssetRegistry &registry,
      std::span<const editor::LibraryThumbnailObservation> observations = {},
      editor::LibraryThumbnailWindow window = {}, float x = -1.0F,
      float y = -1.0F, bool down = false, float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, registry, observations, window);
  }

  editor::LibraryFrame runtime_frame(
      const assets::AssetRegistry &registry,
      std::span<const editor::LibraryProxyRuntimeObservation> proxy_runtime,
      std::span<const editor::LibraryThumbnailObservation> thumbnails = {},
      editor::LibraryThumbnailWindow window = {}, float x = -1.0F,
      float y = -1.0F, bool down = false) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, registry, thumbnails, window, proxy_runtime);
  }
};

[[nodiscard]] assets::ContentFingerprint test_fingerprint(core::u64 index) {
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 1'000U + index;
  fingerprint.sha256.bytes[0U] = static_cast<core::u8>(index & 0xffU);
  fingerprint.sha256.bytes[1U] = static_cast<core::u8>((index >> 8U) & 0xffU);
  return fingerprint;
}

void cpu_only_media_uses_full_resolution_proxy_and_draws_warning() {
  AssetLibraryHarness harness;
  assets::AssetRegistry registry = project_assets();
  const auto *const media =
      std::get_if<assets::MediaAsset>(&registry.assets.front());
  UI_CHECK(media != nullptr);
  const std::array<editor::LibraryProxyRuntimeObservation, 1U> runtime{{
      {media->id, std::nullopt, editor::LibraryProxyRuntimeState::Source,
       assets::ProxyPlaybackReason::SourcePolicy,
       editor::LibraryProxyRuntimeIssue::None, std::nullopt,
       editor::LibraryMediaDecodeState::CpuOnly},
  }};

  static_cast<void>(
      harness.runtime_frame(registry, runtime, {}, {}, 346.0F, 234.0F, false));
  static_cast<void>(
      harness.runtime_frame(registry, runtime, {}, {}, 346.0F, 234.0F, true));
  const editor::LibraryFrame result =
      harness.runtime_frame(registry, runtime, {}, {}, 346.0F, 234.0F, false);
  const bool red_warning = std::any_of(
      harness.list.vertices().begin(), harness.list.vertices().end(),
      [](const gui::UiVertex &vertex) {
        return vertex.x >= 314.0F && vertex.x <= 328.0F && vertex.y >= 200.0F &&
               vertex.y <= 216.0F && vertex.r > 0.8F && vertex.g < 0.6F;
      });
  UI_CHECK(red_warning);
  UI_CHECK(result.proxy_policy.has_value());
  UI_CHECK(result.proxy_policy->requested_profile ==
           assets::DerivativeProfileId::ProxySourceCfrHevcMain10);
  UI_CHECK(result.proxy_policy->policy ==
           assets::ProxyPlaybackPolicy::PreferProxy);
}

[[nodiscard]] assets::AssetRegistry
fingerprinted_media_assets(std::size_t count) {
  assets::AssetRegistry registry;
  registry.assets.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{100U + static_cast<core::u64>(index)};
    asset.display_name = "Plate " + std::to_string(index);
    asset.source = "C:/show/media/plate_" + std::to_string(index) + ".exr";
    asset.fingerprint = test_fingerprint(static_cast<core::u64>(index));
    registry.assets.emplace_back(std::move(asset));
  }
  return registry;
}

[[nodiscard]] assets::DerivativeKey
thumbnail_key(const assets::AssetRegistry &registry, std::size_t index) {
  UI_CHECK(index < registry.assets.size());
  const assets::Asset &asset = registry.assets[index];
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Thumbnail320x180Srgb);
  UI_CHECK(profile != nullptr);
  UI_CHECK(assets::asset_fingerprint(asset).has_value());
  return assets::make_derivative_key(
      assets::asset_id(asset), *assets::asset_fingerprint(asset), *profile);
}

[[nodiscard]] bool
contains_thumbnail_key(std::span<const assets::DerivativeKey> keys,
                       const assets::DerivativeKey &expected) {
  return std::find(keys.begin(), keys.end(), expected) != keys.end();
}

void a_library_item_reports_an_add_action() {
  LibraryHarness harness;
  // Generator heading is 50..71, and Box is the first dense item row.
  static_cast<void>(harness.frame(80.0F, 81.0F, false));
  static_cast<void>(harness.frame(80.0F, 81.0F, true));
  const auto request = harness.frame(80.0F, 81.0F, false);

  UI_CHECK(request.has_value());
  UI_CHECK(request.value() == editor::LibraryAddAction::Box);
}

void the_library_offers_a_camera() {
  // Rows are 20 tall from 71: Box, Plane, Sphere, Camera.
  //
  // A tracker feed is deliberately not here. It has no place on the stage and
  // nothing to select, so it is project state added from the camera that wants
  // one - see the Inspector's TRACKING section.
  LibraryHarness camera_harness;
  static_cast<void>(camera_harness.frame(80.0F, 141.0F, false));
  static_cast<void>(camera_harness.frame(80.0F, 141.0F, true));
  const auto camera = camera_harness.frame(80.0F, 141.0F, false);
  UI_CHECK(camera.has_value());
  UI_CHECK(camera.value() == editor::LibraryAddAction::Camera);

  // Camera is the last generator: the row below it is the MEDIA heading, not
  // another thing to add.
  LibraryHarness past_harness;
  static_cast<void>(past_harness.frame(80.0F, 161.0F, false));
  static_cast<void>(past_harness.frame(80.0F, 161.0F, true));
  UI_CHECK(!past_harness.frame(80.0F, 161.0F, false).has_value());
}

void library_filter_finds_and_instantiates_a_stable_asset() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, true));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false, "car"));
  UI_CHECK(harness.panel.search() == "car");

  // With this filter only MODELS remains: heading 50..71, asset 71..119.
  static_cast<void>(harness.frame(registry, 120.0F, 90.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 90.0F, false);
  UI_CHECK(result.instantiate_asset == doc::NodeId{30U});
  UI_CHECK(!result.import_asset.has_value());
  UI_CHECK(!result.add.has_value());
}

void library_imports_are_separate_persistent_asset_intents() {
  const assets::AssetRegistry empty;

  AssetLibraryHarness media_harness;
  // GENERATORS ends at 151; MEDIA's import button occupies 175..195.
  static_cast<void>(media_harness.frame(empty, 110.0F, 185.0F, false));
  static_cast<void>(media_harness.frame(empty, 110.0F, 185.0F, true));
  const editor::LibraryFrame media =
      media_harness.frame(empty, 110.0F, 185.0F, false);
  UI_CHECK(media.import_asset == editor::LibraryImportAction::Media);
  UI_CHECK(!media.instantiate_asset.has_value());
  UI_CHECK(!media.add.has_value());

  AssetLibraryHarness model_harness;
  // MEDIA's honest empty state ends at 222; MODELS' button is 246..266.
  static_cast<void>(model_harness.frame(empty, 110.0F, 255.0F, false));
  static_cast<void>(model_harness.frame(empty, 110.0F, 255.0F, true));
  const editor::LibraryFrame model =
      model_harness.frame(empty, 110.0F, 255.0F, false);
  UI_CHECK(model.import_asset == editor::LibraryImportAction::Model);
  UI_CHECK(!model.instantiate_asset.has_value());
  UI_CHECK(!model.add.has_value());
}

void an_existing_media_row_returns_its_asset_id() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  // MEDIA heading follows the generators; its import button ends at 198 and
  // the persisted Opening Plate row occupies 198..246.
  static_cast<void>(harness.frame(registry, 120.0F, 220.0F, false));
  static_cast<void>(harness.frame(registry, 120.0F, 220.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 220.0F, false);
  UI_CHECK(result.instantiate_asset == doc::NodeId{20U});
  UI_CHECK(!result.import_asset.has_value());
  UI_CHECK(!result.add.has_value());
}

void an_imported_asset_row_has_a_separate_remove_action() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  // The persisted media row occupies 198..246. Its rightmost action column is
  // reserved for removal, so it cannot also instantiate or begin a drag.
  static_cast<void>(harness.frame(registry, 346.0F, 220.0F, false));
  static_cast<void>(harness.frame(registry, 346.0F, 220.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 346.0F, 220.0F, false);
  UI_CHECK(result.remove_asset == doc::NodeId{20U});
  UI_CHECK(!result.instantiate_asset.has_value());
  UI_CHECK(!result.dragging_asset.has_value());
  UI_CHECK(!result.asset_drop.has_value());
}

void media_proxy_action_cycles_source_prefer_require_without_new_identity() {
  AssetLibraryHarness harness;
  assets::AssetRegistry registry = project_assets();
  auto *const media = std::get_if<assets::MediaAsset>(&registry.assets[0U]);
  UI_CHECK(media != nullptr);

  const auto click_proxy = [&]() {
    static_cast<void>(harness.frame(registry, 346.0F, 234.0F, false));
    static_cast<void>(harness.frame(registry, 346.0F, 234.0F, true));
    return harness.frame(registry, 346.0F, 234.0F, false);
  };

  editor::LibraryFrame result = click_proxy();
  UI_CHECK(result.proxy_policy.has_value());
  UI_CHECK(result.proxy_policy->asset == media->id);
  UI_CHECK(result.proxy_policy->policy ==
           assets::ProxyPlaybackPolicy::PreferProxy);
  UI_CHECK(result.proxy_policy->requested_profile ==
           assets::DerivativeProfileId::Proxy720pCfrH264);
  UI_CHECK(!result.instantiate_asset.has_value());
  UI_CHECK(!result.remove_asset.has_value());

  media->proxy_playback_policy = assets::ProxyPlaybackPolicy::PreferProxy;
  media->requested_proxy_profile =
      assets::DerivativeProfileId::Proxy720pCfrH264;
  result = click_proxy();
  UI_CHECK(result.proxy_policy->asset == media->id);
  UI_CHECK(result.proxy_policy->policy ==
           assets::ProxyPlaybackPolicy::RequireProxy);
  UI_CHECK(result.proxy_policy->requested_profile ==
           assets::DerivativeProfileId::Proxy720pCfrH264);

  media->proxy_playback_policy = assets::ProxyPlaybackPolicy::RequireProxy;
  result = click_proxy();
  UI_CHECK(result.proxy_policy->asset == media->id);
  UI_CHECK(result.proxy_policy->policy == assets::ProxyPlaybackPolicy::Source);
  UI_CHECK(!result.proxy_policy->requested_profile.has_value());
}

void an_asset_row_can_be_dragged_out_for_stage_placement() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  static_cast<void>(harness.frame(registry, 120.0F, 220.0F, false));
  static_cast<void>(harness.frame(registry, 120.0F, 220.0F, true));
  const editor::LibraryFrame dragging =
      harness.frame(registry, 500.0F, 250.0F, true);
  UI_CHECK(dragging.dragging_asset == doc::NodeId{20U});
  UI_CHECK(!dragging.instantiate_asset.has_value());

  const editor::LibraryFrame dropped =
      harness.frame(registry, 500.0F, 250.0F, false);
  UI_CHECK(dropped.asset_drop.has_value());
  UI_CHECK(dropped.asset_drop->asset == doc::NodeId{20U});
  UI_CHECK(dropped.asset_drop->screen_x == 500.0F);
  UI_CHECK(dropped.asset_drop->screen_y == 250.0F);
  UI_CHECK(!dropped.instantiate_asset.has_value());
}

void an_effect_drag_reports_its_graph_drop_position_without_a_selection() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.drag.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 180.0F};
  const auto frame = [&](float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  };

  static_cast<void>(frame(80.0F, 90.0F, false));
  static_cast<void>(frame(80.0F, 90.0F, true));
  static_cast<void>(frame(430.0F, 270.0F, true));
  const auto dropped = frame(430.0F, 270.0F, false);
  UI_CHECK(dropped.has_value());
  UI_CHECK(dropped->target == doc::NodeId{});
  UI_CHECK(dropped->effect == scene::EffectKind::Brightness);
  UI_CHECK(dropped->screen_x == 430.0F);
  UI_CHECK(dropped->screen_y == 270.0F);
}

void clicking_an_effect_does_not_apply_it() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.saturation.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 180.0F};
  const doc::NodeId selected{42U};
  const auto frame = [&](bool down) {
    input.new_frame();
    input.set_mouse_position(80.0F, 140.0F);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, selected);
  };

  static_cast<void>(frame(false));
  static_cast<void>(frame(true));
  const auto applied = frame(false);
  UI_CHECK(!applied.has_value());
}

void effect_search_filters_before_dragging() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.search.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 200.0F};
  const auto frame = [&](float x, float y, bool down,
                         std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  };

  static_cast<void>(frame(80.0F, 37.0F, false));
  static_cast<void>(frame(80.0F, 37.0F, true));
  static_cast<void>(frame(80.0F, 37.0F, false));
  static_cast<void>(frame(80.0F, 37.0F, false, "directional"));
  UI_CHECK(panel.search() == "directional");

  static_cast<void>(frame(80.0F, 90.0F, false));
  static_cast<void>(frame(80.0F, 90.0F, true));
  static_cast<void>(frame(430.0F, 270.0F, true));
  const auto dropped = frame(430.0F, 270.0F, false);
  UI_CHECK(dropped.has_value());
  UI_CHECK(dropped->effect == scene::EffectKind::DirectionalBlur);
}

void effect_drops_apply_only_over_stage_or_inspector() {
  constexpr gui::Rect stage{300.0F, 40.0F, 600.0F, 400.0F};
  constexpr gui::Rect inspector{920.0F, 40.0F, 260.0F, 600.0F};
  editor::EffectApplyRequest request{doc::NodeId{42U}, scene::EffectKind::Blur,
                                     1040.0F, 240.0F};

  UI_CHECK(
      editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
  request.screen_x = 500.0F;
  request.screen_y = 200.0F;
  UI_CHECK(
      editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
  request.screen_x = 120.0F;
  request.screen_y = 500.0F;
  UI_CHECK(
      !editor::effect_drop_targets_selected_layer(request, &stage, &inspector));

  // A drag with no selected target can never apply implicitly.
  request.target = {};
  UI_CHECK(
      !editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
}

void an_effect_drop_target_strengthens_under_the_pointer() {
  constexpr gui::Rect target{300.0F, 40.0F, 260.0F, 600.0F};
  const auto strongest_cyan = [&](float x, float y) {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    input.new_frame();
    input.set_mouse_position(x, y);
    ctx.new_frame();
    editor::draw_effect_drop_target(ctx, target, "Hero clip");
    float strongest{};
    const gui::UiColor accent = ctx.theme().accent;
    for (const gui::UiVertex &vertex : list.vertices()) {
      if (vertex.r == accent.r && vertex.g == accent.g &&
          vertex.b == accent.b) {
        strongest = std::max(strongest, vertex.a);
      }
    }
    return strongest;
  };

  const float candidate = strongest_cyan(120.0F, 500.0F);
  const float hovered = strongest_cyan(420.0F, 240.0F);
  UI_CHECK(candidate > 0.25F && candidate < 0.50F);
  UI_CHECK(hovered > 0.95F);
}

void an_empty_registry_has_no_phantom_asset_rows() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry empty;

  // This is the MEDIA empty-state row (195..219), not its import button.
  static_cast<void>(harness.frame(empty, 110.0F, 207.0F, false));
  static_cast<void>(harness.frame(empty, 110.0F, 207.0F, true));
  const editor::LibraryFrame result =
      harness.frame(empty, 110.0F, 207.0F, false);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(!harness.list.vertices().empty());
}

void long_asset_lists_scroll_and_clip_interaction_to_the_list() {
  AssetLibraryHarness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 220.0F};
  assets::AssetRegistry registry;
  for (core::u64 index = 0U; index < 30U; ++index) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{100U + index};
    asset.display_name = "Plate " + std::to_string(index);
    asset.source = "media/plate_" + std::to_string(index) + ".exr";
    registry.assets.emplace_back(std::move(asset));
  }

  // Scroll to the end, then click the title. Rows whose logical coordinates
  // pass behind the title must neither paint nor receive that click.
  static_cast<void>(
      harness.frame(registry, 120.0F, 100.0F, false, {}, -100.0F));
  static_cast<void>(harness.frame(registry, 120.0F, 10.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 10.0F, false);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(harness.list.vertices().size() < 1'000U);
  for (const gui::UiBatch &batch : harness.list.batches()) {
    UI_CHECK(batch.clip_min_x >= harness.bounds.x);
    UI_CHECK(batch.clip_min_y >= harness.bounds.y);
    UI_CHECK(batch.clip_max_x <= harness.bounds.x + harness.bounds.width);
    UI_CHECK(batch.clip_max_y <= harness.bounds.y + harness.bounds.height);
  }
}

void thumbnail_rows_request_visible_then_bounded_prefetch_assets() {
  AssetLibraryHarness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 240.0F};
  const assets::AssetRegistry registry = fingerprinted_media_assets(8U);
  const editor::LibraryThumbnailWindow window{2U, 4U};

  const editor::LibraryFrame result = harness.thumbnail_frame(
      registry, std::span<const editor::LibraryThumbnailObservation>{}, window);

  UI_CHECK(result.thumbnail_retain.has_value());
  UI_CHECK(result.thumbnail_retain->keys.size() == 3U);
  UI_CHECK(result.thumbnail_requests.size() == 3U);
  UI_CHECK(result.thumbnail_retain->keys[0U] == thumbnail_key(registry, 0U));
  UI_CHECK(result.thumbnail_retain->keys[1U] == thumbnail_key(registry, 1U));
  UI_CHECK(result.thumbnail_retain->keys[2U] == thumbnail_key(registry, 2U));
  UI_CHECK(result.thumbnail_requests[0U].key == thumbnail_key(registry, 0U));
  UI_CHECK(result.thumbnail_requests[0U].priority ==
           editor::LibraryThumbnailRequestPriority::Visible);
  UI_CHECK(result.thumbnail_requests[1U].priority ==
           editor::LibraryThumbnailRequestPriority::Prefetch);
  UI_CHECK(result.thumbnail_requests[2U].priority ==
           editor::LibraryThumbnailRequestPriority::Prefetch);
}

void thumbnail_observations_suppress_duplicate_requests() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = fingerprinted_media_assets(6U);
  constexpr std::array<editor::LibraryThumbnailState, 6U> states{{
      editor::LibraryThumbnailState::Queued,
      editor::LibraryThumbnailState::Generating,
      editor::LibraryThumbnailState::Ready,
      editor::LibraryThumbnailState::Stale,
      editor::LibraryThumbnailState::Failed,
      editor::LibraryThumbnailState::Placeholder,
  }};
  std::vector<editor::LibraryThumbnailObservation> observations;
  observations.reserve(states.size());
  for (std::size_t index = 0U; index < states.size(); ++index) {
    observations.push_back(editor::LibraryThumbnailObservation{
        thumbnail_key(registry, index), states[index], nullptr});
  }

  const editor::LibraryFrame result =
      harness.thumbnail_frame(registry, observations);

  UI_CHECK(result.thumbnail_retain.has_value());
  UI_CHECK(result.thumbnail_retain->keys.size() == states.size());
  UI_CHECK(result.thumbnail_requests.size() == 2U);
  UI_CHECK(result.thumbnail_requests[0U].key == thumbnail_key(registry, 3U));
  UI_CHECK(result.thumbnail_requests[0U].priority ==
           editor::LibraryThumbnailRequestPriority::Visible);
  UI_CHECK(result.thumbnail_requests[1U].key == thumbnail_key(registry, 5U));
  UI_CHECK(result.thumbnail_requests[1U].priority ==
           editor::LibraryThumbnailRequestPriority::Prefetch);
}

void thumbnail_observations_must_match_the_exact_content_revision() {
  AssetLibraryHarness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 220.0F};
  const assets::AssetRegistry registry = fingerprinted_media_assets(1U);
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Thumbnail320x180Srgb);
  UI_CHECK(profile != nullptr);
  const assets::DerivativeKey wrong_revision = assets::make_derivative_key(
      doc::NodeId{100U}, test_fingerprint(999U), *profile);
  const std::array<editor::LibraryThumbnailObservation, 1U> observations{{
      {wrong_revision, editor::LibraryThumbnailState::Queued, nullptr},
  }};

  const editor::LibraryFrame result =
      harness.thumbnail_frame(registry, observations);

  UI_CHECK(result.thumbnail_retain.has_value());
  UI_CHECK(result.thumbnail_retain->keys ==
           std::vector<assets::DerivativeKey>{thumbnail_key(registry, 0U)});
  UI_CHECK(result.thumbnail_requests.size() == 1U);
  UI_CHECK(result.thumbnail_requests[0U].key == thumbnail_key(registry, 0U));
  UI_CHECK(result.thumbnail_requests[0U].key != wrong_revision);
}

void scrolling_replaces_the_thumbnail_retain_set() {
  AssetLibraryHarness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 220.0F};
  const assets::AssetRegistry registry = fingerprinted_media_assets(12U);
  const editor::LibraryThumbnailWindow window{2U, 16U};

  const editor::LibraryFrame first = harness.thumbnail_frame(
      registry, std::span<const editor::LibraryThumbnailObservation>{}, window);
  UI_CHECK(first.thumbnail_retain.has_value());
  UI_CHECK(contains_thumbnail_key(first.thumbnail_retain->keys,
                                  thumbnail_key(registry, 0U)));

  const editor::LibraryFrame scrolled = harness.thumbnail_frame(
      registry, std::span<const editor::LibraryThumbnailObservation>{}, window,
      120.0F, 100.0F, false, -100.0F);
  UI_CHECK(scrolled.thumbnail_retain.has_value());
  UI_CHECK(!contains_thumbnail_key(scrolled.thumbnail_retain->keys,
                                   thumbnail_key(registry, 0U)));
  UI_CHECK(contains_thumbnail_key(scrolled.thumbnail_retain->keys,
                                  thumbnail_key(registry, 11U)));
}

void thumbnail_identity_is_never_invented_for_unfingerprinted_assets() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  const editor::LibraryFrame thumbnail_result =
      harness.thumbnail_frame(registry);
  UI_CHECK(thumbnail_result.thumbnail_retain.has_value());
  UI_CHECK(thumbnail_result.thumbnail_retain->keys.empty());
  UI_CHECK(thumbnail_result.thumbnail_requests.empty());

  const editor::LibraryFrame legacy_result =
      harness.frame(registry, -1.0F, -1.0F, false);
  UI_CHECK(!legacy_result.thumbnail_retain.has_value());
  UI_CHECK(legacy_result.thumbnail_requests.empty());
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

void proxy_runtime_rows_preserve_thumbnail_and_asset_identity() {
  AssetLibraryHarness harness;
  assets::AssetRegistry registry = fingerprinted_media_assets(1U);
  auto *const media = std::get_if<assets::MediaAsset>(&registry.assets.front());
  UI_CHECK(media != nullptr);
  media->requested_proxy_profile =
      assets::DerivativeProfileId::Proxy720pCfrH264;
  media->proxy_playback_policy = assets::ProxyPlaybackPolicy::PreferProxy;
  const doc::NodeId persistent_id = media->id;
  const auto proxy_key = assets::requested_proxy_key(*media);
  UI_CHECK(proxy_key.has_value());
  const std::array<editor::LibraryProxyRuntimeObservation, 1U> runtime{{
      {media->id, proxy_key, editor::LibraryProxyRuntimeState::ProxyReady,
       assets::ProxyPlaybackReason::None,
       editor::LibraryProxyRuntimeIssue::None},
  }};

  const editor::LibraryFrame result = harness.runtime_frame(registry, runtime);
  UI_CHECK(result.thumbnail_retain.has_value());
  UI_CHECK(result.thumbnail_retain->keys ==
           std::vector<assets::DerivativeKey>{thumbnail_key(registry, 0U)});
  UI_CHECK(result.thumbnail_requests.size() == 1U);
  UI_CHECK(result.thumbnail_requests.front().key ==
           thumbnail_key(registry, 0U));
  UI_CHECK(assets::asset_id(registry.assets.front()) == persistent_id);
  UI_CHECK(runtime.front().asset == persistent_id);
  UI_CHECK(runtime.front().key->asset == persistent_id);
}

[[nodiscard]] compositor::GraphNode
graph_node(core::u64 id, compositor::NodeKind kind, float x, float y) {
  compositor::GraphNode node{};
  node.node = doc::NodeId{id};
  node.kind = kind;
  node.label = std::string{compositor::node_kind_name(kind)};
  node.editor_x = x;
  node.editor_y = y;
  switch (kind) {
  case compositor::NodeKind::SceneLayer:
    node.parameters = compositor::SceneLayerParameters{doc::NodeId{1000U}};
    break;
  case compositor::NodeKind::Brightness:
    node.parameters = compositor::BrightnessParameters{1.25F};
    break;
  case compositor::NodeKind::Saturation:
    node.parameters = compositor::SaturationParameters{1.5F};
    break;
  case compositor::NodeKind::Levels:
    node.parameters = compositor::LevelsParameters{};
    break;
  case compositor::NodeKind::LiftGammaGain:
    node.parameters = compositor::LiftGammaGainParameters{};
    break;
  case compositor::NodeKind::ContrastPivot:
    node.parameters = compositor::ContrastPivotParameters{};
    break;
  case compositor::NodeKind::ColorMatrix:
    node.parameters = compositor::ColorMatrixParameters{};
    break;
  case compositor::NodeKind::WhiteBalance:
    node.parameters = compositor::WhiteBalanceParameters{};
    break;
  case compositor::NodeKind::Curves: {
    const auto parameters =
        scene::normalized_curves(compositor::CurvesParameters{});
    UI_CHECK(parameters.has_value());
    node.parameters = *parameters;
    break;
  }
  case compositor::NodeKind::FilmGrain:
    node.parameters = compositor::FilmGrainParameters{
        0.25F, 2.0F, 0.1F, 17U, scene::FilmGrainMode::Monochrome};
    break;
  case compositor::NodeKind::Blur:
    node.parameters = compositor::BlurParameters{12.0F};
    break;
  case compositor::NodeKind::BoxBlur:
    node.parameters = compositor::BoxBlurParameters{4U, 2U};
    break;
  case compositor::NodeKind::DirectionalBlur:
    node.parameters = compositor::DirectionalBlurParameters{16.0F, 0.125F};
    break;
  case compositor::NodeKind::ChromaKey:
    node.parameters = compositor::ChromaKeyParameters{};
    break;
  case compositor::NodeKind::LumaKey:
    node.parameters = compositor::LumaKeyParameters{};
    break;
  case compositor::NodeKind::Matte:
    node.parameters = compositor::MatteParameters{};
    break;
  case compositor::NodeKind::RadialArray:
    node.parameters = compositor::RadialArrayParameters{};
    break;
  case compositor::NodeKind::Composite:
    node.parameters = compositor::CompositeParameters{};
    break;
  case compositor::NodeKind::Output:
    node.parameters = compositor::OutputParameters{};
    break;
  }
  return node;
}

struct CompositorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::CompositorPanel panel{gui::ui_id("compositor.edit.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 640.0F};

  editor::CompositorFrame frame(const compositor::CompositorGraph &graph,
                                float x, float y, bool down,
                                std::string_view text = {},
                                std::optional<gui::Key> key = std::nullopt,
                                gui::KeyMods mods = gui::KeyMods::None) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Middle, false);
    input.set_mods(mods);
    input.add_text(text);
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, graph);
  }

  editor::CompositorFrame
  navigation_frame(const compositor::CompositorGraph &graph, float x, float y,
                   bool middle_down, float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, middle_down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, graph);
  }
};

void select_compositor_node(CompositorHarness &harness,
                            const compositor::CompositorGraph &graph) {
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
}

[[nodiscard]] editor::CompositorFrame
commit_compositor_field(CompositorHarness &harness,
                        const compositor::CompositorGraph &graph, float x,
                        float y, std::string_view value) {
  static_cast<void>(harness.frame(graph, x, y, false));
  static_cast<void>(harness.frame(graph, x, y, true));
  const editor::CompositorFrame opened = harness.frame(graph, x, y, false);
  UI_CHECK(opened.editing);
  UI_CHECK(!opened.property_edit.has_value());
  const editor::CompositorFrame typed =
      harness.frame(graph, x, y, false, value);
  UI_CHECK(typed.editing);
  UI_CHECK(!typed.property_edit.has_value());
  return harness.frame(graph, x, y, false, {}, gui::Key::Enter);
}

void compositor_toolbar_reports_effect_add_intents() {
  const compositor::CompositorGraph graph;
  struct SearchCase final {
    std::string_view query;
    compositor::NodeKind kind;
  };
  constexpr std::array<SearchCase, 7U> searches{{
      {"exposure", compositor::NodeKind::Brightness},
      {"green screen", compositor::NodeKind::ChromaKey},
      {"gaussian", compositor::NodeKind::Blur},
      {"noise", compositor::NodeKind::FilmGrain},
      {"array", compositor::NodeKind::RadialArray},
      {"source object", compositor::NodeKind::SceneLayer},
      {"merge", compositor::NodeKind::Composite},
  }};
  for (const SearchCase &search : searches) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, true));
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, true));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, false));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, false, search.query));
    static_cast<void>(harness.frame(graph, 120.0F, 70.0F, false));
    static_cast<void>(harness.frame(graph, 120.0F, 70.0F, true));
    const editor::CompositorFrame result =
        harness.frame(graph, 120.0F, 70.0F, false);
    UI_CHECK(result.add_node.has_value());
    UI_CHECK(result.add_node->kind == search.kind);
    UI_CHECK(result.add_node->editor_x >= 0.0F);
    UI_CHECK(result.add_node->editor_y >= 0.0F);
    UI_CHECK(!result.move_node.has_value());
    UI_CHECK(!result.connect_ports.has_value());
  }
}

void compositor_output_creation_is_visible_and_unique() {
  const compositor::CompositorGraph empty;
  CompositorHarness add_harness;
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, false));
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, true));
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, false));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, true));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, false));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, false, "output"));
  static_cast<void>(add_harness.frame(empty, 120.0F, 70.0F, false));
  static_cast<void>(add_harness.frame(empty, 120.0F, 70.0F, true));
  const editor::CompositorFrame added =
      add_harness.frame(empty, 120.0F, 70.0F, false);
  UI_CHECK(added.add_node.has_value());
  UI_CHECK(added.add_node->kind == compositor::NodeKind::Output);

  compositor::CompositorGraph configured;
  configured.nodes.push_back(
      graph_node(90U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness disabled_harness;
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, true));
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 80.0F, 42.0F, true));
  static_cast<void>(disabled_harness.frame(configured, 80.0F, 42.0F, false));
  static_cast<void>(
      disabled_harness.frame(configured, 80.0F, 42.0F, false, "output"));
  static_cast<void>(disabled_harness.frame(configured, 120.0F, 70.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 120.0F, 70.0F, true));
  const editor::CompositorFrame rejected =
      disabled_harness.frame(configured, 120.0F, 70.0F, false);
  UI_CHECK(!rejected.add_node.has_value());
}

void selected_compositor_nodes_and_edges_return_delete_intents() {
  compositor::CompositorGraph node_graph;
  node_graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness node_harness;
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));
  const editor::CompositorFrame selected =
      node_harness.frame(node_graph, 100.0F, 75.0F, true);
  UI_CHECK(selected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));

  // Re-clicking the same node is surfaced so the shell can restore the graph
  // as the active Delete-key domain.
  const editor::CompositorFrame reselected =
      node_harness.frame(node_graph, 100.0F, 75.0F, true);
  UI_CHECK(reselected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));

  const editor::CompositorFrame node_deleted = node_harness.frame(
      node_graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(node_deleted.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}});
  UI_CHECK(node_deleted.consume_delete);
  const editor::CompositorFrame idle =
      node_harness.frame(node_graph, 100.0F, 75.0F, false);
  UI_CHECK(!idle.remove.has_value());
  UI_CHECK(!idle.consume_delete);

  // The visible toolbar action returns the same compact intent.
  constexpr float remove_x = 849.0F;
  constexpr float remove_y = 13.0F;
  static_cast<void>(node_harness.frame(node_graph, remove_x, remove_y, false));
  static_cast<void>(node_harness.frame(node_graph, remove_x, remove_y, true));
  const editor::CompositorFrame button_deleted =
      node_harness.frame(node_graph, remove_x, remove_y, false);
  UI_CHECK(button_deleted.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}});

  compositor::CompositorGraph edge_graph;
  edge_graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  edge_graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  edge_graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  CompositorHarness edge_harness;
  constexpr float wire_x = 262.0F;
  constexpr float wire_y = 122.0F;
  static_cast<void>(edge_harness.frame(edge_graph, wire_x, wire_y, false));
  const editor::CompositorFrame edge_selected =
      edge_harness.frame(edge_graph, wire_x, wire_y, true);
  UI_CHECK(edge_selected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                       doc::NodeId{50U}});
  static_cast<void>(edge_harness.frame(edge_graph, wire_x, wire_y, false));
  const editor::CompositorFrame disconnected = edge_harness.frame(
      edge_graph, wire_x, wire_y, false, {}, gui::Key::Delete);
  UI_CHECK(disconnected.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Edge, doc::NodeId{50U}}});
  UI_CHECK(disconnected.consume_delete);
}

void led_car_process_returns_an_ordinary_node_delete_intent() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(7U, compositor::NodeKind::LedCarProcess, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame deleted =
      harness.frame(graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(deleted.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{7U}}});
  UI_CHECK(deleted.consume_delete);
}

void the_required_output_node_cannot_be_deleted() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame deleted =
      harness.frame(graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(!deleted.remove.has_value());
  UI_CHECK(deleted.consume_delete);
}

void project_replacement_clears_compositor_document_state() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;

  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  UI_CHECK(harness.panel.selection() ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});

  harness.panel.reset_project_state();
  UI_CHECK(harness.panel.selection() == editor::CompositorSelection{});
}

void compositor_navigation_zooms_under_the_pointer_and_pans_the_graph() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  constexpr float anchor_x = 450.0F;
  constexpr float anchor_y = 320.0F;
  constexpr float canvas_y = 26.0F;

  static_cast<void>(harness.navigation_frame(graph, anchor_x, anchor_y, false));
  static_cast<void>(
      harness.navigation_frame(graph, anchor_x, anchor_y, false, 2.0F));
  const float zoom = harness.panel.graph_zoom();
  const gui::UiPoint zoom_pan = harness.panel.graph_pan();
  UI_CHECK(zoom > 1.0F);
  UI_CHECK(std::abs((anchor_x - zoom_pan.x) / zoom - anchor_x) < 0.001F);
  UI_CHECK(std::abs((anchor_y - canvas_y - zoom_pan.y) / zoom -
                    (anchor_y - canvas_y)) < 0.001F);

  static_cast<void>(harness.navigation_frame(graph, anchor_x, anchor_y, true));
  static_cast<void>(harness.navigation_frame(graph, anchor_x + 120.0F,
                                             anchor_y + 60.0F, true));
  static_cast<void>(harness.navigation_frame(graph, anchor_x + 120.0F,
                                             anchor_y + 60.0F, false));
  const gui::UiPoint panned = harness.panel.graph_pan();
  UI_CHECK(std::abs(panned.x - (zoom_pan.x + 120.0F)) < 0.001F);
  UI_CHECK(std::abs(panned.y - (zoom_pan.y + 60.0F)) < 0.001F);

  const auto dropped = harness.panel.node_drop_position(
      anchor_x + 120.0F, anchor_y + 60.0F, 164.0F, 54.0F);
  UI_CHECK(dropped.has_value());
  UI_CHECK(std::abs(dropped->x -
                    ((anchor_x + 120.0F - panned.x) / zoom - 82.0F)) < 0.001F);
  UI_CHECK(std::abs(dropped->y -
                    ((anchor_y + 60.0F - canvas_y - panned.y) / zoom - 27.0F)) <
           0.001F);

  harness.panel.reset_project_state();
  UI_CHECK(harness.panel.graph_zoom() == 1.0F);
  UI_CHECK(harness.panel.graph_pan().x == 0.0F);
  UI_CHECK(harness.panel.graph_pan().y == 0.0F);
}

void zoomed_compositor_node_dragging_still_authors_graph_coordinates() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  static_cast<void>(harness.navigation_frame(graph, 100.0F, 80.0F, false));
  static_cast<void>(
      harness.navigation_frame(graph, 100.0F, 80.0F, false, 2.0F));

  const float zoom = harness.panel.graph_zoom();
  const gui::UiPoint pan = harness.panel.graph_pan();
  const float node_x = pan.x + 60.0F * zoom;
  const float node_y = 26.0F + pan.y + 40.0F * zoom;
  const float press_x = node_x + 40.0F;
  const float press_y = node_y + 10.0F;
  static_cast<void>(harness.frame(graph, press_x, press_y, false));
  static_cast<void>(harness.frame(graph, press_x, press_y, true));
  const float release_x = press_x + 30.0F * zoom;
  const float release_y = press_y + 20.0F * zoom;
  static_cast<void>(harness.frame(graph, release_x, release_y, true));
  const editor::CompositorFrame moved =
      harness.frame(graph, release_x, release_y, false);
  UI_CHECK(moved.move_node.has_value());
  UI_CHECK(std::abs(moved.move_node->editor_x - 90.0F) < 0.001F);
  UI_CHECK(std::abs(moved.move_node->editor_y - 60.0F) < 0.001F);
}

void dragging_a_compositor_node_reports_its_authored_position_on_release() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;

  // Canvas starts at y=26. Pressing at (100,75) is 40px/9px into the
  // node. Releasing at (160,135) therefore authors local (120,100).
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  const editor::CompositorFrame held =
      harness.frame(graph, 160.0F, 135.0F, true);
  UI_CHECK(!held.move_node.has_value());
  const editor::CompositorFrame result =
      harness.frame(graph, 160.0F, 135.0F, false);
  UI_CHECK(result.move_node.has_value());
  UI_CHECK(result.move_node->node == doc::NodeId{1U});
  UI_CHECK(result.move_node->editor_x == 120.0F);
  UI_CHECK(result.move_node->editor_y == 100.0F);
}

void compatible_typed_ports_report_a_connection_intent() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;

  // Both are EFFECT ports. Node width is 164; the first port centre is 36px
  // below the node top, and the canvas begins at y=26.
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, false));
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 122.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 122.0F, false);
  UI_CHECK(result.connect_ports.has_value());
  UI_CHECK(result.connect_ports->from ==
           compositor::EdgeEndpoint{doc::NodeId{1U}, 0U});
  UI_CHECK(result.connect_ports->to ==
           compositor::EdgeEndpoint{doc::NodeId{2U}, 0U});
  UI_CHECK(!result.add_node.has_value());
  UI_CHECK(!result.move_node.has_value());
}

void dropping_on_a_single_input_reports_an_atomic_rewire() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 40.0F, 180.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Output, 320.0F, 60.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{1U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{3U}, 0U}, 0});
  CompositorHarness harness;

  // Composite IMAGE OUT replaces the existing Layer -> Output edge when it
  // is dropped on Output's occupied single IMAGE input.
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, false));
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 122.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 122.0F, false);

  UI_CHECK(result.connect_ports ==
           editor::CompositorConnectIntent{
               compositor::EdgeEndpoint{doc::NodeId{2U}, 0U},
               compositor::EdgeEndpoint{doc::NodeId{3U}, 0U},
               doc::NodeId{50U}});
}

void incompatible_ports_are_rejected_and_persisted_edges_draw_wires() {
  compositor::CompositorGraph incompatible;
  incompatible.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  incompatible.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness incompatible_harness;
  static_cast<void>(
      incompatible_harness.frame(incompatible, 204.0F, 122.0F, false));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 204.0F, 122.0F, true));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 320.0F, 122.0F, true));
  const editor::CompositorFrame rejected =
      incompatible_harness.frame(incompatible, 320.0F, 122.0F, false);
  UI_CHECK(!rejected.connect_ports.has_value());

  compositor::CompositorGraph connected;
  connected.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  connected.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness draw_harness;
  static_cast<void>(draw_harness.frame(connected, 0.0F, 0.0F, false));
  const std::size_t without_wire = draw_harness.list.vertices().size();
  connected.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  static_cast<void>(draw_harness.frame(connected, 0.0F, 0.0F, false));
  UI_CHECK(draw_harness.list.vertices().size() > without_wire);
}

void compositor_diagnostics_are_explained_in_plain_language() {
  constexpr std::array<compositor::DiagnosticCode, 17U> codes{{
      compositor::DiagnosticCode::NodesOverCapacity,
      compositor::DiagnosticCode::EdgesOverCapacity,
      compositor::DiagnosticCode::DanglingNode,
      compositor::DiagnosticCode::DanglingSceneReference,
      compositor::DiagnosticCode::UnknownPort,
      compositor::DiagnosticCode::IncompatiblePorts,
      compositor::DiagnosticCode::DuplicateInput,
      compositor::DiagnosticCode::MissingRequiredInput,
      compositor::DiagnosticCode::MissingOutput,
      compositor::DiagnosticCode::DuplicateOutput,
      compositor::DiagnosticCode::DuplicateSceneLayerReference,
      compositor::DiagnosticCode::Cycle,
      compositor::DiagnosticCode::UnsupportedEffectOrder,
      compositor::DiagnosticCode::DeadBranch,
      compositor::DiagnosticCode::PassBudgetExceeded,
      compositor::DiagnosticCode::TextureBudgetExceeded,
      compositor::DiagnosticCode::TapBudgetExceeded,
  }};
  for (const compositor::DiagnosticCode code : codes) {
    const std::string_view message = editor::compositor_diagnostic_message(
        compositor::GraphDiagnostic{code, {}, {}, 0U});
    UI_CHECK(!message.empty());
    UI_CHECK(message.find("Graph is not ready") == std::string_view::npos);
  }
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MissingRequiredInput, {}, {}, 0U}) ==
           "A required input is not connected");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::IncompatiblePorts, {}, {}, 0U}) ==
           "Port types do not match");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MissingOutput, {}, {}, 0U}) ==
           "Add and connect one Output node");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::Cycle, {}, {}, 0U}) ==
           "Cycle detected; wires must flow toward Output");
  UI_CHECK(
      editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
          compositor::DiagnosticCode::UnsupportedEffectOrder, {}, {}, 0U}) ==
      "Inline effects must run before spatial filters");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::DuplicateSceneLayerReference,
               {},
               {},
               0U}) == "One active Scene Layer per source is supported");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::DeadBranch,
               {},
               {},
               0U,
               compositor::DiagnosticSeverity::Warning}) ==
           "This branch does not reach Output");
  UI_CHECK(
      editor::compositor_diagnostic_severity_label(compositor::GraphDiagnostic{
          compositor::DiagnosticCode::MissingOutput, {}, {}, 0U}) == "Error");
  UI_CHECK(
      editor::compositor_diagnostic_severity_label(compositor::GraphDiagnostic{
          compositor::DiagnosticCode::DeadBranch,
          {},
          {},
          0U,
          compositor::DiagnosticSeverity::Warning}) == "Warning");

  compositor::CompositorGraph ready;
  ready.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 180.0F, 90.0F));
  CompositorHarness ready_harness;
  static_cast<void>(ready_harness.frame(ready, 0.0F, 0.0F, false));
  const std::size_t without_guide = ready_harness.list.vertices().size();

  compositor::CompositorGraph invalid = ready;
  invalid.diagnostics.push_back(compositor::GraphDiagnostic{
      compositor::DiagnosticCode::MissingOutput, {}, {}, 0U});
  CompositorHarness invalid_harness;
  static_cast<void>(invalid_harness.frame(invalid, 0.0F, 0.0F, false));
  UI_CHECK(invalid_harness.list.vertices().size() > without_guide);

  compositor::CompositorGraph warning = ready;
  warning.diagnostics.push_back(
      compositor::GraphDiagnostic{compositor::DiagnosticCode::DeadBranch,
                                  doc::NodeId{10U},
                                  {},
                                  0U,
                                  compositor::DiagnosticSeverity::Warning});
  CompositorHarness warning_harness;
  static_cast<void>(warning_harness.frame(warning, 0.0F, 0.0F, false));
  UI_CHECK(warning_harness.list.vertices().size() > without_guide);
}

void brightness_parameter_typing_emits_one_typed_commit() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "2.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{70U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);
  UI_CHECK(std::get<float>(result.property_edit->value) == 2.5F);
  UI_CHECK(!result.editing);
  UI_CHECK(!result.add_node.has_value());
  UI_CHECK(!result.move_node.has_value());
  UI_CHECK(!result.connect_ports.has_value());
}

void compositor_parameter_nudges_report_full_semantic_identity() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  // Open the Amount field, then use a coarse keyboard repeat. It remains a
  // live numeric edit but carries enough identity for the Director's local
  // coalescing policy to distinguish it from every other stream.
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, false));
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, true));
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, false));
  const editor::CompositorFrame result = harness.frame(
      graph, 820.0F, 69.0F, false, {}, gui::Key::Up, gui::KeyMods::Shift);
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);
  UI_CHECK(std::abs(std::get<float>(result.property_edit->value) - 1.35F) <
           1.0e-5F);
  UI_CHECK(result.numeric_nudge.has_value());
  UI_CHECK(result.numeric_nudge->focus != gui::UiId{});
  UI_CHECK(
      result.numeric_nudge->property ==
      static_cast<core::u64>(editor::CompositorProperty::BrightnessAmount) +
          1U);
  UI_CHECK(result.numeric_nudge->direction ==
           editor::NumberFieldNudgeDirection::Increase);
  UI_CHECK(result.numeric_nudge->modifiers == gui::KeyMods::Shift);
}

void saturation_parameter_commits_are_clamped_to_four() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(73U, compositor::NodeKind::Saturation, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "9.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{73U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::SaturationAmount);
  UI_CHECK(std::get<float>(result.property_edit->value) ==
           compositor::max_saturation);
  UI_CHECK(!result.editing);
}

void levels_parameter_typing_emits_the_exact_channel_property() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "0.25");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{74U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::LevelsMasterInputBlack);
  UI_CHECK(std::get<float>(result.property_edit->value) == 0.25F);
  UI_CHECK(!result.editing);
}

void lift_gamma_gain_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(75U, compositor::NodeKind::LiftGammaGain, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  struct FieldCase final {
    float y;
    editor::CompositorProperty property;
    std::string_view authored;
    float expected;
  };
  constexpr std::array<FieldCase, 9U> fields{{
      {69.0F, editor::CompositorProperty::LiftGammaGainLiftR, "-9", -4.0F},
      {95.0F, editor::CompositorProperty::LiftGammaGainLiftG, "9", 4.0F},
      {121.0F, editor::CompositorProperty::LiftGammaGainLiftB, "-2", -2.0F},
      {147.0F, editor::CompositorProperty::LiftGammaGainGammaR, "0", 0.01F},
      {173.0F, editor::CompositorProperty::LiftGammaGainGammaG, "99", 10.0F},
      {199.0F, editor::CompositorProperty::LiftGammaGainGammaB, "2", 2.0F},
      {225.0F, editor::CompositorProperty::LiftGammaGainGainR, "-1", 0.0F},
      {251.0F, editor::CompositorProperty::LiftGammaGainGainG, "99", 16.0F},
      {277.0F, editor::CompositorProperty::LiftGammaGainGainB, "2", 2.0F},
  }};

  for (const FieldCase &field : fields) {
    const editor::CompositorFrame result = commit_compositor_field(
        harness, graph, 820.0F, field.y, field.authored);
    UI_CHECK(result.property_edit.has_value());
    UI_CHECK(result.property_edit->node == doc::NodeId{75U});
    UI_CHECK(result.property_edit->property == field.property);
    UI_CHECK(std::get<float>(result.property_edit->value) == field.expected);
    UI_CHECK(!result.editing);
  }
}

void contrast_pivot_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(76U, compositor::NodeKind::ContrastPivot, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame contrast =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "9");
  UI_CHECK(contrast.property_edit.has_value());
  UI_CHECK(contrast.property_edit->node == doc::NodeId{76U});
  UI_CHECK(contrast.property_edit->property ==
           editor::CompositorProperty::ContrastPivotContrast);
  UI_CHECK(std::get<float>(contrast.property_edit->value) == 4.0F);

  const editor::CompositorFrame pivot =
      commit_compositor_field(harness, graph, 820.0F, 95.0F, "-2");
  UI_CHECK(pivot.property_edit.has_value());
  UI_CHECK(pivot.property_edit->node == doc::NodeId{76U});
  UI_CHECK(pivot.property_edit->property ==
           editor::CompositorProperty::ContrastPivotPivot);
  UI_CHECK(std::get<float>(pivot.property_edit->value) == 0.0F);
}

void color_matrix_fields_emit_exact_typed_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(77U, compositor::NodeKind::ColorMatrix, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  struct FieldCase final {
    float y;
    editor::CompositorProperty property;
    std::string_view authored;
    float expected;
  };
  constexpr std::array<FieldCase, 12U> fields{{
      {69.0F, editor::CompositorProperty::ColorMatrixM00, "20", 16.0F},
      {95.0F, editor::CompositorProperty::ColorMatrixM01, "-20", -16.0F},
      {121.0F, editor::CompositorProperty::ColorMatrixM02, "0.2", 0.2F},
      {147.0F, editor::CompositorProperty::ColorMatrixM10, "0.3", 0.3F},
      {173.0F, editor::CompositorProperty::ColorMatrixM11, "1.1", 1.1F},
      {199.0F, editor::CompositorProperty::ColorMatrixM12, "0.4", 0.4F},
      {225.0F, editor::CompositorProperty::ColorMatrixM20, "0.5", 0.5F},
      {251.0F, editor::CompositorProperty::ColorMatrixM21, "0.6", 0.6F},
      {277.0F, editor::CompositorProperty::ColorMatrixM22, "1.2", 1.2F},
      {303.0F, editor::CompositorProperty::ColorMatrixOffsetR, "-0.1", -0.1F},
      {329.0F, editor::CompositorProperty::ColorMatrixOffsetG, "0.25", 0.25F},
      {355.0F, editor::CompositorProperty::ColorMatrixOffsetB, "20", 16.0F},
  }};

  for (const FieldCase &field : fields) {
    const editor::CompositorFrame result = commit_compositor_field(
        harness, graph, 820.0F, field.y, field.authored);
    UI_CHECK(result.property_edit.has_value());
    UI_CHECK(result.property_edit->node == doc::NodeId{77U});
    UI_CHECK(result.property_edit->property == field.property);
    UI_CHECK(std::get<float>(result.property_edit->value) == field.expected);
    UI_CHECK(!result.editing);
  }
}

void white_balance_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(78U, compositor::NodeKind::WhiteBalance, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame temperature =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "18000");
  UI_CHECK(temperature.property_edit.has_value());
  UI_CHECK(temperature.property_edit->node == doc::NodeId{78U});
  UI_CHECK(temperature.property_edit->property ==
           editor::CompositorProperty::WhiteBalanceTemperatureKelvin);
  UI_CHECK(std::get<float>(temperature.property_edit->value) ==
           scene::max_white_balance_temperature_kelvin);

  const editor::CompositorFrame tint =
      commit_compositor_field(harness, graph, 820.0F, 95.0F, "-9");
  UI_CHECK(tint.property_edit.has_value());
  UI_CHECK(tint.property_edit->node == doc::NodeId{78U});
  UI_CHECK(tint.property_edit->property ==
           editor::CompositorProperty::WhiteBalanceTint);
  UI_CHECK(std::get<float>(tint.property_edit->value) ==
           scene::min_white_balance_tint);
}

void curves_add_and_last_point_edit_emit_one_typed_program() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(79U, compositor::NodeKind::Curves, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  static_cast<void>(harness.frame(graph, 840.0F, 69.0F, false));
  static_cast<void>(harness.frame(graph, 840.0F, 69.0F, true));
  const editor::CompositorFrame added =
      harness.frame(graph, 840.0F, 69.0F, false);
  UI_CHECK(added.property_edit.has_value());
  UI_CHECK(added.property_edit->property ==
           editor::CompositorProperty::CurvesParameters);
  const auto &added_parameters =
      std::get<compositor::CurvesParameters>(added.property_edit->value);
  UI_CHECK(added_parameters.master.points.size() == 3U);
  UI_CHECK(scene::curves_is_identity(added_parameters));

  graph.nodes[0U].parameters = added_parameters;
  const editor::CompositorFrame changed =
      commit_compositor_field(harness, graph, 820.0F, 147.0F, "0.72");
  UI_CHECK(changed.property_edit.has_value());
  UI_CHECK(changed.property_edit->property ==
           editor::CompositorProperty::CurvesParameters);
  const auto &changed_parameters =
      std::get<compositor::CurvesParameters>(changed.property_edit->value);
  UI_CHECK(changed_parameters.master.points.size() == 3U);
  UI_CHECK(std::abs(changed_parameters.master.points[1U].y - 0.72F) < 1.0e-6F);
  UI_CHECK(!scene::curves_is_identity(changed_parameters));
}

void film_grain_fields_preserve_exact_integer_and_mode_types() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(80U, compositor::NodeKind::FilmGrain, 60.0F, 40.0F));

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    const editor::CompositorFrame amount =
        commit_compositor_field(harness, graph, 820.0F, 69.0F, "2");
    UI_CHECK(amount.property_edit.has_value());
    UI_CHECK(amount.property_edit->property ==
             editor::CompositorProperty::FilmGrainAmount);
    UI_CHECK(std::get<float>(amount.property_edit->value) == 1.0F);
  }

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    const editor::CompositorFrame seed =
        commit_compositor_field(harness, graph, 820.0F, 147.0F, "4294967295");
    UI_CHECK(seed.property_edit.has_value());
    UI_CHECK(seed.property_edit->property ==
             editor::CompositorProperty::FilmGrainSeed);
    UI_CHECK(std::get<core::u32>(seed.property_edit->value) == 4'294'967'295U);
  }

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    static_cast<void>(harness.frame(graph, 820.0F, 173.0F, false));
    static_cast<void>(harness.frame(graph, 820.0F, 173.0F, true));
    const editor::CompositorFrame mode =
        harness.frame(graph, 820.0F, 173.0F, false);
    UI_CHECK(mode.property_edit.has_value());
    UI_CHECK(mode.property_edit->property ==
             editor::CompositorProperty::FilmGrainMode);
    UI_CHECK(std::get<core::u32>(mode.property_edit->value) == 1U);
  }
}

void blur_parameter_commits_are_clamped_to_the_model_contract() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(71U, compositor::NodeKind::Blur, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "999");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{71U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BlurRadius);
  UI_CHECK(std::get<float>(result.property_edit->value) ==
           compositor::max_blur_radius);
}

void radial_array_parameters_emit_count_radius_angle_and_center_types() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(72U, compositor::NodeKind::RadialArray, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "17");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCount);
  UI_CHECK(std::get<core::u32>(result.property_edit->value) == 17U);

  result = commit_compositor_field(harness, graph, 820.0F, 95.0F, "8.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialRadius);
  UI_CHECK(std::get<float>(result.property_edit->value) == 8.5F);

  result = commit_compositor_field(harness, graph, 820.0F, 121.0F, "0.125");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialStartAngleTurns);
  UI_CHECK(std::get<float>(result.property_edit->value) == 0.125F);

  result = commit_compositor_field(harness, graph, 790.0F, 147.0F, "-2.25");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCenterX);
  UI_CHECK(std::get<float>(result.property_edit->value) == -2.25F);

  result = commit_compositor_field(harness, graph, 855.0F, 147.0F, "3.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCenterY);
  UI_CHECK(std::get<float>(result.property_edit->value) == 3.5F);
}

void workspace_panels_draw_scene_relevant_content() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  const std::vector<editor::ElementInfo> items{elements()};

  editor::CompositorPanel compositor{gui::ui_id("compositor.test")};
  input.new_frame();
  ctx.new_frame();
  compositor.draw(ctx, gui::Rect{0.0F, 0.0F, 900.0F, 190.0F}, items,
                  doc::NodeId{9U});
  UI_CHECK(list.vertices().size() > 300U);

  editor::OutputMonitorPanel monitor{gui::ui_id("monitor.test")};
  input.new_frame();
  ctx.new_frame();
  static_cast<void>(monitor.draw(
      ctx, gui::Rect{0.0F, 0.0F, 430.0F, 150.0F},
      editor::OutputMonitorState{240U, 3840U, 1080U, 60.0F, true}));
  // The monitor deliberately contains only the real Program output now; it
  // must still paint structured chrome and a truthful live state without
  // relying on fabricated destination rows or meters.
  UI_CHECK(list.vertices().size() > 50U);
  bool has_live_green = false;
  for (const gui::UiVertex &vertex : list.vertices()) {
    has_live_green = has_live_green || (vertex.g > 0.75F && vertex.r < 0.40F);
  }
  UI_CHECK(has_live_green);
}

struct OutputMonitorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.lifecycle.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 430.0F, 150.0F};
  gui::Rect panel_bounds{bounds};

  editor::OutputMonitorFrame frame(float x, float y, bool down,
                                   const editor::OutputMonitorState &state,
                                   float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, panel_bounds, state);
  }
};

void output_monitor_requests_conformance_without_a_lifecycle_action() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState state{};
  state.cluster.conformance_active = true;
  harness.panel_bounds.height = 240.0F;
  // The completed-frame summary sits between Program and the cohort action.
  const float y = 134.0F;
  static_cast<void>(harness.frame(60.0F, y, true, state));
  const auto clicked = harness.frame(60.0F, y, false, state);
  UI_CHECK(clicked.check_render_conformance);
  UI_CHECK(!clicked.lifecycle.has_value());
  UI_CHECK(!harness.frame(60.0F, y, false, state).check_render_conformance);
  state.cluster.conformance_active = false;
  static_cast<void>(harness.frame(60.0F, y, true, state));
  UI_CHECK(!harness.frame(60.0F, y, false, state).check_render_conformance);
}

void output_monitor_emits_one_fixed_lifecycle_intent() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{50U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->endpoint == "127.0.0.1:47840");
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Stop);
}

void duplicate_node_indices_have_no_ambiguous_controls() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::DuplicateNodeIndex,
      0U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
}

void missing_cluster_assets_block_start_but_keep_status_visible() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Stopped,
      0U,
      {}};
  node.content = editor::OutputMonitorState::Node::ContentStatus::Missing;
  node.assets_required = 2U;
  node.assets_ready = 1U;
  node.assets_missing = 1U;
  node.content_detail = "one required object is absent";
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
  UI_CHECK(harness.list.vertices().size() > 50U);
}

void content_labels_distinguish_missing_stale_and_verification_failures() {
  editor::OutputMonitorState::Node node{};
  node.content = editor::OutputMonitorState::Node::ContentStatus::Missing;
  node.assets_missing = 2U;
  node.assets_mismatched = 3U;
  UI_CHECK(editor::output_node_content_label(node) ==
           "ASSETS MISSING 2 / STALE 3");

  node.content = editor::OutputMonitorState::Node::ContentStatus::Error;
  node.assets_verification_failed = 4U;
  UI_CHECK(editor::output_node_content_label(node) == "ASSET VERIFY FAILED 4");
}

void output_monitor_scroll_reaches_later_cluster_nodes() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true};
  std::vector<editor::OutputMonitorState::Node> nodes;
  for (core::u32 index = 0U; index < 4U; ++index) {
    nodes.push_back(editor::OutputMonitorState::Node{
        doc::NodeId{60U + index},
        "Render",
        "127.0.0.1:47840",
        index,
        1920U,
        1080U,
        editor::OutputMonitorState::Node::Status::Stopped,
        0U,
        {}});
  }
  state.nodes = nodes;
  // Scroll the list to its lower bound, then click START on the last row.
  static_cast<void>(harness.frame(400.0F, 90.0F, false, state, -10.0F));
  static_cast<void>(harness.frame(400.0F, 120.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 120.0F, true, state));
  const auto released = harness.frame(400.0F, 120.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{63U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Start);
}

void output_monitor_emits_explicit_cluster_relaunch() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(320.0F, 140.0F, false, state));
  static_cast<void>(harness.frame(320.0F, 140.0F, true, state));
  const auto released = harness.frame(320.0F, 140.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->all);
  UI_CHECK(released.lifecycle->device == doc::NodeId{});
  UI_CHECK(released.lifecycle->endpoint.empty());
  UI_CHECK(released.lifecycle->action ==
           editor::OutputLifecycleAction::Restart);
}

void output_monitor_cluster_health_keeps_node_controls_reachable() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.cluster.health.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 430.0F, 190.0F};
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  state.cluster.health = editor::OutputMonitorState::Cluster::Health::Healthy;
  state.cluster.configured_nodes = 1U;
  state.cluster.live_sessions = 1U;
  state.cluster.diagnostics_recording = true;

  const auto frame = [&](bool down) {
    input.new_frame();
    input.set_mouse_position(400.0F, 128.0F);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  };
  static_cast<void>(frame(false));
  static_cast<void>(frame(true));
  const auto released = frame(false);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Stop);
}

// The Preferences dialog is modal and covers the whole window, so its scrim
// competes with its own controls for the pointer. gui::UiContext resolves a
// frame first-wins, which once made every control in the card unreachable.
struct PreferencesHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::PreferencesPanel panel{gui::ui_id("preferences.test")};
  editor::EditorPreferencesState state;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 1600.0F, 900.0F};

  // The card is centred in `bounds`; these mirror the panel's own layout.
  static constexpr float card_width = 620.0F;
  static constexpr float card_height = 780.0F;
  static constexpr float card_x = (1600.0F - card_width) * 0.5F;
  static constexpr float card_y = (900.0F - card_height) * 0.5F;

  [[nodiscard]] static gui::Rect cancel_button() {
    return gui::Rect{card_x + card_width - 190.0F, card_y + card_height - 38.0F,
                     84.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect save_button() {
    return gui::Rect{card_x + card_width - 98.0F, card_y + card_height - 38.0F,
                     84.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect first_field() {
    return gui::Rect{card_x + 14.0F + 132.0F, card_y + 66.0F,
                     card_width - 28.0F - 132.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect next_to_source_checkbox() {
    return gui::Rect{card_x + 14.0F, card_y + 332.0F, card_width - 28.0F,
                     24.0F};
  }
  [[nodiscard]] static gui::Rect clear_proxy_cache_button() {
    return gui::Rect{card_x + 14.0F, card_y + 456.0F, 118.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect low_latency_preset() {
    const float width = (card_width - 28.0F - 18.0F) / 4.0F;
    return gui::Rect{card_x + 14.0F, card_y + 524.0F, width, 24.0F};
  }
  [[nodiscard]] static gui::Rect balanced_transcode_priority() {
    const float start = card_x + 14.0F + 132.0F;
    const float width = (card_x + card_width - 14.0F - start - 12.0F) / 3.0F;
    return gui::Rect{start + width + 6.0F, card_y + 424.0F, width, 24.0F};
  }

  editor::PreferencesFrame frame(float x, float y, bool down,
                                 std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }

  // Press and release over one point, which is what gui::UiContext turns into
  // a click.
  editor::PreferencesFrame click(const gui::Rect &target) {
    const float x = target.x + target.width * 0.5F;
    const float y = target.y + target.height * 0.5F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

[[nodiscard]] editor::EditorPreferences stored_preferences() {
  editor::EditorPreferences preferences{};
  preferences.director_listen = "0.0.0.0:45060";
  preferences.show_id = "0000000000000001000000000000000B";
  return preferences;
}

void cancel_closes_the_preferences_dialog() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);
  UI_CHECK(harness.panel.is_open());

  const auto result = harness.click(PreferencesHarness::cancel_button());
  UI_CHECK(!result.save.has_value());
  UI_CHECK(!harness.panel.is_open());
}

void cancel_discards_edits_so_reopening_shows_the_stored_values() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // Focus the first field and type into it, then cancel.
  static_cast<void>(harness.click(PreferencesHarness::first_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "9"));
  UI_CHECK(harness.panel.draft() != harness.state.stored);
  static_cast<void>(harness.click(PreferencesHarness::cancel_button()));
  UI_CHECK(!harness.panel.is_open());

  harness.panel.open(harness.state.stored);
  UI_CHECK(harness.panel.draft() == harness.state.stored);
}

void save_reports_the_edited_values_and_closes() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(harness.click(PreferencesHarness::first_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "9"));
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->director_listen !=
           harness.state.stored.director_listen);
  UI_CHECK(result.save->show_id == harness.state.stored.show_id);
  UI_CHECK(!harness.panel.is_open());
}

void save_stays_disabled_until_something_actually_changes() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // Nothing edited yet, so Save must neither fire nor dismiss the card.
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(!result.save.has_value());
  UI_CHECK(harness.panel.is_open());
}

void proxy_storage_mode_is_part_of_the_saved_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(
      harness.click(PreferencesHarness::next_to_source_checkbox()));
  UI_CHECK(harness.panel.draft().proxy_cache_next_to_source);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->proxy_cache_next_to_source);
}

void clear_proxy_cache_reports_without_saving_or_closing() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  const auto result =
      harness.click(PreferencesHarness::clear_proxy_cache_button());
  UI_CHECK(result.clear_proxy_cache);
  UI_CHECK(!result.save.has_value());
  UI_CHECK(harness.panel.is_open());
}

void playback_preset_is_part_of_the_saved_machine_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(harness.click(PreferencesHarness::low_latency_preset()));
  UI_CHECK(harness.panel.draft().output_buffer_frames == 3U);
  UI_CHECK(harness.panel.draft().output_read_ahead_frames == 3U);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->output_buffer_frames == 3U);
  UI_CHECK(result.save->output_read_ahead_frames == 3U);
}

void transcode_priority_is_part_of_the_saved_machine_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(
      harness.click(PreferencesHarness::balanced_transcode_priority()));
  UI_CHECK(harness.panel.draft().proxy_transcode_priority ==
           editor::EditorPreferences::TranscodePriority::Balanced);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->proxy_transcode_priority ==
           editor::EditorPreferences::TranscodePriority::Balanced);
}

void only_a_click_outside_the_card_dismisses_it() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // The card's own background is inert: a stray click inside must not throw
  // away whatever the operator has typed so far.
  static_cast<void>(harness.click(gui::Rect{
      PreferencesHarness::card_x + 20.0F,
      PreferencesHarness::card_y + PreferencesHarness::card_height - 70.0F,
      10.0F, 10.0F}));
  UI_CHECK(harness.panel.is_open());

  static_cast<void>(harness.click(gui::Rect{4.0F, 4.0F, 10.0F, 10.0F}));
  UI_CHECK(!harness.panel.is_open());
}

// The Project Settings dialog shares Preferences' modal card geometry, and
// unlike Preferences its result is a document edit.
struct ProjectSettingsHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ProjectSettingsPanel panel{gui::ui_id("project-settings.test")};
  editor::ProjectSettingsState state;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 1600.0F, 900.0F};
  static constexpr float card_width = 620.0F;
  static constexpr float card_height = 700.0F;
  static constexpr float card_x = (1600.0F - card_width) * 0.5F;
  static constexpr float card_y = (900.0F - card_height) * 0.5F;
  static constexpr float tab_y = card_y + 64.0F;
  static constexpr float tab_height = 24.0F;
  static constexpr float tab_gap = 6.0F;
  static constexpr float category_count = 8.0F;
  static constexpr float tab_width =
      (card_width - 28.0F - tab_gap * (category_count - 1.0F))
      / category_count;
  static constexpr float content_y = card_y + 96.0F;

  [[nodiscard]] static gui::Rect defaults_button() {
    return gui::Rect{card_x + 14.0F, card_y + card_height - 38.0F, 84.0F,
                     26.0F};
  }
  [[nodiscard]] static gui::Rect cancel_button() {
    return gui::Rect{card_x + card_width - 190.0F, card_y + card_height - 38.0F,
                     84.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect apply_button() {
    return gui::Rect{card_x + card_width - 98.0F, card_y + card_height - 38.0F,
                     84.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect category_tab(std::size_t index) {
    return gui::Rect{card_x + 14.0F +
                         static_cast<float>(index) * (tab_width + tab_gap),
                     tab_y, tab_width, tab_height};
  }
  [[nodiscard]] static gui::Rect renderer_tab() { return category_tab(0U); }
  [[nodiscard]] static gui::Rect image_tab() { return category_tab(1U); }
  // Colour sits beside Image: both are how the finished picture is treated.
  [[nodiscard]] static gui::Rect color_tab() { return category_tab(2U); }
  // Environment sits between Colour and Tracking: it is content the operator
  // sets alongside the look, not a timing or calibration policy.
  [[nodiscard]] static gui::Rect environment_tab() {
    return category_tab(3U);
  }
  [[nodiscard]] static gui::Rect tracking_tab() { return category_tab(4U); }
  [[nodiscard]] static gui::Rect lens_tab() { return category_tab(5U); }
  // COLOUR: a DISPLAY TRANSFORM heading then the transform row, a SHOW LOOK
  // heading then the field row, an OUTPUT heading then the dither checkbox.
  [[nodiscard]] static gui::Rect srgb_transform_button() {
    return gui::Rect{card_x + 14.0F + 150.0F, content_y + 22.0F, 116.0F,
                     22.0F};
  }
  [[nodiscard]] static gui::Rect dither_checkbox() {
    return gui::Rect{card_x + 14.0F,
                     content_y + 22.0F + 44.0F + 22.0F + 44.0F + 22.0F,
                     card_width - 28.0F, 24.0F};
  }
  // Gaussian Blur is the first section: a 22px heading then three 44px rows.
  // Directional Blur contributes its heading and one row before Brightness.
  [[nodiscard]] static gui::Rect gain_button() {
    const float y =
        content_y + 22.0F + 44.0F * 3.0F + 4.0F + 22.0F + 44.0F + 4.0F + 22.0F;
    return gui::Rect{card_x + 14.0F + 150.0F + 124.0F, y, 116.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect advanced_button() {
    const float y = content_y + 22.0F;
    return gui::Rect{card_x + 14.0F + 150.0F + 124.0F, y, 116.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect required_button() {
    const float y = content_y + 22.0F + 44.0F;
    return gui::Rect{card_x + 14.0F + 150.0F + 124.0F, y, 116.0F, 22.0F};
  }
  // The two shadow rows follow Backend, Availability, and Profile, and are
  // drawn only while Advanced is selected.
  [[nodiscard]] static gui::Rect shadow_cascades_track() {
    const float y = content_y + 22.0F + 44.0F * 3.0F;
    return gui::Rect{card_x + 14.0F + 150.0F, y,
                     card_width - 28.0F - 150.0F - 66.0F - 8.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect gi_strength_track() {
    const float y = content_y + 22.0F + 44.0F * 4.0F;
    return gui::Rect{card_x + 14.0F + 150.0F, y,
                     card_width - 28.0F - 150.0F - 66.0F - 8.0F, 22.0F};
  }
  // The antialiasing row follows Backend, Availability, Profile, Shadow
  // cascades and Bounced light.
  [[nodiscard]] static gui::Rect antialiasing_button(int index) {
    const float y = content_y + 22.0F + 44.0F * 5.0F;
    return gui::Rect{card_x + 14.0F + 150.0F
                         + (96.0F + 8.0F) * static_cast<float>(index),
                     y, 96.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect jitter_samples_track() {
    const float y = content_y + 22.0F + 44.0F * 6.0F;
    return gui::Rect{card_x + 14.0F + 150.0F, y,
                     card_width - 28.0F - 150.0F - 66.0F - 8.0F, 22.0F};
  }
  // Follows the antialiasing row, and the jitter row when that is drawn.
  [[nodiscard]] static gui::Rect occlusion_radius_track(bool with_jitter) {
    const float y =
        content_y + 22.0F + 44.0F * (with_jitter ? 7.0F : 6.0F);
    return gui::Rect{card_x + 14.0F + 150.0F, y,
                     card_width - 28.0F - 150.0F - 66.0F - 8.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect raster_only_button() {
    const float profile_y = content_y + 22.0F + 44.0F * 2.0F;
    return gui::Rect{card_x + card_width - 14.0F - 104.0F, profile_y + 24.0F,
                     104.0F, 18.0F};
  }
  [[nodiscard]] static gui::Rect calibrated_lens_button() {
    const float y = content_y + 22.0F;
    return gui::Rect{card_x + 14.0F + 150.0F, y, 116.0F, 22.0F};
  }
  [[nodiscard]] static gui::Rect lens_sensor_width_track() {
    const float y = content_y + 22.0F + 44.0F;
    constexpr float lens_gap = 10.0F;
    constexpr float lens_label_width = 66.0F;
    constexpr float lens_value_width = 48.0F;
    const float lens_column_width = (card_width - 28.0F - lens_gap) * 0.5F;
    return gui::Rect{
        card_x + 14.0F + lens_label_width + 2.0F, y,
        lens_column_width - lens_label_width - lens_value_width - 6.0F, 18.0F};
  }
  // INNER FRUSTUM heading, then the enable checkbox and the two plate rows.
  [[nodiscard]] static gui::Rect inner_frustum_checkbox() {
    return gui::Rect{card_x + 14.0F, content_y + 22.0F, card_width - 28.0F,
                     24.0F};
  }

  editor::ProjectSettingsFrame frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }

  editor::ProjectSettingsFrame click(const gui::Rect &target) {
    const float x = target.x + target.width * 0.5F;
    const float y = target.y + target.height * 0.5F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

void project_settings_apply_reports_the_edited_values() {
  ProjectSettingsHarness harness;
  harness.state.settings = scene::EffectSettings{};
  harness.state.node = doc::NodeId{42U};
  harness.panel.open(harness.state.settings, harness.state.renderer);
  UI_CHECK(harness.panel.is_open());

  // Nothing changed yet, so Apply is inert and the card stays up.
  UI_CHECK(
      !harness.click(ProjectSettingsHarness::apply_button()).apply.has_value());
  UI_CHECK(harness.panel.is_open());

  static_cast<void>(harness.click(ProjectSettingsHarness::image_tab()));
  static_cast<void>(harness.click(ProjectSettingsHarness::gain_button()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->settings.brightness_curve ==
           scene::BrightnessCurve::Gain);
  UI_CHECK(!harness.panel.is_open());
}

void baked_lighting_page_starts_cancels_and_preserves_the_authored_field() {
  ProjectSettingsHarness harness;
  harness.state.stage_node = doc::NodeId{42};
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  auto& baked = harness.state.environment.baked;
  baked.enabled = true;
  baked.source = "lighting.1234567890abcdef.msgi";
  baked.content = "1234567890abcdef";
  baked.count_x = 3;
  harness.panel.open(harness.state.settings, harness.state.renderer, {}, harness.state.environment);
  static_cast<void>(harness.click(ProjectSettingsHarness::environment_tab()));
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x + 162, ProjectSettingsHarness::content_y + 6, 160, 24}));
  constexpr float button_y = ProjectSettingsHarness::content_y + 38 + 22 + 30 + 28 + 30;
  const auto start = harness.click({ProjectSettingsHarness::card_x + 14, button_y, 160, 26});
  UI_CHECK(start.bake && *start.bake == baked);
  UI_CHECK(!start.bake_destination.empty());
  harness.state.bake_running = true;
  UI_CHECK(!harness.click({ProjectSettingsHarness::card_x + 14, button_y, 160, 26}).bake);
  UI_CHECK(harness.click({ProjectSettingsHarness::card_x + 184, button_y, 100, 26}).cancel_bake);
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x + 14, ProjectSettingsHarness::content_y + 38 + 22, 16, 24}));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply && !applied.apply->environment.baked.enabled);
  UI_CHECK(applied.apply->environment.baked.source == baked.source);
  UI_CHECK(applied.apply->environment.baked.count_x == 3);
}

void project_settings_cancel_discards_the_draft() {
  ProjectSettingsHarness harness;
  harness.state.settings = scene::EffectSettings{};
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::image_tab()));
  static_cast<void>(harness.click(ProjectSettingsHarness::gain_button()));
  const auto cancelled = harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!cancelled.apply.has_value());
  UI_CHECK(!harness.panel.is_open());

  // Reopening seeds from the project again, so the discarded edit is gone.
  harness.panel.open(harness.state.settings, harness.state.renderer);
  const auto reapplied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(!reapplied.apply.has_value());
}

void project_settings_defaults_restores_the_built_in_values() {
  ProjectSettingsHarness harness;
  harness.state.settings = scene::EffectSettings{};
  harness.state.settings.blur_taps = 21U;
  harness.state.settings.brightness_curve = scene::BrightnessCurve::Gain;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::defaults_button()));
  UI_CHECK(harness.panel.is_open());
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->settings == scene::EffectSettings{});
  UI_CHECK(applied.apply->renderer == scene::RendererSettings{});
}

void advanced_renderer_required_is_an_explicit_project_edit() {
  ProjectSettingsHarness harness;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::advanced_button()));
  static_cast<void>(harness.click(ProjectSettingsHarness::required_button()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->renderer.backend == scene::RendererBackend::Advanced);
  UI_CHECK(applied.apply->renderer.required);
}

void raster_only_clears_only_unavailable_future_pass_requests() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.state.renderer.required = true;
  harness.state.renderer.quality = scene::RendererQuality::Cinematic;
  harness.state.renderer.ray_tracing = true;
  harness.state.renderer.ddgi = true;
  harness.state.renderer.restir = true;
  harness.state.renderer.denoising = true;
  harness.state.renderer.taau = true;
  harness.state.renderer.motion_blur = true;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(
      harness.click(ProjectSettingsHarness::raster_only_button()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  const scene::RendererSettings &renderer = applied.apply->renderer;
  UI_CHECK(renderer.backend == scene::RendererBackend::Advanced);
  UI_CHECK(renderer.required);
  UI_CHECK(renderer.quality == scene::RendererQuality::Cinematic);
  UI_CHECK(!renderer.ray_tracing);
  UI_CHECK(!renderer.ddgi);
  UI_CHECK(!renderer.restir);
  UI_CHECK(!renderer.denoising);
  UI_CHECK(renderer.motion_blur);
  // Temporal antialiasing survives. The button clears requests for passes
  // that do not exist; this one does, so clearing it would be discarding a
  // working feature rather than unblocking the renderer.
  UI_CHECK(renderer.taau);
}

// Occlusion is authored as a radius, and zero is the off.
//
// A toggle beside a value would be two ways to say the same thing, and the
// renderer only reads one of them - so the panel offers exactly the control
// the renderer obeys.
void occlusion_is_authored_as_a_radius() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(
      ProjectSettingsHarness::occlusion_radius_track(false)));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  // The midpoint of a 0..8 slider.
  UI_CHECK(applied.apply->renderer.occlusion_radius > 3.5F);
  UI_CHECK(applied.apply->renderer.occlusion_radius < 4.5F);

  // Dragging it back to zero is reachable and survives Apply: it is the
  // authored "no occlusion", not a value the panel clamps away.
  ProjectSettingsHarness off;
  off.state.renderer.backend = scene::RendererBackend::Advanced;
  off.state.renderer.occlusion_radius = 2.0F;
  off.panel.open(off.state.settings, off.state.renderer);
  // Centred on the track's left edge. The slider clamps its position at zero
  // only at or left of that edge, so a click one pixel inside lands on a tiny
  // positive radius - which the renderer would treat as occlusion switched on.
  // Dragging to the edge is what an operator does, and it has to reach zero.
  const gui::Rect track =
      ProjectSettingsHarness::occlusion_radius_track(false);
  static_cast<void>(off.click(
      gui::Rect{track.x - 1.0F, track.y, 2.0F, track.height}));
  const auto cleared = off.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(cleared.apply.has_value());
  UI_CHECK(cleared.apply->renderer.occlusion_radius == 0.0F);
}

// Antialiasing is one choice, authored, with the jitter period behind it.
//
// The panel stores two booleans and the renderer reads one decision, so the
// thing worth asserting is that they cannot disagree: no click can leave both
// set, and what the buttons show is what `antialiasing_for` returns.
void antialiasing_is_one_authored_choice() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(
      harness.click(ProjectSettingsHarness::antialiasing_button(2)));
  const auto temporal =
      harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(temporal.apply.has_value());
  UI_CHECK(temporal.apply->renderer.taau);
  UI_CHECK(!temporal.apply->renderer.spatial_antialiasing);
  UI_CHECK(scene::antialiasing_for(temporal.apply->renderer)
           == scene::AntialiasingMode::Temporal);

  // Choosing the spatial trade clears the temporal one rather than adding to
  // it. Both set would still resolve to Temporal, so an operator who picked
  // SPATIAL would get the pass they were trying to avoid.
  ProjectSettingsHarness spatial;
  spatial.state.renderer.backend = scene::RendererBackend::Advanced;
  spatial.state.renderer.taau = true;
  spatial.panel.open(spatial.state.settings, spatial.state.renderer);
  static_cast<void>(
      spatial.click(ProjectSettingsHarness::antialiasing_button(1)));
  const auto applied = spatial.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(!applied.apply->renderer.taau);
  UI_CHECK(applied.apply->renderer.spatial_antialiasing);
  UI_CHECK(scene::antialiasing_for(applied.apply->renderer)
           == scene::AntialiasingMode::Spatial);

  // And off is reachable from either.
  ProjectSettingsHarness none;
  none.state.renderer.backend = scene::RendererBackend::Advanced;
  none.state.renderer.spatial_antialiasing = true;
  none.panel.open(none.state.settings, none.state.renderer);
  static_cast<void>(none.click(ProjectSettingsHarness::antialiasing_button(0)));
  const auto cleared = none.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(cleared.apply.has_value());
  UI_CHECK(scene::antialiasing_for(cleared.apply->renderer)
           == scene::AntialiasingMode::None);

  // The jitter period is drawn only while the temporal pass is selected: it
  // describes a sub-pixel sequence nothing else consumes.
  ProjectSettingsHarness period;
  period.state.renderer.backend = scene::RendererBackend::Advanced;
  period.state.renderer.taau = true;
  period.panel.open(period.state.settings, period.state.renderer);
  static_cast<void>(
      period.click(ProjectSettingsHarness::jitter_samples_track()));
  const auto sampled = period.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(sampled.apply.has_value());
  // The midpoint of a 2..64 slider.
  UI_CHECK(sampled.apply->renderer.taa_sequence_length == 33U);
}

void geometry_controls_emit_replicated_settings() {
  ProjectSettingsHarness harness;
  harness.panel.open(scene::EffectSettings{}, scene::RendererSettings{});
  static_cast<void>(harness.click(gui::Rect{
      ProjectSettingsHarness::card_x + 14.0F + 6.0F * (ProjectSettingsHarness::tab_width + ProjectSettingsHarness::tab_gap),
      ProjectSettingsHarness::tab_y, ProjectSettingsHarness::tab_width, ProjectSettingsHarness::tab_height}));
  for (int i=0;i<3;++i) {
    static_cast<void>(harness.click(gui::Rect{ProjectSettingsHarness::card_x+164.0F,
        ProjectSettingsHarness::content_y+22.0F+44.0F*static_cast<float>(i),116.0F,22.0F}));
  }
  const auto result=harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply.has_value());
  UI_CHECK(!result.apply->renderer.frustum_culling);
  UI_CHECK(!result.apply->renderer.geometry_instancing);
  UI_CHECK(!result.apply->renderer.model_lod);
}

void object_motion_controls_apply_and_cancel() {
  ProjectSettingsHarness harness;
  harness.panel.open(harness.state.settings, harness.state.renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::category_tab(6U)));
  constexpr float toggle_y = ProjectSettingsHarness::content_y + 352.0F;
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x + 164.0F, toggle_y, 116.0F, 22.0F}));
  // Quarter of the 0..64 radius track, distinctly different from the default.
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x + 164.0F + 92.0F,
      toggle_y + 44.0F, 1.0F, 22.0F}));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply && applied.apply->renderer.motion_blur);
  UI_CHECK(applied.apply->renderer.motion_blur_radius == 16U);
  harness.state.renderer = applied.apply->renderer;
  harness.panel.open(harness.state.settings, applied.apply->renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::category_tab(6U)));
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x + 164.0F, toggle_y, 116.0F, 22.0F}));
  UI_CHECK(!harness.click(ProjectSettingsHarness::cancel_button()).apply);
  harness.panel.open(harness.state.settings, applied.apply->renderer);
  UI_CHECK(!harness.click(ProjectSettingsHarness::apply_button()).apply);
}

void plate_dof_controls_apply_and_cancel() {
  ProjectSettingsHarness h;
  h.panel.open(h.state.settings, h.state.renderer);
  static_cast<void>(h.click(ProjectSettingsHarness::category_tab(6U)));
  constexpr float y = ProjectSettingsHarness::content_y + 418.0F;
  static_cast<void>(h.click({ProjectSettingsHarness::card_x + 164.0F, y, 116.0F, 22.0F}));
  static_cast<void>(h.click({ProjectSettingsHarness::card_x + 256.0F, y + 44.0F, 1.0F, 22.0F}));
  const auto result = h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply && result.apply->renderer.plate_depth_of_field);
  UI_CHECK(result.apply->renderer.plate_dof_radius == 16U);
  h.state.renderer = result.apply->renderer;
  h.panel.open(h.state.settings, h.state.renderer);
  static_cast<void>(h.click(ProjectSettingsHarness::category_tab(6U)));
  static_cast<void>(h.click({ProjectSettingsHarness::card_x + 164.0F, y, 116.0F, 22.0F}));
  UI_CHECK(!h.click(ProjectSettingsHarness::cancel_button()).apply);
  h.panel.open(h.state.settings, h.state.renderer);
  UI_CHECK(!h.click(ProjectSettingsHarness::apply_button()).apply);
}

void texture_sampling_controls_apply_and_cancel() {
  ProjectSettingsHarness harness;
  harness.panel.open(harness.state.settings, harness.state.renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::category_tab(6U)));
  constexpr float sampling_y = ProjectSettingsHarness::content_y + 22.0F + 3.0F*44.0F + 22.0F;
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x+164.0F+3.0F*42.0F,sampling_y,38.0F,22.0F}));
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x+184.0F,sampling_y+44.0F,1.0F,22.0F}));
  auto result=harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply.has_value());
  UI_CHECK(result.apply->renderer.texture_anisotropy==8U);
  UI_CHECK(result.apply->renderer.texture_lod_bias < -1.0F);
  harness.panel.open(harness.state.settings,result.apply->renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::category_tab(6U)));
  static_cast<void>(harness.click({ProjectSettingsHarness::card_x+164.0F,sampling_y,38.0F,22.0F}));
  UI_CHECK(!harness.click(ProjectSettingsHarness::cancel_button()).apply);
}

void texture_streaming_controls_emit_authored_policy() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.panel.open(harness.state.settings, harness.state.renderer);
  static_cast<void>(harness.click(gui::Rect{
      ProjectSettingsHarness::card_x + 14.0F + 6.0F * (ProjectSettingsHarness::tab_width + ProjectSettingsHarness::tab_gap),
      ProjectSettingsHarness::tab_y, ProjectSettingsHarness::tab_width, ProjectSettingsHarness::tab_height}));
  static_cast<void>(harness.click(gui::Rect{ProjectSettingsHarness::card_x+164.0F,
      ProjectSettingsHarness::content_y+22.0F+3.0F*44.0F+22.0F,116.0F,22.0F}));
  const auto result = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply.has_value());
  UI_CHECK(!result.apply->renderer.texture_streaming);
  UI_CHECK(result.apply->renderer.texture_budget_mib == 512U);
}

void shadow_cascades_and_bounce_are_authored_beside_the_backend() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  // Both controls appear only under Advanced, which is the only renderer that
  // draws them, and both are ordinary project state the operator can copy
  // between shows rather than a machine-local quality setting.
  static_cast<void>(
      harness.click(ProjectSettingsHarness::shadow_cascades_track()));
  static_cast<void>(harness.click(ProjectSettingsHarness::gi_strength_track()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  const scene::RendererSettings &renderer = applied.apply->renderer;
  UI_CHECK(renderer.backend == scene::RendererBackend::Advanced);
  // The midpoint of a 0..4 slider is 2, and of a 0..16 slider is 8.
  UI_CHECK(renderer.shadow_cascades == 2U);
  UI_CHECK(renderer.gi_strength > 7.5F && renderer.gi_strength < 8.5F);

  // Zero cascades is reachable and survives Apply: it is the authored "no
  // directional shadows", not a value the panel clamps away.
  ProjectSettingsHarness off;
  off.state.renderer.backend = scene::RendererBackend::Advanced;
  off.panel.open(off.state.settings, off.state.renderer);
  const gui::Rect cascades = ProjectSettingsHarness::shadow_cascades_track();
  static_cast<void>(
      off.click(gui::Rect{cascades.x, cascades.y, 1.0F, cascades.height}));
  const auto none = off.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(none.apply.has_value());
  UI_CHECK(none.apply->renderer.shadow_cascades == 0U);
}

void calibrated_lens_controls_author_their_profile() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.state.camera_node = doc::NodeId{99U};
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile);

  static_cast<void>(harness.click(ProjectSettingsHarness::lens_tab()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::calibrated_lens_button()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::lens_sensor_width_track()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->lens_profile.has_value());
  UI_CHECK(applied.apply->lens_profile->sensor_width_mm > 90.0F);
  UI_CHECK(applied.apply->lens_profile->sensor_width_mm < 110.0F);
}

// The page authors the show-wide plate policy. Where a tracker listens is not
// here: that is a property of one feed, authored on the feed's own node beside
// the camera it drives.
void the_tracking_category_authors_the_inner_frustum_policy() {
  ProjectSettingsHarness harness;
  harness.state.renderer.icvfx.camera_defaults.margin_px = 32U;
  harness.state.renderer.icvfx.latency_frames = 3U;
  harness.state.renderer.icvfx.prediction_window = 9U;
  harness.state.renderer.icvfx.jitter_filter_percent = 63U;
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile);

  static_cast<void>(harness.click(ProjectSettingsHarness::tracking_tab()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  const gui::Rect predict{ProjectSettingsHarness::card_x + 14.0F,
      ProjectSettingsHarness::content_y + 22.0F + 34.0F + 44.0F,
      ProjectSettingsHarness::card_width - 28.0F, 24.0F};
  static_cast<void>(harness.click(predict));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->renderer.icvfx.inner_frustum);
  UI_CHECK(applied.apply->renderer.icvfx.camera_defaults.margin_px == 32U);
  UI_CHECK(applied.apply->renderer.icvfx.latency_frames == 3U);
  UI_CHECK(applied.apply->renderer.icvfx.pose_prediction);
  UI_CHECK(applied.apply->renderer.icvfx.prediction_window == 9U);
  UI_CHECK(applied.apply->renderer.icvfx.jitter_filter_percent == 63U);
  // Cancel is a draft-only operation, including prediction settings.
  harness.panel.open(harness.state.settings, applied.apply->renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::tracking_tab()));
  static_cast<void>(harness.click(predict));
  const auto canceled = harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!canceled.apply);
}

struct Case final {
  const char *name;
  void (*function)();
};

// The environment is authored here because it belongs to no selectable
// element: there is nothing in the viewport to click on to reach a scene's
// lighting map. Before this category existed the renderer could consume one and
// an operator had no way to set it.
void the_environment_category_authors_the_lighting_map() {
  ProjectSettingsHarness harness;
  harness.state.stage_node = doc::NodeId{7U};
  // Opened with a map the project does not have yet, which is what an operator
  // typing one into the field produces. Apply must light up for that alone -
  // before this, the button only noticed changes to the renderer policy, so an
  // operator who edited nothing but the environment could not commit it.
  scene::Environment authored{};
  authored.map_source = "studio.hdr";
  authored.map_intensity = 2.5F;
  authored.map_rotation = 0.25F;
  authored.fog_density = 0.15F;
  authored.fog_background_distance = 5.0F;
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile, authored);

  static_cast<void>(harness.click(ProjectSettingsHarness::environment_tab()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  // Carried through untouched. The panel is an authoring surface for values it
  // does not interpret - the renderer decides what an empty map means.
  UI_CHECK(applied.apply->environment.map_source == "studio.hdr");
  UI_CHECK(applied.apply->environment.map_intensity == 2.5F);
  UI_CHECK(applied.apply->environment.map_rotation == 0.25F);
  UI_CHECK(applied.apply->environment.fog_background_distance == 5.0F);
}

// Cancelling really discards, including the map. A dialog that kept a typed
// path after Cancel would author a scene the operator had backed out of.
void physical_sky_controls_apply_and_author_a_time_key() {
  ProjectSettingsHarness harness;
  harness.state.stage_node = doc::NodeId{7};
  scene::Environment authored;
  authored.physical_sky = {true, 16.5F, 42.0F, 0.125F, 2.5F, 95000.0F, 0.0015F, 0.35F};
  harness.panel.open(harness.state.settings, harness.state.renderer, harness.state.lens_profile, authored);
  static_cast<void>(harness.click(ProjectSettingsHarness::environment_tab()));
  const gui::Rect key{ProjectSettingsHarness::card_x+14, ProjectSettingsHarness::content_y+38+22+30+26+44,180,26};
  const auto keyed = harness.click(key);
  UI_CHECK(keyed.key_sky_time && *keyed.key_sky_time == 16.5F);
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply && applied.apply->environment.physical_sky.enabled);
  UI_CHECK(applied.apply->environment.physical_sky.time_of_day == 16.5F);
  UI_CHECK(applied.apply->environment.physical_sky.latitude == 42.0F);
  UI_CHECK(applied.apply->environment.physical_sky.solar_illuminance == 95000.0F);
  UI_CHECK(std::abs(applied.apply->environment.physical_sky.angular_radius-0.0015F)<1e-8F);
}

void cancelling_the_environment_category_discards_the_map() {
  ProjectSettingsHarness harness;
  scene::Environment authored{};
  authored.map_source = "studio.hdr";
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile, authored);
  static_cast<void>(harness.click(ProjectSettingsHarness::environment_tab()));
  const auto cancelled =
      harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!cancelled.apply.has_value());
  UI_CHECK(!harness.panel.is_open());
}

// Every way the environment can fail to light the scene has its own words.
//
// They all look identical on screen - the hemisphere - so the dialog is the
// only place an operator can tell a bad path from a project still on Basic.
// Before this, all three were silent and the answer was "it does not work".
// Colour is authored from its own tab, and only what was touched changes.
//
// The transform buttons and the dither switch are the two controls a wall
// operator reaches for during a shoot; the look path is typed. Apply carries
// exactly the edited fields, so a project that never opened this tab keeps
// the defaults - and therefore the profile digest - it had.
void photometric_scale_accepts_precise_numeric_input() {
  ProjectSettingsHarness harness;
  harness.panel.open(harness.state.settings, harness.state.renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::color_tab()));
  const gui::Rect field{ProjectSettingsHarness::card_x + 164.0F,
                        ProjectSettingsHarness::content_y + 220.0F,
                        442.0F, 22.0F};
  static_cast<void>(harness.click(field));
  harness.input.new_frame();
  harness.input.add_text("0.0062831853");
  harness.ctx.new_frame();
  static_cast<void>(harness.panel.draw(harness.ctx, harness.bounds, harness.state));
  harness.input.new_frame();
  harness.input.add_key(gui::Key::Enter);
  harness.ctx.new_frame();
  static_cast<void>(harness.panel.draw(harness.ctx, harness.bounds, harness.state));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->renderer.photometric_scale == 0.0062831853F);
  UI_CHECK(applied.apply->renderer.color == harness.state.renderer.color);
}

void colour_is_authored_from_its_own_tab() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::color_tab()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::srgb_transform_button()));
  static_cast<void>(harness.click(ProjectSettingsHarness::dither_checkbox()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->renderer.color.display_transform ==
           scene::DisplayTransform::Srgb);
  UI_CHECK(!applied.apply->renderer.color.dither);
  UI_CHECK(applied.apply->renderer.color.show_lut_source.empty());
  // Nothing else moved.
  UI_CHECK(applied.apply->renderer.backend == scene::RendererBackend::Advanced);
  UI_CHECK(applied.apply->renderer.bloom_strength == 0.0F);

  harness.state.renderer = applied.apply->renderer;
  harness.panel.open(harness.state.settings, harness.state.renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::color_tab()));
  auto aces = ProjectSettingsHarness::srgb_transform_button();
  aces.x += 248.0F;
  static_cast<void>(harness.click(aces));
  const auto reference = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(reference.apply.has_value());
  UI_CHECK(reference.apply->renderer.color.display_transform ==
           scene::DisplayTransform::Aces13);
  UI_CHECK(!reference.apply->renderer.color.dither);

  // And the tab is reachable with nothing changed: opening it and applying
  // is inert, so a look at the card does not author an edit.
  ProjectSettingsHarness untouched;
  untouched.panel.open(untouched.state.settings, untouched.state.renderer);
  static_cast<void>(untouched.click(ProjectSettingsHarness::color_tab()));
  UI_CHECK(!untouched.click(ProjectSettingsHarness::apply_button())
                .apply.has_value());
}

// Every way the authored colour can fail to reach the wall has its own words,
// and the residency states map onto them without a gap.
void the_colour_status_names_each_reason() {
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Loading) !=
           editor::color_status_text(editor::ColorStatus::Ready));
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Failed) !=
           editor::color_status_text(editor::ColorStatus::Ready));
  UI_CHECK(editor::color_status_text(editor::ColorStatus::BackendUnsupported)
               .find("Basic") != std::string_view::npos);
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Failed)
               .find("cube") != std::string_view::npos);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Ready) ==
           editor::ColorStatus::Ready);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Failed) ==
           editor::ColorStatus::Failed);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Pending) ==
           editor::ColorStatus::Loading);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Unseen) ==
           editor::ColorStatus::Loading);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Busy) ==
           editor::ColorStatus::Loading);
}

void the_environment_status_names_each_reason() {
  UI_CHECK(editor::environment_status_text(
               editor::EnvironmentStatus::NotAuthored)
           != editor::environment_status_text(
               editor::EnvironmentStatus::Failed));
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Failed)
           != editor::environment_status_text(
               editor::EnvironmentStatus::BackendUnsupported));
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Loading)
           != editor::environment_status_text(
               editor::EnvironmentStatus::Ready));
  // The two that need an operator to do something say so; the two that are
  // working or merely waiting do not.
  UI_CHECK(editor::environment_status_text(
                   editor::EnvironmentStatus::BackendUnsupported)
                   .find("Advanced") != std::string_view::npos);
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Failed)
                   .find("EXR") != std::string_view::npos);
  // A project with nowhere to store the map is its own reason: the field
  // simply reverts, which looks exactly like never having typed one.
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::NoStage)
           != editor::environment_status_text(
               editor::EnvironmentStatus::NotAuthored));
  for (const auto status :
       {editor::EnvironmentStatus::NotAuthored,
        editor::EnvironmentStatus::Loading, editor::EnvironmentStatus::Failed,
        editor::EnvironmentStatus::BackendUnsupported,
        editor::EnvironmentStatus::NoStage,
        editor::EnvironmentStatus::Ready}) {
    UI_CHECK(!editor::environment_status_text(status).empty());
  }
}

void program_publication_status_reports_the_actual_wait() {
  editor::OutputMonitorState::Cluster cluster{};
  core::AssetSetReadiness local{19, false, 0};
  auto status = editor::program_asset_status(local, {}, 7, true, cluster, "Loading baked lighting");
  UI_CHECK(status.waiting && status.detail == "Loading baked lighting");
  local.ready = true;
  cluster.asset_cohort.observe(&local);
  cluster.asset_cohort.observe(nullptr);
  cluster.asset_waiting_node = 3;
  status = editor::program_asset_status(local, {}, 7, true, cluster, {});
  UI_CHECK(status.headline == "WAITING FOR OUTPUT NODE 3");
  UI_CHECK(status.detail.starts_with("1/2 renderers ready"));
  cluster.asset_waiting_node.reset();
  status = editor::program_asset_status(local, {19, 10, 42}, 7, true, cluster, {});
  UI_CHECK(status.detail.find("3 control frames") != std::string::npos);
  status = editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {});
  UI_CHECK(status.headline == "PROGRAM ACTIVATION MISSED");
  UI_CHECK(status.detail.find("retried") != std::string::npos);
  local.visible_commit = 10;
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {}).headline == "CONFIRMING OUTPUT UPDATE");
  cluster.asset_cohort.visible = true;
  UI_CHECK(!editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {}).waiting);
}

void shadow_stability_controls_apply_and_reopen() {
  ProjectSettingsHarness h;
  h.state.renderer.backend = scene::RendererBackend::Advanced;
  h.panel.open(h.state.settings, h.state.renderer);
  const auto tab = gui::Rect{ProjectSettingsHarness::card_x + 14.0F
    + 7.0F * (ProjectSettingsHarness::tab_width + ProjectSettingsHarness::tab_gap),
    ProjectSettingsHarness::tab_y, ProjectSettingsHarness::tab_width, ProjectSettingsHarness::tab_height};
  static_cast<void>(h.click(tab));
  for (int row = 0; row < 4; ++row) {
    static_cast<void>(h.click(gui::Rect{ProjectSettingsHarness::card_x + 164.0F,
      ProjectSettingsHarness::content_y + 22.0F + static_cast<float>(row) * 44.0F,
      368.0F, 22.0F}));
  }
  const auto applied = h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  const auto authored = applied.apply->renderer;
  UI_CHECK(authored.shadow_blend_band > 0.12F && authored.shadow_blend_band < 0.13F);
  UI_CHECK(authored.shadow_filter_radius > 1.9F && authored.shadow_filter_radius < 2.1F);
  UI_CHECK(authored.contact_shadow_length > 4.9F && authored.contact_shadow_length < 5.1F);
  UI_CHECK(authored.contact_shadow_thickness > 0.49F && authored.contact_shadow_thickness < 0.51F);
  h.panel.open(h.state.settings, authored);
  static_cast<void>(h.click(tab));
  const auto reopened = h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(reopened.apply.has_value() && reopened.apply->renderer == authored);
}

constexpr std::array<Case, 99U> cases{{
    {"object motion controls apply and cancel", &object_motion_controls_apply_and_cancel},
    {"plate depth of field controls apply and cancel", &plate_dof_controls_apply_and_cancel},
    {"shadow stability controls apply and reopen", &shadow_stability_controls_apply_and_reopen},
    {"photometric scale accepts precise numeric input", &photometric_scale_accepts_precise_numeric_input},
    {"program publication reports the actual wait", &program_publication_status_reports_the_actual_wait},
    {"baked lighting page actions and authored field", &baked_lighting_page_starts_cancels_and_preserves_the_authored_field},
    {"the environment status names each reason",
     &the_environment_status_names_each_reason},
    {"colour is authored from its own tab",
     &colour_is_authored_from_its_own_tab},
    {"the colour status names each reason",
     &the_colour_status_names_each_reason},
    {"the environment category authors the lighting map",
     &the_environment_category_authors_the_lighting_map},
    {"physical sky controls apply and author time keys", &physical_sky_controls_apply_and_author_a_time_key},
    {"cancelling the environment category discards the map",
     &cancelling_the_environment_category_discards_the_map},
    {"a scene row reports the clicked node",
     &a_scene_row_reports_the_clicked_node},
    {"a Ctrl scene click requests an exact selection toggle",
     &a_ctrl_scene_click_requests_an_exact_selection_toggle},
    {"a Shift scene click carries the visible element order",
     &a_shift_scene_click_carries_the_visible_element_order},
    {"bare Scene list space requests a clear",
     &bare_scene_list_space_requests_a_clear},
    {"a locked scene row only offers its unlock control",
     &a_locked_scene_row_only_offers_its_unlock_control},
    {"scene search filters the hierarchy", &scene_search_filters_the_hierarchy},
    {"a Scene group is visible selectable and uses its authored name",
     &a_scene_group_is_visible_selectable_and_uses_its_authored_name},
    {"Scene Group and Ungroup actions are truthfully enabled",
     &scene_group_and_ungroup_actions_are_truthfully_enabled},
    {"the library offers a camera", &the_library_offers_a_camera},
    {"a library item reports an add action",
     &a_library_item_reports_an_add_action},
    {"library filter finds and instantiates a stable asset",
     &library_filter_finds_and_instantiates_a_stable_asset},
    {"library imports are separate persistent asset intents",
     &library_imports_are_separate_persistent_asset_intents},
    {"an existing media row returns its asset id",
     &an_existing_media_row_returns_its_asset_id},
    {"an imported asset row has a separate remove action",
     &an_imported_asset_row_has_a_separate_remove_action},
    {"media proxy action cycles source prefer require without new identity",
     &media_proxy_action_cycles_source_prefer_require_without_new_identity},
    {"cpu-only media uses full-resolution proxy and draws warning",
     &cpu_only_media_uses_full_resolution_proxy_and_draws_warning},
    {"an asset row can be dragged out for stage placement",
     &an_asset_row_can_be_dragged_out_for_stage_placement},
    {"an effect drag reports its graph drop position without a selection",
     &an_effect_drag_reports_its_graph_drop_position_without_a_selection},
    {"clicking an effect does not apply it",
     &clicking_an_effect_does_not_apply_it},
    {"effect search filters before dragging",
     &effect_search_filters_before_dragging},
    {"effect drops apply only over Stage or Inspector",
     &effect_drops_apply_only_over_stage_or_inspector},
    {"an effect drop target strengthens under the pointer",
     &an_effect_drop_target_strengthens_under_the_pointer},
    {"an empty registry has no phantom asset rows",
     &an_empty_registry_has_no_phantom_asset_rows},
    {"long asset lists scroll and clip interaction to the list",
     &long_asset_lists_scroll_and_clip_interaction_to_the_list},
    {"thumbnail rows request visible then bounded prefetch assets",
     &thumbnail_rows_request_visible_then_bounded_prefetch_assets},
    {"thumbnail observations suppress duplicate requests",
     &thumbnail_observations_suppress_duplicate_requests},
    {"thumbnail observations must match the exact content revision",
     &thumbnail_observations_must_match_the_exact_content_revision},
    {"scrolling replaces the thumbnail retain set",
     &scrolling_replaces_the_thumbnail_retain_set},
    {"thumbnail identity is never invented for unfingerprinted assets",
     &thumbnail_identity_is_never_invented_for_unfingerprinted_assets},
    {"proxy runtime status labels explain resolution and failure",
     &proxy_runtime_status_labels_explain_resolution_and_failure},
    {"proxy runtime rows preserve thumbnail and asset identity",
     &proxy_runtime_rows_preserve_thumbnail_and_asset_identity},
    {"compositor toolbar reports effect add intents",
     &compositor_toolbar_reports_effect_add_intents},
    {"compositor output creation is visible and unique",
     &compositor_output_creation_is_visible_and_unique},
    {"selected compositor nodes and edges return delete intents",
     &selected_compositor_nodes_and_edges_return_delete_intents},
    {"LED Car Process returns an ordinary node delete intent",
     &led_car_process_returns_an_ordinary_node_delete_intent},
    {"the required Output node cannot be deleted",
     &the_required_output_node_cannot_be_deleted},
    {"project replacement clears compositor document state",
     &project_replacement_clears_compositor_document_state},
    {"Compositor navigation zooms under the pointer and pans the graph",
     &compositor_navigation_zooms_under_the_pointer_and_pans_the_graph},
    {"zoomed Compositor node dragging still authors graph coordinates",
     &zoomed_compositor_node_dragging_still_authors_graph_coordinates},
    {"dragging a compositor node reports its authored position on release",
     &dragging_a_compositor_node_reports_its_authored_position_on_release},
    {"compatible typed ports report a connection intent",
     &compatible_typed_ports_report_a_connection_intent},
    {"dropping on a single input reports an atomic rewire",
     &dropping_on_a_single_input_reports_an_atomic_rewire},
    {"incompatible ports are rejected and persisted edges draw wires",
     &incompatible_ports_are_rejected_and_persisted_edges_draw_wires},
    {"compositor diagnostics are explained in plain language",
     &compositor_diagnostics_are_explained_in_plain_language},
    {"brightness parameter typing emits one typed commit",
     &brightness_parameter_typing_emits_one_typed_commit},
    {"compositor parameter nudges report full semantic identity",
     &compositor_parameter_nudges_report_full_semantic_identity},
    {"Saturation parameter commits are clamped to four",
     &saturation_parameter_commits_are_clamped_to_four},
    {"Levels parameter typing emits the exact channel property",
     &levels_parameter_typing_emits_the_exact_channel_property},
    {"Lift Gamma Gain fields emit typed clamped properties",
     &lift_gamma_gain_fields_emit_typed_clamped_properties},
    {"Contrast Pivot fields emit typed clamped properties",
     &contrast_pivot_fields_emit_typed_clamped_properties},
    {"Color Matrix fields emit exact typed properties",
     &color_matrix_fields_emit_exact_typed_properties},
    {"White Balance fields emit typed clamped properties",
     &white_balance_fields_emit_typed_clamped_properties},
    {"Curves add and last point edit emit one typed program",
     &curves_add_and_last_point_edit_emit_one_typed_program},
    {"Film Grain fields preserve exact integer and mode types",
     &film_grain_fields_preserve_exact_integer_and_mode_types},
    {"blur parameter commits are clamped to the model contract",
     &blur_parameter_commits_are_clamped_to_the_model_contract},
    {"radial array parameters emit count radius angle and center types",
     &radial_array_parameters_emit_count_radius_angle_and_center_types},
    {"workspace panels draw scene relevant content",
     &workspace_panels_draw_scene_relevant_content},
    {"conformance control", &output_monitor_requests_conformance_without_a_lifecycle_action},
        {"output monitor emits fixed lifecycle intent",
     &output_monitor_emits_one_fixed_lifecycle_intent},
    {"duplicate node indices disable lifecycle controls",
     &duplicate_node_indices_have_no_ambiguous_controls},
    {"missing cluster assets block start but keep status visible",
     &missing_cluster_assets_block_start_but_keep_status_visible},
    {"content labels distinguish missing stale and verification failures",
     &content_labels_distinguish_missing_stale_and_verification_failures},
    {"output monitor scroll reaches later cluster nodes",
     &output_monitor_scroll_reaches_later_cluster_nodes},
    {"output monitor emits explicit cluster relaunch",
     &output_monitor_emits_explicit_cluster_relaunch},
    {"output monitor cluster health keeps node controls reachable",
     &output_monitor_cluster_health_keeps_node_controls_reachable},
    {"project settings apply reports the edited values",
     &project_settings_apply_reports_the_edited_values},
    {"project settings cancel discards the draft",
     &project_settings_cancel_discards_the_draft},
    {"project settings defaults restores the built-in values",
     &project_settings_defaults_restores_the_built_in_values},
    {"advanced renderer required is an explicit project edit",
     &advanced_renderer_required_is_an_explicit_project_edit},
    {"raster only clears only unavailable future pass requests",
     &raster_only_clears_only_unavailable_future_pass_requests},
    {"texture sampling controls apply and cancel", &texture_sampling_controls_apply_and_cancel},
    {"texture streaming controls emit authored policy", &texture_streaming_controls_emit_authored_policy},
    {"geometry controls emit replicated settings", &geometry_controls_emit_replicated_settings},
    {"shadow cascades and bounce are authored beside the backend",
     &shadow_cascades_and_bounce_are_authored_beside_the_backend},
    {"antialiasing is one authored choice",
     &antialiasing_is_one_authored_choice},
    {"occlusion is authored as a radius",
     &occlusion_is_authored_as_a_radius},
    {"calibrated lens controls author their profile",
     &calibrated_lens_controls_author_their_profile},
    {"the tracking category authors the inner frustum policy",
     &the_tracking_category_authors_the_inner_frustum_policy},
    {"cancel closes the preferences dialog",
     &cancel_closes_the_preferences_dialog},
    {"cancel discards edits so reopening shows the stored values",
     &cancel_discards_edits_so_reopening_shows_the_stored_values},
    {"save reports the edited values and closes",
     &save_reports_the_edited_values_and_closes},
    {"save stays disabled until something actually changes",
     &save_stays_disabled_until_something_actually_changes},
    {"proxy storage mode is part of the saved draft",
     &proxy_storage_mode_is_part_of_the_saved_draft},
    {"clear proxy cache reports without saving or closing",
     &clear_proxy_cache_reports_without_saving_or_closing},
    {"playback preset is part of the saved machine draft",
     &playback_preset_is_part_of_the_saved_machine_draft},
    {"transcode priority is part of the saved machine draft",
     &transcode_priority_is_part_of_the_saved_machine_draft},
    {"only a click outside the card dismisses it",
     &only_a_click_outside_the_card_dismisses_it},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
