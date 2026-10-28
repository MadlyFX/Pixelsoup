#pragma once

// Internal to the Timeline panel: the constants, row model and small drawing
// and lookup helpers its parts share. Nothing here is part of the editor's
// public interface.

#include <mediaserver/editor/cues_panel.hpp>
#include <mediaserver/editor/timeline_panel.hpp>

#include "timeline_graph_view.hpp"
#include "timeline_hits.hpp"
#include "timeline_layout.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace mediaserver::editor::timeline {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

inline constexpr float icon_button_width = 24.0F;
inline constexpr float edit_tool_scale = 1.35F;

[[nodiscard]] inline gui::Cursor tool_cursor(TimelineEditTool tool) noexcept {
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
inline constexpr float range_handle_reach = 5.0F;
// A marker's tag answers this far left of its line, so a marker is grabbed
// without landing on the line itself.
inline constexpr float marker_hit_reach = 10.0F;
// The window a second click still counts as a double-click.
inline constexpr double double_click_seconds = 0.35;
inline constexpr float key_radius = 5.5F;
// A summary's key ticks are smaller than the keys they stand for.
inline constexpr float summary_key_radius = 3.5F;
// A key answers a press well beyond the diamond it draws - a disc as tall as
// its row - because at arm's length a five-unit diamond is a hard target.
inline constexpr float key_hit_radius = 12.0F;
// A Curves tangent handle's drawn half-size. A handle answers as far out as
// a key does, or half way to its key where they are closer, and wins over
// the key within that.
inline constexpr float handle_radius = 3.5F;
// How close a press must come to a clip's fade knob to take it.
inline constexpr float fade_handle_reach = 10.0F;
// How far down into the bar a fade knob still answers.
inline constexpr float fade_knob_depth = 8.0F;
// How far outside a clip's bar its trim edges still answer, so a clip's end
// can be taken from beside it as well as from on it.
inline constexpr float trim_outer_reach = 6.0F;
inline constexpr float snap_distance = 8.0F;
inline constexpr float marquee_threshold = 4.0F;
inline constexpr float lane_drag_threshold_squared = 16.0F;
inline constexpr float minimum_pixels_per_frame =
    TimelineTimeAxis::minimum_pixels_per_frame;
inline constexpr std::size_t filmstrip_prefetch_rows = 4U;
inline constexpr std::size_t maximum_filmstrip_requests = 32U;
inline constexpr std::size_t waveform_prefetch_rows = 4U;
// A clip bar at least this tall stacks its waveform under its pictures.
inline constexpr float split_clip_height = 40.0F;
// Pictures at least this tall are drawn as whole frames side by side at the
// source's shape, each showing the frame at its left edge; shorter ones
// stretch the atlas across the clip, where a frame would be too small to
// read anyway.
inline constexpr float tiled_filmstrip_height = 28.0F;
// The most frames one clip draws across the lanes.
inline constexpr unsigned maximum_filmstrip_tiles = 96U;
inline constexpr std::size_t maximum_waveform_requests = 32U;

[[nodiscard]] inline std::optional<core::i64>
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

[[nodiscard]] inline core::u64
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

[[nodiscard]] inline bool
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

// The colour a channel's name, swatch, keys and curve are drawn in. The three
// axes of a position, rotation or scale - and a light's red, green and blue -
// read as red, green and blue, by axis: pitch turns about X, yaw about Y and
// roll about Z. Every other channel takes one of nine further hues in
// enumeration order, so channels listed together never share one.
[[nodiscard]] inline gui::UiColor
channel_color(scene::PropertyChannel property) noexcept {
  constexpr gui::UiColor red{0.96F, 0.38F, 0.36F, 1.0F};
  constexpr gui::UiColor green{0.46F, 0.86F, 0.42F, 1.0F};
  constexpr gui::UiColor blue{0.40F, 0.62F, 1.00F, 1.0F};
  switch (property) {
  case scene::PropertyChannel::PositionX:
  case scene::PropertyChannel::ScaleX:
  case scene::PropertyChannel::RotationPitch:
  case scene::PropertyChannel::LightColorR:
    return red;
  case scene::PropertyChannel::PositionY:
  case scene::PropertyChannel::ScaleY:
  case scene::PropertyChannel::RotationYaw:
  case scene::PropertyChannel::LightColorG:
    return green;
  case scene::PropertyChannel::PositionZ:
  case scene::PropertyChannel::ScaleZ:
  case scene::PropertyChannel::RotationRoll:
  case scene::PropertyChannel::LightColorB:
    return blue;
  default:
    break;
  }
  constexpr std::array<gui::UiColor, 9U> hues{{
      {0.98F, 0.78F, 0.32F, 1.0F}, // amber
      {0.92F, 0.46F, 0.88F, 1.0F}, // magenta
      {0.36F, 0.86F, 0.92F, 1.0F}, // cyan
      {0.98F, 0.58F, 0.28F, 1.0F}, // orange
      {0.68F, 0.56F, 1.00F, 1.0F}, // violet
      {0.76F, 0.92F, 0.36F, 1.0F}, // lime
      {1.00F, 0.56F, 0.70F, 1.0F}, // pink
      {0.30F, 0.78F, 0.66F, 1.0F}, // teal
      {0.86F, 0.76F, 0.62F, 1.0F}, // sand
  }};
  return hues[static_cast<std::size_t>(property) % hues.size()];
}

[[nodiscard]] inline std::string_view
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
inline constexpr std::array<std::string_view, 4U> group_names{
    {"Position", "Rotation", "Scale", "Color"}};
inline constexpr std::array<std::array<std::string_view, 3U>, 4U> component_names{{
    {{"X", "Y", "Z"}},
    {{"Yaw", "Pitch", "Roll"}},
    {{"X", "Y", "Z"}},
    {{"R", "G", "B"}},
}};
inline constexpr core::u8 color_group = 3U;

[[nodiscard]] inline scene::PropertyChannel group_channel(core::u8 group,
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

inline constexpr std::array<RateChoice, 8U> common_rates{{
    {{24'000U, 1'001U}, "23.976 FPS"},
    {{24U, 1U}, "24 FPS"},
    {{25U, 1U}, "25 FPS"},
    {{30'000U, 1'001U}, "29.97 FPS"},
    {{30U, 1U}, "30 FPS"},
    {{50U, 1U}, "50 FPS"},
    {{60'000U, 1'001U}, "59.94 FPS"},
    {{60U, 1U}, "60 FPS"},
}};

[[nodiscard]] inline bool same_rate(const EditorTimeline &timeline,
                             ShowRateRequest rate) noexcept {
  return timeline.rate_numerator == rate.numerator &&
         timeline.rate_denominator == rate.denominator;
}

[[nodiscard]] inline std::string rate_label(const EditorTimeline &timeline) {
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

[[nodiscard]] inline gui::UiId mix(gui::UiId base, std::string_view name,
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

[[nodiscard]] inline gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] inline gui::UiColor mix_colors(gui::UiColor a, gui::UiColor b,
                                      float t) noexcept {
  return gui::UiColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// A square tool button carrying a drawn mark. `engaged` is a mode that is on
// - the current tool, snapping, loop - and reads as a lit key rather than as
// a pressed one. The tooltip names the action and its key, which is how an
// icon stays learnable.
[[nodiscard]] inline bool icon_button(gui::UiContext &ctx, gui::UiId id,
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
[[nodiscard]] inline bool text_toggle(gui::UiContext &ctx, gui::UiId id,
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

// The width of a row's icon switch.
inline constexpr float row_switch_width = 18.0F;

// A row switch drawn as its icon alone, beside the eye and padlock: faint
// while off, lit in `color` on a wash of it while on, and given a body only
// under the pointer, so a column of them reads as state rather than as a
// wall of buttons. A disabled switch still shows its state and answers no
// press.
[[nodiscard]] inline bool icon_toggle(gui::UiContext &ctx, gui::UiId id,
                                      const gui::Rect &bounds, gui::Icon icon,
                                      bool engaged, gui::UiColor color,
                                      std::string_view tooltip,
                                      bool enabled = true) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  const gui::Theme &theme = ctx.theme();
  const bool hot = enabled && interaction.hovered;
  if (engaged) {
    ctx.panel(bounds, alpha(color, hot ? 0.34F : 0.22F));
  } else if (hot) {
    ctx.panel(bounds, theme.widget_hover);
  }
  const gui::UiColor mark =
      engaged ? alpha(color, enabled ? 1.0F : 0.55F)
              : alpha(hot ? theme.text : theme.text_muted,
                      enabled ? (hot ? 1.0F : 0.6F) : 0.3F);
  gui::draw_icon(ctx.draw_list(), icon, bounds.x + bounds.width * 0.5F,
                 bounds.y + bounds.height * 0.5F, mark, 0.85F);
  ctx.tooltip(bounds, tooltip);
  return enabled && interaction.clicked;
}

// `text` cut to `width` by its middle rather than its end, so names that
// share a long beginning - take_01_left.mov, take_01_right.mov - still read
// apart. Unchanged when it fits.
[[nodiscard]] inline std::string elide_middle(const gui::UiContext &ctx,
                                              std::string_view text,
                                              float width) {
  if (ctx.font_width(text) <= width) {
    return std::string{text};
  }
  constexpr std::string_view ellipsis{"..."};
  const float room = width - ctx.font_width(ellipsis);
  if (room <= 0.0F) {
    return std::string{};
  }
  const auto continuation = [](char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
  };
  // The end usually carries what tells two takes apart, so it keeps the
  // larger share.
  const float head_room = room * 0.45F;
  std::size_t head = 0U;
  float used = 0.0F;
  while (head < text.size()) {
    std::size_t next = head + 1U;
    while (next < text.size() && continuation(text[next])) {
      ++next;
    }
    const float advance = ctx.font_width(text.substr(head, next - head));
    if (used + advance > head_room) {
      break;
    }
    used += advance;
    head = next;
  }
  std::size_t tail = text.size();
  while (tail > head) {
    std::size_t previous = tail - 1U;
    while (previous > head && continuation(text[previous])) {
      --previous;
    }
    const float advance =
        ctx.font_width(text.substr(previous, tail - previous));
    if (used + advance > room) {
      break;
    }
    used += advance;
    tail = previous;
  }
  return std::string{text.substr(0U, head)} + std::string{ellipsis} +
         std::string{text.substr(tail)};
}

inline void draw_separator(gui::UiContext &ctx, float x, const gui::Rect &toolbar) {
  ctx.panel(gui::Rect{x, toolbar.y + 8.0F, 1.0F, toolbar.height - 16.0F},
            ctx.theme().divider);
}

// What a context-menu entry does. Entries carry their target with them, so
// the menu means what it said when it opened.
enum class MenuCommand : core::u8 {
  SplitAtPlayhead,
  // Every clip on a track under the playhead.
  SplitTrackAtPlayhead,
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
  // The entry's frame holds the scene::KeyEase it applies.
  EaseKeys,
  OpenInAssetViewer,
};

// A cue's tag: the number in a filled chip. Held cues burn in the transport
// colour, the one warm thing on the timeline, because a held show is the
// state an operator must never lose track of.
[[nodiscard]] inline std::string cue_tag(std::size_t number) {
  return "Q" + std::to_string(number);
}

inline void draw_border(gui::UiContext &ctx, const gui::Rect &bounds,
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

inline void draw_diamond(gui::UiContext &ctx, float cx, float cy, float radius,
                  gui::UiColor color) {
  const std::array<gui::UiPoint, 4U> points{{
      {cx, cy - radius},
      {cx + radius, cy},
      {cx, cy + radius},
      {cx - radius, cy},
  }};
  ctx.draw_list().add_convex_poly(points, color);
}

inline void draw_circle(gui::UiContext &ctx, float cx, float cy, float radius,
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

// A key's glyph says how it leaves for the next key, as in every motion
// editor: a diamond runs straight to it, an hourglass curves into it, and a
// square holds until it.
enum class KeyGlyph : core::u8 {
  Diamond,
  Hourglass,
  Square,
};

[[nodiscard]] inline KeyGlyph
key_glyph(scene::KeyInterpolation interpolation) noexcept {
  switch (interpolation) {
  case scene::KeyInterpolation::Hold:
    return KeyGlyph::Square;
  case scene::KeyInterpolation::Cubic:
    return KeyGlyph::Hourglass;
  case scene::KeyInterpolation::Linear:
    break;
  }
  return KeyGlyph::Diamond;
}

// The glyph of key `index` in `channel`, by its outgoing interpolation; a
// diamond where the projection carries none.
[[nodiscard]] inline KeyGlyph key_glyph(const scene::ChannelKeyframes &channel,
                                        std::size_t index) noexcept {
  return index < channel.interpolations.size()
             ? key_glyph(channel.interpolations[index])
             : KeyGlyph::Diamond;
}

// Draws `glyph` centred on (cx, cy), `radius` from its centre to its top.
// Each shape covers about the same area, so no kind of key reads as heavier.
inline void draw_key_glyph(gui::UiContext &ctx, KeyGlyph glyph, float cx,
                           float cy, float radius, gui::UiColor color) {
  switch (glyph) {
  case KeyGlyph::Diamond:
    draw_diamond(ctx, cx, cy, radius, color);
    return;
  case KeyGlyph::Hourglass: {
    // Two triangles meeting at the key's frame.
    const float half = radius * 0.8F;
    const std::array<gui::UiPoint, 3U> top{
        {{cx - half, cy - radius}, {cx + half, cy - radius}, {cx, cy}}};
    const std::array<gui::UiPoint, 3U> bottom{
        {{cx + half, cy + radius}, {cx - half, cy + radius}, {cx, cy}}};
    ctx.draw_list().add_convex_poly(top, color);
    ctx.draw_list().add_convex_poly(bottom, color);
    return;
  }
  case KeyGlyph::Square: {
    const float half = radius * 0.72F;
    const std::array<gui::UiPoint, 4U> square{{{cx - half, cy - half},
                                               {cx + half, cy - half},
                                               {cx + half, cy + half},
                                               {cx - half, cy + half}}};
    ctx.draw_list().add_convex_poly(square, color);
    return;
  }
  }
}

// The key nearest `from` in one direction among `frames` (ascending), kept in
// `best` when it is nearer than what `best` already holds: the latest key
// before `from`, or the earliest after it. A key at `from` is neither.
inline void nearer_key(std::optional<core::i64> &best,
                       std::span<const core::i64> frames, core::i64 from,
                       bool forward) noexcept {
  if (forward) {
    const auto after = std::upper_bound(frames.begin(), frames.end(), from);
    if (after != frames.end() && (!best.has_value() || *after < *best)) {
      best = *after;
    }
    return;
  }
  const auto at = std::lower_bound(frames.begin(), frames.end(), from);
  if (at != frames.begin() && (!best.has_value() || *(at - 1) > *best)) {
    best = *(at - 1);
  }
}

[[nodiscard]] inline bool key_button(gui::UiContext &ctx, gui::UiId id,
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

[[nodiscard]] inline core::i64 signed_frame(core::u64 frame) noexcept {
  constexpr core::u64 maximum =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  return frame > maximum ? std::numeric_limits<core::i64>::max()
                         : static_cast<core::i64>(frame);
}

[[nodiscard]] inline core::u64 rounded_frame(double frame, core::u64 first,
                                      core::u64 last) noexcept {
  const double lo = static_cast<double>(std::min(first, last));
  const double hi = static_cast<double>(std::max(first, last));
  if (!std::isfinite(frame)) {
    return first;
  }
  return static_cast<core::u64>(std::floor(std::clamp(frame, lo, hi) + 0.5));
}

[[nodiscard]] inline core::u64 entered_frame(double value) noexcept {
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

[[nodiscard]] inline std::string layer_name(const ElementInfo &element) {
  return element_display_name(element);
}

// Which elements a layer row twirls open to keyable properties. A camera's
// eye animates by position; every drawable by its transform and look; a light
// by its placement, output and colour. Anything else lists without a twirl,
// so a row never offers keys the show would not play.
[[nodiscard]] inline bool animatable(const ElementInfo &element) noexcept {
  return element.kind != ElementKind::Track;
}

[[nodiscard]] inline std::string lowercase(std::string_view input) {
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

[[nodiscard]] inline bool matches(std::string_view name, std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] inline const scene::ChannelKeyframes *
channel_for(std::span<const scene::ChannelKeyframes> keyframes,
            doc::NodeId node, scene::TransformChannel component) noexcept {
  for (const scene::ChannelKeyframes &entry : keyframes) {
    if (entry.target == node && entry.component == component) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] inline std::optional<double>
sample_channel_at(const scene::ChannelKeyframes *channel,
                  core::u64 frame) noexcept {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (channel == nullptr || frame > maximum) return std::nullopt;
    return scene::sample_keyframes(*channel, static_cast<core::i64>(frame));
}

[[nodiscard]] inline const std::vector<core::i64> *
frames_for(std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
           scene::TransformChannel component) noexcept {
  const scene::ChannelKeyframes *const channel =
      channel_for(keyframes, node, component);
  return channel != nullptr ? &channel->frames : nullptr;
}

[[nodiscard]] inline const ElementInfo *
find_element(std::span<const ElementInfo> elements, doc::NodeId node) noexcept {
  for (const ElementInfo &element : elements) {
    if (element.node == node) {
      return &element;
    }
  }
  return nullptr;
}

[[nodiscard]] inline const output::ProjectionSurface *
find_surface(std::span<const output::ProjectionSurface> surfaces,
             doc::NodeId node) noexcept {
  for (const output::ProjectionSurface &surface : surfaces) {
    if (surface.node == node) {
      return &surface;
    }
  }
  return nullptr;
}

[[nodiscard]] inline double
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

[[nodiscard]] inline core::i64 ruler_step(float pixels_per_frame) noexcept {
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

// No row: a root's parent, or a row with no summary.
inline constexpr std::size_t no_row = std::numeric_limits<std::size_t>::max();

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
  // A track header's: whether it keeps time with ripples on other tracks.
  bool sync_locked{};
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
  // Where the row sits in the stack, measured from the first row's top, and
  // how tall it is. Rows are laid out as a running sum of their heights.
  float top{};
  float height{};
  // The row's place in the tree the list shows: the row it is listed under
  // (no_row for a root), how deep that is, and one past its last listed
  // descendant - so its descendants are rows (index, end) and its ancestors
  // follow `parent`.
  std::size_t parent{no_row};
  std::size_t depth{};
  std::size_t end{};
  // A closed parent's entry in RowList::summaries, or no_row.
  std::size_t summary{no_row};
};

[[nodiscard]] inline doc::NodeId layer_parent(const ElementInfo &element) noexcept {
  return element.linked ? element.linked_parent
                        : element.parent_group.value_or(doc::NodeId{});
}

[[nodiscard]] inline core::u64 group_id(doc::NodeId node, core::u8 group) noexcept {
  return node.value() * 4U + static_cast<core::u64>(group);
}

// The channels a property group, component or property row keys and steps
// through: a group's components, which are consecutive, or its one channel.
struct RowChannels final {
  doc::NodeId target{};
  core::u8 first{};
  core::u8 count{};

  [[nodiscard]] scene::PropertyChannel at(core::u8 offset) const noexcept {
    return static_cast<scene::PropertyChannel>(first + offset);
  }
};

[[nodiscard]] inline std::optional<RowChannels>
row_channels(const Row &row) noexcept {
  switch (row.kind) {
  case Row::Kind::Group:
    return RowChannels{row.node,
                       static_cast<core::u8>(group_channel(row.group, 0U)),
                       row.component_count};
  case Row::Kind::Component:
    return RowChannels{
        row.node,
        static_cast<core::u8>(group_channel(row.group, row.component)), 1U};
  case Row::Kind::Property:
    return RowChannels{row.property_target,
                       static_cast<core::u8>(row.property), 1U};
  case Row::Kind::LaneHeader:
  case Row::Kind::Layer:
  case Row::Kind::PropertiesHeader:
    break;
  }
  return std::nullopt;
}

// Where a channel or group row's controls sit, from the lane edge inwards:
// next key, the key diamond, previous key, the visibility swatch, and the
// value, whose right edge is `value_right`. The name takes what is left.
struct ChannelControls final {
  gui::Rect next;
  gui::Rect key;
  gui::Rect previous;
  gui::Rect swatch;
  float value_right{};
};

inline constexpr float key_step_width = 14.0F;
inline constexpr float row_value_width = 64.0F;
// The most a closed group's "x, y, z" takes; it takes only what it needs.
inline constexpr float group_value_width = 132.0F;

[[nodiscard]] inline ChannelControls channel_controls(float lane_x, float y,
                                                      float height) noexcept {
  const auto centred = [&](float x, float width, float tall) {
    return gui::Rect{x, y + std::max((height - tall) * 0.5F, 0.0F), width,
                     std::min(height, tall)};
  };
  ChannelControls out{};
  out.next = gui::Rect{lane_x - 2.0F - key_step_width, y, key_step_width,
                       height};
  out.key = centred(out.next.x - 18.0F, 18.0F, 18.0F);
  out.previous =
      gui::Rect{out.key.x - key_step_width, y, key_step_width, height};
  out.swatch = centred(out.previous.x - 4.0F - 14.0F, 14.0F, 14.0F);
  out.value_right = out.swatch.x - 4.0F;
  return out;
}

// The value's share of a row's list cell between `left` and `right`: up to
// `width`, and all of it before the name gets any - a narrow list shortens
// the name first.
[[nodiscard]] inline float value_share(float left, float right,
                                       float width) noexcept {
  return std::clamp(right - left, 0.0F, width);
}

// A clip the operator can edit on the timeline: a media placement with a real
// duration. An unplaced clip is the legacy always-on form and keeps the plain
// full-range layer bar, because there is no in or out point to drag.
[[nodiscard]] inline bool editable_clip(const ElementInfo &element) noexcept {
  return !element.locked && element.kind == ElementKind::Media &&
         element.timing.placed();
}

[[nodiscard]] inline bool orderable_layer(const ElementInfo &element) noexcept {
  return element.kind == ElementKind::Object ||
         element.kind == ElementKind::Media ||
         element.kind == ElementKind::Model ||
         element.kind == ElementKind::Group || is_light(element.kind);
}

[[nodiscard]] inline std::optional<core::i64>
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
[[nodiscard]] inline float edge_grab_width(float bar_width) noexcept {
  return std::clamp(bar_width * 0.3F, 3.0F, 10.0F);
}

// The nearest snap candidate to `value`, or nothing when none is within
// `tolerance`. Ties resolve to the lower frame so the result never depends on
// the order candidates were collected in.
[[nodiscard]] inline std::optional<core::i64>
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

[[nodiscard]] inline core::u64 nominal_rate(core::u32 numerator,
                                     core::u32 denominator) noexcept {
  const core::u64 den = std::max<core::u64>(denominator, 1U);
  return std::max<core::u64>(
      (static_cast<core::u64>(numerator) + den / 2U) / den, 1U);
}

// Tick spacing that lands on whole timecode units: single frames when zoomed
// right in, then whole seconds and minutes, so a label always reads as a
// round number of the unit an operator counts in.
[[nodiscard]] inline core::i64 timecode_step(float pixels_per_frame, core::u64 fps,
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

// The ruler's minor tick spacing under labelled ticks `step` frames apart:
// the finest whole division of the step whose ticks stay at least
// `minimum_spacing` apart, or zero when even halves would crowd. Divisions
// are whole frames, so a minor tick always lands on a frame.
[[nodiscard]] inline core::i64 ruler_subdivision(core::i64 step,
                                                 float pixels_per_frame,
                                                 float minimum_spacing) noexcept {
  constexpr std::array<core::i64, 14U> parts{
      {60, 50, 30, 25, 24, 20, 15, 12, 10, 6, 5, 4, 3, 2}};
  for (const core::i64 count : parts) {
    if (step % count != 0) {
      continue;
    }
    const core::i64 minor = step / count;
    if (static_cast<float>(minor) * pixels_per_frame >= minimum_spacing) {
      return minor;
    }
  }
  return 0;
}

// A ruler label in the short form an operator reads at a glance: the hours
// dropped while the whole view is inside the first hour, the way MM:SS:FF
// reads on a cue sheet. Frame counts are left as they are.
[[nodiscard]] inline std::string compact_timecode(std::string text,
                                                  bool first_hour) {
  if (first_hour && text.size() == 11U && text.compare(0U, 3U, "00:") == 0) {
    text.erase(0U, 3U);
  }
  return text;
}

// The identity a row's widgets are keyed by: what the row shows, never where
// it sits in the list. A row therefore keeps its hover, press and capture
// while rows above it open, close or arrive mid-gesture.
[[nodiscard]] inline core::u64 row_identity(const Row &row) noexcept {
  // splitmix64's finaliser, applied once per word of identity.
  const auto mix64 = [](core::u64 value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  };
  const core::u64 shape = (static_cast<core::u64>(row.kind) << 56U) |
                          (static_cast<core::u64>(row.group) << 48U) |
                          (static_cast<core::u64>(row.component) << 40U) |
                          (static_cast<core::u64>(row.property) << 32U) |
                          (row.surface ? 1U : 0U);
  return mix64(mix64(mix64(row.node.value()) ^ shape) ^
               row.property_target.value());
}

// How tall the whole stack of rows is.
[[nodiscard]] inline float rows_extent(std::span<const Row> rows) noexcept {
  return rows.empty() ? 0.0F : rows.back().top + rows.back().height;
}

// The top of row `index`, or the end of the stack for one past the last row.
[[nodiscard]] inline float row_top(std::span<const Row> rows,
                                   std::size_t index) noexcept {
  return index < rows.size() ? rows[index].top : rows_extent(rows);
}

// The row containing `local`, measured from the first row's top.
[[nodiscard]] inline std::optional<std::size_t>
row_at(std::span<const Row> rows, float local) noexcept {
  const auto after = std::upper_bound(
      rows.begin(), rows.end(), local,
      [](float value, const Row &row) noexcept { return value < row.top; });
  if (after == rows.begin()) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(after - rows.begin()) - 1U;
  if (local >= rows[index].top + rows[index].height) {
    return std::nullopt;
  }
  return index;
}

// One frame a closed parent's descendants have keys on: how many keys, and
// their channel when they all share one.
struct SummaryKey final {
  core::i64 frame{};
  core::u32 count{};
  scene::PropertyChannel channel{scene::PropertyChannel::Count};
  bool mixed{};

  bool operator==(const SummaryKey &) const = default;
};

// What a closed parent stands for - a closed track, group, collection or
// hierarchy parent, or a layer whose properties are folded: the frames its
// descendants' clips and keys span, from `first` up to `end` (a clip's end
// is the frame after its last), and every frame they key, once each, as a
// run of RowList::summary_keys.
struct RowSummary final {
  core::i64 first{};
  core::i64 end{};
  std::size_t key_first{};
  std::size_t key_count{};

  bool operator==(const RowSummary &) const = default;
};

// The rows the Timeline lists this frame, top to bottom, the selectable
// layer and track nodes among them in that order, and the summaries of the
// closed parents among them.
struct RowList final {
  std::vector<Row> rows;
  std::vector<doc::NodeId> visible_order;
  std::vector<RowSummary> summaries;
  std::vector<SummaryKey> summary_keys;

  [[nodiscard]] std::span<const SummaryKey>
  keys(const RowSummary &summary) const {
    return {summary_keys.data() + summary.key_first, summary.key_count};
  }
};

struct RowsInput final {
  std::span<const ElementInfo> elements;
  std::span<const scene::TimelineLane> lanes;
  std::span<const output::ProjectionSurface> surfaces;
  std::span<const scene::EffectInstance> effects;
  std::span<const scene::ChannelKeyframes> keyframes;
  core::u64 show_frame{};
  std::string_view search;
  // The selection anchor, whose ancestors open when it changes.
  doc::NodeId anchor{};
};

// Builds the rows from separate object and property twirls. A parent opens to
// its children and a Properties foldout; leaves open directly to P/R/S,
// opacity and effects. Reveals a newly anchored element's ancestors in
// `state`, which is otherwise only read.
[[nodiscard]] RowList build_rows(RowsState &state, const RowsInput &input,
                                 const gui::Theme &theme);

// A track band a clip or layer drag can land in.
// Which part of a row's lane a pointer target is.
enum class RowPart : core::u8 {
  Lane,
  ClipBody,
  TrimIn,
  TrimOut,
  FadeIn,
  FadeOut,
  Key,
  // The list cell's steps to a channel's previous and next key.
  PreviousKey,
  NextKey,
  // A Curves tangent handle; the item is its index among the graph's.
  Handle,
};

// A lane target's tag: the row it belongs to, the part, and for a key its
// index in the channel.
[[nodiscard]] inline core::u64 row_part_tag(std::size_t row, RowPart part,
                                            std::size_t item = 0U) noexcept {
  return (static_cast<core::u64>(row) << 32U) |
         (static_cast<core::u64>(part) << 24U) |
         (static_cast<core::u64>(item) & 0xFFFFFFU);
}
// The row number a Curves-mode graph target carries in its tag.
inline constexpr std::size_t graph_row = 0xFFFFFFFFU;
[[nodiscard]] inline std::size_t tag_row(core::u64 tag) noexcept {
  return static_cast<std::size_t>(tag >> 32U);
}
[[nodiscard]] inline RowPart tag_part(core::u64 tag) noexcept {
  return static_cast<RowPart>((tag >> 24U) & 0xFFU);
}
[[nodiscard]] inline std::size_t tag_item(core::u64 tag) noexcept {
  return static_cast<std::size_t>(tag & 0xFFFFFFU);
}

struct LaneDropTarget final {
  doc::NodeId lane{};
  gui::Rect bounds{};
  std::string_view label;
  bool locked{};
};

} // namespace mediaserver::editor::timeline

namespace mediaserver::editor {

struct TimelinePanel::FrameScratch final {
  // Unlocked keys as drawn this frame, for the marquee and lasso to test.
  struct VisibleKeyPoint final {
    SelectedKey key;
    float x{};
    float y{};
  };
  std::vector<VisibleKeyPoint> visible_keys;
  // What a right press landed on, for the context menu built after the rows.
  std::optional<std::size_t> right_row;
  std::optional<std::size_t> right_marker;
  // The layer drag that ended this frame, so the row it began on does not
  // also take the release as a click.
  doc::NodeId released_layer_drag_node{};
  bool released_layer_drag_moved{};
  // Drag feedback, resolved with the targets and drawn over everything.
  bool layer_drag_active{};
  bool clip_move_feedback{};
  std::string clip_drag_label;
  std::optional<float> layer_drop_landing_y;
  std::string layer_drag_label{"LAYER"};
  bool layer_assignment_feedback{};
  std::optional<float> lane_drop_landing_y;
  std::string lane_drag_label{"TRACK"};
  bool lane_drag_feedback{};
  // An open popup masks the pointer for what lies under it.
  bool popup_pointer_blocked{};
  bool key_popup_pointer_blocked{};
};

// One draw of the panel. Each section of TimelinePanel::draw() reads what the
// sections before it settled here and adds what later ones need; nothing in
// it outlives the frame.
struct TimelinePanel::Pass final {
  Pass(gui::UiContext &context, const gui::Rect &panel_bounds,
       const TimelineDrawInput &draw_input) noexcept
      : ctx(context), bounds(panel_bounds), input(draw_input) {}

  gui::UiContext &ctx;
  const gui::Rect &bounds;
  const TimelineDrawInput &input;
  TimelinePanelFrame frame;
  FrameScratch scratch;

  // Where the pointer gesture is, as of this frame's input.
  float gesture_x{};
  float gesture_y{};
  bool typing_at_start{};

  // The show range, the selection's anchor and its clip-edit context.
  core::u64 first{};
  core::u64 last{};
  core::i64 first_i{};
  core::i64 last_i{};
  doc::NodeId anchor{};
  std::vector<const ElementInfo *> selected_clips;
  bool clip_selection_valid{};
  std::optional<doc::NodeId> target_lane;
  bool paste_ready{};

  timeline::TimelineLayout layout;
  core::u64 fps{};
  // Each marker's number among the cues alone, zero for a plain marker.
  std::vector<std::size_t> cue_numbers;
  bool any_cue{};
  timeline::RowList built;
  // The most any visible name overran its row this frame.
  float name_overflow{};
  float content_height{};
  float maximum_row_scroll{};

  // Settled by the toolbar: its popups' bounds and field ids, and the
  // playhead the next key acts on and the one snapping pulls onto.
  gui::Rect rate_popup{};
  gui::Rect key_tools_popup{};
  gui::UiId scale_numerator_id{};
  gui::UiId scale_denominator_id{};
  gui::UiId scale_user_anchor_id{};
  core::u64 acting_frame{};
  core::i64 playhead{};
  core::i64 shown_playhead{};

  // Settled by the ruler: the search field, the snap reach in frames, and
  // the In/Out range as a handle drag currently shows it.
  gui::UiId search_id{};
  core::i64 snap_tolerance{};
  core::u64 shown_first{};
  core::u64 shown_last{};

  // Drop targets for a clip, layer or track drag, resolved before the rows
  // draw and outlined after them.
  bool clip_drag_active{};
  std::vector<timeline::LaneDropTarget> lane_drop_targets;
  bool lane_drag_active{};
  const timeline::LaneDropTarget *clip_drop_hovered{};
  const timeline::LaneDropTarget *layer_lane_hovered{};

  // The visible rows' lane targets, gathered before any row draws, and the
  // one target that owns this frame's press and the one under the pointer.
  timeline::HitCollector hits;
  std::optional<timeline::HitTarget> press_owner;
  std::optional<timeline::HitTarget> hover_owner;

  // Curves mode: the channels the graph overlays and their keys as drawn.
  std::vector<timeline::GraphChannel> graph_channels;
  std::vector<timeline::GraphKey> graph_keys;
  std::vector<timeline::GraphHandle> graph_handles;
};


// One visible row as draw_rows() hands it to the row's cell renderers: the
// frame it belongs to, its geometry, its selection and fold state, and what
// its lane cell settled for the lane press.
struct TimelinePanel::RowContext final {
  RowContext(Pass &pass, const timeline::Row &shown,
             std::size_t position) noexcept
      : p(pass), row(shown), index(position) {}

  Pass &p;
  const timeline::Row &row;
  std::size_t index{};
  std::size_t track_number{};
  float y{};
  float centre_y{};
  std::optional<ChannelIdentity> row_channel_identity;
  bool is_selected{};
  bool layer_like{};
  bool row_hovered{};
  gui::UiColor background{};
  core::u64 row_group{};
  bool open{};
  float content_x{};
  float name_x{};
  gui::Rect name_bounds{};
  float lock_x{};
  gui::Rect eye_bounds{};
  gui::Rect lock_bounds{};
  float interaction_y{};
  float interaction_height{};
  // A channel or group row's controls beside the lane, its value cell, and
  // what its channels hold at the show frame: evaluated keys, or the base
  // value where there are none. Worked out only for rows on screen.
  timeline::ChannelControls controls{};
  gui::Rect value_bounds{};
  std::array<double, 3U> values{};
  std::array<const scene::ChannelKeyframes *, 3U> keys{};
  const ElementInfo *clip_element{};
  scene::ClipTiming clip_timing{};
  // A track header whose track holds placed clips draws `clip_bar` as the
  // track's own bar, over `track_first` .. `track_end` as they are shown.
  bool track_bar{};
  core::i64 track_first{};
  core::i64 track_end{};
  std::size_t track_clips{};
  gui::Rect clip_bar{};
  float clip_fade_in_x{};
  float clip_fade_out_x{};
  const std::vector<core::i64> *component_frames{};
  std::vector<float> component_x;
  std::vector<float> component_y;
  scene::TransformChannel channel{};
  // The channel's keys as the dope sheet draws them.
  const scene::ChannelKeyframes *component_channel{};
};

} // namespace mediaserver::editor
