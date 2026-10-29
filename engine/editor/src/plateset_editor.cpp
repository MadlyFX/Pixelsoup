#include <mediaserver/editor/plateset_editor.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace mediaserver::editor {
namespace {

using assets::PlatesetSlot;

constexpr float header_height = 34.0F;
constexpr float info_width = 300.0F;
constexpr float tile_gap = 8.0F;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 salt = 0U) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL ^ (salt * 0x100000001B3ULL);
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

// Where each slot sits on the 3 x 3 rig diagram, the car in the middle
// cell under the ceiling plate, front up.
struct Cell final {
  int column{};
  int row{};
};

[[nodiscard]] Cell cell_of(PlatesetSlot slot) noexcept {
  switch (slot) {
  case PlatesetSlot::FrontLeft:
    return {0, 0};
  case PlatesetSlot::Front:
    return {1, 0};
  case PlatesetSlot::FrontRight:
    return {2, 0};
  case PlatesetSlot::Left:
    return {0, 1};
  case PlatesetSlot::Ceiling:
    return {1, 1};
  case PlatesetSlot::Right:
    return {2, 1};
  case PlatesetSlot::BackLeft:
    return {0, 2};
  case PlatesetSlot::Back:
    return {1, 2};
  case PlatesetSlot::BackRight:
    return {2, 2};
  }
  return {1, 0};
}

[[nodiscard]] std::string frames_text(core::i64 frames) {
  return std::to_string(frames) + (frames == 1 ? " frame" : " frames");
}

[[nodiscard]] std::string megapixels_text(core::u64 pixels_per_second) {
  std::array<char, 32U> text{};
  std::snprintf(text.data(), text.size(), "%.0f Mpx/s",
                static_cast<double>(pixels_per_second) / 1.0e6);
  return text.data();
}

[[nodiscard]] const assets::MediaAsset *
media_in(const assets::AssetRegistry *registry, doc::NodeId id) noexcept {
  if (registry == nullptr || id == doc::NodeId{}) {
    return nullptr;
  }
  const assets::Asset *const asset = registry->asset(id);
  return asset != nullptr ? std::get_if<assets::MediaAsset>(asset) : nullptr;
}

void draw_cross(gui::UiContext &ctx, const gui::Rect &bounds,
                gui::UiColor color) {
  const float inset = bounds.width * 0.3F;
  auto &draw = ctx.draw_list();
  draw.add_line(bounds.x + inset, bounds.y + inset, bounds.right() - inset,
                bounds.bottom() - inset, 1.5F, color);
  draw.add_line(bounds.right() - inset, bounds.y + inset, bounds.x + inset,
                bounds.bottom() - inset, 1.5F, color);
}

} // namespace

std::optional<PlatesetSlot>
PlatesetEditor::slot_at(float x, float y) const noexcept {
  for (const PlatesetSlot slot : assets::all_plateset_slots) {
    if (slot_rects_[static_cast<std::size_t>(slot)].contains(x, y)) {
      return slot;
    }
  }
  return std::nullopt;
}

bool PlatesetEditor::rig_contains(float x, float y) const noexcept {
  return rig_rect_.contains(x, y);
}

PlatesetEditorFrame PlatesetEditor::draw(gui::UiContext &ctx,
                                         const gui::Rect &bounds,
                                         const PlatesetEditorInput &input) {
  PlatesetEditorFrame frame;
  slot_rects_ = {};
  rig_rect_ = {};
  if (input.plateset == nullptr || bounds.width <= 0.0F ||
      bounds.height <= 0.0F) {
    return frame;
  }
  const assets::PlatesetAsset &plateset = *input.plateset;
  const gui::Theme &theme = ctx.theme();
  if (message_set_ != plateset.id) {
    message_.clear();
    message_set_ = plateset.id;
  }
  const assets::PlatesetReadiness readiness =
      input.assets != nullptr
          ? assets::evaluate_plateset(plateset, *input.assets)
          : assets::PlatesetReadiness{};

  ctx.panel(bounds, theme.panel);
  gui::Rect regions = bounds;

  // ------------------------------------------------------------ header
  {
    gui::Rect header = gui::cut_top(regions, header_height);
    ctx.panel(header, theme.header);
    gui::Rect rule = header;
    ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
    header = gui::inset(header, 8.0F, 5.0F);
    const gui::Rect icon = gui::cut_left(header, header.height);
    draw_plateset_icon(ctx, icon);
    static_cast<void>(gui::cut_left(header, 6.0F));
    // The rig templates, right-aligned: which slots the set offers.
    constexpr std::array rigs{assets::PlatesetRig::Three,
                              assets::PlatesetRig::Five,
                              assets::PlatesetRig::Nine};
    for (auto it = rigs.rbegin(); it != rigs.rend(); ++it) {
      const std::string label{assets::plateset_rig_label(*it)};
      const gui::Rect button =
          gui::cut_right(header, ctx.font_width(label) + 18.0F);
      static_cast<void>(gui::cut_right(header, 4.0F));
      if (action_button(ctx, mix(id_, "rig", static_cast<core::u64>(*it)),
                        button, label, true, plateset.rig == *it) &&
          plateset.rig != *it) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetRig;
        edit.target = plateset.id;
        edit.rig = *it;
        frame.edits.push_back(std::move(edit));
      }
    }
    ctx.label(header, plateset.display_name, theme.text);
  }

  // ------------------------------------------------------------ layout
  const bool side_by_side = regions.width >= 560.0F;
  gui::Rect rig_area{};
  gui::Rect info{};
  if (side_by_side) {
    info = gui::cut_right(regions, std::min(info_width, regions.width * 0.45F));
    rig_area = regions;
  } else {
    rig_area = gui::cut_top(regions, std::max(regions.height * 0.62F, 120.0F));
    info = regions;
  }
  rig_area = gui::inset(rig_area, 12.0F, 12.0F);
  const float side = std::max(std::min(rig_area.width, rig_area.height), 0.0F);
  rig_rect_ = gui::Rect{rig_area.x + (rig_area.width - side) * 0.5F,
                        rig_area.y + (rig_area.height - side) * 0.5F, side,
                        side};
  const float cell = (side - tile_gap * 2.0F) / 3.0F;

  // The car under the ceiling plate, so the diagram reads as a top view.
  {
    const float car_w = cell * 0.62F;
    const float car_h = cell * 0.92F;
    const gui::Rect car{rig_rect_.x + cell + tile_gap + (cell - car_w) * 0.5F,
                        rig_rect_.y + cell + tile_gap + (cell - car_h) * 0.5F,
                        car_w, car_h};
    ctx.rounded(car, alpha(theme.accent, 0.08F), car_w * 0.28F);
    ctx.rounded_outline(car, alpha(theme.accent, 0.35F), car_w * 0.28F);
    const gui::Rect windscreen{car.x + car_w * 0.16F, car.y + car_h * 0.16F,
                               car_w * 0.68F, car_h * 0.14F};
    ctx.rounded(windscreen, alpha(theme.accent, 0.2F), 3.0F);
  }

  // A take's plates usually share a long name and differ only in their
  // position tag, which is the part a tile has room for: show each without
  // what they all share.
  std::size_t shared = std::string::npos;
  std::string_view first_name;
  for (const assets::PlatesetSlotAssignment &assignment : plateset.slots) {
    const assets::MediaAsset *const media =
        media_in(input.assets, assignment.media);
    if (media == nullptr) {
      continue;
    }
    if (shared == std::string::npos) {
      first_name = media->display_name;
      shared = first_name.size();
      continue;
    }
    std::size_t common = 0U;
    while (common < shared && common < media->display_name.size() &&
           first_name[common] == media->display_name[common]) {
      ++common;
    }
    shared = common;
  }
  const auto tile_name = [shared](const std::string &name) {
    if (shared == std::string::npos || shared < 8U || shared >= name.size()) {
      return name;
    }
    return "..." + name.substr(shared);
  };

  const std::span<const PlatesetSlot> offered =
      assets::plateset_rig_slots(plateset.rig);
  const float pointer_x = ctx.pointer_x();
  const float pointer_y = ctx.pointer_y();
  for (const PlatesetSlot slot : assets::all_plateset_slots) {
    const std::size_t index = static_cast<std::size_t>(slot);
    const assets::PlatesetSlotAssignment &assignment = plateset.slot(slot);
    const bool in_rig =
        std::find(offered.begin(), offered.end(), slot) != offered.end();
    const bool filled = assignment.media != doc::NodeId{};
    const Cell at = cell_of(slot);
    gui::Rect tile{rig_rect_.x + static_cast<float>(at.column) * (cell + tile_gap),
                   rig_rect_.y + static_cast<float>(at.row) * (cell + tile_gap),
                   cell, cell};
    if (slot == PlatesetSlot::Ceiling) {
      // The ceiling plate floats over the car: a smaller tile, so the car
      // still reads beneath it.
      tile = gui::inset(tile, cell * 0.14F, cell * 0.22F);
    }
    if (!in_rig && !filled) {
      // A slot this rig does not use: a faint outline, still a drop target
      // so an unusual array can be built without changing the template.
      ctx.rounded_outline(tile, alpha(theme.border, 0.5F), theme.radius_small);
      const float mark = std::min(tile.width, tile.height) * 0.4F;
      draw_plateset_icon(ctx,
                         gui::Rect{tile.x + (tile.width - mark) * 0.5F,
                                   tile.y + (tile.height - mark) * 0.5F, mark,
                                   mark},
                         slot);
      slot_rects_[index] = tile;
      if (input.dragging_assets && tile.contains(pointer_x, pointer_y)) {
        draw_drop_target(ctx, tile, assets::plateset_slot_label(slot), true,
                         tile.width);
      }
      continue;
    }
    slot_rects_[index] = tile;
    const assets::MediaAsset *const media =
        media_in(input.assets, assignment.media);
    const bool problem = std::any_of(
        readiness.issues.begin(), readiness.issues.end(),
        [slot](const assets::PlatesetIssue &issue) {
          return issue.slot == slot && issue.blocking();
        });
    ctx.rounded(tile, filled ? theme.widget : alpha(theme.widget, 0.45F),
                theme.radius_small);
    gui::Rect content = gui::inset(tile, 5.0F, 4.0F);
    gui::Rect title = gui::cut_top(content, 16.0F);
    const std::string label{tile.width > 120.0F
                                ? assets::plateset_slot_label(slot)
                                : assets::plateset_slot_tag(slot)};
    if (filled) {
      // Clear, at the title's right end.
      const gui::Rect clear = gui::cut_right(title, 16.0F);
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "clear", index), clear);
      draw_cross(ctx, clear,
                 interaction.hovered ? theme.warning : theme.text_muted);
      ctx.tooltip(clear, "Empty this slot");
      if (interaction.clicked) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetSlot;
        edit.target = plateset.id;
        edit.slot = slot;
        frame.edits.push_back(std::move(edit));
      }
    }
    ctx.label(title, label, problem ? theme.warning : theme.text_muted);

    const bool roomy = content.height > 54.0F;
    const gui::Rect footer =
        roomy ? gui::cut_bottom(content, 36.0F) : gui::Rect{};
    if (filled && media != nullptr) {
      if (media->fingerprint.has_value()) {
        if (const assets::DerivativeProfile *const profile =
                assets::derivative_profile(
                    assets::DerivativeProfileId::Thumbnail320x180Srgb)) {
          frame.thumbnails.push_back(assets::make_derivative_key(
              media->id, *media->fingerprint, *profile));
        }
      }
      const LibraryThumbnailObservation *thumbnail = nullptr;
      for (const LibraryThumbnailObservation &observation : input.thumbnails) {
        if (observation.key.asset == media->id &&
            observation.state == LibraryThumbnailState::Ready &&
            observation.texture != nullptr) {
          thumbnail = std::addressof(observation);
          break;
        }
      }
      if (content.height > 8.0F) {
        // Letterboxed at 16:9 in what is left of the tile.
        const float width = std::min(content.width, content.height * 16.0F / 9.0F);
        const float height = width * 9.0F / 16.0F;
        const gui::Rect picture{content.x + (content.width - width) * 0.5F,
                                content.y + (content.height - height) * 0.5F,
                                width, height};
        ctx.panel(picture, theme.track);
        if (thumbnail != nullptr) {
          ctx.image(picture, *thumbnail->texture);
        } else {
          const float mark = picture.height * 0.8F;
          draw_plateset_icon(
              ctx,
              gui::Rect{picture.x + (picture.width - mark) * 0.5F,
                        picture.y + (picture.height - mark) * 0.5F, mark,
                        mark},
              slot);
        }
      }
      if (roomy) {
        gui::Rect lines = footer;
        const gui::Rect name = gui::cut_top(lines, 16.0F);
        ctx.label(name, tile_name(media->display_name), theme.text);
        ctx.tooltip(name, media->source);
        // The sync offset, scrubbed or typed; authored when the field lets go.
        const gui::Rect offset_row = gui::cut_top(lines, 18.0F);
        const bool editing = offset_slot_ == slot;
        const core::i64 shown = editing ? offset_value_ : assignment.offset;
        double out = static_cast<double>(shown);
        if (offset_field_.draw(ctx, mix(id_, "offset", index), offset_row,
                               "SYNC", static_cast<double>(shown), 0.25, out,
                               0)) {
          offset_slot_ = slot;
          offset_value_ = std::max<core::i64>(
              0, static_cast<core::i64>(std::llround(out)));
        }
        ctx.tooltip(offset_row,
                    "Frames of this plate before the take's common start");
      }
    } else if (filled) {
      ctx.label(content, "missing", theme.warning);
    } else {
      const float mark = std::min(content.width, content.height) * 0.7F;
      draw_plateset_icon(ctx,
                         gui::Rect{content.x + (content.width - mark) * 0.5F,
                                   content.y + (content.height - mark) * 0.5F,
                                   mark, mark},
                         slot);
      ctx.tooltip(tile, input.dragging_assets
                            ? "Drop a plate here"
                            : "Drag a plate here from the Library");
    }
    if (problem) {
      ctx.rounded_outline(tile, theme.warning, theme.radius_small);
    }
    if (input.dragging_assets && tile.contains(pointer_x, pointer_y)) {
      draw_drop_target(ctx, tile, assets::plateset_slot_label(slot), true,
                       tile.width);
    } else if (input.dragging_assets) {
      ctx.rounded_outline(tile, alpha(theme.accent, 0.45F), theme.radius_small);
    }
  }
  // An offset the field has let go of becomes one edit.
  if (offset_slot_.has_value() && !offset_field_.live()) {
    if (offset_value_ != plateset.slot(*offset_slot_).offset) {
      assets::LibraryEdit edit{};
      edit.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
      edit.target = plateset.id;
      for (const PlatesetSlot slot : assets::all_plateset_slots) {
        edit.offsets[static_cast<std::size_t>(slot)] = plateset.slot(slot).offset;
      }
      edit.offsets[static_cast<std::size_t>(*offset_slot_)] = offset_value_;
      frame.edits.push_back(std::move(edit));
    }
    offset_slot_.reset();
  }

  // ------------------------------------------------------------ info
  info = gui::inset(info, 10.0F, 10.0F);
  ctx.push_clip(info);
  const auto line = [&](float height) { return gui::cut_top(info, height); };
  const auto heading = [&](std::string_view text) {
    static_cast<void>(gui::cut_top(info, 4.0F));
    ctx.label(line(18.0F), text, theme.text_muted);
  };

  heading("READINESS");
  if (readiness.ready() &&
      std::none_of(readiness.issues.begin(), readiness.issues.end(),
                   [](const assets::PlatesetIssue &issue) {
                     return issue.kind ==
                            assets::PlatesetIssueKind::DecodeOverBudget;
                   })) {
    gui::Rect row = line(20.0F);
    draw_status_dot(ctx, row.x + 5.0F, row.y + row.height * 0.5F, theme.good);
    static_cast<void>(gui::cut_left(row, 14.0F));
    ctx.label(row,
              std::to_string(readiness.filled) + " plates ready, " +
                  frames_text(readiness.common_frames) + " in common",
              theme.text);
  } else {
    for (const assets::PlatesetIssue &issue : readiness.issues) {
      gui::Rect row = line(20.0F);
      draw_status_dot(ctx, row.x + 5.0F, row.y + row.height * 0.5F,
                      issue.blocking() ? theme.warning : theme.caution);
      static_cast<void>(gui::cut_left(row, 14.0F));
      const std::string text = assets::plateset_issue_text(issue);
      ctx.label(row, text, theme.text);
      ctx.tooltip(row, text);
    }
    if (readiness.ready()) {
      gui::Rect row = line(20.0F);
      static_cast<void>(gui::cut_left(row, 14.0F));
      ctx.label(row, frames_text(readiness.common_frames) + " in common",
                theme.text_muted);
    }
  }

  // Decode load against the measured budget: a bar that reads at a glance
  // and an action that brings it in.
  if (readiness.decode_pixels_per_second > 0U) {
    heading("DECODE");
    const gui::Rect bar = gui::inset(line(10.0F), 0.0F, 2.0F);
    ctx.rounded(bar, theme.track, 3.0F);
    const double ratio =
        static_cast<double>(readiness.decode_pixels_per_second) /
        static_cast<double>(std::max<core::u64>(readiness.decode_budget, 1U));
    const bool over = ratio > 1.0;
    gui::Rect fill = bar;
    fill.width = bar.width * static_cast<float>(std::min(ratio, 1.0));
    ctx.rounded(fill, over ? theme.caution : theme.good, 3.0F);
    ctx.label(line(18.0F),
              megapixels_text(readiness.decode_pixels_per_second) + " of " +
                  megapixels_text(readiness.decode_budget),
              over ? theme.caution : theme.text_muted);
    if (over && input.assets != nullptr) {
      const gui::Rect button = line(24.0F);
      if (pill_button(ctx, mix(id_, "proxies"), button, std::nullopt,
                      "Prefer 720p proxies for every plate",
                      "Plays each plate from a 720p proxy once it is made, "
                      "falling back to the source until then")) {
        for (const assets::PlatesetSlotAssignment &assignment :
             plateset.slots) {
          const assets::MediaAsset *const media =
              media_in(input.assets, assignment.media);
          if (media == nullptr ||
              media->proxy_playback_policy !=
                  assets::ProxyPlaybackPolicy::Source) {
            continue;
          }
          frame.proxy_edits.push_back(LibraryProxyPolicyIntent{
              media->id, assets::DerivativeProfileId::Proxy720pCfrH264,
              assets::ProxyPlaybackPolicy::PreferProxy});
        }
      }
    }
  }

  // Half size for every plate: a quarter of the memory per decoded frame,
  // softer where a plate is shown larger than half its size. It does not
  // make decoding cheaper.
  {
    bool half = plateset.half_resolution;
    if (ctx.checkbox(mix(id_, "half"), line(22.0F), half,
                     "Half resolution decode (all plates)")) {
      assets::LibraryEdit edit{};
      edit.kind = assets::LibraryEdit::Kind::SetPlatesetHalfResolution;
      edit.target = plateset.id;
      edit.half = half;
      frame.edits.push_back(std::move(edit));
    }
  }

  // Sync: by the plates' own timecode, or by analysing their motion.
  heading("SYNC");
  {
    gui::Rect row = line(26.0F);
    const gui::Rect timecode =
        gui::cut_left(row, std::min(row.width * 0.5F - 2.0F, 150.0F));
    static_cast<void>(gui::cut_left(row, 4.0F));
    if (pill_button(ctx, mix(id_, "sync.timecode"), timecode, std::nullopt,
                    "Sync by Timecode",
                    "Line the plates up by the start timecode each file "
                    "carries",
                    ButtonTone::Primary, readiness.filled > 0U) &&
        input.assets != nullptr) {
      const assets::PlatesetTimecodeResult result =
          assets::plateset_timecode_sync(plateset, *input.assets);
      if (result.sync.has_value()) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
        edit.target = plateset.id;
        edit.offsets = result.sync->offsets;
        frame.edits.push_back(std::move(edit));
        message_ = "Synced by timecode";
      } else {
        message_ = assets::plateset_timecode_error_text(result);
      }
      frame.message = message_;
    }
    const bool running =
        input.analysis != nullptr &&
        (input.analysis->state == PlatesetAnalysisState::Queued ||
         input.analysis->state == PlatesetAnalysisState::Running);
    if (pill_button(ctx, mix(id_, "sync.analyse"), row, std::nullopt,
                    running ? "Analysing..." : "Sync by Motion",
                    "Decode every plate and line them up by the bumps they "
                    "share - for plates without timecode",
                    ButtonTone::Quiet,
                    readiness.filled > 1U && !running)) {
      frame.analyse = PlatesetAnalysisPathway::Sync;
    }
  }
  heading("MOMENTS");
  {
    const bool running =
        input.analysis != nullptr &&
        (input.analysis->state == PlatesetAnalysisState::Queued ||
         input.analysis->state == PlatesetAnalysisState::Running);
    gui::Rect row = line(26.0F);
    const gui::Rect find =
        gui::cut_left(row, std::min(row.width * 0.5F - 2.0F, 150.0F));
    static_cast<void>(gui::cut_left(row, 4.0F));
    if (pill_button(ctx, mix(id_, "moments.find"), find, std::nullopt,
                    "Find Moments",
                    "Read the front plate alone for stops, turns, bumps and "
                    "shade - quicker than a sync",
                    ButtonTone::Quiet,
                    plateset.slot(PlatesetSlot::Front).media != doc::NodeId{} &&
                        !running)) {
      frame.analyse = PlatesetAnalysisPathway::Moments;
    }
    const std::string kept =
        std::to_string(plateset.moments.size()) +
        (plateset.moments.size() == 1U ? " moment kept" : " moments kept");
    ctx.label(gui::inset(row, 6.0F, 0.0F), kept, theme.text_muted);
    ctx.tooltip(row, "Kept moments are drawn on the Timeline wherever the "
                     "plateset is placed, and move with its footage");
  }
  if (!message_.empty()) {
    gui::Rect row = line(20.0F);
    ctx.label(row, message_, theme.text_muted);
    ctx.tooltip(row, message_);
  }

  // What the motion analysis found: progress while it runs, then each
  // plate's offset with how sure it is, and the moments it noticed.
  if (input.analysis != nullptr &&
      input.analysis->state != PlatesetAnalysisState::Idle) {
    const PlatesetAnalysisView &analysis = *input.analysis;
    heading("MOTION ANALYSIS");
    if (analysis.state == PlatesetAnalysisState::Queued ||
        analysis.state == PlatesetAnalysisState::Running) {
      const gui::Rect bar = gui::inset(line(10.0F), 0.0F, 2.0F);
      ctx.rounded(bar, theme.track, 3.0F);
      gui::Rect fill = bar;
      fill.width = bar.width * std::clamp(analysis.progress, 0.0F, 1.0F);
      ctx.rounded(fill, theme.accent, 3.0F);
      ctx.label(line(18.0F), analysis.detail, theme.text_muted);
    } else if (analysis.state == PlatesetAnalysisState::Failed) {
      ctx.label(line(18.0F), analysis.detail, theme.warning);
    } else if (analysis.synced) {
      for (const PlatesetSlot slot : assets::all_plateset_slots) {
        const std::size_t index = static_cast<std::size_t>(slot);
        if (!analysis.offsets[index].has_value()) {
          continue;
        }
        gui::Rect row = line(18.0F);
        const float confidence = analysis.confidence[index];
        const gui::UiColor sure = confidence >= 0.5F   ? theme.good
                                  : confidence >= 0.2F ? theme.caution
                                                       : theme.warning;
        draw_status_dot(ctx, row.x + 5.0F, row.y + row.height * 0.5F, sure);
        static_cast<void>(gui::cut_left(row, 14.0F));
        std::string text{assets::plateset_slot_tag(slot)};
        text += "  " + std::to_string(*analysis.offsets[index]) + "  " +
                std::to_string(static_cast<int>(std::lround(confidence * 100.0F))) +
                "%";
        if (analysis.drift[index].has_value() && *analysis.drift[index] != 0) {
          text += "  drifts " + std::to_string(*analysis.drift[index]);
        }
        ctx.label(row, text, theme.text);
        ctx.tooltip(row, "Offset found for this plate, how clearly it beats "
                         "every other, and how far the plate drifts from the "
                         "front between the start and the end of the take");
      }
      // Only offsets the analysis is sure of are applied; the others keep
      // what the set already has, and the operator is told why.
      constexpr float trusted = 0.5F;
      std::size_t sure = 0U;
      std::size_t found = 0U;
      for (std::size_t index = 0U; index < analysis.offsets.size(); ++index) {
        if (analysis.offsets[index].has_value()) {
          ++found;
          sure += analysis.confidence[index] >= trusted ? 1U : 0U;
        }
      }
      if (sure < found) {
        gui::Rect note = line(18.0F);
        const std::string text =
            sure <= 1U ? "These plates share too little motion to sync "
                         "from the picture"
                       : "Red and amber plates are unsure and keep their "
                         "offsets";
        ctx.label(note, text, theme.caution);
        ctx.tooltip(note, text);
      }
      gui::Rect row = line(26.0F);
      const gui::Rect apply =
          gui::cut_left(row, std::min(row.width * 0.5F - 2.0F, 150.0F));
      static_cast<void>(gui::cut_left(row, 4.0F));
      const std::string apply_label =
          sure == found ? std::string{"Apply Sync"}
                        : "Apply " + std::to_string(sure) + " of " +
                              std::to_string(found);
      if (pill_button(ctx, mix(id_, "analysis.apply"), apply, std::nullopt,
                      apply_label,
                      "Author the offsets the analysis is sure of on the "
                      "plateset; placed plates follow",
                      ButtonTone::Quiet, sure > 1U)) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
        edit.target = plateset.id;
        for (const PlatesetSlot slot : assets::all_plateset_slots) {
          const std::size_t index = static_cast<std::size_t>(slot);
          edit.offsets[index] =
              analysis.offsets[index].has_value() &&
                      analysis.confidence[index] >= trusted
                  ? *analysis.offsets[index]
                  : plateset.slot(slot).offset;
        }
        frame.edits.push_back(std::move(edit));
        message_ = "Synced by motion";
        frame.message = message_;
      }
    }
    // The front plate's moments: each a click from being kept, or from
    // being let go again.
    if (analysis.moments_found &&
        analysis.state == PlatesetAnalysisState::Done) {
      const auto kept = [&plateset](const PlatesetMotionEvent &event) {
        return std::any_of(plateset.moments.begin(), plateset.moments.end(),
                           [&event](const assets::PlatesetMoment &moment) {
                             return moment.frame == event.frame &&
                                    moment.kind == event.kind;
                           });
      };
      const auto keep = [&](std::vector<assets::PlatesetMoment> moments) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetMoments;
        edit.target = plateset.id;
        edit.moments = std::move(moments);
        frame.edits.push_back(std::move(edit));
      };
      gui::Rect row = line(26.0F);
      const gui::Rect all =
          gui::cut_left(row, std::min(row.width * 0.5F - 2.0F, 150.0F));
      static_cast<void>(gui::cut_left(row, 4.0F));
      if (pill_button(ctx, mix(id_, "moments.keep"), all, std::nullopt,
                      "Keep All",
                      "Keep every moment found; they show on the Timeline "
                      "wherever the plateset is placed",
                      ButtonTone::Quiet, !analysis.events.empty())) {
        std::vector<assets::PlatesetMoment> moments = plateset.moments;
        for (const PlatesetMotionEvent &event : analysis.events) {
          if (!kept(event)) {
            moments.push_back(assets::PlatesetMoment{event.frame, event.kind});
          }
        }
        keep(std::move(moments));
      }
      if (pill_button(ctx, mix(id_, "moments.clear"), row, std::nullopt,
                      "Clear Kept", "Let every kept moment go",
                      ButtonTone::Quiet, !plateset.moments.empty())) {
        keep({});
      }
      for (std::size_t index = 0U; index < analysis.events.size(); ++index) {
        if (info.height < 18.0F) {
          break;
        }
        const PlatesetMotionEvent &event = analysis.events[index];
        const bool is_kept = kept(event);
        const gui::Rect event_row = line(18.0F);
        const gui::Interaction interaction =
            ctx.grab(mix(id_, "analysis.event", index), event_row);
        if (interaction.hovered) {
          ctx.rounded(event_row, theme.widget_hover, 3.0F);
        }
        draw_status_dot(ctx, event_row.x + 6.0F,
                        event_row.y + event_row.height * 0.5F,
                        is_kept ? plateset_moment_color(event.kind)
                                : theme.border);
        ctx.label(gui::inset(event_row, gui::Padding{16.0F, 0.0F, 0.0F, 0.0F}),
                  std::to_string(event.frame) + "   " +
                      std::string{assets::plateset_moment_label(event.kind)},
                  is_kept || interaction.hovered ? theme.text
                                                 : theme.text_muted);
        ctx.tooltip(event_row, is_kept ? "Kept: click to let it go"
                                       : "Click to keep this moment");
        if (interaction.clicked) {
          std::vector<assets::PlatesetMoment> moments = plateset.moments;
          if (is_kept) {
            std::erase_if(moments, [&event](const assets::PlatesetMoment &m) {
              return m.frame == event.frame && m.kind == event.kind;
            });
          } else {
            moments.push_back(assets::PlatesetMoment{event.frame, event.kind});
          }
          keep(std::move(moments));
        }
      }
    }
  }

  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
