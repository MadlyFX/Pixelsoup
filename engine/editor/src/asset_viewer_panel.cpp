#include <mediaserver/editor/asset_viewer_panel.hpp>

#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/rect_cut.hpp>
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

// A viewer's header strip: controls laid in from the right while the title
// keeps 160px, then the title and status chips from the left in what is left.
class HeaderBar final {
public:
  HeaderBar(gui::UiContext &ctx, const gui::Rect &bar)
      : ctx_(ctx), bar_(bar),
        controls_(gui::inset(bar, gui::Padding{0.0F, 3.0F, 4.0F, 3.0F})) {
    ctx.panel(bar, ctx.theme().header);
    gui::Rect rule = bar;
    ctx.panel(gui::cut_bottom(rule, 1.0F), ctx.theme().divider);
  }

  [[nodiscard]] bool room(float width) const noexcept {
    return controls_.width - width > 160.0F;
  }
  // The next control from the right, `gap` short of the one after it; a
  // group of controls that touch passes zero.
  [[nodiscard]] gui::Rect place(float width, float gap = 4.0F) noexcept {
    const gui::Rect at = gui::cut_right(controls_, width);
    static_cast<void>(gui::cut_right(controls_, gap));
    return at;
  }
  // The title, left of the controls, as wide as it needs; chips follow it.
  gui::Rect title(std::string_view text) {
    gui::Rect lane = bar_;
    lane.width = std::max(controls_.right() - bar_.x, 0.0F);
    gui::Rect title_lane =
        gui::inset(lane, gui::Padding{8.0F, 0.0F, 4.0F, 0.0F});
    const gui::Rect rect =
        gui::cut_left(title_lane, ctx_.font_width(text) + 4.0F);
    ctx_.label(rect, text, ctx_.theme().text);
    chips_ = gui::inset(
        lane, gui::Padding{rect.right() - bar_.x + 8.0F, 6.0F, 0.0F, 6.0F});
    return rect;
  }
  // A tinted status chip after the title, dropped when it would not fit.
  void chip(std::string_view text, gui::UiColor color, std::string_view tip) {
    const float width = ctx_.font_width(text) + 12.0F;
    if (text.empty() || width > chips_.width) {
      return;
    }
    const gui::Rect rect = gui::cut_left(chips_, width);
    static_cast<void>(gui::cut_left(chips_, 6.0F));
    ctx_.panel(rect, alpha(color, 0.18F));
    ctx_.label(gui::inset(rect, gui::Padding{6.0F, 0.0F, 0.0F, 0.0F}), text,
               color);
    ctx_.tooltip(rect, tip);
  }

private:
  gui::UiContext &ctx_;
  gui::Rect bar_;
  gui::Rect controls_;
  gui::Rect chips_;
};

} // namespace

struct AssetViewerPanel::Pass final {
  gui::UiContext &ctx;
  const gui::Rect &bounds_rect;
  const AssetViewerDrawInput &input;
  AssetViewerFrame frame{};
  const assets::Asset *asset{};
  const assets::MediaAsset *media{};
  const assets::ModelAsset *model{};
  // The model's box, in model mode.
  WorldBounds bounds{};
  gui::Rect tools{};
  gui::Rect info{};
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
    model_fresh_ = true;
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
  model_fresh_ = true;
  look_ = AssetViewerLookDev{};
  turntable_ = false;
  part_.reset();
  clip_.reset();
  clip_playing_ = false;
  model_drag_ = ModelDrag::None;
  checked_facts_.reset();
  checks_.clear();
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
    show_message("The target track is locked", now);
    return;
  }
  const auto [in, out] = marked_range(p.count);
  p.frame.source_edit = AssetViewerSourceEdit{mode, in, out - in + 1U};
}

void AssetViewerPanel::history_buttons(gui::UiContext &ctx,
                                       const gui::Rect &back,
                                       const gui::Rect &forward) {
  if (ctx.button(mix(id_, "back"), back, "<") && history_index_ > 0U &&
      !history_.empty()) {
    --history_index_;
    const doc::NodeId wanted = history_[history_index_];
    asset_ = doc::NodeId{};
    open(wanted);
    // open() already reached this entry; keep the forward history.
  }
  if (ctx.button(mix(id_, "forward"), forward, ">") &&
      history_index_ + 1U < history_.size()) {
    ++history_index_;
    const doc::NodeId wanted = history_[history_index_];
    asset_ = doc::NodeId{};
    open(wanted);
  }
  ctx.tooltip(back, "Previous asset");
  ctx.tooltip(forward, "Next asset");
}

void AssetViewerPanel::draw_header(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  // Controls from the right, each only while there is room for it and the
  // name to its left.
  HeaderBar bar{ctx, p.header};
  if (p.media != nullptr && bar.room(126.0F)) {
    const gui::Rect plus = bar.place(22.0F, 0.0F);
    std::array<char, 24U> stops{};
    std::snprintf(stops.data(), stops.size(), "EV %+.1f", exposure_);
    const gui::Rect value = bar.place(56.0F, 0.0F);
    const gui::Rect minus = bar.place(22.0F);
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
  if (p.media != nullptr && bar.room(78.0F)) {
    static constexpr std::array<std::string_view, 5U> channels{
        "RGB", "Red", "Green", "Blue", "Luma"};
    std::size_t selected = static_cast<std::size_t>(channel_);
    if (ctx.dropdown(mix(id_, "channel"), bar.place(78.0F), channels,
                     selected)) {
      channel_ = static_cast<AssetViewerChannel>(
          std::min<std::size_t>(selected, channels.size() - 1U));
    }
  }
  if (p.media != nullptr && bar.room(140.0F)) {
    const gui::Rect actual = bar.place(46.0F, 0.0F);
    const gui::Rect fit = bar.place(40.0F, 0.0F);
    const gui::Rect zoom_text = bar.place(46.0F);
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
  if (bar.room(52.0F)) {
    const gui::Rect forward = bar.place(24.0F, 0.0F);
    history_buttons(ctx, bar.place(24.0F), forward);
  }

  std::string title = p.asset != nullptr
                          ? std::string{assets::asset_display_name(*p.asset)}
                          : std::string{"Asset Viewer"};
  const gui::Rect title_rect = bar.title(title);
  if (p.asset != nullptr) {
    ctx.tooltip(title_rect, assets::asset_source(*p.asset));
  }

  // The file the picture is from, and whether it is still arriving.
  const auto chip = [&](std::string_view text, gui::UiColor color,
                        std::string_view tip) { bar.chip(text, color, tip); };
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
  const gui::Rect readout{area.x, picture_area.bottom(),
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
    centred("The Asset Viewer cannot show this kind of asset",
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
    if (dragging_ && !p.bounds_rect.contains(ctx.pointer_release_x(),
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
    ctx.panel(gui::Rect{right, bar.y, bar.right() - right, bar.height},
              alpha(theme.background, 0.55F));
    ctx.panel(gui::Rect{left, bar.bottom() - 3.0F, right - left, 3.0F},
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
    ctx.panel(gui::Rect{loop.x, loop.bottom() - 2.0F, loop.width, 2.0F},
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
  ctx.label(gui::Rect{x + 6.0F, row.y, std::max(row.right() - x - 12.0F, 0.0F),
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
  float right = row.right() - 8.0F;
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
  ctx.tooltip(overwrite, "Lay the range over the target track at the "
                         "playhead, in front of its clips (.); nothing moves");
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
                          used_button.bottom());
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
    if (!p.input.target_lane.has_value()) {
      text += "   target track locked";
    } else if (*p.input.target_lane == doc::NodeId{}) {
      text += "   outside every track";
    } else {
      text += "   to " + std::string{p.input.target_name};
    }
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
    p.model = p.asset != nullptr ? std::get_if<assets::ModelAsset>(p.asset)
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
  if (p.model != nullptr && input.model != nullptr) {
    draw_model(p);
    ctx.pop_clip();
    return p.frame;
  }
  const float bottom = scrub_height + transport_height + edit_height;
  p.header = gui::Rect{bounds.x, bounds.y, bounds.width, header_height};
  p.image = gui::Rect{bounds.x, bounds.y + header_height, bounds.width,
                      std::max(bounds.height - header_height - bottom, 0.0F)};
  p.scrub = gui::Rect{bounds.x, p.image.bottom(), bounds.width,
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

namespace {

constexpr float tools_height = 30.0F;
constexpr float tab_height = 26.0F;
constexpr float list_row_height = 22.0F;

[[nodiscard]] std::string_view shading_text(render::ViewShading shading) noexcept {
  switch (shading) {
  case render::ViewShading::Lit:
    return "Lit";
  case render::ViewShading::Unlit:
    return "Unlit";
  case render::ViewShading::LightsOnly:
    return "Lights only";
  case render::ViewShading::Wireframe:
    return "Wireframe";
  case render::ViewShading::UvChecker:
    return "UV checker";
  case render::ViewShading::WorldNormals:
    return "World normals";
  }
  return "Lit";
}

[[nodiscard]] std::string length_text(float metres) {
  std::array<char, 32U> text{};
  // Below a tenth of a millimetre is nothing, and never "-0.0 mm".
  if (std::abs(metres) < 0.0001F) {
    metres = 0.0F;
  }
  const float size = std::abs(metres);
  if (size >= 1.0F || size == 0.0F) {
    std::snprintf(text.data(), text.size(), "%.2f m", static_cast<double>(metres));
  } else if (size >= 0.01F) {
    std::snprintf(text.data(), text.size(), "%.1f cm",
                  static_cast<double>(metres) * 100.0);
  } else {
    std::snprintf(text.data(), text.size(), "%.1f mm",
                  static_cast<double>(metres) * 1000.0);
  }
  return text.data();
}

// 1234567 as "1,234,567".
[[nodiscard]] std::string grouped(core::u64 value) {
  std::string digits = std::to_string(value);
  for (std::size_t at = digits.size(); at > 3U; at -= 3U) {
    digits.insert(at - 3U, ",");
  }
  return digits;
}

[[nodiscard]] std::string counted(core::u64 count, std::string_view one,
                                  std::string_view many) {
  return grouped(count) + " " + std::string{count == 1U ? one : many};
}

[[nodiscard]] std::string_view encoding_text(render::ModelTextureEncoding encoding) noexcept {
  switch (encoding) {
  case render::ModelTextureEncoding::Bc7:
    return "BC7";
  case render::ModelTextureEncoding::Bc5:
    return "BC5";
  case render::ModelTextureEncoding::Rgba8:
    break;
  }
  return "RGBA8";
}

// A scene-linear colour roughly as the screen shows it, for a swatch.
[[nodiscard]] gui::UiColor swatch(scene::Vec3 linear) noexcept {
  const auto encode = [](float value) {
    return std::pow(std::clamp(value, 0.0F, 1.0F), 1.0F / 2.2F);
  };
  return gui::UiColor{encode(linear.x), encode(linear.y), encode(linear.z), 1.0F};
}

[[nodiscard]] std::string seconds_text(double seconds) {
  std::array<char, 24U> text{};
  std::snprintf(text.data(), text.size(), "%.2f s", seconds);
  return text.data();
}

} // namespace

void AssetViewerPanel::set_hdri(std::string path) {
  if (path.empty()) {
    return;
  }
  look_.hdri_source = std::move(path);
  look_.environment = AssetViewerEnvironment::Hdri;
}

void AssetViewerPanel::play_clip(double now) noexcept {
  clip_playing_ = true;
  clip_origin_time_ = now;
  clip_origin_frame_ = clip_frame_;
}

void AssetViewerPanel::advance_model(Pass &p) {
  const double now = p.ctx.time_seconds();
  const double elapsed =
      last_time_.has_value() ? std::clamp(now - *last_time_, 0.0, 0.25) : 0.0;
  last_time_ = now;
  if (turntable_ && turntable_period_ > 0.0F) {
    const float turned =
        turntable_angle_ + static_cast<float>(elapsed / turntable_period_);
    turntable_angle_ = turned - std::floor(turned);
  }
  const render::ModelPackageFacts *const facts = p.input.model->facts.get();
  if (!clip_playing_ || !clip_.has_value() || facts == nullptr ||
      *clip_ >= facts->clips.size()) {
    return;
  }
  const double rate = static_cast<double>(asset_viewer_animation_rate);
  core::u64 frame = clip_origin_frame_ +
                    static_cast<core::u64>(std::floor(
                        std::max(now - clip_origin_time_, 0.0) * rate));
  const auto last = static_cast<core::u64>(std::ceil(
      static_cast<double>(std::max(facts->clips[*clip_].duration, 0.0F)) * rate));
  if (!clip_loop_ && frame >= last) {
    frame = last;
    clip_playing_ = false;
  }
  clip_frame_ = frame;
}

void AssetViewerPanel::handle_model_keys(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  if (!focused_ || ctx.typing()) {
    return;
  }
  bool used = false;
  const render::ModelPackageFacts *const facts = p.input.model->facts.get();
  const bool animating = clip_.has_value() && facts != nullptr &&
                         *clip_ < facts->clips.size();
  if (ctx.key_pressed(gui::Key::Home)) {
    orbit_ = asset_viewer_model_home(p.bounds);
    used = true;
  } else if (animating && (ctx.key_pressed(gui::Key::Left) ||
                           ctx.key_pressed(gui::Key::Right))) {
    clip_playing_ = false;
    const core::u64 step =
        ctx.shift_held() ? asset_viewer_animation_rate : 1U;
    if (ctx.key_pressed(gui::Key::Left)) {
      clip_frame_ = clip_frame_ > step ? clip_frame_ - step : 0U;
    } else {
      clip_frame_ += step;
    }
    used = true;
  }
  for (const char key : ctx.shortcut_text()) {
    switch (key) {
    case 'f':
    case 'F':
      orbit_ = stage_focus(orbit_, p.bounds);
      navigated_ = true;
      used = true;
      break;
    case 't':
    case 'T':
      turntable_ = !turntable_;
      used = true;
      break;
    case ' ':
      if (animating) {
        if (clip_playing_) {
          clip_playing_ = false;
        } else {
          play_clip(ctx.time_seconds());
        }
        used = true;
      }
      break;
    default:
      break;
    }
  }
  p.frame.consumed_keys = used;
}

void AssetViewerPanel::draw_model_header(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const AssetViewerModelInput &model = *p.input.model;
  HeaderBar bar{ctx, p.header};
  if (bar.room(112.0F)) {
    static constexpr std::array<std::string_view, 6U> shadings{
        "Lit", "Unlit", "Lights only", "Wireframe", "UV checker",
        "World normals"};
    std::size_t selected = static_cast<std::size_t>(shading_);
    const gui::Rect at = bar.place(112.0F);
    if (ctx.dropdown(mix(id_, "shading"), at, shadings, selected)) {
      shading_ = static_cast<render::ViewShading>(
          std::min<std::size_t>(selected, shadings.size() - 1U));
    }
    ctx.tooltip(at, "How the model is shaded here. UV checker shows its "
                    "texture mapping; World normals its facing.");
  }
  if (bar.room(120.0F)) {
    static constexpr std::array<std::string_view, 3U> environments{
        "Studio", "Stage scene", "HDRI image..."};
    std::size_t selected = static_cast<std::size_t>(look_.environment);
    const gui::Rect at = bar.place(120.0F);
    if (ctx.dropdown(mix(id_, "environment"), at, environments, selected)) {
      if (selected == 2U) {
        p.frame.choose_hdri = true;
      } else {
        look_.environment = static_cast<AssetViewerEnvironment>(selected);
      }
    }
    ctx.tooltip(at, look_.environment == AssetViewerEnvironment::Hdri
                        ? std::string_view{look_.hdri_source}
                        : std::string_view{
                              "What lights the model. Only the viewer changes; "
                              "nothing is authored."});
  }
  if (bar.room(52.0F)) {
    const gui::Rect forward = bar.place(24.0F, 0.0F);
    history_buttons(ctx, bar.place(24.0F), forward);
  }

  const std::string title{assets::asset_display_name(*p.asset)};
  ctx.tooltip(bar.title(title), assets::asset_source(*p.asset));
  const auto chip = [&](std::string_view text, gui::UiColor color,
                        std::string_view tip) { bar.chip(text, color, tip); };
  if (p.model->projection_mesh) {
    chip("PROJECTION MESH", theme.text_muted,
         "A projection mesh: its UV0 is the mapping the wall is calibrated "
         "with, so it opens in UV checker shading");
  }
  if (model.state == AssetViewerModelState::Failed) {
    chip("DID NOT LOAD", theme.warning, model.diagnostic);
  } else if (p.input.load_failed) {
    chip("LOAD FAILED", theme.warning, p.input.loading_detail);
  } else if (model.state == AssetViewerModelState::Loading ||
             p.input.decoding) {
    chip("LOADING", theme.playhead,
         p.input.loading_detail.empty()
             ? std::string_view{"Waiting for the model's geometry or textures"}
             : p.input.loading_detail);
  }
  if (look_.environment == AssetViewerEnvironment::Hdri && !model.advanced) {
    chip("HDRI NEEDS ADVANCED", theme.warning,
         "An environment image lights a scene only in the Advanced renderer "
         "(Project Settings > Renderer)");
  }
}

void AssetViewerPanel::draw_model_image(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const AssetViewerModelInput &model = *p.input.model;
  const gui::Rect area = p.image;
  ctx.panel(area, theme.background);
  if (area.width < 8.0F || area.height < readout_height + 8.0F) {
    return;
  }
  const gui::Rect picture_area{area.x, area.y, area.width,
                               area.height - readout_height};
  const gui::Rect readout{area.x, picture_area.bottom(),
                          area.width, readout_height};
  ctx.panel(readout, theme.panel);
  p.frame.surface_width = static_cast<core::u32>(
      std::max(std::lround(picture_area.width * p.ppu), 1L));
  p.frame.surface_height = static_cast<core::u32>(
      std::max(std::lround(picture_area.height * p.ppu), 1L));

  const auto centred = [&](std::string_view text, gui::UiColor color) {
    const float width =
        std::min(ctx.font_width(text), picture_area.width - 16.0F);
    ctx.label(gui::Rect{picture_area.x + (picture_area.width - width) * 0.5F,
                        picture_area.y + picture_area.height * 0.5F - 12.0F,
                        std::max(width, 0.0F), 24.0F},
              text, color);
  };
  if (p.input.image != nullptr && p.input.image->width() != 0U) {
    ctx.image(picture_area, *p.input.image);
  } else if (model.state == AssetViewerModelState::Failed) {
    centred("This model did not load", theme.warning);
  } else if (!p.input.image_fault.empty()) {
    centred(p.input.image_fault, theme.warning);
  } else {
    centred("Loading the model", theme.text_muted);
  }

  // Left drag orbits about the pivot, Shift+left turns the lights, middle
  // drag pans, right drag and the wheel dolly.
  const bool hovered =
      picture_area.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !ctx.pointer_blocked();
  if (hovered && ctx.wheel() != 0.0F) {
    orbit_ = stage_wheel_dolly(orbit_, ctx.wheel());
    navigated_ = true;
  }
  const gui::Interaction grab = ctx.grab(mix(id_, "model-view"), picture_area);
  if (grab.pressed) {
    model_drag_ = ctx.shift_held() ? ModelDrag::Rig : ModelDrag::Orbit;
  } else if (hovered && ctx.middle_pointer_pressed()) {
    model_drag_ = ModelDrag::Track;
  } else if (hovered && ctx.secondary_pointer_pressed()) {
    model_drag_ = ModelDrag::Dolly;
  }
  const bool held = (model_drag_ == ModelDrag::Orbit ||
                     model_drag_ == ModelDrag::Rig)
                        ? grab.held && !grab.released
                    : model_drag_ == ModelDrag::Track ? ctx.middle_pointer_down()
                    : model_drag_ == ModelDrag::Dolly
                        ? ctx.secondary_pointer_down()
                        : false;
  if (!held) {
    model_drag_ = ModelDrag::None;
  }
  const float dx = ctx.pointer_dx();
  const float dy = ctx.pointer_dy();
  if (model_drag_ != ModelDrag::None && (dx != 0.0F || dy != 0.0F)) {
    StageNavigationStep step{};
    step.dx = dx;
    step.dy = dy;
    step.units_per_pixel = stage_units_per_pixel(orbit_, picture_area.height);
    switch (model_drag_) {
    case ModelDrag::Orbit:
      step.gesture = StageNavigation::Orbit;
      break;
    case ModelDrag::Track:
      step.gesture = StageNavigation::Track;
      break;
    case ModelDrag::Dolly:
      step.gesture = StageNavigation::Dolly;
      break;
    case ModelDrag::Rig: {
      const float turned = look_.rig_yaw + dx * stage_turn_per_pixel;
      look_.rig_yaw = turned - std::floor(turned);
      break;
    }
    case ModelDrag::None:
      break;
    }
    if (step.gesture != StageNavigation::None) {
      orbit_ = navigate_stage(orbit_, step);
      navigated_ = true;
    }
  }
  if (model_drag_ != ModelDrag::None) {
    ctx.set_cursor(gui::Cursor::Move);
  }

  // The model's size and where its pivot sits.
  const AssetViewerDimensions size = asset_viewer_dimensions(p.bounds);
  std::array<char, 160U> text{};
  std::snprintf(text.data(), text.size(),
                "W %s   D %s   H %s     Pivot %s, %s, %s from the base centre",
                length_text(size.width).c_str(), length_text(size.depth).c_str(),
                length_text(size.height).c_str(),
                length_text(size.pivot_offset.x).c_str(),
                length_text(size.pivot_offset.y).c_str(),
                length_text(size.pivot_offset.z).c_str());
  ctx.label(gui::Rect{readout.x + 8.0F, readout.y, readout.width - 16.0F,
                      readout.height},
            text.data(), theme.text_muted);
  ctx.tooltip(readout,
              model.bounds.has_value()
                  ? "Width (x), depth (z) and height (y) of the package, in "
                    "metres, and its origin relative to the middle of its base"
                  : "From the Library's import; the loaded package refines it");
}

void AssetViewerPanel::draw_model_tools(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect &row = p.tools;
  ctx.panel(row, theme.panel);
  ctx.panel(gui::Rect{row.x, row.y, row.width, 1.0F}, theme.divider);
  float x = row.x + 8.0F;
  const float y = row.y + 3.0F;
  const float h = row.height - 6.0F;
  const auto next = [&](float width) {
    const gui::Rect at{x, y, width, h};
    x += width + 4.0F;
    return at;
  };
  const auto lit = [&](const gui::Rect &at, bool on) {
    if (on) {
      ctx.panel(gui::Rect{at.x, at.bottom() - 2.0F, at.width, 2.0F},
                theme.accent);
    }
  };
  const gui::Rect frame = next(50.0F);
  const gui::Rect home = next(50.0F);
  x += 8.0F;
  const gui::Rect turntable = next(74.0F);
  const gui::Rect period = next(62.0F);
  x += 8.0F;
  const gui::Rect floor = next(48.0F);
  const gui::Rect figure = next(54.0F);
  if (ctx.button(mix(id_, "frame"), frame, "Frame")) {
    orbit_ = stage_focus(orbit_, p.bounds);
    navigated_ = true;
  }
  if (ctx.button(mix(id_, "home"), home, "Home")) {
    orbit_ = asset_viewer_model_home(p.bounds);
  }
  if (ctx.button(mix(id_, "turntable"), turntable, "Turntable")) {
    turntable_ = !turntable_;
  }
  lit(turntable, turntable_);
  static constexpr std::array<std::string_view, 3U> periods{"10 s", "20 s",
                                                            "40 s"};
  static constexpr std::array<float, 3U> period_seconds{10.0F, 20.0F, 40.0F};
  std::size_t chosen = 1U;
  for (std::size_t index = 0U; index < period_seconds.size(); ++index) {
    if (turntable_period_ == period_seconds[index]) {
      chosen = index;
    }
  }
  if (ctx.dropdown(mix(id_, "period"), period, periods, chosen)) {
    turntable_period_ =
        period_seconds[std::min(chosen, period_seconds.size() - 1U)];
  }
  if (ctx.button(mix(id_, "floor"), floor, "Floor")) {
    look_.floor = !look_.floor;
  }
  lit(floor, look_.floor);
  if (ctx.button(mix(id_, "figure"), figure, "Figure")) {
    look_.figure = !look_.figure;
  }
  lit(figure, look_.figure);
  ctx.tooltip(frame, "Frame the whole model (F)");
  ctx.tooltip(home, "Back to the three-quarter view it opened in (Home)");
  ctx.tooltip(turntable, "Turn the model about its pivot (T)");
  ctx.tooltip(period, "How long one turn takes");
  ctx.tooltip(floor, "The floor and its grid, ruled in metres");
  ctx.tooltip(figure, "A 1.8 m figure beside the model, for scale");

  if (part_.has_value()) {
    const gui::Rect all{row.right() - 78.0F, y, 70.0F, h};
    if (all.x > x + 90.0F) {
      if (ctx.button(mix(id_, "all-parts"), all, "Show all")) {
        part_.reset();
      }
      ctx.label(gui::Rect{all.x - 90.0F, row.y, 86.0F, row.height},
                "Part " + std::to_string(*part_ + 1U) + " only",
                theme.playhead);
    }
  }
}

void AssetViewerPanel::draw_model_details(Pass &p, gui::FormLayout &rows) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const AssetViewerModelInput &model = *p.input.model;
  const auto line = [&](std::string_view text, gui::UiColor color) {
    ctx.label(rows.caption(), text, color);
  };
  const AssetViewerDimensions size = asset_viewer_dimensions(p.bounds);
  line(length_text(size.width) + " wide, " + length_text(size.depth) +
           " deep, " + length_text(size.height) + " tall",
       theme.text);
  if ((p.input.decoding || p.input.load_failed) &&
      !p.input.loading_detail.empty()) {
    line(p.input.loading_detail,
         p.input.load_failed ? theme.warning : theme.playhead);
  }
  if (model.state == AssetViewerModelState::Failed) {
    line("It did not load: " + std::string{model.diagnostic}, theme.warning);
    return;
  }
  const render::ModelPackageFacts *const facts = model.facts.get();
  if (facts == nullptr) {
    line(model.state == AssetViewerModelState::Loading
             ? "Loading the package"
             : "Checking the package",
         theme.text_muted);
    return;
  }
  // Materials as the Library counted them at import; the package keeps
  // one part per material run, not the source's material list.
  std::string counts = counted(facts->vertices, "vertex", "vertices") + ", " +
                       counted(facts->triangles, "triangle", "triangles") +
                       ", " + counted(facts->parts.size(), "part", "parts");
  if (p.model->material_count.has_value()) {
    counts += ", " + counted(*p.model->material_count, "material", "materials");
  }
  line(counts, theme.text_muted);
  std::array<char, 48U> memory{};
  std::snprintf(memory.data(), memory.size(), "%.0f MiB",
                static_cast<double>(facts->texture_bytes) / (1024.0 * 1024.0));
  line(counted(facts->textures, "texture", "textures") + " (" + memory.data() +
           " at full resolution), " +
           counted(facts->lods, "level of detail", "levels of detail") + ", " +
           counted(facts->clips.size(), "animation clip", "animation clips"),
       theme.text_muted);
  rows.space(4.0F);
  if (checks_.empty()) {
    line("No problems found", theme.good);
  }
  for (const render::ModelCheck &check : checks_) {
    const bool warning = check.level == render::ModelCheckLevel::Warning;
    const gui::Rect at = rows.caption();
    ctx.label(gui::Rect{at.x, at.y, 14.0F, at.height}, warning ? "!" : "i",
              warning ? theme.warning : theme.text_muted);
    ctx.label(gui::Rect{at.x + 16.0F, at.y, at.width - 16.0F, at.height},
              check.text, warning ? theme.text : theme.text_muted);
    ctx.tooltip(at, check.text);
  }
}

void AssetViewerPanel::draw_model_parts(Pass &p, gui::FormLayout &rows) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const render::ModelPackageFacts *const facts = p.input.model->facts.get();
  if (facts == nullptr) {
    ctx.label(rows.caption(), "Waiting for the package", theme.text_muted);
    return;
  }
  for (std::size_t index = 0U; index < facts->parts.size(); ++index) {
    const render::ModelPartFacts &part = facts->parts[index];
    const gui::Rect at = rows.next(list_row_height, 1.0F);
    const bool chosen = part_.has_value() && *part_ == index;
    const bool visible = at.bottom() > p.info.y && at.y < p.info.bottom();
    if (chosen) {
      ctx.panel(at, theme.selection);
    } else if (index % 2U == 1U) {
      ctx.panel(at, theme.row);
    }
    const gui::UiColor colour = part.has_material
                                    ? swatch(part.material.base_color)
                                    : theme.text_muted;
    ctx.panel(gui::Rect{at.x + 4.0F, at.y + 4.0F, 14.0F, at.height - 8.0F},
              colour);
    std::array<char, 96U> factors{};
    std::snprintf(factors.data(), factors.size(), "rough %.2f  metal %.2f",
                  static_cast<double>(part.material.roughness),
                  static_cast<double>(part.material.metallic));
    std::string text = "Part " + std::to_string(index + 1U) + "   " +
                       counted(part.triangles, "tri", "tris") + "   " +
                       (part.has_material ? std::string{factors.data()}
                                          : std::string{"no material"});
    std::string textures;
    for (const render::ModelPartTexture &texture : part.textures) {
      textures += textures.empty() ? "" : ",  ";
      textures += std::string{render::model_texture_role_name(texture.role)} +
                  " " + std::to_string(texture.width) + "x" +
                  std::to_string(texture.height) + " " +
                  std::string{encoding_text(texture.encoding)};
    }
    if (!textures.empty()) {
      text += "   " + textures;
    }
    ctx.label(gui::Rect{at.x + 24.0F, at.y, at.width - 28.0F, at.height}, text,
              part.missing_uv || part.missing_tangent ? theme.warning
                                                      : theme.text);
    if (visible) {
      const gui::Interaction row = ctx.grab(
          mix(mix(id_, "part"), std::to_string(index)), at);
      if (row.pressed) {
        if (chosen) {
          part_.reset();
        } else {
          part_ = static_cast<core::u32>(index);
        }
      }
      ctx.tooltip(at, textures.empty()
                          ? std::string_view{"Click to show this part alone"}
                          : std::string_view{textures});
    }
  }
}

void AssetViewerPanel::draw_model_animation(Pass &p, gui::FormLayout &rows) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const render::ModelPackageFacts *const facts = p.input.model->facts.get();
  if (facts == nullptr) {
    ctx.label(rows.caption(), "Waiting for the package", theme.text_muted);
    return;
  }
  if (facts->clips.empty()) {
    ctx.label(rows.caption(), "This model has no animation clips",
              theme.text_muted);
    return;
  }
  const double now = ctx.time_seconds();
  const double rate = static_cast<double>(asset_viewer_animation_rate);
  // The transport for the chosen clip.
  {
    const gui::Rect at = rows.next(p.ctx.theme().metrics.control, 6.0F);
    const gui::Rect play{at.x, at.y, 56.0F, at.height};
    const gui::Rect loop{play.right() + 4.0F, at.y, 48.0F, at.height};
    const bool chosen = clip_.has_value() && *clip_ < facts->clips.size();
    if (ctx.button(mix(id_, "clip-play"), play,
                   clip_playing_ ? "Pause" : "Play")) {
      if (!chosen) {
        clip_ = 0U;
        clip_frame_ = 0U;
      }
      if (clip_playing_) {
        clip_playing_ = false;
      } else {
        play_clip(now);
      }
    }
    if (ctx.button(mix(id_, "clip-loop"), loop, "Loop")) {
      clip_loop_ = !clip_loop_;
      clip_origin_time_ = now;
      clip_origin_frame_ = clip_frame_;
    }
    if (clip_loop_) {
      ctx.panel(gui::Rect{loop.x, loop.bottom() - 2.0F, loop.width, 2.0F},
                theme.accent);
    }
    ctx.tooltip(play, "Play or pause the clip (Space)");
    ctx.tooltip(loop, "Loop the clip");
    if (chosen) {
      const double duration =
          std::max(static_cast<double>(facts->clips[*clip_].duration), 0.0);
      double shown = static_cast<double>(clip_frame_) / rate;
      if (clip_loop_ && duration > 0.0) {
        shown = std::fmod(shown, duration);
      }
      shown = std::min(shown, duration);
      const std::string time = seconds_text(shown) + " / " + seconds_text(duration);
      const float time_width = ctx.font_width(time) + 12.0F;
      const gui::Rect bar{loop.right() + 10.0F, at.y + 6.0F,
                          std::max(at.right() - time_width -
                                       (loop.right() + 10.0F),
                                   10.0F),
                          at.height - 12.0F};
      ctx.panel(bar, theme.track);
      if (duration > 0.0) {
        const float head = bar.x + bar.width * static_cast<float>(shown / duration);
        ctx.panel(gui::Rect{head - 1.0F, bar.y - 3.0F, 2.0F, bar.height + 6.0F},
                  theme.playhead);
        const gui::Interaction scrub = ctx.grab(mix(id_, "clip-scrub"), bar);
        if (scrub.held) {
          clip_playing_ = false;
          const double t = std::clamp(
              static_cast<double>((ctx.pointer_x() - bar.x) / bar.width), 0.0, 1.0);
          clip_frame_ = static_cast<core::u64>(std::llround(t * duration * rate));
        }
      }
      ctx.label(gui::Rect{bar.right() + 8.0F, at.y, time_width, at.height},
                time, theme.text_muted);
    }
  }
  const auto row = [&](std::size_t index, std::string_view name,
                       std::string_view detail, bool chosen) {
    const gui::Rect at = rows.next(list_row_height, 1.0F);
    if (chosen) {
      ctx.panel(at, theme.selection);
    } else if (index % 2U == 1U) {
      ctx.panel(at, theme.row);
    }
    ctx.label(gui::Rect{at.x + 8.0F, at.y, at.width * 0.7F, at.height}, name,
              theme.text);
    const float detail_width = ctx.font_width(detail) + 8.0F;
    ctx.label(gui::Rect{at.right() - detail_width, at.y, detail_width,
                        at.height},
              detail, theme.text_muted);
    const bool visible =
        at.bottom() > p.info.y && at.y < p.info.bottom();
    return visible &&
           ctx.grab(mix(mix(id_, "clip"), std::to_string(index)), at).pressed;
  };
  if (row(0U, "Bind pose", "", !clip_.has_value())) {
    clip_.reset();
    clip_playing_ = false;
    clip_frame_ = 0U;
  }
  for (std::size_t index = 0U; index < facts->clips.size(); ++index) {
    const render::ModelClipFacts &clip = facts->clips[index];
    const std::string name =
        clip.name.empty() ? "Clip " + std::to_string(index + 1U) : clip.name;
    if (row(index + 1U, name, seconds_text(clip.duration),
            clip_.has_value() && *clip_ == index)) {
      clip_ = static_cast<core::u32>(index);
      clip_frame_ = 0U;
      play_clip(now);
    }
  }
}

void AssetViewerPanel::draw_model_info(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const render::ModelPackageFacts *const facts = p.input.model->facts.get();
  const gui::Rect tabs{p.info.x, p.info.y, p.info.width, tab_height};
  ctx.panel(p.info, theme.panel);
  ctx.panel(tabs, theme.header);
  ctx.panel(gui::Rect{tabs.x, tabs.bottom() - 1.0F, tabs.width, 1.0F},
            theme.divider);
  const std::array<std::string, 3U> names{
      "Details",
      facts != nullptr ? "Parts  " + std::to_string(facts->parts.size())
                       : std::string{"Parts"},
      facts != nullptr ? "Animation  " + std::to_string(facts->clips.size())
                       : std::string{"Animation"}};
  float x = tabs.x + 6.0F;
  for (std::size_t index = 0U; index < names.size(); ++index) {
    const float width = ctx.font_width(names[index]) + 20.0F;
    const gui::Rect at{x, tabs.y + 2.0F, width, tabs.height - 4.0F};
    if (ctx.button(mix(mix(id_, "tab"), names[index].substr(0U, 4U)), at,
                   names[index])) {
      if (tab_ != static_cast<ModelTab>(index)) {
        tab_ = static_cast<ModelTab>(index);
        info_scroll_.reset();
      }
    }
    if (tab_ == static_cast<ModelTab>(index)) {
      ctx.panel(gui::Rect{at.x, at.bottom() - 2.0F, at.width, 2.0F},
                theme.accent);
    }
    x += width + 4.0F;
  }
  if (!checks_.empty()) {
    const auto warnings = static_cast<std::size_t>(std::count_if(
        checks_.begin(), checks_.end(), [](const render::ModelCheck &check) {
          return check.level == render::ModelCheckLevel::Warning;
        }));
    if (warnings != 0U) {
      const std::string text = counted(warnings, "warning", "warnings");
      const float width = ctx.font_width(text) + 12.0F;
      ctx.label(gui::Rect{tabs.right() - width, tabs.y, width,
                          tabs.height},
                text, theme.warning);
    }
  }

  const gui::Rect content{p.info.x, tabs.bottom(), p.info.width,
                          std::max(p.info.height - tabs.height, 0.0F)};
  gui::Metrics metrics = theme.metrics;
  metrics.inset = 8.0F;
  metrics.gap = 4.0F;
  const float offset = info_scroll_.begin(ctx, content);
  gui::FormLayout rows{content, metrics, offset};
  switch (tab_) {
  case ModelTab::Details:
    draw_model_details(p, rows);
    break;
  case ModelTab::Parts:
    draw_model_parts(p, rows);
    break;
  case ModelTab::Animation:
    draw_model_animation(p, rows);
    break;
  }
  info_scroll_.end(ctx, rows.extent());
}

void AssetViewerPanel::draw_model(Pass &p) {
  const AssetViewerModelInput &model = *p.input.model;
  p.bounds = asset_viewer_model_bounds(*p.model, model.bounds);
  if (model_fresh_) {
    model_fresh_ = false;
    navigated_ = false;
    framed_from_package_ = model.bounds.has_value();
    orbit_ = asset_viewer_model_home(p.bounds);
    shading_ = p.model->projection_mesh ? render::ViewShading::UvChecker
                                        : render::ViewShading::Lit;
    part_.reset();
    clip_.reset();
    clip_playing_ = false;
    clip_frame_ = 0U;
    turntable_angle_ = 0.0F;
    tab_ = ModelTab::Details;
    info_scroll_.reset();
  } else if (!framed_from_package_ && model.bounds.has_value()) {
    // The package's own box replaces the imported one once it loads.
    framed_from_package_ = true;
    if (!navigated_) {
      orbit_ = asset_viewer_model_home(p.bounds);
    }
  }
  if (model.facts != checked_facts_ || !(model.checks == checked_context_)) {
    checked_facts_ = model.facts;
    checked_context_ = model.checks;
    checks_ = model.facts != nullptr
                  ? render::model_asset_checks(*model.facts, model.checks)
                  : std::vector<render::ModelCheck>{};
  }
  if (const render::ModelPackageFacts *const facts = model.facts.get()) {
    if (part_.has_value() && *part_ >= facts->parts.size()) {
      part_.reset();
    }
    if (clip_.has_value() && *clip_ >= facts->clips.size()) {
      clip_.reset();
      clip_playing_ = false;
    }
  }

  const gui::Rect &bounds = p.bounds_rect;
  const float info_height =
      bounds.height >= 360.0F
          ? std::clamp(bounds.height * 0.32F, 120.0F, 220.0F)
          : 0.0F;
  p.header = gui::Rect{bounds.x, bounds.y, bounds.width, header_height};
  p.info = gui::Rect{bounds.x, bounds.bottom() - info_height,
                     bounds.width, info_height};
  p.tools = gui::Rect{bounds.x, p.info.y - tools_height, bounds.width,
                      tools_height};
  p.image = gui::Rect{bounds.x, bounds.y + header_height, bounds.width,
                      std::max(p.tools.y - bounds.y - header_height, 0.0F)};

  handle_model_keys(p);
  advance_model(p);
  draw_model_image(p);
  draw_model_tools(p);
  if (info_height > 0.0F) {
    draw_model_info(p);
  }
  draw_model_header(p);
  p.frame.model_view = AssetViewerModelView{
      asset_, orbit_, shading_, look_, turntable_angle_, part_,
      clip_,  clip_loop_, clip_frame_};
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
  ctx.panel(gui::Rect{ghost.x, ghost.bottom() - 1.0F, ghost.width, 1.0F},
            ctx.theme().accent);
}

} // namespace mediaserver::editor
