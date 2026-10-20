#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(MEDIASERVER_TIMELINE_BENCH)
#include <algorithm>
#include <chrono>
#include <cstdio>
#endif

// The bespoke timeline's proof, with no window and no GPU. Its contract is the
// requests it reports to the shell, so synthetic pointer frames can exercise
// the whole interaction: transport, range, selection, ruler scrub, keying, and
// a retime drag that commits exactly once on release.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace assets = mediaserver::assets;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

const doc::NodeId object_node{5U};
const doc::NodeId clip_node{7U};
const doc::NodeId surface_node{90U};
const doc::NodeId lane_node{40U};
const doc::NodeId destination_lane_node{41U};

[[nodiscard]] editor::ElementInfo make_object() {
  editor::ElementInfo out{};
  out.node = object_node;
  out.kind = editor::ElementKind::Object;
  out.position = scene::Vec3{1.0F, 2.0F, 3.0F};
  out.rotation = scene::Vec3{0.1F, 0.2F, 0.3F};
  out.scale = scene::Vec3{1.0F, 1.5F, 2.0F};
  return out;
}

// Placed at frame 20 for forty frames. At the harness's 1000px width the
// graph starts at x=260 and holds 7.4 pixels per frame, so the clip's bar runs
// x=408..704 on the second layer row, y=101..117.
[[nodiscard]] editor::ElementInfo make_clip() {
  editor::ElementInfo out{};
  out.node = clip_node;
  out.kind = editor::ElementKind::Media;
  out.sequence_count = 240U;
  out.timing = scene::ClipTiming{20, 40, 0, 0, 0};
  return out;
}

[[nodiscard]] assets::DerivativeKey
filmstrip_key(mediaserver::core::u64 revision = 1U) {
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
  UI_CHECK(profile != nullptr);
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 4'096U + revision;
  fingerprint.sha256.bytes[0U] =
      static_cast<mediaserver::core::u8>(revision & 0xffU);
  return assets::make_derivative_key(doc::NodeId{70U}, fingerprint, *profile);
}

[[nodiscard]] assets::DerivativeKey
waveform_key(mediaserver::core::u64 revision = 1U) {
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Waveform2048PeakRms);
  UI_CHECK(profile != nullptr);
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 8'192U + revision;
  fingerprint.sha256.bytes[0U] =
      static_cast<mediaserver::core::u8>(revision & 0xffU);
  return assets::make_derivative_key(doc::NodeId{70U}, fingerprint, *profile);
}

[[nodiscard]] output::ProjectionSurface make_surface() {
  output::ProjectionSurface surface{};
  surface.node = surface_node;
  surface.name = "Curved LED";
  surface.geometry.center = scene::Vec3{4.0F, 5.0F, 6.0F};
  surface.geometry.right = scene::Vec3{0.0F, 0.0F, -1.0F};
  surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.geometry.width = 8.0F;
  surface.geometry.height = 4.5F;
  surface.geometry.raster = scene::Canvas{1'920U, 1'080U};
  return surface;
}

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
#if defined(MEDIASERVER_TIMELINE_BENCH)
  // Lays out exactly like empty(), but draws which text went where.
  gui::FontAtlas font{gui::FontAtlas::placeholder()};
#else
  gui::FontAtlas font{gui::FontAtlas::empty()};
#endif
  gui::UiContext ctx{input, list, font};
  editor::TimelinePanel panel{gui::ui_id("timeline")};
  editor::EditorTimeline timeline{50U, 50U, 0U, 100U, false, false, false};
  std::vector<editor::ElementInfo> elements{make_object(), make_clip()};
  std::vector<scene::ChannelKeyframes> keys{
      scene::ChannelKeyframes{
          object_node, scene::TransformChannel::PositionX, {10, 20, 50}},
      scene::ChannelKeyframes{
          object_node, scene::TransformChannel::PositionY, {25, 50}},
  };
  std::vector<doc::NodeId> selection;
  doc::NodeId primary{};
  std::vector<scene::EffectInstance> effects;
  std::vector<output::ProjectionSurface> surfaces;
  doc::NodeId selected_surface{};
  std::vector<scene::TimelineLane> lanes;
  std::optional<mediaserver::core::i64> clipboard_span;
  std::optional<editor::TimelineKeyScalePreview> key_scale_preview;
  std::vector<editor::TimelineFilmstripObservation> filmstrips;
  std::vector<editor::TimelineWaveformObservation> waveforms;
  assets::WaveformEnvelope waveform_bins{};
  std::vector<scene::TimelineMarker> markers;

  gui::Rect bounds{0.0F, 0.0F, 1000.0F, 400.0F};

  // One editor frame. Overlays are drawn as the shell draws them, after the
  // panel, which is where a context menu learns where its entries are.
  editor::TimelinePanelFrame draw() {
    editor::TimelinePanelFrame out = panel.draw(
        ctx, bounds, timeline, keys, elements, selection, primary, effects,
        surfaces, selected_surface, lanes, clipboard_span, key_scale_preview,
        filmstrips, waveforms, markers);
    ctx.draw_overlays(gui::Rect{0.0F, 0.0F, 2'000.0F, 2'000.0F});
    return out;
  }

  editor::TimelinePanelFrame frame(float x, float y, bool down,
                                   gui::KeyMods mods = gui::KeyMods::None,
                                   float wheel = 0.0F,
                                   std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Right, false);
    input.set_mods(mods);
    if (wheel != 0.0F) {
      input.add_wheel(wheel);
    }
    if (!text.empty()) {
      input.add_text(text);
    }
    ctx.new_frame();
    return draw();
  }

  // A right click: the press asks for the menu of what is under it.
  editor::TimelinePanelFrame right_click(float x, float y) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Right, true);
    input.set_mods(gui::KeyMods::None);
    ctx.new_frame();
    editor::TimelinePanelFrame pressed = draw();
    input.new_frame();
    input.set_mouse_button(gui::MouseButton::Right, false);
    ctx.new_frame();
    static_cast<void>(draw());
    return pressed;
  }

  // Right-clicks at (x, y) and picks menu entry `entry`. Menus open 2px
  // under the pointer with 24px rows.
  editor::TimelinePanelFrame choose(float x, float y, std::size_t entry) {
    static_cast<void>(right_click(x, y));
    UI_CHECK(ctx.typing());
    return click(x + 10.0F, y + 14.0F + 24.0F * static_cast<float>(entry));
  }

  // Printable keys while the timeline has the operator's attention.
  editor::TimelinePanelFrame type(std::string_view text,
                                  gui::KeyMods mods = gui::KeyMods::None) {
    return frame(0.0F, 0.0F, false, mods, 0.0F, text);
  }

  // A press on bare toolbar gives the timeline the keyboard without touching
  // anything.
  void focus() { static_cast<void>(click(760.0F, 31.0F)); }

  editor::TimelinePanelFrame click(float x, float y,
                                   gui::KeyMods mods = gui::KeyMods::None) {
    static_cast<void>(frame(x, y, false, mods));
    static_cast<void>(frame(x, y, true, mods));
    return frame(x, y, false, mods);
  }

  editor::TimelinePanelFrame key(gui::Key pressed) {
    input.new_frame();
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, false);
    input.set_mods(gui::KeyMods::None);
    input.add_key(pressed);
    ctx.new_frame();
    return draw();
  }

  editor::TimelinePanelFrame key(gui::Key pressed, gui::KeyMods mods) {
    input.new_frame();
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mods(mods);
    input.add_key(pressed);
    ctx.new_frame();
    return draw();
  }

  editor::TimelinePanelFrame middle(float x, float y, bool down,
                                    gui::KeyMods mods = gui::KeyMods::None) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, down);
    input.set_mods(mods);
    ctx.new_frame();
    return draw();
  }

  void open_position_components() {
    // Layer row is y=76..98. Its twirl is left of the layer name.
    static_cast<void>(click(42.0F, 88.0F));
    // Position becomes the next row, y=98..120; indent puts its twirl at
    // x=26.
    static_cast<void>(click(58.0F, 112.0F));
    static_cast<void>(frame(0.0F, 0.0F, false));
  }
};

void transport_controls_report_their_action() {
  Harness h;
  // The transport icons begin at x=6: to In 6..30, back 32..56, play 58..88,
  // forward 90..114, to Out 116..140, then the loop toggle at 144..168.
  const editor::TimelinePanelFrame played = h.click(73.0F, 17.0F);
  UI_CHECK(played.transport == editor::TransportRequest::TogglePlay);

  const editor::TimelinePanelFrame start = h.click(18.0F, 17.0F);
  UI_CHECK(start.transport == editor::TransportRequest::GoToStart);
  UI_CHECK(h.click(44.0F, 17.0F).transport ==
           editor::TransportRequest::StepBack);
  UI_CHECK(h.click(102.0F, 17.0F).transport ==
           editor::TransportRequest::StepForward);
  UI_CHECK(h.click(128.0F, 17.0F).transport ==
           editor::TransportRequest::GoToEnd);
  // Loop belongs with the transport and flips only the loop flag.
  UI_CHECK(h.click(156.0F, 17.0F).range ==
           (editor::RangeRequest{0U, 100U, true}));
}

void the_playhead_readout_takes_typed_timecode_and_frames() {
  Harness h;
  // The readout sits at x=176..286. A click opens it for typing; the whole
  // edit lands on Enter, and a frame count or a relative offset works too.
  static_cast<void>(h.click(220.0F, 17.0F));
  const editor::TimelinePanelFrame typing = h.frame(220.0F, 17.0F, false);
  UI_CHECK(typing.editing);
  for (int index = 0; index < 12; ++index) {
    static_cast<void>(h.key(gui::Key::Backspace));
  }
  static_cast<void>(h.type("00:00:01:05"));
  const editor::TimelinePanelFrame entered = h.key(gui::Key::Enter);
  // 50 fps: one second and five frames.
  UI_CHECK(entered.seek_to_frame == std::optional<std::uint64_t>{55U});
  // The Enter that committed the time is not also GO.
  UI_CHECK(entered.transport == editor::TransportRequest::None);

  static_cast<void>(h.click(220.0F, 17.0F));
  for (int index = 0; index < 12; ++index) {
    static_cast<void>(h.key(gui::Key::Backspace));
  }
  static_cast<void>(h.type("+20"));
  UI_CHECK(h.key(gui::Key::Enter).seek_to_frame ==
           std::optional<std::uint64_t>{70U});

  // Dragging the readout scrubs a frame per pixel.
  static_cast<void>(h.frame(220.0F, 17.0F, true));
  const editor::TimelinePanelFrame scrubbed = h.frame(230.0F, 17.0F, true);
  UI_CHECK(scrubbed.seek_to_frame == std::optional<std::uint64_t>{60U});
  static_cast<void>(h.frame(230.0F, 17.0F, false));
}

void timecode_reads_and_writes_the_forms_an_operator_types() {
  UI_CHECK(editor::format_timecode(0U, 25U, 1U) == "00:00:00:00");
  UI_CHECK(editor::format_timecode(3'661U * 25U + 7U, 25U, 1U) ==
           "01:01:01:07");
  // 29.97 counts 30 to the second, as every figure in the editor does.
  UI_CHECK(editor::format_timecode(31U, 30'000U, 1'001U) == "00:00:01:01");

  UI_CHECK(editor::parse_timecode("01:00:00:00", 24U, 1U, 0U) ==
           std::optional<std::uint64_t>{86'400U});
  UI_CHECK(editor::parse_timecode("1:05", 50U, 1U, 0U) ==
           std::optional<std::uint64_t>{55U});
  UI_CHECK(editor::parse_timecode("02:00:00", 25U, 1U, 0U) ==
           std::optional<std::uint64_t>{3'000U});
  UI_CHECK(editor::parse_timecode("  420 ", 25U, 1U, 0U) ==
           std::optional<std::uint64_t>{420U});
  UI_CHECK(editor::parse_timecode("00;00;02;00", 25U, 1U, 0U) ==
           std::optional<std::uint64_t>{50U});
  UI_CHECK(editor::parse_timecode("-30", 25U, 1U, 20U) ==
           std::optional<std::uint64_t>{0U});
  UI_CHECK(editor::parse_timecode("+1:00", 25U, 1U, 10U) ==
           std::optional<std::uint64_t>{35U});
  UI_CHECK(!editor::parse_timecode("", 25U, 1U, 0U));
  UI_CHECK(!editor::parse_timecode("1::2", 25U, 1U, 0U));
  UI_CHECK(!editor::parse_timecode("1:2:3:4:5", 25U, 1U, 0U));
  UI_CHECK(!editor::parse_timecode("ten", 25U, 1U, 0U));
}

void the_ruler_scrubs_and_clamps_to_the_show_range() {
  Harness h;
  // At 1000px wide the list is 260px and the 0..100 range fits the 740px
  // graph. One quarter across is frame 25.
  static_cast<void>(h.frame(445.0F, 45.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(445.0F, 45.0F, true);
  UI_CHECK(pressed.seek_to_frame.has_value());
  UI_CHECK(pressed.seek_to_frame.value() == 25U);

  // Capture survives leaving the ruler and clamps at OUT.
  const editor::TimelinePanelFrame outside = h.frame(1400.0F, 45.0F, true);
  UI_CHECK(outside.seek_to_frame.has_value());
  UI_CHECK(outside.seek_to_frame.value() == 100U);
}

void a_layer_row_selects_and_ctrl_click_toggles_exactly() {
  Harness h;
  const editor::TimelinePanelFrame selected = h.click(80.0F, 88.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == object_node);
  UI_CHECK(selected.selection->action == editor::SelectionAction::Replace);

  const editor::TimelinePanelFrame toggled =
      h.click(80.0F, 88.0F, gui::KeyMods::Ctrl);
  UI_CHECK(toggled.selection.has_value());
  UI_CHECK(toggled.selection->node == object_node);
  UI_CHECK(toggled.selection->action == editor::SelectionAction::Toggle);
  UI_CHECK(toggled.selection->visible_order.empty());
}

void a_shift_layer_click_carries_only_visible_top_level_rows() {
  Harness h;
  // The clip is the second top-level row. Expanded property children are not
  // part of this order, so range membership remains layer-addressed.
  const editor::TimelinePanelFrame ranged =
      h.click(80.0F, 112.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection.has_value());
  UI_CHECK(ranged.selection->node == clip_node);
  UI_CHECK(ranged.selection->action == editor::SelectionAction::Range);
  UI_CHECK(ranged.selection->visible_order ==
           std::vector<doc::NodeId>{object_node, clip_node});
}

void physical_surface_rows_author_position_and_yaw_keys() {
  Harness h;
  h.elements.clear();
  h.keys.clear();
  h.surfaces.push_back(make_surface());
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  const editor::TimelinePanelFrame selected = h.click(80.0F, 88.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == surface_node);
  UI_CHECK(selected.selection->surface);

  // Surface layer y=56..78. Open it to reveal Position then Rotation.
  static_cast<void>(h.click(42.0F, 88.0F));
  const editor::TimelinePanelFrame position = h.click(248.0F, 112.0F);
  UI_CHECK(position.keys.size() == 3U);
  UI_CHECK(position.keys[0].target == surface_node);
  UI_CHECK(position.keys[0].component == scene::PropertyChannel::PositionX);
  UI_CHECK(position.keys[0].value == 4.0);
  UI_CHECK(position.keys[1].component == scene::PropertyChannel::PositionY);
  UI_CHECK(position.keys[1].value == 5.0);
  UI_CHECK(position.keys[2].component == scene::PropertyChannel::PositionZ);
  UI_CHECK(position.keys[2].value == 6.0);

  // A physical surface exposes only the horizontal Rotation/Yaw channel.
  const editor::TimelinePanelFrame yaw = h.click(248.0F, 136.0F);
  UI_CHECK(yaw.keys.size() == 1U);
  UI_CHECK(yaw.keys[0].target == surface_node);
  UI_CHECK(yaw.keys[0].component == scene::PropertyChannel::RotationYaw);
  UI_CHECK(std::abs(yaw.keys[0].value - 0.25) < 1.0e-6);
}

// An LED surface row carries the same padlock as every other layer, at the
// list's right edge; it asks for the opposite of the surface's current lock
// and never doubles as a selection. A surface has no eye.
void a_surface_row_carries_its_own_lock() {
  Harness h;
  h.elements.clear();
  h.keys.clear();
  h.surfaces.push_back(make_surface());
  h.surfaces.back().locked = false;
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  const editor::TimelinePanelFrame locked = h.click(246.0F, 88.0F);
  UI_CHECK(locked.element_flags.size() == 1U);
  UI_CHECK(locked.element_flags.front() ==
           (editor::TimelineElementFlagEdit{
               surface_node, editor::TimelineElementFlag::Locked, true}));
  UI_CHECK(!locked.selection.has_value());

  h.surfaces.back().locked = true;
  const editor::TimelinePanelFrame unlocked = h.click(246.0F, 88.0F);
  UI_CHECK(unlocked.element_flags.size() == 1U);
  UI_CHECK(unlocked.element_flags.front().value == false);

  const editor::TimelinePanelFrame no_eye = h.click(16.0F, 88.0F);
  UI_CHECK(no_eye.element_flags.empty());
}

void surface_transform_edits_key_only_animated_components() {
  editor::FeedSurfaceEditRequest request{};
  request.node = surface_node;
  request.name = "Curved LED";
  request.surface = make_surface().geometry;
  request.gesture = 77U;
  request.transform_components = editor::FeedSurfaceTransform::PositionX |
                                 editor::FeedSurfaceTransform::PositionY |
                                 editor::FeedSurfaceTransform::Yaw;

  std::vector<scene::ChannelKeyframes> channels{
      scene::ChannelKeyframes{surface_node,
                              scene::PropertyChannel::PositionX,
                              {0},
                              {4.0},
                              {scene::KeyInterpolation::Linear}},
      scene::ChannelKeyframes{surface_node,
                              scene::PropertyChannel::RotationYaw,
                              {0},
                              {0.25},
                              {scene::KeyInterpolation::Linear}},
  };
  const editor::RoutedSurfaceEdit routed =
      editor::route_surface_transform_edit(request, channels, 42U);

  UI_CHECK(routed.keys.size() == 2U);
  UI_CHECK(routed.keys[0] ==
           editor::KeyIntent{surface_node, scene::PropertyChannel::PositionX,
                             42U, 4.0, false, 77U});
  UI_CHECK(routed.keys[1].component == scene::PropertyChannel::RotationYaw);
  UI_CHECK(std::abs(routed.keys[1].value - 0.25) < 1.0e-6);
  UI_CHECK(routed.keys[1].gesture == 77U);
  UI_CHECK(routed.base.has_value());
  UI_CHECK(routed.base->gesture == 77U);
  UI_CHECK(editor::has_transform(routed.base->transform_components,
                                 editor::FeedSurfaceTransform::PositionY));
  UI_CHECK(!editor::has_transform(routed.base->transform_components,
                                  editor::FeedSurfaceTransform::PositionX));
  UI_CHECK(!editor::has_transform(routed.base->transform_components,
                                  editor::FeedSurfaceTransform::Yaw));

  const auto without_auto_key = editor::route_surface_transform_edit(
      request, channels, 42U, false);
  UI_CHECK(without_auto_key.keys.empty());
  UI_CHECK(without_auto_key.base.has_value());
  UI_CHECK(without_auto_key.base->transform_components == request.transform_components);

  request.transform_components = editor::FeedSurfaceTransform::None;
  const editor::RoutedSurfaceEdit calibration =
      editor::route_surface_transform_edit(request, channels, 42U);
  UI_CHECK(calibration.keys.empty());
  UI_CHECK(calibration.base.has_value());
  UI_CHECK(calibration.base->transform_components ==
           editor::FeedSurfaceTransform::None);
}

void timeline_search_filters_scene_layers_and_surfaces() {
  Harness h;
  h.surfaces.push_back(make_surface());

  // The search well occupies the fixed left side of the ruler, matching the
  // Scene panel's case-insensitive text-field behavior.
  static_cast<void>(h.click(80.0F, 45.0F));
  const editor::TimelinePanelFrame typed =
      h.frame(80.0F, 45.0F, false, gui::KeyMods::None, 0.0F, "cUrVeD");
  UI_CHECK(typed.editing);
  UI_CHECK(h.panel.search() == "cUrVeD");
  static_cast<void>(h.frame(80.0F, 45.0F, false));

  // The filtered surface becomes the first row; object/clip rows are gone.
  const editor::TimelinePanelFrame selected = h.click(80.0F, 88.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == surface_node);
  UI_CHECK(selected.selection->surface);
}

const doc::NodeId collection_node{500U};
const doc::NodeId linked_model_node{502U};
const doc::NodeId first_part_node{501U};
const doc::NodeId second_part_node{503U};
const doc::NodeId linked_light_node{504U};

void linked_hierarchy(Harness &h) {
  editor::ElementInfo collection{};
  collection.node = collection_node;
  collection.kind = editor::ElementKind::Group;
  collection.display_name = "Blender collection";
  collection.linked = collection.linked_collection = true;
  collection.linked_instance = collection.node;
  auto model = collection;
  model.node = linked_model_node;
  model.kind = editor::ElementKind::Model;
  model.linked_collection = false;
  model.linked_parent = collection.node;
  model.display_name = "Tank";
  auto part = model;
  part.node = first_part_node;
  part.linked_parent = model.node;
  part.display_name = "Tank / Hull";
  auto second = part;
  second.node = second_part_node;
  second.display_name = "Tank / Wheels";
  auto light = model;
  light.node = linked_light_node;
  light.kind = editor::ElementKind::PointLight;
  light.display_name = "Light";
  // Projection order is not parent-first: a material part can precede its
  // object. Sibling order must survive turning this into a visible tree.
  h.elements = {second, collection, part, model, light, make_object()};
  h.keys.clear();
}

void linked_models_fold_into_their_collection_and_object_hierarchy() {
  Harness h;
  linked_hierarchy(h);
  auto ranged = h.click(100.0F, 112.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection->node == object_node);
  UI_CHECK(ranged.selection->visible_order ==
           std::vector<doc::NodeId>{collection_node, object_node});

  const auto expanded = h.click(42.0F, 88.0F);
  UI_CHECK(!expanded.selection.has_value());
  UI_CHECK(expanded.keys.empty());
  UI_CHECK(!h.panel.dragging_layer());
  UI_CHECK(h.click(100.0F, 136.0F).selection->node == linked_model_node);
  static_cast<void>(h.click(58.0F, 136.0F));
  ranged = h.click(100.0F, 256.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection->visible_order == std::vector<doc::NodeId>{
      collection_node, linked_model_node, second_part_node, first_part_node,
      linked_light_node, object_node});

  static_cast<void>(h.click(42.0F, 88.0F));
  UI_CHECK(h.click(100.0F, 112.0F).selection->node == object_node);
  static_cast<void>(h.click(42.0F, 88.0F));
  UI_CHECK(h.click(100.0F, 184.0F).selection->node == second_part_node);
}

void parent_properties_fold_separately_and_key_the_parent() {
  Harness h;
  linked_hierarchy(h);
  static_cast<void>(h.click(42.0F, 88.0F));
  static_cast<void>(h.click(58.0F, 136.0F));
  // The model's Properties foldout is one level below its object row.
  const auto properties = h.click(74.0F, 160.0F);
  UI_CHECK(properties.keys.empty());
  UI_CHECK(!properties.selection.has_value());
  const auto keyed = h.click(247.0F, 184.0F);
  UI_CHECK(keyed.keys.size() == 3U);
  for (const auto &key : keyed.keys) {
    UI_CHECK(key.target == linked_model_node);
  }
  UI_CHECK(h.click(100.0F, 280.0F).selection->node == second_part_node);
  static_cast<void>(h.click(74.0F, 160.0F));
  UI_CHECK(h.click(100.0F, 184.0F).selection->node == second_part_node);
}

void hierarchy_search_reveals_matching_parts_with_parent_context() {
  Harness h;
  linked_hierarchy(h);
  static_cast<void>(h.click(80.0F, 45.0F));
  static_cast<void>(h.frame(80.0F, 45.0F, false, gui::KeyMods::None,
                            0.0F, "hUlL"));
  const auto ranged = h.click(100.0F, 184.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection->node == first_part_node);
  UI_CHECK(ranged.selection->visible_order == std::vector<doc::NodeId>{
      collection_node, linked_model_node, first_part_node});

  // Clearing the query restores the user's closed folders.
  static_cast<void>(h.click(80.0F, 45.0F));
  static_cast<void>(h.key(gui::Key::End));
  for (int index = 0; index < 4; ++index) {
    static_cast<void>(h.key(gui::Key::Backspace));
  }
  UI_CHECK(h.panel.search().empty());
  UI_CHECK(h.click(100.0F, 112.0F).selection->node == object_node);
}

void selecting_a_hidden_part_reveals_ancestors_without_forcing_them_open() {
  Harness h;
  linked_hierarchy(h);
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.selection = {first_part_node};
  h.primary = first_part_node;
  UI_CHECK(h.click(100.0F, 208.0F).selection->node == first_part_node);
  static_cast<void>(h.click(42.0F, 88.0F));
  // The unchanged selection must not immediately undo an explicit collapse.
  UI_CHECK(h.click(100.0F, 112.0F).selection->node == object_node);
}

void live_hierarchy_updates_preserve_folds_and_project_reset_clears_them() {
  Harness h;
  linked_hierarchy(h);
  static_cast<void>(h.click(42.0F, 88.0F));
  auto new_part = h.elements.front();
  new_part.node = doc::NodeId{505U};
  new_part.display_name = "Tank / Hull / Part 2";
  h.elements.insert(h.elements.begin(), new_part);
  auto ranged = h.click(100.0F, 184.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection->visible_order == std::vector<doc::NodeId>{
      collection_node, linked_model_node, linked_light_node, object_node});
  static_cast<void>(h.click(58.0F, 136.0F));
  UI_CHECK(h.click(100.0F, 184.0F).selection->node == new_part.node);
  h.panel.reset_project_state();
  UI_CHECK(h.click(100.0F, 112.0F).selection->node == object_node);
}

void authored_groups_keep_sibling_order_and_reorder_only_among_peers() {
  Harness h;
  linked_hierarchy(h);
  for (auto &element : h.elements) {
    element.linked = element.linked_collection = false;
    if (element.linked_parent != doc::NodeId{}) {
      element.parent_group = element.linked_parent;
    }
  }
  h.elements[2U].group_order = 10;
  static_cast<void>(h.click(42.0F, 88.0F));
  static_cast<void>(h.click(58.0F, 136.0F));
  UI_CHECK(h.click(100.0F, 184.0F).selection->node == first_part_node);
  static_cast<void>(h.frame(100.0F, 184.0F, true));
  static_cast<void>(h.frame(100.0F, 214.55F, true));
  const auto reordered = h.frame(100.0F, 214.55F, false);
  UI_CHECK(reordered.layer_reorders == std::vector<editor::TimelineLayerReorder>{
      {first_part_node, second_part_node, editor::TimelineLayerPlacement::After}});
  static_cast<void>(h.frame(100.0F, 184.0F, true));
  static_cast<void>(h.frame(100.0F, 238.55F, true));
  UI_CHECK(h.frame(100.0F, 238.55F, false).layer_reorders.empty());
}

void clear_emits_every_layer_channel() {
  Harness h;
  h.bounds.width = 1200.0F;
  h.selection = {object_node};
  h.primary = object_node;
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  // Clearing a layer's animation is something done to that layer, so it is
  // on the layer's own menu rather than a toolbar button addressing whatever
  // happens to be selected.
  const editor::TimelinePanelFrame cleared = h.choose(80.0F, 88.0F, 1U);
  UI_CHECK(cleared.keys.size() == 10U);
  for (std::size_t index = 0U; index < cleared.keys.size(); ++index) {
    UI_CHECK(cleared.keys[index].target == object_node);
    UI_CHECK(cleared.keys[index].clear);
    UI_CHECK(cleared.keys[index].component ==
             static_cast<scene::TransformChannel>(index));
  }
}

void clear_routes_saturation_to_its_first_class_effect_node() {
  Harness h;
  h.bounds.width = 1200.0F;
  h.selection = {object_node};
  h.primary = object_node;
  h.effects.push_back(scene::EffectInstance{doc::NodeId{77U}, object_node,
                                            scene::EffectKind::Saturation, 0,
                                            true, 1.5F});
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  const editor::TimelinePanelFrame cleared = h.choose(80.0F, 88.0F, 1U);
  UI_CHECK(cleared.keys.size() == 11U);
  const editor::TimelineKeyEdit &saturation = cleared.keys.back();
  UI_CHECK(saturation.target == doc::NodeId{77U});
  UI_CHECK(saturation.component == scene::PropertyChannel::Saturation);
  UI_CHECK(saturation.clear);
}

void spatial_property_rows_key_typed_effect_nodes() {
  Harness h;
  scene::EffectInstance box{};
  box.source = doc::NodeId{77U};
  box.target = object_node;
  box.kind = scene::EffectKind::BoxBlur;
  box.enabled = true;
  box.box_blur = scene::BoxBlurParameters{7U, 2U};
  scene::EffectInstance directional{};
  directional.source = doc::NodeId{78U};
  directional.target = object_node;
  directional.kind = scene::EffectKind::DirectionalBlur;
  directional.enabled = true;
  directional.directional_blur =
      scene::DirectionalBlurParameters{12.0F, 0.125F};
  scene::EffectInstance chroma{};
  chroma.source = doc::NodeId{79U};
  chroma.target = object_node;
  chroma.kind = scene::EffectKind::ChromaKey;
  chroma.enabled = true;
  chroma.chroma_key.mix = 0.75F;
  scene::EffectInstance luma{};
  luma.source = doc::NodeId{80U};
  luma.target = object_node;
  luma.kind = scene::EffectKind::LumaKey;
  luma.enabled = true;
  luma.luma_key.mix = 0.25F;
  h.effects = {box, directional, chroma, luma};

  // Expanded layer rows are P/R/S, Opacity, Box Radius, then Directional
  // Length. Each row's diamond authors the effect node, not its layer.
  static_cast<void>(h.click(42.0F, 88.0F));
  const editor::TimelinePanelFrame box_key = h.click(248.0F, 208.0F);
  UI_CHECK(box_key.keys.size() == 1U);
  UI_CHECK(box_key.keys.front().target == box.source);
  UI_CHECK(box_key.keys.front().component ==
           scene::PropertyChannel::BoxBlurRadius);
  UI_CHECK(box_key.keys.front().value == 7.0);

  const editor::TimelinePanelFrame directional_key = h.click(248.0F, 232.0F);
  UI_CHECK(directional_key.keys.size() == 1U);
  UI_CHECK(directional_key.keys.front().target == directional.source);
  UI_CHECK(directional_key.keys.front().component ==
           scene::PropertyChannel::DirectionalBlurLength);
  UI_CHECK(directional_key.keys.front().value == 12.0);

  const editor::TimelinePanelFrame chroma_key = h.click(248.0F, 256.0F);
  UI_CHECK(chroma_key.keys.size() == 1U);
  UI_CHECK(chroma_key.keys.front().target == chroma.source);
  UI_CHECK(chroma_key.keys.front().component ==
           scene::PropertyChannel::ChromaKeyMix);
  UI_CHECK(chroma_key.keys.front().value == 0.75);

  const editor::TimelinePanelFrame luma_key = h.click(248.0F, 280.0F);
  UI_CHECK(luma_key.keys.size() == 1U);
  UI_CHECK(luma_key.keys.front().target == luma.source);
  UI_CHECK(luma_key.keys.front().component ==
           scene::PropertyChannel::LumaKeyMix);
  UI_CHECK(luma_key.keys.front().value == 0.25);
}

void range_controls_report_loop_and_exact_bounds() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  // Loop sits with the transport at x=144..168.
  const editor::TimelinePanelFrame looped = h.click(156.0F, 17.0F);
  UI_CHECK(looped.range.has_value());
  UI_CHECK(looped.range->first == 0U);
  UI_CHECK(looped.range->last == 100U);
  UI_CHECK(looped.range->loop);

  // IN is a timecode field at x=798..894 that scrubs one frame per pixel.
  static_cast<void>(h.frame(850.0F, 17.0F, false));
  static_cast<void>(h.frame(850.0F, 17.0F, true));
  const editor::TimelinePanelFrame dragged = h.frame(870.0F, 17.0F, true);
  UI_CHECK(dragged.range.has_value());
  UI_CHECK(dragged.range->first == 20U);
  UI_CHECK(dragged.range->last == 100U);
  UI_CHECK(dragged.editing);

  // OUT, at x=898..994, is independently draggable as well.
  Harness out;
  static_cast<void>(out.frame(950.0F, 17.0F, false));
  static_cast<void>(out.frame(950.0F, 17.0F, true));
  const editor::TimelinePanelFrame shortened = out.frame(930.0F, 17.0F, true);
  UI_CHECK(shortened.range.has_value());
  UI_CHECK(shortened.range->first == 0U);
  UI_CHECK(shortened.range->last == 80U);
  UI_CHECK(shortened.editing);
}

void i_and_o_set_the_range_from_the_show_frame_and_preserve_loop() {
  Harness set_in;
  set_in.timeline.first_frame = 20U;
  set_in.timeline.last_frame = 80U;
  set_in.timeline.show_frame = 50U;
  set_in.timeline.loop = true;
  set_in.focus();
  const editor::TimelinePanelFrame new_in = set_in.type("i");
  UI_CHECK(new_in.range == editor::RangeRequest{50U, 80U, true});
  UI_CHECK(!set_in.frame(0.0F, 0.0F, false).range.has_value());

  Harness set_out;
  set_out.timeline.first_frame = 20U;
  set_out.timeline.last_frame = 80U;
  set_out.timeline.show_frame = 50U;
  set_out.timeline.loop = true;
  set_out.focus();
  UI_CHECK(set_out.type("O").range == editor::RangeRequest{20U, 50U, true});

  // Keys belong to the timeline only while it has the operator's attention.
  Harness elsewhere;
  elsewhere.timeline.show_frame = 50U;
  static_cast<void>(elsewhere.click(1'400.0F, 300.0F));
  UI_CHECK(!elsewhere.type("i").range.has_value());
}

void i_and_o_keep_stale_playheads_in_a_valid_range() {
  const auto keyed = [](std::uint64_t show, std::string_view key) {
    Harness h;
    h.timeline.first_frame = 20U;
    h.timeline.last_frame = 80U;
    h.timeline.show_frame = show;
    h.focus();
    return h.type(key).range;
  };
  UI_CHECK(keyed(120U, "i") == editor::RangeRequest{120U, 120U, false});
  UI_CHECK(keyed(10U, "i") == editor::RangeRequest{10U, 80U, false});
  UI_CHECK(keyed(10U, "o") == editor::RangeRequest{10U, 10U, false});
  UI_CHECK(keyed(120U, "o") == editor::RangeRequest{20U, 120U, false});
}

void the_range_handles_on_the_ruler_drag_in_and_out() {
  Harness h;
  h.timeline.first_frame = 20U;
  h.timeline.last_frame = 80U;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  // The view fits 20..80 across 740px, a little over 12.3px per frame, so
  // the Out handle on frame 80 is the graph's last pixel column.
  const float per_frame = 740.0F / 60.0F;
  const float out_x = 999.0F;
  static_cast<void>(h.frame(out_x, 52.0F, false));
  static_cast<void>(h.frame(out_x, 52.0F, true));
  const editor::TimelinePanelFrame dragging =
      h.frame(out_x - 10.0F * per_frame, 52.0F, true, gui::KeyMods::Alt);
  // The bar previews; nothing is authored and nothing scrubs mid-drag.
  UI_CHECK(!dragging.range.has_value());
  UI_CHECK(!dragging.seek_to_frame.has_value());
  UI_CHECK(dragging.editing);
  const editor::TimelinePanelFrame dropped =
      h.frame(out_x - 10.0F * per_frame, 52.0F, false, gui::KeyMods::Alt);
  UI_CHECK(dropped.range == editor::RangeRequest{20U, 70U, false});

  // The In handle sits on the first frame.
  static_cast<void>(h.frame(261.0F, 52.0F, false));
  static_cast<void>(h.frame(261.0F, 52.0F, true));
  static_cast<void>(
      h.frame(261.0F + 5.0F * per_frame, 52.0F, true, gui::KeyMods::Alt));
  UI_CHECK(h.frame(261.0F + 5.0F * per_frame, 52.0F, false, gui::KeyMods::Alt)
               .range == editor::RangeRequest{25U, 80U, false});
}

void show_rate_menu_reports_exact_common_rates() {
  constexpr std::array<editor::ShowRateRequest, 8U> expected{{
      {24'000U, 1'001U},
      {24U, 1U},
      {25U, 1U},
      {30'000U, 1'001U},
      {30U, 1U},
      {50U, 1U},
      {60'000U, 1'001U},
      {60U, 1U},
  }};

  Harness h;
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    // Keep the selected entry different from the current rate so every
    // exact rational is observable as an authored request.
    h.timeline.rate_numerator =
        expected[index].numerator == 50U && expected[index].denominator == 1U
            ? 24U
            : 50U;
    h.timeline.rate_denominator = 1U;

    // The rate button is x=290..360 and its menu drops beneath it.
    const editor::TimelinePanelFrame opened = h.click(325.0F, 17.0F);
    UI_CHECK(!opened.show_rate.has_value());
    UI_CHECK(opened.editing);

    const editor::TimelinePanelFrame selected =
        h.click(325.0F, 46.0F + 21.0F * static_cast<float>(index));
    UI_CHECK(selected.show_rate.has_value());
    UI_CHECK(selected.show_rate.value() == expected[index]);
    UI_CHECK(!selected.seek_to_frame.has_value());
  }

  // Choosing the rate already in force simply dismisses the popup.
  h.timeline.rate_numerator = 24U;
  h.timeline.rate_denominator = 1U;
  static_cast<void>(h.click(325.0F, 17.0F));
  const editor::TimelinePanelFrame unchanged = h.click(325.0F, 67.0F);
  UI_CHECK(!unchanged.show_rate.has_value());
}

void a_key_drag_retimes_once_on_release() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Position X is y=100..122. Frame 10 is 74px into the fitted graph:
  // x=260 + 10*7.4 = 334. Dragging 37px lands exactly at frame 15.
  static_cast<void>(h.frame(334.0F, 136.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(334.0F, 136.0F, true);
  UI_CHECK(pressed.key_moves.empty());

  const editor::TimelinePanelFrame held = h.frame(371.0F, 136.0F, true);
  UI_CHECK(held.key_moves.empty());

  const editor::TimelinePanelFrame released = h.frame(371.0F, 136.0F, false);
  UI_CHECK(released.key_moves.empty());
  UI_CHECK(released.key_batch.has_value());
  UI_CHECK(released.key_batch->action == editor::TimelineKeyBatchAction::Move);
  UI_CHECK(released.key_batch->delta == 5);
  UI_CHECK(released.key_batch->keys ==
           std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10}});

  const editor::TimelinePanelFrame after = h.frame(371.0F, 136.0F, false);
  UI_CHECK(after.key_moves.empty());
  UI_CHECK(!after.key_batch.has_value());
}

void alt_drag_duplicates_the_complete_key_selection() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  static_cast<void>(h.click(334.0F, 136.0F));
  static_cast<void>(h.click(408.0F, 136.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);

  static_cast<void>(h.frame(334.0F, 136.0F, false, gui::KeyMods::Alt));
  static_cast<void>(h.frame(334.0F, 136.0F, true, gui::KeyMods::Alt));
  static_cast<void>(h.frame(371.0F, 136.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame duplicated =
      h.frame(371.0F, 136.0F, false, gui::KeyMods::Alt);

  UI_CHECK(duplicated.key_moves.empty());
  UI_CHECK(!duplicated.key_duplicate.has_value());
  UI_CHECK(duplicated.key_batch.has_value());
  UI_CHECK(duplicated.key_batch->action ==
           editor::TimelineKeyBatchAction::Duplicate);
  UI_CHECK(duplicated.key_batch->delta == 5);
  UI_CHECK(duplicated.key_batch->keys.size() == 2U);
  UI_CHECK(duplicated.key_batch->keys[0] ==
           editor::TimelineKeyAddress{object_node,
                                      scene::PropertyChannel::PositionX, 10});
  UI_CHECK(duplicated.key_batch->keys[1] ==
           editor::TimelineKeyAddress{object_node,
                                      scene::PropertyChannel::PositionX, 20});
  UI_CHECK(h.panel.selected_key_count() == 2U);
}

void focused_key_selection_consumes_copy_and_paste_shortcuts() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));

  const editor::TimelinePanelFrame copied = h.key(gui::Key::Copy);
  UI_CHECK(copied.consume_clipboard);
  UI_CHECK(copied.key_clipboard.has_value());
  UI_CHECK(copied.key_clipboard->action ==
           editor::TimelineKeyClipboardAction::Copy);
  UI_CHECK(copied.key_clipboard->keys.size() == 1U);
  UI_CHECK(copied.key_clipboard->keys.front() ==
           editor::TimelineKeyAddress{object_node,
                                      scene::PropertyChannel::PositionX, 10});

  const editor::TimelinePanelFrame pasted = h.key(gui::Key::Paste);
  UI_CHECK(pasted.consume_clipboard);
  UI_CHECK(pasted.key_clipboard.has_value());
  UI_CHECK(pasted.key_clipboard->action ==
           editor::TimelineKeyClipboardAction::Paste);
  UI_CHECK(pasted.key_clipboard->keys.empty());
}

void selecting_a_clip_exits_the_key_clipboard_domain() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);

  // The media bar owns the same Timeline workspace but a different semantic
  // clipboard. Clicking it clears stale diamond membership before the shell
  // routes the next global chord to clip Copy/Paste.
  static_cast<void>(h.click(334.0F, 256.0F));
  UI_CHECK(h.panel.selected_key_count() == 0U);
  const editor::TimelinePanelFrame copied = h.key(gui::Key::Copy);
  UI_CHECK(!copied.consume_clipboard);
  UI_CHECK(!copied.key_clipboard.has_value());
}

void modifier_clicks_build_and_trim_a_key_selection() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  static_cast<void>(h.click(334.0F, 136.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  static_cast<void>(h.click(408.0F, 136.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);
  static_cast<void>(h.click(334.0F, 136.0F, gui::KeyMods::Ctrl));
  UI_CHECK(h.panel.selected_key_count() == 1U);
}

void property_diamonds_key_groups_and_individual_channels() {
  Harness group;
  static_cast<void>(group.frame(0.0F, 0.0F, false));
  group.open_position_components();

  // The row-local diamond is at the right edge of the layer column.
  const editor::TimelinePanelFrame group_key = group.click(248.0F, 112.0F);
  UI_CHECK(group_key.keys.size() == 3U);
  UI_CHECK(group_key.keys[0].component == scene::TransformChannel::PositionX);
  UI_CHECK(group_key.keys[0].frame == group.timeline.show_frame);
  UI_CHECK(group_key.keys[0].value == 1.0);
  UI_CHECK(group_key.keys[1].component == scene::TransformChannel::PositionY);
  UI_CHECK(group_key.keys[2].component == scene::TransformChannel::PositionZ);
  UI_CHECK(!group_key.keys[0].erase);
  UI_CHECK(!group_key.keys[1].erase);
  UI_CHECK(!group_key.keys[2].erase);

  Harness component;
  static_cast<void>(component.frame(0.0F, 0.0F, false));
  component.open_position_components();
  const editor::TimelinePanelFrame component_key =
      component.click(248.0F, 136.0F);
  UI_CHECK(component_key.keys.size() == 1U);
  UI_CHECK(component_key.keys[0].component ==
           scene::TransformChannel::PositionX);
  UI_CHECK(component_key.keys[0].value == 1.0);
  UI_CHECK(component_key.keys[0].erase);
}

void environment_rows_key_scene_values_and_preserve_animation() {
  Harness h;
  editor::ElementInfo environment{};
  environment.node = doc::NodeId{1201U};
  environment.kind = editor::ElementKind::Environment;
  environment.display_name = "Environment";
  environment.environment.physical_sky.time_of_day = 14.0F;
  environment.environment.fog_density = 0.25F;
  h.elements = {environment};
  h.keys = {{environment.node, scene::PropertyChannel::FogDensity,
             {0, 100}, {0.0, 0.5}}};
  static_cast<void>(h.click(42.0F, 88.0F));
  for (std::size_t index = 0U; index < 9U; ++index) {
    const auto property = static_cast<scene::PropertyChannel>(
        static_cast<std::size_t>(scene::PropertyChannel::SkyTimeOfDay) + index);
    const auto keyed = h.click(248.0F, 112.0F + static_cast<float>(index) * 24.0F);
    UI_CHECK(keyed.keys.size() == 1U);
    UI_CHECK(keyed.keys.front().target == environment.node);
    UI_CHECK(keyed.keys.front().component == property);
    UI_CHECK(keyed.keys.front().frame == h.timeline.show_frame);
    UI_CHECK(keyed.keys.front().value ==
             scene::environment_channel_value(environment.environment, property));
    UI_CHECK(!keyed.keys.front().erase);
  }

  // A key inserted between existing keys uses the interpolated value, even
  // though the scene's authored fallback remains unchanged.
  h.timeline.show_frame = 25U;
  const auto interpolated = h.click(248.0F, 184.0F);
  UI_CHECK(interpolated.keys.size() == 1U);
  UI_CHECK(interpolated.keys.front().component == scene::PropertyChannel::FogDensity);
  UI_CHECK(interpolated.keys.front().value == 0.125);
  h.timeline.show_frame = 100U;
  UI_CHECK(h.click(248.0F, 184.0F).keys.front().erase);
}

void a_marquee_selects_keys_and_the_group_retimes_together() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Drag a box around Position X's frame-10 and frame-20 diamonds.
  static_cast<void>(h.frame(320.0F, 129.45F, false));
  static_cast<void>(h.frame(320.0F, 129.45F, true));
  static_cast<void>(h.frame(415.0F, 141.45F, true));
  static_cast<void>(h.frame(415.0F, 141.45F, false));
  UI_CHECK(h.panel.selected_key_count() == 2U);

  // Grabbing either selected diamond moves the complete selection by five.
  static_cast<void>(h.frame(334.0F, 136.0F, false));
  static_cast<void>(h.frame(334.0F, 136.0F, true));
  static_cast<void>(h.frame(371.0F, 136.0F, true));
  const editor::TimelinePanelFrame moved = h.frame(371.0F, 136.0F, false);
  UI_CHECK(moved.key_moves.empty());
  UI_CHECK(moved.key_batch.has_value());
  UI_CHECK(moved.key_batch->action == editor::TimelineKeyBatchAction::Move);
  UI_CHECK(moved.key_batch->delta == 5);
  UI_CHECK(moved.key_batch->keys.size() == 2U);
  UI_CHECK(moved.key_batch->keys[0].frame == 10);
  UI_CHECK(moved.key_batch->keys[1].frame == 20);
}

void key_drags_snap_to_nearby_channel_keys() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Pointer frame 24 is within eight pixels of Position Y's frame-25 key.
  static_cast<void>(h.frame(334.0F, 136.0F, false));
  static_cast<void>(h.frame(334.0F, 136.0F, true));
  static_cast<void>(h.frame(440.0F, 136.0F, true));
  const editor::TimelinePanelFrame snapped = h.frame(440.0F, 136.0F, false);
  UI_CHECK(snapped.key_moves.empty());
  UI_CHECK(snapped.key_batch.has_value());
  UI_CHECK(snapped.key_batch->action == editor::TimelineKeyBatchAction::Move);
  UI_CHECK(snapped.key_batch->delta == 15);
  UI_CHECK(snapped.key_batch->keys.size() == 1U);
  UI_CHECK(snapped.key_batch->keys[0].frame == 10);
}

void delete_erases_selected_keys_not_the_whole_channel() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));

  const editor::TimelinePanelFrame erased = h.key(gui::Key::Delete);
  UI_CHECK(erased.consume_delete);
  UI_CHECK(erased.keys.empty());
  UI_CHECK(erased.key_batch.has_value());
  UI_CHECK(erased.key_batch->action == editor::TimelineKeyBatchAction::Delete);
  UI_CHECK(erased.key_batch->keys ==
           std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10}});
  // Selection changes only after the Director accepts the atomic plan.
  UI_CHECK(h.panel.selected_key_count() == 1U);
}

void lasso_selection_preserves_add_and_subtract_modifiers() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Key tools open from the toolbar's Keys button. Switch the marquee from
  // a box to a freeform lasso, then encircle Position X frames 10 and 20.
  static_cast<void>(h.click(659.0F, 17.0F));
  static_cast<void>(h.click(668.0F, 49.0F));
  UI_CHECK(h.panel.lasso_mode());

  const auto lasso = [&](gui::KeyMods mods) {
    static_cast<void>(h.frame(320.0F, 127.27F, false, mods));
    static_cast<void>(h.frame(320.0F, 127.27F, true, mods));
    static_cast<void>(h.frame(420.0F, 127.27F, true, mods));
    static_cast<void>(h.frame(420.0F, 144.73F, true, mods));
    static_cast<void>(h.frame(320.0F, 144.73F, true, mods));
    static_cast<void>(h.frame(320.0F, 127.27F, false, mods));
  };
  lasso(gui::KeyMods::None);
  UI_CHECK(h.panel.selected_key_count() == 2U);

  // Ctrl is subtract/toggle for both box and lasso selection.
  lasso(gui::KeyMods::Ctrl);
  UI_CHECK(h.panel.selected_key_count() == 0U);
}

void scale_requires_a_matching_preview_and_emits_one_atomic_batch() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));
  static_cast<void>(h.click(408.0F, 136.0F, gui::KeyMods::Shift));

  static_cast<void>(h.click(659.0F, 17.0F));
  const editor::TimelinePanelFrame opened = h.click(673.0F, 107.64F);
  UI_CHECK(h.panel.scale_open());
  UI_CHECK(opened.key_scale_preview.has_value());
  UI_CHECK(opened.key_scale_preview->keys.size() == 2U);
  UI_CHECK(opened.key_scale_preview->scale.numerator == 1);
  UI_CHECK(opened.key_scale_preview->scale.denominator == 1);
  UI_CHECK(opened.key_scale_preview->scale.anchor ==
           scene::KeyTimeAnchor::FirstSelected);

  // A response for anything except the current exact selection/settings is
  // stale and cannot enable Apply.
  h.key_scale_preview = editor::TimelineKeyScalePreview{
      opened.key_scale_preview->signature + 1U, true, 2U, 2U, 0U, 0U};
  const editor::TimelinePanelFrame stale = h.click(936.0F, 199.27F);
  UI_CHECK(!stale.key_batch.has_value());

  const editor::TimelinePanelFrame playhead_anchor = h.click(846.0F, 138.18F);
  UI_CHECK(playhead_anchor.key_scale_preview.has_value());
  UI_CHECK(playhead_anchor.key_scale_preview->scale.anchor ==
           scene::KeyTimeAnchor::Playhead);
  UI_CHECK(playhead_anchor.key_scale_preview->scale.playhead == 50);
  h.key_scale_preview = editor::TimelineKeyScalePreview{
      playhead_anchor.key_scale_preview->signature, true, 2U, 2U, 1U, 1U};

  const editor::TimelinePanelFrame applied = h.click(936.0F, 199.27F);
  UI_CHECK(applied.key_batch.has_value());
  UI_CHECK(applied.key_batch->action == editor::TimelineKeyBatchAction::Scale);
  UI_CHECK(applied.key_batch->keys == playhead_anchor.key_scale_preview->keys);
  UI_CHECK(applied.key_batch->scale ==
           playhead_anchor.key_scale_preview->scale);
  UI_CHECK(!h.panel.scale_open());
}

void curve_tools_fit_navigate_hide_and_author_the_active_channel() {
  Harness h;
  h.keys[0].values = {1.0, 2.0, 3.0};
  h.keys[0].interpolations = {scene::KeyInterpolation::Linear,
                              scene::KeyInterpolation::Cubic,
                              scene::KeyInterpolation::Hold};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  // Curves is the text toggle at x=582..634. Entering it fits the one
  // plottable channel, Position X with values 1..3, to [0.8, 3.2] over the
  // full rows area, y=76..400: value v draws at y = 76 + 135 * (3.2 - v).
  // Its keys at frames 10, 20 and 50 are at (334, 373), (408, 238) and
  // (630, 103).
  static_cast<void>(h.click(608.0F, 17.0F));
  UI_CHECK(h.panel.curves_mode());
  static_cast<void>(h.click(334.0F, 373.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  static_cast<void>(h.click(408.0F, 238.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);
  static_cast<void>(h.click(659.0F, 17.0F));

  // Alt/Ctrl wheel address the value axis without changing row scroll. Alt
  // also holds the value under the pointer still, so zooming away from the
  // middle of the graph shifts the pan.
  static_cast<void>(h.frame(500.0F, 238.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_zoom() > 1.0F);
  const float anchored_pan = h.panel.curve_value_pan();
  static_cast<void>(h.frame(500.0F, 330.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_pan() != anchored_pan);
  const float before_ctrl = h.panel.curve_value_pan();
  static_cast<void>(h.frame(500.0F, 238.0F, false, gui::KeyMods::Ctrl, 1.0F));
  UI_CHECK(h.panel.curve_value_pan() > before_ctrl);

  // FIT ALL is the only view button the popup still carries: every zoom step
  // it used to hold is a wheel gesture over the graph.
  static_cast<void>(h.click(797.0F, 49.0F));
  UI_CHECK(h.panel.curve_value_zoom() == 1.0F);
  UI_CHECK(h.panel.curve_value_pan() == 0.0F);
  static_cast<void>(h.frame(500.0F, 136.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_zoom() > 1.0F);

  static_cast<void>(h.click(727.0F, 49.0F));
  const editor::TimelinePanelFrame fitted = h.click(260.0F, 45.0F);
  UI_CHECK(fitted.seek_to_frame == 9U);
  UI_CHECK(h.panel.curve_value_zoom() == 1.0F);
  UI_CHECK(h.panel.curve_value_pan() == 0.0F);

  // The per-row swatch is a true visibility toggle, not a destructive key
  // edit. Showing it again restores hit testing and its stable channel color.
  static_cast<void>(h.click(225.0F, 136.0F));
  UI_CHECK(
      !h.panel.channel_visible(object_node, scene::PropertyChannel::PositionX));
  static_cast<void>(h.click(225.0F, 136.0F));
  UI_CHECK(
      h.panel.channel_visible(object_node, scene::PropertyChannel::PositionX));

  // Re-selecting a key makes the channel active. The numeric field samples at
  // the playhead and an absolute typed commit authors that exact property at
  // the playhead; target/property are never inferred from another row.
  // FIT SEL showed frames 9..21 and values [0.9, 2.1]: the frame-10 key,
  // value 1, sits at x=321.7, y=373.
  static_cast<void>(h.click(322.0F, 373.0F));
  static_cast<void>(h.click(659.0F, 17.0F));
  static_cast<void>(h.frame(918.0F, 77.09F, false));
  static_cast<void>(h.frame(918.0F, 77.09F, true));
  static_cast<void>(h.frame(918.0F, 77.09F, false));
  static_cast<void>(
      h.frame(0.0F, 0.0F, false, gui::KeyMods::None, 0.0F, "4.5"));
  const editor::TimelinePanelFrame committed = h.frame(673.0F, 107.64F, true);
  UI_CHECK(committed.keys.size() == 1U);
  UI_CHECK(committed.keys.front().target == object_node);
  UI_CHECK(committed.keys.front().component ==
           scene::PropertyChannel::PositionX);
  UI_CHECK(committed.keys.front().frame == h.timeline.show_frame);
  UI_CHECK(std::fabs(committed.keys.front().value - 4.5) < 1.0e-9);
}

void middle_mouse_pans_time_without_seeking() {
  Harness h;
  static_cast<void>(h.frame(500.0F, 233.09F, false));
  static_cast<void>(h.middle(500.0F, 233.09F, false));
  static_cast<void>(h.middle(500.0F, 233.09F, true));
  const editor::TimelinePanelFrame panned = h.middle(400.0F, 233.09F, true);
  UI_CHECK(!panned.seek_to_frame.has_value());
  static_cast<void>(h.middle(400.0F, 233.09F, false));

  // The same ruler x was frame 50 before the 100px leftward pan; it now
  // addresses roughly frame 64.
  const editor::TimelinePanelFrame scrubbed = h.click(630.0F, 45.0F);
  UI_CHECK(scrubbed.seek_to_frame.has_value());
  UI_CHECK(scrubbed.seek_to_frame.value() == 64U);
}

void clicking_an_empty_lane_clears_layers_and_scrubs() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Frame 40 has no Position X key.
  const editor::TimelinePanelFrame pressed = h.frame(556.0F, 136.0F, true);
  UI_CHECK(pressed.key_moves.empty());
  UI_CHECK(pressed.seek_to_frame.has_value());
  UI_CHECK(pressed.seek_to_frame.value() == 40U);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(pressed.selection->action == editor::SelectionAction::Clear);
}

void a_project_replacement_clears_stale_keys_and_refits_the_new_range() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);

  // Deliberately zoom the old 0..100 project and preserve a user preference.
  static_cast<void>(h.frame(630.0F, 233.09F, false, gui::KeyMods::None, 4.0F));
  // Snap is the magnet at x=530..554.
  static_cast<void>(h.click(542.0F, 17.0F));
  UI_CHECK(!h.panel.snapping_enabled());

  h.timeline = editor::EditorTimeline{1'500U, 1'500U, 1'000U, 2'000U,
                                      false,  false,  false};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  const editor::TimelinePanelFrame still_zoomed = h.click(630.0F, 45.0F);
  UI_CHECK(still_zoomed.seek_to_frame.has_value());
  UI_CHECK(still_zoomed.seek_to_frame.value() != 1'500U);

  // EditorShell calls this only when EditorProjectState::generation changes,
  // which happens after a replacement has successfully committed.
  h.panel.reset_project_state();
  UI_CHECK(h.panel.selected_key_count() == 0U);
  UI_CHECK(!h.panel.snapping_enabled());
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  const editor::TimelinePanelFrame fitted = h.click(630.0F, 45.0F);
  UI_CHECK(fitted.seek_to_frame.has_value());
  UI_CHECK(fitted.seek_to_frame.value() == 1'500U);
}

void a_scene_switch_clears_only_the_typed_key_context() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  static_cast<void>(h.click(334.0F, 136.0F));
  static_cast<void>(h.click(659.0F, 17.0F));
  static_cast<void>(h.click(673.0F, 107.64F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  UI_CHECK(h.panel.scale_open());

  h.panel.clear_key_context();
  UI_CHECK(h.panel.selected_key_count() == 0U);
  UI_CHECK(!h.panel.scale_open());
  UI_CHECK(
      h.panel.channel_visible(object_node, scene::PropertyChannel::PositionX));
  // View preferences survive a Stage scene switch.
  UI_CHECK(h.panel.snapping_enabled());
}

void a_panel_with_no_room_reports_nothing() {
  Harness h;
  h.input.new_frame();
  h.ctx.new_frame();
  const editor::TimelinePanelFrame frame =
      h.panel.draw(h.ctx, gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, h.timeline, h.keys,
                   h.elements, h.selection, h.primary);
  UI_CHECK(!frame.seek_to_frame.has_value());
  UI_CHECK(frame.keys.empty());
  UI_CHECK(frame.key_moves.empty());
  UI_CHECK(!frame.selection.has_value());
}

void dragging_a_clip_body_moves_the_placement() {
  Harness h;
  static_cast<void>(h.frame(500.0F, 113.09F, false));
  // The press claims the bar rather than scrubbing the playhead.
  const editor::TimelinePanelFrame pressed = h.frame(500.0F, 113.09F, true);
  UI_CHECK(!pressed.seek_to_frame.has_value());
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(pressed.selection->node == clip_node);
  UI_CHECK(h.panel.dragging_clip());

  const editor::TimelinePanelFrame moved = h.frame(574.0F, 113.09F, true);
  UI_CHECK(moved.clip_edits.empty());
  UI_CHECK(!moved.seek_to_frame.has_value());

  // The lifted bar previews locally. One placement edit is authored when it is
  // dropped, matching the Feed Mapping gesture and making one drag one edit.
  const editor::TimelinePanelFrame released = h.frame(574.0F, 113.09F, false);
  UI_CHECK(released.clip_edits.size() == 1U);
  UI_CHECK(released.clip_edits.front().clip == clip_node);
  UI_CHECK(released.clip_edits.front().timing ==
           (scene::ClipTiming{30, 40, 0, 0, 0}));
  UI_CHECK(!h.panel.dragging_clip());

  Harness click;
  static_cast<void>(click.frame(500.0F, 113.09F, false));
  static_cast<void>(click.frame(500.0F, 113.09F, true));
  const editor::TimelinePanelFrame below_threshold =
      click.frame(502.0F, 113.09F, true);
  UI_CHECK(below_threshold.clip_edits.empty());
  const editor::TimelinePanelFrame click_release =
      click.frame(502.0F, 113.09F, false);
  UI_CHECK(click_release.clip_edits.empty());
}

void dragging_a_clip_edge_trims_without_moving_the_rest() {
  Harness h;
  static_cast<void>(h.frame(410.0F, 113.09F, false));
  static_cast<void>(h.frame(410.0F, 113.09F, true));
  const editor::TimelinePanelFrame trimmed = h.frame(447.0F, 113.09F, true);
  UI_CHECK(trimmed.clip_edits.empty());
  const editor::TimelinePanelFrame trim_drop = h.frame(447.0F, 113.09F, false);
  // The out point is untouched and the source slides with the in point, so
  // the frames that survive still show what they showed.
  UI_CHECK(trim_drop.clip_edits.size() == 1U);
  UI_CHECK(trim_drop.clip_edits.front().timing ==
           (scene::ClipTiming{25, 35, 5, 0, 0}));

  h.elements[1].timing = trim_drop.clip_edits.front().timing;
  // The other end changes only the duration.
  static_cast<void>(h.frame(700.0F, 113.09F, false));
  static_cast<void>(h.frame(700.0F, 113.09F, true));
  const editor::TimelinePanelFrame shortened = h.frame(663.0F, 113.09F, true);
  UI_CHECK(shortened.clip_edits.empty());
  const editor::TimelinePanelFrame shorten_drop =
      h.frame(663.0F, 113.09F, false);
  UI_CHECK(shorten_drop.clip_edits.size() == 1U);
  UI_CHECK(shorten_drop.clip_edits.front().timing ==
           (scene::ClipTiming{25, 30, 5, 0, 0}));
}

void a_clip_fade_handle_sets_the_head_ramp() {
  Harness h;
  static_cast<void>(h.frame(409.0F, 105.45F, false));
  static_cast<void>(h.frame(409.0F, 105.45F, true));
  const editor::TimelinePanelFrame faded = h.frame(446.0F, 105.45F, true);
  UI_CHECK(faded.clip_edits.empty());
  const editor::TimelinePanelFrame fade_drop = h.frame(446.0F, 105.45F, false);
  UI_CHECK(fade_drop.clip_edits.size() == 1U);
  UI_CHECK(fade_drop.clip_edits.front().timing ==
           (scene::ClipTiming{20, 40, 0, 5, 0}));
}

void fade_handles_accept_the_visible_outer_knobs() {
  Harness h;
  // The head knob extends left of the clip body (x=408). Its outer half
  // must be as interactive as the half overlapping the fill.
  static_cast<void>(h.frame(404.0F, 105.0F, false));
  const auto press = h.frame(404.0F, 105.0F, true);
  UI_CHECK(!press.seek_to_frame.has_value());
  UI_CHECK(h.panel.dragging_clip());
  const auto drop = h.frame(441.0F, 105.0F, false);
  UI_CHECK(drop.clip_edits.size() == 1U);
  UI_CHECK(drop.clip_edits.front().timing.fade_in == 5);
  UI_CHECK(drop.clip_edits.front().timing.start == 20);
}

void clip_release_uses_the_final_pointer_and_waits_for_authority() {
  Harness h;
  static_cast<void>(h.frame(500.0F, 113.0F, false));
  static_cast<void>(h.frame(500.0F, 113.0F, true));
  // No intervening held frame: the release carries the final motion.
  const auto drop = h.frame(870.0F, 113.0F, false, gui::KeyMods::Alt);
  UI_CHECK(drop.clip_edits.size() == 1U);
  UI_CHECK(drop.clip_edits.front().timing.start == 70);
  UI_CHECK(!h.panel.dragging_clip());
  // Authority still reports the old timing. The visible pending bar can
  // already be picked at its destination, beyond the old bar's tail.
  static_cast<void>(h.frame(950.0F, 113.0F, false));
  const auto picked = h.frame(950.0F, 113.0F, true);
  UI_CHECK(h.panel.dragging_clip());
  UI_CHECK(picked.selection.has_value());
  UI_CHECK(picked.selection->node == clip_node);
}

void batched_clip_gestures_keep_the_original_press_target_and_anchor() {
  for (const bool release_in_batch : {false, true}) {
    for (const float end_y : {113.0F, 150.0F}) {
      Harness h;
      static_cast<void>(h.frame(900.0F, 300.0F, false));
      // Native polling drains down, motion and sometimes up before drawing.
      // The endpoint is beyond the clip, and can be beyond its row too.
      h.input.new_frame();
      h.input.set_mouse_position(500.0F, 113.0F);
      h.input.set_mouse_button(gui::MouseButton::Left, true);
      h.input.set_mouse_position(870.0F, end_y);
      if (release_in_batch) {
        h.input.set_mouse_button(gui::MouseButton::Left, false);
      }
      h.input.set_mods(gui::KeyMods::Alt);
      h.ctx.new_frame();
      const auto captured = h.draw();
      UI_CHECK(captured.selection.has_value());
      UI_CHECK(captured.selection->node == clip_node);
      UI_CHECK(captured.selection->action != editor::SelectionAction::Clear);
      UI_CHECK(!captured.seek_to_frame);
      UI_CHECK(h.panel.dragging_clip());

      // The next draw resolves even a complete between-refreshes gesture.
      const auto dropped = h.frame(870.0F, end_y, false, gui::KeyMods::Alt);
      UI_CHECK(dropped.clip_edits.size() == 1U);
      UI_CHECK(dropped.clip_edits.front().clip == clip_node);
      UI_CHECK(dropped.clip_edits.front().timing ==
               (scene::ClipTiming{70, 40, 0, 0, 0}));
      UI_CHECK(!dropped.seek_to_frame);
      UI_CHECK(!h.panel.dragging_clip());
      UI_CHECK(h.frame(870.0F, end_y, false).clip_edits.empty());
    }
  }
}

void clip_and_key_releases_ignore_later_uncaptured_motion() {
  for (const bool press_in_batch : {false, true}) {
    Harness clip;
    static_cast<void>(clip.frame(500.0F, 113.0F, false));
    if (!press_in_batch) {
      static_cast<void>(clip.frame(500.0F, 113.0F, true));
    }
    clip.input.new_frame();
    if (press_in_batch) {
      clip.input.set_mouse_button(gui::MouseButton::Left, true);
    }
    clip.input.set_mouse_position(574.0F, 113.0F);
    clip.input.set_mouse_button(gui::MouseButton::Left, false);
    clip.input.set_mouse_position(950.0F, 300.0F);
    clip.input.set_mods(gui::KeyMods::Alt);
    clip.ctx.new_frame();
    auto dropped = clip.draw();
    UI_CHECK(!dropped.seek_to_frame);
    if (press_in_batch) {
      // A new gesture resolves before drawing the next frame's clip bars.
      dropped = clip.frame(960.0F, 310.0F, false, gui::KeyMods::Alt);
    }
    UI_CHECK(dropped.clip_edits.size() == 1U);
    UI_CHECK(dropped.clip_edits.front().timing ==
             (scene::ClipTiming{30, 40, 0, 0, 0}));
    UI_CHECK(!dropped.seek_to_frame);

    Harness key;
    static_cast<void>(key.frame(0.0F, 0.0F, false));
    key.open_position_components();
    static_cast<void>(key.frame(334.0F, 136.0F, false));
    if (!press_in_batch) {
      static_cast<void>(key.frame(334.0F, 136.0F, true));
    }
    key.input.new_frame();
    if (press_in_batch) {
      key.input.set_mouse_button(gui::MouseButton::Left, true);
    }
    key.input.set_mouse_position(371.0F, 136.0F);
    key.input.set_mouse_button(gui::MouseButton::Left, false);
    key.input.set_mouse_position(630.0F, 300.0F);
    key.ctx.new_frame();
    const auto retimed = key.draw();
    UI_CHECK(retimed.key_batch.has_value());
    UI_CHECK(retimed.key_batch->action == editor::TimelineKeyBatchAction::Move);
    UI_CHECK(retimed.key_batch->delta == 5);
    UI_CHECK(retimed.key_batch->keys ==
             std::vector<editor::TimelineKeyAddress>{
                 {object_node, scene::PropertyChannel::PositionX, 10}});
  }
}

void clips_can_move_before_in_and_the_workspace_pans_past_out() {
  Harness h;
  static_cast<void>(h.frame(500.0F, 113.0F, false));
  h.timeline.first_frame = 15U;
  static_cast<void>(h.frame(500.0F, 113.0F, true));
  const auto drop = h.frame(389.0F, 113.0F, false, gui::KeyMods::Alt);
  UI_CHECK(drop.clip_edits.size() == 1U);
  UI_CHECK(drop.clip_edits.front().timing.start == 5);

  Harness beyond;
  static_cast<void>(beyond.frame(0.0F, 0.0F, false));
  beyond.elements[1].timing.start = 150;
  static_cast<void>(beyond.middle(800.0F, 280.0F, true));
  static_cast<void>(beyond.middle(-310.0F, 280.0F, true));
  static_cast<void>(beyond.middle(-310.0F, 280.0F, false));
  // After a 150-frame pan, the parked clip starts at the graph's left edge.
  static_cast<void>(beyond.frame(400.0F, 113.0F, true));
  UI_CHECK(beyond.panel.dragging_clip());
}

void clips_move_across_zero_and_past_out_without_trimming_the_source() {
  for (const auto [end_x, expected_start] :
       std::array<std::pair<float, mediaserver::core::i64>, 2>{{
           {278.0F, -10}, {1240.0F, 120}}}) {
    Harness h;
    h.elements[1].timing.source_in = 17;
    static_cast<void>(h.frame(500.0F, 113.0F, false));
    static_cast<void>(h.frame(500.0F, 113.0F, true));
    const auto dropped = h.frame(end_x, 113.0F, false, gui::KeyMods::Alt);
    UI_CHECK(dropped.clip_edits.size() == 1U);
    UI_CHECK(dropped.clip_edits.front().timing ==
             (scene::ClipTiming{expected_start, 40, 17, 0, 0}));
    UI_CHECK(!dropped.seek_to_frame);
    UI_CHECK(dropped.clip_status.find(std::to_string(expected_start)) !=
             std::string::npos);
  }
}

void negative_workspace_supports_clip_picking_and_signed_razor_cuts() {
  Harness h;
  h.elements[1].timing.start = -20;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  static_cast<void>(h.middle(500.0F, 280.0F, true));
  static_cast<void>(h.middle(722.0F, 280.0F, true));
  static_cast<void>(h.middle(722.0F, 280.0F, false));
  // The view now starts at -30. Frame -10 is x=408; transport stays >=0.
  const auto seek = h.frame(408.0F, 45.0F, true);
  UI_CHECK(seek.seek_to_frame.has_value());
  UI_CHECK(*seek.seek_to_frame == 0U);
  static_cast<void>(h.frame(408.0F, 113.0F, false));
  const auto picked = h.frame(408.0F, 113.0F, true);
  UI_CHECK(picked.selection.has_value());
  UI_CHECK(picked.selection->node == clip_node);
  static_cast<void>(h.frame(408.0F, 113.0F, false));
  static_cast<void>(h.click(510.0F, 17.0F));
  const auto cut = h.frame(408.0F, 113.0F, true, gui::KeyMods::Alt);
  UI_CHECK(cut.clip_splits.size() == 1U);
  UI_CHECK(cut.clip_splits.front().frame == -10);
  UI_CHECK(!cut.seek_to_frame);
}

void head_trim_converts_source_rate_and_wraps_before_zero() {
  for (const auto [end_x, start, duration, source] :
       std::array<std::array<mediaserver::core::i64, 4>, 2>{{
           {447, 25, 35, 2}, {188, -10, 70, 36}}}) {
    Harness h;
    h.timeline.rate_numerator = 60U;
    h.timeline.rate_denominator = 1U;
    h.elements[1].sequence_count = 48U;
    h.elements[1].source_frame_rate = scene::MediaFrameRate{24U, 1U};
    static_cast<void>(h.frame(410.0F, 113.0F, false));
    static_cast<void>(h.frame(410.0F, 113.0F, true));
    const auto dropped = h.frame(static_cast<float>(end_x), 113.0F, false,
                                 gui::KeyMods::Alt);
    UI_CHECK(dropped.clip_edits.size() == 1U);
    UI_CHECK(dropped.clip_edits.front().timing ==
             (scene::ClipTiming{start, duration, source, 0, 0}));
  }
  Harness fractional;
  fractional.timeline.rate_numerator = 60U;
  fractional.elements[1].source_frame_rate = scene::MediaFrameRate{24U, 1U};
  static_cast<void>(fractional.frame(410.0F, 113.0F, false));
  static_cast<void>(fractional.frame(410.0F, 113.0F, true));
  // Two show frames are 0.8 source frames: use the first retained playback
  // frame (floor 0), rather than gesture rounding (nearest 1).
  const auto dropped = fractional.frame(424.8F, 113.0F, false,
                                        gui::KeyMods::Alt);
  UI_CHECK(dropped.clip_edits.size() == 1U);
  UI_CHECK(dropped.clip_edits.front().timing ==
           (scene::ClipTiming{22, 38, 0, 0, 0}));
}

void slip_wraps_looping_sources_and_keeps_feedback_after_release() {
  for (const auto [end_x, delta, source, wrapped] :
       std::array<std::array<mediaserver::core::i64, 4>, 3>{{
           {537, 5, 5, 0}, {463, -5, 5, 1}, {611, 15, 5, 1}}}) {
    Harness h;
    h.elements[1].sequence_count = 10U; // The placement loops four times.
    static_cast<void>(h.click(458.0F, 17.0F));
    static_cast<void>(h.frame(500.0F, 113.0F, true));
    const auto preview = h.frame(static_cast<float>(end_x), 113.0F, true);
    UI_CHECK(preview.timeline_edits.empty());
    UI_CHECK(!preview.clip_status_error);
    UI_CHECK(preview.clip_status.find("Source " + std::to_string(source) +
                                     " of 10 frames") != std::string::npos);
    UI_CHECK((preview.clip_status.find("wrapped") != std::string::npos) ==
             (wrapped != 0));
    const auto dropped = h.frame(static_cast<float>(end_x), 113.0F, false);
    UI_CHECK(dropped.clip_edits.empty());
    UI_CHECK(dropped.timeline_edits.size() == 1U);
    const auto *request = std::get_if<editor::TimelineSlipRequest>(
        &dropped.timeline_edits.front());
    UI_CHECK(request != nullptr && request->delta == delta);
    UI_CHECK(dropped.clip_status.find("Source 5 of 10 frames") != std::string::npos);
    const auto pending = h.frame(0.0F, 0.0F, false);
    UI_CHECK(pending.clip_status == dropped.clip_status);
    UI_CHECK(h.elements[1].timing == (scene::ClipTiming{20, 40, 0, 0, 0}));
  }
  Harness full_source;
  full_source.elements[1].sequence_count = 40U;
  static_cast<void>(full_source.click(458.0F, 17.0F));
  static_cast<void>(full_source.frame(500.0F, 113.0F, true));
  const auto wrapped = full_source.frame(463.0F, 113.0F, false);
  UI_CHECK(wrapped.timeline_edits.size() == 1U);
  UI_CHECK(wrapped.clip_status.find("Source 35 of 40 frames") != std::string::npos);
  Harness rated;
  rated.timeline.rate_numerator = 60U;
  rated.elements[1].sequence_count = 10U;
  rated.elements[1].source_frame_rate = scene::MediaFrameRate{24U, 1U};
  static_cast<void>(rated.click(458.0F, 17.0F));
  static_cast<void>(rated.frame(500.0F, 113.0F, true));
  const auto converted = rated.frame(537.0F, 113.0F, false);
  UI_CHECK(converted.timeline_edits.size() == 1U);
  UI_CHECK(converted.clip_status.find("Source 2 of 10 frames") != std::string::npos);
}

void slip_explains_invalid_source_metadata_and_single_frame_media() {
  Harness invalid;
  invalid.elements[1].sequence_count = 0U;
  static_cast<void>(invalid.click(458.0F, 17.0F));
  static_cast<void>(invalid.frame(500.0F, 113.0F, true));
  const auto rejected = invalid.frame(537.0F, 113.0F, false);
  UI_CHECK(rejected.timeline_edits.empty());
  UI_CHECK(rejected.clip_status_error);
  UI_CHECK(rejected.clip_status.find("SLIP UNAVAILABLE") != std::string::npos);
  Harness still;
  still.elements[1].sequence_count = 1U;
  static_cast<void>(still.click(458.0F, 17.0F));
  static_cast<void>(still.frame(500.0F, 113.0F, true));
  const auto unchanged = still.frame(537.0F, 113.0F, false);
  UI_CHECK(!unchanged.clip_status_error);
  UI_CHECK(unchanged.clip_status.find("One source frame; image is unchanged") !=
           std::string::npos);
}

void looping_filmstrip_preview_changes_source_and_stays_bounded() {
  Harness h;
  mediaserver::rhi::Texture texture;
  h.filmstrips.push_back(editor::TimelineFilmstripObservation{
      clip_node, filmstrip_key(), assets::DerivativeState::Ready, &texture});
  h.elements[1].sequence_count = 10U;
  const auto image_uvs = [&]() {
    std::vector<float> out;
    for (const auto &batch : h.list.batches()) {
      if (batch.texture != &texture) continue;
      UI_CHECK(batch.vertex_count <= 32U * 6U);
      for (unsigned i = 0U; i < batch.vertex_count; i += 6U) {
        out.push_back(h.list.vertices()[batch.first_vertex + i].u);
      }
    }
    UI_CHECK(out.size() <= 32U);
    return out;
  };
  static_cast<void>(h.click(458.0F, 17.0F));
  const auto before = image_uvs();
  UI_CHECK(!before.empty());
  static_cast<void>(h.frame(500.0F, 113.0F, true));
  static_cast<void>(h.frame(537.0F, 113.0F, true));
  const auto preview = image_uvs();
  UI_CHECK(before != preview);
  const auto dropped = h.frame(537.0F, 113.0F, false);
  UI_CHECK(dropped.timeline_edits.size() == 1U);
  UI_CHECK(image_uvs() == preview);
  for (unsigned delayed_frame = 0U; delayed_frame < 3U; ++delayed_frame) {
    UI_CHECK(h.frame(0.0F, 0.0F, false).timeline_edits.empty());
    UI_CHECK(image_uvs() == preview);
  }
  // Authority now adopts source phase 5. The same thumbnail positions remain,
  // and a later authored change must supersede the expired local proposal.
  h.elements[1].timing.source_in = 5;
  UI_CHECK(h.frame(0.0F, 0.0F, false).timeline_edits.empty());
  UI_CHECK(image_uvs() == preview);
  h.elements[1].timing.source_in = 0;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  UI_CHECK(image_uvs() == before);
  h.elements[1].timing.duration = 1'000'000;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  UI_CHECK(!image_uvs().empty());
}

void extreme_imported_timing_is_refused_visibly_and_cannot_overflow_snapping() {
  Harness extreme;
  extreme.elements[1].timing = scene::ClipTiming{
      -10, std::numeric_limits<mediaserver::core::i64>::max() / 2, 0, 0, 0};
  static_cast<void>(extreme.frame(500.0F, 113.0F, false));
  const auto refused = extreme.frame(500.0F, 113.0F, true);
  UI_CHECK(!extreme.panel.dragging_clip());
  UI_CHECK(refused.clip_status_error);
  UI_CHECK(refused.clip_status.find("Clip timing exceeds the editable range") !=
           std::string::npos);
  UI_CHECK(!refused.seek_to_frame);
  const auto released = extreme.frame(574.0F, 113.0F, false);
  UI_CHECK(released.clip_edits.empty());
  UI_CHECK(released.timeline_edits.empty());

  Harness distant;
  auto far_clip = make_clip();
  far_clip.node = doc::NodeId{99U};
  far_clip.timing = scene::ClipTiming{
      std::numeric_limits<mediaserver::core::i64>::min(), 1, 0, 0, 0};
  distant.elements.push_back(far_clip);
  static_cast<void>(distant.frame(500.0F, 113.0F, false));
  static_cast<void>(distant.frame(500.0F, 113.0F, true));
  const auto moved = distant.frame(574.0F, 113.0F, false);
  UI_CHECK(moved.clip_edits.size() == 1U);
  UI_CHECK(moved.clip_edits.front().timing.start == 30);
}

void auto_key_is_an_explicit_toggle_and_camera_lens_rows_are_keyable() {
  Harness h;
  UI_CHECK(h.panel.auto_key_enabled());
  static_cast<void>(h.click(190.0F, 45.0F));
  UI_CHECK(!h.panel.auto_key_enabled());
  static_cast<void>(h.type("k"));
  UI_CHECK(h.panel.auto_key_enabled());

  Harness camera;
  camera.elements.resize(1U);
  camera.elements.front().kind = editor::ElementKind::Camera;
  camera.elements.front().field_of_view = 0.173F;
  camera.elements.front().shutter_seconds = 0.017F;
  camera.elements.front().focus_distance_m = 7.5F;
  camera.elements.front().aperture_f_number = 2.8F;
  camera.keys.clear();
  static_cast<void>(camera.click(42.0F, 88.0F));
  const std::array channels{scene::PropertyChannel::CameraFieldOfView,
      scene::PropertyChannel::CameraShutter,
      scene::PropertyChannel::CameraFocusDistance,
      scene::PropertyChannel::CameraAperture};
  const std::array values{0.173F, 0.017F, 7.5F, 2.8F};
  for (std::size_t index = 0; index < channels.size(); ++index) {
    const auto keyed = camera.click(248.0F, 160.0F + 24.0F * static_cast<float>(index));
    UI_CHECK(keyed.keys.size() == 1U);
    UI_CHECK(keyed.keys.front().component == channels[index]);
    UI_CHECK(std::abs(keyed.keys.front().value - values[index]) < 1.0e-6);
  }
}

void timeline_camera_keys_preserve_cubic_evaluated_values() {
  for (const bool weighted : {false, true}) {
    Harness camera;
    camera.elements.resize(1U);
    camera.elements.front().kind = editor::ElementKind::Camera;
    camera.timeline.show_frame = weighted ? 13U : 5U;
    scene::ChannelKeyframes channel{object_node,
        scene::PropertyChannel::CameraFieldOfView,
        {0, weighted ? 32 : 20}, {0.1, 0.2},
        {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Cubic}};
    if (weighted) {
      channel.tangents = {
          scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 0.02, {}, 4.0},
          scene::KeyTangent{scene::KeyTangentMode::Broken, -0.005, 0.0, 12.0, {}}};
    }
    camera.keys = {channel};
    static_cast<void>(camera.click(42.0F, 88.0F));
    const auto keyed = camera.click(248.0F, 160.0F);
    UI_CHECK(keyed.keys.size() == 1U);
    UI_CHECK(keyed.keys.front().component == scene::PropertyChannel::CameraFieldOfView);
    UI_CHECK(std::abs(keyed.keys.front().value - (weighted ? 0.2025 : 0.125)) < 1.0e-6);
  }
}

void a_clip_move_snaps_to_the_playhead() {
  Harness h;
  // Playhead is frame 50, so a body drag that lands the head two frames
  // short of it is pulled onto the cut.
  static_cast<void>(h.frame(500.0F, 113.09F, false));
  static_cast<void>(h.frame(500.0F, 113.09F, true));
  const editor::TimelinePanelFrame snapped = h.frame(707.0F, 113.09F, true);
  UI_CHECK(snapped.clip_edits.empty());
  const editor::TimelinePanelFrame snap_drop = h.frame(707.0F, 113.09F, false);
  UI_CHECK(snap_drop.clip_edits.size() == 1U);
  UI_CHECK(snap_drop.clip_edits.front().timing.start == 50);
}

void pointer_tools_act_on_the_clip_they_are_pressed_on() {
  // The tools sit at x=368..522: Selection, Ripple 394..418, Roll 420..444,
  // Slip 446..470, Slide 472..496, Razor 498..522. None needs the clip to be
  // selected first; each acts on what it is pressed on. Alt keeps snapping
  // out of these exact deltas.
  Harness ripple;
  static_cast<void>(ripple.click(406.0F, 17.0F));
  UI_CHECK(ripple.panel.edit_tool() == editor::TimelineEditTool::RippleTrim);
  static_cast<void>(ripple.frame(700.0F, 113.09F, false, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame ripple_pressed =
      ripple.frame(700.0F, 113.09F, true, gui::KeyMods::Alt);
  UI_CHECK(ripple_pressed.selection.has_value());
  UI_CHECK(ripple_pressed.selection->node == clip_node);
  const editor::TimelinePanelFrame ripple_preview =
      ripple.frame(663.0F, 113.09F, true, gui::KeyMods::Alt);
  UI_CHECK(ripple_preview.timeline_edits.empty());
  UI_CHECK(ripple_preview.clip_edits.empty());
  const editor::TimelinePanelFrame ripple_commit =
      ripple.frame(663.0F, 113.09F, false, gui::KeyMods::Alt);
  UI_CHECK(ripple_commit.timeline_edits.size() == 1U);
  const auto *ripple_request = std::get_if<editor::TimelineRippleTrimRequest>(
      &ripple_commit.timeline_edits.front());
  UI_CHECK(ripple_request != nullptr);
  UI_CHECK(ripple_request->clip == clip_node);
  UI_CHECK(ripple_request->edge == scene::TimelineTrimEdge::Tail);
  UI_CHECK(ripple_request->delta == -5);

  Harness slip;
  static_cast<void>(slip.click(458.0F, 17.0F));
  UI_CHECK(slip.panel.edit_tool() == editor::TimelineEditTool::Slip);
  static_cast<void>(slip.frame(500.0F, 113.09F, false, gui::KeyMods::Alt));
  static_cast<void>(slip.frame(500.0F, 113.09F, true, gui::KeyMods::Alt));
  static_cast<void>(slip.frame(537.0F, 113.09F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame slip_commit =
      slip.frame(537.0F, 113.09F, false, gui::KeyMods::Alt);
  UI_CHECK(slip_commit.timeline_edits.size() == 1U);
  const auto *slip_request = std::get_if<editor::TimelineSlipRequest>(
      &slip_commit.timeline_edits.front());
  UI_CHECK(slip_request != nullptr);
  UI_CHECK(slip_request->clip == clip_node);
  UI_CHECK(slip_request->delta == 5);

  // Roll finds the cut nearest the press: the right half of the clip rolls
  // the cut it shares with the clip butted against its tail.
  Harness roll;
  editor::ElementInfo right = make_clip();
  right.node = doc::NodeId{8U};
  right.timing = scene::ClipTiming{60, 20, 40, 0, 0};
  roll.elements.push_back(right);
  static_cast<void>(roll.click(432.0F, 17.0F));
  UI_CHECK(roll.panel.edit_tool() == editor::TimelineEditTool::Roll);
  static_cast<void>(roll.frame(700.0F, 113.09F, false, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(700.0F, 113.09F, true, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(737.0F, 113.09F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame roll_commit =
      roll.frame(737.0F, 113.09F, false, gui::KeyMods::Alt);
  UI_CHECK(roll_commit.timeline_edits.size() == 1U);
  const auto *roll_request = std::get_if<editor::TimelineRollRequest>(
      &roll_commit.timeline_edits.front());
  UI_CHECK(roll_request != nullptr);
  UI_CHECK(roll_request->first == clip_node);
  UI_CHECK(roll_request->second == right.node);
  UI_CHECK(roll_request->delta == 5);

  // Pressed on the later clip's head, the same cut rolls, still reported
  // left to right.
  static_cast<void>(roll.frame(745.0F, 136.0F, false, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(745.0F, 136.0F, true, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(708.0F, 136.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame rolled_back =
      roll.frame(708.0F, 136.0F, false, gui::KeyMods::Alt);
  UI_CHECK(rolled_back.timeline_edits.size() == 1U);
  const auto *back_request = std::get_if<editor::TimelineRollRequest>(
      &rolled_back.timeline_edits.front());
  UI_CHECK(back_request != nullptr);
  UI_CHECK(back_request->first == clip_node);
  UI_CHECK(back_request->second == right.node);
  UI_CHECK(back_request->delta == -5);

  // Nothing butts against a lone clip, so Roll there is a no-op rather than
  // a scrub.
  Harness lone;
  static_cast<void>(lone.click(432.0F, 17.0F));
  static_cast<void>(lone.frame(700.0F, 113.09F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame lone_drag =
      lone.frame(737.0F, 113.09F, true, gui::KeyMods::Alt);
  UI_CHECK(!lone_drag.seek_to_frame.has_value());
  UI_CHECK(lone.frame(737.0F, 113.09F, false, gui::KeyMods::Alt)
               .timeline_edits.empty());

  Harness slide;
  editor::ElementInfo predecessor = make_clip();
  predecessor.node = doc::NodeId{6U};
  predecessor.timing = scene::ClipTiming{0, 20, 0, 0, 0};
  editor::ElementInfo successor = make_clip();
  successor.node = doc::NodeId{8U};
  successor.timing = scene::ClipTiming{60, 20, 60, 0, 0};
  slide.elements.insert(slide.elements.begin() + 1, predecessor);
  slide.elements.push_back(successor);
  static_cast<void>(slide.click(484.0F, 17.0F));
  UI_CHECK(slide.panel.edit_tool() == editor::TimelineEditTool::Slide);
  static_cast<void>(slide.frame(500.0F, 136.0F, false, gui::KeyMods::Alt));
  static_cast<void>(slide.frame(500.0F, 136.0F, true, gui::KeyMods::Alt));
  static_cast<void>(slide.frame(537.0F, 136.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame slide_commit =
      slide.frame(537.0F, 136.0F, false, gui::KeyMods::Alt);
  UI_CHECK(slide_commit.timeline_edits.size() == 1U);
  const auto *slide_request = std::get_if<editor::TimelineSlideRequest>(
      &slide_commit.timeline_edits.front());
  UI_CHECK(slide_request != nullptr);
  UI_CHECK(slide_request->clip == clip_node);
  UI_CHECK(slide_request->delta == 5);
}

void tool_keys_choose_the_tool_an_editor_expects() {
  Harness h;
  h.focus();
  const std::array<std::pair<std::string_view, editor::TimelineEditTool>, 6U>
      keys{{{"b", editor::TimelineEditTool::RippleTrim},
            {"n", editor::TimelineEditTool::Roll},
            {"y", editor::TimelineEditTool::Slip},
            {"u", editor::TimelineEditTool::Slide},
            {"c", editor::TimelineEditTool::Razor},
            {"v", editor::TimelineEditTool::Standard}}};
  for (const auto &[key, tool] : keys) {
    static_cast<void>(h.type(key));
    UI_CHECK(h.panel.edit_tool() == tool);
  }
  static_cast<void>(h.type("s"));
  UI_CHECK(!h.panel.snapping_enabled());
}

void tool_cursors_follow_selection_and_captured_gestures() {
  Harness h;
  h.focus();
  const std::array<std::pair<std::string_view, gui::Cursor>, 5U> tools{{
      {"b", gui::Cursor::RippleTrim}, {"n", gui::Cursor::Roll},
      {"y", gui::Cursor::Slip}, {"u", gui::Cursor::Slide},
      {"c", gui::Cursor::Razor}}};
  for (const auto& [key, cursor] : tools) {
    static_cast<void>(h.type(key));
    static_cast<void>(h.frame(520.0F, 113.0F, false));
    UI_CHECK(h.ctx.cursor() == cursor);
    // Fade handles must not advertise Selection edits under another tool.
    static_cast<void>(h.frame(409.0F, 102.0F, false));
    UI_CHECK(h.ctx.cursor() == cursor);
    static_cast<void>(h.frame(150.0F, 200.0F, false));
    UI_CHECK(h.ctx.cursor() == gui::Cursor::Arrow);
  }
  static_cast<void>(h.type("y"));
  static_cast<void>(h.frame(520.0F, 113.0F, true));
  static_cast<void>(h.frame(150.0F, 200.0F, true));
  UI_CHECK(h.ctx.cursor() == gui::Cursor::Slip);
  static_cast<void>(h.frame(150.0F, 200.0F, false));
  static_cast<void>(h.type("v"));
  static_cast<void>(h.frame(520.0F, 113.0F, false));
  UI_CHECK(h.ctx.cursor() == gui::Cursor::Move);
  static_cast<void>(h.frame(409.0F, 113.0F, false));
  UI_CHECK(h.ctx.cursor() == gui::Cursor::ResizeHorizontal);
}

void the_razor_cuts_where_it_lands_and_shift_cuts_every_track() {
  Harness h;
  editor::ElementInfo other = make_clip();
  other.node = doc::NodeId{8U};
  other.timing = scene::ClipTiming{10, 40, 0, 0, 0};
  h.elements.push_back(other);
  h.focus();
  static_cast<void>(h.type("c"));
  // Frame 30 is x=482. The clip bar is on the second row.
  static_cast<void>(h.frame(482.0F, 113.09F, false));
  const editor::TimelinePanelFrame cut = h.frame(482.0F, 113.09F, true);
  UI_CHECK(cut.clip_splits ==
           std::vector<editor::TimelineClipSplit>{{clip_node, 30U}});
  UI_CHECK(!cut.seek_to_frame.has_value());
  UI_CHECK(!cut.selection.has_value());
  const editor::TimelinePanelFrame held = h.frame(500.0F, 113.09F, true);
  UI_CHECK(!held.seek_to_frame.has_value());
  UI_CHECK(h.frame(500.0F, 113.09F, false).clip_splits.empty());

  static_cast<void>(h.frame(482.0F, 113.09F, false, gui::KeyMods::Shift));
  const editor::TimelinePanelFrame every =
      h.frame(482.0F, 113.09F, true, gui::KeyMods::Shift);
  UI_CHECK(every.clip_splits ==
           (std::vector<editor::TimelineClipSplit>{{clip_node, 30U},
                                                   {other.node, 30U}}));
  static_cast<void>(h.frame(482.0F, 113.09F, false, gui::KeyMods::Shift));

  // On a clip's own first frame there is nothing to cut.
  static_cast<void>(h.frame(408.0F, 113.09F, false));
  UI_CHECK(h.frame(408.0F, 113.09F, true).clip_splits.empty());
}

void clip_menus_offer_only_what_applies_to_the_clip() {
  Harness edits;
  // The playhead (50) is inside the clip: Split at Playhead, Ripple Delete,
  // Set In/Out to Clip, Clear Keyframes.
  const editor::TimelinePanelFrame split = edits.choose(500.0F, 113.09F, 0U);
  UI_CHECK(split.clip_splits ==
           std::vector<editor::TimelineClipSplit>{{clip_node, 50U}});
  const editor::TimelinePanelFrame erased = edits.choose(500.0F, 113.09F, 1U);
  UI_CHECK(erased.timeline_edits.size() == 1U);
  const auto *erase_request = std::get_if<editor::TimelineRippleDeleteRequest>(
      &erased.timeline_edits.front());
  UI_CHECK(erase_request != nullptr);
  UI_CHECK(erase_request->clips == std::vector<doc::NodeId>{clip_node});

  // A clipboard adds Insert and Overwrite into the clip's own track.
  edits.clipboard_span = 12;
  const editor::TimelinePanelFrame inserted =
      edits.choose(500.0F, 113.09F, 2U);
  UI_CHECK(inserted.timeline_edits.size() == 1U);
  const auto *insert_request = std::get_if<editor::TimelineInsertRequest>(
      &inserted.timeline_edits.front());
  UI_CHECK(insert_request != nullptr);
  UI_CHECK(insert_request->lane == doc::NodeId{});
  const editor::TimelinePanelFrame overwritten =
      edits.choose(500.0F, 113.09F, 3U);
  const auto *overwrite_request = std::get_if<editor::TimelineOverwriteRequest>(
      &overwritten.timeline_edits.front());
  UI_CHECK(overwrite_request != nullptr);
  UI_CHECK(overwrite_request->lane == doc::NodeId{});
  UI_CHECK(edits.choose(500.0F, 113.09F, 4U).range ==
           (editor::RangeRequest{20U, 59U, false}));

  // With the playhead outside the clip there is nothing to split, so the
  // menu does not offer it.
  edits.timeline.show_frame = 90U;
  edits.clipboard_span.reset();
  const editor::TimelinePanelFrame first = edits.choose(500.0F, 113.09F, 0U);
  UI_CHECK(first.clip_splits.empty());
  UI_CHECK(first.timeline_edits.size() == 1U);
}

void the_split_chord_and_comma_and_period_edit_at_the_playhead() {
  Harness h;
  h.selection = {clip_node};
  h.primary = clip_node;
  h.focus();
  const editor::TimelinePanelFrame split = h.key(gui::Key::Split);
  UI_CHECK(split.clip_splits ==
           std::vector<editor::TimelineClipSplit>{{clip_node, 50U}});

  // Comma inserts and period overwrites the clipboard into the selected
  // clip's track, as in every editor.
  h.clipboard_span = 12;
  const editor::TimelinePanelFrame inserted = h.type(",");
  UI_CHECK(inserted.timeline_edits.size() == 1U);
  UI_CHECK(std::holds_alternative<editor::TimelineInsertRequest>(
      inserted.timeline_edits.front()));
  const editor::TimelinePanelFrame overwritten = h.type(".");
  UI_CHECK(std::holds_alternative<editor::TimelineOverwriteRequest>(
      overwritten.timeline_edits.front()));

  Harness outside;
  outside.selection = {clip_node};
  outside.primary = clip_node;
  outside.timeline.show_frame = 90U;
  outside.focus();
  UI_CHECK(outside.key(gui::Key::Split).clip_splits.empty());
  UI_CHECK(outside.type(",").timeline_edits.empty());
}

void track_rows_and_toolbar_emit_typed_lane_actions() {
  Harness h;
  h.bounds.width = 1'400.0F;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{
      destination_lane_node, scene::SceneId{100U}, 20, "Background", false,
      false, scene::BlendMode::Normal, false});

  // The list runs top-down through the stack, so the later Background track
  // is row zero and Foreground - the one holding the clip - is row one.
  // A track owns the screen its clips play on, so selecting one is an
  // ordinary scene-element selection: the Inspector has a transform, a
  // format, and a grade to show for it.
  const editor::TimelinePanelFrame selected = h.click(80.0F, 112.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == lane_node);
  UI_CHECK(selected.selection->action == editor::SelectionAction::Replace);
  UI_CHECK(!selected.selection->surface);

  // A track's mute is the eye every listed row carries at the list's left
  // edge (x=4..28); its lock sits at the list's right edge, the same columns
  // as the Scene outliner's. At 1400 wide the list is 340, so x=312..336.
  const editor::TimelinePanelFrame muted = h.click(16.0F, 112.0F);
  UI_CHECK(muted.lane_edits.size() == 1U);
  UI_CHECK(muted.lane_edits.front().action ==
           editor::TimelineLaneAction::ToggleMute);
  UI_CHECK(muted.lane_edits.front().lane == lane_node);

  const editor::TimelinePanelFrame locked = h.click(324.0F, 112.0F);
  UI_CHECK(locked.lane_edits.size() == 1U);
  UI_CHECK(locked.lane_edits.front().action ==
           editor::TimelineLaneAction::ToggleLock);
  UI_CHECK(locked.lane_edits.front().lane == lane_node);

  // A new track comes from the + that heads the layer list, x=311..335.
  const editor::TimelinePanelFrame created = h.click(323.0F, 45.0F);
  UI_CHECK(created.lane_edits.size() == 1U);
  UI_CHECK(created.lane_edits.front().action ==
           editor::TimelineLaneAction::Create);

  // Everything else a track can have done to it is on its own menu: Rename,
  // Mute, Lock, New Track, then - for a track holding clips - the two
  // explicit destructive choices, delete its clips or move them to a named
  // track. There is no ambiguous delete on a non-empty track.
  const editor::TimelinePanelFrame deleted_clips = h.choose(80.0F, 112.0F, 4U);
  UI_CHECK(deleted_clips.lane_edits.size() == 1U);
  UI_CHECK(deleted_clips.lane_edits.front().action ==
           editor::TimelineLaneAction::DeleteClips);
  UI_CHECK(deleted_clips.lane_edits.front().lane == lane_node);

  const editor::TimelinePanelFrame moved_clips = h.choose(80.0F, 112.0F, 5U);
  UI_CHECK(moved_clips.lane_edits.size() == 1U);
  UI_CHECK(moved_clips.lane_edits.front().action ==
           editor::TimelineLaneAction::DeleteMoveClips);
  UI_CHECK(moved_clips.lane_edits.front().lane == lane_node);
  UI_CHECK(moved_clips.lane_edits.front().destination == destination_lane_node);

  const editor::TimelinePanelFrame menu_muted = h.choose(80.0F, 112.0F, 1U);
  UI_CHECK(menu_muted.lane_edits.size() == 1U);
  UI_CHECK(menu_muted.lane_edits.front().action ==
           editor::TimelineLaneAction::ToggleMute);

  // The empty Background track offers a plain delete.
  const editor::TimelinePanelFrame emptied = h.choose(80.0F, 88.0F, 4U);
  UI_CHECK(emptied.lane_edits.size() == 1U);
  UI_CHECK(emptied.lane_edits.front().action ==
           editor::TimelineLaneAction::DeleteEmpty);
  UI_CHECK(emptied.lane_edits.front().lane == destination_lane_node);

  // A second click on the header opens its inline name field with the whole
  // name selected and hands it the keyboard. Renaming is a double-click
  // because a single click selects the track, which is the screen it owns and
  // the thing the Inspector shows. The name lands once, on Enter, like every
  // other in-place rename.
  static_cast<void>(h.click(185.0F, 112.0F));
  const editor::TimelinePanelFrame typing =
      h.frame(185.0F, 112.0F, false, gui::KeyMods::None, 0.0F);
  UI_CHECK(typing.lane_editing);
  UI_CHECK(typing.lane_edits.empty());
  static_cast<void>(h.key(gui::Key::End));
  static_cast<void>(h.type(" X"));
  const editor::TimelinePanelFrame renamed = h.key(gui::Key::Enter);
  UI_CHECK(renamed.lane_edits.size() == 1U);
  UI_CHECK(renamed.lane_edits.front().action ==
           editor::TimelineLaneAction::Rename);
  UI_CHECK(renamed.lane_edits.front().lane == lane_node);
  UI_CHECK(renamed.lane_edits.front().name == "Foreground X");

  // Rename from the menu opens the same field; typing replaces the selected
  // name, and Escape abandons an edit without renaming anything.
  static_cast<void>(h.frame(1'300.0F, 300.0F, true));
  static_cast<void>(h.frame(1'300.0F, 300.0F, false));
  static_cast<void>(h.choose(80.0F, 112.0F, 0U));
  UI_CHECK(h.type("Backdrop").lane_editing);
  const editor::TimelinePanelFrame menu_renamed = h.key(gui::Key::Enter);
  UI_CHECK(menu_renamed.lane_edits.size() == 1U);
  UI_CHECK(menu_renamed.lane_edits.front().name == "Backdrop");
  static_cast<void>(h.choose(80.0F, 112.0F, 0U));
  static_cast<void>(h.type("Discarded"));
  UI_CHECK(h.key(gui::Key::Escape).lane_edits.empty());
}

// A light opens to the properties a show animates on it - where it is, how
// bright, what colour, how far it reaches - and each keys like any other.
void light_rows_key_intensity_colour_and_position() {
  Harness h;
  editor::ElementInfo light{};
  light.node = doc::NodeId{60U};
  light.kind = editor::ElementKind::PointLight;
  light.intensity = 3.5F;
  light.color = scene::Vec3{1.0F, 0.5F, 0.25F};
  light.range = 12.0F;
  light.position = scene::Vec3{2.0F, 4.0F, 6.0F};
  h.elements = {light};
  h.keys.clear();
  static_cast<void>(h.click(42.0F, 88.0F));
  // Rows: the light, Position, Intensity, Color, Range.
  const auto intensity = h.click(248.0F, 136.0F);
  UI_CHECK(intensity.keys.size() == 1U);
  UI_CHECK(intensity.keys.front().target == light.node);
  UI_CHECK(intensity.keys.front().component ==
           scene::PropertyChannel::LightIntensity);
  UI_CHECK(intensity.keys.front().value == 3.5);
  const auto colour = h.click(248.0F, 160.0F);
  UI_CHECK(colour.keys.size() == 3U);
  UI_CHECK(colour.keys[0].component == scene::PropertyChannel::LightColorR);
  UI_CHECK(colour.keys[1].value == 0.5);
  UI_CHECK(colour.keys[2].component == scene::PropertyChannel::LightColorB);
  const auto position = h.click(248.0F, 112.0F);
  UI_CHECK(position.keys.size() == 3U);
  UI_CHECK(position.keys[2].component == scene::PropertyChannel::PositionZ);
  UI_CHECK(position.keys[2].value == 6.0);
  const auto range = h.click(248.0F, 184.0F);
  UI_CHECK(range.keys.size() == 1U);
  UI_CHECK(range.keys.front().component == scene::PropertyChannel::LightRange);
}

// A layer renames in place, as in the outliner: a second click on its name,
// a new name, Enter.
void a_layer_renames_in_place() {
  Harness h;
  static_cast<void>(h.click(100.0F, 88.0F));
  static_cast<void>(h.click(100.0F, 88.0F));
  UI_CHECK(h.panel.renaming());
  static_cast<void>(h.type("Hero Riser"));
  const auto renamed = h.key(gui::Key::Enter);
  UI_CHECK(renamed.renames ==
           std::vector<editor::TimelineRename>{{object_node, "Hero Riser"}});
  UI_CHECK(!h.panel.renaming());
}

void every_listed_element_carries_its_own_mute_and_lock() {
  Harness h;
  // No tracks, so the rows are the plain object and the clip beneath it. The
  // eye is at the list's left edge (x=4..28) and the lock at its right edge
  // (x=232..256), as in the Scene outliner.
  const editor::TimelinePanelFrame hidden = h.click(16.0F, 88.0F);
  UI_CHECK(hidden.element_flags.size() == 1U);
  UI_CHECK(hidden.element_flags.front() ==
           (editor::TimelineElementFlagEdit{
               object_node, editor::TimelineElementFlag::Visible, false}));

  const editor::TimelinePanelFrame locked = h.click(246.0F, 88.0F);
  UI_CHECK(locked.element_flags.size() == 1U);
  UI_CHECK(locked.element_flags.front() ==
           (editor::TimelineElementFlagEdit{
               object_node, editor::TimelineElementFlag::Locked, true}));

  // A clip is an element like any other and carries the same pair.
  const editor::TimelinePanelFrame clip_locked = h.click(246.0F, 112.0F);
  UI_CHECK(clip_locked.element_flags.size() == 1U);
  UI_CHECK(clip_locked.element_flags.front().element == clip_node);
  UI_CHECK(clip_locked.element_flags.front().flag ==
           editor::TimelineElementFlag::Locked);

  // The value carried is the state being asked for, not the gesture, so an
  // already-hidden element asks to come back rather than to be hidden again.
  h.elements[0U].visible = false;
  const editor::TimelinePanelFrame shown = h.click(16.0F, 88.0F);
  UI_CHECK(shown.element_flags.size() == 1U);
  UI_CHECK(shown.element_flags.front().value == true);

  // Reaching a toggle is not a row click, so it never doubles as a selection.
  UI_CHECK(!shown.selection.has_value());
}

void dragging_a_track_header_reorders_the_stack() {
  Harness h;
  h.bounds.width = 1'400.0F;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{
      destination_lane_node, scene::SceneId{100U}, 20, "Background", false,
      false, scene::BlendMode::Normal, false});

  // Rows run Background, Foreground, Foreground's clip - the stack top-down.
  // Pressing a header claims it for a drag without selecting it, so the row
  // stays a grab target rather than turning into its rename field mid-gesture.
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 112.0F, true);
  UI_CHECK(pressed.lane_edits.empty());

  // Dragging Foreground up onto the Background band previews a later place in
  // the stack, because up the list is toward the front of the picture. Like a
  // Feed Mapping surface drop, hovering authors nothing: the target and the
  // labelled drag ghost are only immediate feedback.
  const editor::TimelinePanelFrame over_background =
      h.frame(1'200.0F, 87.0F, true);
  UI_CHECK(over_background.lane_edits.empty());
  UI_CHECK(over_background.lane_editing);

  // Back over its own band asks for nothing.
  const editor::TimelinePanelFrame home = h.frame(1'200.0F, 109.0F, true);
  UI_CHECK(home.lane_edits.empty());

  // A clip row inside a track counts as that track, so the drop does not have
  // to land on the header itself.
  const editor::TimelinePanelFrame over_clip = h.frame(1'200.0F, 131.0F, true);
  UI_CHECK(over_clip.lane_edits.empty());

  // One resolved edit lands on release even when the pointer is over the time
  // graph rather than the narrow header column.
  const editor::TimelinePanelFrame released =
      h.frame(1'200.0F, 87.0F, false);
  UI_CHECK(released.lane_edits.size() == 1U);
  UI_CHECK(released.lane_edits.front() ==
           editor::TimelineLaneEdit{editor::TimelineLaneAction::MoveTo,
                                    lane_node, {}, destination_lane_node});
  const editor::TimelinePanelFrame after = h.frame(80.0F, 88.0F, false);
  UI_CHECK(after.lane_edits.empty());
  UI_CHECK(!after.lane_editing);

  // A press that never clears the movement threshold remains a click, not an
  // accidental reorder.
  static_cast<void>(h.frame(80.0F, 112.0F, true));
  const editor::TimelinePanelFrame still = h.frame(82.0F, 112.0F, true);
  UI_CHECK(still.lane_edits.empty());
  UI_CHECK(!still.lane_editing);
  const editor::TimelinePanelFrame clicked = h.frame(82.0F, 112.0F, false);
  UI_CHECK(clicked.lane_edits.empty());
  UI_CHECK(clicked.selection.has_value());

  // A locked track advertises the lock instead of a live drag affordance and
  // cannot be lifted into a reorder gesture.
  h.lanes.front().locked = true;
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  static_cast<void>(h.frame(80.0F, 112.0F, true));
  const editor::TimelinePanelFrame locked = h.frame(1'200.0F, 87.0F, true);
  UI_CHECK(locked.lane_edits.empty());
  UI_CHECK(!locked.lane_editing);
  const editor::TimelinePanelFrame locked_release =
      h.frame(1'200.0F, 87.0F, false);
  UI_CHECK(locked_release.lane_edits.empty());
}

void a_track_drop_crosses_multiple_bands_in_one_transaction() {
  Harness h;
  h.bounds.width = 1'400.0F;
  h.elements[1U].timeline_lane = lane_node;
  const doc::NodeId top_lane{42U};
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{
      destination_lane_node, scene::SceneId{100U}, 20, "Midground", false,
      false, scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{top_lane, scene::SceneId{100U}, 30,
                                        "Background", false, false,
                                        scene::BlendMode::Normal, false});

  // Display order is Background, Midground, Foreground, then its clip. Lift
  // Foreground and drop it directly on Background; no document edit leaks out
  // while crossing Midground.
  static_cast<void>(h.frame(80.0F, 136.0F, false));
  static_cast<void>(h.frame(80.0F, 136.0F, true));
  const editor::TimelinePanelFrame crossed = h.frame(900.0F, 88.0F, true);
  UI_CHECK(crossed.lane_edits.empty());
  UI_CHECK(crossed.lane_editing);

  const editor::TimelinePanelFrame dropped = h.frame(900.0F, 88.0F, false);
  UI_CHECK(dropped.lane_edits.size() == 1U);
  UI_CHECK(dropped.lane_edits.front() ==
           editor::TimelineLaneEdit{editor::TimelineLaneAction::MoveTo,
                                    lane_node, {}, top_lane});
}

void dragging_a_clip_across_rows_moves_it_between_tracks() {
  Harness h;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{
      destination_lane_node, scene::SceneId{100U}, 20, "Background", false,
      false, scene::BlendMode::Normal, false});

  // Rows run Background, Foreground, Foreground's clip, so the bar is row two.
  static_cast<void>(h.frame(500.0F, 137.09F, false));
  const editor::TimelinePanelFrame pressed = h.frame(500.0F, 137.09F, true);
  UI_CHECK(h.panel.dragging_clip());
  UI_CHECK(pressed.selection.has_value());

  // Straight up into the Background band with no horizontal movement: the
  // placement is untouched and the target previews without authoring.
  const editor::TimelinePanelFrame crossed = h.frame(500.0F, 88.0F, true);
  UI_CHECK(crossed.clip_edits.empty());

  const editor::TimelinePanelFrame dropped = h.frame(500.0F, 88.0F, false);
  UI_CHECK(dropped.clip_edits.size() == 1U);
  UI_CHECK(dropped.clip_edits.front().clip == clip_node);
  UI_CHECK(dropped.clip_edits.front().timing ==
           (scene::ClipTiming{20, 40, 0, 0, 0}));
  UI_CHECK(dropped.clip_edits.front().lane.has_value());
  UI_CHECK(*dropped.clip_edits.front().lane == destination_lane_node);
  UI_CHECK(!h.panel.dragging_clip());
}

void dragging_a_media_row_moves_it_without_hunting_for_its_bar() {
  Harness h;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{
      destination_lane_node, scene::SceneId{100U}, 20, "Background", false,
      false, scene::BlendMode::Normal, false});

  // The media row itself is a full-width lift handle. This is usable even when
  // the clip is offscreen or only a few pixels wide in the time graph.
  static_cast<void>(h.frame(80.0F, 137.09F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 137.09F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(h.panel.dragging_layer());
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 88.0F, true);
  UI_CHECK(preview.clip_edits.empty());

  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 88.0F, false);
  UI_CHECK(dropped.clip_edits.size() == 1U);
  UI_CHECK(dropped.clip_edits.front().clip == clip_node);
  UI_CHECK(dropped.clip_edits.front().timing ==
           (scene::ClipTiming{20, 40, 0, 0, 0}));
  UI_CHECK(dropped.clip_edits.front().lane.has_value());
  UI_CHECK(*dropped.clip_edits.front().lane == destination_lane_node);
}

void dragging_object_and_model_rows_reorders_the_layer_stack() {
  Harness h;
  h.keys.clear();
  h.elements[0U].group_order = 2;
  editor::ElementInfo model{};
  model.node = doc::NodeId{8U};
  model.kind = editor::ElementKind::Model;
  model.source = "fixture.glb";
  model.group_order = 1;
  h.elements = {h.elements[0U], model};

  // Higher order is visible first. Lift the object from the first row and
  // release in the lower half of the model row: one visible After insertion.
  static_cast<void>(h.frame(80.0F, 88.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 88.0F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(h.panel.dragging_layer());
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 118.55F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.lane_editing);

  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 118.55F, false);
  UI_CHECK(dropped.layer_reorders ==
           std::vector<editor::TimelineLayerReorder>{{
               object_node, model.node, editor::TimelineLayerPlacement::After}});
  UI_CHECK(!h.panel.dragging_layer());
}

void dragging_a_layer_out_reports_one_screen_space_drop() {
  Harness h;
  h.keys.clear();

  static_cast<void>(h.frame(80.0F, 88.0F, false));
  static_cast<void>(h.frame(80.0F, 88.0F, true));
  const editor::TimelinePanelFrame lifted =
      h.frame(1'180.0F, 240.0F, true);
  UI_CHECK(h.panel.dragging_layer());
  UI_CHECK(!lifted.layer_drop.has_value());

  const editor::TimelinePanelFrame dropped =
      h.frame(1'180.0F, 240.0F, false);
  UI_CHECK(dropped.layer_drop == editor::TimelineLayerDrop{
                                    {object_node}, 1'180.0F, 240.0F});
  UI_CHECK(dropped.layer_reorders.empty());
  UI_CHECK(!h.panel.dragging_layer());
}

void dragging_a_track_out_reports_one_screen_space_drop() {
  Harness h;
  h.bounds.width = 1'400.0F;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Program Track", false, false,
                                        scene::BlendMode::Normal, false});

  static_cast<void>(h.frame(80.0F, 88.0F, false));
  static_cast<void>(h.frame(80.0F, 88.0F, true));
  const editor::TimelinePanelFrame lifted =
      h.frame(1'180.0F, 240.0F, true);
  UI_CHECK(h.panel.dragging_compositor_source());
  UI_CHECK(h.panel.dragging_compositor_source_node() == lane_node);
  UI_CHECK(h.panel.dragging_compositor_source_count() == 1U);
  UI_CHECK(!lifted.layer_drop.has_value());

  const editor::TimelinePanelFrame dropped =
      h.frame(1'180.0F, 240.0F, false);
  UI_CHECK(dropped.layer_drop == editor::TimelineLayerDrop{
                                    {lane_node}, 1'180.0F, 240.0F});
  UI_CHECK(dropped.lane_edits.empty());
  UI_CHECK(!h.panel.dragging_compositor_source());
}

void dragging_selected_layers_preserves_the_group_in_timeline_order() {
  Harness h;
  h.keys.clear();
  h.selection = {clip_node, object_node};
  h.primary = clip_node;
  static_cast<void>(h.frame(80.0F, 88.0F, false));
  const auto pressed = h.frame(80.0F, 88.0F, true);
  UI_CHECK(!pressed.selection.has_value());
  UI_CHECK(h.panel.dragging_compositor_source_count() == 2U);
  const auto lifted = h.frame(1'180.0F, 240.0F, true);
  UI_CHECK(!lifted.selection.has_value());
  UI_CHECK(!lifted.layer_drop.has_value());
  const auto dropped = h.frame(1'180.0F, 240.0F, false);
  UI_CHECK(dropped.layer_drop == editor::TimelineLayerDrop{
      {object_node, clip_node}, 1'180.0F, 240.0F});
  UI_CHECK(!dropped.selection.has_value());
  UI_CHECK(h.panel.dragging_compositor_source_count() == 0U);
  UI_CHECK(!h.frame(1'180.0F, 240.0F, false).layer_drop.has_value());

  // A click without a drag still focuses just the clicked layer.
  const auto clicked = h.click(80.0F, 88.0F);
  UI_CHECK(clicked.selection.has_value());
  UI_CHECK(clicked.selection->action == editor::SelectionAction::Replace);
  UI_CHECK(clicked.selection->node == object_node);
}

void layer_drags_exclude_locked_selection_and_reset_the_payload() {
  Harness h;
  h.keys.clear();
  h.elements[1U].locked = true;
  h.selection = {clip_node, object_node, doc::NodeId{999U}};
  static_cast<void>(h.frame(80.0F, 88.0F, false));
  static_cast<void>(h.frame(80.0F, 88.0F, true));
  static_cast<void>(h.frame(1'180.0F, 240.0F, true));
  UI_CHECK(h.frame(1'180.0F, 240.0F, false).layer_drop ==
           editor::TimelineLayerDrop{{object_node}, 1'180.0F, 240.0F});
  h.elements[1U].locked = false;
  h.selection = {object_node};
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  const auto pressed = h.frame(80.0F, 112.0F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(pressed.selection->node == clip_node);
  static_cast<void>(h.frame(1'180.0F, 240.0F, true));
  UI_CHECK(h.frame(1'180.0F, 240.0F, false).layer_drop ==
           editor::TimelineLayerDrop{{clip_node}, 1'180.0F, 240.0F});
}

void modifier_layer_clicks_select_once_per_gesture() {
  Harness h;
  h.keys.clear();
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  editor::NodeSelection selection;
  selection.replace(object_node);
  const auto click = [&](float y, gui::KeyMods mods) {
    for (const bool down : {false, true, false}) {
      h.selection.assign(selection.members().begin(), selection.members().end());
      h.primary = selection.primary();
      const auto result = h.frame(80.0F, y, down, mods);
      if (result.selection.has_value()) {
        const auto &request = *result.selection;
        selection.apply(request.action, request.node, request.visible_order);
      }
    }
  };
  click(112.0F, gui::KeyMods::Ctrl);
  UI_CHECK(selection.size() == 2U);
  click(112.0F, gui::KeyMods::Ctrl);
  UI_CHECK(selection.size() == 1U);
  UI_CHECK(selection.contains(object_node));
  click(112.0F, gui::KeyMods::Shift);
  UI_CHECK(selection.size() == 2U);
}

void dragging_selected_media_outside_the_timeline_does_not_reassign_tracks() {
  Harness h;
  h.keys.clear();
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.selection = {object_node, clip_node};
  // Header, clip, object. The compositor can sit beside the timeline at the
  // same height as a row belonging to a different track (or a loose layer).
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  static_cast<void>(h.frame(80.0F, 112.0F, true));
  static_cast<void>(h.frame(1'180.0F, 160.0F, true));
  const auto dropped = h.frame(1'180.0F, 160.0F, false);
  UI_CHECK(dropped.clip_edits.empty());
  UI_CHECK(dropped.layer_drop == editor::TimelineLayerDrop{
      {clip_node, object_node}, 1'180.0F, 160.0F});
}

void track_headers_support_group_selection_and_group_drops() {
  Harness h;
  h.elements.clear();
  h.keys.clear();
  h.lanes = {
      scene::TimelineLane{lane_node, scene::SceneId{100U}, 10, "Back", false,
                          false, scene::BlendMode::Normal, false},
      scene::TimelineLane{destination_lane_node, scene::SceneId{100U}, 20,
                          "Front", false, false, scene::BlendMode::Normal, false}};
  const auto toggled = h.click(80.0F, 112.0F, gui::KeyMods::Ctrl);
  UI_CHECK(toggled.selection.has_value());
  UI_CHECK(toggled.selection->action == editor::SelectionAction::Toggle);
  UI_CHECK(toggled.selection->node == lane_node);
  const auto ranged = h.click(80.0F, 112.0F, gui::KeyMods::Shift);
  UI_CHECK(ranged.selection.has_value());
  UI_CHECK(ranged.selection->action == editor::SelectionAction::Range);
  UI_CHECK(ranged.selection->visible_order ==
           std::vector<doc::NodeId>{destination_lane_node, lane_node});
  h.selection = {lane_node, destination_lane_node};
  static_cast<void>(h.frame(80.0F, 88.0F, true));
  UI_CHECK(h.panel.dragging_compositor_source_count() == 2U);
  static_cast<void>(h.frame(1'180.0F, 240.0F, true));
  const auto dropped = h.frame(1'180.0F, 240.0F, false);
  UI_CHECK(dropped.layer_drop == editor::TimelineLayerDrop{
      {destination_lane_node, lane_node}, 1'180.0F, 240.0F});
  UI_CHECK(dropped.lane_edits.empty());
  UI_CHECK(!dropped.selection.has_value());
  UI_CHECK(h.panel.dragging_compositor_source_count() == 0U);
}

void dragging_clip_rows_reorders_clips_inside_one_track() {
  Harness h;
  h.keys.clear();
  editor::ElementInfo front = make_clip();
  front.timeline_lane = lane_node;
  front.clip_order = 2;
  editor::ElementInfo back = make_clip();
  back.node = doc::NodeId{8U};
  back.source = "back.png";
  back.timeline_lane = lane_node;
  back.clip_order = 1;
  h.elements = {front, back};
  h.lanes = {scene::TimelineLane{
      lane_node, scene::SceneId{100U}, 10, "Screen", false, false,
      scene::BlendMode::Normal, false}};

  // Header is y=56..78, then front and back clip rows. This is an ordering
  // insertion inside the track, not a track-assignment drop.
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  static_cast<void>(h.frame(80.0F, 112.0F, true));
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 142.55F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 142.55F, false);
  UI_CHECK(dropped.clip_edits.empty());
  UI_CHECK(dropped.layer_reorders ==
           std::vector<editor::TimelineLayerReorder>{{
               clip_node, back.node, editor::TimelineLayerPlacement::After}});
}

void an_explicit_track_does_not_steal_loose_clip_ordering() {
  Harness h;
  h.keys.clear();
  editor::ElementInfo front = make_clip();
  front.group_order = 2;
  editor::ElementInfo back = make_clip();
  back.node = doc::NodeId{8U};
  back.source = "back.png";
  back.group_order = 1;
  h.elements = {front, back};
  // Scene projection includes the implicit authoring lane whenever loose
  // media exists. Its presence must not turn loose clips into lane children.
  h.lanes = {
      scene::TimelineLane{doc::NodeId{}, scene::SceneId{100U}, 0, {}, false,
                          false, scene::BlendMode::Normal, true},
      scene::TimelineLane{lane_node, scene::SceneId{100U}, 10, "Screen",
                          false, false, scene::BlendMode::Normal, false}};

  // The explicit header is first, followed by loose clips in group_order.
  // Reordering those rows remains a sibling-layer insertion, not a track move.
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  static_cast<void>(h.frame(80.0F, 112.0F, true));
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 142.55F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 142.55F, false);
  UI_CHECK(dropped.clip_edits.empty());
  UI_CHECK(dropped.layer_reorders ==
           std::vector<editor::TimelineLayerReorder>{{
               clip_node, back.node, editor::TimelineLayerPlacement::After}});
}

void a_locked_track_refuses_a_clip_and_a_drop_outside_leaves_every_track() {
  Harness h;
  h.elements[1U].timeline_lane = lane_node;
  h.lanes.push_back(scene::TimelineLane{lane_node, scene::SceneId{100U}, 10,
                                        "Foreground", false, false,
                                        scene::BlendMode::Normal, false});
  h.lanes.push_back(scene::TimelineLane{destination_lane_node,
                                        scene::SceneId{100U}, 20, "Background",
                                        false, true, scene::BlendMode::Normal,
                                        false});
  // The implicit band sorts to the back of the stack, so callers hand it over
  // first and the reversed list puts it last.
  h.lanes.insert(h.lanes.begin(),
                 scene::TimelineLane{doc::NodeId{}, scene::SceneId{100U}, 0,
                                     {}, false, false,
                                     scene::BlendMode::Normal, true});

  // Rows run Background, Foreground, Foreground's clip, object.
  static_cast<void>(h.frame(500.0F, 137.09F, false));
  static_cast<void>(h.frame(500.0F, 137.09F, true));
  UI_CHECK(h.panel.dragging_clip());

  // Row zero is the locked Background. A locked track refuses an arriving clip
  // the same way it refuses edits to the ones it already holds.
  const editor::TimelinePanelFrame refused = h.frame(500.0F, 88.0F, true);
  UI_CHECK(refused.clip_edits.empty());

  // Clips with no track get no header of their own, so row three is the plain
  // object row. Every row outside a track's band means the same thing - leave
  // the track - which is what makes dropping a clip out of one possible at all.
  const editor::TimelinePanelFrame outside = h.frame(500.0F, 160.0F, true);
  UI_CHECK(outside.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(500.0F, 160.0F, false);
  UI_CHECK(dropped.clip_edits.size() == 1U);
  UI_CHECK(dropped.clip_edits.front().lane.has_value());
  UI_CHECK(*dropped.clip_edits.front().lane == doc::NodeId{});
}

void filmstrips_request_exact_visible_keys_and_do_not_retry_failures() {
  Harness h;
  const assets::DerivativeKey key = filmstrip_key();
  h.filmstrips.push_back(editor::TimelineFilmstripObservation{
      clip_node, key, assets::DerivativeState::Missing, nullptr});

  const editor::TimelinePanelFrame missing =
      h.frame(0.0F, 0.0F, false);
  UI_CHECK(missing.filmstrip_retain.has_value());
  UI_CHECK(missing.filmstrip_retain->keys ==
           std::vector<assets::DerivativeKey>{key});
  UI_CHECK(missing.filmstrip_requests.size() == 1U);
  UI_CHECK(missing.filmstrip_requests.front() ==
           editor::TimelineFilmstripRequestIntent{
               key, editor::TimelineFilmstripRequestPriority::Visible});

  h.filmstrips.front().state = assets::DerivativeState::Generating;
  const editor::TimelinePanelFrame generating =
      h.frame(0.0F, 0.0F, false);
  UI_CHECK(generating.filmstrip_retain.has_value());
  UI_CHECK(generating.filmstrip_retain->keys.size() == 1U);
  UI_CHECK(generating.filmstrip_requests.empty());

  h.filmstrips.front().state = assets::DerivativeState::Failed;
  const editor::TimelinePanelFrame failed =
      h.frame(0.0F, 0.0F, false);
  UI_CHECK(failed.filmstrip_retain.has_value());
  UI_CHECK(failed.filmstrip_retain->keys.size() == 1U);
  UI_CHECK(failed.filmstrip_requests.empty());

  h.filmstrips.front().state = assets::DerivativeState::Stale;
  const editor::TimelinePanelFrame stale = h.frame(0.0F, 0.0F, false);
  UI_CHECK(stale.filmstrip_requests.size() == 1U);
  UI_CHECK(stale.filmstrip_requests.front().key == key);
}

void waveforms_request_exact_keys_and_draw_only_valid_ready_envelopes() {
  Harness h;
  const assets::DerivativeKey key = waveform_key();
  h.waveforms.push_back(editor::TimelineWaveformObservation{
      clip_node, key, assets::DerivativeState::Missing, {}});

  const editor::TimelinePanelFrame missing = h.frame(0.0F, 0.0F, false);
  UI_CHECK(missing.waveform_retain.has_value());
  UI_CHECK(missing.waveform_retain->keys ==
           std::vector<assets::DerivativeKey>{key});
  UI_CHECK(missing.waveform_requests ==
           std::vector<editor::TimelineWaveformRequestIntent>{
               {key, editor::TimelineWaveformRequestPriority::Visible}});
  const std::size_t without_envelope = h.list.vertices().size();

  h.waveform_bins.fill(assets::WaveformPeakRms{0.75F, 0.35F});
  h.waveforms.front().state = assets::DerivativeState::Ready;
  h.waveforms.front().bins = h.waveform_bins;
  const editor::TimelinePanelFrame ready = h.frame(0.0F, 0.0F, false);
  UI_CHECK(ready.waveform_requests.empty());
  UI_CHECK(ready.waveform_retain.has_value());
  UI_CHECK(ready.waveform_retain->keys.size() == 1U);
  UI_CHECK(h.list.vertices().size() > without_envelope);

  h.waveforms.front().state = assets::DerivativeState::Failed;
  h.waveforms.front().bins = {};
  const editor::TimelinePanelFrame failed = h.frame(0.0F, 0.0F, false);
  UI_CHECK(failed.waveform_requests.empty());
}

[[nodiscard]] scene::TimelineMarker
timeline_marker(std::uint64_t id, std::int64_t frame, scene::CueAction cue,
                std::string name) {
  scene::TimelineMarker out{};
  out.id = doc::NodeId{id};
  out.scene = scene::SceneId{100U};
  out.frame = frame;
  out.name = std::move(name);
  out.cue = cue;
  return out;
}

void markers_and_cues_are_added_at_the_playhead_and_where_double_clicked() {
  Harness h;
  // The marker and cue buttons follow the view tools, x=690..714 and
  // 716..740; each creates at the playhead (frame 50).
  const editor::TimelinePanelFrame marked = h.click(702.0F, 17.0F);
  UI_CHECK(marked.marker_edits.size() == 1U);
  UI_CHECK(marked.marker_edits.front().action ==
           editor::TimelineMarkerAction::Create);
  UI_CHECK(marked.marker_edits.front().marker.frame == 50);
  UI_CHECK(marked.marker_edits.front().marker.cue == scene::CueAction::None);
  UI_CHECK(marked.marker_edits.front().marker.name == "Marker 1");
  const editor::TimelinePanelFrame cued = h.click(728.0F, 17.0F);
  UI_CHECK(cued.marker_edits.size() == 1U);
  UI_CHECK(cued.marker_edits.front().marker.cue == scene::CueAction::Hold);

  // M, Q, and Shift+Q are the same three, from the keyboard.
  h.focus();
  UI_CHECK(h.type("m").marker_edits.front().marker.cue ==
           scene::CueAction::None);
  UI_CHECK(h.type("q").marker_edits.front().marker.cue ==
           scene::CueAction::Hold);
  UI_CHECK(h.type("Q").marker_edits.front().marker.cue ==
           scene::CueAction::Loop);

  // The marker strip is y=56..76. One click scrubs; a double-click drops a
  // marker on the frame under it (frame 30 at x=482).
  const editor::TimelinePanelFrame scrubbed = h.click(482.0F, 66.0F);
  UI_CHECK(scrubbed.seek_to_frame == std::optional<std::uint64_t>{30U});
  UI_CHECK(scrubbed.marker_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(482.0F, 66.0F, true);
  UI_CHECK(dropped.marker_edits.size() == 1U);
  UI_CHECK(dropped.marker_edits.front().marker.frame == 30);
  static_cast<void>(h.frame(482.0F, 66.0F, false));

  // On a marker that is already there, M names it instead of stacking a
  // second one on the same frame.
  Harness existing;
  existing.markers = {timeline_marker(60U, 50, scene::CueAction::None, "Intro")};
  existing.focus();
  const editor::TimelinePanelFrame naming = existing.type("m");
  UI_CHECK(naming.marker_edits.empty());
  static_cast<void>(existing.type("!"));
  const editor::TimelinePanelFrame named = existing.key(gui::Key::Enter);
  UI_CHECK(named.marker_edits.size() == 1U);
  UI_CHECK(named.marker_edits.front().marker.name == "Intro!");
  UI_CHECK(named.transport == editor::TransportRequest::None);
}

void a_marker_is_chosen_moved_renamed_and_deleted_on_the_strip() {
  Harness h;
  h.markers = {timeline_marker(60U, 20, scene::CueAction::None, "Intro"),
               timeline_marker(61U, 40, scene::CueAction::Hold, "Band")};
  // Frame 20 is x=408. A click chooses the marker and goes there.
  const editor::TimelinePanelFrame chosen = h.click(410.0F, 66.0F);
  UI_CHECK(chosen.seek_to_frame == std::optional<std::uint64_t>{20U});
  UI_CHECK(h.panel.selected_markers().size() == 1U);
  UI_CHECK(h.panel.selected_markers().front() == doc::NodeId{60U});

  // A second click names it in place.
  static_cast<void>(h.click(410.0F, 66.0F));
  const editor::TimelinePanelFrame typing = h.type("!");
  UI_CHECK(typing.editing);
  const editor::TimelinePanelFrame renamed = h.key(gui::Key::Enter);
  UI_CHECK(renamed.marker_edits.size() == 1U);
  UI_CHECK(renamed.marker_edits.front().marker.name == "Intro!");

  // A drag previews and lands once. Alt holds snapping off for an exact ten
  // frames.
  static_cast<void>(h.frame(410.0F, 66.0F, true));
  const editor::TimelinePanelFrame dragging =
      h.frame(484.0F, 66.0F, true, gui::KeyMods::Alt);
  UI_CHECK(dragging.marker_edits.empty());
  UI_CHECK(dragging.editing);
  UI_CHECK(h.panel.dragging_marker());
  const editor::TimelinePanelFrame moved =
      h.frame(484.0F, 66.0F, false, gui::KeyMods::Alt);
  UI_CHECK(moved.marker_edits.size() == 1U);
  UI_CHECK(moved.marker_edits.front().action ==
           editor::TimelineMarkerAction::Update);
  UI_CHECK(moved.marker_edits.front().marker.id == doc::NodeId{60U});
  UI_CHECK(moved.marker_edits.front().marker.frame == 30);
  UI_CHECK(moved.marker_edits.front().marker.name == "Intro");
  UI_CHECK(!moved.seek_to_frame.has_value());

  // Without Alt the marker pulls onto the playhead, two frames away.
  static_cast<void>(h.frame(410.0F, 66.0F, true));
  static_cast<void>(h.frame(615.0F, 66.0F, true));
  const editor::TimelinePanelFrame snapped = h.frame(615.0F, 66.0F, false);
  UI_CHECK(snapped.marker_edits.size() == 1U);
  UI_CHECK(snapped.marker_edits.front().marker.frame == 50);

  // Delete removes the chosen marker, and only that: the scene selection is
  // left for the shell.
  const editor::TimelinePanelFrame deleted = h.key(gui::Key::Delete);
  UI_CHECK(deleted.consume_delete);
  UI_CHECK(deleted.marker_edits.size() == 1U);
  UI_CHECK(deleted.marker_edits.front().action ==
           editor::TimelineMarkerAction::Delete);
  UI_CHECK(deleted.marker_edits.front().marker.id == doc::NodeId{60U});

  // Choosing a layer leaves the marker domain.
  static_cast<void>(h.click(80.0F, 88.0F));
  UI_CHECK(h.panel.selected_markers().empty());
}

void marker_and_ruler_menus_convert_recolour_and_add() {
  Harness h;
  h.markers = {timeline_marker(60U, 20, scene::CueAction::None, "Intro")};
  // A plain marker's menu: Rename, Make Hold Cue, Make Loop Cue, Color,
  // Go To, Delete.
  const editor::TimelinePanelFrame held = h.choose(410.0F, 66.0F, 1U);
  UI_CHECK(held.marker_edits.size() == 1U);
  UI_CHECK(held.marker_edits.front().marker.cue == scene::CueAction::Hold);
  UI_CHECK(held.marker_edits.front().marker.name == "Intro");
  const editor::TimelinePanelFrame looped = h.choose(410.0F, 66.0F, 2U);
  UI_CHECK(looped.marker_edits.front().marker.cue == scene::CueAction::Loop);
  const editor::TimelinePanelFrame recoloured = h.choose(410.0F, 66.0F, 3U);
  UI_CHECK(recoloured.marker_edits.front().marker.color == 1U);
  UI_CHECK(h.choose(410.0F, 66.0F, 4U).seek_to_frame ==
           std::optional<std::uint64_t>{20U});
  const editor::TimelinePanelFrame removed = h.choose(410.0F, 66.0F, 5U);
  UI_CHECK(removed.marker_edits.front().action ==
           editor::TimelineMarkerAction::Delete);

  // A cue's menu offers the way back to a plain marker.
  h.markers = {timeline_marker(61U, 20, scene::CueAction::Hold, "Band")};
  const editor::TimelinePanelFrame plain = h.choose(410.0F, 66.0F, 2U);
  UI_CHECK(plain.marker_edits.front().marker.cue == scene::CueAction::None);

  // The ruler's menu works on the frame it was opened over: frame 60 at
  // x=704. Add Marker, Add Hold Cue, Add Loop Cue, Set In, Set Out, Zoom to
  // Fit, Show Frames.
  const editor::TimelinePanelFrame added = h.choose(704.0F, 45.0F, 1U);
  UI_CHECK(added.marker_edits.size() == 1U);
  UI_CHECK(added.marker_edits.front().marker.frame == 60);
  UI_CHECK(added.marker_edits.front().marker.cue == scene::CueAction::Hold);
  UI_CHECK(h.choose(704.0F, 45.0F, 3U).range ==
           (editor::RangeRequest{60U, 100U, false}));
  UI_CHECK(h.choose(704.0F, 45.0F, 4U).range ==
           (editor::RangeRequest{0U, 60U, false}));
}

void go_and_the_arm_follow_the_cues_and_their_status() {
  Harness h;
  h.markers = {timeline_marker(61U, 40, scene::CueAction::Hold, "Band"),
               timeline_marker(62U, 80, scene::CueAction::Loop, "Vamp")};
  h.timeline.held_cue = doc::NodeId{61U};
  h.timeline.next_cue = doc::NodeId{62U};
  // With cues in the scene, GO is the right-most control, x=894..994, and
  // the arm sits beside it at 866..890.
  UI_CHECK(h.click(944.0F, 17.0F).transport == editor::TransportRequest::Go);
  UI_CHECK(h.click(878.0F, 17.0F).cues_armed == std::optional<bool>{false});
  h.timeline.cues_armed = false;
  UI_CHECK(h.click(878.0F, 17.0F).cues_armed == std::optional<bool>{true});

  // Enter is GO and Space is play while the timeline has the keyboard.
  h.focus();
  UI_CHECK(h.key(gui::Key::Enter).transport == editor::TransportRequest::Go);
  UI_CHECK(h.type(" ").transport == editor::TransportRequest::TogglePlay);

  // Without cues there is no GO; that space belongs to In/Out.
  Harness plain;
  UI_CHECK(plain.click(944.0F, 17.0F).transport ==
           editor::TransportRequest::None);
}

void arrows_home_end_and_up_down_navigate_time() {
  Harness h;
  h.markers = {timeline_marker(60U, 30, scene::CueAction::None, "Mark")};
  h.focus();
  UI_CHECK(h.key(gui::Key::Left).transport ==
           editor::TransportRequest::StepBack);
  UI_CHECK(h.key(gui::Key::Right, gui::KeyMods::Shift).transport ==
           editor::TransportRequest::JumpForward);
  UI_CHECK(h.key(gui::Key::Home).transport ==
           editor::TransportRequest::GoToStart);
  UI_CHECK(h.key(gui::Key::End).transport ==
           editor::TransportRequest::GoToEnd);
  // Up and Down walk the edit points either side of the playhead (50): the
  // marker at 30 behind, the clip's end at 60 ahead.
  UI_CHECK(h.key(gui::Key::Up).seek_to_frame ==
           std::optional<std::uint64_t>{30U});
  UI_CHECK(h.key(gui::Key::Down).seek_to_frame ==
           std::optional<std::uint64_t>{60U});
}

void a_clip_move_snaps_to_a_marker() {
  Harness h;
  h.markers = {timeline_marker(60U, 35, scene::CueAction::None, "Hit")};
  // Fourteen frames right would put the head on 34, one frame short of the
  // marker, which pulls it the rest of the way.
  static_cast<void>(h.frame(500.0F, 113.09F, false));
  static_cast<void>(h.frame(500.0F, 113.09F, true));
  static_cast<void>(h.frame(603.6F, 113.09F, true));
  const editor::TimelinePanelFrame dropped = h.frame(603.6F, 113.09F, false);
  UI_CHECK(dropped.clip_edits.size() == 1U);
  UI_CHECK(dropped.clip_edits.front().timing.start == 35);
}

void keys_act_on_the_playhead_just_asked_for() {
  Harness h;
  // A click on the ruler at frame 25 asks the Director to seek; until it
  // answers, the timeline still reports 50. A key struck in that gap acts on
  // the frame the operator clicked.
  const editor::TimelinePanelFrame sought = h.click(445.0F, 45.0F);
  UI_CHECK(sought.seek_to_frame == std::optional<std::uint64_t>{25U});
  UI_CHECK(h.type("q").marker_edits.front().marker.frame == 25);
  UI_CHECK(h.type("i").range == (editor::RangeRequest{25U, 100U, false}));
  // Once the playhead arrives the request is spent, and keys follow the
  // playhead wherever it goes next.
  h.timeline.show_frame = 25U;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.timeline.show_frame = 60U;
  UI_CHECK(h.type("q").marker_edits.front().marker.frame == 60);
}

// Rows opening above a key drag - a selection elsewhere revealing a group's
// children - must neither drop the drag nor turn it into a scrub. The drag
// is keyed by the row it began on, not by where that row now sits.
void expanding_rows_above_a_key_drag_keeps_the_drag() {
  Harness h;
  editor::ElementInfo group{};
  group.node = doc::NodeId{700U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Rig";
  group.group_order = 10;
  editor::ElementInfo child = make_object();
  child.node = doc::NodeId{701U};
  child.display_name = "Rig light";
  child.parent_group = group.node;
  h.elements.push_back(group);
  h.elements.push_back(child);
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  // The collapsed group heads the stack, so the object is the second row:
  // open it and its Position group, putting Position X at y=148..172.
  static_cast<void>(h.click(42.0F, 112.0F));
  static_cast<void>(h.click(58.0F, 136.0F));
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  // Grab Position X's frame-10 key and start moving it.
  static_cast<void>(h.frame(334.0F, 160.0F, false));
  static_cast<void>(h.frame(334.0F, 160.0F, true));
  static_cast<void>(h.frame(352.0F, 160.0F, true));

  // Selecting the group's child elsewhere opens the group above: its
  // Properties foldout and the child push the dragged row down two rows.
  h.selection = {child.node};
  h.primary = child.node;
  const editor::TimelinePanelFrame shifted = h.frame(371.0F, 208.0F, true);
  UI_CHECK(!shifted.seek_to_frame.has_value());
  UI_CHECK(!shifted.selection.has_value());

  const editor::TimelinePanelFrame released = h.frame(371.0F, 208.0F, false);
  UI_CHECK(released.key_batch.has_value());
  UI_CHECK(released.key_batch->action == editor::TimelineKeyBatchAction::Move);
  UI_CHECK(released.key_batch->delta == 5);
  UI_CHECK(released.key_batch->keys ==
           (std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10}}));
  UI_CHECK(!released.seek_to_frame.has_value());
}

// A row's press belongs to that row. When a layer arrives above it - a live
// update between press and release - the release must not click whichever
// row now sits under the pointer.
void a_row_press_released_after_rows_shift_clicks_no_other_row() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  static_cast<void>(h.frame(150.0F, 88.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(150.0F, 88.0F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(pressed.selection->node == object_node);

  editor::ElementInfo arrived = make_object();
  arrived.node = doc::NodeId{702U};
  arrived.display_name = "Arrived";
  arrived.group_order = 10;
  h.elements.push_back(arrived);
  const editor::TimelinePanelFrame released = h.frame(150.0F, 88.0F, false);
  UI_CHECK(!released.selection.has_value() ||
           released.selection->node == object_node);
}

// Drags the clip row from (from_x, y) to (to_x, y) with snapping held off
// and returns the edit it reports on release.
[[nodiscard]] std::optional<scene::ClipTiming>
clip_drag(Harness &h, float from_x, float to_x, float y) {
  static_cast<void>(h.frame(from_x, y, false));
  static_cast<void>(h.frame(from_x, y, true, gui::KeyMods::Alt));
  static_cast<void>(h.frame(to_x, y, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame dropped =
      h.frame(to_x, y, false, gui::KeyMods::Alt);
  if (dropped.clip_edits.size() != 1U) {
    return std::nullopt;
  }
  return dropped.clip_edits.front().timing;
}

// A three-frame clip is 22 units wide, so its fade knobs and trim edges
// overlap. The knobs own the bar's top corners; below them each end trims,
// and the middle moves.
void a_fade_knob_on_a_short_clip_owns_its_top_corner() {
  const auto drag = [](float from_x, float to_x, float y) {
    Harness h;
    h.elements[1].timing = scene::ClipTiming{20, 3, 0, 0, 0};
    return clip_drag(h, from_x, to_x, y);
  };
  // The bar runs x=408..430.2 on y=103..121, knobs centred on its top edge.
  const auto fade_in = drag(409.0F, 424.0F, 105.0F);
  UI_CHECK(fade_in.has_value());
  UI_CHECK(fade_in->start == 20 && fade_in->duration == 3);
  UI_CHECK(fade_in->fade_in > 0);
  const auto fade_out = drag(429.0F, 415.0F, 105.0F);
  UI_CHECK(fade_out.has_value());
  UI_CHECK(fade_out->start == 20 && fade_out->duration == 3);
  UI_CHECK(fade_out->fade_out > 0);
  const auto trimmed = drag(409.0F, 416.0F, 117.0F);
  UI_CHECK(trimmed.has_value());
  UI_CHECK(trimmed->start == 21 && trimmed->duration == 2);
  UI_CHECK(trimmed->fade_in == 0);
  const auto moved = drag(419.0F, 433.0F, 117.0F);
  UI_CHECK(moved.has_value());
  UI_CHECK(moved->start == 22 && moved->duration == 3);
}

// A two-frame clip is under 15 units wide. Each end trims from just
// outside the bar as well as on it, the nearer edge wins inside it, and the
// gap between the edges still moves the clip.
void two_frame_clips_trim_from_either_side_and_from_outside() {
  const auto drag = [](float from_x, float to_x) {
    Harness h;
    h.elements[1].timing = scene::ClipTiming{20, 2, 0, 0, 0};
    return clip_drag(h, from_x, to_x, 116.0F);
  };
  // The bar runs x=408..422.8.
  const auto head = drag(404.0F, 412.0F);
  UI_CHECK(head.has_value());
  UI_CHECK(head->start == 21 && head->duration == 1);
  const auto tail = drag(426.0F, 441.0F);
  UI_CHECK(tail.has_value());
  UI_CHECK(tail->start == 20 && tail->duration == 4);
  const auto moved = drag(415.0F, 430.0F);
  UI_CHECK(moved.has_value());
  UI_CHECK(moved->start == 22 && moved->duration == 2);
}

// A clip's Opacity key directly under its head trim edge: the key row's
// press takes the key and the clip row's press trims, each in its own row.
void a_key_under_a_trim_edge_and_the_edge_each_keep_their_press() {
  Harness h;
  h.keys.push_back(scene::ChannelKeyframes{
      clip_node, scene::PropertyChannel::Opacity, {20, 40}, {1.0, 0.5}});
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  // Open the clip: Position, Rotation and Scale groups, then Opacity at
  // y=196..220. Its frame-20 key sits at x=408, under the bar's head.
  static_cast<void>(h.click(42.0F, 112.0F));
  static_cast<void>(h.frame(411.0F, 208.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(411.0F, 208.0F, true);
  UI_CHECK(pressed.clip_edits.empty());
  UI_CHECK(!pressed.seek_to_frame.has_value());
  UI_CHECK(h.panel.selected_key_count() == 1U);
  static_cast<void>(h.frame(411.0F, 208.0F, false));

  const auto trimmed = clip_drag(h, 405.0F, 449.0F, 116.0F);
  UI_CHECK(trimmed.has_value());
  UI_CHECK(trimmed->start == 26 && trimmed->duration == 34);
}

// The In and Out handles answer down the ruler's full height, not only on
// the range bar along its foot.
void range_handles_answer_down_the_full_ruler() {
  Harness h;
  static_cast<void>(h.frame(262.0F, 38.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(262.0F, 38.0F, true);
  UI_CHECK(!pressed.seek_to_frame.has_value());
  static_cast<void>(h.frame(334.0F, 38.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame released =
      h.frame(334.0F, 38.0F, false, gui::KeyMods::Alt);
  UI_CHECK(released.range.has_value());
  UI_CHECK(released.range->first == 10U && released.range->last == 100U);
}

// A marker's tag answers ten units left of its line.
void a_marker_answers_ten_units_from_its_line() {
  Harness h;
  h.markers = {timeline_marker(60U, 20, scene::CueAction::None, "Intro")};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  static_cast<void>(h.click(399.0F, 66.0F));
  UI_CHECK(h.panel.selected_markers().size() == 1U);
  UI_CHECK(h.panel.selected_markers().front() == doc::NodeId{60U});
}

// The dope sheet and the Curves graph share one key selection: keys chosen
// in either stay chosen across the switch, and Delete after it removes what
// was chosen before it.
void key_selection_carries_across_the_curves_switch() {
  Harness h;
  h.keys[0].values = {1.0, 2.0, 3.0};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  // Dope sheet: Position X's frame-10 key at (334, 136).
  static_cast<void>(h.click(334.0F, 136.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  static_cast<void>(h.click(608.0F, 17.0F));
  UI_CHECK(h.panel.curves_mode());
  UI_CHECK(h.panel.selected_key_count() == 1U);
  // On the graph, fitted to [0.8, 3.2] over y=76..400, add the frame-50 key,
  // value 3, at (630, 103).
  static_cast<void>(h.click(630.0F, 103.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);
  static_cast<void>(h.click(608.0F, 17.0F));
  UI_CHECK(!h.panel.curves_mode());
  UI_CHECK(h.panel.selected_key_count() == 2U);
  const editor::TimelinePanelFrame deleted = h.key(gui::Key::Delete);
  UI_CHECK(deleted.key_batch.has_value());
  UI_CHECK(deleted.key_batch->action == editor::TimelineKeyBatchAction::Delete);
  UI_CHECK(deleted.key_batch->keys ==
           (std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10},
               {object_node, scene::PropertyChannel::PositionX, 50}}));
}

// The dope sheet and the Curves graph read time from one axis, so a zoom in
// either places keys where the other expects them.
void the_dope_sheet_and_the_graph_share_one_time_axis() {
  Harness h;
  h.keys[0].values = {1.0, 2.0, 3.0};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();
  // One wheel notch at x=500 in the dope sheet: 7.4 px a frame becomes
  // 8.51 and frame 32.43 stays at x=500, so frame 20 moves from x=408 to
  // 500 - 12.43 * 8.51 = 394.2.
  static_cast<void>(h.frame(500.0F, 136.0F, false, gui::KeyMods::None, 1.0F));
  static_cast<void>(h.click(608.0F, 17.0F));
  // The graph draws the frame-20 key, value 2, there: at y=238, not at the
  // unzoomed x=408, 14 units away.
  static_cast<void>(h.click(394.0F, 238.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  const editor::TimelinePanelFrame twenty = h.key(gui::Key::Delete);
  UI_CHECK(twenty.key_batch.has_value());
  UI_CHECK(twenty.key_batch->keys ==
           (std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 20}}));

  // Another notch, on the graph about frame 20: 9.79 px a frame, so back in
  // the dope sheet frame 10 sits at 394.2 - 10 * 9.79 = 296.3.
  static_cast<void>(h.frame(394.2F, 238.0F, false, gui::KeyMods::None, 1.0F));
  static_cast<void>(h.click(608.0F, 17.0F));
  UI_CHECK(!h.panel.curves_mode());
  static_cast<void>(h.click(296.0F, 136.0F));
  const editor::TimelinePanelFrame ten = h.key(gui::Key::Delete);
  UI_CHECK(ten.key_batch.has_value());
  UI_CHECK(ten.key_batch->keys ==
           (std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10}}));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 103U> cases{{
    {"tool cursors follow selection and captured gestures",
     &tool_cursors_follow_selection_and_captured_gestures},
    {"extreme imported timing is refused visibly and cannot overflow snapping",
     &extreme_imported_timing_is_refused_visibly_and_cannot_overflow_snapping},
    {"clips move across zero and past Out without trimming source",
     &clips_move_across_zero_and_past_out_without_trimming_the_source},
    {"negative workspace supports picking and signed razor cuts",
     &negative_workspace_supports_clip_picking_and_signed_razor_cuts},
    {"head trim converts source rate and wraps before zero",
     &head_trim_converts_source_rate_and_wraps_before_zero},
    {"Slip wraps looping sources and keeps feedback after release",
     &slip_wraps_looping_sources_and_keeps_feedback_after_release},
    {"Slip explains invalid metadata and single frame media",
     &slip_explains_invalid_source_metadata_and_single_frame_media},
    {"looping filmstrip preview changes source and stays bounded",
     &looping_filmstrip_preview_changes_source_and_stays_bounded},
    {"timeline camera keys preserve cubic evaluated values",
     &timeline_camera_keys_preserve_cubic_evaluated_values},
    {"clip and key releases ignore later uncaptured motion",
     &clip_and_key_releases_ignore_later_uncaptured_motion},
    {"batched clip gestures keep the original press target and anchor",
     &batched_clip_gestures_keep_the_original_press_target_and_anchor},
    {"fade handles accept the visible outer knobs", &fade_handles_accept_the_visible_outer_knobs},
    {"clip release uses final pointer and waits for authority", &clip_release_uses_the_final_pointer_and_waits_for_authority},
    {"clips move before In and workspace pans past Out", &clips_can_move_before_in_and_the_workspace_pans_past_out},
    {"auto key toggle and camera lens rows", &auto_key_is_an_explicit_toggle_and_camera_lens_rows_are_keyable},
    {"light rows key intensity colour and position",
     &light_rows_key_intensity_colour_and_position},
    {"a layer renames in place", &a_layer_renames_in_place},
    {"linked models fold into their collection and object hierarchy",
     &linked_models_fold_into_their_collection_and_object_hierarchy},
    {"parent properties fold separately and key the parent",
     &parent_properties_fold_separately_and_key_the_parent},
    {"hierarchy search reveals matching parts with parent context",
     &hierarchy_search_reveals_matching_parts_with_parent_context},
    {"selecting a hidden part reveals ancestors without forcing them open",
     &selecting_a_hidden_part_reveals_ancestors_without_forcing_them_open},
    {"live hierarchy updates preserve folds and project reset clears them",
     &live_hierarchy_updates_preserve_folds_and_project_reset_clears_them},
    {"authored groups keep sibling order and reorder only among peers",
     &authored_groups_keep_sibling_order_and_reorder_only_among_peers},
    {"environment rows key scene values and preserve animation",
     &environment_rows_key_scene_values_and_preserve_animation},
    {"transport controls report their action",
     &transport_controls_report_their_action},
    {"dragging a clip body moves the placement",
     &dragging_a_clip_body_moves_the_placement},
    {"dragging a clip edge trims without moving the rest",
     &dragging_a_clip_edge_trims_without_moving_the_rest},
    {"a clip fade handle sets the head ramp",
     &a_clip_fade_handle_sets_the_head_ramp},
    {"a clip move snaps to the playhead", &a_clip_move_snaps_to_the_playhead},
    {"pointer tools act on the clip they are pressed on",
     &pointer_tools_act_on_the_clip_they_are_pressed_on},
    {"tool keys choose the tool an editor expects",
     &tool_keys_choose_the_tool_an_editor_expects},
    {"the razor cuts where it lands and Shift cuts every track",
     &the_razor_cuts_where_it_lands_and_shift_cuts_every_track},
    {"clip menus offer only what applies to the clip",
     &clip_menus_offer_only_what_applies_to_the_clip},
    {"the split chord and comma and period edit at the playhead",
     &the_split_chord_and_comma_and_period_edit_at_the_playhead},
    {"markers and cues are added at the playhead and where double-clicked",
     &markers_and_cues_are_added_at_the_playhead_and_where_double_clicked},
    {"a marker is chosen, moved, renamed, and deleted on the strip",
     &a_marker_is_chosen_moved_renamed_and_deleted_on_the_strip},
    {"marker and ruler menus convert, recolour, and add",
     &marker_and_ruler_menus_convert_recolour_and_add},
    {"GO and the arm follow the cues and their status",
     &go_and_the_arm_follow_the_cues_and_their_status},
    {"a clip move snaps to a marker", &a_clip_move_snaps_to_a_marker},
    {"keys act on the playhead just asked for",
     &keys_act_on_the_playhead_just_asked_for},
    {"arrows, Home, End, and Up/Down navigate time",
     &arrows_home_end_and_up_down_navigate_time},
    {"the playhead readout takes typed timecode and frames",
     &the_playhead_readout_takes_typed_timecode_and_frames},
    {"timecode reads and writes the forms an operator types",
     &timecode_reads_and_writes_the_forms_an_operator_types},
    {"track rows and toolbar emit typed lane actions",
     &track_rows_and_toolbar_emit_typed_lane_actions},
    {"every listed element carries its own mute and lock",
     &every_listed_element_carries_its_own_mute_and_lock},
    {"dragging a track header reorders the stack",
     &dragging_a_track_header_reorders_the_stack},
    {"a track drop crosses multiple bands in one transaction",
     &a_track_drop_crosses_multiple_bands_in_one_transaction},
    {"dragging a clip across rows moves it between tracks",
     &dragging_a_clip_across_rows_moves_it_between_tracks},
    {"dragging a media row moves it without hunting for its bar",
     &dragging_a_media_row_moves_it_without_hunting_for_its_bar},
    {"dragging object and model rows reorders the layer stack",
     &dragging_object_and_model_rows_reorders_the_layer_stack},
    {"dragging a layer out reports one screen space drop",
     &dragging_a_layer_out_reports_one_screen_space_drop},
    {"dragging a track out reports one screen space drop",
     &dragging_a_track_out_reports_one_screen_space_drop},
    {"dragging selected layers preserves the group in timeline order",
     &dragging_selected_layers_preserves_the_group_in_timeline_order},
    {"layer drags exclude locked selection and reset the payload",
     &layer_drags_exclude_locked_selection_and_reset_the_payload},
    {"modifier layer clicks select once per gesture",
     &modifier_layer_clicks_select_once_per_gesture},
    {"dragging selected media outside the timeline does not reassign tracks",
     &dragging_selected_media_outside_the_timeline_does_not_reassign_tracks},
    {"track headers support group selection and group drops",
     &track_headers_support_group_selection_and_group_drops},
    {"dragging clip rows reorders clips inside one track",
     &dragging_clip_rows_reorders_clips_inside_one_track},
    {"an explicit track does not steal loose clip ordering",
     &an_explicit_track_does_not_steal_loose_clip_ordering},
    {"a locked track refuses a clip, a drop outside leaves every track",
     &a_locked_track_refuses_a_clip_and_a_drop_outside_leaves_every_track},
    {"filmstrips request exact visible keys and do not retry failures",
     &filmstrips_request_exact_visible_keys_and_do_not_retry_failures},
    {"waveforms request exact keys and draw only valid ready envelopes",
     &waveforms_request_exact_keys_and_draw_only_valid_ready_envelopes},
    {"the ruler scrubs and clamps to the show range",
     &the_ruler_scrubs_and_clamps_to_the_show_range},
    {"a layer row selects and Ctrl click toggles exactly",
     &a_layer_row_selects_and_ctrl_click_toggles_exactly},
    {"a Shift layer click carries only visible top-level rows",
     &a_shift_layer_click_carries_only_visible_top_level_rows},
    {"physical surface rows author position and yaw keys",
     &physical_surface_rows_author_position_and_yaw_keys},
    {"a surface row carries its own lock", &a_surface_row_carries_its_own_lock},
    {"surface transform edits key only animated components",
     &surface_transform_edits_key_only_animated_components},
    {"timeline search filters scene layers and surfaces",
     &timeline_search_filters_scene_layers_and_surfaces},
    {"clear emits every layer channel", &clear_emits_every_layer_channel},
    {"clear routes saturation to its first-class effect node",
     &clear_routes_saturation_to_its_first_class_effect_node},
    {"spatial property rows key typed effect nodes",
     &spatial_property_rows_key_typed_effect_nodes},
    {"range controls report loop and exact bounds",
     &range_controls_report_loop_and_exact_bounds},
    {"I and O set the range from the show frame and preserve loop",
     &i_and_o_set_the_range_from_the_show_frame_and_preserve_loop},
    {"I and O keep stale playheads in a valid range",
     &i_and_o_keep_stale_playheads_in_a_valid_range},
    {"the range handles on the ruler drag In and Out",
     &the_range_handles_on_the_ruler_drag_in_and_out},
    {"show rate menu reports exact common rates",
     &show_rate_menu_reports_exact_common_rates},
    {"a key drag retimes once on release", &a_key_drag_retimes_once_on_release},
    {"Alt drag duplicates the complete key selection",
     &alt_drag_duplicates_the_complete_key_selection},
    {"focused key selection consumes copy and paste shortcuts",
     &focused_key_selection_consumes_copy_and_paste_shortcuts},
    {"selecting a clip exits the key clipboard domain",
     &selecting_a_clip_exits_the_key_clipboard_domain},
    {"modifier clicks build and trim a key selection",
     &modifier_clicks_build_and_trim_a_key_selection},
    {"property diamonds key groups and individual channels",
     &property_diamonds_key_groups_and_individual_channels},
    {"a marquee selects keys and the group retimes together",
     &a_marquee_selects_keys_and_the_group_retimes_together},
    {"key drags snap to nearby channel keys",
     &key_drags_snap_to_nearby_channel_keys},
    {"delete erases selected keys not the whole channel",
     &delete_erases_selected_keys_not_the_whole_channel},
    {"lasso selection preserves add and subtract modifiers",
     &lasso_selection_preserves_add_and_subtract_modifiers},
    {"scale requires a matching preview and emits one atomic batch",
     &scale_requires_a_matching_preview_and_emits_one_atomic_batch},
    {"curve tools fit navigate hide and author the active channel",
     &curve_tools_fit_navigate_hide_and_author_the_active_channel},
    {"middle mouse pans time without seeking",
     &middle_mouse_pans_time_without_seeking},
    {"clicking an empty lane clears layers and scrubs",
     &clicking_an_empty_lane_clears_layers_and_scrubs},
    {"a project replacement clears stale keys and refits the new range",
     &a_project_replacement_clears_stale_keys_and_refits_the_new_range},
    {"a scene switch clears only the typed key context",
     &a_scene_switch_clears_only_the_typed_key_context},
    {"a panel with no room reports nothing",
     &a_panel_with_no_room_reports_nothing},
    {"expanding rows above a key drag keeps the drag",
     &expanding_rows_above_a_key_drag_keeps_the_drag},
    {"a row press released after rows shift clicks no other row",
     &a_row_press_released_after_rows_shift_clicks_no_other_row},
    {"a fade knob on a short clip owns its top corner",
     &a_fade_knob_on_a_short_clip_owns_its_top_corner},
    {"two-frame clips trim from either side and from outside",
     &two_frame_clips_trim_from_either_side_and_from_outside},
    {"a key under a trim edge and the edge each keep their press",
     &a_key_under_a_trim_edge_and_the_edge_each_keep_their_press},
    {"range handles answer down the full ruler",
     &range_handles_answer_down_the_full_ruler},
    {"a marker answers ten units from its line",
     &a_marker_answers_ten_units_from_its_line},
    {"key selection carries across the Curves switch",
     &key_selection_carries_across_the_curves_switch},
    {"the dope sheet and the graph share one time axis",
     &the_dope_sheet_and_the_graph_share_one_time_axis},
}};

} // namespace

#if defined(MEDIASERVER_TIMELINE_BENCH)
namespace {

// Times TimelinePanel::draw() on a large, fully expanded scene: `layers`
// top-level objects and clips, each keyed on Position, Opacity and an effect,
// each twirled open with its Position group open too. Reports the median and
// fastest per-frame time over batches of idle frames and of frames with the
// pointer sweeping the lanes, in the panel's typical 1920x1080 bounds -
// in the dope sheet, or with `curves` on the Curves graph.
int bench(std::size_t layers, bool curves) {
  Harness h;
  h.timeline = editor::EditorTimeline{600U, 600U, 0U, 7'200U, false, false,
                                      false};
  h.elements.clear();
  h.keys.clear();
  for (std::size_t index = 0U; index < layers; ++index) {
    editor::ElementInfo element =
        index % 4U == 3U ? make_clip() : make_object();
    element.node = doc::NodeId{10'000U + index};
    element.display_name = "Layer " + std::to_string(index);
    if (element.kind == editor::ElementKind::Media) {
      element.timing = scene::ClipTiming{
          static_cast<std::int64_t>(index * 37U % 6'000U), 900, 0, 24, 24};
    }
    h.elements.push_back(element);
    for (const scene::PropertyChannel channel :
         {scene::PropertyChannel::PositionX, scene::PropertyChannel::PositionY,
          scene::PropertyChannel::PositionZ, scene::PropertyChannel::Opacity}) {
      scene::ChannelKeyframes keys{element.node, channel, {}, {}, {}};
      for (std::int64_t key = 0; key < 12; ++key) {
        keys.frames.push_back(key * 600 + static_cast<std::int64_t>(index % 50U));
        keys.values.push_back(static_cast<double>((key * 7 + 3) % 11));
        keys.interpolations.push_back(
            static_cast<scene::KeyInterpolation>(key % 3));
      }
      keys.tangents.assign(keys.frames.size(), std::nullopt);
      for (std::size_t key = 0U; key < keys.frames.size(); ++key) {
        if (keys.interpolations[key] == scene::KeyInterpolation::Cubic) {
          keys.tangents[key] = scene::KeyTangent{};
        }
      }
      h.keys.push_back(std::move(keys));
    }
    h.effects.push_back(scene::EffectInstance{
        doc::NodeId{90'000U + index}, element.node,
        scene::EffectKind::Saturation, 0, true, 1.5F});
  }
  // The placeholder font draws one quad per character whose u names it, so
  // counting 'P' quads at the top of the stack shows the Position rows open.
  const auto position_rows = [&] {
    std::size_t count = 0U;
    const auto &vertices = h.list.vertices();
    for (std::size_t index = 0U; index + 5U < vertices.size(); index += 6U) {
      if (vertices[index].u == static_cast<float>('P') / 128.0F &&
          vertices[index].x < 250.0F) {
        ++count;
      }
    }
    return count;
  };
  h.bounds = gui::Rect{0.0F, 0.0F, 1920.0F, 1080.0F};
  static_cast<void>(h.frame(1200.0F, 600.0F, false));
  const std::size_t collapsed_vertices = h.list.vertices().size();
  const std::size_t collapsed_p = position_rows();
  // Every row on screen while twirling, bottom-up so the rows above a click
  // never move: open the layer, then its Position group.
  h.bounds = gui::Rect{0.0F, 0.0F, 1920.0F,
                       76.0F + 24.0F * static_cast<float>(layers * 12U)};
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  for (std::size_t index = layers; index-- > 0U;) {
    const float y = 88.0F + 24.0F * static_cast<float>(index);
    static_cast<void>(h.click(42.0F, y));
    static_cast<void>(h.click(58.0F, y + 24.0F));
  }
  h.bounds = gui::Rect{0.0F, 0.0F, 1920.0F, 1080.0F};
  static_cast<void>(h.frame(1200.0F, 600.0F, false));
  std::printf("'P' glyphs in the list: %zu collapsed, %zu expanded\n",
              collapsed_p, position_rows());
  // Scroll into the middle of the stack.
  for (int notch = 0; notch < 200; ++notch) {
    static_cast<void>(
        h.frame(200.0F, 600.0F, false, gui::KeyMods::None, -1.0F));
  }
  static_cast<void>(h.frame(1200.0F, 600.0F, false));

  const auto measure = [&](const char *label, auto &&pointer) {
    using clock = std::chrono::steady_clock;
    for (int warm = 0; warm < 30; ++warm) pointer(warm);
    std::vector<double> batches;
    constexpr int frames_per_batch = 100;
    for (int batch = 0; batch < 9; ++batch) {
      const auto begin = clock::now();
      for (int frame = 0; frame < frames_per_batch; ++frame) pointer(frame);
      const std::chrono::duration<double, std::milli> elapsed =
          clock::now() - begin;
      batches.push_back(elapsed.count() / frames_per_batch);
    }
    std::sort(batches.begin(), batches.end());
    std::printf("%-28s median %.3f ms/frame  fastest %.3f ms/frame\n", label,
                batches[batches.size() / 2U], batches.front());
  };
  if (curves) {
    // The Curves toggle on the toolbar.
    static_cast<void>(h.click(608.0F, 17.0F));
    UI_CHECK(h.panel.curves_mode());
    static_cast<void>(h.frame(1200.0F, 600.0F, false));
  }
  std::printf("%zu layers%s; %zu vertices per frame collapsed, %zu now\n",
              layers, curves ? " in Curves" : "", collapsed_vertices,
              h.list.vertices().size());
  measure("idle", [&](int) { static_cast<void>(h.frame(1200.0F, 600.0F, false)); });
  measure("pointer sweeping lanes", [&](int frame) {
    static_cast<void>(h.frame(400.0F + static_cast<float>(frame % 100) * 14.0F,
                              300.0F + static_cast<float>(frame % 37) * 18.0F,
                              false));
  });
  return 0;
}

} // namespace

// mediaserver_editor_timeline_bench [layers] [curves]
int main(int argc, char **argv) {
  // The behaviour cases share this file; the benchmark does not run them.
  static_cast<void>(cases);
  return bench(argc >= 2 ? std::stoul(argv[1]) : 400U,
               argc >= 3 && std::string_view{argv[2]} == "curves");
}
#else
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
#endif
