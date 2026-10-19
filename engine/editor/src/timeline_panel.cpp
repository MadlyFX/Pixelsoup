#include <mediaserver/editor/timeline_panel.hpp>

#include <mediaserver/editor/cues_panel.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

constexpr float toolbar_height = 34.0F;
constexpr float ruler_height = 22.0F;
// Markers and cues get a band of their own under the ruler, so dragging one
// never competes with scrubbing and a name has room to be read.
constexpr float strip_height = 20.0F;
// Every row - layer, track, or property - is a full list row high, so a
// toggle or a key diamond is as easy to hit as the name beside it.
constexpr float row_height = list_metrics::row_height;
constexpr float icon_button_width = 24.0F;
constexpr float edit_tool_scale = 1.35F;

[[nodiscard]] gui::Cursor tool_cursor(TimelineEditTool tool) noexcept {
  switch (tool) {
  case TimelineEditTool::Standard: return gui::Cursor::Arrow;
  case TimelineEditTool::RippleTrim: return gui::Cursor::RippleTrim;
  case TimelineEditTool::Roll: return gui::Cursor::Roll;
  case TimelineEditTool::Slip: return gui::Cursor::Slip;
  case TimelineEditTool::Slide: return gui::Cursor::Slide;
  case TimelineEditTool::Razor: return gui::Cursor::Razor;
  }
  return gui::Cursor::Arrow;
}
// How close to an In/Out edge on the ruler's range bar a press grabs it.
constexpr float range_handle_reach = 5.0F;
constexpr float marker_hit_reach = 6.0F;
// The window a second click still counts as a double-click.
constexpr double double_click_seconds = 0.35;
constexpr float key_radius = 5.5F;
// A key answers a press well beyond the diamond it draws: at arm's length a
// five-unit diamond is a hard target, and nothing else in a lane competes.
constexpr float key_hit_radius = 10.0F;
// How close a press must come to a clip's fade handle to take it.
constexpr float fade_handle_reach = 8.0F;
// The layer list's limits while the operator drags its edge.
constexpr float minimum_list_width = 200.0F;
constexpr float minimum_graph_width = 160.0F;
constexpr float snap_distance = 8.0F;
constexpr float marquee_threshold = 4.0F;
constexpr float lane_drag_threshold_squared = 16.0F;
constexpr float minimum_pixels_per_frame = 0.02F;
constexpr float maximum_pixels_per_frame = 200.0F;
constexpr std::size_t filmstrip_prefetch_rows = 4U;
constexpr std::size_t maximum_filmstrip_requests = 32U;
constexpr std::size_t waveform_prefetch_rows = 4U;
constexpr std::size_t maximum_waveform_requests = 32U;

[[nodiscard]] std::optional<core::i64>
parse_integer(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  bool explicit_plus = text.front() == '+';
  if (explicit_plus) {
    text.remove_prefix(1U);
    if (text.empty()) {
      return std::nullopt;
    }
  }
  core::i64 value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
             ? std::optional<core::i64>{value}
             : std::nullopt;
}

[[nodiscard]] core::u64
key_scale_signature(std::span<const TimelineKeyAddress> keys,
                    const scene::KeyTimeScale &scale) noexcept {
  core::u64 hash = 1469598103934665603ULL;
  const auto append = [&](core::u64 value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  append(static_cast<core::u64>(scale.numerator));
  append(static_cast<core::u64>(scale.denominator));
  append(static_cast<core::u64>(scale.anchor));
  append(static_cast<core::u64>(scale.playhead));
  append(static_cast<core::u64>(scale.user_anchor));
  for (const TimelineKeyAddress &key : keys) {
    append(key.target.value());
    append(static_cast<core::u64>(key.property));
    append(static_cast<core::u64>(key.frame));
  }
  return hash;
}

[[nodiscard]] bool
point_in_polygon(float x, float y,
                 std::span<const gui::UiPoint> polygon) noexcept {
  if (polygon.size() < 3U) {
    return false;
  }
  bool inside = false;
  std::size_t previous = polygon.size() - 1U;
  for (std::size_t current = 0U; current < polygon.size(); ++current) {
    const gui::UiPoint &a = polygon[current];
    const gui::UiPoint &b = polygon[previous];
    const bool crosses = (a.y > y) != (b.y > y);
    if (crosses) {
      const float at_x = (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x;
      if (x < at_x) {
        inside = !inside;
      }
    }
    previous = current;
  }
  return inside;
}

[[nodiscard]] gui::UiColor
channel_color(scene::PropertyChannel) noexcept {
  return gui::UiColor{0.67F, 0.73F, 0.80F, 1.0F};
}

[[nodiscard]] std::string_view
channel_label(scene::PropertyChannel property) noexcept {
  switch (property) {
  case scene::PropertyChannel::PositionX:
    return "Position X";
  case scene::PropertyChannel::PositionY:
    return "Position Y";
  case scene::PropertyChannel::PositionZ:
    return "Position Z";
  case scene::PropertyChannel::RotationYaw:
    return "Rotation Yaw";
  case scene::PropertyChannel::RotationPitch:
    return "Rotation Pitch";
  case scene::PropertyChannel::RotationRoll:
    return "Rotation Roll";
  case scene::PropertyChannel::ScaleX:
    return "Scale X";
  case scene::PropertyChannel::ScaleY:
    return "Scale Y";
  case scene::PropertyChannel::ScaleZ:
    return "Scale Z";
  case scene::PropertyChannel::Opacity:
    return "Opacity";
  case scene::PropertyChannel::Brightness:
    return "Brightness";
  case scene::PropertyChannel::Saturation:
    return "Saturation";
  case scene::PropertyChannel::BlurRadius:
    return "Gaussian Blur";
  case scene::PropertyChannel::FilmGrainAmount:
    return "Film Grain";
  case scene::PropertyChannel::BoxBlurRadius:
    return "Box Blur";
  case scene::PropertyChannel::DirectionalBlurLength:
    return "Directional Blur";
  case scene::PropertyChannel::ChromaKeyMix:
    return "Chroma Key Mix";
  case scene::PropertyChannel::LumaKeyMix:
    return "Luma Key Mix";
  case scene::PropertyChannel::EnvironmentIntensity:
    return "Map intensity";
  case scene::PropertyChannel::EnvironmentRotation:
    return "Map rotation";
  case scene::PropertyChannel::FogDensity:
    return "Fog density";
  case scene::PropertyChannel::FogHeight:
    return "Fog height";
  case scene::PropertyChannel::FogFalloff:
    return "Height falloff";
  case scene::PropertyChannel::FogAnisotropy:
    return "Anisotropy";
  case scene::PropertyChannel::SkyHaze:
    return "Haze";
  case scene::PropertyChannel::SkyIlluminance:
    return "Sun illuminance";
  case scene::PropertyChannel::SkyTimeOfDay:
    return "Solar time";
  case scene::PropertyChannel::LightIntensity:
    return "Intensity";
  case scene::PropertyChannel::LightColorR:
    return "Color R";
  case scene::PropertyChannel::LightColorG:
    return "Color G";
  case scene::PropertyChannel::LightColorB:
    return "Color B";
  case scene::PropertyChannel::LightRange:
    return "Range";
  case scene::PropertyChannel::CameraFieldOfView:
    return "Field of view";
  case scene::PropertyChannel::CameraShutter:
    return "Shutter seconds";
  case scene::PropertyChannel::CameraFocusDistance:
    return "Focus distance";
  case scene::PropertyChannel::CameraAperture:
    return "Aperture";
  case scene::PropertyChannel::Count:
    break;
  }
  return "Unknown Channel";
}

// Property groups a layer twirls open to. The first three are transform
// groups whose channels are laid out three apiece from PositionX; Color is a
// light's colour, whose channels start at LightColorR.
constexpr std::array<std::string_view, 4U> group_names{
    {"Position", "Rotation", "Scale", "Color"}};
constexpr std::array<std::array<std::string_view, 3U>, 4U> component_names{{
    {{"X", "Y", "Z"}},
    {{"Yaw", "Pitch", "Roll"}},
    {{"X", "Y", "Z"}},
    {{"R", "G", "B"}},
}};
constexpr core::u8 color_group = 3U;

[[nodiscard]] scene::PropertyChannel group_channel(core::u8 group,
                                                   core::u8 component) noexcept {
  if (group == color_group) {
    return static_cast<scene::PropertyChannel>(
        static_cast<core::u8>(scene::PropertyChannel::LightColorR) + component);
  }
  return static_cast<scene::PropertyChannel>(group * 3U + component);
}

struct RateChoice final {
  ShowRateRequest rate;
  std::string_view label;
};

constexpr std::array<RateChoice, 8U> common_rates{{
    {{24'000U, 1'001U}, "23.976 FPS"},
    {{24U, 1U}, "24 FPS"},
    {{25U, 1U}, "25 FPS"},
    {{30'000U, 1'001U}, "29.97 FPS"},
    {{30U, 1U}, "30 FPS"},
    {{50U, 1U}, "50 FPS"},
    {{60'000U, 1'001U}, "59.94 FPS"},
    {{60U, 1U}, "60 FPS"},
}};

[[nodiscard]] bool same_rate(const EditorTimeline &timeline,
                             ShowRateRequest rate) noexcept {
  return timeline.rate_numerator == rate.numerator &&
         timeline.rate_denominator == rate.denominator;
}

[[nodiscard]] std::string rate_label(const EditorTimeline &timeline) {
  const auto found = std::find_if(common_rates.begin(), common_rates.end(),
                                  [&](const RateChoice &choice) {
                                    return same_rate(timeline, choice.rate);
                                  });
  if (found != common_rates.end()) {
    return std::string{found->label};
  }

  std::array<char, 48U> text{};
  const int length =
      timeline.rate_denominator == 1U
          ? std::snprintf(text.data(), text.size(), "%u FPS",
                          timeline.rate_numerator)
          : std::snprintf(text.data(), text.size(), "%u/%u FPS",
                          timeline.rate_numerator, timeline.rate_denominator);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char c : name) {
    hash ^= static_cast<core::u8>(c);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9E3779B9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] gui::UiColor mix_colors(gui::UiColor a, gui::UiColor b,
                                      float t) noexcept {
  return gui::UiColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// A square tool button carrying a drawn mark. `engaged` is a mode that is on
// - the current tool, snapping, loop - and reads as a lit key rather than as
// a pressed one. The tooltip names the action and its key, which is how an
// icon stays learnable.
[[nodiscard]] bool icon_button(gui::UiContext &ctx, gui::UiId id,
                               const gui::Rect &bounds, gui::Icon icon,
                               bool engaged, std::string_view tooltip,
                               gui::UiColor engaged_color,
                               float icon_scale = 1.0F) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  const gui::Theme &theme = ctx.theme();
  ctx.panel(bounds, (interaction.held && !interaction.released)
                        ? theme.widget_active
                        : (engaged ? alpha(engaged_color, 0.22F)
                                   : (interaction.hovered ? theme.widget_hover
                                                          : theme.widget)));
  if (engaged) {
    ctx.panel(gui::Rect{bounds.x, bounds.y + bounds.height - 2.0F,
                        bounds.width, 2.0F},
              engaged_color);
  }
  gui::draw_icon(ctx.draw_list(), icon, bounds.x + bounds.width * 0.5F,
                 bounds.y + bounds.height * 0.5F,
                 engaged ? engaged_color
                         : (interaction.hovered ? theme.text
                                                : theme.text_muted), icon_scale);
  ctx.tooltip(bounds, tooltip);
  return interaction.clicked;
}

// A text toggle in the same family, for the few modes with no honest icon.
[[nodiscard]] bool text_toggle(gui::UiContext &ctx, gui::UiId id,
                               const gui::Rect &bounds, std::string_view label,
                               bool engaged, std::string_view tooltip) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  const gui::Theme &theme = ctx.theme();
  ctx.panel(bounds, (interaction.held && !interaction.released)
                        ? theme.widget_active
                        : (engaged ? alpha(theme.accent, 0.22F)
                                   : (interaction.hovered ? theme.widget_hover
                                                          : theme.widget)));
  if (engaged) {
    ctx.panel(gui::Rect{bounds.x, bounds.y + bounds.height - 2.0F,
                        bounds.width, 2.0F},
              theme.accent);
  }
  const float width = ctx.font_width(label);
  ctx.label(gui::Rect{bounds.x + std::max((bounds.width - width) * 0.5F, 2.0F),
                      bounds.y, std::max(bounds.width - 4.0F, 0.0F),
                      bounds.height},
            label, engaged ? theme.text : theme.text_muted);
  ctx.tooltip(bounds, tooltip);
  return interaction.clicked;
}

void draw_separator(gui::UiContext &ctx, float x, const gui::Rect &toolbar) {
  ctx.panel(gui::Rect{x, toolbar.y + 8.0F, 1.0F, toolbar.height - 16.0F},
            ctx.theme().divider);
}

// What a context-menu entry does. Entries carry their target with them, so
// the menu means what it said when it opened.
enum class MenuCommand : core::u8 {
  SplitAtPlayhead,
  RippleDeleteSelection,
  RippleDeleteClip,
  InsertClipboard,
  OverwriteClipboard,
  RangeToClip,
  ClearKeys,
  RenameTrack,
  RenameLayer,
  ToggleTrackMute,
  ToggleTrackLock,
  DeleteTrack,
  DeleteTrackClips,
  MoveClipsDeleteTrack,
  NewTrack,
  AddMarker,
  AddHoldCue,
  AddLoopCue,
  SetIn,
  SetOut,
  ZoomFit,
  ToggleRulerUnits,
  RenameMarker,
  MarkerToHold,
  MarkerToLoop,
  MarkerToPlain,
  MarkerNextColor,
  GoToMarker,
  DeleteMarker,
};

// A cue's tag: the number in a filled chip. Held cues burn in the transport
// colour, the one warm thing on the timeline, because a held show is the
// state an operator must never lose track of.
[[nodiscard]] std::string cue_tag(std::size_t number) {
  return "Q" + std::to_string(number);
}

void draw_border(gui::UiContext &ctx, const gui::Rect &bounds,
                 gui::UiColor color, float thickness = 1.0F) {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F || thickness <= 0.0F) {
    return;
  }
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, thickness, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - thickness, bounds.width,
                thickness, color);
  draw.add_rect(bounds.x, bounds.y, thickness, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - thickness, bounds.y, thickness,
                bounds.height, color);
}

void draw_diamond(gui::UiContext &ctx, float cx, float cy, float radius,
                  gui::UiColor color) {
  const std::array<gui::UiPoint, 4U> points{{
      {cx, cy - radius},
      {cx + radius, cy},
      {cx, cy + radius},
      {cx - radius, cy},
  }};
  ctx.draw_list().add_convex_poly(points, color);
}

void draw_circle(gui::UiContext &ctx, float cx, float cy, float radius,
                 gui::UiColor color) {
  constexpr std::size_t point_count = 12U;
  constexpr float tau = 6.28318530718F;
  std::array<gui::UiPoint, point_count> points{};
  for (std::size_t index = 0U; index < point_count; ++index) {
    const float angle =
        tau * static_cast<float>(index) / static_cast<float>(point_count);
    points[index] = gui::UiPoint{cx + std::cos(angle) * radius,
                                 cy + std::sin(angle) * radius};
  }
  ctx.draw_list().add_convex_poly(points, color);
}

[[nodiscard]] bool key_button(gui::UiContext &ctx, gui::UiId id,
                              const gui::Rect &bounds, bool animated,
                              bool keyed_here, gui::UiColor background) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  gui::UiColor color =
      keyed_here ? ctx.theme().playhead
                 : (animated ? ctx.theme().keyframe : ctx.theme().text_muted);
  if (interaction.hovered) {
    color = ctx.theme().accent;
  }
  const float cx = bounds.x + bounds.width * 0.5F;
  const float cy = bounds.y + bounds.height * 0.5F;
  draw_diamond(ctx, cx, cy, interaction.held ? 5.0F : 4.5F, color);
  if (!keyed_here && !interaction.held) {
    draw_diamond(ctx, cx, cy, 2.4F, background);
  }
  return interaction.clicked;
}

[[nodiscard]] core::i64 signed_frame(core::u64 frame) noexcept {
  constexpr core::u64 maximum =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  return frame > maximum ? std::numeric_limits<core::i64>::max()
                         : static_cast<core::i64>(frame);
}

[[nodiscard]] core::u64 rounded_frame(double frame, core::u64 first,
                                      core::u64 last) noexcept {
  const double lo = static_cast<double>(std::min(first, last));
  const double hi = static_cast<double>(std::max(first, last));
  if (!std::isfinite(frame)) {
    return first;
  }
  return static_cast<core::u64>(std::floor(std::clamp(frame, lo, hi) + 0.5));
}

[[nodiscard]] core::u64 entered_frame(double value) noexcept {
  if (!std::isfinite(value) || value <= 0.0) {
    return 0U;
  }
  constexpr core::u64 maximum =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  const double limit = static_cast<double>(maximum);
  if (value >= limit) {
    return maximum;
  }
  return static_cast<core::u64>(std::llround(value));
}

[[nodiscard]] std::string layer_name(const ElementInfo &element) {
  return element_display_name(element);
}

// Which elements a layer row twirls open to keyable properties. A camera's
// eye animates by position; every drawable by its transform and look; a light
// by its placement, output and colour. Anything else lists without a twirl,
// so a row never offers keys the show would not play.
[[nodiscard]] bool animatable(const ElementInfo &element) noexcept {
  return element.kind != ElementKind::Track;
}

[[nodiscard]] std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char character : input) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool matches(std::string_view name, std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] const scene::ChannelKeyframes *
channel_for(std::span<const scene::ChannelKeyframes> keyframes,
            doc::NodeId node, scene::TransformChannel component) noexcept {
  for (const scene::ChannelKeyframes &entry : keyframes) {
    if (entry.target == node && entry.component == component) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<double>
sample_channel_at(const scene::ChannelKeyframes *channel,
                  core::u64 frame) noexcept {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (channel == nullptr || frame > maximum) return std::nullopt;
    return scene::sample_keyframes(*channel, static_cast<core::i64>(frame));
}

[[nodiscard]] const std::vector<core::i64> *
frames_for(std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
           scene::TransformChannel component) noexcept {
  const scene::ChannelKeyframes *const channel =
      channel_for(keyframes, node, component);
  return channel != nullptr ? &channel->frames : nullptr;
}

[[nodiscard]] const ElementInfo *
find_element(std::span<const ElementInfo> elements, doc::NodeId node) noexcept {
  for (const ElementInfo &element : elements) {
    if (element.node == node) {
      return &element;
    }
  }
  return nullptr;
}

[[nodiscard]] const output::ProjectionSurface *
find_surface(std::span<const output::ProjectionSurface> surfaces,
             doc::NodeId node) noexcept {
  for (const output::ProjectionSurface &surface : surfaces) {
    if (surface.node == node) {
      return &surface;
    }
  }
  return nullptr;
}

[[nodiscard]] double
surface_component_value(const output::ProjectionSurface &surface,
                        scene::PropertyChannel component) noexcept {
  switch (component) {
  case scene::PropertyChannel::PositionX:
    return surface.geometry.center.x;
  case scene::PropertyChannel::PositionY:
    return surface.geometry.center.y;
  case scene::PropertyChannel::PositionZ:
    return surface.geometry.center.z;
  case scene::PropertyChannel::RotationYaw: {
    constexpr double tau = 6.28318530717958647692;
    return std::atan2(-static_cast<double>(surface.geometry.right.z),
                      static_cast<double>(surface.geometry.right.x)) /
           tau;
  }
  default:
    return 0.0;
  }
}

[[nodiscard]] core::i64 ruler_step(float pixels_per_frame) noexcept {
  const double wanted =
      76.0 / std::max(static_cast<double>(pixels_per_frame),
                      static_cast<double>(minimum_pixels_per_frame));
  constexpr std::array<core::i64, 18U> nice{{1, 2, 5, 10, 15, 30, 60, 120, 300,
                                             600, 1'200, 1'800, 3'600, 7'200,
                                             18'000, 36'000, 72'000, 180'000}};
  for (const core::i64 step : nice) {
    if (static_cast<double>(step) >= wanted) {
      return step;
    }
  }
  return nice.back();
}

struct Row final {
  enum class Kind : core::u8 {
    LaneHeader,
    Layer,
    PropertiesHeader,
    Group,
    Component,
    Property,
  };

  Kind kind{Kind::Layer};
  doc::NodeId node{};
  core::u8 group{};
  core::u8 component{};
  std::string label;
  gui::UiColor color{};
  ElementKind element_kind{ElementKind::Object};
  std::size_t indent{};
  bool expandable{};
  scene::PropertyChannel property{scene::PropertyChannel::Count};
  doc::NodeId property_target{};
  double property_value{};
  bool surface{};
  core::u8 component_count{3U};
  bool locked{};
  bool muted{};
  // The element's own authored visibility, before any group above it. The
  // toggle reports what this row owns, so a row hidden by its group still
  // shows its own state rather than its parent's.
  bool element_visible{true};
  // The lane band this row sits in: a lane header, or any row belonging to a
  // clip under one. The implicit lane's owner is the zero id, which is also
  // the default, so the flag rather than the id is what says "in a band".
  bool lane_member{};
  doc::NodeId lane_owner{};
  bool hierarchy{};
};

[[nodiscard]] doc::NodeId layer_parent(const ElementInfo &element) noexcept {
  return element.linked ? element.linked_parent
                        : element.parent_group.value_or(doc::NodeId{});
}

[[nodiscard]] core::u64 group_id(doc::NodeId node, core::u8 group) noexcept {
  return node.value() * 4U + static_cast<core::u64>(group);
}

// A clip the operator can edit on the timeline: a media placement with a real
// duration. An unplaced clip is the legacy always-on form and keeps the plain
// full-range layer bar, because there is no in or out point to drag.
[[nodiscard]] bool editable_clip(const ElementInfo &element) noexcept {
  return !element.locked && element.kind == ElementKind::Media &&
         element.timing.placed();
}

[[nodiscard]] bool orderable_layer(const ElementInfo &element) noexcept {
  return element.kind == ElementKind::Object ||
         element.kind == ElementKind::Media ||
         element.kind == ElementKind::Model ||
         element.kind == ElementKind::Group || is_light(element.kind);
}

[[nodiscard]] std::optional<core::i64>
exclusive_clip_end(const scene::ClipTiming &timing) noexcept {
  if (timing.duration <= 0 ||
      timing.start > std::numeric_limits<core::i64>::max() - timing.duration) {
    return std::nullopt;
  }
  return timing.start + timing.duration;
}

// How wide the grab strips at a clip's two ends are. They shrink on a very
// short bar so a two-frame clip does not become impossible to move, but on an
// ordinary bar they are a comfortable target rather than a hairline.
[[nodiscard]] float edge_grab_width(float bar_width) noexcept {
  return std::clamp(bar_width * 0.3F, 3.0F, 10.0F);
}

// The nearest snap candidate to `value`, or nothing when none is within
// `tolerance`. Ties resolve to the lower frame so the result never depends on
// the order candidates were collected in.
[[nodiscard]] std::optional<core::i64>
nearest_snap(core::i64 value, std::span<const core::i64> candidates,
             core::i64 tolerance) noexcept {
  std::optional<core::i64> best;
  const core::u64 maximum_distance = static_cast<core::u64>(std::max(tolerance, core::i64{0}));
  core::u64 best_distance = maximum_distance + 1U;
  for (const core::i64 candidate : candidates) {
    // Unsigned subtraction represents the full distance even when the two
    // signed authored frames straddle the complete i64 range.
    const core::u64 distance = candidate > value
        ? static_cast<core::u64>(candidate) - static_cast<core::u64>(value)
        : static_cast<core::u64>(value) - static_cast<core::u64>(candidate);
    if (distance > maximum_distance) {
      continue;
    }
    if (!best.has_value() || distance < best_distance ||
        (distance == best_distance && candidate < *best)) {
      best = candidate;
      best_distance = distance;
    }
  }
  return best;
}

[[nodiscard]] core::u64 nominal_rate(core::u32 numerator,
                                     core::u32 denominator) noexcept {
  const core::u64 den = std::max<core::u64>(denominator, 1U);
  return std::max<core::u64>(
      (static_cast<core::u64>(numerator) + den / 2U) / den, 1U);
}

// Tick spacing that lands on whole timecode units: single frames when zoomed
// right in, then whole seconds and minutes, so a label always reads as a
// round number of the unit an operator counts in.
[[nodiscard]] core::i64 timecode_step(float pixels_per_frame, core::u64 fps,
                                      float minimum_spacing) noexcept {
  const double wanted =
      static_cast<double>(minimum_spacing) /
      std::max(static_cast<double>(pixels_per_frame),
               static_cast<double>(minimum_pixels_per_frame));
  const auto rate = static_cast<core::i64>(fps);
  constexpr std::array<core::i64, 4U> frames{{1, 2, 5, 10}};
  for (const core::i64 step : frames) {
    if (step < rate && static_cast<double>(step) >= wanted) {
      return step;
    }
  }
  constexpr std::array<core::i64, 14U> seconds{
      {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1'800, 3'600, 7'200}};
  for (const core::i64 step : seconds) {
    if (static_cast<double>(step * rate) >= wanted) {
      return step * rate;
    }
  }
  return seconds.back() * rate;
}

} // namespace

gui::UiColor timeline_marker_color(core::u8 index) noexcept {
  constexpr std::array<gui::UiColor, scene::timeline_marker_colors> palette{{
      {0.93F, 0.36F, 0.35F, 1.0F},
      {0.96F, 0.60F, 0.27F, 1.0F},
      {0.94F, 0.84F, 0.33F, 1.0F},
      {0.42F, 0.84F, 0.47F, 1.0F},
      {0.32F, 0.82F, 0.90F, 1.0F},
      {0.40F, 0.58F, 0.98F, 1.0F},
      {0.71F, 0.50F, 0.96F, 1.0F},
      {0.95F, 0.47F, 0.73F, 1.0F},
  }};
  return palette[index % palette.size()];
}

std::string_view timeline_marker_color_name(core::u8 index) noexcept {
  constexpr std::array<std::string_view, scene::timeline_marker_colors> names{
      {"Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Pink"}};
  return names[index % names.size()];
}

std::string format_timecode(core::u64 frame, core::u32 rate_numerator,
                            core::u32 rate_denominator) {
  const core::u64 fps = nominal_rate(rate_numerator, rate_denominator);
  const core::u64 total_seconds = frame / fps;
  std::array<char, 48U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%02llu:%02llu:%02llu:%02llu",
      static_cast<unsigned long long>(total_seconds / 3'600U),
      static_cast<unsigned long long>((total_seconds / 60U) % 60U),
      static_cast<unsigned long long>(total_seconds % 60U),
      static_cast<unsigned long long>(frame % fps));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

std::optional<core::u64> parse_timecode(std::string_view text,
                                        core::u32 rate_numerator,
                                        core::u32 rate_denominator,
                                        core::u64 relative_to) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1U);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1U);
  }
  int sign = 0;
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    sign = text.front() == '+' ? 1 : -1;
    text.remove_prefix(1U);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  // Up to four fields, most significant first; the last is always frames.
  std::array<core::u64, 4U> fields{};
  std::size_t count = 0U;
  std::size_t begin = 0U;
  for (std::size_t index = 0U; index <= text.size(); ++index) {
    const bool end = index == text.size();
    const char c = end ? ':' : text[index];
    if (c == ':' || c == ';' || c == '.') {
      if (index == begin || count == fields.size()) {
        return std::nullopt;
      }
      core::u64 value{};
      const auto parsed =
          std::from_chars(text.data() + begin, text.data() + index, value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index) {
        return std::nullopt;
      }
      fields[count++] = value;
      begin = index + 1U;
    } else if (c < '0' || c > '9') {
      return std::nullopt;
    }
  }
  const core::u64 fps = nominal_rate(rate_numerator, rate_denominator);
  constexpr core::u64 limit =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  core::u64 frames{};
  if (count == 1U) {
    frames = fields[0U];
  } else {
    // Hours, minutes and seconds accumulate in base sixty; the last field is
    // always frames.
    core::u64 seconds{};
    for (std::size_t index = 0U; index + 1U < count; ++index) {
      if (seconds > limit / 60U) {
        return std::nullopt;
      }
      seconds = seconds * 60U + fields[index];
    }
    if (seconds > limit / fps) {
      return std::nullopt;
    }
    frames = seconds * fps + fields[count - 1U];
  }
  if (frames > limit) {
    return std::nullopt;
  }
  if (sign > 0) {
    return relative_to > limit - frames ? limit : relative_to + frames;
  }
  if (sign < 0) {
    return relative_to > frames ? relative_to - frames : 0U;
  }
  return frames;
}

bool TimelinePanel::selected(
    doc::NodeId node, std::span<const doc::NodeId> selection) const noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

bool TimelinePanel::key_selected(const SelectedKey &key) const noexcept {
  return std::find(selected_keys_.begin(), selected_keys_.end(), key) !=
         selected_keys_.end();
}

void TimelinePanel::canonicalize_key_selection() noexcept {
  std::sort(selected_keys_.begin(), selected_keys_.end(),
            [](const SelectedKey &left, const SelectedKey &right) noexcept {
              return std::tie(left.target, left.component, left.frame) <
                     std::tie(right.target, right.component, right.frame);
            });
  selected_keys_.erase(
      std::unique(selected_keys_.begin(), selected_keys_.end()),
      selected_keys_.end());
}

void TimelinePanel::replace_key_selection(
    std::span<const TimelineKeyAddress> selection) {
  std::vector<SelectedKey> next;
  next.reserve(selection.size());
  for (const TimelineKeyAddress &address : selection) {
    next.push_back(
        SelectedKey{address.target, address.property, address.frame});
  }
  selected_keys_ = std::move(next);
  canonicalize_key_selection();
  drag_keys_.clear();
  dragging_key_ = false;
  duplicate_drag_ = false;
  drag_delta_ = 0;
  snap_guide_.reset();
}

std::optional<TimelineKeyClipboardEdit> TimelinePanel::key_clipboard_edit(
    TimelineKeyClipboardAction action) const {
  if (selected_keys_.empty()) {
    return std::nullopt;
  }
  TimelineKeyClipboardEdit edit{};
  edit.action = action;
  if (action == TimelineKeyClipboardAction::Copy) {
    edit.keys.reserve(selected_keys_.size());
    for (const SelectedKey &key : selected_keys_) {
      edit.keys.push_back(
          TimelineKeyAddress{key.target, key.component, key.frame});
    }
    std::sort(edit.keys.begin(), edit.keys.end());
    edit.keys.erase(std::unique(edit.keys.begin(), edit.keys.end()),
                    edit.keys.end());
  }
  return edit;
}

void TimelinePanel::clear_key_context() noexcept {
  selected_keys_.clear();
  dragging_key_ = false;
  duplicate_drag_ = false;
  drag_keys_.clear();
  drag_delta_ = 0;
  snap_guide_.reset();
  key_tools_open_ = false;
  scale_open_ = false;
  hidden_channels_.clear();
  active_channel_.reset();
  key_value_field_ = NumberField{};
  marquee_tracking_ = false;
  marquee_active_ = false;
  marquee_base_.clear();
  lasso_points_.clear();
  // Markers are scene content, so a Stage-scene switch leaves none of the
  // old scene's selected or half-dragged.
  selected_markers_.clear();
  marker_drag_ = doc::NodeId{};
  marker_drag_moved_ = false;
  marker_drag_delta_ = 0;
  marker_snap_guide_.reset();
  marker_name_node_ = doc::NodeId{};
  marker_name_draft_.clear();
  menu_entries_.clear();
}

void TimelinePanel::select_marker(doc::NodeId marker) {
  selected_markers_.assign(1U, marker);
}

void TimelinePanel::reset_project_state() noexcept {
  replacement_targets_.clear();
  pixels_per_frame_ = 0.0F;
  left_frame_ = 0.0;
  time_view_initialized_ = false;
  row_scroll_ = 0.0F;
  expanded_layers_.clear();
  expanded_groups_.clear();
  expanded_hierarchies_.clear();
  hierarchy_selection_ = doc::NodeId{};
  selected_lane_ = doc::NodeId{};
  lane_destination_ = doc::NodeId{};
  rename_.reset();
  clicks_.reset();
  search_.clear();
  range_fields_ = NumberField{};
  clear_key_context();
  drag_anchor_frame_ = 0;
  rate_menu_open_ = false;
  edit_tool_ = TimelineEditTool::Standard;
  range_drag_ = RangeHandle::None;
  time_drag_id_ = gui::UiId{};
  time_drag_moved_ = false;
  time_edit_id_ = gui::UiId{};
  time_draft_.clear();
  marker_click_ = doc::NodeId{};
  timeline_focused_ = false;
  panning_ = false;
  marquee_start_x_ = 0.0F;
  marquee_start_y_ = 0.0F;
  marquee_shift_ = false;
  marquee_ctrl_ = false;
  lane_drag_node_ = doc::NodeId{};
  lane_drag_start_x_ = 0.0F;
  lane_drag_start_y_ = 0.0F;
  lane_drag_moved_ = false;
  layer_drag_node_ = doc::NodeId{};
  layer_drag_nodes_.clear();
  layer_drag_start_x_ = 0.0F;
  layer_drag_start_y_ = 0.0F;
  layer_drag_moved_ = false;
  clip_drag_ = ClipDrag::None;
  clip_drag_node_ = doc::NodeId{};
  clip_drag_peer_ = doc::NodeId{};
  clip_drag_origin_ = scene::ClipTiming{};
  clip_drag_proposed_ = scene::ClipTiming{};
  pending_clip_edit_.reset();
  pending_clip_frames_ = 0U;
  clip_status_.clear();
  clip_status_error_ = false;
  clip_status_until_ = 0.0;
  previous_draw_time_ = 0.0;
  clip_drag_anchor_ = 0;
  clip_drag_delta_ = 0;
  clip_drag_start_x_ = 0.0F;
  clip_drag_start_y_ = 0.0F;
  clip_drag_release_.reset();
  clip_drag_moved_ = false;
  clip_drop_lane_.reset();
  clip_snap_guide_.reset();
}

TimelinePanelFrame
TimelinePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                    const EditorTimeline &timeline,
                    std::span<const scene::ChannelKeyframes> keyframes,
                    std::span<const ElementInfo> elements,
                    std::span<const doc::NodeId> selection, doc::NodeId primary,
                    std::span<const scene::EffectInstance> effects,
                    std::span<const output::ProjectionSurface> surfaces,
                    doc::NodeId selected_surface,
                    std::span<const scene::TimelineLane> lanes,
                    std::optional<core::i64> clipboard_span,
                    std::optional<TimelineKeyScalePreview> key_scale_preview,
                    std::span<const TimelineFilmstripObservation> filmstrips,
                    std::span<const TimelineWaveformObservation> waveforms,
                    std::span<const scene::TimelineMarker> markers) {
  replacement_targets_.clear();
  TimelinePanelFrame frame{};
  const float gesture_x = ctx.pointer_released()
      ? ctx.pointer_release_x() : ctx.pointer_x();
  const float gesture_y = ctx.pointer_released()
      ? ctx.pointer_release_y() : ctx.pointer_y();
  const bool typing_at_start = ctx.typing();
  frame.filmstrip_retain = TimelineFilmstripRetainIntent{};
  frame.waveform_retain = TimelineWaveformRetainIntent{};
  ctx.panel(bounds, ctx.theme().panel);
  if (ctx.pointer_pressed()) {
    timeline_focused_ =
        bounds.contains(ctx.pointer_press_x(), ctx.pointer_press_y());
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    range_fields_.cancel(ctx);
    return frame;
  }
  ctx.push_clip(bounds);

  const core::u64 first = std::min(timeline.first_frame, timeline.last_frame);
  const core::u64 last = std::max(timeline.first_frame, timeline.last_frame);
  const core::i64 first_i = signed_frame(first);
  const core::i64 last_i = signed_frame(last);

  // The anchor must actually belong to the selection. A stale primary falls
  // back to the first selected element, exactly as the shell's viewport does.
  doc::NodeId anchor = primary;
  if (!selected(anchor, selection)) {
    anchor = selection.empty() ? doc::NodeId{} : selection.front();
  }
  const ElementInfo *const anchor_info = find_element(elements, anchor);

  // Canonical advanced-edit context. Controls only light up when the current
  // selection has the shape their planner requires; the Director validates
  // the same identities again against its projected document before it
  // authors anything.
  std::vector<const ElementInfo *> selected_clips;
  selected_clips.reserve(selection.size());
  bool clip_selection_valid = !selection.empty();
  for (const doc::NodeId id : selection) {
    const ElementInfo *const element = find_element(elements, id);
    if (element == nullptr || !editable_clip(*element) ||
        !exclusive_clip_end(element->timing).has_value()) {
      clip_selection_valid = false;
      continue;
    }
    selected_clips.push_back(element);
  }
  clip_selection_valid =
      clip_selection_valid && selected_clips.size() == selection.size();
  std::sort(selected_clips.begin(), selected_clips.end(),
            [](const ElementInfo *left, const ElementInfo *right) noexcept {
              if (left->timing.start != right->timing.start) {
                return left->timing.start < right->timing.start;
              }
              return left->node < right->node;
            });
  const bool same_selected_lane =
      clip_selection_valid &&
      std::all_of(selected_clips.begin(), selected_clips.end(),
                  [&](const ElementInfo *clip) noexcept {
                    return clip->timeline_lane ==
                           selected_clips.front()->timeline_lane;
                  });

  std::optional<doc::NodeId> target_lane;
  const auto explicit_lane = std::find_if(
      lanes.begin(), lanes.end(), [&](const scene::TimelineLane &lane) {
        return !lane.implicit && lane.id == selected_lane_;
      });
  if (explicit_lane != lanes.end() && !explicit_lane->locked) {
    target_lane = explicit_lane->id;
  } else if (same_selected_lane) {
    target_lane = selected_clips.front()->timeline_lane;
  }
  const bool paste_ready = clipboard_span.has_value() && *clipboard_span > 0 &&
                           target_lane.has_value();

  const float preferred_list =
      list_width_ > 0.0F ? list_width_
                         : std::clamp(bounds.width * 0.26F, 250.0F, 340.0F);
  const float list_width = std::min(
      std::max(preferred_list, std::min(minimum_list_width, bounds.width)),
      std::max(bounds.width - minimum_graph_width, 0.0F));
  const gui::Rect toolbar{bounds.x, bounds.y, bounds.width, toolbar_height};
  const gui::Rect ruler{bounds.x, bounds.y + toolbar_height, bounds.width,
                        ruler_height};
  const gui::Rect strip{bounds.x, ruler.y + ruler.height, bounds.width,
                        strip_height};
  const gui::Rect rows_bounds{
      bounds.x, strip.y + strip.height, bounds.width,
      std::max(bounds.height - toolbar_height - ruler_height - strip_height,
               0.0F)};
  const gui::Rect graph{bounds.x + list_width, ruler.y,
                        std::max(bounds.width - list_width, 0.0F),
                        ruler.height + strip.height + rows_bounds.height};
  // The part of the graph that is time rather than the ruler's own band.
  const gui::Rect strip_graph{graph.x, strip.y, graph.width, strip.height};
  const core::u64 fps =
      nominal_rate(timeline.rate_numerator, timeline.rate_denominator);
  const auto timecode = [&](core::u64 value) {
    if (ruler_frames_) {
      return std::to_string(value);
    }
    return format_timecode(value, timeline.rate_numerator,
                           timeline.rate_denominator);
  };

  // Markers arrive in (frame, id) order. A cue's number is its place among
  // the cues alone, which is the number a stage manager calls.
  std::vector<std::size_t> cue_numbers(markers.size(), 0U);
  bool any_cue{};
  {
    std::size_t number = 0U;
    for (std::size_t index = 0U; index < markers.size(); ++index) {
      if (markers[index].is_cue()) {
        cue_numbers[index] = ++number;
        any_cue = true;
      }
    }
  }
  const auto marker_index = [&](doc::NodeId id) -> std::optional<std::size_t> {
    for (std::size_t index = 0U; index < markers.size(); ++index) {
      if (markers[index].id == id) {
        return index;
      }
    }
    return std::nullopt;
  };
  selected_markers_.erase(
      std::remove_if(selected_markers_.begin(), selected_markers_.end(),
                     [&](doc::NodeId id) { return !marker_index(id); }),
      selected_markers_.end());
  if (marker_drag_ != doc::NodeId{} && !marker_index(marker_drag_)) {
    marker_drag_ = doc::NodeId{};
    marker_drag_moved_ = false;
    marker_drag_delta_ = 0;
  }
  if (marker_name_node_ != doc::NodeId{} && !marker_index(marker_name_node_)) {
    marker_name_node_ = doc::NodeId{};
    marker_name_draft_.clear();
  }

  if (!time_view_initialized_ && graph.width > 0.0F) {
    const double span =
        std::max(static_cast<double>(last) - static_cast<double>(first), 1.0);
    pixels_per_frame_ =
        std::clamp(static_cast<float>(static_cast<double>(graph.width) / span),
                   minimum_pixels_per_frame, maximum_pixels_per_frame);
    left_frame_ = static_cast<double>(first);
    time_view_initialized_ = true;
    if (anchor != doc::NodeId{} && anchor_info != nullptr &&
        !is_light(anchor_info->kind)) {
      expanded_layers_.insert(anchor.value());
    }
  }
  if (pixels_per_frame_ <= 0.0F) {
    pixels_per_frame_ = 1.0F;
  }

  const auto frame_to_x = [&](double value) noexcept {
    return graph.x + static_cast<float>((value - left_frame_) *
                                        static_cast<double>(pixels_per_frame_));
  };
  const auto x_to_frame = [&](float x) noexcept {
    return left_frame_ + static_cast<double>(x - graph.x) /
                             static_cast<double>(pixels_per_frame_);
  };

  // Build visible rows from separate object and property twirls. A parent
  // opens to its children and a Properties foldout; leaves open directly to
  // P/R/S, opacity and effects as before.
  std::vector<Row> rows;
  rows.reserve(elements.size() * 7U + surfaces.size() * 4U);
  const auto append_element = [&](const ElementInfo &element,
                                  std::size_t base_indent = 0U,
                                  bool hierarchy = false,
                                  bool include_context = false) {
    const std::string name = layer_name(element);
    if (!include_context && !matches(name, search_) &&
        !matches(kind_name(element), search_)) {
      return;
    }
    const bool transformable = animatable(element);
    rows.push_back(Row{Row::Kind::Layer, element.node, 0U, 0U, name,
                       element_kind_color(element), element.kind, base_indent,
                       transformable || hierarchy});
    rows.back().locked = element.locked;
    rows.back().element_visible = element.visible;
    rows.back().hierarchy = hierarchy;
    if (hierarchy) {
      if (!expanded_hierarchies_.contains(element.node) && search_.empty()) {
        return;
      }
      if (transformable) {
        rows.push_back(Row{Row::Kind::PropertiesHeader, element.node, 0U, 0U,
                           "Properties", ctx.theme().text_muted, element.kind,
                           ++base_indent, true});
      }
    }
    if (!transformable || expanded_layers_.count(element.node.value()) == 0U) {
      return;
    }
    if (element.kind == ElementKind::Environment) {
      for (auto index = static_cast<std::size_t>(scene::PropertyChannel::SkyTimeOfDay);
           index <= static_cast<std::size_t>(scene::PropertyChannel::SkyIlluminance); ++index) {
        const auto property = static_cast<scene::PropertyChannel>(index);
        const auto sampled = sample_channel_at(
            channel_for(keyframes, element.node, property), timeline.show_frame);
        rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
            std::string{channel_label(property)}, ctx.theme().text_muted, element.kind,
            base_indent + 1U, false, property, element.node,
            sampled.has_value() && std::isfinite(*sampled)
                ? *sampled
                : scene::environment_channel_value(element.environment, property)});
      }
      return;
    }
    const auto append_group = [&](core::u8 group) {
      rows.push_back(
          Row{Row::Kind::Group, element.node, group, 0U,
              std::string{group_names[static_cast<std::size_t>(group)]},
              ctx.theme().text_muted, element.kind,
              base_indent + 1U, true});
      if (expanded_groups_.count(group_id(element.node, group)) == 0U) {
        return;
      }
      for (core::u8 component = 0U; component < 3U; ++component) {
        rows.push_back(Row{Row::Kind::Component, element.node, group, component,
                           std::string{component_names[static_cast<std::size_t>(
                               group)][static_cast<std::size_t>(component)]},
                           ctx.theme().text_muted, element.kind,
                           base_indent + 2U, false});
      }
    };
    const auto append_property = [&](scene::PropertyChannel property,
                                      std::string label, double authored) {
      const auto sampled = sample_channel_at(
          channel_for(keyframes, element.node, property), timeline.show_frame);
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
                         std::move(label), ctx.theme().text_muted, element.kind,
                         base_indent + 1U, false, property, element.node,
                         sampled.has_value() && std::isfinite(*sampled)
                             ? *sampled
                             : authored});
    };
    if (is_light(element.kind)) {
      // A light keys where it is (a sun has no position, only a direction),
      // how bright it is, and its colour.
      if (element.kind != ElementKind::DirectionalLight) {
        append_group(0U);
      }
      append_property(scene::PropertyChannel::LightIntensity, "Intensity",
                      element.intensity);
      append_group(color_group);
      if (element.kind != ElementKind::DirectionalLight) {
        append_property(scene::PropertyChannel::LightRange, "Range",
                        element.range);
      }
      return;
    }
    if (element.kind == ElementKind::Camera) {
      // A camera moves and turns like anything else; it has no size.
      append_group(0U);
      append_group(1U);
      append_property(scene::PropertyChannel::CameraFieldOfView,
                      "Field of view", element.field_of_view);
      append_property(scene::PropertyChannel::CameraShutter,
                      "Shutter seconds", element.shutter_seconds);
      append_property(scene::PropertyChannel::CameraFocusDistance,
                      "Focus distance", element.focus_distance_m);
      append_property(scene::PropertyChannel::CameraAperture,
                      "Aperture", element.aperture_f_number);
      return;
    }
    for (core::u8 group = 0U; group < 3U; ++group) {
      append_group(group);
    }
    if (element.kind != ElementKind::Group) {
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U, "Opacity",
                         ctx.theme().text_muted, element.kind,
                         base_indent + 1U, false,
                         scene::PropertyChannel::Opacity, element.node,
                         element.style.opacity});
    }
    for (const scene::EffectInstance &effect : effects) {
      if (effect.target != element.node || !effect.enabled) {
        continue;
      }
      scene::PropertyChannel property{};
      std::string_view label;
      switch (effect.kind) {
      case scene::EffectKind::Brightness:
        property = scene::PropertyChannel::Brightness;
        label = "Brightness";
        break;
      case scene::EffectKind::Saturation:
        property = scene::PropertyChannel::Saturation;
        label = "Saturation";
        break;
      case scene::EffectKind::Blur:
        property = scene::PropertyChannel::BlurRadius;
        label = "Gaussian Blur Radius";
        break;
      case scene::EffectKind::BoxBlur:
        property = scene::PropertyChannel::BoxBlurRadius;
        label = "Box Blur Radius";
        break;
      case scene::EffectKind::DirectionalBlur:
        property = scene::PropertyChannel::DirectionalBlurLength;
        label = "Directional Blur Length";
        break;
      case scene::EffectKind::FilmGrain:
        property = scene::PropertyChannel::FilmGrainAmount;
        label = "Film Grain Amount";
        break;
      case scene::EffectKind::ChromaKey:
        property = scene::PropertyChannel::ChromaKeyMix;
        label = "Chroma Key Mix";
        break;
      case scene::EffectKind::LumaKey:
        property = scene::PropertyChannel::LumaKeyMix;
        label = "Luma Key Mix";
        break;
      case scene::EffectKind::Levels:
      case scene::EffectKind::LiftGammaGain:
      case scene::EffectKind::ContrastPivot:
      case scene::EffectKind::ColorMatrix:
      case scene::EffectKind::WhiteBalance:
      case scene::EffectKind::Curves:
      case scene::EffectKind::Count:
        // Structured colour payloads are edited by named schema
        // properties in the Color panel. They do not masquerade as a
        // transform channel until typed keyframe channels exist.
        continue;
      }
      double authored_value = static_cast<double>(effect.value);
      if (effect.kind == scene::EffectKind::BoxBlur) {
        authored_value = static_cast<double>(effect.box_blur.radius_px);
      } else if (effect.kind == scene::EffectKind::DirectionalBlur) {
        authored_value = static_cast<double>(effect.directional_blur.length_px);
      } else if (effect.kind == scene::EffectKind::FilmGrain) {
        authored_value = static_cast<double>(effect.film_grain.amount);
      } else if (effect.kind == scene::EffectKind::ChromaKey) {
        authored_value = static_cast<double>(effect.chroma_key.mix);
      } else if (effect.kind == scene::EffectKind::LumaKey) {
        authored_value = static_cast<double>(effect.luma_key.mix);
      }
      const auto sampled = sample_channel_at(
          channel_for(keyframes, effect.source, property), timeline.show_frame);
      const double displayed_value =
          sampled.has_value() && std::isfinite(*sampled) ? *sampled
                                                         : authored_value;
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
                         std::string{label}, ctx.theme().keyframe, element.kind,
                         base_indent + 1U, false,
                         property, effect.source, displayed_value});
    }
  };

  // Explicit tracks and their clips arrive in compositing order: lowest first,
  // which is the one drawn first and therefore the one at the back. The list
  // shows that stack the way every other tool does - what is in front is at
  // the top - so both the bands and the clips inside them are walked backwards.
  // The implicit lane is only an authoring sentinel for "no track". Consuming
  // it here would make every loose clip display by clip_order even though a
  // loose-clip reorder authors group_order, so those clips deliberately fall
  // through to the ordinary layer stack below.
  std::set<doc::NodeId> emitted_media;
  for (auto held = lanes.rbegin(); held != lanes.rend(); ++held) {
    const scene::TimelineLane &lane = *held;
    if (lane.implicit) {
      continue;
    }
    bool matching_child{};
    for (const ElementInfo &element : elements) {
      if (element.kind == ElementKind::Media &&
          element.timeline_lane == lane.id &&
          (matches(layer_name(element), search_) ||
           matches(kind_name(element.kind), search_))) {
        matching_child = true;
        break;
      }
    }
    if (!matches(lane.name, search_) && !matching_child) {
      continue;
    }
    Row header{Row::Kind::LaneHeader,
               lane.id,
               0U,
               0U,
               lane.name,
               ctx.theme().accent,
               ElementKind::Media,
               0U,
               false};
    header.locked = lane.locked;
    header.muted = lane.muted;
    header.lane_member = true;
    header.lane_owner = lane.id;
    rows.push_back(std::move(header));
    std::vector<const ElementInfo *> lane_clips;
    for (const ElementInfo &element : elements) {
      if (element.kind == ElementKind::Media &&
          element.timeline_lane == lane.id) {
        lane_clips.push_back(&element);
      }
    }
    // Higher clip_order is in front and therefore appears above lower peers.
    // Preserve the projection order for legacy files whose orders are equal.
    std::stable_sort(
        lane_clips.begin(), lane_clips.end(),
        [](const ElementInfo *left, const ElementInfo *right) noexcept {
          return left->clip_order > right->clip_order;
        });
    for (const ElementInfo *element : lane_clips) {
      emitted_media.insert(element->node);
      const std::size_t first_row = rows.size();
      append_element(*element, 1U);
      // A clip's property rows belong to its lane too, so dropping onto an
      // expanded clip's Opacity row is not a hole in the target band.
      for (std::size_t row = first_row; row < rows.size(); ++row) {
        rows[row].lane_member = true;
        rows[row].lane_owner = lane.id;
      }
    }
  }
  std::vector<const ElementInfo *> ordinary_layers;
  std::vector<const ElementInfo *> unordered_layers;
  for (const ElementInfo &element : elements) {
    if (element.kind == ElementKind::Media &&
        emitted_media.contains(element.node)) {
      continue;
    }
    (orderable_layer(element) ? ordinary_layers : unordered_layers)
        .push_back(&element);
  }
  // group_order is a sibling order, not a hierarchy operation. Equal legacy
  // defaults retain their projected order; an authored reorder gets an
  // unambiguous higher-is-front visual stack.
  std::stable_sort(
      ordinary_layers.begin(), ordinary_layers.end(),
      [](const ElementInfo *left, const ElementInfo *right) noexcept {
        return left->group_order > right->group_order;
      });
  ordinary_layers.insert(ordinary_layers.end(), unordered_layers.begin(),
                         unordered_layers.end());
  std::set<doc::NodeId> layer_nodes;
  for (const ElementInfo *element : ordinary_layers) {
    layer_nodes.insert(element->node);
  }
  std::map<doc::NodeId, std::vector<const ElementInfo *>> children;
  for (const ElementInfo *element : ordinary_layers) {
    const doc::NodeId parent = layer_parent(*element);
    children[parent != element->node && layer_nodes.contains(parent)
                 ? parent : doc::NodeId{}].push_back(element);
  }

  // Parent-first traversal retains the outliner's linked sibling order and
  // applies authored group_order only among peers. Keep disconnected nodes
  // visible too; a missing parent or a malformed cycle must not lose a row.
  constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();
  struct TreeEntry final {
    const ElementInfo *element{};
    std::size_t parent{std::numeric_limits<std::size_t>::max()};
    std::size_t depth{};
    bool matching{};
    bool visible{};
  };
  std::vector<TreeEntry> tree;
  tree.reserve(ordinary_layers.size());
  std::set<doc::NodeId> visited;
  const auto append_tree = [&](const ElementInfo *root) {
    std::vector<TreeEntry> pending{TreeEntry{root}};
    while (!pending.empty()) {
      TreeEntry entry = pending.back();
      pending.pop_back();
      if (!visited.insert(entry.element->node).second) {
        continue;
      }
      const std::size_t index = tree.size();
      entry.matching = matches(layer_name(*entry.element), search_) ||
                       matches(kind_name(entry.element->kind), search_) ||
                       (entry.parent != no_parent && tree[entry.parent].matching);
      tree.push_back(entry);
      if (const auto found = children.find(entry.element->node);
          found != children.end()) {
        for (auto child = found->second.rbegin(); child != found->second.rend();
             ++child) {
          pending.push_back(TreeEntry{*child, index, entry.depth + 1U});
        }
      }
    }
  };
  if (const auto roots = children.find(doc::NodeId{}); roots != children.end()) {
    for (const ElementInfo *root : roots->second) {
      append_tree(root);
    }
  }
  for (const ElementInfo *element : ordinary_layers) {
    if (!visited.contains(element->node)) {
      append_tree(element);
    }
  }
  for (std::size_t index = tree.size(); index-- > 0U;) {
    const TreeEntry &entry = tree[index];
    if (entry.matching && entry.parent != no_parent) {
      tree[entry.parent].matching = true;
    }
  }
  if (anchor != hierarchy_selection_) {
    hierarchy_selection_ = anchor;
    for (const TreeEntry &entry : tree) {
      if (entry.element->node == anchor) {
        for (std::size_t parent = entry.parent; parent != no_parent;
             parent = tree[parent].parent) {
          expanded_hierarchies_.insert(tree[parent].element->node);
        }
        break;
      }
    }
  }
  for (std::size_t index = 0U; index < tree.size(); ++index) {
    TreeEntry &entry = tree[index];
    entry.visible = entry.matching &&
        (entry.parent == no_parent ||
         (tree[entry.parent].visible &&
          (!search_.empty() || expanded_hierarchies_.contains(
                                  tree[entry.parent].element->node))));
    if (entry.visible) {
      const bool has_children = index + 1U < tree.size() &&
                                tree[index + 1U].parent == index;
      append_element(*entry.element, entry.depth, has_children, true);
    }
  }
  for (const output::ProjectionSurface &surface : surfaces) {
    if (!matches(surface.name, search_) &&
        !matches("LED output physical surface", search_)) {
      continue;
    }
    Row surface_row{Row::Kind::Layer,
                    surface.node,
                    0U,
                    0U,
                    surface.name.empty() ? "Surface" : surface.name,
                    ctx.theme().accent,
                    ElementKind::Object,
                    0U,
                    true};
    surface_row.surface = true;
    rows.push_back(std::move(surface_row));
    if (expanded_layers_.count(surface.node.value()) == 0U) {
      continue;
    }

    Row position{
        Row::Kind::Group,       surface.node,        0U, 0U,  "Position",
        ctx.theme().text_muted, ElementKind::Object, 1U, true};
    position.surface = true;
    position.component_count = 3U;
    rows.push_back(std::move(position));
    if (expanded_groups_.count(group_id(surface.node, 0U)) != 0U) {
      for (core::u8 component = 0U; component < 3U; ++component) {
        Row one{Row::Kind::Component,
                surface.node,
                0U,
                component,
                std::string{component_names[0U][component]},
                ctx.theme().text_muted,
                ElementKind::Object,
                2U,
                false};
        one.surface = true;
        rows.push_back(std::move(one));
      }
    }

    Row rotation{
        Row::Kind::Group,       surface.node,        1U, 0U,  "Rotation",
        ctx.theme().text_muted, ElementKind::Object, 1U, true};
    rotation.surface = true;
    rotation.component_count = 1U;
    rows.push_back(std::move(rotation));
    if (expanded_groups_.count(group_id(surface.node, 1U)) != 0U) {
      Row yaw{Row::Kind::Component,   surface.node,        1U, 0U,   "Yaw",
              ctx.theme().text_muted, ElementKind::Object, 2U, false};
      yaw.surface = true;
      rows.push_back(std::move(yaw));
    }
  }
  for (Row &row : rows) {
    if (row.surface) {
      const output::ProjectionSurface *const surface =
          find_surface(surfaces, row.node);
      row.locked = surface != nullptr && surface->locked;
    } else if (row.kind != Row::Kind::LaneHeader) {
      const ElementInfo *const element = find_element(elements, row.node);
      row.locked = element != nullptr && element->locked;
    }
  }
  std::vector<doc::NodeId> visible_order;
  visible_order.reserve(elements.size());
  for (const Row &row : rows) {
    if ((row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader) &&
        !row.surface && !row.locked &&
        std::find(visible_order.begin(), visible_order.end(), row.node) ==
            visible_order.end()) {
      visible_order.push_back(row.node);
    }
  }

  const auto capture_layer_drag = [&](doc::NodeId grabbed) {
    layer_drag_nodes_.clear();
    if (!selected(grabbed, selection)) {
      layer_drag_nodes_.push_back(grabbed);
      return;
    }
    const auto append_selected = [&](doc::NodeId node) {
      if (!selected(node, selection)) {
        return;
      }
      const ElementInfo *const element = find_element(elements, node);
      const bool layer = element != nullptr && !element->locked &&
                         (orderable_layer(*element) ||
                          element->kind == ElementKind::Track);
      const bool track = std::any_of(lanes.begin(), lanes.end(),
          [&](const scene::TimelineLane &lane) {
            return !lane.implicit && !lane.locked && lane.id == node;
          });
      if ((layer || track) &&
          std::find(layer_drag_nodes_.begin(), layer_drag_nodes_.end(), node) ==
              layer_drag_nodes_.end()) {
        layer_drag_nodes_.push_back(node);
      }
    };
    for (const doc::NodeId node : visible_order) {
      append_selected(node);
    }
    // A collapsed parent does not remove its children from selection.
    for (const ElementInfo &element : elements) {
      append_selected(element.node);
    }
  };

  const auto target_locked = [&](doc::NodeId target) noexcept {
    if (const ElementInfo *const element = find_element(elements, target)) {
      return element->locked;
    }
    if (const output::ProjectionSurface *const surface =
            find_surface(surfaces, target)) {
      return surface->locked;
    }
    const auto effect = std::find_if(
        effects.begin(), effects.end(),
        [target](const auto &candidate) { return candidate.source == target; });
    if (effect != effects.end()) {
      const ElementInfo *const element = find_element(elements, effect->target);
      return element != nullptr && element->locked;
    }
    return false;
  };
  selected_keys_.erase(std::remove_if(selected_keys_.begin(),
                                      selected_keys_.end(),
                                      [&](const SelectedKey &key) {
                                        return target_locked(key.target);
                                      }),
                       selected_keys_.end());
  if (active_channel_.has_value() && target_locked(active_channel_->target)) {
    active_channel_.reset();
    key_value_field_.cancel(ctx);
  }
  drag_keys_.erase(std::remove_if(drag_keys_.begin(), drag_keys_.end(),
                                  [&](const SelectedKey &key) {
                                    return target_locked(key.target);
                                  }),
                   drag_keys_.end());
  if (drag_keys_.empty()) {
    dragging_key_ = false;
    duplicate_drag_ = false;
    drag_delta_ = 0;
    snap_guide_.reset();
  }
  const auto selected_addresses = [&] {
    std::vector<TimelineKeyAddress> addresses;
    addresses.reserve(selected_keys_.size());
    for (const SelectedKey &key : selected_keys_) {
      addresses.push_back(
          TimelineKeyAddress{key.target, key.component, key.frame});
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()),
                    addresses.end());
    return addresses;
  };

  const float content_height = static_cast<float>(rows.size()) * row_height;
  const float maximum_row_scroll =
      std::max(content_height - rows_bounds.height, 0.0F);

  // Wheel gestures have the compositor convention: over the graph, wheel
  // zooms about the pointer and Shift+wheel pans. In curve mode Alt+wheel
  // zooms values and Ctrl+wheel pans values; over the layer list it scrolls
  // rows. Time and value axes therefore remain independently addressable.
  if (bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.pointer_y() >= ruler.y && ctx.wheel() != 0.0F) {
    if (graph_mode_ && ctx.pointer_x() >= graph.x && ctx.alt_held()) {
      // Hold the value under the pointer still, exactly as the time zoom below
      // holds the frame under it. A curve band is normalised inside its own
      // row, so the fixed point is where the pointer sits in that row's band:
      // with normal = 0.5 + deviation * zoom + pan, keeping normal fixed while
      // zoom scales by k means pan becomes pan * k + (normal - 0.5) * (1 - k).
      const float previous = curve_value_zoom_;
      curve_value_zoom_ = std::clamp(
          curve_value_zoom_ * static_cast<float>(std::pow(
                                  1.15, static_cast<double>(ctx.wheel()))),
          0.25F, 16.0F);
      const float band = row_height - 8.0F;
      if (previous > 0.0F && band > 0.0F &&
          ctx.pointer_y() >= rows_bounds.y) {
        const float local = ctx.pointer_y() - rows_bounds.y + row_scroll_;
        const float row_top =
            rows_bounds.y - row_scroll_ +
            std::floor(local / row_height) * row_height;
        const float normal =
            std::clamp((ctx.pointer_y() - row_top - 4.0F) / band, 0.0F, 1.0F);
        const float ratio = curve_value_zoom_ / previous;
        curve_value_pan_ =
            std::clamp(curve_value_pan_ * ratio +
                           (normal - 0.5F) * (1.0F - ratio),
                       -4.0F, 4.0F);
      }
    } else if (graph_mode_ && ctx.pointer_x() >= graph.x && ctx.ctrl_held()) {
      curve_value_pan_ =
          std::clamp(curve_value_pan_ + ctx.wheel() * 0.08F, -4.0F, 4.0F);
    } else if (ctx.shift_held()) {
      left_frame_ -= static_cast<double>(ctx.wheel()) * 80.0 /
                     static_cast<double>(pixels_per_frame_);
    } else if (ctx.pointer_x() >= graph.x) {
      const double under_pointer = x_to_frame(ctx.pointer_x());
      const double factor = std::pow(1.15, static_cast<double>(ctx.wheel()));
      pixels_per_frame_ = std::clamp(
          static_cast<float>(static_cast<double>(pixels_per_frame_) * factor),
          minimum_pixels_per_frame, maximum_pixels_per_frame);
      left_frame_ =
          under_pointer - static_cast<double>(ctx.pointer_x() - graph.x) /
                              static_cast<double>(pixels_per_frame_);
    } else {
      row_scroll_ -= ctx.wheel() * row_height * 3.0F;
    }
  }

  const bool began_pan = ctx.middle_pointer_pressed();
  if (began_pan) {
    panning_ = graph.contains(ctx.pointer_x(), ctx.pointer_y());
    if (panning_) {
      timeline_focused_ = true;
    }
  }
  if (panning_ && ctx.middle_pointer_down() && !began_pan) {
    left_frame_ -= static_cast<double>(ctx.pointer_dx()) /
                   static_cast<double>(pixels_per_frame_);
    if (graph_mode_ && ctx.ctrl_held()) {
      curve_value_pan_ =
          std::clamp(curve_value_pan_ + ctx.pointer_dy() / 100.0F, -4.0F, 4.0F);
    } else {
      row_scroll_ -= ctx.pointer_dy();
    }
  }
  if (ctx.middle_pointer_released()) {
    panning_ = false;
  }
  // In/Out delimit playback, not the editable workspace. Signed placement
  // positions let an operator move a clip partly before frame zero without
  // trimming away its source; both ends of the workspace remain reachable.
  const double draw_delta = std::clamp(
      ctx.time_seconds() - previous_draw_time_, 0.0, 0.05);
  previous_draw_time_ = ctx.time_seconds();
  if (clip_drag_ != ClipDrag::None && clip_drag_moved_ && ctx.pointer_down()) {
    constexpr float edge_zone = 24.0F;
    float velocity = 0.0F;
    if (ctx.pointer_x() < graph.x + edge_zone) {
      velocity = -std::clamp((graph.x + edge_zone - ctx.pointer_x()) /
                                edge_zone, 0.0F, 1.0F);
    } else if (ctx.pointer_x() > graph.x + graph.width - edge_zone) {
      velocity = std::clamp((ctx.pointer_x() - graph.x - graph.width +
                             edge_zone) / edge_zone, 0.0F, 1.0F);
    }
    left_frame_ += static_cast<double>(velocity) * 720.0 * draw_delta /
                   static_cast<double>(pixels_per_frame_);
  }
  left_frame_ = std::clamp(left_frame_,
      -static_cast<double>(std::numeric_limits<core::i64>::max() / 4),
      static_cast<double>(std::numeric_limits<core::i64>::max() / 4));
  row_scroll_ = std::clamp(row_scroll_, 0.0F, maximum_row_scroll);

  // The panel owns visibility, so it is the only layer that can truthfully
  // distinguish visible work from adjacent-row prefetch. Keys are still
  // deduplicated by full derivative identity: two clips borrowing one Asset
  // revision schedule and retain one atlas.
  const auto filmstrip_for_clip = [&](doc::NodeId clip) noexcept
      -> const TimelineFilmstripObservation * {
    const auto found = std::find_if(
        filmstrips.begin(), filmstrips.end(),
        [clip](const TimelineFilmstripObservation &observation) noexcept {
          return observation.clip == clip;
        });
    return found != filmstrips.end() ? std::addressof(*found) : nullptr;
  };
  const auto waveform_for_clip = [&](doc::NodeId clip) noexcept
      -> const TimelineWaveformObservation * {
    const auto found = std::find_if(
        waveforms.begin(), waveforms.end(),
        [clip](const TimelineWaveformObservation &observation) noexcept {
          return observation.clip == clip;
        });
    return found != waveforms.end() ? std::addressof(*found) : nullptr;
  };
  TimelineFilmstripRetainIntent &filmstrip_retain = *frame.filmstrip_retain;
  const auto append_filmstrip_candidate =
      [&](const TimelineFilmstripObservation &observation,
          TimelineFilmstripRequestPriority priority) {
        if (std::find(filmstrip_retain.keys.begin(),
                      filmstrip_retain.keys.end(), observation.key) ==
            filmstrip_retain.keys.end()) {
          filmstrip_retain.keys.push_back(observation.key);
        }
        const bool needs_request =
            observation.state == assets::DerivativeState::Missing ||
            observation.state == assets::DerivativeState::Stale ||
            observation.state == assets::DerivativeState::Cancelled;
        if (!needs_request) {
          return;
        }
        auto existing = std::find_if(
            frame.filmstrip_requests.begin(), frame.filmstrip_requests.end(),
            [&](const TimelineFilmstripRequestIntent &request) noexcept {
              return request.key == observation.key;
            });
        if (existing != frame.filmstrip_requests.end()) {
          if (priority == TimelineFilmstripRequestPriority::Visible) {
            existing->priority = priority;
          }
          return;
        }
        if (frame.filmstrip_requests.size() < maximum_filmstrip_requests) {
          frame.filmstrip_requests.push_back(
              TimelineFilmstripRequestIntent{observation.key, priority});
        }
      };
  TimelineWaveformRetainIntent &waveform_retain = *frame.waveform_retain;
  const auto append_waveform_candidate =
      [&](const TimelineWaveformObservation &observation,
          TimelineWaveformRequestPriority priority) {
        if (std::find(waveform_retain.keys.begin(), waveform_retain.keys.end(),
                      observation.key) == waveform_retain.keys.end()) {
          waveform_retain.keys.push_back(observation.key);
        }
        const bool needs_request =
            observation.state == assets::DerivativeState::Missing ||
            observation.state == assets::DerivativeState::Stale ||
            observation.state == assets::DerivativeState::Cancelled;
        if (!needs_request) {
          return;
        }
        auto existing = std::find_if(
            frame.waveform_requests.begin(), frame.waveform_requests.end(),
            [&](const TimelineWaveformRequestIntent &request) noexcept {
              return request.key == observation.key;
            });
        if (existing != frame.waveform_requests.end()) {
          if (priority == TimelineWaveformRequestPriority::Visible) {
            existing->priority = priority;
          }
          return;
        }
        if (frame.waveform_requests.size() < maximum_waveform_requests) {
          frame.waveform_requests.push_back(
              TimelineWaveformRequestIntent{observation.key, priority});
        }
      };
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    if (row.kind != Row::Kind::Layer || row.surface ||
        row.element_kind != ElementKind::Media) {
      continue;
    }
    const ElementInfo *const element = find_element(elements, row.node);
    const TimelineFilmstripObservation *const observation =
        element != nullptr && element->timing.placed()
            ? filmstrip_for_clip(row.node)
            : nullptr;
    const TimelineWaveformObservation *const waveform =
        element != nullptr && element->timing.placed()
            ? waveform_for_clip(row.node)
            : nullptr;
    if (observation == nullptr && waveform == nullptr) {
      continue;
    }
    const float row_top =
        rows_bounds.y + static_cast<float>(index) * row_height - row_scroll_;
    const float row_bottom = row_top + row_height;
    const bool visible = row_bottom > rows_bounds.y &&
                         row_top < rows_bounds.y + rows_bounds.height;
    std::size_t distance{};
    if (!visible) {
      const float gap = row_bottom <= rows_bounds.y
                            ? rows_bounds.y - row_bottom
                            : row_top - (rows_bounds.y + rows_bounds.height);
      distance = static_cast<std::size_t>(
          std::floor(std::max(gap, 0.0F) / row_height)) +
                 1U;
    }
    if (observation != nullptr &&
        (visible || distance <= filmstrip_prefetch_rows)) {
      append_filmstrip_candidate(
          *observation,
          visible ? TimelineFilmstripRequestPriority::Visible
                  : TimelineFilmstripRequestPriority::Prefetch);
    }
    if (waveform != nullptr &&
        (visible || distance <= waveform_prefetch_rows)) {
      append_waveform_candidate(
          *waveform,
          visible ? TimelineWaveformRequestPriority::Visible
                  : TimelineWaveformRequestPriority::Prefetch);
    }
  }

  // ---------------------------------------------------------------------
  // Toolbar. Left to right: transport, where the show is, its rate, the
  // pointer tools, view toggles, and the two things the timeline creates at
  // the playhead. GO and the cue arm sit hard against the right edge, where a
  // hand running a live show finds them without reading; exact In/Out entry
  // takes the space between when there is room for it.
  const gui::Theme &theme = ctx.theme();
  ctx.panel(toolbar, theme.header);
  ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                      toolbar.width, 1.0F},
            theme.divider);
  const float control_y = toolbar.y + 6.0F;
  constexpr float button_height = 22.0F;
  const bool show_key_tools = !keyframes.empty() || !selected_keys_.empty();

  float right_x = toolbar.x + toolbar.width - 6.0F;
  gui::Rect go_button{};
  gui::Rect armed_button{};
  if (any_cue) {
    go_button = gui::Rect{right_x - 100.0F, control_y, 100.0F, button_height};
    armed_button = gui::Rect{go_button.x - 4.0F - icon_button_width,
                             control_y, icon_button_width, button_height};
    right_x = armed_button.x - 8.0F;
  }
  // Everything left of the marker group, measured from the panel's edge.
  const float left_extent = show_key_tools ? 740.0F : 692.0F;
  constexpr float range_field_width = 96.0F;
  const bool show_range =
      right_x - (toolbar.x + left_extent) >= range_field_width * 2.0F + 12.0F;
  gui::Rect in_field{};
  gui::Rect out_field{};
  if (show_range) {
    out_field = gui::Rect{right_x - range_field_width, control_y,
                          range_field_width, button_height};
    in_field = gui::Rect{out_field.x - 4.0F - range_field_width, control_y,
                         range_field_width, button_height};
    right_x = in_field.x - 8.0F;
  }
  const auto fits = [&](float x, float width) noexcept {
    return x + width <= right_x;
  };

  // A time field: drag sideways to scrub a frame per pixel, click to type a
  // timecode or a frame count. Reports a value on every frame a drag changes
  // it, and once when typed text is committed.
  const auto time_field = [&](gui::UiId id, const gui::Rect &field,
                              std::string_view caption, core::u64 value,
                              core::u64 maximum,
                              gui::UiColor color) -> std::optional<core::u64> {
    const gui::UiId text_id = mix(id, "text");
    std::optional<core::u64> result;
    if (time_edit_id_ == id) {
      static_cast<void>(ctx.text_field(text_id, field, time_draft_));
      if (ctx.focus() != text_id) {
        // Enter, Tab, or a press elsewhere commits; Escape has already put
        // the original text back, which parses to the value unchanged.
        if (const auto parsed =
                parse_timecode(time_draft_, timeline.rate_numerator,
                               timeline.rate_denominator, value);
            parsed.has_value() && std::min(*parsed, maximum) != value) {
          result = std::min(*parsed, maximum);
        }
        time_edit_id_ = gui::UiId{};
        time_draft_.clear();
      }
      return result;
    }
    const gui::Interaction interaction = ctx.grab(id, field);
    if (interaction.pressed) {
      time_drag_id_ = id;
      time_drag_start_x_ = ctx.pointer_press_x();
      time_drag_origin_ = value;
      time_drag_moved_ = false;
    }
    if (time_drag_id_ == id && interaction.held) {
      const float dx = ctx.pointer_x() - time_drag_start_x_;
      time_drag_moved_ = time_drag_moved_ || std::fabs(dx) >= 3.0F;
      if (time_drag_moved_) {
        const core::i64 moved =
            signed_frame(time_drag_origin_) + std::llround(dx);
        const core::u64 next = static_cast<core::u64>(std::clamp<core::i64>(
            moved, 0, signed_frame(maximum)));
        if (next != value) {
          result = next;
        }
      }
      if (interaction.released) {
        if (!time_drag_moved_ && interaction.clicked) {
          time_edit_id_ = id;
          time_draft_ = timecode(value);
          ctx.focus_text_field(text_id, time_draft_);
        }
        time_drag_id_ = gui::UiId{};
        time_drag_moved_ = false;
      }
    }
    ctx.panel(field, interaction.hovered ? theme.widget_hover : theme.track);
    ctx.panel(gui::Rect{field.x, field.y, 2.0F, field.height}, color);
    float text_x = field.x + 8.0F;
    if (!caption.empty()) {
      ctx.label(gui::Rect{text_x, field.y, 28.0F, field.height}, caption,
                theme.text_muted);
      text_x += ctx.font_width(caption) + 6.0F;
    }
    ctx.label(gui::Rect{text_x, field.y,
                        std::max(field.x + field.width - text_x - 2.0F, 0.0F),
                        field.height},
              timecode(value), color);
    ctx.tooltip(field, "Drag to scrub, click to type a timecode or frame");
    return result;
  };

  float bar_x = toolbar.x + 6.0F;
  const auto transport_icon = [&](std::string_view name, float width,
                                  gui::Icon icon, TransportRequest request,
                                  std::string_view tip, bool engaged = false) {
    if (fits(bar_x, width) &&
        icon_button(ctx, mix(id_, name),
                    gui::Rect{bar_x, control_y, width, button_height}, icon,
                    engaged, tip, theme.playhead)) {
      frame.transport = request;
    }
    bar_x += width + 2.0F;
  };
  transport_icon("to start", icon_button_width, gui::Icon::ToStart,
                 TransportRequest::GoToStart, "Go to In (Home)");
  transport_icon("step back", icon_button_width, gui::Icon::StepBack,
                 TransportRequest::StepBack,
                 "Back one frame (Left), one second (Shift+Left)");
  transport_icon("play", 30.0F,
                 timeline.playing ? gui::Icon::Pause : gui::Icon::Play,
                 TransportRequest::TogglePlay, "Play / Pause (Space)",
                 timeline.playing);
  transport_icon("step forward", icon_button_width, gui::Icon::StepForward,
                 TransportRequest::StepForward,
                 "Forward one frame (Right), one second (Shift+Right)");
  transport_icon("to end", icon_button_width, gui::Icon::ToEnd,
                 TransportRequest::GoToEnd, "Go to Out (End)");
  bar_x += 2.0F;
  if (fits(bar_x, icon_button_width) &&
      icon_button(ctx, mix(id_, "loop"),
                  gui::Rect{bar_x, control_y, icon_button_width, button_height},
                  gui::Icon::Loop, timeline.loop,
                  "Loop the In/Out range", theme.accent)) {
    frame.range = RangeRequest{first, last, !timeline.loop};
  }
  bar_x += icon_button_width + 8.0F;

  // Where the Stage playhead is. Typing a time goes there; dragging scrubs.
  constexpr float readout_width = 110.0F;
  if (fits(bar_x, readout_width)) {
    if (const auto moved = time_field(
            mix(id_, "playhead time"),
            gui::Rect{bar_x, control_y, readout_width, button_height}, {},
            timeline.show_frame, std::max(last, timeline.show_frame),
            theme.playhead)) {
      frame.seek_to_frame = std::clamp(*moved, first, last);
    }
  }
  bar_x += readout_width + 4.0F;

  constexpr float rate_width = 70.0F;
  gui::Rect rate_button{};
  gui::Rect rate_popup{};
  bool popup_pointer_blocked = false;
  if (fits(bar_x, rate_width)) {
    rate_button = gui::Rect{bar_x, control_y, rate_width, button_height};
    if (ctx.button(mix(id_, "show rate"), rate_button, rate_label(timeline))) {
      rate_menu_open_ = !rate_menu_open_;
      key_tools_open_ = false;
    }
    constexpr float popup_width = 104.0F;
    constexpr float choice_height = 21.0F;
    rate_popup = gui::Rect{rate_button.x, toolbar.y + toolbar.height - 2.0F,
                           popup_width,
                           8.0F + choice_height *
                                      static_cast<float>(common_rates.size())};
    if (rate_menu_open_ && ctx.pointer_pressed() &&
        !rate_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        !rate_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
      rate_menu_open_ = false;
    }
    if (rate_menu_open_ &&
        rate_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
      // Process and draw the menu after the timeline. Until then, mask
      // the graph underneath it so choosing a rate cannot also scrub.
      ctx.push_pointer_block();
      popup_pointer_blocked = true;
    }
  } else {
    rate_menu_open_ = false;
  }
  bar_x += rate_width + 4.0F;

  // The pointer tools. Each acts on the clip it is pressed on.
  struct ToolChoice final {
    TimelineEditTool tool;
    gui::Icon icon;
    std::string_view name;
    std::string_view tip;
  };
  constexpr std::array<ToolChoice, 6U> tool_choices{{
      {TimelineEditTool::Standard, gui::Icon::Select, "tool select",
       "Selection: move, trim and fade clips (V)"},
      {TimelineEditTool::RippleTrim, gui::Icon::Ripple, "tool ripple",
       "Ripple Trim: trim an edge and pull what follows along (B)"},
      {TimelineEditTool::Roll, gui::Icon::Roll, "tool roll",
       "Roll: move the cut between two butted clips (N)"},
      {TimelineEditTool::Slip, gui::Icon::Slip, "tool slip",
       "Slip: change the source frames a clip shows (Y)"},
      {TimelineEditTool::Slide, gui::Icon::Slide, "tool slide",
       "Slide: move a clip between its neighbours (U)"},
      {TimelineEditTool::Razor, gui::Icon::Razor, "tool razor",
       "Razor: cut a clip where clicked; Shift cuts every track (C)"},
  }};
  draw_separator(ctx, bar_x, toolbar);
  bar_x += 4.0F;
  for (const ToolChoice &choice : tool_choices) {
    // The enlarged marks still fit the existing width, keeping transport and
    // editing controls reachable at the same compact panel sizes.
    constexpr float tool_width = icon_button_width;
    constexpr float tool_height = button_height * edit_tool_scale;
    if (fits(bar_x, tool_width) &&
        icon_button(ctx, mix(id_, choice.name),
                    gui::Rect{bar_x, toolbar.y + (toolbar.height - tool_height) * 0.5F,
                              tool_width, tool_height},
                    choice.icon, edit_tool_ == choice.tool, choice.tip,
                    theme.accent, edit_tool_scale)) {
      edit_tool_ = choice.tool;
    }
    bar_x += tool_width + 2.0F;
  }
  bar_x += 2.0F;
  draw_separator(ctx, bar_x, toolbar);
  bar_x += 4.0F;

  const auto zoom_to_fit = [&]() noexcept {
    const double span =
        std::max(static_cast<double>(last) - static_cast<double>(first), 1.0);
    pixels_per_frame_ = std::clamp(
        static_cast<float>(static_cast<double>(graph.width) / span),
        minimum_pixels_per_frame, maximum_pixels_per_frame);
    left_frame_ = static_cast<double>(first);
  };
  if (fits(bar_x, icon_button_width) &&
      icon_button(ctx, mix(id_, "snap"),
                  gui::Rect{bar_x, control_y, icon_button_width, button_height},
                  gui::Icon::Snap, snapping_,
                  "Snapping to the playhead, cuts, keys and markers (S)",
                  theme.accent)) {
    snapping_ = !snapping_;
  }
  bar_x += icon_button_width + 2.0F;
  if (fits(bar_x, icon_button_width) &&
      icon_button(ctx, mix(id_, "fit"),
                  gui::Rect{bar_x, control_y, icon_button_width, button_height},
                  gui::Icon::Fit, false, "Zoom to fit the range (\\)",
                  theme.accent)) {
    zoom_to_fit();
  }
  bar_x += icon_button_width + 2.0F;
  constexpr float curves_width = 52.0F;
  if (fits(bar_x, curves_width) &&
      text_toggle(ctx, mix(id_, "graph mode"),
                  gui::Rect{bar_x, control_y, curves_width, button_height},
                  "Curves", graph_mode_,
                  "Draw animated values as curves instead of keys")) {
    graph_mode_ = !graph_mode_;
  }
  bar_x += curves_width + 2.0F;

  // Key selection tools live behind one button, shown only when there is
  // animation to use them on.
  constexpr float keys_width = 46.0F;
  gui::Rect key_tools_button{};
  if (show_key_tools && fits(bar_x, keys_width)) {
    key_tools_button = gui::Rect{bar_x, control_y, keys_width, button_height};
    if (text_toggle(ctx, mix(id_, "key tools"), key_tools_button, "Keys",
                    key_tools_open_,
                    "Key selection, value and time-scale tools")) {
      key_tools_open_ = !key_tools_open_;
      rate_menu_open_ = false;
      if (!key_tools_open_) {
        scale_open_ = false;
      }
    }
    bar_x += keys_width + 2.0F;
  } else {
    key_tools_open_ = false;
    scale_open_ = false;
  }
  const float key_popup_width = std::min(338.0F, bounds.width);
  const gui::Rect key_tools_popup{
      std::clamp(key_tools_button.x, bounds.x,
                 std::max(bounds.x, bounds.x + bounds.width - key_popup_width)),
      toolbar.y + toolbar.height - 2.0F, key_popup_width, 224.0F};
  const gui::UiId scale_numerator_id = mix(id_, "scale numerator");
  const gui::UiId scale_denominator_id = mix(id_, "scale denominator");
  const gui::UiId scale_user_anchor_id = mix(id_, "scale user anchor");
  if (key_tools_open_ && ctx.pointer_pressed() &&
      !key_tools_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !key_tools_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    if (ctx.focus() == scale_numerator_id ||
        ctx.focus() == scale_denominator_id ||
        ctx.focus() == scale_user_anchor_id) {
      ctx.clear_focus();
    }
    key_tools_open_ = false;
    scale_open_ = false;
  }
  bool key_popup_pointer_blocked = false;
  if (key_tools_open_ &&
      key_tools_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    ctx.push_pointer_block();
    key_popup_pointer_blocked = true;
  }
  bar_x += 2.0F;
  draw_separator(ctx, bar_x, toolbar);
  bar_x += 4.0F;

  // New markers take the next number and a colour that says what they are:
  // plain markers green, holds red, loops purple. The operator recolours and
  // renames from the marker itself.
  const auto create_marker = [&](core::i64 at, scene::CueAction cue) {
    TimelineMarkerEdit edit{};
    edit.action = TimelineMarkerAction::Create;
    edit.marker.frame = std::max<core::i64>(at, 0);
    edit.marker.cue = cue;
    if (cue == scene::CueAction::None) {
      const auto plain = static_cast<std::size_t>(std::count_if(
          markers.begin(), markers.end(),
          [](const scene::TimelineMarker &marker) noexcept {
            return !marker.is_cue();
          }));
      edit.marker.name = "Marker " + std::to_string(plain + 1U);
      edit.marker.color = 3U;
    } else {
      edit.marker.color = cue == scene::CueAction::Hold ? 0U : 6U;
    }
    frame.marker_edits.push_back(std::move(edit));
  };
  // The playhead an operator just put somewhere is where the next key acts,
  // even before the Director's answer has come back round: a click on the
  // ruler followed at once by Q must cue the frame clicked, not the one the
  // playhead was leaving.
  if (requested_playhead_.has_value() &&
      (timeline.show_frame == *requested_playhead_ || timeline.playing ||
       requested_playhead_frames_ == 0U)) {
    requested_playhead_.reset();
  } else if (requested_playhead_.has_value()) {
    --requested_playhead_frames_;
  }
  const core::u64 acting_frame =
      requested_playhead_.value_or(timeline.show_frame);
  const core::i64 playhead = signed_frame(acting_frame);
  // Snapping pulls onto the playhead the operator can see.
  const core::i64 shown_playhead = signed_frame(timeline.show_frame);
  if (fits(bar_x, icon_button_width) &&
      icon_button(ctx, mix(id_, "add marker"),
                  gui::Rect{bar_x, control_y, icon_button_width, button_height},
                  gui::Icon::Marker, false, "Add a marker at the playhead (M)",
                  theme.accent)) {
    create_marker(playhead, scene::CueAction::None);
  }
  bar_x += icon_button_width + 2.0F;
  if (fits(bar_x, icon_button_width) &&
      icon_button(ctx, mix(id_, "add cue"),
                  gui::Rect{bar_x, control_y, icon_button_width, button_height},
                  gui::Icon::Hold, false,
                  "Add a hold cue at the playhead (Q); Shift+Q adds a loop",
                  theme.accent)) {
    create_marker(playhead, scene::CueAction::Hold);
  }
  bar_x += icon_button_width + 2.0F;

  if (show_range) {
    if (const auto entered = time_field(mix(id_, "range first"), in_field,
                                        "IN", first, last, theme.text)) {
      frame.range = RangeRequest{*entered, std::max(last, *entered),
                                 timeline.loop};
    }
    if (const auto entered =
            time_field(mix(id_, "range last"), out_field, "OUT", last,
                       static_cast<core::u64>(
                           std::numeric_limits<core::i64>::max()),
                       theme.text)) {
      frame.range = RangeRequest{std::min(first, *entered), *entered,
                                 timeline.loop};
    }
  }

  // The cue being acted on, and the one GO reaches next.
  const auto cue_label = [&](doc::NodeId id) {
    std::string label;
    if (const auto index = marker_index(id); index.has_value()) {
      label = cue_tag(cue_numbers[*index]);
      if (!markers[*index].name.empty()) {
        label += ' ';
        label += markers[*index].name;
      }
    }
    return label;
  };
  const auto seconds_label = [&](core::u64 frames) {
    const double rate =
        static_cast<double>(timeline.rate_numerator) /
        static_cast<double>(std::max(timeline.rate_denominator, 1U));
    std::array<char, 32U> text{};
    const int length = std::snprintf(
        text.data(), text.size(), "%.1fs",
        rate > 0.0 ? static_cast<double>(frames) / rate : 0.0);
    return std::string{text.data(),
                       static_cast<std::size_t>(std::max(length, 0))};
  };
  if (any_cue) {
    if (icon_button(ctx, mix(id_, "cues armed"), armed_button, gui::Icon::Hold,
                    timeline.cues_armed,
                    timeline.cues_armed
                        ? "Cues armed: the show stops and loops on cues"
                        : "Cues off: the show plays straight through them",
                    timeline_marker_color(0U))) {
      frame.cues_armed = !timeline.cues_armed;
    }
    // GO does something when the show is held, looping, or stopped short of
    // a cue; while it runs towards one, the button counts down instead.
    const bool running = timeline.playing && timeline.looping_cue == doc::NodeId{};
    const gui::Interaction go = ctx.grab(mix(id_, "go"), go_button);
    if (go.clicked) {
      frame.transport = TransportRequest::Go;
    }
    const gui::UiColor go_color =
        timeline.held_cue != doc::NodeId{} ? theme.playhead : theme.good;
    ctx.panel(go_button,
              running ? (go.hovered ? theme.widget_hover : theme.widget)
                      : alpha(go_color,
                              (go.held && !go.released)
                                  ? 0.55F
                                  : (go.hovered ? 0.40F : 0.28F)));
    if (!running) {
      draw_border(ctx, go_button, go_color);
    }
    std::string go_text;
    if (running) {
      go_text = timeline.next_cue != doc::NodeId{}
                    ? cue_tag(cue_numbers[marker_index(timeline.next_cue)
                                              .value_or(0U)]) +
                          " in " +
                          seconds_label(timeline.frames_to_next_cue.value_or(0U))
                    : "RUNNING";
    } else if (timeline.looping_cue != doc::NodeId{}) {
      go_text = "GO  release";
    } else if (timeline.next_cue != doc::NodeId{}) {
      go_text = "GO  " + cue_tag(cue_numbers[marker_index(timeline.next_cue)
                                                 .value_or(0U)]);
    } else {
      go_text = "GO";
    }
    ctx.label(gui::Rect{go_button.x + 9.0F, go_button.y,
                        go_button.width - 12.0F, go_button.height},
              go_text, running ? theme.text_muted : theme.text);
    ctx.tooltip(go_button,
                "GO (Enter): run from a hold, release a loop, or play to the "
                "next cue");
  }

  // A track that went away under the panel takes its selection with it.
  if (selected_lane_ != doc::NodeId{} &&
      std::none_of(lanes.begin(), lanes.end(),
                   [&](const scene::TimelineLane &lane) {
                     return !lane.implicit && lane.id == selected_lane_;
                   })) {
    selected_lane_ = doc::NodeId{};
    lane_destination_ = doc::NodeId{};
  }

  // ---------------------------------------------------------------------
  // The ruler. Its left end heads the layer list: search, and a new track.
  // Over the graph it reads in timecode, carries the In/Out range as a bar
  // with a handle at each end, and scrubs everywhere else.
  const gui::UiId search_id = mix(id_, "search");
  ctx.panel(ruler, theme.header);
  ctx.panel(gui::Rect{ruler.x, ruler.y, list_width, ruler.height},
            theme.row_alt);
  const float auto_key_width = list_width >= 160.0F ? 72.0F : 0.0F;
  const gui::Rect search_bounds{ruler.x + 5.0F, ruler.y + 2.0F,
                                std::max(list_width - 37.0F - auto_key_width, 0.0F),
                                std::max(ruler.height - 4.0F, 0.0F)};
  static_cast<void>(ctx.text_field(search_id, search_bounds, search_));
  if (search_.empty() && ctx.focus() != search_id) {
    ctx.label(gui::Rect{search_bounds.x + 19.0F, search_bounds.y,
                        std::max(search_bounds.width - 23.0F, 0.0F),
                        search_bounds.height},
              "Search Timeline...", theme.text_muted);
  }
  const float search_cy = search_bounds.y + search_bounds.height * 0.46F;
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 3.0F, theme.text_muted);
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 1.7F, theme.widget);
  ctx.draw_list().add_line(search_bounds.x + 11.0F, search_cy + 2.0F,
                           search_bounds.x + 14.0F, search_cy + 5.0F, 1.0F,
                           theme.text_muted);
  if (auto_key_width > 0.0F &&
      text_toggle(ctx, mix(id_, "auto key"),
                  gui::Rect{ruler.x + list_width - 32.0F - auto_key_width,
                            ruler.y + 2.0F, auto_key_width - 3.0F,
                            std::max(ruler.height - 4.0F, 0.0F)},
                  "Auto Key", auto_key_,
                  "Auto Key (K): edits to animated properties create keys. "
                  "Off edits base values; existing animation still plays. "
                  "Use a property diamond to add or remove a key explicitly.")) {
    auto_key_ = !auto_key_;
  }
  if (list_width >= 60.0F &&
      icon_button(ctx, mix(id_, "create lane"),
                  gui::Rect{ruler.x + list_width - 29.0F, ruler.y + 2.0F,
                            icon_button_width,
                            std::max(ruler.height - 4.0F, 0.0F)},
                  gui::Icon::Plus, false, "New track", theme.accent)) {
    frame.lane_edits.push_back(
        TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
  }
  // The edge between the layer list and the lanes is a seam the operator
  // drags: long names want a wide list, long shows want wide lanes.
  {
    // Over the rows only: on the ruler and the marker strip the same edge is
    // where the In handle and a cue at frame zero are grabbed.
    const gui::Rect seam{graph.x - 3.0F, rows_bounds.y, 6.0F,
                         rows_bounds.height};
    const gui::Interaction drag = ctx.grab(mix(id_, "list seam"), seam);
    ctx.cursor_for(drag, gui::Cursor::ResizeHorizontal);
    if (drag.held) {
      list_width_ = std::clamp(ctx.pointer_x() - bounds.x, minimum_list_width,
                               std::max(bounds.width - minimum_graph_width,
                                        minimum_list_width));
    }
    ctx.panel(gui::Rect{graph.x - (drag.held || drag.hovered ? 1.0F : 0.0F),
                        ruler.y, drag.held || drag.hovered ? 2.0F : 1.0F,
                        ruler.height + strip.height + rows_bounds.height},
              drag.held ? theme.accent
                        : (drag.hovered ? theme.text_muted : theme.border));
  }

  // Edit points: where a cut, a range end, or a marker sits. Snapping and
  // Up/Down navigation share the one list.
  const auto edit_points = [&](doc::NodeId exclude_clip,
                               bool include_markers) {
    std::vector<core::i64> points{first_i, last_i + 1};
    for (const ElementInfo &element : elements) {
      if (element.node == exclude_clip || element.kind != ElementKind::Media ||
          !element.timing.placed()) {
        continue;
      }
      points.push_back(element.timing.start);
      points.push_back(element.timing.start + element.timing.duration);
    }
    if (include_markers) {
      for (const scene::TimelineMarker &marker : markers) {
        if (std::find(selected_markers_.begin(), selected_markers_.end(),
                      marker.id) == selected_markers_.end() ||
            marker_drag_ == doc::NodeId{}) {
          points.push_back(marker.frame);
        }
      }
    }
    return points;
  };
  const core::i64 snap_tolerance = static_cast<core::i64>(std::max(
      1.0, std::ceil(8.0 / static_cast<double>(pixels_per_frame_))));
  // Where a razor pressed at `px` cuts: the frame under it, pulled onto the
  // playhead, a marker, or another cut when snapping.
  const auto razor_frame = [&](float px) {
    core::i64 cut = static_cast<core::i64>(std::llround(x_to_frame(px)));
    if (snapping_ && !ctx.alt_held()) {
      std::vector<core::i64> targets = edit_points(doc::NodeId{}, true);
      targets.push_back(shown_playhead);
      if (const auto snapped = nearest_snap(cut, targets, snap_tolerance)) {
        cut = *snapped;
      }
    }
    return cut;
  };

  ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  {
    const float minimum_spacing =
        std::max(76.0F, ctx.font_width(timecode(last)) + 14.0F);
    const core::i64 step =
        ruler_frames_ ? ruler_step(pixels_per_frame_)
                      : timecode_step(pixels_per_frame_, fps, minimum_spacing);
    const core::i64 minor = step >= static_cast<core::i64>(fps) * 2
                                ? step / 2
                                : (step > 1 ? 1 : 0);
    const core::i64 visible_begin =
        static_cast<core::i64>(std::floor(left_frame_));
    core::i64 tick = (visible_begin / step) * step;
    if (tick > visible_begin) {
      tick -= step;
    }
    for (std::size_t count = 0U; count < 512U; ++count, tick += step) {
      const float tick_x = frame_to_x(static_cast<double>(tick));
      if (tick_x > graph.x + graph.width) {
        break;
      }
      if (minor > 0 && minor * 6 >= step &&
          static_cast<float>(minor) * pixels_per_frame_ >= 6.0F) {
        for (core::i64 sub = tick + minor; sub < tick + step; sub += minor) {
          const float sub_x = frame_to_x(static_cast<double>(sub));
          if (sub_x >= graph.x) {
            ctx.draw_list().add_rect(sub_x, ruler.y + ruler.height - 3.0F,
                                     1.0F, 3.0F,
                                     alpha(theme.text_muted, 0.55F));
          }
        }
      }
      if (tick_x < graph.x) {
        continue;
      }
      ctx.draw_list().add_rect(tick_x, ruler.y + ruler.height - 7.0F, 1.0F,
                               7.0F, theme.text_muted);
      ctx.label(gui::Rect{tick_x + 4.0F, ruler.y, minimum_spacing,
                          ruler.height - 5.0F},
                tick < 0
                    ? "-" + timecode(static_cast<core::u64>(-tick))
                    : timecode(static_cast<core::u64>(tick)), theme.text_muted);
    }
  }
  ctx.pop_clip();

  // The range bar and its handles. A handle drag previews locally and
  // reports once, on release, like every other drag here.
  core::u64 shown_first = first;
  core::u64 shown_last = last;
  {
    // Each handle sits on the frame it names, where the playhead stands on
    // that frame, so a fitted view shows both.
    const float in_x = frame_to_x(static_cast<double>(first));
    const float out_x = frame_to_x(static_cast<double>(last));
    const float band_top = ruler.y + ruler.height - 9.0F;
    const gui::Rect in_grip{in_x - range_handle_reach, band_top,
                            range_handle_reach * 2.0F, 9.0F};
    const gui::Rect out_grip{out_x - range_handle_reach, band_top,
                             range_handle_reach * 2.0F, 9.0F};
    ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
    const gui::Interaction in_it = ctx.grab(mix(id_, "range in"), in_grip);
    const gui::Interaction out_it = ctx.grab(mix(id_, "range out"), out_grip);
    ctx.pop_clip();
    if (in_it.pressed) {
      range_drag_ = RangeHandle::In;
    } else if (out_it.pressed) {
      range_drag_ = RangeHandle::Out;
    }
    const gui::Interaction &held = range_drag_ == RangeHandle::In ? in_it : out_it;
    if (range_drag_ != RangeHandle::None && held.held) {
      core::i64 wanted =
          static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_x())));
      if (snapping_ && !ctx.alt_held()) {
        std::vector<core::i64> targets = edit_points(doc::NodeId{}, true);
        targets.push_back(shown_playhead);
        if (range_drag_ == RangeHandle::Out) {
          // A clip's exclusive end is the frame after its last; Out lands on
          // the last frame itself.
          for (core::i64 &target : targets) {
            target -= 1;
          }
          targets.push_back(shown_playhead);
        }
        if (const auto snapped = nearest_snap(wanted, targets, snap_tolerance)) {
          wanted = *snapped;
        }
      }
      wanted = std::max<core::i64>(wanted, 0);
      range_drag_value_ = static_cast<core::u64>(wanted);
      if (range_drag_ == RangeHandle::In) {
        shown_first = std::min(range_drag_value_, last);
      } else {
        shown_last = std::max(range_drag_value_, first);
      }
      frame.editing = true;
      if (held.released) {
        if (shown_first != first || shown_last != last) {
          frame.range = RangeRequest{shown_first, shown_last, timeline.loop};
        }
        range_drag_ = RangeHandle::None;
      }
    } else if (range_drag_ != RangeHandle::None && !held.held) {
      range_drag_ = RangeHandle::None;
    }
    ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
    const float bar_left = frame_to_x(static_cast<double>(shown_first));
    const float bar_right = frame_to_x(static_cast<double>(shown_last));
    ctx.panel(gui::Rect{bar_left, ruler.y + ruler.height - 3.0F,
                        std::max(bar_right - bar_left, 1.0F), 3.0F},
              alpha(theme.accent, timeline.loop ? 0.85F : 0.5F));
    const auto handle = [&](float handle_x, bool in, bool hot) {
      const gui::UiColor color = hot ? theme.text : theme.accent;
      ctx.panel(gui::Rect{handle_x - (in ? 0.0F : 2.0F),
                          ruler.y + ruler.height - 9.0F, 2.0F, 9.0F},
                color);
      ctx.panel(gui::Rect{in ? handle_x : handle_x - 5.0F,
                          ruler.y + ruler.height - 9.0F, 5.0F, 2.0F},
                color);
    };
    handle(bar_left, true, in_it.hovered || range_drag_ == RangeHandle::In);
    handle(bar_right, false, out_it.hovered || range_drag_ == RangeHandle::Out);
    ctx.pop_clip();
  }

  // Scrubbing the ruler pauses/seeks through the shell. It follows the
  // pointer while held, including the release frame.
  const gui::Interaction ruler_it =
      ctx.grab(mix(id_, "ruler"),
               gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  if (ruler_it.held) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }

  // ---------------------------------------------------------------------
  // The marker strip. Its list end reports where the show stands against
  // its cues; over the graph each marker is a flag and each cue a numbered
  // tag, Loop cues bracketing the section they repeat.
  ctx.panel(gui::Rect{strip.x, strip.y, list_width, strip.height},
            theme.row_alt);
  ctx.panel(strip_graph, theme.background);
  ctx.panel(gui::Rect{strip.x, strip.y + strip.height - 1.0F, strip.width,
                      1.0F},
            theme.divider);
  if (any_cue) {
    std::string status;
    gui::UiColor status_color = theme.text_muted;
    if (!timeline.cues_armed) {
      status = "CUES OFF";
    } else if (timeline.held_cue != doc::NodeId{}) {
      status = "HOLD  " + cue_label(timeline.held_cue);
      status_color = theme.playhead;
    } else if (timeline.looping_cue != doc::NodeId{}) {
      status = "LOOP  " + cue_label(timeline.looping_cue);
      if (timeline.frames_to_loop_restart.has_value()) {
        status += "  again in " + seconds_label(*timeline.frames_to_loop_restart);
      }
      status_color = theme.accent;
    } else if (timeline.next_cue != doc::NodeId{}) {
      status = "NEXT  " + cue_label(timeline.next_cue);
      if (timeline.playing && timeline.frames_to_next_cue.has_value()) {
        status += "  " + seconds_label(*timeline.frames_to_next_cue);
      }
      status_color = theme.text;
    }
    ctx.label(gui::Rect{strip.x + 8.0F, strip.y,
                        std::max(list_width - 12.0F, 0.0F), strip.height},
              status, status_color);
  }

  // Where each marker draws this frame: a drag previews from the panel.
  const auto shown_marker_frame = [&](const scene::TimelineMarker &marker) {
    if (marker_drag_moved_ &&
        std::find(selected_markers_.begin(), selected_markers_.end(),
                  marker.id) != selected_markers_.end()) {
      return std::max<core::i64>(marker.frame + marker_drag_delta_, 0);
    }
    return marker.frame;
  };
  struct MarkerHit final {
    std::size_t index{};
    gui::Rect bounds{};
  };
  std::vector<MarkerHit> marker_hits;
  marker_hits.reserve(markers.size());
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    const scene::TimelineMarker &marker = markers[index];
    const float marker_x =
        frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    std::string label = marker.is_cue() ? cue_tag(cue_numbers[index]) : "";
    if (!marker.name.empty()) {
      label += label.empty() ? marker.name : " " + marker.name;
    }
    const float tag_width =
        std::clamp(ctx.font_width(label) + 14.0F, 14.0F, 180.0F);
    marker_hits.push_back(MarkerHit{
        index, gui::Rect{marker_x - marker_hit_reach, strip.y,
                         tag_width + marker_hit_reach, strip.height - 1.0F}});
  }

  // Later markers draw over earlier ones, so they are offered the press
  // first.
  std::optional<std::size_t> pressed_marker;
  std::optional<std::size_t> right_marker;
  ctx.push_clip(strip_graph);
  for (auto hit = marker_hits.rbegin(); hit != marker_hits.rend(); ++hit) {
    const gui::Interaction interaction = ctx.grab(
        mix(id_, "marker", markers[hit->index].id.value()), hit->bounds);
    if (interaction.pressed) {
      pressed_marker = hit->index;
    }
    if (!right_marker.has_value() && ctx.secondary_pointer_pressed() &&
        hit->bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        strip_graph.contains(ctx.pointer_x(), ctx.pointer_y())) {
      right_marker = hit->index;
    }
    if (markers[hit->index].id == marker_drag_ && interaction.held) {
      const float dx = ctx.pointer_x() - marker_drag_start_x_;
      marker_drag_moved_ = marker_drag_moved_ || std::fabs(dx) >= 3.0F;
      if (marker_drag_moved_) {
        const core::i64 pointer_frame =
            static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_x())));
        core::i64 delta = pointer_frame - marker_drag_anchor_;
        marker_snap_guide_.reset();
        const scene::TimelineMarker &dragged = markers[hit->index];
        if (snapping_ && !ctx.alt_held()) {
          std::vector<core::i64> targets = edit_points(doc::NodeId{}, true);
          targets.push_back(shown_playhead);
          if (const auto snapped =
                  nearest_snap(dragged.frame + delta, targets, snap_tolerance)) {
            delta = *snapped - dragged.frame;
            marker_snap_guide_ = *snapped;
          }
        }
        // No selected marker may pass frame zero.
        for (const doc::NodeId id : selected_markers_) {
          if (const auto moving = marker_index(id)) {
            delta = std::max(delta, -markers[*moving].frame);
          }
        }
        marker_drag_delta_ = delta;
      }
      frame.editing = true;
      if (interaction.released) {
        if (marker_drag_moved_ && marker_drag_delta_ != 0) {
          for (const doc::NodeId id : selected_markers_) {
            if (const auto moving = marker_index(id)) {
              TimelineMarkerEdit edit{TimelineMarkerAction::Update,
                                      markers[*moving]};
              edit.marker.frame += marker_drag_delta_;
              frame.marker_edits.push_back(std::move(edit));
            }
          }
        } else if (!marker_drag_moved_ && interaction.clicked) {
          const double now = ctx.time_seconds();
          if (marker_click_ == markers[hit->index].id &&
              now >= marker_click_time_ &&
              now - marker_click_time_ <= double_click_seconds) {
            // A second click names it. Opened on release, so the press that
            // began the click cannot close the field again.
            marker_name_node_ = marker_click_;
            marker_name_draft_ = markers[hit->index].name;
            ctx.focus_text_field(
                mix(id_, "marker name", marker_name_node_.value()),
                marker_name_draft_);
            marker_click_ = doc::NodeId{};
          } else {
            // A click goes there, so the markers are a way around the show.
            frame.seek_to_frame =
                std::clamp(static_cast<core::u64>(std::max<core::i64>(
                               markers[hit->index].frame, 0)),
                           first, last);
            marker_click_ = markers[hit->index].id;
            marker_click_time_ = now;
          }
        }
        marker_drag_ = doc::NodeId{};
        marker_drag_moved_ = false;
        marker_drag_delta_ = 0;
        marker_snap_guide_.reset();
      }
    }
  }
  ctx.pop_clip();
  if (pressed_marker.has_value()) {
    const scene::TimelineMarker &marker = markers[*pressed_marker];
    const auto found = std::find(selected_markers_.begin(),
                                 selected_markers_.end(), marker.id);
    if (ctx.ctrl_held()) {
      if (found != selected_markers_.end()) {
        selected_markers_.erase(found);
      } else {
        selected_markers_.push_back(marker.id);
      }
    } else if (ctx.shift_held()) {
      if (found == selected_markers_.end()) {
        selected_markers_.push_back(marker.id);
      }
    } else if (found == selected_markers_.end()) {
      selected_markers_.assign(1U, marker.id);
    }
    std::sort(selected_markers_.begin(), selected_markers_.end());
    // Choosing a marker leaves the key and layer domains, so Delete and the
    // clipboard aim at it rather than at whatever was selected before.
    selected_keys_.clear();
    if (std::find(selected_markers_.begin(), selected_markers_.end(),
                  marker.id) != selected_markers_.end()) {
      marker_drag_ = marker.id;
      marker_drag_start_x_ = ctx.pointer_press_x();
      marker_drag_anchor_ =
          static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_press_x())));
      marker_drag_delta_ = 0;
      marker_drag_moved_ = false;
    }
  }

  // The empty strip scrubs like the ruler, and a double-click drops a marker
  // where it lands.
  const gui::Interaction strip_it =
      ctx.grab(mix(id_, "marker strip"), strip_graph);
  if (strip_it.pressed) {
    const double now = ctx.time_seconds();
    if (now >= strip_click_time_ &&
        now - strip_click_time_ <= double_click_seconds &&
        std::fabs(ctx.pointer_x() - strip_click_x_) <= 4.0F) {
      create_marker(
          static_cast<core::i64>(rounded_frame(x_to_frame(ctx.pointer_x()),
                                               0U, std::max(last, first))),
          scene::CueAction::None);
      strip_click_time_ = -1.0;
    } else {
      strip_click_time_ = now;
      strip_click_x_ = ctx.pointer_x();
    }
    selected_markers_.clear();
  }
  if (strip_it.held) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }

  // Draw: loop brackets under everything, then flags and tags.
  ctx.push_clip(strip_graph);
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    const scene::TimelineMarker &marker = markers[index];
    if (marker.cue != scene::CueAction::Loop) {
      continue;
    }
    core::i64 section = signed_frame(first);
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (markers[earlier].is_cue() && markers[earlier].frame < marker.frame &&
          markers[earlier].frame >= signed_frame(first)) {
        section = markers[earlier].frame;
      }
    }
    const float from = frame_to_x(static_cast<double>(section));
    const float to = frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    const bool looping = timeline.looping_cue == marker.id;
    const gui::UiColor color = alpha(timeline_marker_color(marker.color),
                                     looping ? 0.95F : 0.45F);
    const float y = strip.y + strip.height - 4.0F;
    ctx.panel(gui::Rect{from, y, std::max(to - from, 1.0F), 1.0F}, color);
    ctx.panel(gui::Rect{from, y - 4.0F, 1.0F, 5.0F}, color);
    ctx.draw_list().add_triangle(from + 1.0F, y - 3.0F, from + 6.0F, y - 0.5F,
                                 from + 1.0F, y + 2.0F, color);
    // With no wait, a crossfade overlaps the end of the section: a ramp
    // over the frames that fade out, capped as the transport caps it.
    if (marker.loop_wait_ms == 0U && marker.loop_fade_ms != 0U) {
      const double rate =
          static_cast<double>(timeline.rate_numerator) /
          static_cast<double>(std::max(timeline.rate_denominator, 1U));
      const double length =
          static_cast<double>(shown_marker_frame(marker) - section + 1);
      const double fade =
          std::min(std::round(static_cast<double>(marker.loop_fade_ms) *
                              rate / 1'000.0),
                   std::floor(length / 2.0));
      if (fade >= 1.0) {
        const float ramp = frame_to_x(
            static_cast<double>(shown_marker_frame(marker)) - fade + 1.0);
        ctx.draw_list().add_triangle(ramp, y, to, y - 6.0F, to, y,
                                     alpha(color, 0.6F));
      }
    }
  }
  for (const MarkerHit &hit : marker_hits) {
    const scene::TimelineMarker &marker = markers[hit.index];
    const float marker_x =
        frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    const bool chosen =
        std::find(selected_markers_.begin(), selected_markers_.end(),
                  marker.id) != selected_markers_.end();
    const bool held = timeline.held_cue == marker.id;
    const bool upcoming = timeline.next_cue == marker.id;
    gui::UiColor color = timeline_marker_color(marker.color);
    if (held) {
      color = theme.playhead;
    }
    const float top = strip.y + 2.0F;
    const float height = strip.height - 5.0F;
    if (marker.is_cue()) {
      // A cue is a chip carrying its number, filled so it reads from across
      // a stage.
      const gui::Rect chip{marker_x, top, hit.bounds.width - marker_hit_reach,
                           height};
      ctx.panel(chip, alpha(color, chosen ? 0.95F : 0.72F));
      ctx.panel(gui::Rect{marker_x, top, 2.0F, height + 3.0F}, color);
      if (chosen || upcoming) {
        draw_border(ctx, chip, chosen ? theme.text : alpha(theme.text, 0.6F));
      }
      gui::draw_icon(ctx.draw_list(),
                     marker.cue == scene::CueAction::Hold ? gui::Icon::Hold
                                                          : gui::Icon::Loop,
                     marker_x + 8.0F, top + height * 0.5F,
                     theme.background);
    } else {
      // A plain marker is a pennant on a staff.
      ctx.panel(gui::Rect{marker_x, top, 1.5F, height + 3.0F}, color);
      ctx.draw_list().add_triangle(marker_x + 1.5F, top, marker_x + 9.0F,
                                   top + 4.0F, marker_x + 1.5F, top + 8.0F,
                                   color);
      if (chosen) {
        ctx.panel(gui::Rect{marker_x - 1.0F, top + height + 1.0F,
                            hit.bounds.width - marker_hit_reach, 1.0F},
                  theme.text);
      }
    }
    if (marker_name_node_ == marker.id) {
      continue;
    }
    std::string label = marker.is_cue() ? cue_tag(cue_numbers[hit.index]) : "";
    if (!marker.name.empty()) {
      label += label.empty() ? marker.name : " " + marker.name;
    }
    ctx.label(gui::Rect{marker_x + (marker.is_cue() ? 15.0F : 11.0F), top,
                        std::max(hit.bounds.width - marker_hit_reach -
                                     (marker.is_cue() ? 16.0F : 11.0F),
                                 0.0F),
                        height},
              label,
              marker.is_cue() ? theme.background
                              : (chosen ? theme.text : color));
    std::string tip = marker.is_cue()
                          ? (marker.cue == scene::CueAction::Hold
                                 ? "Hold cue "
                                 : "Loop cue ")
                          : "Marker ";
    tip += label;
    tip += " at ";
    tip += timecode(static_cast<core::u64>(std::max<core::i64>(marker.frame, 0)));
    if (marker.cue == scene::CueAction::Loop) {
      if (marker.loop_wait_ms != 0U) {
        tip += ", waits " + format_cue_seconds(marker.loop_wait_ms);
      }
      if (marker.loop_fade_ms != 0U) {
        tip += ", crossfades " + format_cue_seconds(marker.loop_fade_ms);
      }
    }
    if (!marker.note.empty()) {
      tip += " - ";
      tip += marker.note;
    }
    ctx.tooltip(hit.bounds, tip);
  }
  ctx.pop_clip();

  // The inline name field sits on its marker's tag.
  if (marker_name_node_ != doc::NodeId{}) {
    if (const auto index = marker_index(marker_name_node_)) {
      const gui::UiId name_id =
          mix(id_, "marker name", marker_name_node_.value());
      const float field_x = std::clamp(
          frame_to_x(static_cast<double>(markers[*index].frame)), graph.x,
          std::max(graph.x, graph.x + graph.width - 160.0F));
      static_cast<void>(ctx.text_field(
          name_id,
          gui::Rect{field_x, strip.y + 1.0F, 160.0F, strip.height - 3.0F},
          marker_name_draft_));
      if (ctx.focus() != name_id) {
        if (marker_name_draft_ != markers[*index].name &&
            marker_name_draft_.size() <= scene::max_timeline_marker_name_bytes) {
          TimelineMarkerEdit edit{TimelineMarkerAction::Update,
                                  markers[*index]};
          edit.marker.name = marker_name_draft_;
          frame.marker_edits.push_back(std::move(edit));
        }
        marker_name_node_ = doc::NodeId{};
        marker_name_draft_.clear();
      } else {
        frame.editing = true;
      }
    }
  }

  // A selected key drag follows time even when the pointer wanders into
  // another lane. Every selected diamond shares one delta, and any one of
  // them may magnetise to the playhead, range edges, or an unselected key.
  snap_guide_.reset();
  const auto update_key_drag = [&] {
    if (!dragging_key_) {
      return;
    }
    const core::i64 pointer_frame =
        signed_frame(rounded_frame(x_to_frame(gesture_x), first, last));
    core::i64 proposed_delta = pointer_frame - drag_anchor_frame_;

    core::i64 earliest = drag_anchor_frame_;
    core::i64 latest = drag_anchor_frame_;
    for (const SelectedKey &key : drag_keys_) {
      earliest = std::min(earliest, key.frame);
      latest = std::max(latest, key.frame);
    }
    proposed_delta =
        std::clamp(proposed_delta, first_i - earliest, last_i - latest);

    if (snapping_ && !duplicate_drag_ && !ctx.alt_held() &&
        !drag_keys_.empty()) {
      float best_distance = snap_distance + 1.0F;
      core::i64 best_adjustment = 0;
      std::optional<core::i64> best_target;
      const auto consider = [&](core::i64 moving, core::i64 target) noexcept {
        const core::i64 adjustment = target - moving;
        const float distance =
            std::fabs(static_cast<float>(adjustment) * pixels_per_frame_);
        if (distance <= snap_distance && distance < best_distance) {
          best_distance = distance;
          best_adjustment = adjustment;
          best_target = target;
        }
      };

      for (const SelectedKey &moving : drag_keys_) {
        const core::i64 proposed = moving.frame + proposed_delta;
        consider(proposed, signed_frame(timeline.show_frame));
        consider(proposed, first_i);
        consider(proposed, last_i);
        for (const scene::TimelineMarker &marker : markers) {
          consider(proposed, marker.frame);
        }
        for (const scene::ChannelKeyframes &channel : keyframes) {
          for (const core::i64 candidate : channel.frames) {
            const SelectedKey identity{channel.target, channel.component,
                                       candidate};
            if (std::find(drag_keys_.begin(), drag_keys_.end(), identity) ==
                drag_keys_.end()) {
              consider(proposed, candidate);
            }
          }
        }
      }
      if (best_target.has_value()) {
        proposed_delta = std::clamp(proposed_delta + best_adjustment,
                                    first_i - earliest, last_i - latest);
        snap_guide_ = best_target;
      }
    }
    drag_delta_ = proposed_delta;
  };
  update_key_drag();

  if (marquee_tracking_ && ctx.pointer_down()) {
    const float travel = std::fabs(ctx.pointer_x() - marquee_start_x_) +
                         std::fabs(ctx.pointer_y() - marquee_start_y_);
    marquee_active_ = marquee_active_ || travel > marquee_threshold;
    if (lasso_mode_) {
      const gui::UiPoint point{
          std::clamp(ctx.pointer_x(), graph.x, graph.x + graph.width),
          std::clamp(ctx.pointer_y(), rows_bounds.y,
                     rows_bounds.y + rows_bounds.height)};
      if (lasso_points_.empty()) {
        lasso_points_.push_back(
            gui::UiPoint{marquee_start_x_, marquee_start_y_});
      }
      const gui::UiPoint &previous = lasso_points_.back();
      const float dx = point.x - previous.x;
      const float dy = point.y - previous.y;
      if (dx * dx + dy * dy >= 9.0F) {
        lasso_points_.push_back(point);
      }
    }
  }

  // Every boundary a clip gesture may land on: the show range, the playhead,
  // and the ends of every other clip. Butt-joining two clips and cutting on
  // the playhead are the two edits a video timeline is asked for constantly,
  // and neither should require pixel accuracy.
  const auto snap_targets = [&](doc::NodeId exclude) {
    std::vector<core::i64> targets{first_i, last_i + 1,
                                   signed_frame(timeline.show_frame)};
    for (const ElementInfo &element : elements) {
      if (element.node == exclude || !editable_clip(element)) {
        continue;
      }
      targets.push_back(element.timing.start);
      targets.push_back(element.timing.start + element.timing.duration);
    }
    // A cue is placed against a cut and a cut against a cue, so each pulls
    // the other.
    for (const scene::TimelineMarker &marker : markers) {
      targets.push_back(marker.frame);
    }
    return targets;
  };

  // A vertical clip drop names a track band, or the ordinary top-level area
  // outside every band. Locked tracks are not destinations. Keeping this
  // resolver independent of the graph lets the same gesture begin on a media
  // row when its bar is short or off screen.
  const auto clip_lane_under_pointer = [&](float pointer_y) -> std::optional<doc::NodeId> {
    if (pointer_y < rows_bounds.y ||
        pointer_y >= rows_bounds.y + rows_bounds.height) {
      return std::nullopt;
    }
    const float local = pointer_y - rows_bounds.y + row_scroll_;
    if (local < 0.0F) {
      return std::nullopt;
    }
    const auto row_index = static_cast<std::size_t>(local / row_height);
    const doc::NodeId target =
        row_index < rows.size() && rows[row_index].lane_member
            ? rows[row_index].lane_owner
            : doc::NodeId{};
    if (target == doc::NodeId{}) {
      return target;
    }
    const auto lane = std::find_if(
        lanes.begin(), lanes.end(), [&](const scene::TimelineLane &candidate) {
          return !candidate.implicit && candidate.id == target;
        });
    if (lane == lanes.end() || lane->locked) {
      return std::nullopt;
    }
    return target;
  };

  if (pending_clip_edit_.has_value()) {
    const ElementInfo *const target =
        find_element(elements, pending_clip_edit_->clip);
    if (target == nullptr || target->timing != pending_clip_origin_ ||
        pending_clip_frames_ == 0U) {
      pending_clip_edit_.reset();
    } else {
      --pending_clip_frames_;
    }
  }
  const gui::UiPoint clip_pointer =
      clip_drag_release_.value_or(gui::UiPoint{gesture_x, gesture_y});
  if (clip_drag_ != ClipDrag::None) {
    const float dx = clip_pointer.x - clip_drag_start_x_;
    const float dy = clip_pointer.y - clip_drag_start_y_;
    clip_drag_moved_ = clip_drag_moved_ ||
                       dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  const bool clip_drag_active =
      clip_drag_ != ClipDrag::None && clip_drag_moved_;
  if (clip_drag_ == ClipDrag::Move) {
    clip_drop_lane_ = clip_drag_active ? clip_lane_under_pointer(clip_pointer.y)
                                       : std::optional<doc::NodeId>{};
  }

  // Resolve a live clip proposal before anything is drawn, so the bar under
  // the pointer follows the gesture without authoring intermediate document
  // edits. Like Feed Mapping, the document changes once, on release.
  if (clip_drag_ != ClipDrag::None) {
    if (clip_drag_active) {
      clip_status_.clear();
      clip_status_error_ = false;
      clip_status_until_ = ctx.time_seconds() + 3.0;
      const core::i64 pointer_frame =
          static_cast<core::i64>(std::llround(x_to_frame(clip_pointer.x)));
      const core::i64 delta = pointer_frame - clip_drag_anchor_;
      const scene::ClipTiming origin = clip_drag_origin_;
      scene::ClipTiming proposed = origin;
      const auto targets = snap_targets(clip_drag_node_);
      const core::i64 tolerance = static_cast<core::i64>(std::max(
          1.0, std::ceil(8.0 / static_cast<double>(pixels_per_frame_))));
      clip_snap_guide_.reset();
      const auto snapped = [&](core::i64 edge) {
        // Alt suspends snapping for the duration of a gesture, the
        // same temporary override a key drag already honours.
        if (!snapping_ || ctx.alt_held()) {
          return edge;
        }
        const auto target = nearest_snap(edge, targets, tolerance);
        if (!target.has_value()) {
          return edge;
        }
        clip_snap_guide_ = *target;
        return *target;
      };

      core::i64 semantic_delta = delta;
      if (delta == 0) {
        semantic_delta = 0;
      } else if (clip_drag_ == ClipDrag::RippleTrimHead) {
        semantic_delta = snapped(origin.start + delta) - origin.start;
      } else if (clip_drag_ == ClipDrag::RippleTrimTail) {
        const core::i64 tail = origin.start + origin.duration;
        semantic_delta = snapped(tail + delta) - tail;
      } else if (clip_drag_ == ClipDrag::Roll) {
        const ElementInfo *const first_clip =
            find_element(elements, clip_drag_node_);
        const ElementInfo *const second_clip =
            find_element(elements, clip_drag_peer_);
        if (first_clip != nullptr && second_clip != nullptr) {
          const ElementInfo *const left =
              first_clip->timing.start <= second_clip->timing.start
                  ? first_clip
                  : second_clip;
          const core::i64 cut = left->timing.start + left->timing.duration;
          semantic_delta = snapped(cut + delta) - cut;
        }
      } else if (clip_drag_ == ClipDrag::Slide) {
        semantic_delta = snapped(origin.start + delta) - origin.start;
      }
      clip_drag_delta_ = semantic_delta;

      switch (clip_drag_) {
      case ClipDrag::Move: {
        core::i64 start = origin.start;
        if (delta != 0) {
          start = snapped(origin.start + delta);
          if (!clip_snap_guide_.has_value()) {
            // The tail is as good a thing to butt against as the head.
            const core::i64 tail = origin.start + delta + origin.duration;
            const core::i64 tail_snapped = snapped(tail);
            if (clip_snap_guide_.has_value()) {
              start = tail_snapped - origin.duration;
            }
          }
        }
        proposed.start = start;
        break;
      }
      case ClipDrag::TrimIn: {
        // The out point stays where it is and the source slides with
        // the in point, which is what makes this a trim rather than a
        // move. Convert at the source rate and retain the source frame at
        // the new cut; the authored source phase currently has integer precision.
        const core::i64 limit = origin.start + origin.duration - 1;
        const core::i64 start = delta == 0
                                    ? origin.start
                                    : std::min(snapped(origin.start + delta), limit);
        const core::i64 shift = start - origin.start;
        proposed.start = start;
        proposed.duration = origin.duration - shift;
        if (const ElementInfo *const clip = find_element(elements, clip_drag_node_)) {
          const auto source = scene::advance_source_phase(
              origin.source_in, shift,
              scene::MediaFrameRate{timeline.rate_numerator, timeline.rate_denominator},
              clip->source_frame_rate, clip->sequence_count,
              scene::SourcePhaseRounding::Floor);
          if (source) {
            proposed.source_in = source.value();
          } else {
            proposed = origin;
            clip_status_ = "TRIM UNAVAILABLE  /  Check source frames and frame rate";
            clip_status_error_ = true;
          }
        }
        break;
      }
      case ClipDrag::TrimOut: {
        const core::i64 end =
            delta == 0
                ? origin.start + origin.duration
                : std::max(snapped(origin.start + origin.duration + delta),
                           origin.start + 1);
        proposed.duration = end - origin.start;
        break;
      }
      case ClipDrag::FadeIn:
        proposed.fade_in =
            std::clamp(origin.fade_in + delta, core::i64{0}, origin.duration);
        break;
      case ClipDrag::FadeOut:
        proposed.fade_out =
            std::clamp(origin.fade_out - delta, core::i64{0}, origin.duration);
        break;
      case ClipDrag::RippleTrimHead:
        proposed.duration =
            std::max(origin.duration - semantic_delta, core::i64{1});
        proposed.source_in =
            std::max(origin.source_in + semantic_delta, core::i64{0});
        break;
      case ClipDrag::RippleTrimTail:
        proposed.duration =
            std::max(origin.duration + semantic_delta, core::i64{1});
        break;
      case ClipDrag::Roll: {
        const ElementInfo *const peer = find_element(elements, clip_drag_peer_);
        const bool dragged_left =
            peer != nullptr && origin.start <= peer->timing.start;
        if (dragged_left) {
          proposed.duration =
              std::max(origin.duration + semantic_delta, core::i64{1});
        } else {
          proposed.start = origin.start + semantic_delta;
          proposed.duration =
              std::max(origin.duration - semantic_delta, core::i64{1});
          proposed.source_in =
              std::max(origin.source_in + semantic_delta, core::i64{0});
        }
        break;
      }
      case ClipDrag::Slip: {
        const ElementInfo *const clip = find_element(elements, clip_drag_node_);
        if (clip != nullptr) {
          scene::ClipPlacement placement{};
          placement.node = clip->node;
          placement.sequence_count = clip->sequence_count;
          placement.timing = origin;
          placement.frame_rate = clip->source_frame_rate;
          const auto planned = scene::plan_slip(
              std::span{&placement, 1U}, clip->node, semantic_delta,
              scene::MediaFrameRate{timeline.rate_numerator,
                                    timeline.rate_denominator});
          if (planned) {
            clip_drag_delta_ = planned.value().applied_delta;
            if (!planned.value().updates.empty()) {
              proposed = planned.value().updates.front().timing;
            }
            clip_status_ = "SLIP  /  Source " +
                           std::to_string(proposed.source_in) + " of " +
                           std::to_string(clip->sequence_count) + " frames";
            const auto offset = scene::source_ordinal_delta(
                semantic_delta,
                scene::MediaFrameRate{timeline.rate_numerator,
                                      timeline.rate_denominator},
                clip->source_frame_rate);
            const core::i64 phase = origin.source_in % clip->sequence_count;
            if (!offset || offset.value() < -phase ||
                offset.value() > static_cast<core::i64>(clip->sequence_count) -
                                     1 - phase) {
              clip_status_ += "  /  wrapped";
            }
            if (clip->sequence_count == 1U) {
              clip_status_ += "  /  One source frame; image is unchanged";
            } else if (ctx.pointer_down()) {
              clip_status_ += "  /  release to apply";
            }
          } else {
            clip_drag_delta_ = 0;
            clip_status_error_ = true;
            clip_status_ = "SLIP UNAVAILABLE  /  ";
            switch (planned.error()) {
            case scene::TimelineEditError::InvalidRate:
              clip_status_ += "Invalid source or show frame rate";
              break;
            case scene::TimelineEditError::LockedClip:
              clip_status_ += "Clip is locked";
              break;
            case scene::TimelineEditError::ClipNotFound:
              clip_status_ += "Clip is no longer available";
              break;
            case scene::TimelineEditError::IntegerOverflow:
              clip_status_ += "Source offset exceeds the supported range";
              break;
            default:
              clip_status_ += "Check source frame count and clip timing";
              break;
            }
          }
        }
        break;
      }
      case ClipDrag::Slide:
        proposed.start = origin.start + semantic_delta;
        break;
      case ClipDrag::None:
        break;
      }
      proposed.fade_in =
          std::clamp(proposed.fade_in, core::i64{0}, proposed.duration);
      proposed.fade_out =
          std::clamp(proposed.fade_out, core::i64{0}, proposed.duration);
      clip_drag_proposed_ = proposed;
      if (clip_status_.empty() &&
          (clip_drag_ == ClipDrag::Move || clip_drag_ == ClipDrag::TrimIn ||
           clip_drag_ == ClipDrag::TrimOut)) {
        clip_status_ = clip_drag_ == ClipDrag::Move ? "MOVE" : "TRIM";
        clip_status_ += "  /  Start " + std::to_string(proposed.start) +
                        "  /  End " +
                        std::to_string(proposed.start + proposed.duration) +
                        "  /  Source " + std::to_string(proposed.source_in);
      }
    }
    if (!ctx.pointer_down()) {
      if (clip_drag_moved_) {
        switch (clip_drag_) {
        case ClipDrag::RippleTrimHead:
          if (clip_drag_delta_ != 0) {
            frame.timeline_edits.emplace_back(TimelineRippleTrimRequest{
                clip_drag_node_, scene::TimelineTrimEdge::Head,
                clip_drag_delta_});
          }
          break;
        case ClipDrag::RippleTrimTail:
          if (clip_drag_delta_ != 0) {
            frame.timeline_edits.emplace_back(TimelineRippleTrimRequest{
                clip_drag_node_, scene::TimelineTrimEdge::Tail,
                clip_drag_delta_});
          }
          break;
        case ClipDrag::Roll:
          if (clip_drag_delta_ != 0) {
            frame.timeline_edits.emplace_back(TimelineRollRequest{
                clip_drag_node_, clip_drag_peer_, clip_drag_delta_});
          }
          break;
        case ClipDrag::Slip:
          if (clip_drag_delta_ != 0) {
            frame.timeline_edits.emplace_back(
                TimelineSlipRequest{clip_drag_node_, clip_drag_delta_});
            pending_clip_edit_ = TimelineClipEdit{
                clip_drag_node_, clip_drag_proposed_, std::nullopt};
            pending_clip_origin_ = clip_drag_origin_;
            pending_clip_frames_ = 12U;
          }
          break;
        case ClipDrag::Slide:
          if (clip_drag_delta_ != 0) {
            frame.timeline_edits.emplace_back(
                TimelineSlideRequest{clip_drag_node_, clip_drag_delta_});
          }
          break;
        case ClipDrag::Move:
        case ClipDrag::TrimIn:
        case ClipDrag::TrimOut:
        case ClipDrag::FadeIn:
        case ClipDrag::FadeOut: {
          std::optional<doc::NodeId> retarget;
          const ElementInfo *const dragged =
              clip_drag_ == ClipDrag::Move
                  ? find_element(elements, clip_drag_node_)
                  : nullptr;
          if (dragged != nullptr && clip_drop_lane_.has_value() &&
              *clip_drop_lane_ != dragged->timeline_lane) {
            retarget = *clip_drop_lane_;
          }
          if (clip_drag_proposed_ != clip_drag_origin_ ||
              retarget.has_value()) {
            frame.clip_edits.push_back(TimelineClipEdit{
                clip_drag_node_, clip_drag_proposed_, retarget});
            pending_clip_edit_ = frame.clip_edits.back();
            pending_clip_origin_ = clip_drag_origin_;
            pending_clip_frames_ = 12U;
          }
          break;
        }
        case ClipDrag::None:
          break;
        }
      }
      clip_drag_ = ClipDrag::None;
      clip_drag_node_ = doc::NodeId{};
      clip_drag_peer_ = doc::NodeId{};
      clip_drag_delta_ = 0;
      clip_drag_start_x_ = 0.0F;
      clip_drag_start_y_ = 0.0F;
      clip_drag_release_.reset();
      clip_drag_moved_ = false;
      clip_drop_lane_.reset();
      clip_snap_guide_.reset();
    }
  }

  struct VisibleKeyPoint final {
    SelectedKey key;
    float x{};
    float y{};
  };
  std::vector<VisibleKeyPoint> visible_keys;

  // Track reordering deliberately mirrors the Feed Mapping surface drop:
  // movement lifts the source, every complete track band is a target, and the
  // document changes only once the pointer is released. A band includes its
  // clips and expanded property rows, so there are no narrow dead zones.
  struct LaneDropTarget final {
    doc::NodeId lane{};
    gui::Rect bounds{};
    std::string_view label;
    bool locked{};
  };
  std::vector<LaneDropTarget> lane_drop_targets;
  lane_drop_targets.reserve(lanes.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (rows[index].kind != Row::Kind::LaneHeader) {
      continue;
    }
    const doc::NodeId lane = rows[index].node;
    std::size_t end = index + 1U;
    while (end < rows.size() && rows[end].lane_member &&
           rows[end].lane_owner == lane) {
      ++end;
    }
    const float raw_top = rows_bounds.y + static_cast<float>(index) * row_height -
                          row_scroll_;
    const float raw_bottom = rows_bounds.y + static_cast<float>(end) * row_height -
                             row_scroll_;
    const float top = std::max(raw_top, rows_bounds.y);
    const float bottom = std::min(raw_bottom, rows_bounds.y + rows_bounds.height);
    if (bottom > top) {
      lane_drop_targets.push_back(LaneDropTarget{
          lane, gui::Rect{rows_bounds.x, top, rows_bounds.width, bottom - top},
          rows[index].label, rows[index].locked});
    }
  }

  // A layer target owns its complete expanded row band. Pointer placement in
  // the upper/lower half chooses a before/after insertion; peers are filtered
  // below so an ordering drag can never imply reparenting or track nesting.
  struct LayerDropTarget final {
    const ElementInfo *element{};
    gui::Rect bounds{};
    std::string_view label;
  };
  std::vector<LayerDropTarget> layer_drop_targets;
  layer_drop_targets.reserve(elements.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (rows[index].kind != Row::Kind::Layer || rows[index].surface) {
      continue;
    }
    const ElementInfo *const element = find_element(elements, rows[index].node);
    if (element == nullptr || !orderable_layer(*element)) {
      continue;
    }
    std::size_t end = index + 1U;
    while (end < rows.size() && rows[end].kind != Row::Kind::LaneHeader &&
           (rows[end].kind != Row::Kind::Layer ||
            rows[end].indent > rows[index].indent)) {
      ++end;
    }
    const float raw_top = rows_bounds.y + static_cast<float>(index) * row_height -
                          row_scroll_;
    const float raw_bottom = rows_bounds.y + static_cast<float>(end) * row_height -
                             row_scroll_;
    const float top = std::max(raw_top, rows_bounds.y);
    const float bottom = std::min(raw_bottom, rows_bounds.y + rows_bounds.height);
    if (bottom > top) {
      layer_drop_targets.push_back(LayerDropTarget{
          element,
          gui::Rect{rows_bounds.x, top, rows_bounds.width, bottom - top},
          rows[index].label});
    }
  }
  if (lane_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - lane_drag_start_x_;
    const float dy = ctx.pointer_y() - lane_drag_start_y_;
    lane_drag_moved_ = lane_drag_moved_ ||
                       dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  const bool lane_drag_active =
      lane_drag_node_ != doc::NodeId{} && lane_drag_moved_;

  if (layer_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - layer_drag_start_x_;
    const float dy = ctx.pointer_y() - layer_drag_start_y_;
    layer_drag_moved_ = layer_drag_moved_ ||
                        dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  const bool layer_drag_active =
      layer_drag_node_ != doc::NodeId{} && layer_drag_moved_;

  // A track drag is ordering, so it gets only an insertion cursor. A clip
  // drag is assignment, so outlining a track band is truthful there: the clip
  // will become a child of that track. Keeping these feedback vocabularies
  // separate prevents a reorder from looking like nesting.
  const bool clip_move_feedback =
      clip_drag_active && clip_drag_ == ClipDrag::Move;
  const LaneDropTarget *clip_drop_hovered = nullptr;
  if (clip_move_feedback && clip_drop_lane_.has_value() &&
      *clip_drop_lane_ != doc::NodeId{}) {
    const auto target = std::find_if(
        lane_drop_targets.begin(), lane_drop_targets.end(),
        [&](const LaneDropTarget &candidate) {
          return !candidate.locked && candidate.lane == *clip_drop_lane_;
        });
    if (target != lane_drop_targets.end()) {
      clip_drop_hovered = &*target;
    }
  }
  std::string clip_drag_label;
  if (clip_move_feedback) {
    const ElementInfo *const dragged = find_element(elements, clip_drag_node_);
    clip_drag_label = dragged != nullptr ? layer_name(*dragged) : "CLIP";
  }

  const auto layer_peers = [](const ElementInfo &source,
                              const ElementInfo &destination) noexcept {
    const bool source_tracked = source.kind == ElementKind::Media &&
                                source.timeline_lane != doc::NodeId{};
    const bool destination_tracked = destination.kind == ElementKind::Media &&
                                     destination.timeline_lane != doc::NodeId{};
    if (source_tracked || destination_tracked) {
      return source_tracked && destination_tracked &&
             source.timeline_lane == destination.timeline_lane;
    }
    return layer_parent(source) == layer_parent(destination);
  };

  doc::NodeId released_layer_drag_node{};
  bool released_layer_drag_moved{};
  const LayerDropTarget *layer_drop_hovered = nullptr;
  const LaneDropTarget *layer_lane_hovered = nullptr;
  std::optional<float> layer_drop_landing_y;
  std::string layer_drag_label{"LAYER"};
  bool layer_assignment_feedback{};
  bool layer_drop_committed{};
  if (layer_drag_node_ != doc::NodeId{}) {
    const ElementInfo *const source =
        find_element(elements, layer_drag_node_);
    if (source == nullptr || !orderable_layer(*source) || source->locked) {
      layer_drag_node_ = doc::NodeId{};
      layer_drag_nodes_.clear();
      layer_drag_moved_ = false;
    } else {
      layer_drag_label = layer_name(*source);
      if (layer_drag_nodes_.size() > 1U) {
        layer_drag_label = std::to_string(layer_drag_nodes_.size()) + " layers";
      }
      if (layer_drag_active) {
        frame.lane_editing = ctx.pointer_down();
        std::optional<doc::NodeId> assignment;
        if (source->kind == ElementKind::Media &&
            rows_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          assignment = clip_lane_under_pointer(gesture_y);
          if (assignment.has_value() && *assignment != source->timeline_lane) {
            layer_assignment_feedback = true;
            if (*assignment != doc::NodeId{}) {
              const auto target = std::find_if(
                  lane_drop_targets.begin(), lane_drop_targets.end(),
                  [&](const LaneDropTarget &candidate) {
                    return !candidate.locked &&
                           candidate.lane == *assignment;
                  });
              if (target != lane_drop_targets.end()) {
                layer_lane_hovered = &*target;
              }
            }
            if (!ctx.pointer_down()) {
              frame.clip_edits.push_back(
                  TimelineClipEdit{source->node, source->timing, assignment});
              layer_drop_committed = true;
            }
          }
        }
        if (!layer_assignment_feedback) {
          for (const LayerDropTarget &target : layer_drop_targets) {
            if (target.element->node != source->node &&
                !target.element->locked && layer_peers(*source, *target.element) &&
                target.bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
              layer_drop_hovered = &target;
            }
          }
          if (layer_drop_hovered != nullptr) {
            const bool before =
                ctx.pointer_y() < layer_drop_hovered->bounds.y +
                                      layer_drop_hovered->bounds.height * 0.5F;
            layer_drop_landing_y =
                before ? layer_drop_hovered->bounds.y
                       : layer_drop_hovered->bounds.y +
                             layer_drop_hovered->bounds.height;
            if (!ctx.pointer_down()) {
              frame.layer_reorders.push_back(TimelineLayerReorder{
                  source->node, layer_drop_hovered->element->node,
                  before ? TimelineLayerPlacement::Before
                         : TimelineLayerPlacement::After});
              layer_drop_committed = true;
            }
          }
        }
      }
      if (!ctx.pointer_down()) {
        if (layer_drag_moved_ && !layer_drop_committed) {
          frame.layer_drop = TimelineLayerDrop{
              layer_drag_nodes_, ctx.pointer_x(), ctx.pointer_y()};
        }
        released_layer_drag_node = layer_drag_node_;
        released_layer_drag_moved = layer_drag_moved_;
        layer_drag_node_ = doc::NodeId{};
        layer_drag_nodes_.clear();
        layer_drag_start_x_ = 0.0F;
        layer_drag_start_y_ = 0.0F;
        layer_drag_moved_ = false;
      }
    }
  }

  std::vector<std::size_t> track_numbers(rows.size(), 0U);
  {
    std::size_t number = 0U;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      if (rows[index].kind == Row::Kind::LaneHeader) {
        track_numbers[index] = ++number;
      }
    }
  }
  // A right press on a row opens that row's menu once the rows are drawn.
  std::optional<std::size_t> right_row;

  ctx.push_clip(rows_bounds);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    const float y =
        rows_bounds.y + static_cast<float>(index) * row_height - row_scroll_;
    if (y + row_height <= rows_bounds.y ||
        y >= rows_bounds.y + rows_bounds.height) {
      continue;
    }
    if (row.kind == Row::Kind::Layer && !row.surface) {
      const float top = std::max(y, rows_bounds.y);
      const float bottom = std::min(y + row_height, rows_bounds.y + rows_bounds.height);
      replacement_targets_.push_back({row.node,
          {rows_bounds.x, top, rows_bounds.width, bottom - top}});
    }
    const float centre_y = y + row_height * 0.5F;
    std::optional<ChannelIdentity> row_channel_identity;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      row_channel_identity = ChannelIdentity{
          row.kind == Row::Kind::Property ? row.property_target : row.node,
          row.kind == Row::Kind::Property
              ? row.property
              : group_channel(row.group, row.component)};
    }
    const bool is_selected =
        row.kind == Row::Kind::LaneHeader
            ? row.node == selected_lane_
            : (row.surface ? row.node == selected_surface
                           : selected(row.node, selection));
    const bool layer_like =
        row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader;
    const gui::Theme &row_theme = ctx.theme();
    const bool row_hovered =
        !ctx.pointer_blocked() &&
        rows_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_y() >= y && ctx.pointer_y() < y + row_height;
    gui::UiColor background{};
    if (is_selected && layer_like) {
      background = row_theme.selection;
    } else if (is_selected) {
      // The selected layer's own properties: tinted, not flooded, so the
      // selection still reads as one row.
      background = mix_colors(row_theme.track, row_theme.selection, 0.35F);
    } else if (row.kind == Row::Kind::LaneHeader) {
      background = row_theme.header;
    } else if (layer_like) {
      background = row_theme.row;
    } else {
      // Property rows sit a shade below the layer they open from, so the eye
      // reads them as that layer's contents rather than as more layers.
      background = row_theme.track;
    }
    ctx.panel(gui::Rect{rows_bounds.x, y, rows_bounds.width, row_height},
              background);
    if (!is_selected && row_hovered && layer_like) {
      ctx.panel(gui::Rect{rows_bounds.x, y, list_width, row_height},
                alpha(row_theme.widget_hover, 0.6F));
    }
    // Every layer is ruled off across the whole panel; property rows only
    // across the lanes, so a twirled-open layer still reads as one block.
    ctx.panel(gui::Rect{layer_like ? rows_bounds.x : graph.x,
                        y + row_height - 1.0F,
                        layer_like ? rows_bounds.width : graph.width, 1.0F},
              row_theme.divider);
    if (layer_like && !row.surface) {
      // The row's own colour down its left edge: the kind for a layer, the
      // track's for a track. Selection takes the accent.
      ctx.panel(gui::Rect{rows_bounds.x, y, 3.0F, row_height - 1.0F},
                is_selected ? row_theme.accent
                            : alpha(row.color, row.locked ? 0.35F : 0.85F));
    }

    const float tree_x = rows_bounds.x + list_metrics::toggle + 6.0F;
    const float twirl_x =
        tree_x + static_cast<float>(row.indent) * list_metrics::indent;
    draw_indent_guides(ctx, tree_x, y, row_height, row.indent,
                       alpha(row_theme.border, 0.9F));
    const core::u64 row_group = group_id(row.node, row.group);
    const bool open = row.hierarchy
        ? (!search_.empty() || expanded_hierarchies_.contains(row.node))
        : ((row.kind == Row::Kind::Layer ||
            row.kind == Row::Kind::PropertiesHeader)
               ? expanded_layers_.contains(row.node.value())
               : (row.kind == Row::Kind::Group &&
                  expanded_groups_.contains(row_group)));
    if (row.expandable) {
      const bool twirl_hovered =
          row_hovered && ctx.pointer_x() >= twirl_x &&
          ctx.pointer_x() < twirl_x + list_metrics::twirl_width;
      draw_twirl(ctx, twirl_x + list_metrics::twirl_width * 0.5F, centre_y,
                 open, twirl_hovered ? row_theme.text : row_theme.text_muted);
    }
    // Left of this a press opens or closes the row; right of it, it means
    // the row itself.
    const float content_x = twirl_x + list_metrics::twirl_width + 2.0F;
    float name_x = content_x;
    gui::Rect name_bounds{content_x, y,
                          std::max(graph.x - content_x - 48.0F, 0.0F),
                          row_height};
    const float lock_x = graph.x - list_metrics::toggle - 4.0F;
    const gui::Rect eye_bounds{rows_bounds.x + 4.0F, y, list_metrics::toggle,
                               row_height};
    const gui::Rect lock_bounds{lock_x, y, list_metrics::toggle, row_height};
    if (row.kind == Row::Kind::LaneHeader) {
      // Tracks are numbered top-down, the way they are called across a
      // stage: "kill track two".
      const std::string number = std::to_string(track_numbers[index]);
      const float badge_width = std::max(18.0F, ctx.font_width(number) + 8.0F);
      const gui::Rect badge{content_x + 2.0F, centre_y - 8.0F, badge_width,
                            16.0F};
      ctx.panel(badge, row.muted ? alpha(row_theme.text_muted, 0.6F)
                                 : row.color);
      ctx.label(gui::Rect{badge.x + std::max((badge.width -
                                              ctx.font_width(number)) *
                                                 0.5F,
                                             0.0F),
                          badge.y, badge.width, badge.height},
                number, row_theme.background);
      name_x = badge.x + badge.width + 8.0F;
      name_bounds = gui::Rect{name_x, y, std::max(lock_x - name_x - 4.0F, 0.0F),
                              row_height};
      if (rename_.editing(row.node.value())) {
        // While a track's name is open the shell holds one lane gesture and
        // keeps the keyboard's shortcuts away from the field.
        frame.lane_editing = true;
        if (auto renamed = rename_.draw(
                ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                               name_bounds.width + 4.0F, row_height - 4.0F})) {
          frame.lane_edits.push_back(TimelineLaneEdit{
              TimelineLaneAction::Rename, row.node, std::move(*renamed)});
        }
      } else {
        ctx.label(name_bounds, row.label,
                  row.muted ? row_theme.text_muted : row_theme.text);
      }
      // Muting a track hides it, so its switch is the eye every layer has;
      // the toggles double as the state readout, so no badge repeats them.
      if (row_flag_toggle(ctx, mix(id_, "track mute", row.node.value()),
                          eye_bounds, RowFlag::Visible, !row.muted,
                          row_hovered)) {
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleMute, row.node, {}});
      }
      if (row_flag_toggle(ctx, mix(id_, "track lock", row.node.value()),
                          lock_bounds, RowFlag::Locked, row.locked,
                          row_hovered)) {
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleLock, row.node, {}});
      }
    } else if (row.kind == Row::Kind::Layer) {
      const ElementInfo *const row_element =
          row.surface ? nullptr : find_element(elements, row.node);
      const float icon_cx = content_x + 8.0F;
      if (row_element != nullptr) {
        draw_element_icon(ctx, *row_element, icon_cx, centre_y,
                          row_element->effective_visible
                              ? row.color
                              : alpha(row.color, 0.4F));
      } else {
        draw_kind_icon(ctx, ElementKind::Track, scene::LocalLightKind::Point,
                       icon_cx, centre_y, row.color);
      }
      name_x = icon_cx + 13.0F;
      // Everything the timeline lists can be switched off and protected, not
      // only the clips: a light or an LED surface is as worth muting while
      // working as a plate is. A surface is physical stage geometry rather
      // than a scene node, so it has no eye - but it takes the same padlock,
      // the one its Feed Mapping lock and Stage picking already honour. The
      // environment is always there.
      const bool has_toggles =
          row.surface || row_element == nullptr ||
          row_element->kind != ElementKind::Environment;
      // A surface keeps its badge, just inside the padlock.
      const float name_right =
          (has_toggles ? lock_x : graph.x) - (row.surface ? 60.0F : 0.0F) -
          (has_toggles ? 0.0F : 64.0F);
      name_bounds = gui::Rect{name_x, y,
                              std::max(name_right - name_x - 6.0F, 0.0F),
                              row_height};
      if (rename_.editing(row.node.value())) {
        if (auto renamed = rename_.draw(
                ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                               name_bounds.width + 4.0F, row_height - 4.0F})) {
          frame.renames.push_back(TimelineRename{row.node, std::move(*renamed)});
        }
      } else {
        const bool shown =
            row_element == nullptr || row_element->effective_visible;
        ctx.label(name_bounds, row.label,
                  row.locked ? row_theme.text_muted
                             : (shown ? row_theme.text
                                      : alpha(row_theme.text, 0.45F)));
      }
      if (row.surface) {
        ctx.label(gui::Rect{lock_x - 60.0F, y, 56.0F, row_height}, "SURFACE",
                  row_theme.text_muted);
        if (row_flag_toggle(ctx, mix(id_, "surface lock", row.node.value()),
                            lock_bounds, RowFlag::Locked, row.locked,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Locked, !row.locked});
        }
      } else if (has_toggles) {
        const bool hidden = !row.element_visible;
        if (row_flag_toggle(ctx, mix(id_, "element mute", row.node.value()),
                            eye_bounds, RowFlag::Visible, row.element_visible,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Visible, hidden});
        }
        if (row_flag_toggle(ctx, mix(id_, "element lock", row.node.value()),
                            lock_bounds, RowFlag::Locked, row.locked,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Locked, !row.locked});
        }
      }
    } else {
      // A property: named in the colour its keys and curve are drawn in, so
      // the list and the lanes read as one table.
      name_bounds = gui::Rect{content_x, y,
                              std::max(graph.x - content_x - 48.0F, 0.0F),
                              row_height};
      ctx.label(name_bounds, row.label,
                row.kind == Row::Kind::Component ||
                        row.kind == Row::Kind::Property
                    ? channel_color(row_channel_identity->property)
                    : row.color);
    }

    const float interaction_y = std::max(y, rows_bounds.y);
    const float interaction_bottom =
        std::min(y + row_height, rows_bounds.y + rows_bounds.height);
    const float interaction_height =
        std::max(interaction_bottom - interaction_y, 0.0F);

    if (row_channel_identity.has_value()) {
      const bool channel_hidden =
          hidden_channels_.contains(*row_channel_identity);
      const gui::Rect visibility_bounds{
          graph.x - 41.0F,
          interaction_y + std::max((interaction_height - 14.0F) * 0.5F, 0.0F),
          14.0F, std::min(interaction_height, 14.0F)};
      const gui::Interaction visibility = ctx.grab(
          mix(id_, "channel visibility", static_cast<core::u64>(index)),
          visibility_bounds);
      ctx.panel(visibility_bounds,
                channel_hidden ? alpha(ctx.theme().track, 0.82F)
                               : channel_color(row_channel_identity->property));
      if (channel_hidden) {
        ctx.draw_list().add_line(visibility_bounds.x, visibility_bounds.y,
                                 visibility_bounds.x + visibility_bounds.width,
                                 visibility_bounds.y + visibility_bounds.height,
                                 1.0F, ctx.theme().warning);
      }
      if (visibility.clicked) {
        active_channel_ = *row_channel_identity;
        if (channel_hidden) {
          hidden_channels_.erase(*row_channel_identity);
        } else {
          hidden_channels_.insert(*row_channel_identity);
          selected_keys_.erase(
              std::remove_if(selected_keys_.begin(), selected_keys_.end(),
                             [&](const SelectedKey &key) {
                               return key.target ==
                                          row_channel_identity->target &&
                                      key.component ==
                                          row_channel_identity->property;
                             }),
              selected_keys_.end());
          drag_keys_.erase(
              std::remove_if(drag_keys_.begin(), drag_keys_.end(),
                             [&](const SelectedKey &key) {
                               return key.target ==
                                          row_channel_identity->target &&
                                      key.component ==
                                          row_channel_identity->property;
                             }),
              drag_keys_.end());
          if (drag_keys_.empty()) {
            dragging_key_ = false;
            duplicate_drag_ = false;
          }
        }
      }
    }

    // Property-local key controls: the group diamond keys all three axes;
    // a component diamond keys only that channel. A filled diamond means
    // there is already a key at the playhead, while a hollow bright one
    // means the channel is animated elsewhere.
    if (!row.locked && (row.kind == Row::Kind::Group ||
                        row.kind == Row::Kind::Component ||
                        row.kind == Row::Kind::Property)) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      const output::ProjectionSurface *const row_surface =
          row.surface ? find_surface(surfaces, row.node) : nullptr;
      if ((row_surface != nullptr) || row_element != nullptr) {
        bool animated = false;
        bool keyed_here = true;
        const core::i64 show_frame = signed_frame(timeline.show_frame);
        const core::u8 first_component =
            row.kind == Row::Kind::Property
                ? static_cast<core::u8>(row.property)
                : static_cast<core::u8>(group_channel(
                      row.group,
                      row.kind == Row::Kind::Component ? row.component : 0U));
        const core::u8 component_count =
            row.kind == Row::Kind::Group ? row.component_count : 1U;
        const doc::NodeId key_target =
            row.kind == Row::Kind::Property ? row.property_target : row.node;
        for (core::u8 offset = 0U; offset < component_count; ++offset) {
          const auto component =
              static_cast<scene::TransformChannel>(first_component + offset);
          const scene::ChannelKeyframes *const authored =
              channel_for(keyframes, key_target, component);
          animated =
              animated || (authored != nullptr && !authored->frames.empty());
          keyed_here =
              keyed_here && authored != nullptr &&
              std::find(authored->frames.begin(), authored->frames.end(),
                        show_frame) != authored->frames.end();
        }

        const gui::Rect key_bounds{
            graph.x - 22.0F,
            interaction_y + std::max((interaction_height - 18.0F) * 0.5F, 0.0F),
            18.0F, std::min(interaction_height, 18.0F)};
        if (key_button(ctx, mix(id_, "row key", static_cast<core::u64>(index)),
                       key_bounds, animated, keyed_here, background)) {
          for (core::u8 offset = 0U; offset < component_count; ++offset) {
            const auto component =
                static_cast<scene::TransformChannel>(first_component + offset);
            frame.keys.push_back(TimelineKeyEdit{
                key_target, component, timeline.show_frame,
                row.kind == Row::Kind::Property
                    ? row.property_value
                    : (row_surface != nullptr
                           ? surface_component_value(*row_surface, component)
                           : component_value(*row_element, component)),
                false, keyed_here});
          }
        }
      }
    }

    // A header being renamed gives its width to the text field; a header that
    // is merely selected stays fully grabbable, because selecting a track is
    // now the ordinary thing a click does to it.
    const float label_width =
        rename_.editing(row.node.value())
            ? std::max(name_x - rows_bounds.x - 4.0F, 0.0F)
            : list_width;
    const gui::Interaction label_it =
        ctx.grab(mix(id_, "row", static_cast<core::u64>(index)),
                 gui::Rect{rows_bounds.x, interaction_y, label_width,
                           interaction_height});
    if (label_it.pressed && row.kind == Row::Kind::LaneHeader && !row.locked) {
      lane_drag_node_ = row.node;
      capture_layer_drag(row.node);
      lane_drag_start_x_ = ctx.pointer_press_x();
      lane_drag_start_y_ = ctx.pointer_press_y();
      lane_drag_moved_ = false;
    } else if (label_it.pressed && row.kind == Row::Kind::Layer &&
               !row.surface &&
               !(row.expandable && ctx.pointer_press_x() < content_x)) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      if (row_element != nullptr && orderable_layer(*row_element) &&
          !row.locked) {
        selected_keys_.clear();
        drag_keys_.clear();
        dragging_key_ = false;
        duplicate_drag_ = false;
        layer_drag_node_ = row.node;
        const bool preserve_selection = selected(row.node, selection);
        capture_layer_drag(row.node);
        layer_drag_start_x_ = ctx.pointer_press_x();
        layer_drag_start_y_ = ctx.pointer_press_y();
        layer_drag_moved_ = false;
        marquee_tracking_ = false;
        marquee_active_ = false;
        lasso_points_.clear();
        // Keep a selected group intact until a click is released. Modifier
        // clicks also select on release, so Ctrl toggles exactly once.
        if (!preserve_selection && !ctx.ctrl_held() && !ctx.shift_held()) {
          TimelineSelectionRequest next{};
          next.node = row.node;
          next.action = SelectionAction::Replace;
          frame.selection = std::move(next);
        }
      }
    }
    const bool dragged_header_click =
        row.kind == Row::Kind::LaneHeader && row.node == lane_drag_node_ &&
        lane_drag_moved_;
    const bool dragged_layer_click =
        row.kind == Row::Kind::Layer && row.node == released_layer_drag_node &&
        released_layer_drag_moved;
    if (label_it.clicked && !dragged_header_click && !dragged_layer_click) {
      if (row.kind == Row::Kind::LaneHeader &&
          !(row.expandable && ctx.pointer_x() < content_x)) {
        selected_lane_ = row.node;
        lane_destination_ = doc::NodeId{};
        selected_keys_.clear();
        drag_keys_.clear();
        dragging_key_ = false;
        duplicate_drag_ = false;
        // A track is a screen, so selecting one has to reach the Inspector
        // like selecting anything else does. Without this the header would
        // highlight and the Inspector would keep showing whatever was
        // selected before, which is the shape of a control that does nothing.
        if (!row.locked) {
          TimelineSelectionRequest next{};
          next.node = row.node;
          next.action =
              selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
          if (next.action == SelectionAction::Range) {
            next.visible_order = visible_order;
          }
          frame.selection = std::move(next);
        }
        // A second click on the same name opens it for editing, focused
        // with the whole name selected so typing replaces it.
        if (!row.locked && !ctx.ctrl_held() && !ctx.shift_held() &&
            name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            clicks_.double_click(row.node.value(), ctx.time_seconds())) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
        continue;
      }
      const bool on_twirl = row.expandable && ctx.pointer_x() < content_x;
      if (on_twirl) {
        if (row.hierarchy) {
          if (open) {
            expanded_hierarchies_.erase(row.node);
          } else {
            expanded_hierarchies_.insert(row.node);
          }
        } else if (row.kind == Row::Kind::Layer ||
                   row.kind == Row::Kind::PropertiesHeader) {
          if (open) {
            expanded_layers_.erase(row.node.value());
          } else {
            expanded_layers_.insert(row.node.value());
          }
        } else if (open) {
          expanded_groups_.erase(row_group);
        } else {
          expanded_groups_.insert(row_group);
        }
      } else if (!row.locked) {
        // A layer-row click leaves the key-selection domain. This
        // keeps a later Ctrl+C/Ctrl+V aimed at the newly selected
        // clip/layer rather than at stale Timeline diamonds.
        selected_keys_.clear();
        drag_keys_.clear();
        dragging_key_ = false;
        duplicate_drag_ = false;
        TimelineSelectionRequest next{};
        next.node = row.node;
        next.action =
            selection_action(ctx.ctrl_held(), ctx.shift_held(), !row.surface);
        if (next.action == SelectionAction::Range) {
          next.visible_order = visible_order;
        }
        next.surface = row.surface;
        const bool plain_click = next.action == SelectionAction::Replace;
        frame.selection = std::move(next);
        // A second click on a layer's name renames it, as in the outliner.
        if (row.kind == Row::Kind::Layer && !row.surface && plain_click &&
            name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            clicks_.double_click(row.node.value(), ctx.time_seconds())) {
          const ElementInfo *const element = find_element(elements, row.node);
          if (element != nullptr && !element->linked &&
              element->kind != ElementKind::Environment) {
            rename_.begin(ctx, row.node.value(), row.label);
          }
        }
      }
    }

    // The time lane: a clip bar for a placed media layer, a translucent
    // authored-span bar for every other layer, aggregate diamonds for
    // groups, and draggable diamonds for component channels.
    ctx.push_clip(gui::Rect{graph.x, y, graph.width, row_height});
    // Filled in for a placed clip row, and consulted by the lane
    // interaction below so that a press on the bar becomes a clip gesture
    // rather than a scrub.
    const ElementInfo *clip_element = nullptr;
    scene::ClipTiming clip_timing{};
    gui::Rect clip_bar{};
    float clip_fade_in_x{};
    float clip_fade_out_x{};
    if (row.kind == Row::Kind::Layer && !row.surface) {
      const ElementInfo *const layer_element = find_element(elements, row.node);
      if (layer_element != nullptr &&
          layer_element->kind == ElementKind::Media &&
          layer_element->timing.placed()) {
        clip_element = layer_element;
        clip_timing =
            clip_drag_ != ClipDrag::None && clip_drag_node_ == row.node
                ? clip_drag_proposed_
                : (pending_clip_edit_.has_value() &&
                           pending_clip_edit_->clip == row.node
                       ? pending_clip_edit_->timing
                       : layer_element->timing);
      }
    }
    if (clip_element != nullptr) {
      const float x0 = frame_to_x(static_cast<double>(clip_timing.start));
      const float x1 = frame_to_x(
          static_cast<double>(clip_timing.start + clip_timing.duration));
      clip_bar =
          gui::Rect{x0, y + 3.0F, std::max(x1 - x0, 2.0F), row_height - 6.0F};
      const bool live =
          clip_drag_ != ClipDrag::None && clip_drag_node_ == row.node;
      ctx.panel(clip_bar, alpha(row.color, live ? 0.42F : 0.30F));
      bool drew_filmstrip = false;
      if (const TimelineFilmstripObservation *const observation =
              filmstrip_for_clip(row.node);
          observation != nullptr &&
          observation->state == assets::DerivativeState::Ready &&
          observation->texture != nullptr) {
        float u0 = 0.0F;
        float u1 = 1.0F;
        bool looped = false;
        const scene::MediaFrameRate source_mapping_show_rate{
            timeline.rate_numerator, timeline.rate_denominator};
        if (clip_element->sequence_count > 1U &&
            clip_timing.source_in >= 0 && clip_timing.duration > 0) {
          const auto mapped = scene::source_ordinal_delta(
              clip_timing.duration - 1, source_mapping_show_rate,
              clip_element->source_frame_rate);
          const core::i64 maximum_source =
              static_cast<core::i64>(clip_element->sequence_count - 1U);
          if (mapped && mapped.value() >= 0 &&
              clip_timing.source_in <= maximum_source - mapped.value()) {
            const auto atlas_center = [maximum_source](core::i64 ordinal) {
              const double normalized = static_cast<double>(ordinal) /
                                        static_cast<double>(maximum_source);
              return static_cast<float>(
                  (normalized * 7.0 + 0.5) / 8.0);
            };
            u0 = atlas_center(clip_timing.source_in);
            u1 = atlas_center(clip_timing.source_in + mapped.value());
            constexpr float one_tile = 1.0F / 8.0F;
            if (u1 - u0 < one_tile) {
              const float midpoint = (u0 + u1) * 0.5F;
              u0 = std::clamp(midpoint - one_tile * 0.5F, 0.0F,
                              1.0F - one_tile);
              u1 = u0 + one_tile;
            }
          } else {
            looped = true;
          }
        }
        const gui::UiColor tint{1.0F, 1.0F, 1.0F, live ? 0.62F : 0.78F};
        if (looped) {
          // Long placements repeat the source. Sample the existing eight-thumb
          // atlas at visible timeline positions, with a fixed per-row budget.
          // Neither a very long placement nor many loops increases this work.
          const float begin = std::max(clip_bar.x, graph.x);
          const float end = std::min(clip_bar.x + clip_bar.width,
                                     graph.x + graph.width);
          if (end > begin) {
            const unsigned tiles = static_cast<unsigned>(std::clamp(
                std::ceil((end - begin) / 72.0F), 1.0F, 32.0F));
            const float width = (end - begin) / static_cast<float>(tiles);
            for (unsigned tile = 0U; tile < tiles; ++tile) {
              const float x = begin + static_cast<float>(tile) * width;
              const double offset = std::clamp(
                  x_to_frame(x + width * 0.5F) -
                      static_cast<double>(clip_timing.start),
                  0.0, static_cast<double>(clip_timing.duration - 1));
              const core::i64 show_offset =
                  offset >= static_cast<double>(clip_timing.duration - 1)
                      ? clip_timing.duration - 1
                      : static_cast<core::i64>(offset);
              const auto phase = scene::advance_source_phase(
                  clip_timing.source_in, show_offset,
                  source_mapping_show_rate, clip_element->source_frame_rate,
                  clip_element->sequence_count, scene::SourcePhaseRounding::Floor);
              if (!phase) {
                continue;
              }
              const auto thumbnail = static_cast<unsigned>(std::llround(
                  static_cast<double>(phase.value()) * 7.0 /
                  static_cast<double>(clip_element->sequence_count - 1U)));
              const float left_uv = static_cast<float>(thumbnail) / 8.0F;
              ctx.draw_list().add_image_region(
                  x, clip_bar.y, width, clip_bar.height, *observation->texture,
                  left_uv, 0.0F, left_uv + 0.125F, 1.0F, tint);
            }
          }
        } else {
          ctx.draw_list().add_image_region(
              clip_bar.x, clip_bar.y, clip_bar.width, clip_bar.height,
              *observation->texture, u0, 0.0F, u1, 1.0F, tint);
        }
        drew_filmstrip = true;
      }
      if (const TimelineWaveformObservation *const waveform =
              waveform_for_clip(row.node);
          waveform != nullptr &&
          waveform->state == assets::DerivativeState::Ready &&
          waveform->bins.size() == assets::waveform_peak_rms_bins &&
          clip_element->sequence_count > 0U && clip_timing.duration > 0) {
        const float begin = std::max(clip_bar.x, graph.x) + 1.0F;
        const float end =
            std::min(clip_bar.x + clip_bar.width, graph.x + graph.width) - 1.0F;
        const float half_height = std::max(clip_bar.height * 0.42F, 1.0F);
        const scene::MediaFrameRate show_frame_rate{timeline.rate_numerator,
                                                     timeline.rate_denominator};
        const core::i64 maximum_source = static_cast<core::i64>(
            std::min<core::u64>(
                clip_element->sequence_count - 1U,
                static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
        for (float x = begin; x <= end; x += 2.0F) {
          const double offset = std::clamp(
              std::floor(x_to_frame(x)) - static_cast<double>(clip_timing.start),
              0.0, static_cast<double>(clip_timing.duration - 1));
          const core::i64 show_offset =
              offset >= static_cast<double>(clip_timing.duration - 1)
                  ? clip_timing.duration - 1
                  : static_cast<core::i64>(offset);
          const auto source_phase = scene::advance_source_phase(
              clip_timing.source_in, show_offset, show_frame_rate,
              clip_element->source_frame_rate, clip_element->sequence_count,
              scene::SourcePhaseRounding::Floor);
          if (!source_phase) {
            continue;
          }
          const core::i64 source_ordinal = source_phase.value();
          const double normalized =
              maximum_source > 0
                  ? static_cast<double>(source_ordinal) /
                        static_cast<double>(maximum_source)
                  : 0.0;
          const std::size_t bin = std::min<std::size_t>(
              static_cast<std::size_t>(std::floor(
                  normalized * static_cast<double>(waveform->bins.size()))),
              waveform->bins.size() - 1U);
          const assets::WaveformPeakRms sample = waveform->bins[bin];
          const float peak_height = sample.peak * half_height;
          const float rms_height = sample.rms * half_height;
          ctx.panel(gui::Rect{x, centre_y - rms_height, 1.0F,
                              std::max(rms_height * 2.0F, 1.0F)},
                    alpha(ctx.theme().keyframe,
                          drew_filmstrip ? 0.68F : 0.76F));
          ctx.panel(gui::Rect{x, centre_y - peak_height, 1.0F, 1.0F},
                    alpha(ctx.theme().text, 0.82F));
          ctx.panel(gui::Rect{x, centre_y + peak_height, 1.0F, 1.0F},
                    alpha(ctx.theme().text, 0.82F));
        }
      }
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, clip_bar.width, 1.0F},
                alpha(row.color, 0.85F));
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y + clip_bar.height - 1.0F,
                          clip_bar.width, 1.0F},
                alpha(row.color, 0.85F));
      const float grab = edge_grab_width(clip_bar.width);
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                alpha(row.color, 0.68F));
      ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab, clip_bar.y, grab,
                          clip_bar.height},
                alpha(row.color, 0.68F));

      // The ramps, drawn the way an editor draws them: a diagonal from
      // the bar's foot at the cut to its head where the clip is fully in.
      clip_fade_in_x = frame_to_x(
          static_cast<double>(clip_timing.start + clip_timing.fade_in));
      clip_fade_out_x = frame_to_x(static_cast<double>(
          clip_timing.start + clip_timing.duration - clip_timing.fade_out));
      const gui::UiColor ramp = alpha(ctx.theme().keyframe, 0.9F);
      if (clip_timing.fade_in > 0) {
        ctx.draw_list().add_line(clip_bar.x, clip_bar.y + clip_bar.height,
                                 clip_fade_in_x, clip_bar.y, 1.0F, ramp);
      }
      if (clip_timing.fade_out > 0) {
        ctx.draw_list().add_line(clip_fade_out_x, clip_bar.y,
                                 clip_bar.x + clip_bar.width,
                                 clip_bar.y + clip_bar.height, 1.0F, ramp);
      }
      // The fade handles ride the bar's top edge as round knobs. They grow
      // and light under the pointer, and answer a press several units away,
      // so a crossfade can be pulled without hunting for a pixel.
      const auto fade_handle = [&](float handle_x, bool engaged) {
        const bool near =
            edit_tool_ == TimelineEditTool::Standard &&
            !ctx.pointer_blocked() && clip_drag_ == ClipDrag::None &&
            std::fabs(ctx.pointer_x() - handle_x) <= fade_handle_reach &&
            ctx.pointer_y() >= clip_bar.y - 4.0F &&
            ctx.pointer_y() <= clip_bar.y + fade_handle_reach;
        const bool lit = engaged || near;
        draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                           lit ? 6.0F : 4.5F,
                           alpha(ctx.theme().background, 0.85F));
        draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                           lit ? 4.5F : 3.0F,
                           lit ? ctx.theme().accent : ramp);
        if (near) {
          ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          ctx.tooltip(gui::Rect{handle_x - fade_handle_reach, clip_bar.y - 4.0F,
                                fade_handle_reach * 2.0F,
                                fade_handle_reach + 4.0F},
                      "Drag to set the fade");
        }
      };
      fade_handle(clip_fade_in_x, clip_drag_ == ClipDrag::FadeIn &&
                                      clip_drag_node_ == row.node);
      fade_handle(clip_fade_out_x, clip_drag_ == ClipDrag::FadeOut &&
                                       clip_drag_node_ == row.node);

      // Every editor names its clips on the bar. The name rides on a dark
      // backing so it reads over any filmstrip, and stays pinned to the
      // visible start of a bar scrolled half off the left edge.
      const float name_left =
          std::max(clip_bar.x, graph.x) + 6.0F + edge_grab_width(clip_bar.width);
      const float name_right = clip_bar.x + clip_bar.width - 6.0F;
      if (name_right - name_left >= 24.0F) {
        std::string name = row.label;
        if (clip_drag_ == ClipDrag::Slip && clip_drag_node_ == row.node) {
          name += "  Source " + std::to_string(clip_timing.source_in);
        }
        if (!clip_element->on_air) {
          name += "  off air";
        }
        const float text_width =
            std::min(ctx.font_width(name) + 8.0F, name_right - name_left);
        ctx.panel(gui::Rect{name_left - 4.0F, clip_bar.y + 2.0F, text_width,
                            clip_bar.height - 4.0F},
                  alpha(ctx.theme().background, 0.55F));
        ctx.label(gui::Rect{name_left, clip_bar.y, name_right - name_left,
                            clip_bar.height},
                  name,
                  clip_element->on_air ? ctx.theme().text
                                       : ctx.theme().text_muted);
      }
      if (selected(row.node, selection)) {
        draw_border(ctx, clip_bar, ctx.theme().text, 1.5F);
      }
      // What a press here would do, shown before it is done: the trim edge
      // under a Selection pointer lights, and the razor draws its cut.
      if (!row.locked &&
          clip_bar.contains(ctx.pointer_x(), ctx.pointer_y()) &&
          clip_drag_ == ClipDrag::None && !ctx.pointer_blocked()) {
        const float px = ctx.pointer_x();
        if (edit_tool_ == TimelineEditTool::Razor) {
          const float cut_x =
              frame_to_x(static_cast<double>(razor_frame(px)));
          ctx.panel(gui::Rect{cut_x - 0.5F, clip_bar.y - 2.0F, 1.5F,
                              clip_bar.height + 4.0F},
                    ctx.theme().warning);
        } else if (edit_tool_ == TimelineEditTool::Standard) {
          const bool on_fade =
              ctx.pointer_y() <= clip_bar.y + fade_handle_reach &&
              (std::fabs(px - clip_fade_in_x) <= fade_handle_reach ||
               std::fabs(px - clip_fade_out_x) <= fade_handle_reach);
          if (on_fade) {
            // The handle itself shows the hover.
          } else if (px <= clip_bar.x + grab) {
            ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                      row.color);
            ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          } else if (px >= clip_bar.x + clip_bar.width - grab) {
            ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab,
                                clip_bar.y, grab, clip_bar.height},
                      row.color);
            ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          } else {
            ctx.set_cursor(gui::Cursor::Move);
          }
        }
      }
    } else if (row.kind == Row::Kind::Layer) {
      // A layer with no placement is present for the whole show: a quiet
      // band in its colour, so it reads as "always on" rather than as a clip.
      const float x0 = frame_to_x(static_cast<double>(first_i));
      const float x1 = frame_to_x(static_cast<double>(last_i));
      const gui::Rect clip{x0, y + 5.0F, std::max(x1 - x0, 1.0F),
                           row_height - 10.0F};
      ctx.panel(clip, alpha(row.color, row.element_visible ? 0.16F : 0.06F));
      ctx.panel(gui::Rect{clip.x, clip.y, clip.width, 1.0F},
                alpha(row.color, row.element_visible ? 0.45F : 0.18F));
      if (row.element_kind == ElementKind::Media) {
        const float begin = std::max(clip.x, graph.x);
        const float end = std::min(clip.x + clip.width, graph.x + graph.width);
        std::size_t sample = 0U;
        for (float x = begin + 2.0F; x < end - 1.0F; x += 5.0F) {
          const core::u64 pattern =
              row.node.value() * 17U + sample * 29U + sample * sample;
          const float amplitude = 2.0F + static_cast<float>(pattern % 7U);
          ctx.panel(gui::Rect{x, centre_y - amplitude * 0.5F, 1.0F, amplitude},
                    alpha(row.color, 0.78F));
          ++sample;
        }
      }
    }

    const auto draw_frames = [&](const std::vector<core::i64> *frames,
                                 gui::UiColor color) {
      if (frames == nullptr) {
        return;
      }
      for (const core::i64 key : *frames) {
        draw_diamond(ctx, frame_to_x(static_cast<double>(key)), centre_y,
                     key_radius, color);
      }
    };

    const scene::ChannelKeyframes *component_channel = nullptr;
    const std::vector<core::i64> *component_frames = nullptr;
    std::vector<float> component_x;
    std::vector<float> component_y;
    scene::TransformChannel channel{};
    bool component_visible = true;
    gui::UiColor component_color = ctx.theme().keyframe;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      channel = row_channel_identity->property;
      const doc::NodeId channel_target = row_channel_identity->target;
      component_visible = !hidden_channels_.contains(*row_channel_identity);
      component_color = channel_color(channel);
      component_channel = component_visible
                              ? channel_for(keyframes, channel_target, channel)
                              : nullptr;
      component_frames =
          component_channel != nullptr ? &component_channel->frames : nullptr;
      if (component_frames != nullptr) {
        component_x.reserve(component_frames->size());
        component_y.reserve(component_frames->size());

        double minimum_value = std::numeric_limits<double>::infinity();
        double maximum_value = -std::numeric_limits<double>::infinity();
        const bool has_values = component_channel->values.size() ==
                                component_channel->frames.size();
        if (graph_mode_ && has_values) {
          for (const double value : component_channel->values) {
            if (std::isfinite(value)) {
              minimum_value = std::min(minimum_value, value);
              maximum_value = std::max(maximum_value, value);
            }
          }
        }
        const double value_span = maximum_value - minimum_value;
        const auto value_y = [&](std::size_t key_index) noexcept {
          if (!graph_mode_ || !has_values || !std::isfinite(value_span) ||
              value_span <= 1.0e-12) {
            return centre_y;
          }
          const double value = component_channel->values[key_index];
          if (!std::isfinite(value)) {
            return centre_y;
          }
          const double normal = 0.5 +
                                (((maximum_value - value) / value_span) - 0.5) *
                                    static_cast<double>(curve_value_zoom_) +
                                static_cast<double>(curve_value_pan_);
          return y + 4.0F + static_cast<float>(normal) * (row_height - 8.0F);
        };

        if (graph_mode_ && has_values && std::isfinite(value_span) &&
            value_span > 1.0e-12) {
          ctx.draw_list().add_line(graph.x, centre_y, graph.x + graph.width,
                                   centre_y, 1.0F,
                                   alpha(component_color, 0.18F));
          std::array<char, 32U> maximum_label{};
          std::array<char, 32U> minimum_label{};
          const int maximum_length =
              std::snprintf(maximum_label.data(), maximum_label.size(), "%.3g",
                            maximum_value);
          const int minimum_length =
              std::snprintf(minimum_label.data(), minimum_label.size(), "%.3g",
                            minimum_value);
          ctx.label(gui::Rect{graph.x + 3.0F, y, std::min(graph.width, 52.0F),
                              row_height * 0.5F},
                    std::string_view{
                        maximum_label.data(),
                        static_cast<std::size_t>(std::max(maximum_length, 0))},
                    alpha(component_color, 0.68F));
          ctx.label(gui::Rect{graph.x + 3.0F, centre_y,
                              std::min(graph.width, 52.0F), row_height * 0.5F},
                    std::string_view{
                        minimum_label.data(),
                        static_cast<std::size_t>(std::max(minimum_length, 0))},
                    alpha(component_color, 0.68F));
        }

        float previous_x = 0.0F;
        float previous_y = centre_y;
        bool have_previous = false;
        for (std::size_t key_index = 0U; key_index < component_frames->size();
             ++key_index) {
          const core::i64 key_frame = (*component_frames)[key_index];
          const SelectedKey identity{channel_target, channel, key_frame};
          const auto moving =
              std::find(drag_keys_.begin(), drag_keys_.end(), identity);
          const bool moving_key = dragging_key_ && moving != drag_keys_.end();
          const core::i64 shown_frame = moving_key && !duplicate_drag_
                                            ? key_frame + drag_delta_
                                            : key_frame;
          const float key_x = frame_to_x(static_cast<double>(shown_frame));
          const float key_y = value_y(key_index);
          if (graph_mode_ && have_previous) {
            const auto interpolation =
                component_channel->interpolations.size() ==
                        component_channel->frames.size()
                    ? component_channel->interpolations[key_index - 1U]
                    : scene::KeyInterpolation::Linear;
            if (interpolation == scene::KeyInterpolation::Hold) {
              ctx.draw_list().add_line(previous_x, previous_y, key_x,
                                       previous_y, 1.0F,
                                       alpha(component_color, 0.72F));
              ctx.draw_list().add_line(key_x, previous_y, key_x, key_y, 1.0F,
                                       alpha(component_color, 0.72F));
            } else if (interpolation == scene::KeyInterpolation::Cubic) {
              float curve_x = previous_x;
              float curve_y = previous_y;
              constexpr std::size_t samples = 8U;
              for (std::size_t sample = 1U; sample <= samples; ++sample) {
                const float t =
                    static_cast<float>(sample) / static_cast<float>(samples);
                const float eased = t * t * (3.0F - 2.0F * t);
                const float next_x = previous_x + (key_x - previous_x) * t;
                const float next_y = previous_y + (key_y - previous_y) * eased;
                ctx.draw_list().add_line(curve_x, curve_y, next_x, next_y, 1.0F,
                                         alpha(component_color, 0.72F));
                curve_x = next_x;
                curve_y = next_y;
              }
            } else {
              ctx.draw_list().add_line(previous_x, previous_y, key_x, key_y,
                                       1.0F, alpha(component_color, 0.72F));
            }
          }
          have_previous = true;
          previous_x = key_x;
          previous_y = key_y;
          component_x.push_back(key_x);
          component_y.push_back(key_y);

          const bool is_key_selected = key_selected(identity);
          if (is_key_selected) {
            draw_diamond(ctx, key_x, key_y, key_radius + 2.0F,
                         alpha(ctx.theme().accent, 0.50F));
          }
          draw_diamond(ctx, key_x, key_y,
                       is_key_selected ? key_radius + 0.5F : key_radius,
                       is_key_selected ? ctx.theme().playhead
                                       : component_color);
          if (moving_key && duplicate_drag_ && drag_delta_ != 0) {
            const float duplicate_x =
                frame_to_x(static_cast<double>(key_frame + drag_delta_));
            draw_diamond(ctx, duplicate_x, key_y, key_radius + 0.5F,
                         ctx.theme().accent);
          }
          if (!row.locked) {
            visible_keys.push_back(VisibleKeyPoint{identity, key_x, key_y});
          }
        }
      }
    } else if (row.kind == Row::Kind::Group) {
      for (core::u8 component = 0U; component < row.component_count;
           ++component) {
        const auto aggregate = group_channel(row.group, component);
        if (hidden_channels_.contains(ChannelIdentity{row.node, aggregate})) {
          continue;
        }
        draw_frames(frames_for(keyframes, row.node, aggregate),
                    alpha(channel_color(aggregate), 0.58F));
      }
    }

    const gui::Interaction lane_it = ctx.grab(
        mix(id_, "lane", static_cast<core::u64>(index)),
        gui::Rect{graph.x, interaction_y, graph.width, interaction_height});
    if (ctx.secondary_pointer_pressed() && interaction_height > 0.0F &&
        ctx.pointer_y() >= interaction_y &&
        ctx.pointer_y() < interaction_y + interaction_height &&
        ctx.pointer_x() >= rows_bounds.x &&
        ctx.pointer_x() < rows_bounds.x + rows_bounds.width) {
      right_row = index;
    }
    // The razor cuts where it lands and starts no drag; Shift cuts every
    // unlocked clip under that frame, on every track.
    // A native event batch may already contain motion or release after the
    // down edge. Classify the gesture where it began, not at that endpoint.
    const float press_x = ctx.pointer_press_x();
    const float press_y = ctx.pointer_press_y();
    const bool razor_press = !row.locked && lane_it.pressed &&
                             clip_element != nullptr &&
                             edit_tool_ == TimelineEditTool::Razor &&
                             clip_bar.contains(press_x, press_y);
    if (razor_press) {
      clip_press_consumed_ = true;
      const core::i64 cut = razor_frame(press_x);
      for (const ElementInfo &element : elements) {
        if (!editable_clip(element) ||
            (element.node != row.node && !ctx.shift_held())) {
          continue;
        }
        if (cut > element.timing.start && cut <= element.timing.last_frame()) {
          frame.clip_splits.push_back(
              TimelineClipSplit{element.node, cut});
        }
      }
    } else if (!row.locked && lane_it.pressed && clip_element != nullptr &&
               (clip_bar.contains(press_x, press_y) ||
                (edit_tool_ == TimelineEditTool::Standard &&
                 press_y >= clip_bar.y - 4.0F &&
                 press_y <= clip_bar.y + fade_handle_reach &&
                 (std::fabs(press_x - clip_fade_in_x) <= fade_handle_reach ||
                  std::fabs(press_x - clip_fade_out_x) <= fade_handle_reach)))) {
      clip_press_consumed_ = true;
      selected_keys_.clear();
      drag_keys_.clear();
      dragging_key_ = false;
      duplicate_drag_ = false;
      selected_markers_.clear();
      const float grab = edge_grab_width(clip_bar.width);
      const float px = press_x;
      const float midpoint = clip_bar.x + clip_bar.width * 0.5F;
      clip_drag_peer_ = doc::NodeId{};
      doc::NodeId dragged = row.node;
      scene::ClipTiming dragged_timing = clip_timing;
      if (edit_tool_ == TimelineEditTool::RippleTrim) {
        clip_drag_ =
            px < midpoint ? ClipDrag::RippleTrimHead : ClipDrag::RippleTrimTail;
      } else if (edit_tool_ == TimelineEditTool::Roll) {
        // Roll the cut nearest the press: the clip butted against this
        // one's head on the left half, its tail on the right half, falling
        // back to whichever neighbour exists.
        const auto neighbour = [&](bool before) -> const ElementInfo * {
          for (const ElementInfo &candidate : elements) {
            if (candidate.node == row.node || !editable_clip(candidate) ||
                candidate.timeline_lane != clip_element->timeline_lane) {
              continue;
            }
            const core::i64 candidate_end =
                candidate.timing.start + candidate.timing.duration;
            if (before ? candidate_end == clip_timing.start
                       : candidate.timing.start ==
                             clip_timing.start + clip_timing.duration) {
              return &candidate;
            }
          }
          return nullptr;
        };
        const ElementInfo *left_of = neighbour(true);
        const ElementInfo *right_of = neighbour(false);
        const bool head = px < midpoint;
        const ElementInfo *peer = head ? (left_of ? left_of : right_of)
                                       : (right_of ? right_of : left_of);
        if (peer != nullptr) {
          clip_drag_ = ClipDrag::Roll;
          // The earlier clip leads, so the request reads left to right.
          const bool peer_first = peer->timing.start < clip_timing.start;
          dragged = peer_first ? peer->node : row.node;
          clip_drag_peer_ = peer_first ? row.node : peer->node;
          dragged_timing = peer_first ? peer->timing : clip_timing;
        } else {
          // Nothing butts against this clip, so there is no cut to roll.
          clip_drag_ = ClipDrag::None;
          clip_press_consumed_ = true;
        }
      } else if (edit_tool_ == TimelineEditTool::Slip) {
        clip_drag_ = ClipDrag::Slip;
      } else if (edit_tool_ == TimelineEditTool::Slide) {
        clip_drag_ = ClipDrag::Slide;
      } else {
        // Standard placement editing. Fade handles sit on the bar's
        // top edge and take precedence over the trim strips.
        const bool on_top = press_y <= clip_bar.y + fade_handle_reach;
        const float to_in = std::fabs(px - clip_fade_in_x);
        const float to_out = std::fabs(px - clip_fade_out_x);
        if (on_top && to_in <= fade_handle_reach && to_in <= to_out) {
          clip_drag_ = ClipDrag::FadeIn;
        } else if (on_top && to_out <= fade_handle_reach) {
          clip_drag_ = ClipDrag::FadeOut;
        } else if (px <= clip_bar.x + grab) {
          clip_drag_ = ClipDrag::TrimIn;
        } else if (px >= clip_bar.x + clip_bar.width - grab) {
          clip_drag_ = ClipDrag::TrimOut;
        } else {
          clip_drag_ = ClipDrag::Move;
        }
      }
      clip_drag_node_ = clip_drag_ != ClipDrag::None ? dragged : doc::NodeId{};
      clip_drag_origin_ = dragged_timing;
      clip_drag_proposed_ = dragged_timing;
      clip_status_.clear();
      clip_status_error_ = false;
      constexpr core::i64 edit_limit = std::numeric_limits<core::i64>::max() / 4;
      if (dragged_timing.start < -edit_limit || dragged_timing.start > edit_limit ||
          dragged_timing.duration > edit_limit || dragged_timing.source_in > edit_limit) {
        // Imported documents can carry a wider range than the interactive
        // workspace. Refuse arithmetic beyond that range rather than wrap it.
        clip_drag_ = ClipDrag::None;
        clip_drag_node_ = doc::NodeId{};
        clip_status_ = "EDIT UNAVAILABLE  /  Clip timing exceeds the editable range";
        clip_status_error_ = true;
        clip_status_until_ = ctx.time_seconds() + 3.0;
      }
      clip_drag_anchor_ = static_cast<core::i64>(std::llround(x_to_frame(px)));
      clip_drag_delta_ = 0;
      clip_drag_start_x_ = press_x;
      clip_drag_start_y_ = press_y;
      // A whole gesture can be captured after the proposal phase above.
      // Preserve its up position until the next draw resolves it, even if
      // later motion in this batch or the next one moves the free pointer.
      clip_drag_release_ = lane_it.released
          ? std::optional{gui::UiPoint{ctx.pointer_release_x(),
                                       ctx.pointer_release_y()}}
          : std::nullopt;
      clip_drag_moved_ = false;
      clip_drop_lane_.reset();
      clip_snap_guide_.reset();
      marquee_tracking_ = false;
      marquee_active_ = false;
      lasso_points_.clear();
      TimelineSelectionRequest next{};
      next.node = row.node;
      next.action = selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
      if (next.action == SelectionAction::Range) {
        next.visible_order = visible_order;
      }
      frame.selection = std::move(next);
    } else if (lane_it.pressed) {
      bool hit_key = false;
      if ((row.kind == Row::Kind::Component ||
           row.kind == Row::Kind::Property) &&
          component_frames != nullptr) {
        for (std::size_t key_index = 0U; key_index < component_frames->size();
             ++key_index) {
          const core::i64 key_frame = (*component_frames)[key_index];
          const float dx = component_x[key_index] - press_x;
          const float dy = component_y[key_index] - press_y;
          if (dx * dx + dy * dy <= key_hit_radius * key_hit_radius) {
            const SelectedKey hit{row.kind == Row::Kind::Property
                                      ? row.property_target
                                      : row.node,
                                  channel, key_frame};
            active_channel_ = ChannelIdentity{hit.target, hit.component};
            bool begin_drag = true;
            const auto selected_it =
                std::find(selected_keys_.begin(), selected_keys_.end(), hit);
            if (ctx.ctrl_held()) {
              if (selected_it != selected_keys_.end()) {
                selected_keys_.erase(selected_it);
                begin_drag = false;
              } else {
                selected_keys_.push_back(hit);
              }
            } else if (ctx.shift_held()) {
              if (selected_it == selected_keys_.end()) {
                selected_keys_.push_back(hit);
              }
            } else if (selected_it == selected_keys_.end()) {
              selected_keys_.assign(1U, hit);
            }
            canonicalize_key_selection();

            if (begin_drag) {
              drag_keys_.clear();
              for (const SelectedKey &selected_key : selected_keys_) {
                const auto *const authored = frames_for(
                    keyframes, selected_key.target, selected_key.component);
                if (authored != nullptr &&
                    std::find(authored->begin(), authored->end(),
                              selected_key.frame) != authored->end()) {
                  drag_keys_.push_back(selected_key);
                }
              }
              if (drag_keys_.empty()) {
                drag_keys_.push_back(hit);
              }
              dragging_key_ = true;
              // Alt is latched at press: releasing the modifier
              // before the pointer does not turn a duplicate into
              // a destructive retime halfway through the gesture.
              duplicate_drag_ = ctx.alt_held();
              drag_anchor_frame_ = key_frame;
              drag_delta_ = 0;
              marquee_tracking_ = false;
              marquee_active_ = false;
              lasso_points_.clear();
            }
            hit_key = true;
            selected_markers_.clear();
            break;
          }
        }
      }
      if (!hit_key && !ctx.ctrl_held() && !ctx.shift_held()) {
        TimelineSelectionRequest clear{};
        clear.action = SelectionAction::Clear;
        frame.selection = std::move(clear);
      }
      if (!hit_key && !row.locked) {
        marquee_base_ = selected_keys_;
        marquee_shift_ = ctx.shift_held();
        marquee_ctrl_ = ctx.ctrl_held();
        if (!marquee_shift_ && !marquee_ctrl_) {
          selected_keys_.clear();
        }
        marquee_tracking_ = true;
        marquee_active_ = false;
        marquee_start_x_ = press_x;
        marquee_start_y_ = press_y;
        lasso_points_.clear();
        if (lasso_mode_) {
          lasso_points_.push_back(
              gui::UiPoint{marquee_start_x_, marquee_start_y_});
        }
        frame.seek_to_frame =
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
      } else if (row.locked) {
        frame.seek_to_frame =
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
      }
    }
    if (lane_it.held && !dragging_key_ && !marquee_active_ &&
        clip_drag_ == ClipDrag::None && !clip_press_consumed_ &&
        layer_drag_node_ == doc::NodeId{} &&
        lane_drag_node_ == doc::NodeId{}) {
      frame.seek_to_frame =
          rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
    }
    ctx.pop_clip();
  }

  ctx.pop_clip();
  rename_.end_frame(ctx);

  // F2 renames the selected layer or track while the timeline has the
  // operator's attention.
  if (timeline_focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Rename)) {
    for (const Row &row : rows) {
      if (row.kind == Row::Kind::LaneHeader && row.node == selected_lane_ &&
          !row.locked) {
        rename_.begin(ctx, row.node.value(), row.label);
        break;
      }
      if (row.kind == Row::Kind::Layer && !row.surface && row.node == anchor &&
          !row.locked) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->linked &&
            element->kind != ElementKind::Environment) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
        break;
      }
    }
  }

  // Resolve the track drop after every band has been drawn. Reordering is an
  // insertion between peers, never a drop into a peer, so the only destination
  // feedback is the exact landing edge. Crossing several tracks still reports
  // one absolute document edit rather than one adjacent swap per band.
  const LaneDropTarget *lane_drop_hovered = nullptr;
  std::optional<float> lane_drop_landing_y;
  std::string lane_drag_label{"TRACK"};
  bool lane_drag_feedback{};
  bool lane_drop_committed{};
  if (lane_drag_node_ != doc::NodeId{}) {
    std::vector<doc::NodeId> stack;
    for (const scene::TimelineLane &lane : lanes) {
      if (!lane.implicit) {
        stack.push_back(lane.id);
      }
    }
    const auto dragged = std::find(stack.begin(), stack.end(), lane_drag_node_);
    if (dragged == stack.end()) {
      // The track went away under the drag - a peer deleted it, or the scene
      // changed. Nothing to reorder and nothing to report.
      lane_drag_node_ = doc::NodeId{};
      layer_drag_nodes_.clear();
      lane_drag_moved_ = false;
    } else if (lane_drag_active) {
      lane_drag_feedback = true;
      frame.lane_editing = ctx.pointer_down();
      for (const LaneDropTarget &target : lane_drop_targets) {
        if (target.bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          lane_drop_hovered = &target;
        }
      }

      if (lane_drop_hovered != nullptr) {
        const auto destination =
            std::find(stack.begin(), stack.end(), lane_drop_hovered->lane);
        if (destination != stack.end() && destination != dragged) {
          const bool toward_front = destination > dragged;
          lane_drop_landing_y =
              toward_front ? lane_drop_hovered->bounds.y
                            : lane_drop_hovered->bounds.y +
                                  lane_drop_hovered->bounds.height;
          if (!ctx.pointer_down()) {
            frame.lane_edits.push_back(TimelineLaneEdit{
                TimelineLaneAction::MoveTo, lane_drag_node_, {},
                lane_drop_hovered->lane});
            lane_drop_committed = true;
          }
        }
      }

      const auto source = std::find_if(
          lane_drop_targets.begin(), lane_drop_targets.end(),
          [&](const LaneDropTarget &target) { return target.lane == lane_drag_node_; });
      if (source != lane_drop_targets.end()) {
        lane_drag_label = source->label;
      }
      if (layer_drag_nodes_.size() > 1U) {
        lane_drag_label = std::to_string(layer_drag_nodes_.size()) + " layers";
      }
    }
    if (!ctx.pointer_down()) {
      if (lane_drag_moved_ && !lane_drop_committed) {
        frame.layer_drop = TimelineLayerDrop{
            layer_drag_nodes_, ctx.pointer_x(), ctx.pointer_y()};
      }
      lane_drag_node_ = doc::NodeId{};
      layer_drag_nodes_.clear();
      lane_drag_start_x_ = 0.0F;
      lane_drag_start_y_ = 0.0F;
      lane_drag_moved_ = false;
    }
  }

  if (marquee_tracking_ && marquee_active_ && ctx.pointer_down()) {
    ctx.push_clip(rows_bounds);
    if (lasso_mode_) {
      for (std::size_t index = 1U; index < lasso_points_.size(); ++index) {
        const gui::UiPoint &a = lasso_points_[index - 1U];
        const gui::UiPoint &b = lasso_points_[index];
        ctx.draw_list().add_line(a.x, a.y, b.x, b.y, 1.0F, ctx.theme().accent);
      }
      if (lasso_points_.size() >= 2U) {
        const gui::UiPoint &first_point = lasso_points_.front();
        const gui::UiPoint &last_point = lasso_points_.back();
        ctx.draw_list().add_line(last_point.x, last_point.y, first_point.x,
                                 first_point.y, 1.0F,
                                 alpha(ctx.theme().accent, 0.65F));
      }
    } else {
      const float left =
          std::max(std::min(marquee_start_x_, ctx.pointer_x()), graph.x);
      const float right = std::min(std::max(marquee_start_x_, ctx.pointer_x()),
                                   graph.x + graph.width);
      const float top =
          std::max(std::min(marquee_start_y_, ctx.pointer_y()), rows_bounds.y);
      const float bottom = std::min(std::max(marquee_start_y_, ctx.pointer_y()),
                                    rows_bounds.y + rows_bounds.height);
      const gui::Rect marquee{left, top, std::max(right - left, 0.0F),
                              std::max(bottom - top, 0.0F)};
      ctx.panel(marquee, alpha(ctx.theme().accent, 0.15F));
      ctx.draw_list().add_line(marquee.x, marquee.y, marquee.x + marquee.width,
                               marquee.y, 1.0F, ctx.theme().accent);
      ctx.draw_list().add_line(
          marquee.x, marquee.y + marquee.height, marquee.x + marquee.width,
          marquee.y + marquee.height, 1.0F, ctx.theme().accent);
      ctx.draw_list().add_line(marquee.x, marquee.y, marquee.x,
                               marquee.y + marquee.height, 1.0F,
                               ctx.theme().accent);
      ctx.draw_list().add_line(
          marquee.x + marquee.width, marquee.y, marquee.x + marquee.width,
          marquee.y + marquee.height, 1.0F, ctx.theme().accent);
    }
    ctx.pop_clip();
  }

  if (marquee_tracking_ && !ctx.pointer_down()) {
    if (marquee_active_) {
      const float left = std::min(marquee_start_x_, ctx.pointer_x());
      const float right = std::max(marquee_start_x_, ctx.pointer_x());
      const float top = std::min(marquee_start_y_, ctx.pointer_y());
      const float bottom = std::max(marquee_start_y_, ctx.pointer_y());
      selected_keys_ = marquee_base_;
      if (!marquee_shift_ && !marquee_ctrl_) {
        selected_keys_.clear();
      }
      for (const VisibleKeyPoint &point : visible_keys) {
        const bool hit = lasso_mode_
                             ? point_in_polygon(point.x, point.y, lasso_points_)
                             : (point.x >= left && point.x <= right &&
                                point.y >= top && point.y <= bottom);
        if (!hit) {
          continue;
        }
        const auto found =
            std::find(selected_keys_.begin(), selected_keys_.end(), point.key);
        if (marquee_ctrl_) {
          if (found == selected_keys_.end()) {
            selected_keys_.push_back(point.key);
          } else {
            selected_keys_.erase(found);
          }
        } else if (found == selected_keys_.end()) {
          selected_keys_.push_back(point.key);
        }
      }
      canonicalize_key_selection();
    }
    marquee_tracking_ = false;
    marquee_active_ = false;
    marquee_base_.clear();
    lasso_points_.clear();
  }

  // Over the rows: what lies outside the In/Out range is dimmed rather than
  // hidden, cues and markers run their line down through the clips they are
  // timed against, then the snap guide and the playhead on top of all of it.
  const gui::Rect time_area{graph.x, ruler.y, graph.width,
                            ruler.height + strip.height + rows_bounds.height};
  const gui::Rect lanes_area{graph.x, strip.y, graph.width,
                             strip.height + rows_bounds.height};
  ctx.push_clip(lanes_area);
  {
    const float in_x = frame_to_x(static_cast<double>(shown_first));
    const float out_x = frame_to_x(static_cast<double>(shown_last) + 1.0);
    const gui::UiColor dim = alpha(ctx.theme().background, 0.45F);
    if (in_x > graph.x) {
      ctx.panel(gui::Rect{graph.x, lanes_area.y, in_x - graph.x,
                          lanes_area.height},
                dim);
    }
    if (out_x < graph.x + graph.width) {
      ctx.panel(gui::Rect{out_x, lanes_area.y,
                          graph.x + graph.width - out_x, lanes_area.height},
                dim);
    }
    for (const scene::TimelineMarker &marker : markers) {
      const float marker_x =
          frame_to_x(static_cast<double>(shown_marker_frame(marker)));
      if (marker_x < graph.x || marker_x > graph.x + graph.width) {
        continue;
      }
      const gui::UiColor color =
          timeline.held_cue == marker.id
              ? ctx.theme().playhead
              : timeline_marker_color(marker.color);
      ctx.panel(gui::Rect{marker_x, rows_bounds.y, 1.0F, rows_bounds.height},
                alpha(color, marker.is_cue() ? 0.45F : 0.22F));
    }
  }
  ctx.pop_clip();
  ctx.push_clip(time_area);
  const float playhead_x = frame_to_x(static_cast<double>(timeline.show_frame));
  const std::optional<core::i64> guide =
      dragging_key_
          ? snap_guide_
          : (clip_drag_ != ClipDrag::None
                 ? clip_snap_guide_
                 : (marker_drag_moved_ ? marker_snap_guide_
                                       : std::optional<core::i64>{}));
  if (guide.has_value()) {
    const float guide_x = frame_to_x(static_cast<double>(*guide));
    if (guide_x >= graph.x && guide_x <= graph.x + graph.width) {
      ctx.draw_list().add_line(guide_x, ruler.y, guide_x,
                               rows_bounds.y + rows_bounds.height, 1.0F,
                               alpha(ctx.theme().accent, 0.72F));
    }
  }
  if (playhead_x >= graph.x && playhead_x <= graph.x + graph.width) {
    ctx.draw_list().add_line(playhead_x, ruler.y, playhead_x,
                             rows_bounds.y + rows_bounds.height, 1.5F,
                             ctx.theme().playhead);
    // A shield-shaped head on the ruler: a handle the eye finds first.
    const std::array<gui::UiPoint, 5U> head{{
        {playhead_x - 5.5F, ruler.y + 1.0F},
        {playhead_x + 5.5F, ruler.y + 1.0F},
        {playhead_x + 5.5F, ruler.y + 9.0F},
        {playhead_x, ruler.y + 14.0F},
        {playhead_x - 5.5F, ruler.y + 9.0F},
    }};
    ctx.draw_list().add_convex_poly(head, ctx.theme().playhead);
  }
  ctx.pop_clip();

  // A slim non-interactive scroll thumb is enough here: wheel owns row
  // navigation, and the indicator makes a clipped layer stack obvious.
  if (maximum_row_scroll > 0.0F && rows_bounds.height > 0.0F) {
    const float fraction = rows_bounds.height / content_height;
    const float thumb_height = std::max(rows_bounds.height * fraction, 20.0F);
    const float travel = std::max(rows_bounds.height - thumb_height, 0.0F);
    const float position =
        maximum_row_scroll > 0.0F ? row_scroll_ / maximum_row_scroll : 0.0F;
    ctx.push_clip(rows_bounds);
    ctx.panel(gui::Rect{bounds.x + bounds.width - 4.0F,
                        rows_bounds.y + travel * position, 3.0F, thumb_height},
              ctx.theme().widget_hover);
    ctx.pop_clip();
  }

  // Commit the complete selection once, on release. Moving positive time is
  // authored from the last key backwards (and negative time first-key
  // forwards), preventing one destination from hiding another source key in
  // the same channel while the batch is applied.
  if (dragging_key_ && !ctx.pointer_down()) {
    update_key_drag();
    if (drag_delta_ != 0) {
      TimelineKeyBatchEdit batch{};
      batch.action = duplicate_drag_ ? TimelineKeyBatchAction::Duplicate
                                     : TimelineKeyBatchAction::Move;
      batch.delta = drag_delta_;
      batch.keys.reserve(drag_keys_.size());
      for (const SelectedKey &key : drag_keys_) {
        batch.keys.push_back(
            TimelineKeyAddress{key.target, key.component, key.frame});
      }
      std::sort(batch.keys.begin(), batch.keys.end());
      batch.keys.erase(std::unique(batch.keys.begin(), batch.keys.end()),
                       batch.keys.end());
      frame.key_batch = std::move(batch);
    }
    dragging_key_ = false;
    duplicate_drag_ = false;
    drag_keys_.clear();
    drag_delta_ = 0;
    snap_guide_.reset();
  }

  if (key_popup_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (key_tools_open_) {
    ctx.panel(key_tools_popup, ctx.theme().background);
    ctx.panel(gui::Rect{key_tools_popup.x, key_tools_popup.y,
                        key_tools_popup.width, 1.0F},
              ctx.theme().divider);
    ctx.panel(gui::Rect{key_tools_popup.x, key_tools_popup.y, 1.0F,
                        key_tools_popup.height},
              ctx.theme().divider);
    ctx.panel(gui::Rect{key_tools_popup.x + key_tools_popup.width - 1.0F,
                        key_tools_popup.y, 1.0F, key_tools_popup.height},
              ctx.theme().divider);

    float tool_x = key_tools_popup.x + 6.0F;
    const float tool_y = key_tools_popup.y + 6.0F;
    if (ctx.button(mix(id_, "key selection shape"),
                   gui::Rect{tool_x, tool_y, 53.0F, 22.0F},
                   lasso_mode_ ? "LASSO" : "BOX")) {
      lasso_mode_ = !lasso_mode_;
      marquee_tracking_ = false;
      marquee_active_ = false;
      lasso_points_.clear();
    }
    tool_x += 56.0F;
    const std::vector<TimelineKeyAddress> keys = selected_addresses();
    if (!keys.empty()) {
      if (ctx.button(mix(id_, "fit selected keys"),
                     gui::Rect{tool_x, tool_y, 59.0F, 22.0F}, "FIT SEL")) {
        core::i64 selected_first = keys.front().frame;
        core::i64 selected_last = keys.front().frame;
        for (const TimelineKeyAddress &key : keys) {
          selected_first = std::min(selected_first, key.frame);
          selected_last = std::max(selected_last, key.frame);
        }
        const double span = std::max(static_cast<double>(selected_last) -
                                         static_cast<double>(selected_first),
                                     2.0);
        const double padding = std::max(span * 0.1, 1.0);
        const double fitted_begin =
            std::max(static_cast<double>(first_i),
                     static_cast<double>(selected_first) - padding);
        const double fitted_end =
            std::min(static_cast<double>(last_i),
                     static_cast<double>(selected_last) + padding);
        const double fitted_span = std::max(fitted_end - fitted_begin, 1.0);
        pixels_per_frame_ = std::clamp(
            static_cast<float>(static_cast<double>(graph.width) / fitted_span),
            minimum_pixels_per_frame, maximum_pixels_per_frame);
        left_frame_ = fitted_begin;
        curve_value_zoom_ = 1.0F;
        curve_value_pan_ = 0.0F;
      }
    } else {
      const gui::Rect disabled{tool_x, tool_y, 59.0F, 22.0F};
      ctx.panel(disabled, ctx.theme().track);
      ctx.label(disabled, "FIT SEL", ctx.theme().text_muted);
    }
    tool_x += 62.0F;
    // Zooming and panning either axis is a wheel gesture over the graph, so
    // there are no stepper buttons here for it. What is left is the one thing
    // the wheel cannot do: put the whole animation back on screen.
    if (ctx.button(mix(id_, "fit curves"),
                   gui::Rect{tool_x, tool_y, 74.0F, 22.0F}, "FIT ALL")) {
      const double span =
          std::max(static_cast<double>(last_i) - static_cast<double>(first_i),
                   1.0);
      pixels_per_frame_ = std::clamp(
          static_cast<float>(static_cast<double>(graph.width) / span),
          minimum_pixels_per_frame, maximum_pixels_per_frame);
      left_frame_ = static_cast<double>(first_i);
      curve_value_zoom_ = 1.0F;
      curve_value_pan_ = 0.0F;
    }

    const float channel_y = tool_y + 28.0F;
    if (active_channel_.has_value()) {
      std::array<char, 128U> active_label{};
      const int active_length = std::snprintf(
          active_label.data(), active_label.size(), "%s - #%llu",
          channel_label(active_channel_->property).data(),
          static_cast<unsigned long long>(active_channel_->target.value()));
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, channel_y, 170.0F, 22.0F},
                std::string_view{
                    active_label.data(),
                    static_cast<std::size_t>(std::max(active_length, 0))},
                channel_color(active_channel_->property));
      const bool hidden = hidden_channels_.contains(*active_channel_);
      if (ctx.button(
              mix(id_, "active channel visibility"),
              gui::Rect{key_tools_popup.x + 178.0F, channel_y, 50.0F, 22.0F},
              hidden ? "SHOW" : "HIDE")) {
        if (hidden) {
          hidden_channels_.erase(*active_channel_);
        } else {
          hidden_channels_.insert(*active_channel_);
          selected_keys_.erase(
              std::remove_if(selected_keys_.begin(), selected_keys_.end(),
                             [&](const SelectedKey &key) {
                               return key.target == active_channel_->target &&
                                      key.component ==
                                          active_channel_->property;
                             }),
              selected_keys_.end());
          drag_keys_.erase(
              std::remove_if(drag_keys_.begin(), drag_keys_.end(),
                             [&](const SelectedKey &key) {
                               return key.target == active_channel_->target &&
                                      key.component ==
                                          active_channel_->property;
                             }),
              drag_keys_.end());
          if (drag_keys_.empty()) {
            dragging_key_ = false;
            duplicate_drag_ = false;
          }
        }
      }
      const scene::ChannelKeyframes *const active_keyframes = channel_for(
          keyframes, active_channel_->target, active_channel_->property);
      const std::optional<double> sampled =
          sample_channel_at(active_keyframes, timeline.show_frame);
      if (sampled.has_value() && std::isfinite(*sampled)) {
        double changed_value = *sampled;
        const double speed = std::max(std::fabs(*sampled) * 0.01, 0.01);
        if (key_value_field_.draw(
                ctx, mix(id_, "playhead key value"),
                gui::Rect{key_tools_popup.x + 232.0F, channel_y,
                          key_tools_popup.width - 238.0F, 22.0F},
                "", *sampled, speed, changed_value, 4)) {
          frame.keys.push_back(TimelineKeyEdit{
              active_channel_->target, active_channel_->property,
              timeline.show_frame, changed_value, false, false});
        }
      } else {
        ctx.label(gui::Rect{key_tools_popup.x + 232.0F, channel_y,
                            key_tools_popup.width - 238.0F, 22.0F},
                  "NO VALUE", ctx.theme().text_muted);
      }
    } else {
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, channel_y,
                          key_tools_popup.width - 12.0F, 22.0F},
                "Select a key to edit its value at the playhead",
                ctx.theme().text_muted);
    }

    const float scale_button_y = channel_y + 28.0F;
    if (!keys.empty()) {
      if (ctx.button(
              mix(id_, "toggle key scale"),
              gui::Rect{key_tools_popup.x + 6.0F, scale_button_y, 62.0F, 22.0F},
              scale_open_ ? "SCALE*" : "SCALE")) {
        scale_open_ = !scale_open_;
      }
    } else {
      const gui::Rect disabled{key_tools_popup.x + 6.0F, scale_button_y, 62.0F,
                               22.0F};
      ctx.panel(disabled, ctx.theme().track);
      ctx.label(disabled, "SCALE", ctx.theme().text_muted);
      scale_open_ = false;
    }
    std::array<char, 64U> selection_label{};
    const int selection_length =
        std::snprintf(selection_label.data(), selection_label.size(),
                      "%zu selected", keys.size());
    ctx.label(gui::Rect{key_tools_popup.x + 75.0F, scale_button_y,
                        key_tools_popup.width - 81.0F, 22.0F},
              std::string_view{
                  selection_label.data(),
                  static_cast<std::size_t>(std::max(selection_length, 0))},
              ctx.theme().text_muted);

    if (scale_open_) {
      const float anchor_y = scale_button_y + 28.0F;
      constexpr std::array<std::pair<scene::KeyTimeAnchor, std::string_view>,
                           4U>
          anchors{{
              {scene::KeyTimeAnchor::FirstSelected, "FIRST"},
              {scene::KeyTimeAnchor::LastSelected, "LAST"},
              {scene::KeyTimeAnchor::Playhead, "PLAYHEAD"},
              {scene::KeyTimeAnchor::User, "USER"},
          }};
      float anchor_x = key_tools_popup.x + 6.0F;
      const float anchor_width = (key_tools_popup.width - 21.0F) / 4.0F;
      for (std::size_t index = 0U; index < anchors.size(); ++index) {
        const gui::Rect anchor_bounds{anchor_x, anchor_y, anchor_width, 22.0F};
        const gui::Interaction interaction =
            ctx.grab(mix(id_, "scale anchor", index), anchor_bounds);
        ctx.panel(anchor_bounds,
                  scale_anchor_ == anchors[index].first
                      ? ctx.theme().selection
                      : (interaction.hovered ? ctx.theme().widget_hover
                                             : ctx.theme().widget));
        ctx.label(anchor_bounds, anchors[index].second,
                  scale_anchor_ == anchors[index].first
                      ? ctx.theme().text
                      : ctx.theme().text_muted);
        if (interaction.clicked) {
          scale_anchor_ = anchors[index].first;
        }
        anchor_x += anchor_width + 3.0F;
      }

      const float rational_y = anchor_y + 28.0F;
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, rational_y, 18.0F, 22.0F},
                "N", ctx.theme().text_muted);
      static_cast<void>(ctx.text_field(
          scale_numerator_id,
          gui::Rect{key_tools_popup.x + 24.0F, rational_y, 62.0F, 22.0F},
          scale_numerator_));
      ctx.label(gui::Rect{key_tools_popup.x + 91.0F, rational_y, 18.0F, 22.0F},
                "D", ctx.theme().text_muted);
      static_cast<void>(ctx.text_field(
          scale_denominator_id,
          gui::Rect{key_tools_popup.x + 109.0F, rational_y, 62.0F, 22.0F},
          scale_denominator_));
      ctx.label(gui::Rect{key_tools_popup.x + 177.0F, rational_y, 43.0F, 22.0F},
                "MARK", ctx.theme().text_muted);
      static_cast<void>(
          ctx.text_field(scale_user_anchor_id,
                         gui::Rect{key_tools_popup.x + 220.0F, rational_y,
                                   key_tools_popup.width - 226.0F, 22.0F},
                         scale_user_anchor_));

      const std::optional<core::i64> numerator =
          parse_integer(scale_numerator_);
      const std::optional<core::i64> denominator =
          parse_integer(scale_denominator_);
      const std::optional<core::i64> user_anchor =
          parse_integer(scale_user_anchor_);
      const bool rational_valid =
          numerator.has_value() && denominator.has_value() &&
          *denominator > 0 &&
          (scale_anchor_ != scene::KeyTimeAnchor::User ||
           user_anchor.has_value());
      std::optional<core::u64> signature;
      scene::KeyTimeScale scale{};
      if (rational_valid) {
        scale.numerator = *numerator;
        scale.denominator = *denominator;
        scale.anchor = scale_anchor_;
        scale.playhead = signed_frame(timeline.show_frame);
        scale.user_anchor = user_anchor.value_or(0);
        signature = key_scale_signature(keys, scale);
        frame.key_scale_preview =
            TimelineKeyScalePreviewRequest{*signature, keys, scale};
      }
      const bool preview_matches = signature.has_value() &&
                                   key_scale_preview.has_value() &&
                                   key_scale_preview->signature == *signature;
      const bool apply_ready = preview_matches && key_scale_preview->valid;
      const float preview_y = rational_y + 28.0F;
      std::array<char, 128U> preview_label{};
      int preview_length{};
      if (!rational_valid) {
        preview_length = std::snprintf(
            preview_label.data(), preview_label.size(),
            "Invalid scale: signed N, positive D, integer marker");
      } else if (!preview_matches) {
        preview_length =
            std::snprintf(preview_label.data(), preview_label.size(),
                          "Checking selection and collisions...");
      } else {
        preview_length = std::snprintf(
            preview_label.data(), preview_label.size(),
            "%zu apply - %zu collisions - %zu skipped",
            key_scale_preview->applied, key_scale_preview->collisions,
            key_scale_preview->skips);
      }
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, preview_y,
                          key_tools_popup.width - 82.0F, 22.0F},
                std::string_view{
                    preview_label.data(),
                    static_cast<std::size_t>(std::max(preview_length, 0))},
                !rational_valid || (preview_matches && !apply_ready)
                    ? ctx.theme().warning
                    : ctx.theme().text_muted);
      const gui::Rect apply_bounds{key_tools_popup.x + key_tools_popup.width -
                                       70.0F,
                                   preview_y, 64.0F, 22.0F};
      if (apply_ready) {
        if (ctx.button(mix(id_, "apply key scale"), apply_bounds, "APPLY")) {
          frame.key_batch = TimelineKeyBatchEdit{TimelineKeyBatchAction::Scale,
                                                 keys, 0, scale};
          scale_open_ = false;
        }
      } else {
        ctx.panel(apply_bounds, ctx.theme().track);
        ctx.label(apply_bounds, "APPLY", ctx.theme().text_muted);
      }
    }
  }
  if (popup_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (rate_menu_open_) {
    ctx.panel(rate_popup, ctx.theme().background);
    ctx.panel(gui::Rect{rate_popup.x, rate_popup.y, rate_popup.width, 1.0F},
              ctx.theme().divider);
    ctx.panel(gui::Rect{rate_popup.x, rate_popup.y, 1.0F, rate_popup.height},
              ctx.theme().divider);
    ctx.panel(gui::Rect{rate_popup.x + rate_popup.width - 1.0F, rate_popup.y,
                        1.0F, rate_popup.height},
              ctx.theme().divider);
    float choice_y = rate_popup.y + 4.0F;
    for (std::size_t index = 0U; index < common_rates.size(); ++index) {
      const RateChoice &choice = common_rates[index];
      const gui::Rect row{rate_popup.x + 4.0F, choice_y,
                          rate_popup.width - 8.0F, 21.0F};
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "show rate choice", index), row);
      ctx.panel(row, same_rate(timeline, choice.rate)
                         ? ctx.theme().selection
                         : (interaction.hovered ? ctx.theme().widget_hover
                                                : ctx.theme().row));
      ctx.label(gui::Rect{row.x + 7.0F, row.y, std::max(row.width - 7.0F, 0.0F),
                          row.height},
                choice.label,
                same_rate(timeline, choice.rate) ? ctx.theme().text
                                                 : ctx.theme().text_muted);
      if (interaction.clicked) {
        if (!same_rate(timeline, choice.rate)) {
          frame.show_rate = choice.rate;
        }
        rate_menu_open_ = false;
      }
      choice_y += 21.0F;
    }
  }

  // Drag feedback is the panel's final visual layer, just as it is in Feed
  // Mapping. The playhead, filmstrips, scroll thumb, and popups therefore
  // cannot cut through a lifted item or obscure its destination.
  if (layer_drag_active) {
    if (layer_drop_landing_y.has_value()) {
      ctx.panel(gui::Rect{rows_bounds.x, *layer_drop_landing_y - 1.5F,
                          rows_bounds.width, 3.0F},
                ctx.theme().accent);
      ctx.panel(gui::Rect{rows_bounds.x, *layer_drop_landing_y - 5.0F, 3.0F,
                          10.0F},
                ctx.theme().accent);
      ctx.panel(gui::Rect{rows_bounds.x + rows_bounds.width - 3.0F,
                          *layer_drop_landing_y - 5.0F, 3.0F, 10.0F},
                ctx.theme().accent);
    }
    if (layer_assignment_feedback) {
      for (const LaneDropTarget &target : lane_drop_targets) {
        if (target.locked) {
          continue;
        }
        const bool hovered = &target == layer_lane_hovered;
        draw_border(ctx, target.bounds,
                    hovered ? ctx.theme().accent
                            : alpha(ctx.theme().accent, 0.28F),
                    hovered ? 2.0F : 1.0F);
      }
    }
    std::string ghost_label{
        layer_assignment_feedback ? "MOVE  " : "REORDER  "};
    ghost_label += layer_drag_label;
    if (layer_lane_hovered != nullptr) {
      ghost_label += "  >  ";
      ghost_label += layer_lane_hovered->label;
    } else if (layer_assignment_feedback) {
      ghost_label += "  >  OWN TRANSFORM";
    }
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 300.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  if (lane_drag_feedback) {
    if (lane_drop_landing_y.has_value()) {
      ctx.panel(gui::Rect{rows_bounds.x, *lane_drop_landing_y - 1.5F,
                          rows_bounds.width, 3.0F},
                ctx.theme().accent);
      ctx.panel(gui::Rect{rows_bounds.x, *lane_drop_landing_y - 5.0F, 3.0F,
                          10.0F},
                ctx.theme().accent);
      ctx.panel(gui::Rect{rows_bounds.x + rows_bounds.width - 3.0F,
                          *lane_drop_landing_y - 5.0F, 3.0F, 10.0F},
                ctx.theme().accent);
    }
    std::string ghost_label{"REORDER  "};
    ghost_label += lane_drag_label;
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 240.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  if (clip_move_feedback) {
    // Track outlines mean exactly one thing here: dropping the clip into that
    // track. This is the nesting operation the same outline would falsely
    // suggest during a track reorder.
    for (const LaneDropTarget &target : lane_drop_targets) {
      if (target.locked) {
        continue;
      }
      const bool hovered = &target == clip_drop_hovered;
      draw_border(ctx, target.bounds,
                  hovered ? ctx.theme().accent
                          : alpha(ctx.theme().accent, 0.28F),
                  hovered ? 2.0F : 1.0F);
    }

    if (clip_drop_lane_.has_value()) {
      const float preview_x = frame_to_x(
          static_cast<double>(clip_drag_proposed_.start));
      const float preview_right = frame_to_x(static_cast<double>(
          clip_drag_proposed_.start + clip_drag_proposed_.duration));
      const float preview_y = std::clamp(
          ctx.pointer_y() - (row_height - 6.0F) * 0.5F,
          rows_bounds.y + 3.0F,
          rows_bounds.y + rows_bounds.height - row_height + 3.0F);
      const gui::Rect preview_bar{
          std::max(preview_x, graph.x), preview_y,
          std::max(std::min(preview_right, graph.x + graph.width) -
                       std::max(preview_x, graph.x),
                   1.0F),
          row_height - 6.0F};
      ctx.panel(preview_bar, alpha(ctx.theme().accent, 0.24F));
      draw_border(ctx, preview_bar, alpha(ctx.theme().accent, 0.90F), 1.0F);
    }

    std::string ghost_label{"MOVE  "};
    ghost_label += clip_drag_label;
    if (clip_drop_hovered != nullptr) {
      ghost_label += "  >  ";
      ghost_label += clip_drop_hovered->label;
    } else if (clip_drop_lane_.has_value() &&
               *clip_drop_lane_ == doc::NodeId{}) {
      const ElementInfo *const dragged =
          find_element(elements, clip_drag_node_);
      if (dragged != nullptr && dragged->timeline_lane != doc::NodeId{}) {
        ghost_label += "  >  OWN TRANSFORM";
      }
    }
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 300.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  // Keep source feedback readable even for very short or partly hidden clips.
  // It remains after release while the authored result reaches the editor.
  if (!clip_status_.empty() && ctx.time_seconds() <= clip_status_until_) {
    frame.clip_status = clip_status_;
    frame.clip_status_error = clip_status_error_;
    const gui::Rect status{bounds.x + 8.0F, bounds.y + bounds.height - 30.0F,
                           std::max(bounds.width - 16.0F, 0.0F), 24.0F};
    ctx.panel(status, ctx.theme().background);
    draw_border(ctx, status,
                clip_status_error_ ? ctx.theme().warning : ctx.theme().accent,
                1.0F);
    ctx.label(gui::Rect{status.x + 8.0F, status.y,
                        std::max(status.width - 16.0F, 0.0F), status.height},
              clip_status_, clip_status_error_ ? ctx.theme().warning
                                               : ctx.theme().text);
  }

  // Choosing a layer, clip, or track leaves the marker domain, so Delete and
  // the menus aim at what was just chosen.
  if (frame.selection.has_value()) {
    selected_markers_.clear();
  }
  frame.editing = frame.editing || range_fields_.live() ||
                  key_value_field_.live() || ctx.focus() == search_id ||
                  ctx.focus() == scale_numerator_id ||
                  ctx.focus() == scale_denominator_id ||
                  ctx.focus() == scale_user_anchor_id || rate_menu_open_ ||
                  time_drag_id_ != gui::UiId{} ||
                  time_edit_id_ != gui::UiId{} ||
                  marker_name_node_ != doc::NodeId{} || frame.lane_editing ||
                  rename_.active();

  // ---------------------------------------------------------------------
  // Shared actions. The context menus, the shortcuts, and the toolbar all
  // reach these, so a cut or a cue means the same thing however it is asked
  // for.
  const auto ripple_ready = [&](std::span<const ElementInfo *const> clips) {
    if (clips.empty()) {
      return false;
    }
    std::vector<const ElementInfo *> ordered{clips.begin(), clips.end()};
    std::sort(ordered.begin(), ordered.end(),
              [](const ElementInfo *left, const ElementInfo *right) noexcept {
                return left->timing.start < right->timing.start;
              });
    const doc::NodeId lane = ordered.front()->timeline_lane;
    core::i64 begin = ordered.front()->timing.start;
    core::i64 end = begin;
    for (const ElementInfo *clip : ordered) {
      const auto clip_end = exclusive_clip_end(clip->timing);
      if (!editable_clip(*clip) || !clip_end.has_value() ||
          clip->timeline_lane != lane || clip->timing.start > end) {
        return false;
      }
      end = std::max(end, *clip_end);
    }
    // Nothing else in the track may straddle the span being closed up.
    for (const ElementInfo &other : elements) {
      if (other.kind != ElementKind::Media || !other.timing.placed() ||
          other.timeline_lane != lane ||
          std::find(ordered.begin(), ordered.end(), &other) != ordered.end()) {
        continue;
      }
      const auto other_end = exclusive_clip_end(other.timing);
      if (!other_end.has_value() ||
          (other.timing.start < end && *other_end > begin)) {
        return false;
      }
    }
    return true;
  };
  const auto ripple_delete = [&](std::span<const ElementInfo *const> clips) {
    TimelineRippleDeleteRequest request{};
    for (const ElementInfo *clip : clips) {
      request.clips.push_back(clip->node);
    }
    std::sort(request.clips.begin(), request.clips.end());
    frame.timeline_edits.emplace_back(std::move(request));
  };
  const auto clear_keys = [&](doc::NodeId layer) {
    // Transform and opacity belong to the layer. Effect parameters belong to
    // their first-class effect nodes, so those addresses are cleared
    // explicitly instead of inventing brightness and blur channels on the
    // layer itself.
    for (core::u8 component = 0U;
         component <= static_cast<core::u8>(scene::PropertyChannel::Opacity);
         ++component) {
      frame.keys.push_back(TimelineKeyEdit{
          layer, static_cast<scene::TransformChannel>(component), 0U, 0.0,
          true});
    }
    if (const ElementInfo *const element = find_element(elements, layer);
        element != nullptr && element->kind == ElementKind::Camera) {
      for (auto component = static_cast<core::u8>(scene::PropertyChannel::CameraFieldOfView);
           component <= static_cast<core::u8>(scene::PropertyChannel::CameraAperture);
           ++component) {
        frame.keys.push_back(TimelineKeyEdit{
            layer, static_cast<scene::PropertyChannel>(component), 0U, 0.0, true});
      }
    }
    // A light's output and colour are its own channels too.
    if (const ElementInfo *const element = find_element(elements, layer);
        element != nullptr && is_light(element->kind)) {
      for (auto component =
               static_cast<core::u8>(scene::PropertyChannel::LightIntensity);
           component <=
           static_cast<core::u8>(scene::PropertyChannel::LightRange);
           ++component) {
        frame.keys.push_back(TimelineKeyEdit{
            layer, static_cast<scene::PropertyChannel>(component), 0U, 0.0,
            true});
      }
    }
    for (const scene::EffectInstance &effect : effects) {
      if (effect.target != layer) {
        continue;
      }
      std::optional<scene::PropertyChannel> property;
      switch (effect.kind) {
      case scene::EffectKind::Brightness:
        property = scene::PropertyChannel::Brightness;
        break;
      case scene::EffectKind::Saturation:
        property = scene::PropertyChannel::Saturation;
        break;
      case scene::EffectKind::Blur:
        property = scene::PropertyChannel::BlurRadius;
        break;
      case scene::EffectKind::BoxBlur:
        property = scene::PropertyChannel::BoxBlurRadius;
        break;
      case scene::EffectKind::DirectionalBlur:
        property = scene::PropertyChannel::DirectionalBlurLength;
        break;
      case scene::EffectKind::FilmGrain:
        property = scene::PropertyChannel::FilmGrainAmount;
        break;
      case scene::EffectKind::ChromaKey:
        property = scene::PropertyChannel::ChromaKeyMix;
        break;
      case scene::EffectKind::LumaKey:
        property = scene::PropertyChannel::LumaKeyMix;
        break;
      case scene::EffectKind::Levels:
      case scene::EffectKind::LiftGammaGain:
      case scene::EffectKind::ContrastPivot:
      case scene::EffectKind::ColorMatrix:
      case scene::EffectKind::WhiteBalance:
      case scene::EffectKind::Curves:
      case scene::EffectKind::Count:
        break;
      }
      if (property.has_value()) {
        frame.keys.push_back(
            TimelineKeyEdit{effect.source, *property, 0U, 0.0, true});
      }
    }
  };
  const auto update_marker = [&](doc::NodeId id, auto &&change) {
    if (const auto index = marker_index(id)) {
      TimelineMarkerEdit edit{TimelineMarkerAction::Update, markers[*index]};
      change(edit.marker);
      if (edit.marker != markers[*index]) {
        frame.marker_edits.push_back(std::move(edit));
      }
    }
  };
  const auto delete_markers = [&](std::span<const doc::NodeId> ids) {
    for (const doc::NodeId id : ids) {
      if (const auto index = marker_index(id)) {
        frame.marker_edits.push_back(
            TimelineMarkerEdit{TimelineMarkerAction::Delete, markers[*index]});
      }
    }
  };
  const auto open_marker_name = [&](doc::NodeId id) {
    if (const auto index = marker_index(id)) {
      marker_name_node_ = id;
      marker_name_draft_ = markers[*index].name;
      ctx.focus_text_field(mix(id_, "marker name", id.value()),
                           marker_name_draft_);
    }
  };
  const auto zoom_about_playhead = [&](double factor) {
    const double anchor = static_cast<double>(timeline.show_frame);
    const float anchor_x = frame_to_x(anchor);
    const float keep_x = anchor_x >= graph.x && anchor_x <= graph.x + graph.width
                             ? anchor_x
                             : graph.x + graph.width * 0.5F;
    pixels_per_frame_ = std::clamp(
        static_cast<float>(static_cast<double>(pixels_per_frame_) * factor),
        minimum_pixels_per_frame, maximum_pixels_per_frame);
    left_frame_ = anchor - static_cast<double>(keep_x - graph.x) /
                              static_cast<double>(pixels_per_frame_);
  };
  const auto set_in = [&](core::u64 at) {
    frame.range = RangeRequest{at, std::max(last, at), timeline.loop};
  };
  const auto set_out = [&](core::u64 at) {
    frame.range = RangeRequest{std::min(first, at), at, timeline.loop};
  };

  // ---------------------------------------------------------------------
  // Context menus. A right press names what is under it; the menu lists only
  // what applies to that thing, which is what keeps track deletion, cutting,
  // and cue conversion off a toolbar full of buttons that mostly do nothing.
  const gui::UiId menu_id = mix(id_, "context menu");
  const auto add_entry = [&](std::string label, MenuCommand command,
                             doc::NodeId node = {}, core::i64 at = 0,
                             doc::NodeId other = {}, core::i64 end = 0) {
    menu_entries_.push_back(MenuEntry{std::move(label),
                                      static_cast<core::u8>(command), node,
                                      other, at, end});
  };
  bool open_menu{};
  const core::i64 pointer_frame_i = static_cast<core::i64>(rounded_frame(
      x_to_frame(ctx.pointer_x()), 0U,
      static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
  if (ctx.secondary_pointer_pressed()) {
    menu_entries_.clear();
    const gui::Rect ruler_graph{graph.x, ruler.y, graph.width, ruler.height};
    if (right_marker.has_value()) {
      const scene::TimelineMarker &marker = markers[*right_marker];
      if (std::find(selected_markers_.begin(), selected_markers_.end(),
                    marker.id) == selected_markers_.end()) {
        selected_markers_.assign(1U, marker.id);
      }
      add_entry("Rename...", MenuCommand::RenameMarker, marker.id);
      if (marker.cue != scene::CueAction::Hold) {
        add_entry("Make Hold Cue", MenuCommand::MarkerToHold, marker.id);
      }
      if (marker.cue != scene::CueAction::Loop) {
        add_entry("Make Loop Cue", MenuCommand::MarkerToLoop, marker.id);
      }
      if (marker.is_cue()) {
        add_entry("Make Plain Marker", MenuCommand::MarkerToPlain, marker.id);
      }
      add_entry(std::string{"Color: "} +
                    std::string{timeline_marker_color_name(static_cast<core::u8>(
                        (marker.color + 1U) % scene::timeline_marker_colors))},
                MenuCommand::MarkerNextColor, marker.id);
      add_entry("Go To", MenuCommand::GoToMarker, marker.id);
      add_entry(selected_markers_.size() > 1U
                    ? "Delete Selected Markers"
                    : (marker.is_cue() ? "Delete Cue" : "Delete Marker"),
                MenuCommand::DeleteMarker, marker.id);
      open_menu = true;
    } else if (ruler_graph.contains(ctx.pointer_x(), ctx.pointer_y()) ||
               strip_graph.contains(ctx.pointer_x(), ctx.pointer_y())) {
      const core::i64 at = std::max<core::i64>(pointer_frame_i, 0);
      add_entry("Add Marker Here", MenuCommand::AddMarker, {}, at);
      add_entry("Add Hold Cue Here", MenuCommand::AddHoldCue, {}, at);
      add_entry("Add Loop Cue Here", MenuCommand::AddLoopCue, {}, at);
      add_entry("Set In Here", MenuCommand::SetIn, {}, at);
      add_entry("Set Out Here", MenuCommand::SetOut, {}, at);
      add_entry("Zoom to Fit", MenuCommand::ZoomFit);
      add_entry(ruler_frames_ ? "Show Timecode" : "Show Frames",
                MenuCommand::ToggleRulerUnits);
      open_menu = true;
    } else if (right_row.has_value()) {
      const Row &row = rows[*right_row];
      if (row.kind == Row::Kind::LaneHeader) {
        const auto lane = std::find_if(
            lanes.begin(), lanes.end(), [&](const scene::TimelineLane &held) {
              return !held.implicit && held.id == row.node;
            });
        if (lane != lanes.end()) {
          add_entry("Rename Track", MenuCommand::RenameTrack, lane->id);
          add_entry(lane->muted ? "Unmute Track" : "Mute Track",
                    MenuCommand::ToggleTrackMute, lane->id);
          add_entry(lane->locked ? "Unlock Track" : "Lock Track",
                    MenuCommand::ToggleTrackLock, lane->id);
          if (clipboard_span.has_value() && *clipboard_span > 0 &&
              !lane->locked) {
            add_entry("Insert Clipboard at Playhead",
                      MenuCommand::InsertClipboard, lane->id);
            add_entry("Overwrite Clipboard at Playhead",
                      MenuCommand::OverwriteClipboard, lane->id);
          }
          add_entry("New Track", MenuCommand::NewTrack);
          const bool empty = std::none_of(
              elements.begin(), elements.end(), [&](const ElementInfo &held) {
                return held.kind == ElementKind::Media &&
                       held.timeline_lane == lane->id;
              });
          if (empty) {
            add_entry("Delete Track", MenuCommand::DeleteTrack, lane->id);
          } else {
            add_entry("Delete Track and Its Clips",
                      MenuCommand::DeleteTrackClips, lane->id);
            for (const scene::TimelineLane &destination : lanes) {
              if (!destination.implicit && destination.id != lane->id &&
                  destination.scene == lane->scene) {
                add_entry("Move Clips to " + destination.name +
                              ", Delete Track",
                          MenuCommand::MoveClipsDeleteTrack, lane->id, 0,
                          destination.id);
              }
            }
          }
          open_menu = true;
        }
      } else if (row.kind == Row::Kind::Layer && !row.surface) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->locked) {
          const bool clip = element->kind == ElementKind::Media &&
                            element->timing.placed();
          if (clip) {
            if (playhead > element->timing.start &&
                playhead <= element->timing.last_frame()) {
              add_entry("Split at Playhead", MenuCommand::SplitAtPlayhead,
                        element->node);
            }
            const bool in_selection = selected(element->node, selection);
            if (in_selection && selected_clips.size() > 1U &&
                ripple_ready(selected_clips)) {
              add_entry("Ripple Delete Selection",
                        MenuCommand::RippleDeleteSelection);
            } else {
              const std::array<const ElementInfo *, 1U> alone{element};
              if (ripple_ready(alone)) {
                add_entry("Ripple Delete", MenuCommand::RippleDeleteClip,
                          element->node);
              }
            }
            if (clipboard_span.has_value() && *clipboard_span > 0) {
              add_entry("Insert Clipboard at Playhead",
                        MenuCommand::InsertClipboard, element->timeline_lane);
              add_entry("Overwrite Clipboard at Playhead",
                        MenuCommand::OverwriteClipboard,
                        element->timeline_lane);
            }
            add_entry("Set In/Out to Clip", MenuCommand::RangeToClip,
                      element->node, element->timing.start,
                      {}, element->timing.last_frame());
          }
          if (!element->linked &&
              element->kind != ElementKind::Environment) {
            add_entry("Rename      F2", MenuCommand::RenameLayer,
                      element->node);
          }
          if (element->kind != ElementKind::Environment) {
            add_entry("Clear Keyframes", MenuCommand::ClearKeys,
                      element->node);
          }
          open_menu = !menu_entries_.empty();
        }
      }
    }
    if (open_menu) {
      ctx.open_context_menu(menu_id, ctx.pointer_x(), ctx.pointer_y());
    } else {
      menu_entries_.clear();
    }
  }
  if (!menu_entries_.empty()) {
    std::vector<std::string_view> labels;
    labels.reserve(menu_entries_.size());
    for (const MenuEntry &entry : menu_entries_) {
      labels.push_back(entry.label);
    }
    if (const auto chosen = ctx.context_menu(menu_id, labels);
        chosen.has_value() && *chosen < menu_entries_.size()) {
      const MenuEntry entry = menu_entries_[*chosen];
      menu_entries_.clear();
      const auto lane_named = [&](doc::NodeId id) -> std::string {
        for (const scene::TimelineLane &lane : lanes) {
          if (lane.id == id) {
            return lane.name;
          }
        }
        return {};
      };
      switch (static_cast<MenuCommand>(entry.command)) {
      case MenuCommand::SplitAtPlayhead:
        frame.clip_splits.push_back(
            TimelineClipSplit{entry.node, signed_frame(acting_frame)});
        break;
      case MenuCommand::RippleDeleteSelection:
        ripple_delete(selected_clips);
        break;
      case MenuCommand::RippleDeleteClip:
        if (const ElementInfo *const clip = find_element(elements, entry.node)) {
          const std::array<const ElementInfo *, 1U> alone{clip};
          ripple_delete(alone);
        }
        break;
      case MenuCommand::InsertClipboard:
        frame.timeline_edits.emplace_back(TimelineInsertRequest{entry.node});
        break;
      case MenuCommand::OverwriteClipboard:
        frame.timeline_edits.emplace_back(TimelineOverwriteRequest{entry.node});
        break;
      case MenuCommand::RangeToClip:
        frame.range = RangeRequest{
            static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)),
            static_cast<core::u64>(std::max<core::i64>(entry.end, 0)),
            timeline.loop};
        break;
      case MenuCommand::ClearKeys:
        clear_keys(entry.node);
        break;
      case MenuCommand::RenameTrack:
        rename_.begin(ctx, entry.node.value(), lane_named(entry.node));
        break;
      case MenuCommand::RenameLayer:
        if (const ElementInfo *const element =
                find_element(elements, entry.node)) {
          rename_.begin(ctx, entry.node.value(), layer_name(*element));
        }
        break;
      case MenuCommand::ToggleTrackMute:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleMute, entry.node, {}});
        break;
      case MenuCommand::ToggleTrackLock:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleLock, entry.node, {}});
        break;
      case MenuCommand::DeleteTrack:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteEmpty, entry.node, {}});
        break;
      case MenuCommand::DeleteTrackClips:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteClips, entry.node, {}});
        break;
      case MenuCommand::MoveClipsDeleteTrack:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteMoveClips, entry.node, {}, entry.other});
        break;
      case MenuCommand::NewTrack:
        frame.lane_edits.push_back(
            TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
        break;
      case MenuCommand::AddMarker:
        create_marker(entry.frame, scene::CueAction::None);
        break;
      case MenuCommand::AddHoldCue:
        create_marker(entry.frame, scene::CueAction::Hold);
        break;
      case MenuCommand::AddLoopCue:
        create_marker(entry.frame, scene::CueAction::Loop);
        break;
      case MenuCommand::SetIn:
        set_in(static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)));
        break;
      case MenuCommand::SetOut:
        set_out(static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)));
        break;
      case MenuCommand::ZoomFit:
        zoom_to_fit();
        break;
      case MenuCommand::ToggleRulerUnits:
        ruler_frames_ = !ruler_frames_;
        break;
      case MenuCommand::RenameMarker:
        open_marker_name(entry.node);
        break;
      case MenuCommand::MarkerToHold:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::Hold;
        });
        break;
      case MenuCommand::MarkerToLoop:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::Loop;
        });
        break;
      case MenuCommand::MarkerToPlain:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::None;
        });
        break;
      case MenuCommand::MarkerNextColor:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.color = static_cast<core::u8>(
              (marker.color + 1U) % scene::timeline_marker_colors);
        });
        break;
      case MenuCommand::GoToMarker:
        if (const auto index = marker_index(entry.node)) {
          frame.seek_to_frame = std::clamp(
              static_cast<core::u64>(
                  std::max<core::i64>(markers[*index].frame, 0)),
              first, last);
        }
        break;
      case MenuCommand::DeleteMarker:
        if (std::find(selected_markers_.begin(), selected_markers_.end(),
                      entry.node) != selected_markers_.end()) {
          delete_markers(selected_markers_);
        } else {
          const std::array<doc::NodeId, 1U> alone{entry.node};
          delete_markers(alone);
        }
        break;
      }
    } else if (!ctx.menu_open(menu_id)) {
      menu_entries_.clear();
    }
  }

  // ---------------------------------------------------------------------
  // Keyboard. The single-key vocabulary every editor shares, live only while
  // the timeline has the operator's attention and nothing is being typed.
  // A field that owned the keyboard when this frame began has just taken its
  // Enter as a commit; it must never also run the show.
  const bool shortcuts = timeline_focused_ && !frame.editing &&
                         !typing_at_start && !ctx.typing() &&
                         !ctx.pointer_down();
  if (shortcuts) {
    if (ctx.key_pressed(gui::Key::Left)) {
      frame.transport = ctx.shift_held() ? TransportRequest::JumpBack
                                         : TransportRequest::StepBack;
    } else if (ctx.key_pressed(gui::Key::Right)) {
      frame.transport = ctx.shift_held() ? TransportRequest::JumpForward
                                         : TransportRequest::StepForward;
    } else if (ctx.key_pressed(gui::Key::Home)) {
      frame.transport = TransportRequest::GoToStart;
    } else if (ctx.key_pressed(gui::Key::End)) {
      frame.transport = TransportRequest::GoToEnd;
    } else if (ctx.key_pressed(gui::Key::Enter)) {
      frame.transport = TransportRequest::Go;
    }
    if (ctx.key_pressed(gui::Key::Up) || ctx.key_pressed(gui::Key::Down)) {
      // Previous or next edit point: a cut, a range end, or a marker.
      const bool forward = ctx.key_pressed(gui::Key::Down);
      std::optional<core::i64> target;
      for (const core::i64 point : edit_points(doc::NodeId{}, true)) {
        if (forward ? point > playhead : point < playhead) {
          if (!target.has_value() ||
              (forward ? point < *target : point > *target)) {
            target = point;
          }
        }
      }
      if (target.has_value()) {
        frame.seek_to_frame = std::clamp(
            static_cast<core::u64>(std::max<core::i64>(*target, 0)), first,
            last);
      }
    }
    if (ctx.key_pressed(gui::Key::Split)) {
      for (const ElementInfo *clip : selected_clips) {
        if (playhead > clip->timing.start &&
            playhead <= clip->timing.last_frame()) {
          frame.clip_splits.push_back(
              TimelineClipSplit{clip->node, signed_frame(acting_frame)});
        }
      }
    }
    if ((ctx.key_pressed(gui::Key::Delete) ||
         ctx.key_pressed(gui::Key::Backspace)) &&
        !selected_markers_.empty()) {
      delete_markers(selected_markers_);
      frame.consume_delete = true;
    } else if (ctx.key_pressed(gui::Key::Delete) && ctx.shift_held() &&
               selected_keys_.empty() && clip_selection_valid &&
               ripple_ready(selected_clips)) {
      ripple_delete(selected_clips);
      frame.consume_delete = true;
    }
    for (const char key : ctx.shortcut_text()) {
      switch (key) {
      case ' ':
        frame.transport = TransportRequest::TogglePlay;
        break;
      case 'i':
      case 'I':
        set_in(acting_frame);
        break;
      case 'o':
      case 'O':
        set_out(acting_frame);
        break;
      case 'x':
      case 'X':
        // Mark the selected clips: In/Out around them.
        if (!selected_clips.empty()) {
          core::i64 begin = selected_clips.front()->timing.start;
          core::i64 end = selected_clips.front()->timing.last_frame();
          for (const ElementInfo *clip : selected_clips) {
            begin = std::min(begin, clip->timing.start);
            end = std::max(end, clip->timing.last_frame());
          }
          frame.range = RangeRequest{
              static_cast<core::u64>(std::max<core::i64>(begin, 0)),
              static_cast<core::u64>(std::max<core::i64>(end, 0)),
              timeline.loop};
        }
        break;
      case 'm':
      case 'M': {
        // On an existing marker, M names it - the quickest way to label a
        // place the playhead was parked on.
        const auto here = std::find_if(
            markers.begin(), markers.end(),
            [&](const scene::TimelineMarker &marker) {
              return marker.frame == playhead;
            });
        if (here != markers.end()) {
          selected_markers_.assign(1U, here->id);
          open_marker_name(here->id);
        } else {
          create_marker(playhead, scene::CueAction::None);
        }
        break;
      }
      case 'q':
        create_marker(playhead, scene::CueAction::Hold);
        break;
      case 'Q':
        create_marker(playhead, scene::CueAction::Loop);
        break;
      case 's':
      case 'S':
        snapping_ = !snapping_;
        break;
      case 'k':
      case 'K':
        auto_key_ = !auto_key_;
        break;
      case 'v':
      case 'V':
        edit_tool_ = TimelineEditTool::Standard;
        break;
      case 'b':
      case 'B':
        edit_tool_ = TimelineEditTool::RippleTrim;
        break;
      case 'n':
      case 'N':
        edit_tool_ = TimelineEditTool::Roll;
        break;
      case 'y':
      case 'Y':
        edit_tool_ = TimelineEditTool::Slip;
        break;
      case 'u':
      case 'U':
        edit_tool_ = TimelineEditTool::Slide;
        break;
      case 'c':
      case 'C':
        edit_tool_ = TimelineEditTool::Razor;
        break;
      case '\\':
        zoom_to_fit();
        break;
      case '=':
      case '+':
        zoom_about_playhead(1.5);
        break;
      case '-':
      case '_':
        zoom_about_playhead(1.0 / 1.5);
        break;
      case ',':
        if (paste_ready) {
          frame.timeline_edits.emplace_back(TimelineInsertRequest{*target_lane});
        }
        break;
      case '.':
        if (paste_ready) {
          frame.timeline_edits.emplace_back(
              TimelineOverwriteRequest{*target_lane});
        }
        break;
      default:
        break;
      }
    }
  }
  if (timeline_focused_ && !frame.editing && !ctx.typing() &&
      !ctx.pointer_down() && !selected_keys_.empty()) {
    std::optional<TimelineKeyClipboardAction> action;
    if (ctx.key_pressed(gui::Key::Copy)) {
      action = TimelineKeyClipboardAction::Copy;
    } else if (ctx.key_pressed(gui::Key::Paste)) {
      action = TimelineKeyClipboardAction::Paste;
    }
    if (action.has_value()) {
      if (auto edit = key_clipboard_edit(*action); edit.has_value()) {
        frame.key_clipboard = std::move(*edit);
        frame.consume_clipboard = true;
      }
    }
  }
  if (timeline_focused_ && !frame.editing && !selected_keys_.empty() &&
      ctx.key_pressed(gui::Key::Delete)) {
    TimelineKeyBatchEdit batch{};
    batch.action = TimelineKeyBatchAction::Delete;
    batch.keys.reserve(selected_keys_.size());
    for (const SelectedKey &key : selected_keys_) {
      batch.keys.push_back(
          TimelineKeyAddress{key.target, key.component, key.frame});
    }
    std::sort(batch.keys.begin(), batch.keys.end());
    batch.keys.erase(std::unique(batch.keys.begin(), batch.keys.end()),
                     batch.keys.end());
    frame.key_batch = std::move(batch);
    frame.consume_delete = true;
  }
  if (!ctx.pointer_down()) {
    clip_press_consumed_ = false;
  }
  if (frame.seek_to_frame.has_value()) {
    requested_playhead_ = frame.seek_to_frame;
    // A seek the Director clamped or refused must not pin the keys to a
    // frame the playhead never reaches; half a second of frames is ample for
    // the round trip.
    requested_playhead_frames_ = 30U;
  }
  ctx.pop_clip();
  // Tool shapes apply to the editing surface, and stay engaged throughout a
  // captured clip gesture even when the pointer leaves the panel. Selection
  // keeps the more specific move, trim and fade feedback drawn above.
  if (!ctx.pointer_blocked() && !popup_pointer_blocked &&
      !key_popup_pointer_blocked &&
      (clip_drag_ != ClipDrag::None ||
       (graph.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_y() >= rows_bounds.y))) {
    if (edit_tool_ != TimelineEditTool::Standard) {
      ctx.set_cursor(tool_cursor(edit_tool_));
    } else if (clip_drag_ != ClipDrag::None) {
      ctx.set_cursor(clip_drag_ == ClipDrag::Move ? gui::Cursor::Move
                                               : gui::Cursor::ResizeHorizontal);
    }
  }
  return frame;
}

} // namespace mediaserver::editor
