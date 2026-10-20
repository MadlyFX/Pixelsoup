#include "timeline_graph_view.hpp"

#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

namespace mediaserver::editor::timeline {
namespace {

// The largest span the axis zooms out to, and the smallest it zooms in to,
// relative to the values it shows.
constexpr double maximum_span = 1.0e12;
constexpr double minimum_span = 1.0e-9;
// The most frames a fit samples per channel between its keys.
constexpr core::i64 fit_samples = 512;

} // namespace

void ValueAxis::show(double bottom, double top) noexcept {
  if (std::isfinite(bottom) && std::isfinite(top) && bottom < top) {
    bottom_ = bottom;
    top_ = top;
  }
}

void ValueAxis::fit(double minimum, double maximum) noexcept {
  if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
      minimum > maximum) {
    show(-1.0, 1.0);
    return;
  }
  const double span = maximum - minimum;
  const double spare =
      span > minimum_span * std::max(1.0, std::fabs(maximum))
          ? span * 0.1
          : std::max(std::fabs(maximum) * 0.1, 0.5);
  show(minimum - spare, maximum + spare);
}

void ValueAxis::zoom_about(double anchor, double factor) noexcept {
  if (!std::isfinite(anchor) || !std::isfinite(factor) || factor <= 0.0) {
    return;
  }
  const double bottom = anchor + (bottom_ - anchor) / factor;
  const double top = anchor + (top_ - anchor) / factor;
  const double scale = std::max({1.0, std::fabs(bottom), std::fabs(top)});
  if (top - bottom < minimum_span * scale || top - bottom > maximum_span) {
    return;
  }
  show(bottom, top);
}

void ValueAxis::pan(float pixels, const gui::Rect &area) noexcept {
  if (area.height <= 0.0F) {
    return;
  }
  const double delta = static_cast<double>(pixels) /
                       static_cast<double>(area.height) * (top_ - bottom_);
  show(bottom_ + delta, top_ + delta);
}

double ValueAxis::tick_step(const gui::Rect &area,
                            float minimum_spacing) const noexcept {
  if (area.height <= 0.0F) {
    return top_ - bottom_;
  }
  const double wanted = static_cast<double>(minimum_spacing) *
                        (top_ - bottom_) / static_cast<double>(area.height);
  const double magnitude = std::pow(10.0, std::floor(std::log10(wanted)));
  for (const double multiple : {1.0, 2.0, 5.0, 10.0}) {
    if (multiple * magnitude >= wanted) {
      return multiple * magnitude;
    }
  }
  return 10.0 * magnitude;
}

std::vector<double> ValueAxis::ticks(const gui::Rect &area,
                                     float minimum_spacing) const {
  std::vector<double> out;
  const double step = tick_step(area, minimum_spacing);
  if (!std::isfinite(step) || step <= 0.0) {
    return out;
  }
  const double first = std::ceil(bottom_ / step);
  const double last = std::floor(top_ / step);
  for (double index = first; index <= last && out.size() < 256U; ++index) {
    out.push_back(index * step);
  }
  return out;
}

void TimelineGraphView::set_visible(const ChannelId &channel, bool shown) {
  if (shown) {
    hidden_.erase(channel);
  } else {
    hidden_.insert(channel);
  }
}

void TimelineGraphView::reset() noexcept {
  hidden_.clear();
  active_.reset();
  fit_requested_ = true;
}

float TimelineGraphView::zoom() const noexcept {
  return static_cast<float>((fit_top_ - fit_bottom_) /
                            (axis_.top() - axis_.bottom()));
}

float TimelineGraphView::pan() const noexcept {
  const double middle = (axis_.top() + axis_.bottom()) * 0.5;
  const double fitted_middle = (fit_top_ + fit_bottom_) * 0.5;
  return static_cast<float>((middle - fitted_middle) /
                            (fit_top_ - fit_bottom_));
}

void TimelineGraphView::fit(std::span<const GraphChannel> channels,
                            core::i64 first, core::i64 last) {
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = -std::numeric_limits<double>::infinity();
  const auto take = [&](std::optional<double> value) {
    if (value.has_value() && std::isfinite(*value)) {
      minimum = std::min(minimum, *value);
      maximum = std::max(maximum, *value);
    }
  };
  for (const GraphChannel &channel : channels) {
    if (channel.keys == nullptr || channel.keys->frames.empty()) {
      continue;
    }
    const scene::ChannelKeyframes &keys = *channel.keys;
    for (const core::i64 frame : keys.frames) {
      take(scene::sample_keyframes(keys, frame));
    }
    // The curve between the keys, which a weighted or broken tangent can
    // carry beyond them, over the part of it the show range plays.
    const core::i64 begin = std::max(first, keys.frames.front());
    const core::i64 end = std::min(last, keys.frames.back());
    if (end > begin) {
      const core::i64 step = std::max<core::i64>((end - begin) / fit_samples, 1);
      for (core::i64 frame = begin; frame <= end; frame += step) {
        take(scene::sample_keyframes(keys, frame));
      }
    }
  }
  if (minimum > maximum) {
    fit_values(-1.0, 1.0);
  } else {
    fit_values(minimum, maximum);
  }
  fit_requested_ = false;
}

void TimelineGraphView::fit_values(double minimum, double maximum) noexcept {
  axis_.fit(minimum, maximum);
  fit_bottom_ = axis_.bottom();
  fit_top_ = axis_.top();
}

std::vector<gui::UiPoint>
TimelineGraphView::sample_curve(const scene::ChannelKeyframes &channel,
                                const TimelineTimeAxis &time,
                                const ValueAxis &axis, const gui::Rect &area) {
  std::vector<gui::UiPoint> points;
  if (area.width <= 0.0F || area.height <= 0.0F ||
      !(time.pixels_per_frame > 0.0F)) {
    return points;
  }
  constexpr double limit =
      static_cast<double>(std::numeric_limits<core::i64>::max() / 4);
  const auto add = [&](core::i64 frame) {
    const std::optional<double> value = scene::sample_keyframes(channel, frame);
    if (value.has_value() && std::isfinite(*value)) {
      points.push_back(gui::UiPoint{time.x(static_cast<double>(frame), area.x),
                                    axis.y(*value, area)});
    }
  };
  const double left = time.frame(area.x, area.x);
  const double right = time.frame(area.x + area.width, area.x);
  if (!std::isfinite(left) || !std::isfinite(right) || left < -limit ||
      right > limit) {
    return points;
  }
  if (time.pixels_per_frame >= 1.0F) {
    // Every frame on screen, plus one beyond each edge so the curve reaches
    // the area's sides.
    const auto begin = static_cast<core::i64>(std::floor(left));
    const auto end = static_cast<core::i64>(std::ceil(right));
    points.reserve(static_cast<std::size_t>(end - begin + 1));
    for (core::i64 frame = begin; frame <= end; ++frame) {
      add(frame);
    }
    return points;
  }
  // Frames closer than a pixel: one sample per column, at the frame nearest
  // its centre.
  const auto columns = static_cast<int>(std::ceil(area.width));
  points.reserve(static_cast<std::size_t>(columns) + 3U);
  bool have_previous = false;
  core::i64 previous{};
  for (int column = -1; column <= columns + 1; ++column) {
    const double at = time.frame(area.x + static_cast<float>(column), area.x);
    const auto frame = static_cast<core::i64>(std::llround(at));
    if (have_previous && frame == previous) {
      continue;
    }
    have_previous = true;
    previous = frame;
    add(frame);
  }
  return points;
}

std::vector<gui::UiPoint>
TimelineGraphView::simplify(std::span<const gui::UiPoint> points,
                            float tolerance) {
  if (points.size() <= 2U) {
    return {points.begin(), points.end()};
  }
  std::vector<bool> keep(points.size(), false);
  keep.front() = true;
  keep.back() = true;
  std::vector<std::pair<std::size_t, std::size_t>> spans{
      {0U, points.size() - 1U}};
  while (!spans.empty()) {
    const auto [first, last] = spans.back();
    spans.pop_back();
    const gui::UiPoint a = points[first];
    const gui::UiPoint b = points[last];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    float farthest = 0.0F;
    std::size_t at = first;
    for (std::size_t index = first + 1U; index < last; ++index) {
      const gui::UiPoint q = points[index];
      const float distance =
          length > 0.0F
              ? std::fabs(dy * (q.x - a.x) - dx * (q.y - a.y)) / length
              : std::sqrt((q.x - a.x) * (q.x - a.x) +
                          (q.y - a.y) * (q.y - a.y));
      if (distance > farthest) {
        farthest = distance;
        at = index;
      }
    }
    if (farthest > tolerance) {
      keep[at] = true;
      spans.emplace_back(first, at);
      spans.emplace_back(at, last);
    }
  }
  std::vector<gui::UiPoint> out;
  for (std::size_t index = 0U; index < points.size(); ++index) {
    if (keep[index]) {
      out.push_back(points[index]);
    }
  }
  return out;
}

const std::vector<gui::UiPoint> &
TimelineGraphView::curve(const GraphChannel &channel,
                         const TimelineTimeAxis &time, const gui::Rect &area) {
  CachedCurve &cached = curves_[channel.id];
  cached.used = true;
  const bool same_area = cached.area.x == area.x && cached.area.y == area.y &&
                         cached.area.width == area.width &&
                         cached.area.height == area.height;
  if (!cached.points.empty() && same_area && cached.time == time &&
      cached.axis == axis_ && cached.keys == *channel.keys) {
    return cached.points;
  }
  cached.keys = *channel.keys;
  cached.time = time;
  cached.axis = axis_;
  cached.area = area;
  cached.points =
      simplify(sample_curve(*channel.keys, time, axis_, area), 0.25F);
  ++resampled_;
  return cached.points;
}

void TimelineGraphView::draw(gui::UiContext &ctx, const gui::Rect &area,
                             const TimelineTimeAxis &time,
                             std::span<const GraphChannel> channels,
                             std::span<const GraphKey> keys) {
  for (auto &[id, cached] : curves_) {
    cached.used = false;
  }
  if (area.width <= 0.0F || area.height <= 0.0F) {
    return;
  }
  const gui::Theme &theme = ctx.theme();
  ctx.push_clip(area);
  ctx.panel(area, theme.panel);

  // The value grid, labelled down its left edge: the graph's own ruler.
  constexpr float tick_spacing = 36.0F;
  for (const double value : axis_.ticks(area, tick_spacing)) {
    const float y = axis_.y(value, area);
    const bool zero = std::fabs(value) <
                      axis_.tick_step(area, tick_spacing) * 1.0e-6;
    ctx.panel(gui::Rect{area.x, std::round(y), area.width, 1.0F},
              alpha(theme.divider, zero ? 1.0F : 0.55F));
    std::array<char, 32U> text{};
    const int length = std::snprintf(text.data(), text.size(), "%.4g",
                                     std::fabs(value) < 1.0e-12 ? 0.0 : value);
    ctx.label(gui::Rect{area.x + 4.0F, y - 15.0F, 72.0F, 14.0F},
              std::string_view{text.data(),
                               static_cast<std::size_t>(std::max(length, 0))},
              theme.text_muted);
  }

  // Curves, the active one heavier and drawn last so it reads on top.
  const auto draw_curve = [&](const GraphChannel &channel) {
    if (channel.keys == nullptr) {
      return;
    }
    const std::vector<gui::UiPoint> &points = curve(channel, time, area);
    const float width = channel.active ? 2.0F : 1.25F;
    const gui::UiColor colour = alpha(channel.color, channel.active ? 1.0F : 0.85F);
    for (std::size_t index = 1U; index < points.size(); ++index) {
      ctx.draw_list().add_line(points[index - 1U].x, points[index - 1U].y,
                               points[index].x, points[index].y, width,
                               colour);
    }
  };
  for (const GraphChannel &channel : channels) {
    if (!channel.active) {
      draw_curve(channel);
    }
  }
  for (const GraphChannel &channel : channels) {
    if (channel.active) {
      draw_curve(channel);
    }
  }

  for (const GraphKey &key : keys) {
    const gui::UiColor colour = channels[key.channel].color;
    if (key.selected) {
      draw_diamond(ctx, key.x, key.y, key_radius + 2.0F,
                   alpha(theme.accent, 0.50F));
    }
    draw_diamond(ctx, key.x, key.y,
                 key.selected ? key_radius + 0.5F : key_radius,
                 key.selected ? theme.playhead : colour);
    if (std::isfinite(key.duplicate_x)) {
      draw_diamond(ctx, key.duplicate_x, key.y, key_radius + 0.5F,
                   theme.accent);
    }
  }
  ctx.pop_clip();
  // Forget curves no longer shown, so the cache holds what is on screen.
  std::erase_if(curves_, [](const auto &entry) { return !entry.second.used; });
}

} // namespace mediaserver::editor::timeline
