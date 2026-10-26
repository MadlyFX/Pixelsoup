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

// The outliner's geometry: a 32-unit search bar, then 24-unit rows (the first
// row's centre is y=44), the eye at x=2..26 and the padlock at the right edge,
// a 30-unit footer holding Group and Ungroup.
constexpr float scene_row(int index) {
  return 44.0F + 24.0F * static_cast<float>(index);
}

struct SceneHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ScenePanel panel{gui::ui_id("scene.test")};
  std::vector<editor::ElementInfo> items{elements()};
  std::vector<doc::NodeId> selection;
  std::vector<mediaserver::output::ProjectionSurface> surfaces;
  doc::NodeId selected_surface{};
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
                      selection.empty() ? doc::NodeId{} : selection.back(),
                      false, editor::SceneSettingsTarget::None, surfaces,
                      selected_surface);
  }

  std::optional<editor::SceneSelectionRequest> click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

// The stage's LED surfaces follow the scene's elements. A click selects one
// on Stage; its padlock - at the right edge, like every row's - sets its lock;
// a locked surface's row offers only the padlock. No eye: x=2..26 is inert.
void led_surfaces_list_after_the_scene_with_a_lock() {
  SceneHarness harness;
  mediaserver::output::ProjectionSurface surface{};
  surface.node = doc::NodeId{40U};
  surface.name = "Main LED";
  surface.locked = false;
  harness.surfaces.push_back(surface);
  static_cast<void>(harness.frame(0.0F, 0.0F, false));

  const auto selected = harness.click(80.0F, scene_row(3));
  UI_CHECK(selected.has_value());
  UI_CHECK(selected->surface);
  UI_CHECK(selected->node == doc::NodeId{40U});
  UI_CHECK(!selected->lock_changed);

  const auto locked = harness.click(226.0F, scene_row(3));
  UI_CHECK(locked.has_value());
  UI_CHECK(locked->surface);
  UI_CHECK(locked->node == doc::NodeId{40U});
  UI_CHECK(locked->lock_changed);
  UI_CHECK(locked->locked);

  const auto eye = harness.click(14.0F, scene_row(3));
  UI_CHECK(!eye.has_value() || !eye->visibility_changed);

  harness.surfaces.back().locked = true;
  const auto inert = harness.click(80.0F, scene_row(3));
  UI_CHECK(!inert.has_value() || !inert->surface);
  const auto unlocked = harness.click(226.0F, scene_row(3));
  UI_CHECK(unlocked.has_value());
  UI_CHECK(unlocked->lock_changed);
  UI_CHECK(!unlocked->locked);
}

void a_scene_row_reports_the_clicked_node() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  const auto request = harness.frame(80.0F, scene_row(0), false);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

void a_ctrl_scene_click_requests_an_exact_selection_toggle() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(
      harness.frame(80.0F, scene_row(0), true, gui::KeyMods::Ctrl));
  const auto request =
      harness.frame(80.0F, scene_row(0), false, gui::KeyMods::Ctrl);

  UI_CHECK(request.has_value());
  UI_CHECK(request->action == editor::SelectionAction::Toggle);
  UI_CHECK(request->visible_order.empty());
}

void a_shift_scene_click_carries_the_visible_element_order() {
  SceneHarness harness;
  // The outliner is the scene's hierarchy, not a sort by kind, so the light
  // is simply the third top-level row.
  static_cast<void>(harness.frame(80.0F, scene_row(2), false));
  static_cast<void>(
      harness.frame(80.0F, scene_row(2), true, gui::KeyMods::Shift));
  const auto request =
      harness.frame(80.0F, scene_row(2), false, gui::KeyMods::Shift);

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

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  UI_CHECK(!harness.frame(80.0F, scene_row(0), false).has_value());

  // The padlock sits at the row's right edge, x=214..238.
  constexpr float lock_x = 226.0F;
  static_cast<void>(harness.frame(lock_x, scene_row(0), false));
  static_cast<void>(harness.frame(lock_x, scene_row(0), true));
  const auto request = harness.frame(lock_x, scene_row(0), false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->lock_changed);
  UI_CHECK(!request->locked);
}

void scene_search_filters_the_hierarchy() {
  SceneHarness harness;
  // Focus the search well, then type. The filtered tree holds only the
  // matching point light, in the first row.
  static_cast<void>(harness.frame(70.0F, 16.0F, false));
  static_cast<void>(harness.frame(70.0F, 16.0F, true));
  static_cast<void>(harness.frame(70.0F, 16.0F, false));
  static_cast<void>(
      harness.frame(70.0F, 16.0F, false, gui::KeyMods::None, "point"));
  UI_CHECK(harness.panel.search() == "point");

  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  const auto request = harness.frame(80.0F, scene_row(0), false);
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

  // An empty group is a top-level row of its own, after its siblings.
  static_cast<void>(harness.frame(80.0F, scene_row(3), false));
  static_cast<void>(harness.frame(80.0F, scene_row(3), true));
  const auto request = harness.frame(80.0F, scene_row(3), false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{13U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

void scene_group_and_ungroup_actions_are_truthfully_enabled() {
  // The footer's Group button is x=91..161 and Ungroup x=165..235.
  SceneHarness group_harness;
  group_harness.selection = {doc::NodeId{7U}, doc::NodeId{9U}};
  static_cast<void>(group_harness.frame(126.0F, 285.0F, false));
  static_cast<void>(group_harness.frame(126.0F, 285.0F, true));
  const auto grouped = group_harness.frame(126.0F, 285.0F, false);
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
  static_cast<void>(ungroup_harness.frame(200.0F, 285.0F, false));
  static_cast<void>(ungroup_harness.frame(200.0F, 285.0F, true));
  const auto ungrouped = ungroup_harness.frame(200.0F, 285.0F, false);
  UI_CHECK(ungrouped.has_value());
  UI_CHECK(ungrouped->structure ==
           editor::SceneSelectionRequest::StructureAction::Ungroup);

  // A light is not accepted by the core grouping planner, and the disabled
  // button must not fabricate an action.
  SceneHarness light_harness;
  light_harness.selection = {doc::NodeId{11U}};
  static_cast<void>(light_harness.frame(126.0F, 285.0F, false));
  static_cast<void>(light_harness.frame(126.0F, 285.0F, true));
  UI_CHECK(!light_harness.frame(126.0F, 285.0F, false).has_value());
}

// A double-click on a row's name opens it for renaming with the old name
// selected; typing replaces it and Enter reports the new name once.
void a_scene_row_renames_in_place() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  UI_CHECK(harness.panel.renaming());
  static_cast<void>(harness.frame(300.0F, 400.0F, false, gui::KeyMods::None,
                                  "Floor Riser"));
  harness.input.new_frame();
  harness.input.add_key(gui::Key::Enter);
  harness.ctx.new_frame();
  const auto renamed = harness.panel.draw(harness.ctx, harness.bounds,
                                          harness.items, harness.selection);
  UI_CHECK(renamed.has_value());
  UI_CHECK(renamed->node == doc::NodeId{7U});
  UI_CHECK(renamed->rename == std::optional<std::string>{"Floor Riser"});
  UI_CHECK(!harness.panel.renaming());
}

// The eye at a row's left edge is the element's own visibility, the same
// switch the Timeline's eye throws.
void a_scene_eye_hides_and_shows() {
  SceneHarness harness;
  static_cast<void>(harness.frame(14.0F, scene_row(1), false));
  static_cast<void>(harness.frame(14.0F, scene_row(1), true));
  const auto hidden = harness.frame(14.0F, scene_row(1), false);
  UI_CHECK(hidden.has_value());
  UI_CHECK(hidden->node == doc::NodeId{9U});
  UI_CHECK(hidden->visibility_changed);
  UI_CHECK(!hidden->visible);
}

// Dragging a row onto the middle of a group files it under the group; onto
// the edge of a sibling, it reorders beside it.
void scene_rows_drag_into_groups_and_between_siblings() {
  SceneHarness harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Rig";
  harness.items.push_back(group);

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(2), true));
  static_cast<void>(harness.frame(80.0F, scene_row(3), true));
  const auto filed = harness.frame(80.0F, scene_row(3), false);
  UI_CHECK(filed.has_value());
  UI_CHECK(filed->structure ==
           editor::SceneSelectionRequest::StructureAction::MoveInto);
  UI_CHECK(filed->destination == group.node);
  UI_CHECK(filed->moved == std::vector<doc::NodeId>{doc::NodeId{7U}});

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(1), true));
  const auto reordered = harness.frame(80.0F, scene_row(1) + 9.0F, false);
  UI_CHECK(reordered.has_value());
  UI_CHECK(!reordered->structure.has_value());
  UI_CHECK(reordered->node == doc::NodeId{7U});
  UI_CHECK(reordered->reorder_destination == doc::NodeId{9U});
  UI_CHECK(!reordered->reorder_before);
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

void compositor_scene_layers_show_the_referenced_element_name() {
  editor::ElementInfo model{};
  model.node = doc::NodeId{1000U};
  model.kind = editor::ElementKind::Model;
  model.display_name = "Hero Robot";
  std::array<editor::ElementInfo, 1U> items{model};
  UI_CHECK(editor::compositor_scene_layer_detail(model.node, items) ==
           "Hero Robot");
  items.front().display_name = "Robot Closeup";
  UI_CHECK(editor::compositor_scene_layer_detail(model.node, items) ==
           "Robot Closeup");
  UI_CHECK(editor::compositor_scene_layer_detail(doc::NodeId{2000U}, items) ==
           "OBJECT #2000");
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
  constexpr float wire_x = 268.0F;
  constexpr float wire_y = 132.0F;
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

  // Replacement picking follows the navigated, visible node geometry.
  const float node_x = panned.x + 60.0F * zoom;
  const float node_y = canvas_y + panned.y + 40.0F * zoom;
  const auto hit = harness.panel.node_at(graph, node_x + 30.0F, node_y + 20.0F);
  UI_CHECK(hit.has_value() && hit->first == doc::NodeId{1U});
  UI_CHECK(std::abs(hit->second.x - node_x) < 0.001F);
  UI_CHECK(std::abs(hit->second.y - node_y) < 0.001F);
  UI_CHECK(!harness.panel.node_at(graph, 10.0F, 13.0F).has_value());

  harness.panel.reset_project_state();
  UI_CHECK(harness.panel.graph_zoom() == 1.0F);
  UI_CHECK(harness.panel.graph_pan().x == 0.0F);
  UI_CHECK(harness.panel.graph_pan().y == 0.0F);
}

void compositor_replacement_picks_topmost_and_clips_feedback() {
  CompositorHarness harness;
  compositor::CompositorGraph graph;
  graph.nodes.push_back(graph_node(1U, compositor::NodeKind::SceneLayer, -30.0F, -20.0F));
  graph.nodes.push_back(graph_node(2U, compositor::NodeKind::Blur, -10.0F, -10.0F));
  static_cast<void>(harness.frame(graph, 50.0F, 40.0F, false));
  const auto hit = harness.panel.node_at(graph, 50.0F, 40.0F);
  UI_CHECK(hit.has_value() && hit->first == doc::NodeId{2U});
  UI_CHECK(hit->second.x == 0.0F && hit->second.y == 26.0F);
  UI_CHECK(hit->second.width == 166.0F && hit->second.height == 54.0F);
  UI_CHECK(!harness.panel.node_at(graph, -1.0F, 40.0F).has_value());
  UI_CHECK(!harness.panel.node_at(graph, 20.0F, 20.0F).has_value());
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

  // Both are EFFECT ports. With the headless font a node is its minimum 176
  // wide, its header 35 tall and its port rows 22, so the first port centre
  // is 46 below the node top; the canvas begins at y=26.
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 132.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 132.0F, false);
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
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 132.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 132.0F, false);

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
      incompatible_harness.frame(incompatible, 216.0F, 132.0F, false));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 216.0F, 132.0F, true));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 320.0F, 132.0F, true));
  const editor::CompositorFrame rejected =
      incompatible_harness.frame(incompatible, 320.0F, 132.0F, false);
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
      commit_compositor_field(harness, graph, 820.0F, 99.0F, "0.25");
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

  result = commit_compositor_field(harness, graph, 740.0F, 147.0F, "-2.25");
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

// The authored move reaches the evaluated graph some frames after release.
// Until it does the node stays where it was let go, rather than snapping back
// to its old position for those frames and then jumping forward.
void a_released_node_holds_its_position_until_the_graph_reports_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  // Old screen bounds (60, 66)-(236, 130); after the drag, (120, 126)-(296,
  // 190). Each point below is inside one and not the other, and in a header
  // rather than on a port.
  constexpr float old_only_x = 70.0F;
  constexpr float old_only_y = 80.0F;
  constexpr float new_only_x = 280.0F;
  constexpr float new_only_y = 140.0F;

  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 160.0F, 135.0F, true));
  const editor::CompositorFrame released =
      harness.frame(graph, 160.0F, 135.0F, false);
  UI_CHECK(released.move_node ==
           (editor::CompositorMoveIntent{doc::NodeId{1U}, 120.0F, 100.0F}));

  // The graph has not caught up: the node is drawn and picked where it was
  // dropped, and pressing there selects it.
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  UI_CHECK(harness.panel.node_at(graph, new_only_x, new_only_y).has_value());
  UI_CHECK(!harness.panel.node_at(graph, old_only_x, old_only_y).has_value());
  const editor::CompositorFrame pressed =
      harness.frame(graph, new_only_x, new_only_y, true);
  UI_CHECK(pressed.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{1U}}));
  static_cast<void>(harness.frame(graph, new_only_x, new_only_y, false));

  // The edit arrives, normalized to a slightly different value: the graph's
  // own position takes over from then on.
  graph.nodes.front().editor_x = 121.0F;
  graph.nodes.front().editor_y = 100.0F;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  UI_CHECK(harness.panel.node_at(graph, 296.5F, new_only_y).has_value());

  // An edit that never arrives - refused - lets go after a bounded wait.
  compositor::CompositorGraph refused;
  refused.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness refused_harness;
  static_cast<void>(refused_harness.frame(refused, 100.0F, 75.0F, false));
  static_cast<void>(refused_harness.frame(refused, 100.0F, 75.0F, true));
  static_cast<void>(refused_harness.frame(refused, 160.0F, 135.0F, true));
  static_cast<void>(refused_harness.frame(refused, 160.0F, 135.0F, false));
  for (int frame = 0; frame < 60; ++frame) {
    static_cast<void>(refused_harness.frame(refused, 0.0F, 0.0F, false));
  }
  UI_CHECK(refused_harness.panel.node_at(refused, new_only_x, new_only_y)
               .has_value());
  for (int frame = 0; frame < 90; ++frame) {
    static_cast<void>(refused_harness.frame(refused, 0.0F, 0.0F, false));
  }
  UI_CHECK(refused_harness.panel.node_at(refused, old_only_x, old_only_y)
               .has_value());
  UI_CHECK(!refused_harness.panel.node_at(refused, new_only_x, new_only_y)
                .has_value());
}

// A wire is a curve now, so it is picked where it is drawn: through the
// middle of its two ports, and not at the elbow a square wire used to turn.
void wires_are_picked_along_their_curve() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 200.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  // Layer IMAGE OUT at (216, 132); Output's IMAGE input at (320, 272). A
  // curve leaving and entering horizontally is symmetric about the midpoint.
  constexpr float middle_x = (216.0F + 320.0F) * 0.5F;
  constexpr float middle_y = (132.0F + 272.0F) * 0.5F;

  CompositorHarness elbow;
  static_cast<void>(elbow.frame(graph, middle_x, 132.0F, false));
  const editor::CompositorFrame missed = elbow.frame(graph, middle_x, 132.0F, true);
  UI_CHECK(missed.selection == editor::CompositorSelection{});

  CompositorHarness middle;
  static_cast<void>(middle.frame(graph, middle_x, middle_y, false));
  const editor::CompositorFrame picked =
      middle.frame(graph, middle_x, middle_y + 3.0F, true);
  UI_CHECK(picked.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                        doc::NodeId{50U}}));
  // The pressed wire keeps the pointer until release, wherever it wanders.
  static_cast<void>(middle.frame(graph, 600.0F, 500.0F, true));
  static_cast<void>(middle.frame(graph, 600.0F, 500.0F, false));
  const editor::CompositorFrame next = middle.frame(graph, 100.0F, 100.0F, true);
  UI_CHECK(next.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{10U}}));
}

// Letting a wire go near a compatible port connects it: the operator does
// not have to land on the dot.
void a_wire_snaps_to_a_compatible_port_near_the_pointer() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  // Blur EFFECT OUT at (216, 132); the Layer's EFFECTS input dot at (320,
  // 132), its row target starting 9 to the left of the dot.
  const auto release_at = [&](float x, float y) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
    static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
    static_cast<void>(harness.frame(graph, x, y, true));
    return harness.frame(graph, x, y, false);
  };
  // 20 short of the dot and outside its row: within snapping reach.
  const editor::CompositorFrame near = release_at(300.0F, 132.0F);
  UI_CHECK(near.connect_ports ==
           (editor::CompositorConnectIntent{
               compositor::EdgeEndpoint{doc::NodeId{1U}, 0U},
               compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}, std::nullopt}));
  // Well inside the row, far from the dot: the row is the target.
  const editor::CompositorFrame over_row = release_at(390.0F, 140.0F);
  UI_CHECK(over_row.connect_ports.has_value());
  UI_CHECK(over_row.connect_ports->to ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  // 30 short: out of reach, so nothing connects.
  UI_CHECK(!release_at(290.0F, 132.0F).connect_ports.has_value());
  // The INSTANCES row below is a TransformSet input; an effect never snaps
  // to it, even with the pointer on it.
  UI_CHECK(!release_at(330.0F, 158.0F).connect_ports.has_value());
}

// A drop from another panel can name the input it lands on, from the whole
// row rather than the dot alone.
void an_input_ports_target_covers_its_whole_row() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  // Node (320, 86)-(496, 152): header to 121, EFFECTS row 121-143, INSTANCES
  // row 143-165.
  UI_CHECK(harness.panel.input_port_at(graph, 314.0F, 132.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(harness.panel.input_port_at(graph, 400.0F, 125.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(harness.panel.input_port_at(graph, 330.0F, 150.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 1U}));
  UI_CHECK(!harness.panel.input_port_at(graph, 330.0F, 100.0F).has_value());
  UI_CHECK(!harness.panel.input_port_at(graph, 470.0F, 132.0F).has_value());
  UI_CHECK(!harness.panel.input_port_at(graph, 200.0F, 132.0F).has_value());
}

void the_parameters_panel_resizes_from_its_edge() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  UI_CHECK(harness.panel.parameter_panel_width() == 0.0F);

  // The panel's left edge is at 900 - 280. Press just on the graph side of
  // it - the seam reaches 3 either way - and drag it 100 to the left.
  static_cast<void>(harness.frame(graph, 618.0F, 400.0F, false));
  const editor::CompositorFrame pressed =
      harness.frame(graph, 618.0F, 400.0F, true);
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, true));
  const editor::CompositorFrame released =
      harness.frame(graph, 518.0F, 400.0F, false);
  UI_CHECK(harness.panel.parameter_panel_width() == 382.0F);
  UI_CHECK(!released.move_node.has_value());
  // Resizing is not a click on empty graph: the node stays selected.
  UI_CHECK(!pressed.selection.has_value());
  UI_CHECK(harness.panel.selection() ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{70U}}));

  // The field moved with the edge: x=600 was graph canvas and is now the
  // AMOUNT field.
  const editor::CompositorFrame typed =
      commit_compositor_field(harness, graph, 600.0F, 73.0F, "2.5");
  UI_CHECK(typed.property_edit.has_value());
  UI_CHECK(typed.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);

  // Dragged far left, it stops at its widest, still leaving the graph a
  // usable 260.
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, false));
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, true));
  static_cast<void>(harness.frame(graph, 10.0F, 400.0F, true));
  static_cast<void>(harness.frame(graph, 10.0F, 400.0F, false));
  UI_CHECK(harness.panel.parameter_panel_width() == 640.0F);
}

// Levels has four channels of five fields - taller than the panel. The list
// scrolls, so the last field is still reachable.
void long_parameter_lists_scroll_to_their_last_field() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  // Rows start at y=62, 26 apart: BLUE OUT WHITE is row 23, at 660, below
  // the 640-high panel.
  static_cast<void>(harness.navigation_frame(graph, 820.0F, 300.0F, false, -5.0F));
  static_cast<void>(harness.frame(graph, 820.0F, 300.0F, false));
  // Scrolled to the end: the list is 632 tall in a 582 body, so 50 up.
  const editor::CompositorFrame last =
      commit_compositor_field(harness, graph, 820.0F, 621.0F, "0.75");
  UI_CHECK(last.property_edit.has_value());
  UI_CHECK(last.property_edit->property ==
           editor::CompositorProperty::LevelsBlueOutputWhite);
  UI_CHECK(std::get<float>(last.property_edit->value) == 0.75F);
}

void parameter_sections_collapse() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  // Collapse MASTER: RED's heading moves up into the next row, and its
  // first field below that.
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, false));
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, true));
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, false));
  const editor::CompositorFrame red =
      commit_compositor_field(harness, graph, 820.0F, 125.0F, "0.125");
  UI_CHECK(red.property_edit.has_value());
  UI_CHECK(red.property_edit->property ==
           editor::CompositorProperty::LevelsRedInputBlack);
}

// The fault this layout replaces: titles and port names drawn in boxes
// shorter than the font, and nodes narrower than their names. With a real
// font, every glyph a node draws lies inside the clip it is drawn under, and
// every character of its title, source and port names is drawn - none was
// shortened to an ellipsis.
void node_text_fits_unclipped_in_a_real_font() {
  constexpr std::array<std::string_view, 2U> candidates{
      "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
  constexpr std::array<float, 4U> sizes{0.5F, 0.75F, 0.875F, 1.25F};
  std::optional<gui::FontAtlas> font;
  for (const std::string_view path : candidates) {
    font = gui::FontAtlas::create(path, 14U, 1.0F, sizes);
    if (font.has_value()) {
      break;
    }
  }
  if (!font.has_value()) {
    std::cout << "[skip] no system font; node text fit needs real glyphs\n";
    return;
  }
  gui::InputState input;
  gui::UiDrawList list;
  list.set_white_texel(font->white_u(), font->white_v());
  gui::UiContext ctx{input, list, *font};
  editor::CompositorPanel panel{gui::ui_id("compositor.font.test")};

  editor::ElementInfo plate{};
  plate.node = doc::NodeId{1000U};
  plate.kind = editor::ElementKind::Media;
  plate.display_name = "Plate Front Driver Side 3/4 Long Lens";
  const std::array<editor::ElementInfo, 1U> elements{plate};
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 20.0F, 20.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::LedCarProcess, 420.0F, 20.0F));

  input.new_frame();
  input.set_mouse_position(890.0F, 630.0F);
  ctx.new_frame();
  static_cast<void>(panel.draw(ctx, CompositorHarness::bounds, graph, elements));

  const auto non_space = [](std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(),
                      [](char character) { return character != ' '; }));
  };
  std::size_t expected{};
  for (const compositor::GraphNode &node : graph.nodes) {
    expected += non_space(compositor::node_kind_name(node.kind));
    for (const auto &port : compositor::input_ports(node.kind)) {
      expected += non_space(port.name);
    }
    for (const auto &port : compositor::output_ports(node.kind)) {
      expected += non_space(port.name);
    }
  }
  expected += non_space(plate.display_name);
  expected += non_space("8 WALL + CEILING  ARRAY 4.00");

  // Glyphs sample the atlas away from its white texel; everything below the
  // toolbar belongs to the two nodes.
  std::size_t glyph_vertices{};
  for (const gui::UiBatch &batch : list.batches()) {
    for (core::u32 index = batch.first_vertex;
         index < batch.first_vertex + batch.vertex_count; ++index) {
      const gui::UiVertex &vertex = list.vertices()[index];
      if ((vertex.u == font->white_u() && vertex.v == font->white_v()) ||
          vertex.y < 26.0F) {
        continue;
      }
      ++glyph_vertices;
      UI_CHECK(vertex.x >= batch.clip_min_x - 0.01F &&
               vertex.x <= batch.clip_max_x + 0.01F);
      UI_CHECK(vertex.y >= batch.clip_min_y - 0.01F &&
               vertex.y <= batch.clip_max_y + 0.01F);
    }
  }
  UI_CHECK(glyph_vertices == expected * 6U);

  // Both nodes grew past the minimum to fit their names.
  UI_CHECK(panel.new_node_size(compositor::NodeKind::LedCarProcess).x > 176.0F);
  const auto layer = panel.node_at(graph, 30.0F, 50.0F);
  UI_CHECK(layer.has_value() && layer->second.width > 176.0F);
}

// Headless geometry, as in the tests above: nodes are 176 wide with a 35
// header and 22 rows, so input i's dot is at the node's left edge and
// 46 + 22i below its top; the canvas starts at y=26.

[[nodiscard]] compositor::GraphEdge graph_edge(core::u64 id, core::u64 from,
                                               core::u8 from_port, core::u64 to,
                                               core::u8 to_port) {
  return compositor::GraphEdge{
      doc::NodeId{id}, compositor::EdgeEndpoint{doc::NodeId{from}, from_port},
      compositor::EdgeEndpoint{doc::NodeId{to}, to_port}, 0};
}

// Press at one point, drag to another, let go; the result of the release.
[[nodiscard]] editor::CompositorFrame
drag_in_graph(CompositorHarness &harness,
              const compositor::CompositorGraph &graph, float from_x,
              float from_y, float to_x, float to_y,
              gui::KeyMods mods = gui::KeyMods::None) {
  static_cast<void>(
      harness.frame(graph, from_x, from_y, false, {}, std::nullopt, mods));
  static_cast<void>(
      harness.frame(graph, from_x, from_y, true, {}, std::nullopt, mods));
  static_cast<void>(harness.frame(graph, to_x, to_y, true, {}, std::nullopt, mods));
  return harness.frame(graph, to_x, to_y, false, {}, std::nullopt, mods);
}

// Letting a wire go on empty canvas opens the node search there, offering
// only nodes that can take the wire. The one chosen arrives connected, its
// input where the wire was let go.
void a_wire_let_go_on_empty_canvas_offers_nodes_that_take_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  CompositorHarness harness;
  // Blur's EFFECT output is at (216, 132).
  const editor::CompositorFrame released =
      drag_in_graph(harness, graph, 216.0F, 132.0F, 500.0F, 300.0F);
  UI_CHECK(!released.connect_ports.has_value());
  UI_CHECK(!released.add_node.has_value());
  static_cast<void>(harness.frame(graph, 500.0F, 300.0F, false));
  // "layer" matches Scene Layer and Composite; only Scene Layer has an input
  // an effect can feed.
  static_cast<void>(harness.frame(graph, 500.0F, 300.0F, false, "layer"));
  const editor::CompositorFrame chosen = harness.frame(
      graph, 500.0F, 300.0F, false, {}, gui::Key::Enter);
  UI_CHECK(chosen.add_node.has_value());
  UI_CHECK(chosen.add_node->kind == compositor::NodeKind::SceneLayer);
  UI_CHECK(chosen.add_node->editor_x == 500.0F);
  UI_CHECK(chosen.add_node->editor_y == 300.0F - 26.0F - 46.0F);
  UI_CHECK(chosen.add_node->links ==
           (std::vector<editor::CompositorAddLink>{
               {false, 0U, compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}}}));
  // Taken, the search closes.
  UI_CHECK(!harness.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Enter)
                .add_node.has_value());

  // Escape closes it without adding anything.
  CompositorHarness escaped;
  static_cast<void>(
      drag_in_graph(escaped, graph, 216.0F, 132.0F, 500.0F, 300.0F));
  static_cast<void>(escaped.frame(graph, 500.0F, 300.0F, false));
  static_cast<void>(
      escaped.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Escape));
  UI_CHECK(!escaped.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Enter)
                .add_node.has_value());
}

// Tab over the canvas opens the same search with nothing attached; the node
// chosen is placed at the pointer.
void tab_opens_the_node_search_at_the_pointer() {
  const compositor::CompositorGraph graph;
  CompositorHarness harness;
  // A click on the graph gives it the keyboard.
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, true));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false));
  static_cast<void>(
      harness.frame(graph, 400.0F, 300.0F, false, {}, gui::Key::Tab));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false, "gaussian"));
  const editor::CompositorFrame chosen = harness.frame(
      graph, 400.0F, 300.0F, false, {}, gui::Key::Enter);
  UI_CHECK(chosen.add_node.has_value());
  UI_CHECK(chosen.add_node->kind == compositor::NodeKind::Blur);
  UI_CHECK(chosen.add_node->editor_x == 400.0F);
  UI_CHECK(chosen.add_node->editor_y == 274.0F);
  UI_CHECK(chosen.add_node->links.empty());
}

// Alt-click breaks wires: on an output, every wire leaving it; on an input,
// the wire arriving; on a wire, that wire.
void alt_click_breaks_wires() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  const editor::CompositorRewireIntent broken{{doc::NodeId{50U}}, {}};
  const auto alt_click = [&](float x, float y) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, x, y, false, {}, std::nullopt,
                                    gui::KeyMods::Alt));
    return harness.frame(graph, x, y, true, {}, std::nullopt,
                         gui::KeyMods::Alt);
  };
  UI_CHECK(alt_click(216.0F, 132.0F).rewire == broken);
  const editor::CompositorFrame input = alt_click(330.0F, 132.0F);
  UI_CHECK(input.rewire == broken);
  UI_CHECK(!input.move_node.has_value());
  UI_CHECK(alt_click(268.0F, 132.0F).rewire == broken);
  // Without Alt the same presses do what they always did.
  CompositorHarness plain;
  static_cast<void>(plain.frame(graph, 268.0F, 132.0F, false));
  UI_CHECK(!plain.frame(graph, 268.0F, 132.0F, true).rewire.has_value());
}

// Ctrl-drag lifts wires to move them: off an input, its wire's end moves to
// another input; off an output, its wires' start moves to another output.
// Let go on nothing, they are disconnected; put back, nothing changes.
void ctrl_drag_moves_wires_between_ports() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(12U, compositor::NodeKind::Composite, 320.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 200.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 12U, 0U));
  // Composite's LAYERS input at (320, 132); Output's IMAGE at (320, 272).
  {
    CompositorHarness harness;
    const editor::CompositorFrame moved = drag_in_graph(
        harness, graph, 330.0F, 132.0F, 330.0F, 272.0F, gui::KeyMods::Ctrl);
    UI_CHECK(moved.rewire ==
             (editor::CompositorRewireIntent{
                 {doc::NodeId{50U}},
                 {{compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
                   compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));
    UI_CHECK(!moved.move_node.has_value());
  }
  {
    CompositorHarness harness;
    UI_CHECK(drag_in_graph(harness, graph, 330.0F, 132.0F, 700.0F, 500.0F,
                           gui::KeyMods::Ctrl)
                 .rewire ==
             (editor::CompositorRewireIntent{{doc::NodeId{50U}}, {}}));
  }
  {
    CompositorHarness harness;
    const editor::CompositorFrame put_back = drag_in_graph(
        harness, graph, 330.0F, 132.0F, 325.0F, 134.0F, gui::KeyMods::Ctrl);
    UI_CHECK(!put_back.rewire.has_value());
    UI_CHECK(!put_back.connect_ports.has_value());
  }

  compositor::CompositorGraph sources;
  sources.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  sources.nodes.push_back(
      graph_node(13U, compositor::NodeKind::SceneLayer, 40.0F, 200.0F));
  sources.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  sources.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  CompositorHarness harness;
  // From the first Layer's IMAGE output to the second's, at (216, 272).
  UI_CHECK(drag_in_graph(harness, sources, 216.0F, 132.0F, 216.0F, 272.0F,
                         gui::KeyMods::Ctrl)
               .rewire ==
           (editor::CompositorRewireIntent{
               {doc::NodeId{50U}},
               {{compositor::EdgeEndpoint{doc::NodeId{13U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));
}

// A node with no wires of its own, dropped onto a wire it can carry, is
// spliced into it: the wire's source feeds the node, the node feeds the
// wire's old target, as one gesture with the move.
void a_free_node_dropped_on_a_wire_splices_into_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(12U, compositor::NodeKind::Composite, 300.0F, 300.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  // The wire runs straight along y=132. Composite's header is grabbed 40 in
  // and 9 down, and let go 9 below the wire.
  CompositorHarness harness;
  const editor::CompositorFrame dropped =
      drag_in_graph(harness, graph, 340.0F, 335.0F, 400.0F, 141.0F);
  UI_CHECK(dropped.move_node ==
           (editor::CompositorMoveIntent{doc::NodeId{12U}, 360.0F, 106.0F}));
  UI_CHECK(dropped.rewire ==
           (editor::CompositorRewireIntent{
               {doc::NodeId{50U}},
               {{compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{12U}, 0U}},
                {compositor::EdgeEndpoint{doc::NodeId{12U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));

  // Let go well away from the wire, it is only a move.
  CompositorHarness away;
  const editor::CompositorFrame moved =
      drag_in_graph(away, graph, 340.0F, 335.0F, 400.0F, 220.0F);
  UI_CHECK(moved.move_node.has_value());
  UI_CHECK(!moved.rewire.has_value());

  // A node that already has a wire is not spliced in.
  compositor::CompositorGraph wired = graph;
  wired.nodes.push_back(
      graph_node(13U, compositor::NodeKind::SceneLayer, 40.0F, 400.0F));
  wired.edges.push_back(graph_edge(51U, 13U, 0U, 12U, 0U));
  CompositorHarness busy;
  UI_CHECK(!drag_in_graph(busy, wired, 340.0F, 335.0F, 400.0F, 141.0F)
                .rewire.has_value());
}

// Double-click a wire to add a reroute point; drag a point to move it;
// double-click a point to remove it. A routed wire is picked along its route.
void reroute_points_are_added_moved_and_removed() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    const editor::CompositorFrame first =
        harness.frame(graph, 400.0F, 132.0F, true);
    UI_CHECK(!first.waypoints.has_value());
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    UI_CHECK(harness.frame(graph, 400.0F, 132.0F, true).waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U},
                                               {{400.0F, 106.0F}}}));
  }

  compositor::CompositorGraph routed = graph;
  routed.edges.front().points = {{400.0F, 306.0F}};
  // Through (400, 332) on screen: each stretch is a curve symmetric about
  // its middle, so (308, 232) is on the first one and the straight path's
  // middle is not on either.
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 308.0F, 232.0F, false));
    UI_CHECK(harness.frame(routed, 308.0F, 232.0F, true).selection ==
             (editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                          doc::NodeId{50U}}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 408.0F, 132.0F, false));
    UI_CHECK(harness.frame(routed, 408.0F, 132.0F, true).selection ==
             editor::CompositorSelection{});
  }
  {
    CompositorHarness harness;
    UI_CHECK(drag_in_graph(harness, routed, 400.0F, 332.0F, 420.0F, 400.0F)
                 .waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U},
                                               {{420.0F, 374.0F}}}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, false));
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, true));
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, false));
    UI_CHECK(harness.frame(routed, 400.0F, 332.0F, true).waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U}, {}}));
  }
}

// A wire that would make a loop is refused, rather than authored and then
// reported by the graph.
void a_wire_that_would_make_a_loop_is_refused() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Composite, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 320.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 1U, 0U, 2U, 0U));
  CompositorHarness harness;
  const editor::CompositorFrame refused =
      drag_in_graph(harness, graph, 496.0F, 132.0F, 50.0F, 132.0F);
  UI_CHECK(!refused.connect_ports.has_value());
  UI_CHECK(!refused.rewire.has_value());
}

// What other panels ask before a drop: the compatible input under or near
// the pointer, the wire it would replace, and where a new node goes.
void drops_from_other_panels_find_compatible_inputs() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 2U, 0U, 11U, 0U));
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));

  const auto effects = harness.panel.port_drop_target(
      graph, 330.0F, 132.0F, compositor::PortValueType::Effect);
  UI_CHECK(effects.has_value());
  UI_CHECK(effects->endpoint == (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(effects->x == 320.0F && effects->y == 132.0F);
  UI_CHECK(effects->multiple && !effects->occupied.has_value());
  // The row below takes array instances, and the effects dot is out of reach.
  UI_CHECK(!harness.panel
                .port_drop_target(graph, 330.0F, 158.0F,
                                  compositor::PortValueType::Effect)
                .has_value());
  const auto image = harness.panel.port_drop_target(
      graph, 610.0F, 132.0F, compositor::PortValueType::Image);
  UI_CHECK(image.has_value());
  UI_CHECK(image->endpoint == (compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}));
  UI_CHECK(image->occupied == doc::NodeId{50U});
  UI_CHECK(!image->multiple);
  UI_CHECK(!harness.panel
                .port_drop_target(graph, 610.0F, 132.0F,
                                  compositor::PortValueType::Effect)
                .has_value());

  // A new Blur goes 70 left of the input with its output dot level with it.
  const gui::UiPoint at = harness.panel.node_position_feeding(
      compositor::NodeKind::Blur, *effects);
  UI_CHECK(at.x + 176.0F + 70.0F == 320.0F);
  UI_CHECK(at.y + 46.0F == 132.0F - 26.0F);
}

// A wire made by letting go on an input stays drawn until the graph reports
// it, and is not drawn twice once it has: the connection never blinks.
void a_new_wire_is_held_until_the_graph_reports_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;
  UI_CHECK(drag_in_graph(harness, graph, 216.0F, 132.0F, 320.0F, 132.0F)
               .connect_ports.has_value());
  static_cast<void>(harness.frame(graph, 600.0F, 500.0F, false));
  const std::size_t held = harness.list.vertices().size();
  for (int frame = 0; frame < 125; ++frame) {
    static_cast<void>(harness.frame(graph, 600.0F, 500.0F, false));
  }
  const std::size_t refused = harness.list.vertices().size();
  UI_CHECK(held > refused);

  CompositorHarness arriving;
  UI_CHECK(drag_in_graph(arriving, graph, 216.0F, 132.0F, 320.0F, 132.0F)
               .connect_ports.has_value());
  compositor::CompositorGraph connected = graph;
  connected.edges.push_back(graph_edge(50U, 1U, 0U, 2U, 0U));
  static_cast<void>(arriving.frame(connected, 600.0F, 500.0F, false));
  const std::size_t arrived = arriving.list.vertices().size();
  for (int frame = 0; frame < 125; ++frame) {
    static_cast<void>(arriving.frame(connected, 600.0F, 500.0F, false));
  }
  UI_CHECK(arriving.list.vertices().size() == arrived);
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
  // The cohort check is the Program row's own action, at its right edge.
  constexpr float x = 409.0F;
  constexpr float y = 68.0F;
  static_cast<void>(harness.frame(x, y, false, state));
  static_cast<void>(harness.frame(x, y, true, state));
  const auto clicked = harness.frame(x, y, false, state);
  UI_CHECK(clicked.check_render_conformance);
  UI_CHECK(!clicked.lifecycle.has_value());
  UI_CHECK(!harness.frame(x, y, false, state).check_render_conformance);
  state.cluster.conformance_active = false;
  static_cast<void>(harness.frame(x, y, true, state));
  UI_CHECK(!harness.frame(x, y, false, state).check_render_conformance);
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

  // Offline media keeps its warning but has a renderer fallback, so the
  // shipping Start control must remain reachable.
  node.missing_media_only = true;
  node.content_detail = "offline media will display placeholders";
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto offline_start = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(offline_start.lifecycle.has_value());
  UI_CHECK(offline_start.lifecycle->action == editor::OutputLifecycleAction::Start);
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

// A node whose displays never opened reports nothing for its renderer or its
// assets, so every other row on it reads like a slow load. The output status
// is the one field that separates "not driving its displays" from "not
// finished yet", and each physical fault has to stay distinguishable: an
// operator sent to check the assets when a display is mirrored looks in the
// wrong place entirely.
void output_failures_are_named_and_distinct_from_asset_failures() {
  using OutputStatus = editor::OutputMonitorState::Node::OutputStatus;
  editor::OutputMonitorState::Node node{};
  // Nothing reported is not a failure, and is never evidence of success.
  UI_CHECK(!node.output_failed());
  UI_CHECK(editor::output_node_deployment_label(node).empty());

  node.output = OutputStatus::Deployed;
  UI_CHECK(!node.output_failed());
  UI_CHECK(editor::output_node_deployment_label(node).empty());

  node.output = OutputStatus::DisplayMirrored;
  UI_CHECK(node.output_failed());
  const auto mirrored = editor::output_node_deployment_label(node);
  UI_CHECK(mirrored == "BOUND DISPLAY MIRRORS ANOTHER - EXTEND THE DESKTOP");

  node.output = OutputStatus::PresenterRefused;
  const auto refused = editor::output_node_deployment_label(node);
  UI_CHECK(refused == "DISPLAY DOES NOT OFFER THE SAVED MODE");
  UI_CHECK(refused != mirrored);

  node.output = OutputStatus::DisplayMissing;
  UI_CHECK(editor::output_node_deployment_label(node) !=
           "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF");
  node.output = OutputStatus::DisplayInactive;
  UI_CHECK(editor::output_node_deployment_label(node) ==
           "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF");

  // A display fault says nothing about the assets, and the asset label must
  // not start describing one.
  node.content = editor::OutputMonitorState::Node::ContentStatus::Ready;
  node.assets_required = 2U;
  node.assets_ready = 2U;
  UI_CHECK(editor::output_node_content_label(node) == "ASSETS 2/2 READY");
}

void process_running_is_distinct_from_editor_control() {
  editor::OutputMonitorState::Node node{};
  node.status = editor::OutputMonitorState::Node::Status::Running;
  UI_CHECK(editor::output_node_connection_label(node) == "CONNECTING");
  node.connection = editor::OutputMonitorState::Node::Connection::CatchingUp;
  UI_CHECK(editor::output_node_connection_label(node) == "CATCHING UP");
  node.connection = editor::OutputMonitorState::Node::Connection::Live;
  UI_CHECK(editor::output_node_connection_label(node) == "LIVE");
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
  // Scroll the list to its lower bound, then click Start on the last row,
  // which then sits at the bottom of the list, y=86..118.
  static_cast<void>(harness.frame(400.0F, 90.0F, false, state, -10.0F));
  static_cast<void>(harness.frame(409.0F, 102.0F, false, state));
  static_cast<void>(harness.frame(409.0F, 102.0F, true, state));
  const auto released = harness.frame(409.0F, 102.0F, false, state);
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
  // Footer buttons run right to left: Stop all, Restart all, Start all.
  // With the glyphless test font each is 24 wide: Restart all is x=370..394.
  static_cast<void>(harness.frame(382.0F, 134.0F, false, state));
  static_cast<void>(harness.frame(382.0F, 134.0F, true, state));
  const auto released = harness.frame(382.0F, 134.0F, false, state);
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
    input.set_mouse_position(409.0F, 100.0F);
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
    return gui::Rect{card_x + 14.0F, card_y + 374.0F, card_width - 28.0F,
                     24.0F};
  }
  [[nodiscard]] static gui::Rect clear_proxy_cache_button() {
    return gui::Rect{card_x + 14.0F, card_y + 498.0F, 118.0F, 26.0F};
  }
  [[nodiscard]] static gui::Rect low_latency_preset() {
    const float width = (card_width - 28.0F - 18.0F) / 4.0F;
    return gui::Rect{card_x + 14.0F, card_y + 566.0F, width, 24.0F};
  }
  [[nodiscard]] static gui::Rect balanced_transcode_priority() {
    const float start = card_x + 14.0F + 132.0F;
    const float width = (card_x + card_width - 14.0F - start - 12.0F) / 3.0F;
    return gui::Rect{start + width + 6.0F, card_y + 466.0F, width, 24.0F};
  }
  // RENDER NODES: the password well, then its CLEAR button.
  [[nodiscard]] static gui::Rect password_field() {
    return gui::Rect{card_x + 146.0F, card_y + 286.0F, 376.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect clear_password_button() {
    return gui::Rect{card_x + card_width - 90.0F, card_y + 286.0F, 76.0F,
                     24.0F};
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

// The render node password is typed, never shown back, and reported with
// the save only when the operator touched it: a new password, or empty for
// none. Untouched, the stored one stays.
void a_render_node_password_is_typed_cleared_and_saved() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);
  static_cast<void>(harness.click(PreferencesHarness::password_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "hunter2"));
  UI_CHECK(harness.panel.draft().render_node_password);
  auto typed = harness.click(PreferencesHarness::save_button());
  UI_CHECK(typed.save.has_value());
  UI_CHECK(typed.save->render_node_password);
  UI_CHECK(typed.render_node_password == std::optional<std::string>{"hunter2"});

  // Set before: CLEAR removes it.
  harness.state.stored.render_node_password = true;
  harness.panel.open(harness.state.stored);
  static_cast<void>(
      harness.click(PreferencesHarness::clear_password_button()));
  UI_CHECK(!harness.panel.draft().render_node_password);
  auto cleared = harness.click(PreferencesHarness::save_button());
  UI_CHECK(cleared.save.has_value());
  UI_CHECK(!cleared.save->render_node_password);
  UI_CHECK(cleared.render_node_password == std::optional<std::string>{""});

  // Untouched: another change saves, the password is not reported.
  harness.panel.open(harness.state.stored);
  static_cast<void>(
      harness.click(PreferencesHarness::next_to_source_checkbox()));
  auto other = harness.click(PreferencesHarness::save_button());
  UI_CHECK(other.save.has_value());
  UI_CHECK(other.save->render_node_password);
  UI_CHECK(!other.render_node_password.has_value());
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
  static constexpr float category_count = 2.0F;
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
  [[nodiscard]] static gui::Rect tracking_tab() { return category_tab(0U); }
  [[nodiscard]] static gui::Rect lens_tab() { return category_tab(1U); }
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

// Tests for the docked, immediately authored scene controls. A frame adopts
// emitted values just as the Director does, so subsequent edits must preserve
// earlier changes and keep the correct node's base values.
struct LiveSettingsHarness {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input,list,font};
  editor::SceneSettingsPanel panel;
  editor::ProjectSettingsState state;
  editor::SceneSettingsTarget target{editor::SceneSettingsTarget::Renderer};
  std::vector<scene::ChannelKeyframes> keys;
  std::vector<editor::ElementInfo> elements;
  mediaserver::core::u64 time{};
  gui::Rect bounds{0,0,330,2000};
  LiveSettingsHarness() {state.stage_node=doc::NodeId{10};state.node=doc::NodeId{20};}
  editor::SceneSettingsFrame draw() {
    auto result=panel.draw(ctx,bounds,target,state,keys,time,elements);
    ctx.draw_overlays(bounds);
    if(result.project.apply) {
      state.renderer=result.project.apply->renderer;state.settings=result.project.apply->settings;
      state.environment=result.project.apply->environment;state.lens_profile=result.project.apply->lens_profile;
    }
    return result;
  }
  editor::SceneSettingsFrame frame(float x,float y,bool down) {
    input.new_frame();input.set_mouse_position(x,y);input.set_mouse_button(gui::MouseButton::Left,down);ctx.new_frame();return draw();
  }
  editor::SceneSettingsFrame click(float y,float x=200) {
    static_cast<void>(frame(x,y+11,false));static_cast<void>(frame(x,y+11,true));return frame(x,y+11,false);
  }
  editor::SceneSettingsFrame key(gui::Key key) {
    input.new_frame();input.add_key(key);ctx.new_frame();return draw();
  }
  editor::SceneSettingsFrame enter(float y,std::string_view value) {
    static_cast<void>(click(y));input.new_frame();input.add_text(value);ctx.new_frame();static_cast<void>(draw());return key(gui::Key::Enter);
  }
  editor::SceneSettingsFrame choose(float y,std::size_t from,std::size_t to) {
    static_cast<void>(click(y));
    while(from<to){static_cast<void>(key(gui::Key::Down));++from;}
    while(from>to){static_cast<void>(key(gui::Key::Up));--from;}
    return key(gui::Key::Enter);
  }
  void advanced_section(std::size_t section) {
    state.renderer.backend=scene::RendererBackend::Advanced;
    static_cast<void>(click(58,30));
    static_cast<void>(click(58.0F+40.0F*static_cast<float>(section),30));
  }
  void environment_section(std::size_t section) {
    target=editor::SceneSettingsTarget::Environment;
    static_cast<void>(click(58,30));
    static_cast<void>(click(58.0F+40.0F*static_cast<float>(section),30));
  }
};

void project_settings_apply_reports_the_edited_values() {
  ProjectSettingsHarness h;h.panel.open(h.state.settings,h.state.renderer);
  UI_CHECK(!h.click(ProjectSettingsHarness::apply_button()).apply);
  static_cast<void>(h.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  auto result=h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply && result.apply->renderer.icvfx.inner_frustum!=h.state.renderer.icvfx.inner_frustum);
  UI_CHECK(!h.panel.is_open());
}

void scene_file_fields_request_the_matching_picker_without_editing() {
  LiveSettingsHarness environment;
  environment.target = editor::SceneSettingsTarget::Environment;
  environment.state.environment.map_source = "C:/lighting/stage.exr";
  const auto map = environment.click(154, 285);
  UI_CHECK(map.file_picker && map.file_picker->kind == editor::FilePickerKind::EnvironmentMap);
  UI_CHECK(map.file_picker->current == "C:/lighting/stage.exr");
  UI_CHECK(!map.project.apply);

  LiveSettingsHarness renderer;
  renderer.advanced_section(2);
  renderer.state.renderer.color.show_lut_source = "C:/color/show.cube";
  const auto lut = renderer.click(234, 285);
  UI_CHECK(lut.file_picker && lut.file_picker->kind == editor::FilePickerKind::ShowLut);
  UI_CHECK(lut.file_picker->current == "C:/color/show.cube");
  UI_CHECK(!lut.project.apply);

  LiveSettingsHarness bake;
  bake.environment_section(2);
  bake.panel.set_bake_destination("C:/exports/lighting");
  const auto folder = bake.click(618, 285);
  UI_CHECK(folder.file_picker && folder.file_picker->kind == editor::FilePickerKind::BakeFolder);
  UI_CHECK(folder.file_picker->current == "C:/exports/lighting");
  UI_CHECK(!folder.project.apply && !folder.project.bake);
}

void baked_lighting_page_starts_cancels_and_preserves_the_authored_field() {
  LiveSettingsHarness h;h.state.environment.baked.enabled=true;h.state.environment.baked.source="lighting.msgi";
  h.state.environment.baked.content="1234567890abcdef";h.state.environment.baked.count_x=3;
  h.environment_section(2);
  UI_CHECK(!h.click(650.0).project.bake);
  h.state.renderer.backend=scene::RendererBackend::Advanced;
  auto bake=h.click(650.0);
  UI_CHECK(bake.project.bake && *bake.project.bake==h.state.environment.baked && !bake.project.bake_destination.empty());
  h.state.bake_running=true;auto cancel=h.click(650.0);UI_CHECK(cancel.project.cancel_bake && !cancel.project.bake);
}

void reflection_targets_choose_scene_objects_without_truncating_ids() {
  LiveSettingsHarness h;
  editor::ElementInfo floor{};
  floor.node = doc::NodeId{0xF123456789ABCDEFULL};
  floor.kind = editor::ElementKind::Object;
  floor.display_name = "Stage floor";
  h.elements.push_back(floor);
  h.state.environment.planar.enabled = true;
  h.state.environment.planar.receiver = doc::NodeId{999U};
  h.state.environment.baked.reflections[0].enabled = true;
  h.environment_section(3);
  UI_CHECK(h.state.environment.planar.receiver == doc::NodeId{999U});
  const auto receiver = h.choose(242.0, 2, 1);
  UI_CHECK(receiver.project.apply);
  UI_CHECK(receiver.project.apply->environment.planar.receiver == floor.node);
  const auto probe = h.choose(530.0, 0, 1);
  UI_CHECK(probe.project.apply);
  UI_CHECK(probe.project.apply->environment.baked.reflections[0].source == floor.node);
}

void project_settings_cancel_discards_the_draft() {
  ProjectSettingsHarness harness;
  harness.state.settings = scene::EffectSettings{};
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  const auto cancelled = harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!cancelled.apply.has_value());
  UI_CHECK(!harness.panel.is_open());

  // Reopening seeds from the project again, so the discarded edit is gone.
  harness.panel.open(harness.state.settings, harness.state.renderer);
  const auto reapplied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(!reapplied.apply.has_value());
}

void project_settings_defaults_restores_the_built_in_values() {
  ProjectSettingsHarness h;h.state.renderer.icvfx.latency_frames=4;
  h.state.renderer.bloom_strength=0.75F;h.state.environment.fog_density=0.2F;
  h.panel.open(h.state.settings,h.state.renderer,{},h.state.environment);
  static_cast<void>(h.click(ProjectSettingsHarness::defaults_button()));
  auto result=h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply && result.apply->renderer.icvfx==scene::IcvfxSettings{});
  UI_CHECK(result.apply->renderer.bloom_strength==0.75F && result.apply->environment.fog_density==0.2F);
}

void advanced_renderer_required_is_an_explicit_project_edit() {
  LiveSettingsHarness h;
  auto backend=h.choose(90.0,0,1);UI_CHECK(backend.project.apply && h.state.renderer.backend==scene::RendererBackend::Advanced);
  auto required=h.click(122.0);UI_CHECK(required.project.apply && h.state.renderer.required);
  static_cast<void>(h.choose(154.0,1,2));UI_CHECK(h.state.renderer.quality==scene::RendererQuality::Cinematic);
}

void raster_only_clears_only_unavailable_future_pass_requests() {
  LiveSettingsHarness h;h.state.renderer.backend=scene::RendererBackend::Advanced;
  h.state.renderer.ray_tracing=true;h.state.renderer.ddgi=true;h.state.renderer.restir=true;h.state.renderer.denoising=true;
  h.state.renderer.taau=true;h.state.renderer.motion_blur=true;
  auto result=h.click(218.0);UI_CHECK(result.project.apply);
  UI_CHECK(!h.state.renderer.ray_tracing && !h.state.renderer.ddgi && !h.state.renderer.restir && !h.state.renderer.denoising);
  UI_CHECK(h.state.renderer.taau && h.state.renderer.motion_blur);
}

// Occlusion is authored as a radius, and zero is the off.
//
// A toggle beside a value would be two ways to say the same thing, and the
// renderer only reads one of them - so the panel offers exactly the control
// the renderer obeys.
void occlusion_is_authored_as_a_radius() {
  LiveSettingsHarness h;h.advanced_section(1);
  auto result=h.enter(258.0,"2.5");UI_CHECK(result.project.apply && h.state.renderer.occlusion_radius==2.5F);
  static_cast<void>(h.enter(258.0,"0"));UI_CHECK(h.state.renderer.occlusion_radius==0.0F);
}

// Antialiasing is one choice, authored, with the jitter period behind it.
//
// The panel stores two booleans and the renderer reads one decision, so the
// thing worth asserting is that they cannot disagree: no click can leave both
// set, and what the buttons show is what `antialiasing_for` returns.
void antialiasing_is_one_authored_choice() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.choose(130.0,0,2));UI_CHECK(h.state.renderer.taau && !h.state.renderer.spatial_antialiasing);
  static_cast<void>(h.enter(162.0,"33"));UI_CHECK(h.state.renderer.taa_sequence_length==33);
  static_cast<void>(h.choose(130.0,2,1));UI_CHECK(!h.state.renderer.taau && h.state.renderer.spatial_antialiasing);
  static_cast<void>(h.choose(130.0,1,0));UI_CHECK(scene::antialiasing_for(h.state.renderer)==scene::AntialiasingMode::None);
}

void geometry_controls_emit_replicated_settings() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.click(250.0));static_cast<void>(h.click(282.0));static_cast<void>(h.click(314.0));
  UI_CHECK(!h.state.renderer.frustum_culling && !h.state.renderer.geometry_instancing && !h.state.renderer.model_lod);
}

void object_motion_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.click(162.0));UI_CHECK(h.state.renderer.motion_blur);
  static_cast<void>(h.enter(194.0,"16"));UI_CHECK(h.state.renderer.motion_blur_radius==16);
  static_cast<void>(h.click(194.0));h.input.new_frame();h.input.add_text("9");h.ctx.new_frame();static_cast<void>(h.draw());
  static_cast<void>(h.key(gui::Key::Escape));UI_CHECK(h.state.renderer.motion_blur_radius==16);
}

void plate_dof_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.click(194.0));UI_CHECK(h.state.renderer.plate_depth_of_field);
  static_cast<void>(h.enter(226.0,"14"));UI_CHECK(h.state.renderer.plate_dof_radius==14);
  static_cast<void>(h.click(194.0));UI_CHECK(!h.state.renderer.plate_depth_of_field);
}

void texture_sampling_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.choose(442.0,0,3));UI_CHECK(h.state.renderer.texture_anisotropy==8);
  static_cast<void>(h.enter(474.0,"-0.75"));UI_CHECK(h.state.renderer.texture_lod_bias==-0.75F);
}

void texture_streaming_controls_emit_authored_policy() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.enter(378.0,"256"));static_cast<void>(h.enter(410.0,"2"));
  UI_CHECK(h.state.renderer.texture_budget_mib==256 && h.state.renderer.texture_mip_bias==2);
  // The field stops at the renderer's envelope rather than accepting a value it would cut.
  static_cast<void>(h.enter(378.0,"4096"));UI_CHECK(h.state.renderer.texture_budget_mib==1024);
  static_cast<void>(h.click(346.0));UI_CHECK(!h.state.renderer.texture_streaming);
}

void shadow_cascades_and_bounce_are_authored_beside_the_backend() {
  LiveSettingsHarness h;h.advanced_section(3);
  static_cast<void>(h.enter(210.0,"3"));UI_CHECK(h.state.renderer.shadow_cascades==3);
  static_cast<void>(h.enter(210.0,"0"));UI_CHECK(h.state.renderer.shadow_cascades==0);
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

// Map edits are immediate and preserve the other scene environment values.
void the_environment_category_authors_the_lighting_map() {
  LiveSettingsHarness h;h.target=editor::SceneSettingsTarget::Environment;
  static_cast<void>(h.click(154.0));h.input.new_frame();h.input.add_text("studio.hdr");h.ctx.new_frame();auto result=h.draw();
  UI_CHECK(result.project.apply && h.state.environment.map_source=="studio.hdr");
  static_cast<void>(h.key(gui::Key::Enter));static_cast<void>(h.enter(186.0,"2.5"));static_cast<void>(h.enter(218.0,"0.25"));
  UI_CHECK(h.state.environment.map_intensity==2.5F && h.state.environment.map_rotation==0.25F);
}

// Physical sky values use the same property key controls as fog and maps.
void physical_sky_controls_apply_and_author_a_time_key() {
  LiveSettingsHarness h;h.target=editor::SceneSettingsTarget::Environment;
  static_cast<void>(h.click(90.0));UI_CHECK(h.state.environment.physical_sky.enabled);
  static_cast<void>(h.enter(122.0,"16.5"));
  auto result=h.click(122.0,306);UI_CHECK(result.keys.size()==1 && result.keys[0].channel==scene::PropertyChannel::SkyTimeOfDay && result.keys[0].value==16.5);
  UI_CHECK(!result.keys[0].erase);
}

void environment_keys_preserve_interpolation_and_authored_base() {
  LiveSettingsHarness h;h.environment_section(1);
  scene::ChannelKeyframes channel{};channel.target=h.state.stage_node;channel.component=scene::PropertyChannel::FogDensity;
  channel.frames={0,60};channel.values={0.0,0.6};h.keys={channel};h.time=30;
  auto key=h.click(130.0,306);UI_CHECK(key.keys.size()==1 && std::abs(key.keys[0].value-0.3)<1e-6 && !key.keys[0].erase);
  h.time=60;auto erased=h.click(130.0,306);UI_CHECK(erased.keys.size()==1 && erased.keys[0].erase);
  h.time=30;auto edited=h.enter(130.0,"0.4");UI_CHECK(edited.keys.size()==1 && !edited.project.apply);
  UI_CHECK(h.state.environment.fog_density==0.0F);
}

// An environment key added between weighted Cubic keys keeps the value
// playback shows there, tangents and weights included.
void environment_keys_sample_weighted_cubic_tangents() {
  LiveSettingsHarness h;h.environment_section(1);
  scene::ChannelKeyframes channel{};channel.target=h.state.stage_node;channel.component=scene::PropertyChannel::FogDensity;
  channel.frames={0,32};channel.values={0.1,0.2};
  channel.interpolations={scene::KeyInterpolation::Cubic,scene::KeyInterpolation::Cubic};
  channel.tangents={scene::KeyTangent{scene::KeyTangentMode::Broken,0.0,0.02,{},4.0},
                    scene::KeyTangent{scene::KeyTangentMode::Broken,-0.005,0.0,12.0,{}}};
  h.keys={channel};h.time=13;
  auto key=h.click(130.0,306);
  UI_CHECK(key.keys.size()==1 && std::abs(key.keys[0].value-0.2025)<1e-6 && !key.keys[0].erase);
}

// Small physical-lighting scales retain their precision in numeric fields.
void photometric_scale_accepts_precise_numeric_input() {
  LiveSettingsHarness h;h.advanced_section(1);
  auto result=h.enter(322.0,"0.0062831853");UI_CHECK(result.project.apply);
  UI_CHECK(h.state.renderer.photometric_scale==0.0062831853F);
}

void colour_is_authored_from_its_own_tab() {
  LiveSettingsHarness h;h.advanced_section(2);
  static_cast<void>(h.choose(170.0,1,0));static_cast<void>(h.click(266.0));
  UI_CHECK(h.state.renderer.color.display_transform==scene::DisplayTransform::Srgb && !h.state.renderer.color.dither);
  static_cast<void>(h.choose(170.0,0,2));UI_CHECK(h.state.renderer.color.display_transform==scene::DisplayTransform::Aces13);
  UI_CHECK(!h.state.renderer.color.dither && h.state.renderer.backend==scene::RendererBackend::Advanced);
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
  UI_CHECK(status.routine);
  status = editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {});
  UI_CHECK(status.headline == "PROGRAM ACTIVATION MISSED");
  UI_CHECK(status.detail.find("retried") != std::string::npos);
  // A missed activation is the only wait the monitor raises at once.
  UI_CHECK(!status.routine);
  local.visible_commit = 10;
  status = editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {});
  UI_CHECK(status.headline == "CONFIRMING OUTPUT UPDATE" && status.routine);
  const auto expired = 10 + core::asset_commit_confirmation_frames;
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired - 1, true, cluster, {}).headline == "CONFIRMING OUTPUT UPDATE");
  status = editor::program_asset_status(local, {19, 10, 42}, expired, true, cluster, {});
  UI_CHECK(status.waiting && status.headline == "OUTPUT ACTIVATION MISSED");
  UI_CHECK(status.detail.find("retried") != std::string::npos);
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired + 10000, true, cluster, {}).headline == "OUTPUT ACTIVATION MISSED");
  status = editor::program_asset_status(local, {19, expired + 31, 42}, expired + 1, true, cluster, {});
  UI_CHECK(status.headline == "RETRYING OUTPUT UPDATE" && !status.routine);
  UI_CHECK(status.detail.find("30 control frames") != std::string::npos);
  cluster.asset_waiting_node = 3;
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired, true, cluster, {}).headline == "WAITING FOR OUTPUT NODE 3");
  cluster.asset_waiting_node.reset();
  UI_CHECK(editor::program_asset_status(local, {}, expired, true, cluster, {}).headline == "WAITING TO UPDATE PROGRAM");
  cluster.asset_cohort.visible = true;
  UI_CHECK(!editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {}).waiting);
}

void shadow_stability_controls_apply_and_reopen() {
  LiveSettingsHarness h;h.advanced_section(3);
  static_cast<void>(h.enter(242.0,"0.2"));static_cast<void>(h.enter(274.0,"3.5"));static_cast<void>(h.enter(306.0,"0.4"));static_cast<void>(h.enter(338.0,"0.05"));
  UI_CHECK(h.state.renderer.shadow_blend_band==0.2F && h.state.renderer.shadow_filter_radius==3.5F);
  UI_CHECK(h.state.renderer.contact_shadow_length==0.4F && h.state.renderer.contact_shadow_thickness==0.05F);
}

void linked_tree_preserves_hierarchy_collapse_and_range() {
  SceneHarness h; h.items.clear();
  editor::ElementInfo collection; collection.node=doc::NodeId{500}; collection.linked=true;
  collection.linked_collection=true; collection.linked_instance=collection.node; collection.kind=editor::ElementKind::Group;
  collection.display_name="Set";
  auto parent=collection; parent.node=doc::NodeId{502}; parent.linked_collection=false; parent.linked_parent=collection.node; parent.display_name="Parent";
  auto child=parent; child.node=doc::NodeId{501}; child.linked_parent=parent.node; child.display_name="Prop";
  h.items={child,collection,parent};
  // Collection, its object, and the object's part: rows 0, 1 and 2.
  static_cast<void>(h.frame(80,scene_row(2),false)); static_cast<void>(h.frame(80,scene_row(2),true,gui::KeyMods::Shift));
  auto selected=h.frame(80,scene_row(2),false,gui::KeyMods::Shift);
  UI_CHECK(selected && selected->node==child.node);
  UI_CHECK(selected->visible_order==std::vector<doc::NodeId>{collection.node,parent.node,child.node});
  // The collection's twirl (x=28..44) closes it without selecting anything.
  static_cast<void>(h.frame(36,scene_row(0),true)); UI_CHECK(!h.frame(36,scene_row(0),false));
  static_cast<void>(h.frame(80,scene_row(2),false));
  selected=h.frame(80,scene_row(2),true);
  UI_CHECK(selected && selected->action==editor::SelectionAction::Clear);
  static_cast<void>(h.frame(80,scene_row(0),false)); static_cast<void>(h.frame(80,scene_row(0),true));
  selected=h.frame(80,scene_row(0),false); UI_CHECK(selected && selected->node==collection.node);
}

constexpr std::array<Case, 129U> cases{{
    {"a scene row renames in place", &a_scene_row_renames_in_place},
    {"a scene eye hides and shows", &a_scene_eye_hides_and_shows},
    {"scene rows drag into groups and between siblings",
     &scene_rows_drag_into_groups_and_between_siblings},
    {"linked tree preserves hierarchy collapse and range", &linked_tree_preserves_hierarchy_collapse_and_range},
    {"reflection targets choose scene objects without truncating IDs",
     &reflection_targets_choose_scene_objects_without_truncating_ids},
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
    {"environment keys preserve interpolation and authored base",
     &environment_keys_preserve_interpolation_and_authored_base},
    {"environment keys sample weighted cubic tangents",
     &environment_keys_sample_weighted_cubic_tangents},
    {"a scene row reports the clicked node",
     &a_scene_row_reports_the_clicked_node},
    {"LED surfaces list after the scene with a lock",
     &led_surfaces_list_after_the_scene_with_a_lock},
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
    {"Compositor Scene Layers show the referenced element name",
     &compositor_scene_layers_show_the_referenced_element_name},
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
    {"Compositor replacement picks the topmost node and clips feedback",
     &compositor_replacement_picks_topmost_and_clips_feedback},
    {"a released node holds its position until the graph reports it",
     &a_released_node_holds_its_position_until_the_graph_reports_it},
    {"wires are picked along their curve",
     &wires_are_picked_along_their_curve},
    {"a wire snaps to a compatible port near the pointer",
     &a_wire_snaps_to_a_compatible_port_near_the_pointer},
    {"an input port's target covers its whole row",
     &an_input_ports_target_covers_its_whole_row},
    {"the Parameters panel resizes from its edge",
     &the_parameters_panel_resizes_from_its_edge},
    {"long parameter lists scroll to their last field",
     &long_parameter_lists_scroll_to_their_last_field},
    {"parameter sections collapse",
     &parameter_sections_collapse},
    {"node text fits unclipped in a real font",
     &node_text_fits_unclipped_in_a_real_font},
    {"a wire let go on empty canvas offers nodes that take it",
     &a_wire_let_go_on_empty_canvas_offers_nodes_that_take_it},
    {"Tab opens the node search at the pointer",
     &tab_opens_the_node_search_at_the_pointer},
    {"Alt-click breaks wires", &alt_click_breaks_wires},
    {"Ctrl-drag moves wires between ports",
     &ctrl_drag_moves_wires_between_ports},
    {"a free node dropped on a wire splices into it",
     &a_free_node_dropped_on_a_wire_splices_into_it},
    {"reroute points are added, moved and removed",
     &reroute_points_are_added_moved_and_removed},
    {"a wire that would make a loop is refused",
     &a_wire_that_would_make_a_loop_is_refused},
    {"drops from other panels find compatible inputs",
     &drops_from_other_panels_find_compatible_inputs},
    {"a new wire is held until the graph reports it",
     &a_new_wire_is_held_until_the_graph_reports_it},
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
    {"running process needs an editor connection", &process_running_is_distinct_from_editor_control},
    {"content labels distinguish missing stale and verification failures",
     &content_labels_distinguish_missing_stale_and_verification_failures},
    {"output failures are named and distinct from asset failures",
     &output_failures_are_named_and_distinct_from_asset_failures},
    {"output monitor scroll reaches later cluster nodes",
     &output_monitor_scroll_reaches_later_cluster_nodes},
    {"output monitor emits explicit cluster relaunch",
     &output_monitor_emits_explicit_cluster_relaunch},
    {"output monitor cluster health keeps node controls reachable",
     &output_monitor_cluster_health_keeps_node_controls_reachable},
    {"project settings apply reports the edited values",
     &project_settings_apply_reports_the_edited_values},
    {"scene file fields request matching pickers without editing",
     &scene_file_fields_request_the_matching_picker_without_editing},
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
    {"a render node password is typed cleared and saved",
     &a_render_node_password_is_typed_cleared_and_saved},
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
