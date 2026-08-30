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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
// x=408..704 on the second layer row, y=81..97.
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
  gui::FontAtlas font{gui::FontAtlas::empty()};
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

  gui::Rect bounds{0.0F, 0.0F, 1000.0F, 400.0F};

  editor::TimelinePanelFrame frame(float x, float y, bool down,
                                   gui::KeyMods mods = gui::KeyMods::None,
                                   float wheel = 0.0F,
                                   std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mods(mods);
    if (wheel != 0.0F) {
      input.add_wheel(wheel);
    }
    if (!text.empty()) {
      input.add_text(text);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, timeline, keys, elements, selection, primary,
                      effects, surfaces, selected_surface, lanes,
                      clipboard_span, key_scale_preview, filmstrips, waveforms);
  }

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
    return panel.draw(ctx, bounds, timeline, keys, elements, selection, primary,
                      effects, surfaces, selected_surface, lanes,
                      clipboard_span, key_scale_preview, filmstrips, waveforms);
  }

  editor::TimelinePanelFrame middle(float x, float y, bool down,
                                    gui::KeyMods mods = gui::KeyMods::None) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, down);
    input.set_mods(mods);
    ctx.new_frame();
    return panel.draw(ctx, bounds, timeline, keys, elements, selection, primary,
                      effects, surfaces, selected_surface, lanes,
                      clipboard_span, key_scale_preview, filmstrips, waveforms);
  }

  void open_position_components() {
    // Layer row is y=56..78. Its twirl is left of the layer name.
    static_cast<void>(click(10.0F, 67.0F));
    // Position becomes the next row, y=78..100; indent puts its twirl at
    // x=26.
    static_cast<void>(click(25.0F, 89.0F));
    static_cast<void>(frame(0.0F, 0.0F, false));
  }
};

void transport_controls_report_their_action() {
  Harness h;
  // The six compact transport buttons begin at x=6. PLAY occupies 90..128.
  const editor::TimelinePanelFrame played = h.click(105.0F, 17.0F);
  UI_CHECK(played.transport == editor::TransportRequest::TogglePlay);

  const editor::TimelinePanelFrame start = h.click(16.0F, 17.0F);
  UI_CHECK(start.transport == editor::TransportRequest::GoToStart);
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
  const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == object_node);
  UI_CHECK(selected.selection->action == editor::SelectionAction::Replace);

  const editor::TimelinePanelFrame toggled =
      h.click(80.0F, 67.0F, gui::KeyMods::Ctrl);
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
      h.click(80.0F, 89.0F, gui::KeyMods::Shift);
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

  const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == surface_node);
  UI_CHECK(selected.selection->surface);

  // Surface layer y=56..78. Open it to reveal Position then Rotation.
  static_cast<void>(h.click(10.0F, 67.0F));
  const editor::TimelinePanelFrame position = h.click(248.0F, 89.0F);
  UI_CHECK(position.keys.size() == 3U);
  UI_CHECK(position.keys[0].target == surface_node);
  UI_CHECK(position.keys[0].component == scene::PropertyChannel::PositionX);
  UI_CHECK(position.keys[0].value == 4.0);
  UI_CHECK(position.keys[1].component == scene::PropertyChannel::PositionY);
  UI_CHECK(position.keys[1].value == 5.0);
  UI_CHECK(position.keys[2].component == scene::PropertyChannel::PositionZ);
  UI_CHECK(position.keys[2].value == 6.0);

  // A physical surface exposes only the horizontal Rotation/Yaw channel.
  const editor::TimelinePanelFrame yaw = h.click(248.0F, 111.0F);
  UI_CHECK(yaw.keys.size() == 1U);
  UI_CHECK(yaw.keys[0].target == surface_node);
  UI_CHECK(yaw.keys[0].component == scene::PropertyChannel::RotationYaw);
  UI_CHECK(std::abs(yaw.keys[0].value - 0.25) < 1.0e-6);
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
  const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == surface_node);
  UI_CHECK(selected.selection->surface);
}

void clear_emits_every_layer_channel() {
  Harness h;
  h.bounds.width = 1200.0F;
  h.selection = {object_node};
  h.primary = object_node;
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  // Clear remains distinct after the view tools and the nearby track action.
  // Broad Copy/Paste live in the application Edit menu rather than here.
  const editor::TimelinePanelFrame cleared = h.click(590.0F, 17.0F);
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

  const editor::TimelinePanelFrame cleared = h.click(590.0F, 17.0F);
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
  static_cast<void>(h.click(10.0F, 67.0F));
  const editor::TimelinePanelFrame box_key = h.click(248.0F, 177.0F);
  UI_CHECK(box_key.keys.size() == 1U);
  UI_CHECK(box_key.keys.front().target == box.source);
  UI_CHECK(box_key.keys.front().component ==
           scene::PropertyChannel::BoxBlurRadius);
  UI_CHECK(box_key.keys.front().value == 7.0);

  const editor::TimelinePanelFrame directional_key = h.click(248.0F, 199.0F);
  UI_CHECK(directional_key.keys.size() == 1U);
  UI_CHECK(directional_key.keys.front().target == directional.source);
  UI_CHECK(directional_key.keys.front().component ==
           scene::PropertyChannel::DirectionalBlurLength);
  UI_CHECK(directional_key.keys.front().value == 12.0);

  const editor::TimelinePanelFrame chroma_key = h.click(248.0F, 221.0F);
  UI_CHECK(chroma_key.keys.size() == 1U);
  UI_CHECK(chroma_key.keys.front().target == chroma.source);
  UI_CHECK(chroma_key.keys.front().component ==
           scene::PropertyChannel::ChromaKeyMix);
  UI_CHECK(chroma_key.keys.front().value == 0.75);

  const editor::TimelinePanelFrame luma_key = h.click(248.0F, 243.0F);
  UI_CHECK(luma_key.keys.size() == 1U);
  UI_CHECK(luma_key.keys.front().target == luma.source);
  UI_CHECK(luma_key.keys.front().component ==
           scene::PropertyChannel::LumaKeyMix);
  UI_CHECK(luma_key.keys.front().value == 0.25);
}

void range_controls_report_loop_and_exact_bounds() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));

  // Loop remains right aligned at x=948..1000.
  const editor::TimelinePanelFrame looped = h.click(950.0F, 17.0F);
  UI_CHECK(looped.range.has_value());
  UI_CHECK(looped.range->first == 0U);
  UI_CHECK(looped.range->last == 100U);
  UI_CHECK(looped.range->loop);

  // IN remains an independently usable NumberField at x=718..768, one frame
  // per drag pixel.
  static_cast<void>(h.frame(760.0F, 17.0F, false));
  static_cast<void>(h.frame(760.0F, 17.0F, true));
  const editor::TimelinePanelFrame dragged = h.frame(780.0F, 17.0F, true);
  UI_CHECK(dragged.range.has_value());
  UI_CHECK(dragged.range->first == 20U);
  UI_CHECK(dragged.range->last == 100U);
  UI_CHECK(dragged.editing);

  // OUT remains independently draggable as well.
  Harness out;
  static_cast<void>(out.frame(860.0F, 17.0F, false));
  static_cast<void>(out.frame(860.0F, 17.0F, true));
  const editor::TimelinePanelFrame shortened = out.frame(840.0F, 17.0F, true);
  UI_CHECK(shortened.range.has_value());
  UI_CHECK(shortened.range->first == 0U);
  UI_CHECK(shortened.range->last == 80U);
  UI_CHECK(shortened.editing);
}

void set_range_buttons_use_the_show_frame_and_preserve_loop() {
  Harness set_in;
  set_in.timeline.first_frame = 20U;
  set_in.timeline.last_frame = 80U;
  set_in.timeline.show_frame = 50U;
  set_in.timeline.loop = true;
  const editor::TimelinePanelFrame before_in =
      set_in.frame(804.0F, 17.0F, false);
  const editor::TimelinePanelFrame pressed_in =
      set_in.frame(804.0F, 17.0F, true);
  const editor::TimelinePanelFrame new_in = set_in.frame(804.0F, 17.0F, false);
  const editor::TimelinePanelFrame after_in =
      set_in.frame(804.0F, 17.0F, false);
  UI_CHECK(!before_in.range.has_value());
  UI_CHECK(!pressed_in.range.has_value());
  UI_CHECK(new_in.range == editor::RangeRequest{50U, 80U, true});
  UI_CHECK(!after_in.range.has_value());

  Harness set_out;
  set_out.timeline.first_frame = 20U;
  set_out.timeline.last_frame = 80U;
  set_out.timeline.show_frame = 50U;
  set_out.timeline.loop = true;
  const editor::TimelinePanelFrame before_out =
      set_out.frame(914.0F, 17.0F, false);
  const editor::TimelinePanelFrame pressed_out =
      set_out.frame(914.0F, 17.0F, true);
  const editor::TimelinePanelFrame new_out =
      set_out.frame(914.0F, 17.0F, false);
  const editor::TimelinePanelFrame after_out =
      set_out.frame(914.0F, 17.0F, false);
  UI_CHECK(!before_out.range.has_value());
  UI_CHECK(!pressed_out.range.has_value());
  UI_CHECK(new_out.range == editor::RangeRequest{20U, 50U, true});
  UI_CHECK(!after_out.range.has_value());
}

void set_range_buttons_keep_stale_playheads_in_a_valid_range() {
  Harness late_in;
  late_in.timeline.first_frame = 20U;
  late_in.timeline.last_frame = 80U;
  late_in.timeline.show_frame = 120U;
  const editor::TimelinePanelFrame collapsed_at_in =
      late_in.click(804.0F, 17.0F);
  UI_CHECK(collapsed_at_in.range == editor::RangeRequest{120U, 120U, false});

  Harness early_in;
  early_in.timeline.first_frame = 20U;
  early_in.timeline.last_frame = 80U;
  early_in.timeline.show_frame = 10U;
  const editor::TimelinePanelFrame extended_at_in =
      early_in.click(804.0F, 17.0F);
  UI_CHECK(extended_at_in.range == editor::RangeRequest{10U, 80U, false});

  Harness early_out;
  early_out.timeline.first_frame = 20U;
  early_out.timeline.last_frame = 80U;
  early_out.timeline.show_frame = 10U;
  const editor::TimelinePanelFrame collapsed_at_out =
      early_out.click(914.0F, 17.0F);
  UI_CHECK(collapsed_at_out.range == editor::RangeRequest{10U, 10U, false});

  Harness late_out;
  late_out.timeline.first_frame = 20U;
  late_out.timeline.last_frame = 80U;
  late_out.timeline.show_frame = 120U;
  const editor::TimelinePanelFrame extended_at_out =
      late_out.click(914.0F, 17.0F);
  UI_CHECK(extended_at_out.range == editor::RangeRequest{20U, 120U, false});
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

    const editor::TimelinePanelFrame opened = h.click(670.0F, 17.0F);
    UI_CHECK(!opened.show_rate.has_value());
    UI_CHECK(opened.editing);

    const editor::TimelinePanelFrame selected =
        h.click(670.0F, 46.0F + 21.0F * static_cast<float>(index));
    UI_CHECK(selected.show_rate.has_value());
    UI_CHECK(selected.show_rate.value() == expected[index]);
    UI_CHECK(!selected.seek_to_frame.has_value());
  }

  // Choosing the rate already in force simply dismisses the popup.
  h.timeline.rate_numerator = 24U;
  h.timeline.rate_denominator = 1U;
  static_cast<void>(h.click(670.0F, 17.0F));
  const editor::TimelinePanelFrame unchanged = h.click(670.0F, 67.0F);
  UI_CHECK(!unchanged.show_rate.has_value());
}

void a_key_drag_retimes_once_on_release() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Position X is y=100..122. Frame 10 is 74px into the fitted graph:
  // x=260 + 10*7.4 = 334. Dragging 37px lands exactly at frame 15.
  static_cast<void>(h.frame(334.0F, 111.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(334.0F, 111.0F, true);
  UI_CHECK(pressed.key_moves.empty());

  const editor::TimelinePanelFrame held = h.frame(371.0F, 111.0F, true);
  UI_CHECK(held.key_moves.empty());

  const editor::TimelinePanelFrame released = h.frame(371.0F, 111.0F, false);
  UI_CHECK(released.key_moves.empty());
  UI_CHECK(released.key_batch.has_value());
  UI_CHECK(released.key_batch->action == editor::TimelineKeyBatchAction::Move);
  UI_CHECK(released.key_batch->delta == 5);
  UI_CHECK(released.key_batch->keys ==
           std::vector<editor::TimelineKeyAddress>{
               {object_node, scene::PropertyChannel::PositionX, 10}});

  const editor::TimelinePanelFrame after = h.frame(371.0F, 111.0F, false);
  UI_CHECK(after.key_moves.empty());
  UI_CHECK(!after.key_batch.has_value());
}

void alt_drag_duplicates_the_complete_key_selection() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  static_cast<void>(h.click(334.0F, 111.0F));
  static_cast<void>(h.click(408.0F, 111.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);

  static_cast<void>(h.frame(334.0F, 111.0F, false, gui::KeyMods::Alt));
  static_cast<void>(h.frame(334.0F, 111.0F, true, gui::KeyMods::Alt));
  static_cast<void>(h.frame(371.0F, 111.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame duplicated =
      h.frame(371.0F, 111.0F, false, gui::KeyMods::Alt);

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
  static_cast<void>(h.click(334.0F, 111.0F));

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
  static_cast<void>(h.click(334.0F, 111.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);

  // The media bar owns the same Timeline workspace but a different semantic
  // clipboard. Clicking it clears stale diamond membership before the shell
  // routes the next global chord to clip Copy/Paste.
  static_cast<void>(h.click(334.0F, 221.0F));
  UI_CHECK(h.panel.selected_key_count() == 0U);
  const editor::TimelinePanelFrame copied = h.key(gui::Key::Copy);
  UI_CHECK(!copied.consume_clipboard);
  UI_CHECK(!copied.key_clipboard.has_value());
}

void modifier_clicks_build_and_trim_a_key_selection() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  static_cast<void>(h.click(334.0F, 111.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);
  static_cast<void>(h.click(408.0F, 111.0F, gui::KeyMods::Shift));
  UI_CHECK(h.panel.selected_key_count() == 2U);
  static_cast<void>(h.click(334.0F, 111.0F, gui::KeyMods::Ctrl));
  UI_CHECK(h.panel.selected_key_count() == 1U);
}

void property_diamonds_key_groups_and_individual_channels() {
  Harness group;
  static_cast<void>(group.frame(0.0F, 0.0F, false));
  group.open_position_components();

  // The row-local diamond is at the right edge of the layer column.
  const editor::TimelinePanelFrame group_key = group.click(248.0F, 89.0F);
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
      component.click(248.0F, 111.0F);
  UI_CHECK(component_key.keys.size() == 1U);
  UI_CHECK(component_key.keys[0].component ==
           scene::TransformChannel::PositionX);
  UI_CHECK(component_key.keys[0].value == 1.0);
  UI_CHECK(component_key.keys[0].erase);
}

void a_marquee_selects_keys_and_the_group_retimes_together() {
  Harness h;
  static_cast<void>(h.frame(0.0F, 0.0F, false));
  h.open_position_components();

  // Drag a box around Position X's frame-10 and frame-20 diamonds.
  static_cast<void>(h.frame(320.0F, 105.0F, false));
  static_cast<void>(h.frame(320.0F, 105.0F, true));
  static_cast<void>(h.frame(415.0F, 116.0F, true));
  static_cast<void>(h.frame(415.0F, 116.0F, false));
  UI_CHECK(h.panel.selected_key_count() == 2U);

  // Grabbing either selected diamond moves the complete selection by five.
  static_cast<void>(h.frame(334.0F, 111.0F, false));
  static_cast<void>(h.frame(334.0F, 111.0F, true));
  static_cast<void>(h.frame(371.0F, 111.0F, true));
  const editor::TimelinePanelFrame moved = h.frame(371.0F, 111.0F, false);
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
  static_cast<void>(h.frame(334.0F, 111.0F, false));
  static_cast<void>(h.frame(334.0F, 111.0F, true));
  static_cast<void>(h.frame(440.0F, 111.0F, true));
  const editor::TimelinePanelFrame snapped = h.frame(440.0F, 111.0F, false);
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
  static_cast<void>(h.click(334.0F, 111.0F));

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

  // Key tools live at the right edge of the ruler. Switch the marquee from
  // a box to a freeform lasso, then encircle Position X frames 10 and 20.
  static_cast<void>(h.click(965.0F, 45.0F));
  static_cast<void>(h.click(690.0F, 73.0F));
  UI_CHECK(h.panel.lasso_mode());

  const auto lasso = [&](gui::KeyMods mods) {
    static_cast<void>(h.frame(320.0F, 103.0F, false, mods));
    static_cast<void>(h.frame(320.0F, 103.0F, true, mods));
    static_cast<void>(h.frame(420.0F, 103.0F, true, mods));
    static_cast<void>(h.frame(420.0F, 119.0F, true, mods));
    static_cast<void>(h.frame(320.0F, 119.0F, true, mods));
    static_cast<void>(h.frame(320.0F, 103.0F, false, mods));
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
  static_cast<void>(h.click(334.0F, 111.0F));
  static_cast<void>(h.click(408.0F, 111.0F, gui::KeyMods::Shift));

  static_cast<void>(h.click(965.0F, 45.0F));
  const editor::TimelinePanelFrame opened = h.click(700.0F, 129.0F);
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
  const editor::TimelinePanelFrame stale = h.click(962.0F, 213.0F);
  UI_CHECK(!stale.key_batch.has_value());

  const editor::TimelinePanelFrame playhead_anchor = h.click(870.0F, 157.0F);
  UI_CHECK(playhead_anchor.key_scale_preview.has_value());
  UI_CHECK(playhead_anchor.key_scale_preview->scale.anchor ==
           scene::KeyTimeAnchor::Playhead);
  UI_CHECK(playhead_anchor.key_scale_preview->scale.playhead == 50);
  h.key_scale_preview = editor::TimelineKeyScalePreview{
      playhead_anchor.key_scale_preview->signature, true, 2U, 2U, 1U, 1U};

  const editor::TimelinePanelFrame applied = h.click(962.0F, 213.0F);
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
  static_cast<void>(h.click(470.0F, 17.0F));
  static_cast<void>(h.click(334.0F, 111.0F));
  static_cast<void>(h.click(408.0F, 111.0F, gui::KeyMods::Shift));
  static_cast<void>(h.click(965.0F, 45.0F));

  // Alt/Ctrl wheel address the value axis without changing row scroll. Alt
  // also holds the value under the pointer still, so zooming off the middle of
  // a row shifts the pan rather than sliding the curve out from under it.
  static_cast<void>(h.frame(500.0F, 111.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_zoom() > 1.0F);
  const float anchored_pan = h.panel.curve_value_pan();
  static_cast<void>(h.frame(500.0F, 103.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_pan() != anchored_pan);
  static_cast<void>(h.frame(500.0F, 111.0F, false, gui::KeyMods::Ctrl, 1.0F));
  UI_CHECK(h.panel.curve_value_pan() > 0.0F);

  // FIT ALL is the only view button the popup still carries: every zoom step
  // it used to hold is a wheel gesture over the graph.
  static_cast<void>(h.click(800.0F, 73.0F));
  UI_CHECK(h.panel.curve_value_zoom() == 1.0F);
  UI_CHECK(h.panel.curve_value_pan() == 0.0F);
  static_cast<void>(h.frame(500.0F, 111.0F, false, gui::KeyMods::Alt, 1.0F));
  UI_CHECK(h.panel.curve_value_zoom() > 1.0F);

  static_cast<void>(h.click(750.0F, 73.0F));
  const editor::TimelinePanelFrame fitted = h.click(260.0F, 45.0F);
  UI_CHECK(fitted.seek_to_frame == 9U);
  UI_CHECK(h.panel.curve_value_zoom() == 1.0F);
  UI_CHECK(h.panel.curve_value_pan() == 0.0F);

  // The per-row swatch is a true visibility toggle, not a destructive key
  // edit. Showing it again restores hit testing and its stable channel color.
  static_cast<void>(h.click(225.0F, 111.0F));
  UI_CHECK(
      !h.panel.channel_visible(object_node, scene::PropertyChannel::PositionX));
  static_cast<void>(h.click(225.0F, 111.0F));
  UI_CHECK(
      h.panel.channel_visible(object_node, scene::PropertyChannel::PositionX));

  // Re-selecting a key makes the channel active. The numeric field samples at
  // the playhead and an absolute typed commit authors that exact property at
  // the playhead; target/property are never inferred from another row.
  static_cast<void>(h.click(322.0F, 111.0F));
  static_cast<void>(h.click(965.0F, 45.0F));
  static_cast<void>(h.frame(940.0F, 101.0F, false));
  static_cast<void>(h.frame(940.0F, 101.0F, true));
  static_cast<void>(h.frame(940.0F, 101.0F, false));
  static_cast<void>(
      h.frame(0.0F, 0.0F, false, gui::KeyMods::None, 0.0F, "4.5"));
  const editor::TimelinePanelFrame committed = h.frame(700.0F, 129.0F, true);
  UI_CHECK(committed.keys.size() == 1U);
  UI_CHECK(committed.keys.front().target == object_node);
  UI_CHECK(committed.keys.front().component ==
           scene::PropertyChannel::PositionX);
  UI_CHECK(committed.keys.front().frame == h.timeline.show_frame);
  UI_CHECK(std::fabs(committed.keys.front().value - 4.5) < 1.0e-9);
}

void middle_mouse_pans_time_without_seeking() {
  Harness h;
  static_cast<void>(h.frame(500.0F, 200.0F, false));
  static_cast<void>(h.middle(500.0F, 200.0F, false));
  static_cast<void>(h.middle(500.0F, 200.0F, true));
  const editor::TimelinePanelFrame panned = h.middle(400.0F, 200.0F, true);
  UI_CHECK(!panned.seek_to_frame.has_value());
  static_cast<void>(h.middle(400.0F, 200.0F, false));

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
  const editor::TimelinePanelFrame pressed = h.frame(556.0F, 111.0F, true);
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
  static_cast<void>(h.click(334.0F, 111.0F));
  UI_CHECK(h.panel.selected_key_count() == 1U);

  // Deliberately zoom the old 0..100 project and preserve a user preference.
  static_cast<void>(h.frame(630.0F, 200.0F, false, gui::KeyMods::None, 4.0F));
  static_cast<void>(h.click(370.0F, 17.0F));
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
  static_cast<void>(h.click(334.0F, 111.0F));
  static_cast<void>(h.click(965.0F, 45.0F));
  static_cast<void>(h.click(700.0F, 129.0F));
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
  static_cast<void>(h.frame(500.0F, 90.0F, false));
  // The press claims the bar rather than scrubbing the playhead.
  const editor::TimelinePanelFrame pressed = h.frame(500.0F, 90.0F, true);
  UI_CHECK(!pressed.seek_to_frame.has_value());
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(pressed.selection->node == clip_node);
  UI_CHECK(h.panel.dragging_clip());

  const editor::TimelinePanelFrame moved = h.frame(574.0F, 90.0F, true);
  UI_CHECK(moved.clip_edits.empty());
  UI_CHECK(!moved.seek_to_frame.has_value());

  // The lifted bar previews locally. One placement edit is authored when it is
  // dropped, matching the Feed Mapping gesture and making one drag one edit.
  const editor::TimelinePanelFrame released = h.frame(574.0F, 90.0F, false);
  UI_CHECK(released.clip_edits.size() == 1U);
  UI_CHECK(released.clip_edits.front().clip == clip_node);
  UI_CHECK(released.clip_edits.front().timing ==
           (scene::ClipTiming{30, 40, 0, 0, 0}));
  UI_CHECK(!h.panel.dragging_clip());

  Harness click;
  static_cast<void>(click.frame(500.0F, 90.0F, false));
  static_cast<void>(click.frame(500.0F, 90.0F, true));
  const editor::TimelinePanelFrame below_threshold =
      click.frame(502.0F, 90.0F, true);
  UI_CHECK(below_threshold.clip_edits.empty());
  const editor::TimelinePanelFrame click_release =
      click.frame(502.0F, 90.0F, false);
  UI_CHECK(click_release.clip_edits.empty());
}

void dragging_a_clip_edge_trims_without_moving_the_rest() {
  Harness h;
  static_cast<void>(h.frame(410.0F, 90.0F, false));
  static_cast<void>(h.frame(410.0F, 90.0F, true));
  const editor::TimelinePanelFrame trimmed = h.frame(447.0F, 90.0F, true);
  UI_CHECK(trimmed.clip_edits.empty());
  const editor::TimelinePanelFrame trim_drop = h.frame(447.0F, 90.0F, false);
  // The out point is untouched and the source slides with the in point, so
  // the frames that survive still show what they showed.
  UI_CHECK(trim_drop.clip_edits.size() == 1U);
  UI_CHECK(trim_drop.clip_edits.front().timing ==
           (scene::ClipTiming{25, 35, 5, 0, 0}));

  // The other end changes only the duration.
  static_cast<void>(h.frame(700.0F, 90.0F, false));
  static_cast<void>(h.frame(700.0F, 90.0F, true));
  const editor::TimelinePanelFrame shortened = h.frame(663.0F, 90.0F, true);
  UI_CHECK(shortened.clip_edits.empty());
  const editor::TimelinePanelFrame shorten_drop =
      h.frame(663.0F, 90.0F, false);
  UI_CHECK(shorten_drop.clip_edits.size() == 1U);
  UI_CHECK(shorten_drop.clip_edits.front().timing ==
           (scene::ClipTiming{20, 35, 0, 0, 0}));
}

void a_clip_fade_handle_sets_the_head_ramp() {
  Harness h;
  static_cast<void>(h.frame(409.0F, 83.0F, false));
  static_cast<void>(h.frame(409.0F, 83.0F, true));
  const editor::TimelinePanelFrame faded = h.frame(446.0F, 83.0F, true);
  UI_CHECK(faded.clip_edits.empty());
  const editor::TimelinePanelFrame fade_drop = h.frame(446.0F, 83.0F, false);
  UI_CHECK(fade_drop.clip_edits.size() == 1U);
  UI_CHECK(fade_drop.clip_edits.front().timing ==
           (scene::ClipTiming{20, 40, 0, 5, 0}));
}

void a_clip_move_snaps_to_the_playhead() {
  Harness h;
  // Playhead is frame 50, so a body drag that lands the head two frames
  // short of it is pulled onto the cut.
  static_cast<void>(h.frame(500.0F, 90.0F, false));
  static_cast<void>(h.frame(500.0F, 90.0F, true));
  const editor::TimelinePanelFrame snapped = h.frame(707.0F, 90.0F, true);
  UI_CHECK(snapped.clip_edits.empty());
  const editor::TimelinePanelFrame snap_drop = h.frame(707.0F, 90.0F, false);
  UI_CHECK(snap_drop.clip_edits.size() == 1U);
  UI_CHECK(snap_drop.clip_edits.front().timing.start == 50);
}

void advanced_drag_tools_emit_one_typed_request_on_release() {
  Harness ripple;
  ripple.selection = {clip_node};
  ripple.primary = clip_node;
  static_cast<void>(ripple.click(200.0F, 45.0F));
  static_cast<void>(ripple.click(150.0F, 91.0F));
  UI_CHECK(ripple.panel.edit_tool() == editor::TimelineEditTool::RippleTrim);
  static_cast<void>(ripple.frame(700.0F, 90.0F, false, gui::KeyMods::Alt));
  static_cast<void>(ripple.frame(700.0F, 90.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame ripple_preview =
      ripple.frame(663.0F, 90.0F, true, gui::KeyMods::Alt);
  UI_CHECK(ripple_preview.timeline_edits.empty());
  UI_CHECK(ripple_preview.clip_edits.empty());
  const editor::TimelinePanelFrame ripple_commit =
      ripple.frame(663.0F, 90.0F, false, gui::KeyMods::Alt);
  UI_CHECK(ripple_commit.timeline_edits.size() == 1U);
  const auto *ripple_request = std::get_if<editor::TimelineRippleTrimRequest>(
      &ripple_commit.timeline_edits.front());
  UI_CHECK(ripple_request != nullptr);
  UI_CHECK(ripple_request->clip == clip_node);
  UI_CHECK(ripple_request->edge == scene::TimelineTrimEdge::Tail);
  UI_CHECK(ripple_request->delta == -5);

  Harness slip;
  slip.selection = {clip_node};
  slip.primary = clip_node;
  static_cast<void>(slip.click(200.0F, 45.0F));
  static_cast<void>(slip.click(150.0F, 154.0F));
  UI_CHECK(slip.panel.edit_tool() == editor::TimelineEditTool::Slip);
  static_cast<void>(slip.frame(500.0F, 90.0F, false, gui::KeyMods::Alt));
  static_cast<void>(slip.frame(500.0F, 90.0F, true, gui::KeyMods::Alt));
  static_cast<void>(slip.frame(537.0F, 90.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame slip_commit =
      slip.frame(537.0F, 90.0F, false, gui::KeyMods::Alt);
  UI_CHECK(slip_commit.timeline_edits.size() == 1U);
  const auto *slip_request = std::get_if<editor::TimelineSlipRequest>(
      &slip_commit.timeline_edits.front());
  UI_CHECK(slip_request != nullptr);
  UI_CHECK(slip_request->clip == clip_node);
  UI_CHECK(slip_request->delta == 5);

  Harness roll;
  editor::ElementInfo right = make_clip();
  right.node = doc::NodeId{8U};
  right.timing = scene::ClipTiming{60, 20, 40, 0, 0};
  roll.elements.push_back(right);
  roll.selection = {clip_node, right.node};
  roll.primary = clip_node;
  static_cast<void>(roll.click(200.0F, 45.0F));
  static_cast<void>(roll.click(150.0F, 133.0F));
  UI_CHECK(roll.panel.edit_tool() == editor::TimelineEditTool::Roll);
  static_cast<void>(roll.frame(700.0F, 90.0F, false, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(700.0F, 90.0F, true, gui::KeyMods::Alt));
  static_cast<void>(roll.frame(737.0F, 90.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame roll_commit =
      roll.frame(737.0F, 90.0F, false, gui::KeyMods::Alt);
  UI_CHECK(roll_commit.timeline_edits.size() == 1U);
  const auto *roll_request = std::get_if<editor::TimelineRollRequest>(
      &roll_commit.timeline_edits.front());
  UI_CHECK(roll_request != nullptr);
  UI_CHECK(roll_request->first == clip_node);
  UI_CHECK(roll_request->second == right.node);
  UI_CHECK(roll_request->delta == 5);

  Harness slide;
  editor::ElementInfo predecessor = make_clip();
  predecessor.node = doc::NodeId{6U};
  predecessor.timing = scene::ClipTiming{0, 20, 0, 0, 0};
  editor::ElementInfo successor = make_clip();
  successor.node = doc::NodeId{8U};
  successor.timing = scene::ClipTiming{60, 20, 60, 0, 0};
  slide.elements.insert(slide.elements.begin() + 1, predecessor);
  slide.elements.push_back(successor);
  slide.selection = {clip_node};
  slide.primary = clip_node;
  static_cast<void>(slide.click(200.0F, 45.0F));
  static_cast<void>(slide.click(150.0F, 175.0F));
  UI_CHECK(slide.panel.edit_tool() == editor::TimelineEditTool::Slide);
  static_cast<void>(slide.frame(500.0F, 111.0F, false, gui::KeyMods::Alt));
  static_cast<void>(slide.frame(500.0F, 111.0F, true, gui::KeyMods::Alt));
  static_cast<void>(slide.frame(537.0F, 111.0F, true, gui::KeyMods::Alt));
  const editor::TimelinePanelFrame slide_commit =
      slide.frame(537.0F, 111.0F, false, gui::KeyMods::Alt);
  UI_CHECK(slide_commit.timeline_edits.size() == 1U);
  const auto *slide_request = std::get_if<editor::TimelineSlideRequest>(
      &slide_commit.timeline_edits.front());
  UI_CHECK(slide_request != nullptr);
  UI_CHECK(slide_request->clip == clip_node);
  UI_CHECK(slide_request->delta == 5);
}

void advanced_actions_are_contextual_and_typed() {
  Harness edits;
  edits.selection = {clip_node};
  edits.primary = clip_node;

  static_cast<void>(edits.click(200.0F, 45.0F));
  static_cast<void>(edits.click(150.0F, 112.0F));
  UI_CHECK(edits.panel.edit_tool() == editor::TimelineEditTool::RippleDelete);
  const editor::TimelinePanelFrame erased = edits.click(240.0F, 45.0F);
  UI_CHECK(erased.timeline_edits.size() == 1U);
  const auto *erase_request = std::get_if<editor::TimelineRippleDeleteRequest>(
      &erased.timeline_edits.front());
  UI_CHECK(erase_request != nullptr);
  UI_CHECK(erase_request->clips == std::vector<doc::NodeId>{clip_node});

  edits.clipboard_span = 12;
  static_cast<void>(edits.click(200.0F, 45.0F));
  static_cast<void>(edits.click(150.0F, 196.0F));
  UI_CHECK(edits.panel.edit_tool() == editor::TimelineEditTool::Insert);
  const editor::TimelinePanelFrame inserted = edits.click(240.0F, 45.0F);
  UI_CHECK(inserted.timeline_edits.size() == 1U);
  const auto *insert_request = std::get_if<editor::TimelineInsertRequest>(
      &inserted.timeline_edits.front());
  UI_CHECK(insert_request != nullptr);
  UI_CHECK(insert_request->lane == doc::NodeId{});

  static_cast<void>(edits.click(200.0F, 45.0F));
  static_cast<void>(edits.click(150.0F, 217.0F));
  UI_CHECK(edits.panel.edit_tool() == editor::TimelineEditTool::Overwrite);
  const editor::TimelinePanelFrame overwritten = edits.click(240.0F, 45.0F);
  UI_CHECK(overwritten.timeline_edits.size() == 1U);
  const auto *overwrite_request = std::get_if<editor::TimelineOverwriteRequest>(
      &overwritten.timeline_edits.front());
  UI_CHECK(overwrite_request != nullptr);
  UI_CHECK(overwrite_request->lane == doc::NodeId{});

  Harness disabled;
  static_cast<void>(disabled.click(200.0F, 45.0F));
  static_cast<void>(disabled.click(150.0F, 133.0F));
  UI_CHECK(disabled.panel.edit_tool() == editor::TimelineEditTool::Standard);
}

void splitting_reports_the_selected_clip_at_the_playhead() {
  Harness h;
  h.bounds.width = 1200.0F;
  h.selection.push_back(clip_node);
  h.primary = clip_node;
  // SPLIT follows the view tools and the nearby lane-create action. Broad
  // clipboard actions no longer consume space in this panel toolbar.
  const editor::TimelinePanelFrame split = h.click(585.0F, 17.0F);
  UI_CHECK(split.clip_splits.size() == 1U);
  UI_CHECK(split.clip_splits.front().clip == clip_node);
  UI_CHECK(split.clip_splits.front().frame == 50U);

  // With the playhead outside the clip there is nothing to cut and the
  // button is not offered at all.
  Harness outside;
  outside.bounds.width = 1200.0F;
  outside.selection.push_back(clip_node);
  outside.primary = clip_node;
  outside.timeline.show_frame = 90U;
  const editor::TimelinePanelFrame none = outside.click(585.0F, 17.0F);
  UI_CHECK(none.clip_splits.empty());
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
  const editor::TimelinePanelFrame selected = h.click(80.0F, 89.0F);
  UI_CHECK(selected.selection.has_value());
  UI_CHECK(selected.selection->node == lane_node);
  UI_CHECK(selected.selection->action == editor::SelectionAction::Replace);
  UI_CHECK(!selected.selection->surface);

  // Mute and lock are on the track's own row, at the right edge of the layer
  // list, so the state they report sits beside the clips it applies to. The
  // list column is 260 wide here, which puts them at 214 and 236.
  const editor::TimelinePanelFrame muted = h.click(224.0F, 89.0F);
  UI_CHECK(muted.lane_edits.size() == 1U);
  UI_CHECK(muted.lane_edits.front().action ==
           editor::TimelineLaneAction::ToggleMute);
  UI_CHECK(muted.lane_edits.front().lane == lane_node);

  const editor::TimelinePanelFrame locked = h.click(246.0F, 89.0F);
  UI_CHECK(locked.lane_edits.size() == 1U);
  UI_CHECK(locked.lane_edits.front().action ==
           editor::TimelineLaneAction::ToggleLock);
  UI_CHECK(locked.lane_edits.front().lane == lane_node);

  // Reaching a row toggle is not a row click, so the track stays selected and
  // the toolbar keeps addressing it.
  const editor::TimelinePanelFrame created = h.click(525.0F, 17.0F);
  UI_CHECK(created.lane_edits.size() == 1U);
  UI_CHECK(created.lane_edits.front().action ==
           editor::TimelineLaneAction::Create);

  // What is left in the toolbar is only what needs a confirmed destination.
  // A non-empty track cannot expose the ambiguous empty-track X, so the two
  // destructive choices are explicit: delete its clips, or move them to the
  // named destination shown immediately before MOVE+DELETE.
  const editor::TimelinePanelFrame deleted_clips = h.click(620.0F, 17.0F);
  UI_CHECK(deleted_clips.lane_edits.size() == 1U);
  UI_CHECK(deleted_clips.lane_edits.front().action ==
           editor::TimelineLaneAction::DeleteClips);
  UI_CHECK(deleted_clips.lane_edits.front().lane == lane_node);

  const editor::TimelinePanelFrame moved_clips = h.click(800.0F, 17.0F);
  UI_CHECK(moved_clips.lane_edits.size() == 1U);
  UI_CHECK(moved_clips.lane_edits.front().action ==
           editor::TimelineLaneAction::DeleteMoveClips);
  UI_CHECK(moved_clips.lane_edits.front().destination == destination_lane_node);

  // A second click on the header opens its inline name field and hands it the
  // keyboard; typed changes are reported while the field owns it so the shell
  // can keep the whole text edit in one transaction. Renaming is a
  // double-click because a single click now selects the track, which is the
  // screen it owns and the thing the Inspector shows.
  static_cast<void>(h.click(185.0F, 89.0F));
  const editor::TimelinePanelFrame renamed =
      h.frame(185.0F, 89.0F, false, gui::KeyMods::None, 0.0F, " X");
  UI_CHECK(renamed.lane_editing);
  UI_CHECK(renamed.lane_edits.size() == 1U);
  UI_CHECK(renamed.lane_edits.front().action ==
           editor::TimelineLaneAction::Rename);
  UI_CHECK(renamed.lane_edits.front().lane == lane_node);
  UI_CHECK(renamed.lane_edits.front().name == "Foreground X");
}

void every_listed_element_carries_its_own_mute_and_lock() {
  Harness h;
  // No tracks, so the rows are the plain object and the clip beneath it. The
  // list column is 260 wide, putting the two toggles at 214 and 236.
  const editor::TimelinePanelFrame hidden = h.click(224.0F, 67.0F);
  UI_CHECK(hidden.element_flags.size() == 1U);
  UI_CHECK(hidden.element_flags.front() ==
           (editor::TimelineElementFlagEdit{
               object_node, editor::TimelineElementFlag::Visible, false}));

  const editor::TimelinePanelFrame locked = h.click(246.0F, 67.0F);
  UI_CHECK(locked.element_flags.size() == 1U);
  UI_CHECK(locked.element_flags.front() ==
           (editor::TimelineElementFlagEdit{
               object_node, editor::TimelineElementFlag::Locked, true}));

  // A clip is an element like any other and carries the same pair.
  const editor::TimelinePanelFrame clip_locked = h.click(246.0F, 89.0F);
  UI_CHECK(clip_locked.element_flags.size() == 1U);
  UI_CHECK(clip_locked.element_flags.front().element == clip_node);
  UI_CHECK(clip_locked.element_flags.front().flag ==
           editor::TimelineElementFlag::Locked);

  // The value carried is the state being asked for, not the gesture, so an
  // already-hidden element asks to come back rather than to be hidden again.
  h.elements[0U].visible = false;
  const editor::TimelinePanelFrame shown = h.click(224.0F, 67.0F);
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
  static_cast<void>(h.frame(80.0F, 89.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 89.0F, true);
  UI_CHECK(pressed.lane_edits.empty());

  // Dragging Foreground up onto the Background band previews a later place in
  // the stack, because up the list is toward the front of the picture. Like a
  // Feed Mapping surface drop, hovering authors nothing: the target and the
  // labelled drag ghost are only immediate feedback.
  const editor::TimelinePanelFrame over_background =
      h.frame(1'200.0F, 67.0F, true);
  UI_CHECK(over_background.lane_edits.empty());
  UI_CHECK(over_background.lane_editing);

  // Back over its own band asks for nothing.
  const editor::TimelinePanelFrame home = h.frame(1'200.0F, 89.0F, true);
  UI_CHECK(home.lane_edits.empty());

  // A clip row inside a track counts as that track, so the drop does not have
  // to land on the header itself.
  const editor::TimelinePanelFrame over_clip = h.frame(1'200.0F, 111.0F, true);
  UI_CHECK(over_clip.lane_edits.empty());

  // One resolved edit lands on release even when the pointer is over the time
  // graph rather than the narrow header column.
  const editor::TimelinePanelFrame released =
      h.frame(1'200.0F, 67.0F, false);
  UI_CHECK(released.lane_edits.size() == 1U);
  UI_CHECK(released.lane_edits.front() ==
           editor::TimelineLaneEdit{editor::TimelineLaneAction::MoveTo,
                                    lane_node, {}, destination_lane_node});
  const editor::TimelinePanelFrame after = h.frame(80.0F, 67.0F, false);
  UI_CHECK(after.lane_edits.empty());
  UI_CHECK(!after.lane_editing);

  // A press that never clears the movement threshold remains a click, not an
  // accidental reorder.
  static_cast<void>(h.frame(80.0F, 89.0F, true));
  const editor::TimelinePanelFrame still = h.frame(82.0F, 89.0F, true);
  UI_CHECK(still.lane_edits.empty());
  UI_CHECK(!still.lane_editing);
  const editor::TimelinePanelFrame clicked = h.frame(82.0F, 89.0F, false);
  UI_CHECK(clicked.lane_edits.empty());
  UI_CHECK(clicked.selection.has_value());

  // A locked track advertises the lock instead of a live drag affordance and
  // cannot be lifted into a reorder gesture.
  h.lanes.front().locked = true;
  static_cast<void>(h.frame(80.0F, 89.0F, false));
  static_cast<void>(h.frame(80.0F, 89.0F, true));
  const editor::TimelinePanelFrame locked = h.frame(1'200.0F, 67.0F, true);
  UI_CHECK(locked.lane_edits.empty());
  UI_CHECK(!locked.lane_editing);
  const editor::TimelinePanelFrame locked_release =
      h.frame(1'200.0F, 67.0F, false);
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
  static_cast<void>(h.frame(80.0F, 111.0F, false));
  static_cast<void>(h.frame(80.0F, 111.0F, true));
  const editor::TimelinePanelFrame crossed = h.frame(900.0F, 67.0F, true);
  UI_CHECK(crossed.lane_edits.empty());
  UI_CHECK(crossed.lane_editing);

  const editor::TimelinePanelFrame dropped = h.frame(900.0F, 67.0F, false);
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
  static_cast<void>(h.frame(500.0F, 112.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(500.0F, 112.0F, true);
  UI_CHECK(h.panel.dragging_clip());
  UI_CHECK(pressed.selection.has_value());

  // Straight up into the Background band with no horizontal movement: the
  // placement is untouched and the target previews without authoring.
  const editor::TimelinePanelFrame crossed = h.frame(500.0F, 67.0F, true);
  UI_CHECK(crossed.clip_edits.empty());

  const editor::TimelinePanelFrame dropped = h.frame(500.0F, 67.0F, false);
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
  static_cast<void>(h.frame(80.0F, 112.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 112.0F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(h.panel.dragging_layer());
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 67.0F, true);
  UI_CHECK(preview.clip_edits.empty());

  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 67.0F, false);
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
  static_cast<void>(h.frame(80.0F, 67.0F, false));
  const editor::TimelinePanelFrame pressed = h.frame(80.0F, 67.0F, true);
  UI_CHECK(pressed.selection.has_value());
  UI_CHECK(h.panel.dragging_layer());
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 95.0F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.lane_editing);

  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 95.0F, false);
  UI_CHECK(dropped.layer_reorders ==
           std::vector<editor::TimelineLayerReorder>{{
               object_node, model.node, editor::TimelineLayerPlacement::After}});
  UI_CHECK(!h.panel.dragging_layer());
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
  static_cast<void>(h.frame(80.0F, 89.0F, false));
  static_cast<void>(h.frame(80.0F, 89.0F, true));
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 117.0F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 117.0F, false);
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
  static_cast<void>(h.frame(80.0F, 89.0F, false));
  static_cast<void>(h.frame(80.0F, 89.0F, true));
  const editor::TimelinePanelFrame preview = h.frame(80.0F, 117.0F, true);
  UI_CHECK(preview.layer_reorders.empty());
  UI_CHECK(preview.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(80.0F, 117.0F, false);
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
  static_cast<void>(h.frame(500.0F, 112.0F, false));
  static_cast<void>(h.frame(500.0F, 112.0F, true));
  UI_CHECK(h.panel.dragging_clip());

  // Row zero is the locked Background. A locked track refuses an arriving clip
  // the same way it refuses edits to the ones it already holds.
  const editor::TimelinePanelFrame refused = h.frame(500.0F, 67.0F, true);
  UI_CHECK(refused.clip_edits.empty());

  // Clips with no track get no header of their own, so row three is the plain
  // object row. Every row outside a track's band means the same thing - leave
  // the track - which is what makes dropping a clip out of one possible at all.
  const editor::TimelinePanelFrame outside = h.frame(500.0F, 133.0F, true);
  UI_CHECK(outside.clip_edits.empty());
  const editor::TimelinePanelFrame dropped = h.frame(500.0F, 133.0F, false);
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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 50U> cases{{
    {"transport controls report their action",
     &transport_controls_report_their_action},
    {"dragging a clip body moves the placement",
     &dragging_a_clip_body_moves_the_placement},
    {"dragging a clip edge trims without moving the rest",
     &dragging_a_clip_edge_trims_without_moving_the_rest},
    {"a clip fade handle sets the head ramp",
     &a_clip_fade_handle_sets_the_head_ramp},
    {"a clip move snaps to the playhead", &a_clip_move_snaps_to_the_playhead},
    {"advanced drag tools emit one typed request on release",
     &advanced_drag_tools_emit_one_typed_request_on_release},
    {"advanced actions are contextual and typed",
     &advanced_actions_are_contextual_and_typed},
    {"splitting reports the selected clip at the playhead",
     &splitting_reports_the_selected_clip_at_the_playhead},
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
    {"set range buttons use the show frame and preserve loop",
     &set_range_buttons_use_the_show_frame_and_preserve_loop},
    {"set range buttons keep stale playheads in a valid range",
     &set_range_buttons_keep_stale_playheads_in_a_valid_range},
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
