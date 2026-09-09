#include <mediaserver/editor/timeline_panel.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

constexpr float toolbar_height = 34.0F;
constexpr float ruler_height = 22.0F;
constexpr float row_height = 22.0F;
constexpr float indent_width = 14.0F;
constexpr float key_radius = 5.0F;
constexpr float key_hit_radius = 7.0F;
constexpr float snap_distance = 8.0F;
constexpr float marquee_threshold = 4.0F;
constexpr float lane_drag_threshold_squared = 16.0F;
constexpr float minimum_pixels_per_frame = 0.02F;
constexpr float maximum_pixels_per_frame = 200.0F;
constexpr std::size_t filmstrip_prefetch_rows = 4U;
constexpr std::size_t maximum_filmstrip_requests = 32U;
constexpr std::size_t waveform_prefetch_rows = 4U;
constexpr std::size_t maximum_waveform_requests = 32U;
// The window a second click on a track header still counts as a rename.
constexpr double lane_rename_click_seconds = 0.35;

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
channel_color(scene::PropertyChannel property) noexcept {
  constexpr std::array<gui::UiColor, 8U> palette{{
      {0.93F, 0.36F, 0.35F, 1.0F},
      {0.35F, 0.76F, 0.96F, 1.0F},
      {0.42F, 0.88F, 0.52F, 1.0F},
      {0.95F, 0.69F, 0.29F, 1.0F},
      {0.72F, 0.48F, 0.96F, 1.0F},
      {0.32F, 0.88F, 0.82F, 1.0F},
      {0.96F, 0.45F, 0.72F, 1.0F},
      {0.76F, 0.82F, 0.34F, 1.0F},
  }};
  return palette[static_cast<std::size_t>(property) % palette.size()];
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
  case scene::PropertyChannel::SkyTimeOfDay:
    return "Solar time";
  case scene::PropertyChannel::Count:
    break;
  }
  return "Unknown Channel";
}

constexpr std::array<std::string_view, 3U> group_names{
    {"Position", "Rotation", "Scale"}};
constexpr std::array<std::array<std::string_view, 3U>, 3U> component_names{{
    {{"X", "Y", "Z"}},
    {{"Yaw", "Pitch", "Roll"}},
    {{"X", "Y", "Z"}},
}};

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

void draw_twirl(gui::UiContext &ctx, float cx, float cy, bool open,
                gui::UiColor color) {
  auto &draw = ctx.draw_list();
  if (open) {
    draw.add_triangle(cx - 4.0F, cy - 2.5F, cx + 4.0F, cy - 2.5F, cx, cy + 3.0F,
                      color);
  } else {
    draw.add_triangle(cx - 2.5F, cy - 4.0F, cx + 3.0F, cy, cx - 2.5F, cy + 4.0F,
                      color);
  }
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

// Six quiet dots make an order-bearing track or media row read as something
// that can be lifted without spending another button or instruction row.
void draw_drag_grip(gui::UiContext &ctx, float cx, float cy,
                    gui::UiColor color) {
  for (int column = -1; column <= 1; column += 2) {
    for (int row = -1; row <= 1; ++row) {
      ctx.panel(gui::Rect{cx + static_cast<float>(column) * 2.0F - 0.75F,
                          cy + static_cast<float>(row) * 3.0F - 0.75F, 1.5F,
                          1.5F},
                color);
    }
  }
}

// One toggle for the whole layer list: an eye or a padlock in a button-shaped
// hit area. `engaged` means muted or locked - the state worth seeing from
// across a stage - so it draws in the warning colour and the row needs no badge
// repeating it in words.
[[nodiscard]] bool toggle_glyph(gui::UiContext &ctx, gui::UiId id,
                                const gui::Rect &bounds, bool engaged,
                                bool lock) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  ctx.panel(bounds, (interaction.held && !interaction.released)
                        ? ctx.theme().widget_active
                        : (interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().widget));
  const gui::UiColor color = engaged
      ? ctx.theme().warning
      : (interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
  const float cx = bounds.x + bounds.width * 0.5F;
  const float cy = bounds.y + bounds.height * 0.5F;
  if (lock) {
    gui::draw_padlock_glyph(ctx.draw_list(), cx, cy, engaged, color);
  } else {
    gui::draw_eye_glyph(ctx.draw_list(), cx, cy, !engaged, color);
  }
  return interaction.clicked;
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
  std::array<char, 64U> text{};
  const std::string_view kind = kind_name(element.kind);
  const int length = std::snprintf(
      text.data(), text.size(), "%.*s %llu", static_cast<int>(kind.size()),
      kind.data(), static_cast<unsigned long long>(element.node.value()));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
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
  if (channel == nullptr || frame > maximum || channel->frames.empty() ||
      channel->values.size() != channel->frames.size()) {
    return std::nullopt;
  }
  const core::i64 wanted = static_cast<core::i64>(frame);
  if (wanted <= channel->frames.front()) {
    return channel->values.front();
  }
  if (wanted >= channel->frames.back()) {
    return channel->values.back();
  }
  const auto after =
      std::upper_bound(channel->frames.begin(), channel->frames.end(), wanted);
  const std::size_t hi =
      static_cast<std::size_t>(std::distance(channel->frames.begin(), after));
  const std::size_t lo = hi - 1U;
  const scene::KeyInterpolation interpolation =
      lo < channel->interpolations.size() ? channel->interpolations[lo]
                                          : scene::KeyInterpolation::Linear;
  if (interpolation == scene::KeyInterpolation::Hold) {
    return channel->values[lo];
  }
  double t = static_cast<double>(wanted - channel->frames[lo]) /
             static_cast<double>(channel->frames[hi] - channel->frames[lo]);
  if (interpolation == scene::KeyInterpolation::Cubic) {
    t = t * t * (3.0 - 2.0 * t);
  }
  return channel->values[lo] + (channel->values[hi] - channel->values[lo]) * t;
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
  core::u8 indent{};
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
};

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
  if (timing.start < 0 || timing.duration <= 0 ||
      timing.duration > std::numeric_limits<core::i64>::max() - timing.start) {
    return std::nullopt;
  }
  return timing.start + timing.duration;
}

// How wide the grab strips at a clip's two ends are, and how wide the fade
// handles are. Both shrink on a very short bar so a two-frame clip does not
// become impossible to move.
[[nodiscard]] float edge_grab_width(float bar_width) noexcept {
  return std::clamp(bar_width * 0.25F, 2.0F, 7.0F);
}

// The nearest snap candidate to `value`, or nothing when none is within
// `tolerance`. Ties resolve to the lower frame so the result never depends on
// the order candidates were collected in.
[[nodiscard]] std::optional<core::i64>
nearest_snap(core::i64 value, std::span<const core::i64> candidates,
             core::i64 tolerance) noexcept {
  std::optional<core::i64> best;
  core::i64 best_distance = tolerance + 1;
  for (const core::i64 candidate : candidates) {
    const core::i64 distance =
        candidate > value ? candidate - value : value - candidate;
    if (distance > tolerance) {
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

} // namespace

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
}

void TimelinePanel::reset_project_state() noexcept {
  pixels_per_frame_ = 0.0F;
  left_frame_ = 0.0;
  time_view_initialized_ = false;
  row_scroll_ = 0.0F;
  expanded_layers_.clear();
  expanded_groups_.clear();
  selected_lane_ = doc::NodeId{};
  lane_destination_ = doc::NodeId{};
  lane_name_node_ = doc::NodeId{};
  lane_name_draft_.clear();
  search_.clear();
  range_fields_ = NumberField{};
  clear_key_context();
  drag_anchor_frame_ = 0;
  rate_menu_open_ = false;
  edit_menu_open_ = false;
  edit_tool_ = TimelineEditTool::Standard;
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
  layer_drag_start_x_ = 0.0F;
  layer_drag_start_y_ = 0.0F;
  layer_drag_moved_ = false;
  clip_drag_ = ClipDrag::None;
  clip_drag_node_ = doc::NodeId{};
  clip_drag_peer_ = doc::NodeId{};
  clip_drag_origin_ = scene::ClipTiming{};
  clip_drag_proposed_ = scene::ClipTiming{};
  clip_drag_anchor_ = 0;
  clip_drag_delta_ = 0;
  clip_drag_start_x_ = 0.0F;
  clip_drag_start_y_ = 0.0F;
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
                    std::span<const TimelineWaveformObservation> waveforms) {
  TimelinePanelFrame frame{};
  frame.filmstrip_retain = TimelineFilmstripRetainIntent{};
  frame.waveform_retain = TimelineWaveformRetainIntent{};
  ctx.panel(bounds, ctx.theme().panel);
  if (ctx.pointer_pressed()) {
    timeline_focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
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
  const bool one_clip = clip_selection_valid && selected_clips.size() == 1U;
  const bool same_selected_lane =
      clip_selection_valid &&
      std::all_of(selected_clips.begin(), selected_clips.end(),
                  [&](const ElementInfo *clip) noexcept {
                    return clip->timeline_lane ==
                           selected_clips.front()->timeline_lane;
                  });

  bool ripple_delete_ready = same_selected_lane;
  core::i64 ripple_begin{};
  core::i64 ripple_end{};
  if (ripple_delete_ready) {
    ripple_begin = selected_clips.front()->timing.start;
    ripple_end = *exclusive_clip_end(selected_clips.front()->timing);
    for (std::size_t index = 1U; index < selected_clips.size(); ++index) {
      const ElementInfo &clip = *selected_clips[index];
      if (clip.timing.start > ripple_end) {
        ripple_delete_ready = false;
        break;
      }
      ripple_end = std::max(ripple_end, *exclusive_clip_end(clip.timing));
    }
    for (const ElementInfo &clip : elements) {
      if (!ripple_delete_ready || clip.kind != ElementKind::Media ||
          !clip.timing.placed() ||
          clip.timeline_lane != selected_clips.front()->timeline_lane ||
          selected(clip.node, selection)) {
        continue;
      }
      const std::optional<core::i64> clip_end = exclusive_clip_end(clip.timing);
      if (!clip_end.has_value() ||
          (clip.timing.start < ripple_end && *clip_end > ripple_begin)) {
        ripple_delete_ready = false;
      }
    }
  }

  const bool roll_ready = same_selected_lane && selected_clips.size() == 2U &&
                          *exclusive_clip_end(selected_clips.front()->timing) ==
                              selected_clips.back()->timing.start;

  bool slide_ready = one_clip;
  if (slide_ready) {
    const ElementInfo &selected_clip = *selected_clips.front();
    const core::i64 selected_end = *exclusive_clip_end(selected_clip.timing);
    std::size_t predecessors{};
    std::size_t successors{};
    for (const ElementInfo &candidate : elements) {
      if (candidate.node == selected_clip.node ||
          candidate.kind != ElementKind::Media || !candidate.timing.placed() ||
          candidate.locked ||
          candidate.timeline_lane != selected_clip.timeline_lane) {
        continue;
      }
      const std::optional<core::i64> candidate_end =
          exclusive_clip_end(candidate.timing);
      if (!candidate_end.has_value()) {
        slide_ready = false;
        continue;
      }
      predecessors += *candidate_end == selected_clip.timing.start ? 1U : 0U;
      successors += candidate.timing.start == selected_end ? 1U : 0U;
    }
    slide_ready = slide_ready && predecessors == 1U && successors == 1U;
  }

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

  const float preferred_list = std::clamp(bounds.width * 0.26F, 180.0F, 260.0F);
  const float list_width =
      std::min(preferred_list, std::max(bounds.width - 80.0F, 0.0F));
  const gui::Rect toolbar{bounds.x, bounds.y, bounds.width, toolbar_height};
  const gui::Rect ruler{bounds.x, bounds.y + toolbar_height, bounds.width,
                        ruler_height};
  const gui::Rect rows_bounds{
      bounds.x, ruler.y + ruler.height, bounds.width,
      std::max(bounds.height - toolbar_height - ruler_height, 0.0F)};
  const gui::Rect graph{bounds.x + list_width, ruler.y,
                        std::max(bounds.width - list_width, 0.0F),
                        ruler.height + rows_bounds.height};

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

  // Build the flat visible row list from the twirl state. A layer row is
  // always present; transformable layers open to P/R/S and then components.
  std::vector<Row> rows;
  rows.reserve(elements.size() * 7U + surfaces.size() * 4U);
  const auto append_element = [&](const ElementInfo &element,
                                  core::u8 base_indent = 0U) {
    const std::string name = layer_name(element);
    if (!matches(name, search_) && !matches(kind_name(element.kind), search_)) {
      return;
    }
    const bool transformable = !is_light(element.kind);
    rows.push_back(Row{Row::Kind::Layer, element.node, 0U, 0U, name,
                       kind_color(element.kind), element.kind, base_indent,
                       transformable});
    rows.back().locked = element.locked;
    rows.back().element_visible = element.visible;
    if (!transformable || expanded_layers_.count(element.node.value()) == 0U) {
      return;
    }
    for (core::u8 group = 0U; group < 3U; ++group) {
      rows.push_back(
          Row{Row::Kind::Group, element.node, group, 0U,
              std::string{group_names[static_cast<std::size_t>(group)]},
              ctx.theme().text_muted, element.kind,
              static_cast<core::u8>(base_indent + 1U), true});
      if (expanded_groups_.count(group_id(element.node, group)) == 0U) {
        continue;
      }
      for (core::u8 component = 0U; component < 3U; ++component) {
        rows.push_back(Row{Row::Kind::Component, element.node, group, component,
                           std::string{component_names[static_cast<std::size_t>(
                               group)][static_cast<std::size_t>(component)]},
                           ctx.theme().text_muted, element.kind,
                           static_cast<core::u8>(base_indent + 2U), false});
      }
    }
    rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U, "Opacity",
                       ctx.theme().text_muted, element.kind,
                       static_cast<core::u8>(base_indent + 1U), false,
                       scene::PropertyChannel::Opacity, element.node,
                       element.style.opacity});
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
                         static_cast<core::u8>(base_indent + 1U), false,
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
    std::string label{"TRACK · "};
    label += lane.name;
    Row header{Row::Kind::LaneHeader,
               lane.id,
               0U,
               0U,
               std::move(label),
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
  for (const ElementInfo *element : ordinary_layers) {
    append_element(*element);
  }
  for (const ElementInfo *element : unordered_layers) {
    append_element(*element);
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
    if (row.kind == Row::Kind::Layer && !row.surface && !row.locked &&
        std::find(visible_order.begin(), visible_order.end(), row.node) ==
            visible_order.end()) {
      visible_order.push_back(row.node);
    }
  }

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
  left_frame_ = std::clamp(left_frame_, static_cast<double>(first),
                           static_cast<double>(last));
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

  // Toolbar: transport first, then the current show frame and view tools.
  // Key authoring lives beside each property in the layer tree, so the old
  // duplicate K P / K R / K S buttons do not compete with those diamonds.
  // Exact in/out and loop controls stay right-aligned on a wide timeline.
  ctx.panel(toolbar, ctx.theme().header);
  ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                      toolbar.width, 1.0F},
            ctx.theme().divider);
  float control_x = toolbar.x + 6.0F;
  const float control_y = toolbar.y + 6.0F;
  constexpr float button_height = 22.0F;
  constexpr float compact_width = 25.0F;
  constexpr float control_gap = 3.0F;
  const auto transport_button = [&](std::string_view label, float width,
                                    TransportRequest request) {
    const gui::Rect button{control_x, control_y, width, button_height};
    if (ctx.button(mix(id_, label), button, label)) {
      frame.transport = request;
    }
    control_x += width + control_gap;
  };
  transport_button("|<", compact_width, TransportRequest::GoToStart);
  transport_button("<<", compact_width, TransportRequest::JumpBack);
  transport_button("<", compact_width, TransportRequest::StepBack);
  transport_button(timeline.playing ? "||" : "PLAY",
                   timeline.playing ? compact_width : 38.0F,
                   TransportRequest::TogglePlay);
  transport_button(">", compact_width, TransportRequest::StepForward);
  transport_button(">>", compact_width, TransportRequest::JumpForward);

  std::array<char, 64U> position_text{};
  const core::u64 display_rate = std::max<core::u64>(
      (static_cast<core::u64>(timeline.rate_numerator) +
       static_cast<core::u64>(timeline.rate_denominator) / 2U) /
          std::max<core::u64>(timeline.rate_denominator, 1U),
      1U);
  const core::u64 total_seconds = timeline.show_frame / display_rate;
  const core::u64 display_frame = timeline.show_frame % display_rate;
  const core::u64 seconds = total_seconds % 60U;
  const core::u64 minutes = (total_seconds / 60U) % 60U;
  const core::u64 hours = total_seconds / 3'600U;
  const int position_length = std::snprintf(
      position_text.data(), position_text.size(), "%02llu:%02llu:%02llu:%02llu",
      static_cast<unsigned long long>(hours),
      static_cast<unsigned long long>(minutes),
      static_cast<unsigned long long>(seconds),
      static_cast<unsigned long long>(display_frame));
  const gui::Rect timecode{control_x + 7.0F, control_y, 150.0F, button_height};
  ctx.panel(timecode, ctx.theme().track);
  ctx.panel(gui::Rect{timecode.x, timecode.y, 2.0F, timecode.height},
            ctx.theme().accent);
  ctx.label(
      gui::Rect{timecode.x + 10.0F, timecode.y, timecode.width - 10.0F,
                timecode.height},
      std::string_view{position_text.data(),
                       static_cast<std::size_t>(std::max(position_length, 0))},
      ctx.theme().keyframe);
  control_x += 164.0F;

  constexpr float rate_width = 82.0F;
  constexpr float rate_gap = 6.0F;
  const float range_x = toolbar.x + toolbar.width - 300.0F;
  const bool show_range =
      bounds.width >= 700.0F && control_x + rate_width + rate_gap <= range_x;
  const float rate_x = show_range
                           ? range_x - rate_width - rate_gap
                           : toolbar.x + toolbar.width - rate_width - 6.0F;
  const bool show_rate = rate_x >= control_x + control_gap;
  const float timeline_tools_end =
      show_rate ? rate_x - control_gap : toolbar.x + toolbar.width;
  if (control_x + 50.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "snap"),
                   gui::Rect{control_x, control_y, 48.0F, button_height},
                   snapping_ ? "SNAP" : "snap")) {
      snapping_ = !snapping_;
    }
    control_x += 51.0F;
  }
  if (control_x + 40.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "fit"),
                   gui::Rect{control_x, control_y, 38.0F, button_height},
                   "FIT")) {
      const double span =
          std::max(static_cast<double>(last) - static_cast<double>(first), 1.0);
      pixels_per_frame_ = std::clamp(
          static_cast<float>(static_cast<double>(graph.width) / span),
          minimum_pixels_per_frame, maximum_pixels_per_frame);
      left_frame_ = static_cast<double>(first);
    }
    control_x += 41.0F;
  }
  if (control_x + 58.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "graph mode"),
                   gui::Rect{control_x, control_y, 56.0F, button_height},
                   graph_mode_ ? "CURVES" : "KEYS")) {
      graph_mode_ = !graph_mode_;
    }
    control_x += 59.0F;
  }
  const auto selected_lane = std::find_if(
      lanes.begin(), lanes.end(), [&](const scene::TimelineLane &lane) {
        return !lane.implicit && lane.id == selected_lane_;
      });
  if (selected_lane_ != doc::NodeId{} && selected_lane == lanes.end()) {
    selected_lane_ = doc::NodeId{};
    lane_destination_ = doc::NodeId{};
    lane_name_node_ = doc::NodeId{};
    lane_name_draft_.clear();
  }
  if (control_x + 58.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "create lane"),
                   gui::Rect{control_x, control_y, 55.0F, button_height},
                   "+ TRACK")) {
      frame.lane_edits.push_back(
          TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
    }
    control_x += 58.0F;
  }
  if (selected_lane != lanes.end()) {
    std::vector<const scene::TimelineLane *> destinations;
    for (const scene::TimelineLane &candidate : lanes) {
      if (!candidate.implicit && candidate.scene == selected_lane->scene &&
          candidate.id != selected_lane->id) {
        destinations.push_back(&candidate);
      }
    }
    const auto selected_destination =
        std::find_if(destinations.begin(), destinations.end(),
                     [&](const scene::TimelineLane *candidate) {
                       return candidate->id == lane_destination_;
                     });
    if (selected_destination == destinations.end()) {
      lane_destination_ =
          destinations.empty() ? doc::NodeId{} : destinations.front()->id;
    }
    const auto lane_button = [&](std::string_view id, std::string_view label,
                                 TimelineLaneAction action) {
      if (control_x + 27.0F >= timeline_tools_end) {
        return;
      }
      if (ctx.button(mix(id_, id, selected_lane->id.value()),
                     gui::Rect{control_x, control_y, 24.0F, button_height},
                     label)) {
        frame.lane_edits.push_back(
            TimelineLaneEdit{action, selected_lane->id, {}});
      }
      control_x += 27.0F;
    };
    // Mute and lock live on the track's own row, where the state they report
    // is next to the clips it applies to.
    const bool empty = std::none_of(
        elements.begin(), elements.end(), [&](const ElementInfo &element) {
          return element.kind == ElementKind::Media &&
                 element.timeline_lane == selected_lane->id;
        });
    if (empty) {
      lane_button("delete lane", "X", TimelineLaneAction::DeleteEmpty);
    } else {
      if (control_x + 99.0F < timeline_tools_end) {
        if (ctx.button(mix(id_, "delete lane clips", selected_lane->id.value()),
                       gui::Rect{control_x, control_y, 96.0F, button_height},
                       "DELETE+CLIPS")) {
          frame.lane_edits.push_back(TimelineLaneEdit{
              TimelineLaneAction::DeleteClips, selected_lane->id, {}, {}});
        }
        control_x += 99.0F;
      }
      if (!destinations.empty()) {
        bool destination_visible = false;
        const auto destination =
            std::find_if(destinations.begin(), destinations.end(),
                         [&](const scene::TimelineLane *candidate) {
                           return candidate->id == lane_destination_;
                         });
        if (destination != destinations.end() &&
            control_x + 100.0F < timeline_tools_end) {
          std::string label{"TO "};
          label += (*destination)->name;
          if (ctx.button(
                  mix(id_, "lane destination", selected_lane->id.value()),
                  gui::Rect{control_x, control_y, 97.0F, button_height},
                  label)) {
            const auto next = std::next(destination) == destinations.end()
                                  ? destinations.begin()
                                  : std::next(destination);
            lane_destination_ = (*next)->id;
          }
          control_x += 100.0F;
          destination_visible = true;
        }
        if (destination_visible && control_x + 80.0F < timeline_tools_end) {
          if (ctx.button(
                  mix(id_, "move then delete lane", selected_lane->id.value()),
                  gui::Rect{control_x, control_y, 77.0F, button_height},
                  "MOVE+DELETE")) {
            frame.lane_edits.push_back(
                TimelineLaneEdit{TimelineLaneAction::DeleteMoveClips,
                                 selected_lane->id,
                                 {},
                                 lane_destination_});
          }
          control_x += 80.0F;
        }
      }
    }
  }
  // Splice. A cut is only offered when the playhead is actually inside a
  // selected clip and has a frame on each side of it, because a split that
  // produced a zero-length half would be a delete wearing the wrong name.
  const core::i64 playhead = signed_frame(timeline.show_frame);
  std::vector<doc::NodeId> splittable;
  for (const ElementInfo &element : elements) {
    if (!editable_clip(element) || !selected(element.node, selection)) {
      continue;
    }
    if (playhead > element.timing.start &&
        playhead <= element.timing.last_frame()) {
      splittable.push_back(element.node);
    }
  }
  if (!splittable.empty() && control_x + 56.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "split clip"),
                   gui::Rect{control_x, control_y, 54.0F, button_height},
                   "SPLIT")) {
      for (const doc::NodeId clip : splittable) {
        frame.clip_splits.push_back(
            TimelineClipSplit{clip, timeline.show_frame});
      }
    }
    control_x += 57.0F;
  }
  if (anchor_info != nullptr && !anchor_info->locked &&
      !is_light(anchor_info->kind) && control_x + 79.0F < timeline_tools_end) {
    if (ctx.button(mix(id_, "clear keys"),
                   gui::Rect{control_x, control_y, 76.0F, button_height},
                   "CLEAR KEYS")) {
      // Transform and opacity belong to the selected layer. Effect
      // parameters belong to their first-class effect nodes, so clear
      // those addresses explicitly instead of inventing brightness and
      // blur channels on the layer itself.
      for (core::u8 component = 0U;
           component <= static_cast<core::u8>(scene::PropertyChannel::Opacity);
           ++component) {
        frame.keys.push_back(TimelineKeyEdit{
            anchor, static_cast<scene::TransformChannel>(component), 0U, 0.0,
            true});
      }
      for (const scene::EffectInstance &effect : effects) {
        if (effect.target != anchor) {
          continue;
        }
        scene::PropertyChannel property{};
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
          continue;
        }
        frame.keys.push_back(
            TimelineKeyEdit{effect.source, property, 0U, 0.0, true});
      }
    }
  }

  gui::Rect rate_button{};
  gui::Rect rate_popup{};
  bool popup_pointer_blocked = false;
  if (show_rate) {
    rate_button = gui::Rect{rate_x, control_y, rate_width, button_height};
    if (ctx.button(mix(id_, "show rate"), rate_button, rate_label(timeline))) {
      rate_menu_open_ = !rate_menu_open_;
      edit_menu_open_ = false;
    }
    constexpr float popup_width = 104.0F;
    constexpr float choice_height = 21.0F;
    rate_popup = gui::Rect{rate_button.x + rate_button.width - popup_width,
                           toolbar.y + toolbar.height - 2.0F, popup_width,
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

  if (show_range) {
    ctx.label(gui::Rect{range_x, control_y, 16.0F, button_height}, "IN",
              ctx.theme().text_muted);
    double changed_first = 0.0;
    const bool first_changed = range_fields_.draw(
        ctx, mix(id_, "range first"),
        gui::Rect{range_x + 18.0F, control_y, 50.0F, button_height}, "",
        static_cast<double>(first), 1.0, changed_first, 0);
    const bool set_first = ctx.button(
        mix(id_, "set range first"),
        gui::Rect{range_x + 70.0F, control_y, 44.0F, button_height}, "SET IN");
    ctx.label(gui::Rect{range_x + 118.0F, control_y, 21.0F, button_height},
              "OUT", ctx.theme().text_muted);
    double changed_last = 0.0;
    const bool last_changed = range_fields_.draw(
        ctx, mix(id_, "range last"),
        gui::Rect{range_x + 142.0F, control_y, 50.0F, button_height}, "",
        static_cast<double>(last), 1.0, changed_last, 0);
    const bool set_last =
        ctx.button(mix(id_, "set range last"),
                   gui::Rect{range_x + 194.0F, control_y, 50.0F, button_height},
                   "SET OUT");
    bool loop = timeline.loop;
    const bool loop_changed = ctx.checkbox(
        mix(id_, "loop"),
        gui::Rect{range_x + 248.0F, control_y, 52.0F, button_height}, loop,
        "LOOP");
    if (set_first) {
      // A stale playhead may sit beyond OUT. Keep the clicked endpoint
      // exact and move OUT only as far as needed to retain an inclusive
      // range.
      frame.range =
          RangeRequest{timeline.show_frame, std::max(last, timeline.show_frame),
                       timeline.loop};
    } else if (set_last) {
      // The symmetric stale-playhead case keeps OUT exact and moves IN
      // only when the playhead precedes the current range.
      frame.range = RangeRequest{std::min(first, timeline.show_frame),
                                 timeline.show_frame, timeline.loop};
    } else if (first_changed || last_changed || loop_changed) {
      core::u64 range_first = first;
      core::u64 range_last = last;
      if (first_changed) {
        range_first = entered_frame(changed_first);
      }
      if (last_changed) {
        range_last = entered_frame(changed_last);
      }
      if (range_last < range_first) {
        range_last = range_first;
      }
      frame.range = RangeRequest{range_first, range_last, loop};
    }
  }
  const gui::UiId search_id = mix(id_, "search");

  struct EditChoice final {
    TimelineEditTool tool;
    std::string_view label;
  };
  constexpr std::array<EditChoice, 8U> edit_choices{{
      {TimelineEditTool::Standard, "STANDARD MOVE / TRIM"},
      {TimelineEditTool::RippleTrim, "RIPPLE TRIM"},
      {TimelineEditTool::RippleDelete, "RIPPLE DELETE"},
      {TimelineEditTool::Roll, "ROLL"},
      {TimelineEditTool::Slip, "SLIP"},
      {TimelineEditTool::Slide, "SLIDE"},
      {TimelineEditTool::Insert, "INSERT CLIPBOARD"},
      {TimelineEditTool::Overwrite, "OVERWRITE CLIPBOARD"},
  }};
  const auto edit_choice_enabled = [&](TimelineEditTool tool) noexcept {
    switch (tool) {
    case TimelineEditTool::Standard:
      return true;
    case TimelineEditTool::RippleTrim:
      return one_clip;
    case TimelineEditTool::RippleDelete:
      return ripple_delete_ready;
    case TimelineEditTool::Roll:
      return roll_ready;
    case TimelineEditTool::Slip:
      return one_clip;
    case TimelineEditTool::Slide:
      return slide_ready;
    case TimelineEditTool::Insert:
    case TimelineEditTool::Overwrite:
      return paste_ready;
    }
    return false;
  };
  const auto edit_tool_label = [](TimelineEditTool tool) noexcept {
    switch (tool) {
    case TimelineEditTool::RippleTrim:
      return std::string_view{"RIP"};
    case TimelineEditTool::RippleDelete:
      return std::string_view{"RDEL"};
    case TimelineEditTool::Roll:
      return std::string_view{"ROLL"};
    case TimelineEditTool::Slip:
      return std::string_view{"SLIP"};
    case TimelineEditTool::Slide:
      return std::string_view{"SLIDE"};
    case TimelineEditTool::Insert:
      return std::string_view{"INS"};
    case TimelineEditTool::Overwrite:
      return std::string_view{"OVR"};
    case TimelineEditTool::Standard:
      break;
    }
    return std::string_view{"EDIT"};
  };

  // The left side of the ruler doubles as the layer filter, leaving row and
  // graph coordinates unchanged from the established timeline layout. Its
  // right edge holds a semantic edit picker and an explicit Apply action for
  // delete/clipboard operations; drag tools commit when their drag ends.
  ctx.panel(ruler, ctx.theme().header);
  ctx.panel(gui::Rect{ruler.x, ruler.y, list_width, ruler.height},
            ctx.theme().row_alt);
  const gui::Rect search_bounds{ruler.x + 5.0F, ruler.y + 2.0F,
                                std::max(list_width - 91.0F, 0.0F),
                                std::max(ruler.height - 4.0F, 0.0F)};
  static_cast<void>(ctx.text_field(search_id, search_bounds, search_));
  if (search_.empty() && ctx.focus() != search_id) {
    ctx.label(gui::Rect{search_bounds.x + 19.0F, search_bounds.y,
                        std::max(search_bounds.width - 23.0F, 0.0F),
                        search_bounds.height},
              "Search Timeline...", ctx.theme().text_muted);
  }
  const float search_cy = search_bounds.y + search_bounds.height * 0.46F;
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 3.0F,
              ctx.theme().text_muted);
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 1.7F, ctx.theme().widget);
  ctx.draw_list().add_line(search_bounds.x + 11.0F, search_cy + 2.0F,
                           search_bounds.x + 14.0F, search_cy + 5.0F, 1.0F,
                           ctx.theme().text_muted);

  const gui::Rect edit_button{ruler.x + list_width - 81.0F, ruler.y + 2.0F,
                              43.0F, std::max(ruler.height - 4.0F, 0.0F)};
  if (ctx.button(mix(id_, "advanced edit menu"), edit_button,
                 edit_tool_label(edit_tool_))) {
    edit_menu_open_ = !edit_menu_open_;
    rate_menu_open_ = false;
  }
  const bool action_tool = edit_tool_ == TimelineEditTool::RippleDelete ||
                           edit_tool_ == TimelineEditTool::Insert ||
                           edit_tool_ == TimelineEditTool::Overwrite;
  const bool action_ready = action_tool && edit_choice_enabled(edit_tool_);
  const gui::Rect apply_button{ruler.x + list_width - 35.0F, ruler.y + 2.0F,
                               30.0F, std::max(ruler.height - 4.0F, 0.0F)};
  if (action_ready) {
    if (ctx.button(mix(id_, "apply advanced edit"), apply_button, "GO")) {
      if (edit_tool_ == TimelineEditTool::RippleDelete) {
        TimelineRippleDeleteRequest request{};
        request.clips.reserve(selected_clips.size());
        for (const ElementInfo *clip : selected_clips) {
          request.clips.push_back(clip->node);
        }
        std::sort(request.clips.begin(), request.clips.end());
        frame.timeline_edits.emplace_back(std::move(request));
      } else if (edit_tool_ == TimelineEditTool::Insert) {
        frame.timeline_edits.emplace_back(TimelineInsertRequest{*target_lane});
      } else if (edit_tool_ == TimelineEditTool::Overwrite) {
        frame.timeline_edits.emplace_back(
            TimelineOverwriteRequest{*target_lane});
      }
    }
  } else {
    ctx.panel(apply_button, ctx.theme().track);
    ctx.label(apply_button, action_tool ? "--" : "DRAG",
              ctx.theme().text_muted);
  }

  constexpr float edit_choice_height = 21.0F;
  constexpr float edit_popup_width = 176.0F;
  const gui::Rect edit_popup{
      ruler.x + list_width - edit_popup_width, ruler.y + ruler.height,
      edit_popup_width,
      8.0F + edit_choice_height * static_cast<float>(edit_choices.size())};
  if (edit_menu_open_ && ctx.pointer_pressed() &&
      !edit_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !edit_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    edit_menu_open_ = false;
  }
  if (edit_menu_open_) {
    for (std::size_t index = 0U; index < edit_choices.size(); ++index) {
      const gui::Rect choice{edit_popup.x + 4.0F,
                             edit_popup.y + 4.0F +
                                 static_cast<float>(index) * edit_choice_height,
                             edit_popup.width - 8.0F, edit_choice_height};
      if (!edit_choice_enabled(edit_choices[index].tool)) {
        continue;
      }
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "advanced edit choice", index), choice);
      if (interaction.clicked) {
        edit_tool_ = edit_choices[index].tool;
        edit_menu_open_ = false;
      }
    }
  }
  bool edit_popup_pointer_blocked = false;
  if (edit_menu_open_ &&
      edit_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    ctx.push_pointer_block();
    edit_popup_pointer_blocked = true;
  }
  const bool show_key_tools = graph.width >= 260.0F;
  const gui::Rect key_tools_button{
      graph.x + std::max(graph.width - 61.0F, 0.0F), ruler.y + 2.0F,
      std::min(56.0F, graph.width), std::max(ruler.height - 4.0F, 0.0F)};
  const float key_popup_width = std::min(338.0F, graph.width);
  const gui::Rect key_tools_popup{graph.x + graph.width - key_popup_width,
                                  ruler.y + ruler.height, key_popup_width,
                                  224.0F};
  const gui::UiId scale_numerator_id = mix(id_, "scale numerator");
  const gui::UiId scale_denominator_id = mix(id_, "scale denominator");
  const gui::UiId scale_user_anchor_id = mix(id_, "scale user anchor");
  if (show_key_tools && ctx.button(mix(id_, "key tools"), key_tools_button,
                                   key_tools_open_ ? "KEY*" : "KEYS")) {
    key_tools_open_ = !key_tools_open_;
    rate_menu_open_ = false;
    edit_menu_open_ = false;
    if (!key_tools_open_) {
      scale_open_ = false;
    }
  }
  if (!show_key_tools) {
    key_tools_open_ = false;
    scale_open_ = false;
  }
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
  ctx.panel(
      gui::Rect{graph.x, ruler.y, 1.0F, ruler.height + rows_bounds.height},
      ctx.theme().divider);

  ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  const core::i64 step = ruler_step(pixels_per_frame_);
  const core::i64 visible_begin =
      static_cast<core::i64>(std::floor(left_frame_));
  core::i64 tick = (visible_begin / step) * step;
  if (tick > visible_begin) {
    tick -= step;
  }
  for (std::size_t count = 0U; count < 512U; ++count, tick += step) {
    const float x = frame_to_x(static_cast<double>(tick));
    if (x > graph.x + graph.width) {
      break;
    }
    if (x < graph.x || tick < 0) {
      continue;
    }
    ctx.draw_list().add_rect(x, ruler.y + ruler.height - 6.0F, 1.0F, 6.0F,
                             ctx.theme().text_muted);
    std::array<char, 24U> label{};
    const int length = std::snprintf(label.data(), label.size(), "%lld",
                                     static_cast<long long>(tick));
    ctx.label(gui::Rect{x + 4.0F, ruler.y, 72.0F, ruler.height - 4.0F},
              std::string_view{label.data(),
                               static_cast<std::size_t>(std::max(length, 0))},
              ctx.theme().text_muted);
  }
  ctx.pop_clip();

  // Scrubbing the ruler pauses/seeks through the shell. It follows the
  // pointer while held, including the release frame.
  const gui::Interaction ruler_it =
      ctx.grab(mix(id_, "ruler"),
               gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  if (ruler_it.held) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }

  // A selected key drag follows time even when the pointer wanders into
  // another lane. Every selected diamond shares one delta, and any one of
  // them may magnetise to the playhead, range edges, or an unselected key.
  snap_guide_.reset();
  if (dragging_key_ && ctx.pointer_down()) {
    const core::i64 pointer_frame =
        signed_frame(rounded_frame(x_to_frame(ctx.pointer_x()), first, last));
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
  }

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
    return targets;
  };

  // A vertical clip drop names a track band, or the ordinary top-level area
  // outside every band. Locked tracks are not destinations. Keeping this
  // resolver independent of the graph lets the same gesture begin on a media
  // row when its bar is short or off screen.
  const auto clip_lane_under_pointer = [&]() -> std::optional<doc::NodeId> {
    if (ctx.pointer_y() < rows_bounds.y ||
        ctx.pointer_y() >= rows_bounds.y + rows_bounds.height) {
      return std::nullopt;
    }
    const float local = ctx.pointer_y() - rows_bounds.y + row_scroll_;
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

  if (clip_drag_ != ClipDrag::None && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - clip_drag_start_x_;
    const float dy = ctx.pointer_y() - clip_drag_start_y_;
    clip_drag_moved_ = clip_drag_moved_ ||
                       dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  const bool clip_drag_active =
      clip_drag_ != ClipDrag::None && clip_drag_moved_;
  if (clip_drag_ == ClipDrag::Move) {
    clip_drop_lane_ = clip_drag_active ? clip_lane_under_pointer()
                                       : std::optional<doc::NodeId>{};
  }

  // Resolve a live clip proposal before anything is drawn, so the bar under
  // the pointer follows the gesture without authoring intermediate document
  // edits. Like Feed Mapping, the document changes once, on release.
  if (clip_drag_ != ClipDrag::None) {
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
      clip_drag_moved_ = false;
      clip_drop_lane_.reset();
      clip_snap_guide_.reset();
    } else if (clip_drag_active) {
      const core::i64 pointer_frame =
          static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_x())));
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
        if (origin.start + semantic_delta < 0) {
          semantic_delta = -origin.start;
        }
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
        proposed.start = std::max(start, first_i);
        break;
      }
      case ClipDrag::TrimIn: {
        // The out point stays where it is and the source slides with
        // the in point, which is what makes this a trim rather than a
        // move: the frames that remain still show what they showed.
        const core::i64 limit = origin.start + origin.duration - 1;
        const core::i64 start = delta == 0
                                    ? origin.start
                                    : std::clamp(snapped(origin.start + delta),
                                                 first_i, limit);
        const core::i64 shift = start - origin.start;
        proposed.start = start;
        proposed.duration = origin.duration - shift;
        proposed.source_in = std::max(origin.source_in + shift, core::i64{0});
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
      case ClipDrag::Slip:
        // Placement is unchanged; the source ordinals move only after
        // the Director applies the exact rational rate mapping.
        break;
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
    while (end < rows.size() && rows[end].kind != Row::Kind::Layer &&
           rows[end].kind != Row::Kind::LaneHeader) {
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
    return source.parent_group == destination.parent_group;
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
      layer_drag_moved_ = false;
    } else {
      layer_drag_label = layer_name(*source);
      if (layer_drag_active) {
        frame.lane_editing = ctx.pointer_down();
        std::optional<doc::NodeId> assignment;
        if (source->kind == ElementKind::Media) {
          assignment = clip_lane_under_pointer();
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
              layer_drag_node_, ctx.pointer_x(), ctx.pointer_y()};
        }
        released_layer_drag_node = layer_drag_node_;
        released_layer_drag_moved = layer_drag_moved_;
        layer_drag_node_ = doc::NodeId{};
        layer_drag_start_x_ = 0.0F;
        layer_drag_start_y_ = 0.0F;
        layer_drag_moved_ = false;
      }
    }
  }

  // One-shot operations show the exact affected show-time span before GO.
  // The Director remains authoritative for locks, source handles, and
  // overlap rejection, so this is deliberately a preview rather than a
  // locally-mutated placement set.
  std::optional<core::i64> preview_begin;
  std::optional<core::i64> preview_end;
  gui::UiColor preview_color = ctx.theme().accent;
  if (edit_tool_ == TimelineEditTool::RippleDelete && ripple_delete_ready) {
    preview_begin = ripple_begin;
    preview_end = ripple_end;
    preview_color = ctx.theme().warning;
  } else if ((edit_tool_ == TimelineEditTool::Insert ||
              edit_tool_ == TimelineEditTool::Overwrite) &&
             paste_ready) {
    const core::i64 start = signed_frame(timeline.show_frame);
    if (*clipboard_span <= std::numeric_limits<core::i64>::max() - start) {
      preview_begin = start;
      preview_end = start + *clipboard_span;
      preview_color = edit_tool_ == TimelineEditTool::Overwrite
                          ? ctx.theme().warning
                          : ctx.theme().accent;
    }
  }
  if (preview_begin.has_value() && preview_end.has_value()) {
    const float preview_x = frame_to_x(static_cast<double>(*preview_begin));
    const float preview_right = frame_to_x(static_cast<double>(*preview_end));
    const gui::Rect preview{
        std::max(preview_x, graph.x), rows_bounds.y,
        std::max(std::min(preview_right, graph.x + graph.width) -
                     std::max(preview_x, graph.x),
                 0.0F),
        rows_bounds.height};
    ctx.push_clip(rows_bounds);
    ctx.panel(preview, alpha(preview_color, 0.12F));
    ctx.pop_clip();
  }

  ctx.push_clip(rows_bounds);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    const float y =
        rows_bounds.y + static_cast<float>(index) * row_height - row_scroll_;
    if (y + row_height <= rows_bounds.y ||
        y >= rows_bounds.y + rows_bounds.height) {
      continue;
    }
    const float centre_y = y + row_height * 0.5F;
    std::optional<ChannelIdentity> row_channel_identity;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      row_channel_identity = ChannelIdentity{
          row.kind == Row::Kind::Property ? row.property_target : row.node,
          row.kind == Row::Kind::Property
              ? row.property
              : static_cast<scene::PropertyChannel>(row.group * 3U +
                                                    row.component)};
    }
    const bool is_selected =
        row.kind == Row::Kind::LaneHeader
            ? row.node == selected_lane_
            : (row.surface ? row.node == selected_surface
                           : selected(row.node, selection));
    const gui::UiColor background =
        is_selected
            ? ctx.theme().selection
            : ((index % 2U) == 0U ? ctx.theme().row : ctx.theme().row_alt);
    ctx.panel(gui::Rect{rows_bounds.x, y, rows_bounds.width, row_height},
              background);
    ctx.panel(gui::Rect{graph.x, y + row_height - 1.0F, graph.width, 1.0F},
              ctx.theme().divider);

    const float text_x =
        rows_bounds.x + 8.0F + static_cast<float>(row.indent) * indent_width;
    const core::u64 row_group = group_id(row.node, row.group);
    const bool open = row.kind == Row::Kind::Layer
                          ? expanded_layers_.count(row.node.value()) != 0U
                          : (row.kind == Row::Kind::Group
                                 ? expanded_groups_.count(row_group) != 0U
                                 : false);
    if (row.expandable) {
      draw_twirl(ctx, text_x + 4.0F, centre_y, open, ctx.theme().text_muted);
    }
    const float name_x = text_x + 14.0F;
    if (row.kind == Row::Kind::LaneHeader) {
      draw_drag_grip(ctx, text_x + 4.0F, centre_y,
                     row.locked ? ctx.theme().text_muted : row.color);
      ctx.panel(gui::Rect{name_x, centre_y - 5.0F, 4.0F, 10.0F},
                row.muted ? ctx.theme().warning : row.color);
      const gui::Rect name_bounds{name_x + 10.0F, y,
                                  std::max(graph.x - name_x - 58.0F, 0.0F),
                                  row_height};
      if (lane_name_node_ == row.node) {
        const gui::UiId name_id = mix(id_, "lane name", row.node.value());
        if (ctx.text_field(name_id, name_bounds, lane_name_draft_) &&
            !lane_name_draft_.empty()) {
          frame.lane_edits.push_back(TimelineLaneEdit{
              TimelineLaneAction::Rename, row.node, lane_name_draft_});
        }
        frame.lane_editing = frame.lane_editing || ctx.focus() == name_id;
      } else {
        ctx.label(name_bounds, row.label,
                  row.muted ? ctx.theme().warning : ctx.theme().text);
      }
      // The toggles double as the state readout, so the row needs no badge
      // saying MUTED or LOCKED next to a control that already says it.
      const auto header_toggle = [&](std::string_view id, float right_offset,
                                     bool engaged, bool lock,
                                     TimelineLaneAction action) {
        const gui::Rect bounds{graph.x - right_offset, y + 3.0F, 20.0F,
                               row_height - 6.0F};
        if (bounds.x <= name_bounds.x) {
          return;
        }
        if (toggle_glyph(ctx, mix(id_, id, row.node.value()), bounds, engaged,
                         lock)) {
          frame.lane_edits.push_back(TimelineLaneEdit{action, row.node, {}});
        }
      };
      header_toggle("track mute", 46.0F, row.muted, false,
                    TimelineLaneAction::ToggleMute);
      header_toggle("track lock", 24.0F, row.locked, true,
                    TimelineLaneAction::ToggleLock);
    } else if (row.kind == Row::Kind::Layer) {
      const ElementInfo *const row_element =
          row.surface ? nullptr : find_element(elements, row.node);
      const bool draggable_layer = row_element != nullptr &&
                                   orderable_layer(*row_element) &&
                                   !row.locked;
      const float marker_x = name_x + (draggable_layer ? 11.0F : 0.0F);
      if (draggable_layer) {
        draw_drag_grip(ctx, name_x + 3.0F, centre_y, row.color);
      }
      ctx.panel(gui::Rect{marker_x, centre_y - 5.0F, 4.0F, 10.0F}, row.color);
      ctx.label(gui::Rect{marker_x + 10.0F, y,
                          std::max(graph.x - marker_x - 58.0F, 0.0F), row_height},
                row.label,
                row.locked ? ctx.theme().text_muted : ctx.theme().text);
      // Everything the timeline lists can be switched off and protected, not
      // only the clips: a light or an LED surface is as worth muting while
      // working as a plate is. A surface is not a scene node, so it keeps its
      // badge and takes no toggles.
      if (row.surface) {
        ctx.label(gui::Rect{graph.x - 54.0F, y, 52.0F, row_height}, "SURFACE",
                  ctx.theme().text_muted);
      } else {
        const auto element_toggle = [&](std::string_view id,
                                        float right_offset, bool engaged,
                                        bool lock, TimelineElementFlag flag,
                                        bool value) {
          const gui::Rect bounds{graph.x - right_offset, y + 3.0F, 20.0F,
                                 row_height - 6.0F};
          if (bounds.x <= name_x) {
            return;
          }
          if (toggle_glyph(ctx, mix(id_, id, row.node.value()), bounds, engaged,
                           lock)) {
            frame.element_flags.push_back(
                TimelineElementFlagEdit{row.node, flag, value});
          }
        };
        const bool hidden = !row.element_visible;
        element_toggle("element mute", 46.0F, hidden, false,
                       TimelineElementFlag::Visible, hidden);
        element_toggle("element lock", 24.0F, row.locked, true,
                       TimelineElementFlag::Locked, !row.locked);
      }
    } else {
      ctx.label(gui::Rect{name_x, y, std::max(graph.x - name_x - 43.0F, 0.0F),
                          row_height},
                row.label, row.color);
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
    if (!row.locked && row.kind != Row::Kind::Layer) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      const output::ProjectionSurface *const row_surface =
          row.surface ? find_surface(surfaces, row.node) : nullptr;
      if ((row_surface != nullptr) ||
          (row_element != nullptr && !is_light(row_element->kind))) {
        bool animated = false;
        bool keyed_here = true;
        const core::i64 show_frame = signed_frame(timeline.show_frame);
        const core::u8 first_component =
            row.kind == Row::Kind::Property
                ? static_cast<core::u8>(row.property)
                : static_cast<core::u8>(
                      row.group * 3U +
                      (row.kind == Row::Kind::Component ? row.component : 0U));
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
        row.kind == Row::Kind::LaneHeader
            ? (lane_name_node_ == row.node
                   ? 24.0F
                   : std::max(list_width - 52.0F, 0.0F))
            : list_width;
    const gui::Interaction label_it =
        ctx.grab(mix(id_, "row", static_cast<core::u64>(index)),
                 gui::Rect{rows_bounds.x, interaction_y, label_width,
                           interaction_height});
    if (label_it.pressed && row.kind == Row::Kind::LaneHeader && !row.locked) {
      lane_drag_node_ = row.node;
      lane_drag_start_x_ = ctx.pointer_x();
      lane_drag_start_y_ = ctx.pointer_y();
      lane_drag_moved_ = false;
    } else if (label_it.pressed && row.kind == Row::Kind::Layer &&
               !row.surface) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      if (row_element != nullptr && orderable_layer(*row_element) &&
          !row.locked) {
        selected_keys_.clear();
        drag_keys_.clear();
        dragging_key_ = false;
        duplicate_drag_ = false;
        layer_drag_node_ = row.node;
        layer_drag_start_x_ = ctx.pointer_x();
        layer_drag_start_y_ = ctx.pointer_y();
        layer_drag_moved_ = false;
        marquee_tracking_ = false;
        marquee_active_ = false;
        lasso_points_.clear();
        TimelineSelectionRequest next{};
        next.node = row.node;
        next.action =
            selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
        if (next.action == SelectionAction::Range) {
          next.visible_order = visible_order;
        }
        frame.selection = std::move(next);
      }
    }
    const bool dragged_header_click =
        row.kind == Row::Kind::LaneHeader && row.node == lane_drag_node_ &&
        lane_drag_moved_;
    const bool dragged_layer_click =
        row.kind == Row::Kind::Layer && row.node == released_layer_drag_node &&
        released_layer_drag_moved;
    if (label_it.clicked && !dragged_header_click && !dragged_layer_click) {
      if (row.kind == Row::Kind::LaneHeader) {
        selected_lane_ = row.node;
        lane_destination_ = doc::NodeId{};
        lane_name_node_ = doc::NodeId{};
        lane_name_draft_.clear();
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
          next.action = SelectionAction::Replace;
          frame.selection = std::move(next);
        }
        // Second click on the same header opens the name for editing.
        const double now = ctx.time_seconds();
        if (lane_click_node_ == row.node && now >= lane_click_time_ &&
            now - lane_click_time_ <= lane_rename_click_seconds &&
            !row.locked) {
          lane_name_node_ = row.node;
          const auto lane =
              std::find_if(lanes.begin(), lanes.end(),
                           [&](const scene::TimelineLane &candidate) {
                             return candidate.id == row.node;
                           });
          lane_name_draft_ = lane != lanes.end() ? lane->name : row.label;
          // The field is not drawn until the next frame, so the click that
          // opened it cannot also focus it. Say so rather than making the
          // operator click a third time.
          ctx.focus_text_field(mix(id_, "lane name", row.node.value()),
                               lane_name_draft_);
          lane_click_node_ = doc::NodeId{};
          lane_click_time_ = 0.0;
        } else {
          lane_click_node_ = row.node;
          lane_click_time_ = now;
        }
        continue;
      }
      // Clicking anything else closes an open track name, so the field never
      // outlives the row it belongs to.
      lane_name_node_ = doc::NodeId{};
      lane_click_node_ = doc::NodeId{};
      const bool on_twirl = row.expandable && ctx.pointer_x() < name_x;
      if (on_twirl) {
        if (row.kind == Row::Kind::Layer) {
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
        frame.selection = std::move(next);
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
                : layer_element->timing;
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
          }
        }
        ctx.draw_list().add_image_region(
            clip_bar.x, clip_bar.y, clip_bar.width, clip_bar.height,
            *observation->texture, u0, 0.0F, u1, 1.0F,
            gui::UiColor{1.0F, 1.0F, 1.0F, live ? 0.62F : 0.78F});
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
          const core::i64 show_offset = std::clamp(
              static_cast<core::i64>(std::floor(x_to_frame(x))) -
                  clip_timing.start,
              core::i64{0}, clip_timing.duration - 1);
          const auto source_delta = scene::source_ordinal_delta(
              show_offset, show_frame_rate, clip_element->source_frame_rate);
          const core::i64 source_ordinal = std::clamp(
              clip_timing.source_in +
                  (source_delta ? source_delta.value() : show_offset),
              core::i64{0}, maximum_source);
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
      ctx.panel(gui::Rect{clip_fade_in_x - 3.0F, clip_bar.y, 6.0F, 5.0F}, ramp);
      ctx.panel(gui::Rect{clip_fade_out_x - 3.0F, clip_bar.y, 6.0F, 5.0F},
                ramp);

      if (!clip_element->on_air) {
        ctx.label(gui::Rect{clip_bar.x + 6.0F, clip_bar.y,
                            std::max(clip_bar.width - 12.0F, 0.0F),
                            clip_bar.height},
                  "off air", ctx.theme().text_muted);
      }
    } else if (row.kind == Row::Kind::Layer) {
      const float x0 = frame_to_x(static_cast<double>(first_i));
      const float x1 = frame_to_x(static_cast<double>(last_i));
      const gui::Rect clip{x0, y + 3.0F, std::max(x1 - x0, 1.0F),
                           row_height - 6.0F};
      ctx.panel(clip, alpha(row.color, 0.28F));
      ctx.panel(gui::Rect{clip.x, clip.y, clip.width, 1.0F},
                alpha(row.color, 0.72F));
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
        const auto aggregate =
            static_cast<scene::TransformChannel>(row.group * 3U + component);
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
    if (!row.locked && lane_it.pressed && clip_element != nullptr &&
        clip_bar.contains(ctx.pointer_x(), ctx.pointer_y())) {
      selected_keys_.clear();
      drag_keys_.clear();
      dragging_key_ = false;
      duplicate_drag_ = false;
      const float grab = edge_grab_width(clip_bar.width);
      const float px = ctx.pointer_x();
      const bool already_selected = selected(row.node, selection);
      clip_drag_peer_ = doc::NodeId{};
      if (edit_tool_ == TimelineEditTool::RippleTrim && one_clip &&
          already_selected) {
        const float midpoint = clip_bar.x + clip_bar.width * 0.5F;
        clip_drag_ =
            px < midpoint ? ClipDrag::RippleTrimHead : ClipDrag::RippleTrimTail;
      } else if (edit_tool_ == TimelineEditTool::Roll && roll_ready &&
                 already_selected) {
        clip_drag_ = ClipDrag::Roll;
        clip_drag_peer_ = selected_clips.front()->node == row.node
                              ? selected_clips.back()->node
                              : selected_clips.front()->node;
      } else if (edit_tool_ == TimelineEditTool::Slip && one_clip &&
                 already_selected) {
        clip_drag_ = ClipDrag::Slip;
      } else if (edit_tool_ == TimelineEditTool::Slide && slide_ready &&
                 already_selected) {
        clip_drag_ = ClipDrag::Slide;
      } else {
        // Standard placement editing. Fade handles sit on the bar's
        // top edge and take precedence over the trim strips.
        const bool on_top = ctx.pointer_y() <= clip_bar.y + 6.0F;
        if (on_top && std::fabs(px - clip_fade_in_x) <= 4.0F) {
          clip_drag_ = ClipDrag::FadeIn;
        } else if (on_top && std::fabs(px - clip_fade_out_x) <= 4.0F) {
          clip_drag_ = ClipDrag::FadeOut;
        } else if (px <= clip_bar.x + grab) {
          clip_drag_ = ClipDrag::TrimIn;
        } else if (px >= clip_bar.x + clip_bar.width - grab) {
          clip_drag_ = ClipDrag::TrimOut;
        } else {
          clip_drag_ = ClipDrag::Move;
        }
      }
      clip_drag_node_ = row.node;
      clip_drag_origin_ = clip_timing;
      clip_drag_proposed_ = clip_timing;
      clip_drag_anchor_ = static_cast<core::i64>(std::llround(x_to_frame(px)));
      clip_drag_delta_ = 0;
      clip_drag_start_x_ = ctx.pointer_x();
      clip_drag_start_y_ = ctx.pointer_y();
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
          const float dx = component_x[key_index] - ctx.pointer_x();
          const float dy = component_y[key_index] - ctx.pointer_y();
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
        marquee_start_x_ = ctx.pointer_x();
        marquee_start_y_ = ctx.pointer_y();
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
        clip_drag_ == ClipDrag::None &&
        layer_drag_node_ == doc::NodeId{} &&
        lane_drag_node_ == doc::NodeId{}) {
      frame.seek_to_frame =
          rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
    }
    ctx.pop_clip();
  }

  ctx.pop_clip();

  // Resolve the track drop after every band has been drawn. Reordering is an
  // insertion between peers, never a drop into a peer, so the only destination
  // feedback is the exact landing edge. Crossing several tracks still reports
  // one absolute document edit rather than one adjacent swap per band.
  const LaneDropTarget *lane_drop_hovered = nullptr;
  std::optional<float> lane_drop_landing_y;
  std::string_view lane_drag_label{"TRACK"};
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
    }
    if (!ctx.pointer_down()) {
      if (lane_drag_moved_ && !lane_drop_committed) {
        frame.layer_drop = TimelineLayerDrop{
            lane_drag_node_, ctx.pointer_x(), ctx.pointer_y()};
      }
      lane_drag_node_ = doc::NodeId{};
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

  // Static playhead over every lane and the ruler. It is drawn after the rows
  // so it stays findable over a saturated clip colour.
  ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width,
                          ruler.height + rows_bounds.height});
  const float playhead_x = frame_to_x(static_cast<double>(timeline.show_frame));
  const std::optional<core::i64> guide =
      dragging_key_
          ? snap_guide_
          : (clip_drag_ != ClipDrag::None ? clip_snap_guide_
                                          : std::optional<core::i64>{});
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
    ctx.draw_list().add_triangle(playhead_x - 5.0F, ruler.y, playhead_x + 5.0F,
                                 ruler.y, playhead_x, ruler.y + 7.0F,
                                 ctx.theme().playhead);
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
  if (edit_popup_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (edit_menu_open_) {
    ctx.panel(edit_popup, ctx.theme().background);
    ctx.panel(gui::Rect{edit_popup.x, edit_popup.y, edit_popup.width, 1.0F},
              ctx.theme().divider);
    ctx.panel(gui::Rect{edit_popup.x, edit_popup.y, 1.0F, edit_popup.height},
              ctx.theme().divider);
    ctx.panel(gui::Rect{edit_popup.x + edit_popup.width - 1.0F, edit_popup.y,
                        1.0F, edit_popup.height},
              ctx.theme().divider);
    for (std::size_t index = 0U; index < edit_choices.size(); ++index) {
      const EditChoice &entry = edit_choices[index];
      const gui::Rect choice{edit_popup.x + 4.0F,
                             edit_popup.y + 4.0F +
                                 static_cast<float>(index) * edit_choice_height,
                             edit_popup.width - 8.0F, edit_choice_height};
      const bool enabled = edit_choice_enabled(entry.tool);
      const bool hovered =
          enabled && choice.contains(ctx.pointer_x(), ctx.pointer_y());
      ctx.panel(choice,
                entry.tool == edit_tool_
                    ? ctx.theme().selection
                    : (hovered ? ctx.theme().widget_hover : ctx.theme().row));
      ctx.label(gui::Rect{choice.x + 7.0F, choice.y,
                          std::max(choice.width - 7.0F, 0.0F), choice.height},
                entry.label,
                enabled ? ctx.theme().text : ctx.theme().text_muted);
    }
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
        layer_assignment_feedback ? "MOVE · " : "REORDER · "};
    ghost_label += layer_drag_label;
    if (layer_lane_hovered != nullptr) {
      ghost_label += "  →  ";
      ghost_label += layer_lane_hovered->label;
    } else if (layer_assignment_feedback) {
      ghost_label += "  →  OWN TRANSFORM";
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
    std::string ghost_label{"REORDER · "};
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

    std::string ghost_label{"MOVE · "};
    ghost_label += clip_drag_label;
    if (clip_drop_hovered != nullptr) {
      ghost_label += "  →  ";
      ghost_label += clip_drop_hovered->label;
    } else if (clip_drop_lane_.has_value() &&
               *clip_drop_lane_ == doc::NodeId{}) {
      const ElementInfo *const dragged =
          find_element(elements, clip_drag_node_);
      if (dragged != nullptr && dragged->timeline_lane != doc::NodeId{}) {
        ghost_label += "  →  OWN TRANSFORM";
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

  frame.editing = range_fields_.live() || key_value_field_.live() ||
                  ctx.focus() == search_id ||
                  ctx.focus() == scale_numerator_id ||
                  ctx.focus() == scale_denominator_id ||
                  ctx.focus() == scale_user_anchor_id || rate_menu_open_ ||
                  edit_menu_open_ || frame.lane_editing;
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

  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
