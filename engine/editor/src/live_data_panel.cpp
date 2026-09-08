#include <mediaserver/editor/live_data_panel.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

constexpr float title_height = 25.0F;
constexpr float row_height = 20.0F;
constexpr float section_height = 24.0F;
constexpr float label_width = 132.0F;

void border(gui::UiContext &ctx, const gui::Rect &bounds, gui::UiColor color) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F,
                color);
  draw.add_rect(bounds.x, bounds.y, 1.0F, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - 1.0F, bounds.y, 1.0F, bounds.height,
                color);
}

[[nodiscard]] std::string format(const char *pattern, double value) {
  std::array<char, 64U> text{};
  const int length = std::snprintf(text.data(), text.size(), pattern, value);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string format_unsigned(const char *pattern,
                                          core::u64 value) {
  std::array<char, 64U> text{};
  const int length = std::snprintf(text.data(), text.size(), pattern,
                                   static_cast<unsigned long long>(value));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

// Turns are the unit the whole engine uses, but nobody on a stage thinks in
// them. Degrees are what is printed on a tripod head and in a tracker's own
// interface, so degrees are what the operator sees.
[[nodiscard]] std::string degrees(scene::Turns turns) {
  return format("%.2f deg", static_cast<double>(turns) * 360.0);
}

[[nodiscard]] std::string metres(float value) {
  return format("%.3f m", static_cast<double>(value));
}

} // namespace

InnerFrustumReport inner_frustum_report(
    const LiveDataState &state,
    const TrackedCameraObservation &camera) noexcept {
  // Two switches, and either one being off means no plate. They are reported
  // as the same state because the fix is the same act - turn one back on.
  if (!state.icvfx.inner_frustum || !camera.icvfx.inner_frustum) {
    return InnerFrustumReport::Off;
  }
  if (camera.inner_rect.has_value()) {
    return InnerFrustumReport::Drawing;
  }
  // Authored on and drawing nothing. Which of the two reasons it is decides
  // what the operator has to go and fix, so they are never merged.
  return camera.tracking.has_pose ? InnerFrustumReport::OffCanvas
                                  : InnerFrustumReport::WaitingForPose;
}

PlateCoverage plate_coverage(
    const LiveDataState &state,
    const TrackedCameraObservation &camera) noexcept {
  PlateCoverage coverage{};
  if (!camera.inner_rect.has_value()) {
    return coverage;
  }
  coverage.drawing = true;
  coverage.clamped = camera.inner_rect_clamped;
  const double canvas_pixels = static_cast<double>(state.canvas.width) *
                               static_cast<double>(state.canvas.height);
  const double plate_pixels =
      static_cast<double>(camera.inner_rect->width) *
      static_cast<double>(camera.inner_rect->height);
  coverage.percent =
      canvas_pixels > 0.0
          ? static_cast<float>(plate_pixels / canvas_pixels * 100.0)
          : 0.0F;
  return coverage;
}

NodeTrackingReport node_tracking_report(
    const NodeTrackingObservation &node) noexcept {
  if (!node.reported) {
    return NodeTrackingReport::Silent;
  }
  if (node.pose_sequence == 0U) {
    return NodeTrackingReport::NoPose;
  }
  if (node.plates_drawn != 0U) {
    // Missing poses matter only where a plate is actually being drawn from
    // them. A node that draws none has nothing running behind.
    //
    // A pose lost on the way here is reported ahead of one this node dropped:
    // if both are happening the link is the thing to fix first, because a node
    // cannot drain what it never received.
    if (node.poses_missed != 0U) {
      return NodeTrackingReport::DrawingBehind;
    }
    return node.poses_coalesced != 0U ? NodeTrackingReport::DrawingSlow
                                      : NodeTrackingReport::Drawing;
  }
  // Nothing drawn: either the plate was planned and every one of them missed
  // this node's slice, or no plate was ever planned for its outputs.
  return node.plates_skipped != 0U ? NodeTrackingReport::NoSlice
                                   : NodeTrackingReport::NotPlanned;
}

std::size_t nonzero_refusals(
    const TrackingRuntimeObservation &tracking,
    std::span<RefusalRow, max_refusal_rows> into) noexcept {
  const std::array<RefusalRow, max_refusal_rows> candidates{{
      {"Truncated", tracking.truncated},
      {"Unrecognized", tracking.unrecognized},
      {"Checksum", tracking.checksum_failures},
      {"Out of range", tracking.invalid_values},
      {"Other camera", tracking.foreign_camera},
  }};
  std::size_t used = 0U;
  for (const RefusalRow &candidate : candidates) {
    if (candidate.count != 0U) {
      into[used] = candidate;
      ++used;
    }
  }
  return used;
}

namespace {

[[nodiscard]] std::string_view node_tracking_report_text(
    NodeTrackingReport report) noexcept {
  switch (report) {
  case NodeTrackingReport::Silent:
    return "Node has not reported";
  case NodeTrackingReport::NoPose:
    return "No pose has reached this node";
  case NodeTrackingReport::NotPlanned:
    return "No plate planned for this node's outputs";
  case NodeTrackingReport::NoSlice:
    return "Plate does not reach this node's slice";
  case NodeTrackingReport::Drawing:
    return "Drawing";
  case NodeTrackingReport::DrawingBehind:
    return "Drawing, behind - poses went missing";
  case NodeTrackingReport::DrawingSlow:
    return "Drawing, dropping poses it cannot drain";
  }
  return "Node has not reported";
}

} // namespace

std::string_view tracking_link_state_name(TrackingLinkState state) noexcept {
  switch (state) {
  case TrackingLinkState::Idle:
    return "IDLE";
  case TrackingLinkState::BindFailed:
    return "BIND FAILED";
  case TrackingLinkState::Waiting:
    return "WAITING";
  case TrackingLinkState::Receiving:
    return "RECEIVING";
  }
  return "IDLE";
}

void LiveDataPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                         const LiveDataState &state) {
  ctx.panel(bounds, ctx.theme().panel);
  const gui::Rect title{bounds.x, bounds.y, bounds.width, title_height};
  ctx.panel(title, ctx.theme().header);
  ctx.label(gui::Rect{title.x + 10.0F, title.y, title.width - 20.0F,
                      title.height},
            "LIVE DATA", ctx.theme().text);

  const gui::Rect body{bounds.x, bounds.y + title_height, bounds.width,
                       std::max(bounds.height - title_height, 0.0F)};

  // Scrolled by what the last frame actually drew, rather than by a second
  // implementation of how tall the content is.
  //
  // A row count derived up front would have to reproduce every conditional in
  // the body below - which rows a held pose adds, what a clamped plate prints -
  // and would drift from it the first time one changed. Measuring the draw
  // costs one frame of lag after the content resizes, which for a diagnostics
  // panel is invisible and self-correcting.
  //
  // Until this existed the panel walked y downward under a clip and simply lost
  // everything past the bottom: one fully-populated camera is a little over
  // 400 px against roughly 630 px of body at the default layout, so the second
  // camera was cut mid-way and a third was never drawn at all, with no way for
  // an operator to reach it.
  const float maximum_scroll = std::max(content_height_ - body.height, 0.0F);
  if (body.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * row_height * 3.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(body);
  const float content_top = body.y + 6.0F - scroll_;
  float y = content_top;

  const auto section = [&](std::string_view heading) {
    ctx.label(gui::Rect{body.x + 10.0F, y, body.width - 20.0F, 18.0F}, heading,
              ctx.theme().accent);
    y += section_height;
  };
  const auto row = [&](std::string_view name, std::string_view value,
                       gui::UiColor color) {
    ctx.label(gui::Rect{body.x + 10.0F, y, label_width, row_height}, name,
              ctx.theme().text_muted);
    ctx.label(gui::Rect{body.x + 10.0F + label_width, y,
                        std::max(body.width - 20.0F - label_width, 0.0F),
                        row_height},
              value, color);
    y += row_height;
  };
  const auto plain_row = [&](std::string_view name, std::string_view value) {
    row(name, value, ctx.theme().text);
  };

  if (state.cameras.empty()) {
    section("CAMERA TRACKING");
    row("Status",
        "No tracked camera. Bind a feed to a camera in the Inspector.",
        ctx.theme().text_muted);
  }

  // Bottom of the stack first, matching the order the plates are composited
  // in, so the last camera listed is the one an operator sees on top.
  for (const TrackedCameraObservation &camera : state.cameras) {
    const TrackingRuntimeObservation &tracking = camera.tracking;
    section(camera.name.empty() ? std::string_view{"CAMERA"}
                                : std::string_view{camera.name});

    // The state, coloured, because it is the one thing an operator scans for
    // from across a room.
    const gui::UiColor state_color =
        tracking.state == TrackingLinkState::Receiving ? ctx.theme().keyframe
        : tracking.state == TrackingLinkState::BindFailed
            ? ctx.theme().warning
            : ctx.theme().text_muted;
    const gui::Rect badge{body.x + 10.0F + label_width, y, 110.0F, row_height};
    ctx.panel(badge, ctx.theme().widget);
    border(ctx, badge, state_color);
    ctx.label(gui::Rect{badge.x + 6.0F, badge.y, badge.width - 12.0F,
                        badge.height},
              tracking_link_state_name(tracking.state), state_color);
    ctx.label(gui::Rect{body.x + 10.0F, y, label_width, row_height}, "Link",
              ctx.theme().text_muted);
    y += row_height;

    if (!tracking.last_error.empty()) {
      row("Error", tracking.last_error, ctx.theme().warning);
    }

    plain_row("Protocol",
              tracking.source.protocol == scene::TrackerProtocolKind::FreeD
                  ? "FreeD"
                  : "OpenTrackIO");
    plain_row("Port", tracking.source.port == 0U
                          ? std::string{"not set"}
                          : format_unsigned("UDP %llu", tracking.source.port));
    plain_row("Camera id", format_unsigned("%llu", tracking.source.camera_id));

    // State the presence of a thing, never the absence of one: when nothing
    // has arrived there is simply no pose block, and the counters say why.
    if (tracking.has_pose) {
      plain_row("X", metres(tracking.position.x));
      plain_row("Y", metres(tracking.position.y));
      plain_row("Z", metres(tracking.position.z));
      plain_row("Pan", degrees(tracking.yaw));
      plain_row("Tilt", degrees(tracking.pitch));
      plain_row("Roll", degrees(tracking.roll));
      plain_row("Field of view", degrees(tracking.field_of_view));
      plain_row("Tracker seq",
                format_unsigned("%llu", tracking.tracker_sequence));
    }

    plain_row("Received", format_unsigned("%llu", tracking.datagrams));
    plain_row("Accepted", format_unsigned("%llu", tracking.accepted));
    // Only the refusals that actually happened.
    std::array<RefusalRow, max_refusal_rows> refusals{};
    const std::size_t refusal_count = nonzero_refusals(
        tracking, std::span<RefusalRow, max_refusal_rows>{refusals});
    for (std::size_t index = 0U; index < refusal_count; ++index) {
      row(refusals[index].name, format_unsigned("%llu", refusals[index].count),
          ctx.theme().warning);
    }

    plain_row("Poses sent", format_unsigned("%llu", tracking.poses_sent));
    // A hold is not a fault - it is what every node does in lockstep when a
    // pose does not arrive, and it is strictly better than each of them
    // guessing. It is still the number that says the tracker is dropping.
    row("Held frames", format_unsigned("%llu", tracking.held_frames),
        tracking.held_frames == 0U ? ctx.theme().text : ctx.theme().warning);
    if (tracking.longest_hold != 0U) {
      row("Longest hold", format_unsigned("%llu frames", tracking.longest_hold),
          ctx.theme().warning);
    }

    const InnerFrustumReport report = inner_frustum_report(state, camera);
    if (report == InnerFrustumReport::Off) {
      row("Plate", "Off. The wall shows the authored camera here.",
          ctx.theme().text_muted);
    } else if (report != InnerFrustumReport::Drawing) {
      row("Plate",
          report == InnerFrustumReport::OffCanvas
              ? "None: the camera sees no part of the canvas."
              : "None: waiting for a pose.",
          ctx.theme().warning);
    } else {
      const scene::CanvasRect &rect = *camera.inner_rect;
      std::array<char, 96U> text{};
      const int length =
          std::snprintf(text.data(), text.size(), "%u x %u at %u, %u",
                        rect.width, rect.height, rect.x, rect.y);
      row("Plate",
          std::string_view{text.data(),
                           static_cast<std::size_t>(std::max(length, 0))},
          ctx.theme().keyframe);
      // Coverage alone cannot distinguish a camera framing the wall exactly
      // from one whose frustum runs off the end of it, and those are very
      // different things to be looking at.
      const PlateCoverage coverage = plate_coverage(state, camera);
      if (coverage.clamped) {
        row("Wall covered",
            "100.0 % - clamped: the frustum is not bounded by this wall",
            ctx.theme().warning);
      } else {
        plain_row("Wall covered",
                  format("%.1f %%", static_cast<double>(coverage.percent)));
      }
    }
    // Where this plate sits in the stack, so an overlap an operator did not
    // expect can be traced to the number that decided it.
    if (state.cameras.size() > 1U) {
      plain_row("Render order",
                format("%.0f",
                       static_cast<double>(camera.icvfx.render_order)));
    }
  }

  // The nodes, after the cameras that feed them. A pose is only worth
  // anything once a node has it, and until this section existed the Director
  // could report a perfectly healthy tracker while every wall on the stage was
  // holding a stale sample.
  for (const NodeTrackingObservation &node : state.nodes) {
    section(node.name.empty() ? std::string_view{"NODE"}
                              : std::string_view{node.name});
    const NodeTrackingReport report = node_tracking_report(node);
    const gui::UiColor report_color =
        report == NodeTrackingReport::Drawing ? ctx.theme().keyframe
        : report == NodeTrackingReport::Silent ||
                report == NodeTrackingReport::NoPose ||
                report == NodeTrackingReport::DrawingBehind ||
                report == NodeTrackingReport::DrawingSlow
            ? ctx.theme().warning
            : ctx.theme().text_muted;
    row("Plate", node_tracking_report_text(report), report_color);
    if (!node.reported) {
      continue;
    }
    plain_row("Pose", format_unsigned("#%llu", node.pose_sequence));
    if (node.poses_missed != 0U) {
      row("Poses missed", format_unsigned("%llu", node.poses_missed),
          ctx.theme().warning);
    }
    if (node.poses_coalesced != 0U) {
      row("Poses dropped here",
          format_unsigned("%llu", node.poses_coalesced),
          ctx.theme().warning);
    }
    plain_row("Frames", format_unsigned("%llu applied", node.poses_applied));
    if (node.held_frames != 0U) {
      plain_row("Held", format_unsigned("%llu frames", node.held_frames));
    }
    plain_row("Plates", format_unsigned("%llu drawn", node.plates_drawn));
    // Skipped is printed unconditionally beside drawn. It is the number that
    // says the second frustum is earning its cost, and a zero here on a wide
    // wall is itself worth seeing.
    plain_row("Skipped", format_unsigned("%llu", node.plates_skipped));
    if (node.plate_refusals != 0U) {
      row("Refused", format_unsigned("%llu", node.plate_refusals),
          ctx.theme().warning);
    }
  }

  section("SHOW");
  row("Inner frustums",
      state.icvfx.inner_frustum ? "On" : "Off for the whole show",
      state.icvfx.inner_frustum ? ctx.theme().text : ctx.theme().warning);
  plain_row("Latency",
            format_unsigned("%llu frames", state.icvfx.latency_frames));

  content_height_ = (y - content_top) + 6.0F;

  ctx.pop_clip();
  // A scrollable panel has to say so. Without it a clipped panel and a short
  // one look identical, and the operator has no reason to try the wheel.
  if (maximum_scroll > 0.0F && content_height_ > 0.0F) {
    constexpr float track_width = 3.0F;
    const float visible = std::clamp(body.height / content_height_, 0.0F, 1.0F);
    const float thumb_height = std::max(visible * body.height, 24.0F);
    const float travel = std::max(body.height - thumb_height, 0.0F);
    const float thumb_y =
        body.y + travel * std::clamp(scroll_ / maximum_scroll, 0.0F, 1.0F);
    ctx.panel(gui::Rect{body.x + body.width - track_width - 2.0F, thumb_y,
                        track_width, thumb_height},
              ctx.theme().divider);
  }
  border(ctx, bounds, ctx.theme().divider);
}

} // namespace mediaserver::editor
