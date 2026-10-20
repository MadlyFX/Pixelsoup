#pragma once

#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

// Internal to the Timeline panel: the Curves view. In Curves mode the lanes
// become one graph, the full height of the rows, on which every visible
// channel's curve and keys are overlaid against a value axis. The graph
// shares the dope sheet's time axis, so the two zoom and pan together, and
// the panel's key selection, so a selection carries across the switch.
namespace mediaserver::editor::timeline {

// The graph's vertical axis: which values sit at the bottom and the top of
// the area it is drawn in.
class ValueAxis final {
public:
  [[nodiscard]] double bottom() const noexcept { return bottom_; }
  [[nodiscard]] double top() const noexcept { return top_; }

  [[nodiscard]] float y(double value, const gui::Rect &area) const noexcept {
    return area.y + static_cast<float>((top_ - value) / (top_ - bottom_) *
                                       static_cast<double>(area.height));
  }
  [[nodiscard]] double value(float y, const gui::Rect &area) const noexcept {
    return top_ - static_cast<double>(y - area.y) /
                      static_cast<double>(area.height) * (top_ - bottom_);
  }

  // Shows exactly [bottom, top]. Ignored unless bottom < top.
  void show(double bottom, double top) noexcept;
  // Shows [minimum, maximum] with a tenth of its span spare above and
  // below; a single value is shown in the middle of a small range.
  void fit(double minimum, double maximum) noexcept;
  // Scales the span by 1 / factor while `anchor` keeps its place.
  void zoom_about(double anchor, double factor) noexcept;
  // Moves the view by `pixels` of the area's height; positive moves the
  // curves down, showing higher values.
  void pan(float pixels, const gui::Rect &area) noexcept;
  // A round step - 1, 2 or 5 times a power of ten - at least
  // `minimum_spacing` pixels apart on `area`.
  [[nodiscard]] double tick_step(const gui::Rect &area,
                                 float minimum_spacing) const noexcept;
  // Every multiple of tick_step() the area shows, bottom to top.
  [[nodiscard]] std::vector<double> ticks(const gui::Rect &area,
                                          float minimum_spacing) const;

  bool operator==(const ValueAxis &) const = default;

private:
  double bottom_{0.0};
  double top_{1.0};
};

// One curve the graph shows.
struct GraphChannel final {
  ChannelId id{};
  const scene::ChannelKeyframes *keys{};
  gui::UiColor color{};
  // The channel the KEYS popup edits: drawn heavier.
  bool active{};
  // A locked target's keys are drawn but cannot be picked.
  bool locked{};
};

// One key as the graph draws it. A key drag shows the key at its proposed
// frame; an Alt-duplicate drag leaves it and draws its copy at duplicate_x.
struct GraphKey final {
  std::size_t channel{};
  std::size_t index{};
  core::i64 frame{};
  float x{};
  float y{};
  bool selected{};
  float duplicate_x{std::numeric_limits<float>::quiet_NaN()};
};

class TimelineGraphView final {
public:
  // Which channels are shown, in the dope sheet as on the graph.
  [[nodiscard]] bool visible(const ChannelId &channel) const noexcept {
    return !hidden_.contains(channel);
  }
  void set_visible(const ChannelId &channel, bool shown);
  // The channel the KEYS popup reads and edits at the playhead.
  [[nodiscard]] const std::optional<ChannelId> &active() const noexcept {
    return active_;
  }
  void set_active(std::optional<ChannelId> channel) noexcept {
    active_ = channel;
  }
  // A new project or scene: every channel shown, none active, and the next
  // graph fitted to what it shows.
  void reset() noexcept;

  [[nodiscard]] ValueAxis &axis() noexcept { return axis_; }
  [[nodiscard]] const ValueAxis &axis() const noexcept { return axis_; }
  // How far the operator has zoomed and panned since the last fit: 1 and 0
  // right after one. Pan is in fitted spans, positive showing higher values.
  [[nodiscard]] float zoom() const noexcept;
  [[nodiscard]] float pan() const noexcept;

  // The next graph drawn fits its value axis first.
  void request_fit() noexcept { fit_requested_ = true; }
  [[nodiscard]] bool fit_requested() const noexcept { return fit_requested_; }
  // Fits the value axis to everything `channels` show between `first` and
  // `last`: their keys and the evaluated curve between them, overshoot
  // included.
  void fit(std::span<const GraphChannel> channels, core::i64 first,
           core::i64 last);
  // Fits the value axis to [minimum, maximum] and takes it as the new fit.
  void fit_values(double minimum, double maximum) noexcept;

  // The curve playback evaluates, as points on `area`: one sample of
  // scene::sample_keyframes per show frame when frames are at least a pixel
  // apart, and otherwise one per pixel column, at that column's nearest
  // frame. Show time only takes whole frames, so this is the exact curve.
  [[nodiscard]] static std::vector<gui::UiPoint>
  sample_curve(const scene::ChannelKeyframes &channel,
               const TimelineTimeAxis &time, const ValueAxis &axis,
               const gui::Rect &area);

  // The value grid and its ruler, every channel's curve, and the keys.
  // Pressing, dragging and selecting keys stay with the panel, which owns
  // the selection the dope sheet shares; a value or tangent drag would
  // read a key's value from `axis()` the same way.
  void draw(gui::UiContext &ctx, const gui::Rect &area,
            const TimelineTimeAxis &time,
            std::span<const GraphChannel> channels,
            std::span<const GraphKey> keys);

  // The fewest of `points` that keep every one of them within `tolerance`
  // pixels of the line through those kept (Douglas-Peucker). The first and
  // last always stay.
  [[nodiscard]] static std::vector<gui::UiPoint>
  simplify(std::span<const gui::UiPoint> points, float tolerance);

  // A channel's sampled curve, simplified to a quarter pixel, kept between
  // frames and sampled again only when its keys, either axis or the area
  // change.
  [[nodiscard]] const std::vector<gui::UiPoint> &
  curve(const GraphChannel &channel, const TimelineTimeAxis &time,
        const gui::Rect &area);
  // How many curves were sampled afresh since the count was last read.
  [[nodiscard]] std::size_t take_resampled() noexcept {
    const std::size_t count = resampled_;
    resampled_ = 0U;
    return count;
  }

private:
  ValueAxis axis_;
  double fit_bottom_{0.0};
  double fit_top_{1.0};
  bool fit_requested_{true};
  std::set<ChannelId> hidden_;
  std::optional<ChannelId> active_;

  struct CachedCurve final {
    scene::ChannelKeyframes keys;
    TimelineTimeAxis time;
    ValueAxis axis;
    gui::Rect area;
    std::vector<gui::UiPoint> points;
    bool used{};
  };
  std::map<ChannelId, CachedCurve> curves_;
  std::size_t resampled_{};
};

} // namespace mediaserver::editor::timeline
