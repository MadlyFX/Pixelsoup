#include <mediaserver/editor/asset_viewer_panel.hpp>

#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace mediaserver::editor {
namespace {

constexpr float header_height = 28.0F;
constexpr float scrub_height = 34.0F;
constexpr float transport_height = 30.0F;
constexpr float edit_height = 30.0F;
constexpr float readout_height = 20.0F;
constexpr std::size_t history_limit = 32U;
// A press on the picture that travels this far is a drag out, not a click.
constexpr float drag_threshold = 5.0F;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char c : name) {
    hash ^= static_cast<core::u8>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float a) noexcept {
  color.a = a;
  return color;
}

[[nodiscard]] double frames_per_second(assets::MediaFrameRate rate) noexcept {
  return rate.denominator == 0U
             ? 0.0
             : static_cast<double>(rate.numerator) /
                   static_cast<double>(rate.denominator);
}

[[nodiscard]] core::u32 nominal_fps(assets::MediaFrameRate rate) noexcept {
  return static_cast<core::u32>(
      std::max(std::llround(frames_per_second(rate)), 1LL));
}

// "3s 12f" at the source's nominal rate.
[[nodiscard]] std::string duration_text(core::u64 frames,
                                        assets::MediaFrameRate rate) {
  const core::u64 fps = nominal_fps(rate);
  std::array<char, 48U> text{};
  const int length =
      std::snprintf(text.data(), text.size(), "%llus %02lluf",
                    static_cast<unsigned long long>(frames / fps),
                    static_cast<unsigned long long>(frames % fps));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string_view
rendition_text(AssetViewerRendition rendition) noexcept {
  switch (rendition) {
  case AssetViewerRendition::None:
    return {};
  case AssetViewerRendition::Source:
    return "SOURCE";
  case AssetViewerRendition::Unchecked:
    return "SOURCE - NOT CHECKED";
  case AssetViewerRendition::Proxy:
    return "PROXY";
  case AssetViewerRendition::ProxyUnready:
    return "PROXY REQUIRED - NOT READY";
  case AssetViewerRendition::Offline:
    return "OFFLINE";
  }
  return {};
}

[[nodiscard]] std::string_view
rendition_tooltip(AssetViewerRendition rendition) noexcept {
  switch (rendition) {
  case AssetViewerRendition::Unchecked:
    return "Nothing in the project uses this asset, so no machine has "
           "checked its file. A missing file shows the grey placeholder.";
  case AssetViewerRendition::Proxy:
    return "Playing its proxy, by the asset's proxy policy";
  case AssetViewerRendition::ProxyUnready:
    return "Require Proxy, and no proxy is ready on this machine. Nothing "
           "is decoded, as on the wall.";
  case AssetViewerRendition::Offline:
    return "The file is missing on this machine";
  default:
    return "The asset's own file";
  }
}

} // namespace

struct AssetViewerPanel::Pass final {
  gui::UiContext &ctx;
  const gui::Rect &bounds;
  const AssetViewerDrawInput &input;
  AssetViewerFrame frame{};
  const assets::Asset *asset{};
  const assets::MediaAsset *media{};
  core::u32 count{1U};
  assets::MediaFrameRate rate{};
  gui::Rect header{};
  gui::Rect image{};
  gui::Rect scrub{};
  gui::Rect transport{};
  gui::Rect edit{};
  float ppu{1.0F};
};

void AssetViewerPanel::open(
    doc::NodeId asset, core::u32 frame,
    std::optional<std::pair<core::u32, core::u32>> marks) {
  if (asset != asset_) {
    if (history_.empty() || history_[history_index_] != asset) {
      if (!history_.empty()) {
        history_.resize(history_index_ + 1U);
      }
      history_.push_back(asset);
      if (history_.size() > history_limit) {
        history_.erase(history_.begin());
      }
      history_index_ = history_.size() - 1U;
    }
    asset_ = asset;
    framing_ = {};
    probe_value_.reset();
  }
  stop();
  frame_ = frame;
  mark_in_.reset();
  mark_out_.reset();
  if (marks.has_value()) {
    mark_in_ = std::min(marks->first, marks->second);
    mark_out_ = std::max(marks->first, marks->second);
  }
}

void AssetViewerPanel::show_message(std::string message, double now) {
  message_ = std::move(message);
  message_until_ = now + 3.0;
}

void AssetViewerPanel::reset_project_state() noexcept {
  asset_ = doc::NodeId{};
  history_.clear();
  history_index_ = 0U;
  frame_ = 0U;
  stop();
  mark_in_.reset();
  mark_out_.reset();
  framing_ = {};
  probe_value_.reset();
  dragging_ = false;
  drag_armed_ = false;
  message_.clear();
}

void AssetViewerPanel::play(int direction, double now) noexcept {
  direction_ = direction;
  origin_time_ = now;
  origin_frame_ = frame_;
}

void AssetViewerPanel::stop() noexcept {
  direction_ = 0;
  in_to_out_ = false;
}

void AssetViewerPanel::seek(core::u32 frame, core::u32 count) noexcept {
  frame_ = std::min(frame, count == 0U ? 0U : count - 1U);
  if (direction_ != 0) {
    origin_frame_ = frame_;
  }
}

std::pair<core::u32, core::u32>
AssetViewerPanel::marked_range(core::u32 count) const noexcept {
  const core::u32 last = count == 0U ? 0U : count - 1U;
  core::u32 in = std::min(mark_in_.value_or(0U), last);
  core::u32 out = std::min(mark_out_.value_or(last), last);
  if (out < in) {
    std::swap(in, out);
  }
  return {in, out};
}

void AssetViewerPanel::advance(Pass &p) {
  if (direction_ == 0 || p.media == nullptr) {
    return;
  }
  const double fps = frames_per_second(p.rate);
  if (fps <= 0.0) {
    stop();
    return;
  }
  const auto [in, out] = marked_range(p.count);
  const core::i64 low = in_to_out_ ? in : 0;
  const core::i64 high =
      in_to_out_ ? out : static_cast<core::i64>(p.count) - 1;
  const double elapsed = std::max(p.ctx.time_seconds() - origin_time_, 0.0);
  const core::i64 steps = static_cast<core::i64>(std::floor(elapsed * fps));
  core::i64 target = static_cast<core::i64>(origin_frame_) + direction_ * steps;
  if (target > high || target < low) {
    if (loop_ && high >= low) {
      const core::i64 span = high - low + 1;
      target = low + ((target - low) % span + span) % span;
    } else {
      target = std::clamp(target, low, high);
      direction_ = 0;
      in_to_out_ = false;
    }
  }
  frame_ = static_cast<core::u32>(std::max<core::i64>(target, 0));
}

void AssetViewerPanel::request_edit(Pass &p, SourceEditMode mode) {
  const double now = p.ctx.time_seconds();
  if (p.media == nullptr) {
    show_message("Open a media asset to cut it into a track", now);
    return;
  }
  if (p.input.edits_locked) {
    show_message("Edits wait until the transition finishes", now);
    return;
  }
  if (!p.media->frame_count.has_value()) {
    show_message("The source has not been probed yet", now);
    return;
  }
  if (!p.input.target_lane.has_value()) {
    show_message("Choose a target track with its target switch", now);
    return;
  }
  const auto [in, out] = marked_range(p.count);
  p.frame.source_edit = AssetViewerSourceEdit{mode, in, out - in + 1U};
}

void AssetViewerPanel::draw_header(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect &bar = p.header;
  ctx.panel(bar, theme.header);
  ctx.panel(gui::Rect{bar.x, bar.y + bar.height - 1.0F, bar.width, 1.0F},
            theme.divider);

  // Controls from the right, each only while there is room for it and the
  // name to its left.
  float right = bar.x + bar.width - 4.0F;
  const float y = bar.y + 3.0F;
  const float h = bar.height - 6.0F;
  const auto room = [&](float width) {
    return right - width > bar.x + 160.0F;
  };
  const auto place = [&](float width) {
    right -= width;
    const gui::Rect at{right, y, width, h};
    right -= 4.0F;
    return at;
  };
  if (p.media != nullptr && room(126.0F)) {
    const gui::Rect plus = place(22.0F);
    std::array<char, 24U> stops{};
    std::snprintf(stops.data(), stops.size(), "EV %+.1f", exposure_);
    right += 4.0F;
    const gui::Rect value = place(56.0F);
    right += 4.0F;
    const gui::Rect minus = place(22.0F);
    if (ctx.button(mix(id_, "ev-"), minus, "-")) {
      exposure_ = std::max(exposure_ - 0.5F, -8.0F);
    }
    if (ctx.button(mix(id_, "ev0"), value, stops.data())) {
      exposure_ = 0.0F;
    }
    if (ctx.button(mix(id_, "ev+"), plus, "+")) {
      exposure_ = std::min(exposure_ + 0.5F, 8.0F);
    }
    ctx.tooltip(value, "A display-only exposure offset; click to reset");
  }
  if (p.media != nullptr && room(78.0F)) {
    static constexpr std::array<std::string_view, 5U> channels{
        "RGB", "Red", "Green", "Blue", "Luma"};
    std::size_t selected = static_cast<std::size_t>(channel_);
    if (ctx.dropdown(mix(id_, "channel"), place(78.0F), channels, selected)) {
      channel_ = static_cast<AssetViewerChannel>(
          std::min<std::size_t>(selected, channels.size() - 1U));
    }
  }
  if (p.media != nullptr && room(140.0F)) {
    const gui::Rect actual = place(46.0F);
    right += 4.0F;
    const gui::Rect fit = place(40.0F);
    right += 4.0F;
    const gui::Rect zoom_text = place(46.0F);
    const float scale = asset_viewer_scale(
        framing_, asset_viewer_pixels(*p.media), p.image.width * p.ppu,
        std::max(p.image.height - readout_height, 1.0F) * p.ppu);
    std::array<char, 16U> percent{};
    std::snprintf(percent.data(), percent.size(), "%.0f%%",
                  static_cast<double>(scale) * 100.0);
    ctx.label(zoom_text, percent.data(), theme.text_muted);
    if (ctx.button(mix(id_, "fit"), fit, "Fit")) {
      framing_ = {};
    }
    if (ctx.button(mix(id_, "actual"), actual, "1:1")) {
      framing_.scale = 1.0F;
    }
    ctx.tooltip(actual, "One source pixel to one screen pixel");
  }
  if (room(52.0F)) {
    const gui::Rect forward = place(24.0F);
    right += 4.0F;
    const gui::Rect back = place(24.0F);
    const bool can_back = history_index_ > 0U && !history_.empty();
    const bool can_forward = history_index_ + 1U < history_.size();
    if (ctx.button(mix(id_, "back"), back, "<") && can_back) {
      --history_index_;
      const doc::NodeId wanted = history_[history_index_];
      asset_ = doc::NodeId{};
      open(wanted);
      // open() already reached this entry; keep the forward history.
    }
    if (ctx.button(mix(id_, "forward"), forward, ">") && can_forward) {
      ++history_index_;
      const doc::NodeId wanted = history_[history_index_];
      asset_ = doc::NodeId{};
      open(wanted);
    }
    ctx.tooltip(back, "Previous asset");
    ctx.tooltip(forward, "Next asset");
  }

  std::string title = p.asset != nullptr
                          ? std::string{assets::asset_display_name(*p.asset)}
                          : std::string{"Asset Viewer"};
  const float title_width =
      std::min(ctx.font_width(title) + 4.0F, std::max(right - bar.x - 12.0F, 0.0F));
  const gui::Rect title_rect{bar.x + 8.0F, bar.y, title_width, bar.height};
  ctx.label(title_rect, title, theme.text);
  if (p.asset != nullptr) {
    ctx.tooltip(title_rect, assets::asset_source(*p.asset));
  }

  // The file the picture is from, and whether it is still arriving.
  float chip_x = title_rect.x + title_rect.width + 8.0F;
  const auto chip = [&](std::string_view text, gui::UiColor color,
                        std::string_view tip) {
    const float width = ctx.font_width(text) + 12.0F;
    if (text.empty() || chip_x + width > right) {
      return;
    }
    const gui::Rect rect{chip_x, bar.y + 6.0F, width, bar.height - 12.0F};
    ctx.panel(rect, alpha(color, 0.18F));
    ctx.label(gui::Rect{rect.x + 6.0F, rect.y, rect.width - 6.0F, rect.height},
              text, color);
    ctx.tooltip(rect, tip);
    chip_x += width + 6.0F;
  };
  if (p.media != nullptr && p.input.director != nullptr &&
      p.input.director->shown.asset == asset_) {
    const AssetViewerRendition rendition = p.input.director->rendition;
    const bool warn = rendition == AssetViewerRendition::Offline ||
                      rendition == AssetViewerRendition::ProxyUnready;
    chip(rendition_text(rendition), warn ? theme.warning : theme.text_muted,
         rendition_tooltip(rendition));
  }
  if (p.media != nullptr && p.input.decoding) {
    chip("DECODING", theme.playhead,
         "Waiting for decoded frames; the last complete one is held");
  }
}

void AssetViewerPanel::draw_image(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect area = p.image;
  ctx.panel(area, theme.background);
  if (area.width < 8.0F || area.height < readout_height + 8.0F) {
    return;
  }
  // The picture renders above a strip that carries the probe readout.
  const gui::Rect picture_area{area.x, area.y, area.width,
                               area.height - readout_height};
  const gui::Rect readout{area.x, picture_area.y + picture_area.height,
                          area.width, readout_height};
  ctx.panel(readout, theme.panel);

  const auto centred = [&](std::string_view text, gui::UiColor color) {
    const float width = std::min(ctx.font_width(text), picture_area.width - 16.0F);
    ctx.label(gui::Rect{picture_area.x + (picture_area.width - width) * 0.5F,
                        picture_area.y + picture_area.height * 0.5F - 12.0F,
                        std::max(width, 0.0F), 24.0F},
              text, color);
  };
  if (asset_ == doc::NodeId{}) {
    centred("Double-click an asset in the Library to view it here",
            theme.text_muted);
    return;
  }
  if (p.asset == nullptr) {
    centred("This asset is no longer in the Library", theme.text_muted);
    return;
  }
  if (p.media == nullptr) {
    centred("The Asset Viewer shows media. Drag a model from the Library to "
            "place it.",
            theme.text_muted);
    return;
  }

  p.frame.surface_width = static_cast<core::u32>(
      std::max(std::lround(picture_area.width * p.ppu), 1L));
  p.frame.surface_height = static_cast<core::u32>(
      std::max(std::lround(picture_area.height * p.ppu), 1L));
  const float surface_w = static_cast<float>(p.frame.surface_width);
  const float surface_h = static_cast<float>(p.frame.surface_height);
  const assets::PixelDimensions source = asset_viewer_pixels(*p.media);

  if (p.input.image != nullptr && p.input.image->width() != 0U) {
    ctx.image(picture_area, *p.input.image);
  } else if (!p.input.image_fault.empty()) {
    centred(p.input.image_fault, theme.warning);
  }

  // Zoom about the pointer, pan with the middle button.
  const bool hovered = picture_area.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                       !ctx.pointer_blocked();
  if (hovered && ctx.wheel() != 0.0F) {
    framing_ = asset_viewer_zoom(
        framing_, source, surface_w, surface_h,
        static_cast<float>(std::pow(1.15, static_cast<double>(ctx.wheel()))),
        (ctx.pointer_x() - picture_area.x) * p.ppu,
        (ctx.pointer_y() - picture_area.y) * p.ppu);
  }
  if (hovered && ctx.middle_pointer_pressed()) {
    panning_ = true;
  }
  if (panning_ && !ctx.middle_pointer_down()) {
    panning_ = false;
  }
  if (panning_ && (ctx.pointer_dx() != 0.0F || ctx.pointer_dy() != 0.0F)) {
    framing_ = asset_viewer_pan(framing_, source, surface_w, surface_h,
                                ctx.pointer_dx() * p.ppu,
                                ctx.pointer_dy() * p.ppu);
  }

  // A left press on the picture drags it out, carrying the marked range.
  const gui::Interaction grab = ctx.grab(mix(id_, "picture"), picture_area);
  if (grab.pressed) {
    drag_armed_ = true;
    dragging_ = false;
    press_x_ = ctx.pointer_x();
    press_y_ = ctx.pointer_y();
  }
  if (drag_armed_ && grab.held && !grab.released &&
      std::hypot(ctx.pointer_x() - press_x_, ctx.pointer_y() - press_y_) >
          drag_threshold) {
    dragging_ = true;
    stop();
  }
  if (dragging_ && !grab.released) {
    p.frame.dragging = true;
    ctx.set_cursor(gui::Cursor::Move);
  }
  // The capture is still held on the frame it is let go, so the release is
  // what ends the drag.
  if (grab.released || (!grab.held && (drag_armed_ || dragging_))) {
    if (dragging_ && !p.bounds.contains(ctx.pointer_release_x(),
                                        ctx.pointer_release_y())) {
      const auto [in, out] = marked_range(p.count);
      p.frame.drop = AssetViewerDrop{asset_, in, out - in + 1U,
                                     ctx.pointer_release_x(),
                                     ctx.pointer_release_y()};
    }
    drag_armed_ = false;
    dragging_ = false;
  }

  // The probe: the screen pixel under the pointer, and the source pixel it
  // shows.
  const AssetViewerPictureRect picture =
      asset_viewer_picture_rect(framing_, source, surface_w, surface_h);
  const float px = (ctx.pointer_x() - picture_area.x) * p.ppu;
  const float py = (ctx.pointer_y() - picture_area.y) * p.ppu;
  const bool on_picture = hovered && px >= picture.x && py >= picture.y &&
                          px < picture.x + picture.width &&
                          py < picture.y + picture.height;
  std::string text;
  if (on_picture && picture.width > 0.0F) {
    p.frame.probe = std::pair{
        std::min(static_cast<core::u32>(std::max(px, 0.0F)),
                 p.frame.surface_width - 1U),
        std::min(static_cast<core::u32>(std::max(py, 0.0F)),
                 p.frame.surface_height - 1U)};
    const auto source_x = static_cast<core::u32>(std::clamp(
        (px - picture.x) / picture.width * static_cast<float>(source.width),
        0.0F, static_cast<float>(source.width - 1U)));
    const auto source_y = static_cast<core::u32>(std::clamp(
        (py - picture.y) / picture.height * static_cast<float>(source.height),
        0.0F, static_cast<float>(source.height - 1U)));
    std::array<char, 96U> buffer{};
    int length = 0;
    if (probe_value_.has_value()) {
      length = std::snprintf(
          buffer.data(), buffer.size(), "X %u  Y %u    R %d  G %d  B %d",
          source_x, source_y,
          static_cast<int>(std::lround(probe_value_->r * 255.0F)),
          static_cast<int>(std::lround(probe_value_->g * 255.0F)),
          static_cast<int>(std::lround(probe_value_->b * 255.0F)));
    } else {
      length = std::snprintf(buffer.data(), buffer.size(), "X %u  Y %u",
                             source_x, source_y);
    }
    text.assign(buffer.data(), static_cast<std::size_t>(std::max(length, 0)));
  } else {
    std::array<char, 64U> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%u x %u",
                                     source.width, source.height);
    text.assign(buffer.data(), static_cast<std::size_t>(std::max(length, 0)));
  }
  ctx.label(gui::Rect{readout.x + 8.0F, readout.y, readout.width - 16.0F,
                      readout.height},
            text, theme.text_muted);
  if (on_picture) {
    ctx.tooltip(readout, "Display values on this screen, after the "
                         "project's colour transform");
  }
}

void AssetViewerPanel::draw_scrub(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect bar{p.scrub.x + 8.0F, p.scrub.y + 4.0F,
                      std::max(p.scrub.width - 16.0F, 1.0F),
                      p.scrub.height - 8.0F};
  ctx.panel(p.scrub, theme.panel);
  ctx.panel(bar, theme.track);
  if (p.media == nullptr) {
    return;
  }
  // The eight-frame filmstrip spans the whole source.
  if (p.input.director != nullptr &&
      p.input.director->filmstrip.has_value() &&
      p.input.director->filmstrip->state == assets::DerivativeState::Ready &&
      p.input.director->filmstrip->texture != nullptr) {
    ctx.draw_list().add_image_region(
        bar.x, bar.y, bar.width, bar.height,
        *p.input.director->filmstrip->texture, 0.0F, 0.0F, 1.0F, 1.0F,
        gui::UiColor{1.0F, 1.0F, 1.0F, 0.55F});
  }
  const float last = static_cast<float>(std::max(p.count, 1U) - 1U);
  const auto x_of = [&](core::u32 frame) {
    return last <= 0.0F ? bar.x
                        : bar.x + bar.width * static_cast<float>(frame) / last;
  };
  if (mark_in_.has_value() || mark_out_.has_value()) {
    const auto [in, out] = marked_range(p.count);
    const float left = x_of(in);
    const float right = std::max(x_of(out), left + 2.0F);
    ctx.panel(gui::Rect{bar.x, bar.y, left - bar.x, bar.height},
              alpha(theme.background, 0.55F));
    ctx.panel(gui::Rect{right, bar.y, bar.x + bar.width - right, bar.height},
              alpha(theme.background, 0.55F));
    ctx.panel(gui::Rect{left, bar.y + bar.height - 3.0F, right - left, 3.0F},
              theme.accent);
    ctx.panel(gui::Rect{left, bar.y, 2.0F, bar.height}, theme.accent);
    ctx.panel(gui::Rect{right - 2.0F, bar.y, 2.0F, bar.height}, theme.accent);
  }
  const float head = x_of(std::min(frame_, p.count - 1U));
  ctx.panel(gui::Rect{head - 1.0F, p.scrub.y + 1.0F, 2.0F, p.scrub.height - 2.0F},
            theme.playhead);

  const gui::Interaction scrub = ctx.grab(mix(id_, "scrub"), p.scrub);
  if (scrub.pressed) {
    scrubbing_ = true;
    stop();
  }
  if (scrubbing_ && scrub.held) {
    const float t = std::clamp((ctx.pointer_x() - bar.x) / bar.width, 0.0F, 1.0F);
    seek(static_cast<core::u32>(std::lround(t * last)), p.count);
  }
  if (!scrub.held) {
    scrubbing_ = false;
  }
}

void AssetViewerPanel::draw_transport(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect &row = p.transport;
  ctx.panel(row, theme.panel);
  if (p.media == nullptr) {
    return;
  }
  const double now = ctx.time_seconds();
  float x = row.x + 8.0F;
  const float y = row.y + 3.0F;
  const float h = row.height - 6.0F;
  const auto next = [&](float width) {
    const gui::Rect at{x, y, width, h};
    x += width + 4.0F;
    return at;
  };
  const auto [in, out] = marked_range(p.count);
  const bool marked = mark_in_.has_value() || mark_out_.has_value();
  const gui::Rect start = next(30.0F);
  const gui::Rect back = next(26.0F);
  const gui::Rect play_button = next(52.0F);
  const gui::Rect forward = next(26.0F);
  const gui::Rect end = next(30.0F);
  const gui::Rect loop = next(44.0F);
  const gui::Rect in_out = next(66.0F);
  if (ctx.button(mix(id_, "start"), start, "|<")) {
    stop();
    seek(marked ? in : 0U, p.count);
  }
  if (ctx.button(mix(id_, "step-back"), back, "<")) {
    stop();
    seek(frame_ == 0U ? 0U : frame_ - 1U, p.count);
  }
  if (ctx.button(mix(id_, "play"), play_button,
                 direction_ != 0 ? "Pause" : "Play")) {
    if (direction_ != 0) {
      stop();
    } else {
      play(1, now);
    }
  }
  if (ctx.button(mix(id_, "step-forward"), forward, ">")) {
    stop();
    seek(frame_ + 1U, p.count);
  }
  if (ctx.button(mix(id_, "end"), end, ">|")) {
    stop();
    seek(marked ? out : p.count - 1U, p.count);
  }
  if (ctx.button(mix(id_, "loop"), loop, "Loop")) {
    loop_ = !loop_;
  }
  if (loop_) {
    ctx.panel(gui::Rect{loop.x, loop.y + loop.height - 2.0F, loop.width, 2.0F},
              theme.accent);
  }
  if (ctx.button(mix(id_, "in-to-out"), in_out, "In > Out")) {
    seek(in, p.count);
    play(1, now);
    in_to_out_ = true;
  }
  ctx.tooltip(start, "Go to In, or the first frame (Home)");
  ctx.tooltip(back, "Step back a frame (Left)");
  ctx.tooltip(play_button, "Play or pause (Space; J K L shuttle)");
  ctx.tooltip(forward, "Step forward a frame (Right)");
  ctx.tooltip(end, "Go to Out, or the last frame (End)");
  ctx.tooltip(loop, "Loop playback");
  ctx.tooltip(in_out, "Play the marked range");

  std::array<char, 32U> numbers{};
  std::snprintf(numbers.data(), numbers.size(), "  %u / %u", frame_ + 1U,
                p.count);
  const std::string time =
      asset_viewer_timecode(*p.media, p.input.show_rate, frame_) +
      numbers.data();
  ctx.label(gui::Rect{x + 6.0F, row.y, std::max(row.x + row.width - x - 12.0F, 0.0F),
                      row.height},
            time, theme.text);
}

void AssetViewerPanel::draw_edit_row(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect &row = p.edit;
  ctx.panel(row, theme.panel);
  ctx.panel(gui::Rect{row.x, row.y, row.width, 1.0F}, theme.divider);
  if (p.media == nullptr) {
    return;
  }
  float x = row.x + 8.0F;
  const float y = row.y + 3.0F;
  const float h = row.height - 6.0F;
  const auto next = [&](float width) {
    const gui::Rect at{x, y, width, h};
    x += width + 4.0F;
    return at;
  };
  const gui::Rect in_button = next(34.0F);
  const gui::Rect out_button = next(40.0F);
  const gui::Rect clear = next(46.0F);
  if (ctx.button(mix(id_, "mark-in"), in_button, "In")) {
    mark_in_ = frame_;
  }
  if (ctx.button(mix(id_, "mark-out"), out_button, "Out")) {
    mark_out_ = frame_;
  }
  if (ctx.button(mix(id_, "clear"), clear, "Clear")) {
    mark_in_.reset();
    mark_out_.reset();
  }
  ctx.tooltip(in_button, "Mark In at this frame (I)");
  ctx.tooltip(out_button, "Mark Out at this frame (O)");
  ctx.tooltip(clear, "Clear In and Out");

  // From the right: the cuts and where they go.
  float right = row.x + row.width - 8.0F;
  const auto from_right = [&](float width) {
    right -= width;
    const gui::Rect at{right, y, width, h};
    right -= 4.0F;
    return at;
  };
  const gui::Rect append = from_right(62.0F);
  const gui::Rect overwrite = from_right(76.0F);
  const gui::Rect insert = from_right(56.0F);
  if (ctx.button(mix(id_, "insert"), insert, "Insert")) {
    request_edit(p, SourceEditMode::Insert);
  }
  if (ctx.button(mix(id_, "overwrite"), overwrite, "Overwrite")) {
    request_edit(p, SourceEditMode::Overwrite);
  }
  if (ctx.button(mix(id_, "append"), append, "Append")) {
    request_edit(p, SourceEditMode::Append);
  }
  ctx.tooltip(insert, "Insert the range at the playhead on the target track "
                      "(,); later clips move along");
  ctx.tooltip(overwrite, "Overwrite the target track at the playhead with "
                         "the range (.)");
  ctx.tooltip(append, "Add the range after the target track's last clip");

  // Where the cut lands, and the placements of this asset already in the
  // scene on the Stage.
  use_labels_.clear();
  use_clips_.clear();
  std::vector<const ElementInfo *> uses;
  for (const ElementInfo &element : p.input.elements) {
    if (element.kind == ElementKind::Media && element.timing.placed() &&
        element.source == p.media->source) {
      uses.push_back(&element);
    }
  }
  std::sort(uses.begin(), uses.end(),
            [](const ElementInfo *a, const ElementInfo *b) {
              return a->timing.start != b->timing.start
                         ? a->timing.start < b->timing.start
                         : a->node < b->node;
            });
  for (const ElementInfo *use : uses) {
    std::string lane = "No track";
    for (const scene::TimelineLane &candidate : p.input.lanes) {
      if (candidate.id == use->timeline_lane) {
        lane = candidate.name;
        break;
      }
    }
    use_labels_.push_back(
        lane + "   " +
        format_timecode(static_cast<core::u64>(std::max<core::i64>(
                            use->timing.start, 0)),
                        p.input.show_rate.numerator,
                        p.input.show_rate.denominator) +
        "   " + use->display_name);
    use_clips_.push_back(use->node);
  }
  std::string used = "Used " + std::to_string(uses.size());
  const gui::Rect used_button = from_right(ctx.font_width(used) + 16.0F);
  const gui::UiId menu = mix(id_, "used-in");
  if (ctx.button(mix(id_, "used"), used_button, used) && !uses.empty()) {
    ctx.open_context_menu(menu, used_button.x,
                          used_button.y + used_button.height);
  }
  ctx.tooltip(used_button, uses.empty()
                               ? "Not placed in the scene on the Stage"
                               : "Placements in the scene on the Stage");
  if (ctx.menu_open(menu)) {
    std::vector<std::string_view> labels(use_labels_.begin(),
                                         use_labels_.end());
    if (const auto chosen = ctx.context_menu(menu, labels);
        chosen.has_value() && *chosen < use_clips_.size()) {
      p.frame.select_clip = use_clips_[*chosen];
    }
  }

  // The marked range, or why a cut cannot happen, between the two groups.
  std::string text;
  const double now = ctx.time_seconds();
  gui::UiColor color = theme.text_muted;
  if (!message_.empty() && now < message_until_) {
    text = message_;
    color = theme.warning;
  } else {
    const auto [in, out] = marked_range(p.count);
    const core::u64 frames = static_cast<core::u64>(out) - in + 1U;
    if (p.count <= 1U) {
      text = "Still";
    } else {
      text = "In " + asset_viewer_timecode(*p.media, p.input.show_rate, in) +
             "  Out " +
             asset_viewer_timecode(*p.media, p.input.show_rate, out) + "  " +
             duration_text(frames, p.rate);
    }
    text += p.input.target_lane.has_value()
                ? "   to " + std::string{p.input.target_name}
                : std::string{"   no target track"};
  }
  ctx.label(gui::Rect{x + 6.0F, row.y, std::max(right - x - 6.0F, 0.0F),
                      row.height},
            text, color);
}

void AssetViewerPanel::handle_keys(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  if (!focused_ || ctx.typing() || p.media == nullptr) {
    return;
  }
  const double now = ctx.time_seconds();
  const double fps = frames_per_second(p.rate);
  const core::u32 second = static_cast<core::u32>(std::max(std::lround(fps), 1L));
  const auto [in, out] = marked_range(p.count);
  const bool marked = mark_in_.has_value() || mark_out_.has_value();
  bool used = false;
  if (ctx.key_pressed(gui::Key::Left)) {
    stop();
    const core::u32 step = ctx.shift_held() ? second : 1U;
    seek(frame_ > step ? frame_ - step : 0U, p.count);
    used = true;
  } else if (ctx.key_pressed(gui::Key::Right)) {
    stop();
    seek(frame_ + (ctx.shift_held() ? second : 1U), p.count);
    used = true;
  } else if (ctx.key_pressed(gui::Key::Home)) {
    stop();
    seek(marked ? in : 0U, p.count);
    used = true;
  } else if (ctx.key_pressed(gui::Key::End)) {
    stop();
    seek(marked ? out : p.count - 1U, p.count);
    used = true;
  }
  for (const char key : ctx.shortcut_text()) {
    switch (key) {
    case ' ':
      if (direction_ != 0) {
        stop();
      } else {
        play(1, now);
      }
      used = true;
      break;
    case 'j':
    case 'J':
      play(-1, now);
      used = true;
      break;
    case 'k':
    case 'K':
      stop();
      used = true;
      break;
    case 'l':
    case 'L':
      play(1, now);
      used = true;
      break;
    case 'i':
    case 'I':
      mark_in_ = frame_;
      used = true;
      break;
    case 'o':
    case 'O':
      mark_out_ = frame_;
      used = true;
      break;
    case ',':
      request_edit(p, SourceEditMode::Insert);
      used = true;
      break;
    case '.':
      request_edit(p, SourceEditMode::Overwrite);
      used = true;
      break;
    case 'f':
    case 'F':
      framing_ = {};
      used = true;
      break;
    default:
      break;
    }
  }
  p.frame.consumed_keys = used;
}

AssetViewerFrame AssetViewerPanel::draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        const AssetViewerDrawInput &input) {
  Pass p{ctx, bounds, input};
  if (ctx.pointer_pressed() || ctx.middle_pointer_pressed() ||
      ctx.secondary_pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_press_x(), ctx.pointer_press_y());
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return p.frame;
  }
  p.ppu = std::max(input.pixels_per_unit, 0.25F);
  if (input.assets != nullptr && asset_ != doc::NodeId{}) {
    p.asset = input.assets->asset(asset_);
    p.media = p.asset != nullptr ? std::get_if<assets::MediaAsset>(p.asset)
                                 : nullptr;
  }
  if (p.media != nullptr) {
    p.count = asset_viewer_frame_count(*p.media);
    p.rate = asset_viewer_rate(*p.media, input.show_rate);
    frame_ = std::min(frame_, p.count - 1U);
  } else {
    stop();
  }

  ctx.panel(bounds, ctx.theme().panel);
  ctx.push_clip(bounds);
  const float bottom = scrub_height + transport_height + edit_height;
  p.header = gui::Rect{bounds.x, bounds.y, bounds.width, header_height};
  p.image = gui::Rect{bounds.x, bounds.y + header_height, bounds.width,
                      std::max(bounds.height - header_height - bottom, 0.0F)};
  p.scrub = gui::Rect{bounds.x, p.image.y + p.image.height, bounds.width,
                      scrub_height};
  p.transport = gui::Rect{bounds.x, p.scrub.y + scrub_height, bounds.width,
                          transport_height};
  p.edit = gui::Rect{bounds.x, p.transport.y + transport_height, bounds.width,
                     edit_height};

  handle_keys(p);
  advance(p);
  draw_image(p);
  draw_scrub(p);
  draw_transport(p);
  draw_edit_row(p);
  // The header last, so its menus draw above the picture.
  draw_header(p);
  ctx.pop_clip();

  if (p.media != nullptr) {
    p.frame.request = AssetViewerRequest{asset_, frame_};
  }
  return p.frame;
}

void AssetViewerPanel::draw_drag_ghost(gui::UiContext &ctx,
                                       const rhi::Texture *image) const {
  if (!dragging_) {
    return;
  }
  const float width = 132.0F;
  const float height = image != nullptr && image->width() != 0U
                           ? width * static_cast<float>(image->height()) /
                                 static_cast<float>(image->width())
                           : 74.0F;
  const gui::Rect ghost{ctx.pointer_x() + 14.0F, ctx.pointer_y() + 10.0F,
                        width, std::clamp(height, 24.0F, 132.0F)};
  ctx.panel(ghost, alpha(ctx.theme().background, 0.85F));
  if (image != nullptr) {
    ctx.image(ghost, *image, gui::UiColor{1.0F, 1.0F, 1.0F, 0.82F});
  }
  ctx.panel(gui::Rect{ghost.x, ghost.y, ghost.width, 1.0F}, ctx.theme().accent);
  ctx.panel(gui::Rect{ghost.x, ghost.y + ghost.height - 1.0F, ghost.width, 1.0F},
            ctx.theme().accent);
}

} // namespace mediaserver::editor
