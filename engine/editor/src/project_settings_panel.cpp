#include <mediaserver/editor/project_settings_panel.hpp>

#include "workspace_panel_helpers.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace panel_helpers;

namespace {

// The category tabs keep each focused pane compact enough to keep the project
// dialog readable without hiding Apply/Cancel below a long settings form. A
// card taller than the window is cut to the window, and its pane scrolls.
constexpr float project_settings_card_height = 700.0F;

constexpr std::array<std::string_view, 3U> feed_protocol_choices{
    {"FreeD", "OpenTrackIO", "NatNet (OptiTrack)"}};
constexpr std::array<std::string_view, 2U> feed_axes_choices{
    {"Stage (Y up)", "Z up"}};

[[nodiscard]] std::size_t
protocol_index(scene::TrackerProtocolKind protocol) noexcept {
  switch (protocol) {
  case scene::TrackerProtocolKind::FreeD:
    return 0U;
  case scene::TrackerProtocolKind::OpenTrackIO:
    return 1U;
  case scene::TrackerProtocolKind::NatNet:
    return 2U;
  }
  return 0U;
}

[[nodiscard]] scene::TrackerProtocolKind
protocol_at(std::size_t index) noexcept {
  return index == 2U   ? scene::TrackerProtocolKind::NatNet
         : index == 1U ? scene::TrackerProtocolKind::OpenTrackIO
                       : scene::TrackerProtocolKind::FreeD;
}

// The server field drafts empty for "not set", which is what an operator
// leaves it as to discover the server from its frames.
[[nodiscard]] std::string
server_text(const std::array<core::u8, 4U> &address) {
  return scene::ipv4_address_set(address) ? scene::ipv4_address_text(address)
                                          : std::string{};
}

[[nodiscard]] bool parse_server(const std::string &text,
                                std::array<core::u8, 4U> &into) noexcept {
  if (text.empty()) {
    into = {};
    return true;
  }
  return scene::parse_ipv4_address(text, into);
}

// What the feed's link is doing, in the operator's terms: the one line that
// says whether the numbers above are right.
[[nodiscard]] std::string feed_status_text(
    const scene::TrackingSourceSettings &settings,
    const TrackingFeedObservation *observed) {
  const bool natnet = settings.protocol == scene::TrackerProtocolKind::NatNet;
  if (observed == nullptr || observed->state == TrackingLinkState::Idle) {
    return settings.port == 0U ? "Not listening: give it a port"
                               : "Not listening";
  }
  if (observed->state == TrackingLinkState::BindFailed) {
    return "Could not open the port: " + observed->last_error;
  }
  if (natnet && observed->server_description.empty()) {
    return observed->server_address.empty()
               ? std::string{"Waiting for frames on the multicast group"}
               : "Waiting for Motive at " + observed->server_address;
  }
  if (!natnet) {
    return observed->state == TrackingLinkState::Receiving
               ? std::string{"Receiving"}
               : std::string{"Waiting for data"};
  }
  std::string text = observed->server_description;
  if (!observed->server_address.empty()) {
    text += " at " + observed->server_address;
  }
  const std::size_t bodies = observed->rigid_bodies.size();
  text += ", " + std::to_string(bodies) +
          (bodies == 1U ? " rigid body" : " rigid bodies");
  return text;
}

} // namespace

void ProjectSettingsPanel::show_feeds() noexcept {
  category_ = Category::Feeds;
  scroll_.reset();
}

std::vector<ProjectFeedEdit>
ProjectSettingsPanel::feed_edits(const ProjectSettingsState &state) const {
  std::vector<ProjectFeedEdit> edits;
  for (const FeedDraft &draft : feed_drafts_) {
    const auto authored = std::find_if(
        state.tracking_sources.begin(), state.tracking_sources.end(),
        [&draft](const scene::TrackingSource &source) {
          return source.node == draft.source.node;
        });
    if (authored == state.tracking_sources.end()) {
      continue;
    }
    const doc::NodeId node = draft.source.node;
    const scene::TrackingSourceSettings &was = authored->settings;
    const scene::TrackingSourceSettings &now = draft.source.settings;
    const auto number = [&](std::string_view property, double value) {
      edits.push_back({node, std::string{property}, value, std::nullopt});
    };
    const auto text = [&](std::string_view property, std::string value) {
      edits.push_back({node, std::string{property}, 0.0, std::move(value)});
    };
    if (!draft.name.empty() && draft.name != authored->name) {
      text(scene::schema::tracking_source_name, draft.name);
    }
    // Protocol first: switching to NatNet fills Motive's ports where none
    // were authored, and the explicit ports below then win.
    if (now.protocol != was.protocol) {
      number(scene::schema::tracking_protocol,
             static_cast<double>(protocol_index(now.protocol)));
    }
    if (now.enabled != was.enabled) {
      number(scene::schema::tracking_enabled, now.enabled ? 1.0 : 0.0);
    }
    if (now.port != was.port) {
      number(scene::schema::tracking_port, static_cast<double>(now.port));
    }
    if (now.command_port != was.command_port) {
      number(scene::schema::tracking_command_port,
             static_cast<double>(now.command_port));
    }
    std::array<core::u8, 4U> server{};
    if (parse_server(draft.server, server) && server != was.server_address) {
      text(scene::schema::tracking_server_address, server_text(server));
    }
    std::array<core::u8, 4U> group{};
    if (scene::parse_multicast_group(draft.multicast, group) &&
        group != was.multicast_group) {
      text(scene::schema::tracking_multicast_group,
           scene::multicast_group_text(group));
    }
    if (now.sensor_height_mm != was.sensor_height_mm) {
      number(scene::schema::tracking_sensor_height_mm, now.sensor_height_mm);
    }
    if (now.sensor_aspect_ratio != was.sensor_aspect_ratio) {
      number(scene::schema::tracking_sensor_aspect, now.sensor_aspect_ratio);
    }
    if (now.fallback_focal_length_mm != was.fallback_focal_length_mm) {
      number(scene::schema::tracking_fallback_focal_mm,
             now.fallback_focal_length_mm);
    }
    if (now.calibration.axes != was.calibration.axes) {
      number(scene::schema::tracking_axes,
             now.calibration.axes == scene::TrackerAxes::ZUpRightHanded ? 1.0
                                                                        : 0.0);
    }
    if (now.calibration.heading != was.calibration.heading) {
      number(scene::schema::tracking_heading, now.calibration.heading);
    }
    if (now.calibration.origin.x != was.calibration.origin.x) {
      number(scene::schema::tracking_origin_x, now.calibration.origin.x);
    }
    if (now.calibration.origin.y != was.calibration.origin.y) {
      number(scene::schema::tracking_origin_y, now.calibration.origin.y);
    }
    if (now.calibration.origin.z != was.calibration.origin.z) {
      number(scene::schema::tracking_origin_z, now.calibration.origin.z);
    }
    if (now.calibration.scale != was.calibration.scale) {
      number(scene::schema::tracking_scale, now.calibration.scale);
    }
  }
  return edits;
}

void ProjectSettingsPanel::draw_feeds(gui::UiContext &ctx,
                                      gui::FormLayout &form,
                                      const ProjectSettingsState &state,
                                      ProjectSettingsFrame &result) {
  const gui::Metrics &m = ctx.theme().metrics;
  const gui::Rect feeds_heading = form.heading();
  ctx.label(feeds_heading, "TRACKING FEEDS", ctx.theme().accent);
  ctx.tooltip(feeds_heading,
              "Each feed is one tracking system the Director listens to. "
              "Pick a camera's feed and rigid body in the Inspector.");
  {
    gui::Rect strip = form.control();
    if (preferences_button(ctx, gui::ui_id(id_, "feeds.add"),
                           gui::cut_left(strip, m.button_width), "NEW FEED",
                           true)) {
      result.add_tracking_source = true;
    }
  }
  if (feed_drafts_.empty()) {
    ctx.label(form.caption(), "No feeds yet.", ctx.theme().text_muted);
    return;
  }

  for (FeedDraft &draft : feed_drafts_) {
    const core::u64 salt = draft.source.node.value();
    scene::TrackingSourceSettings &settings = draft.source.settings;
    static_cast<void>(gui::draw_form_section(
        ctx, form, draft.name.empty() ? draft.source.name : draft.name,
        ctx.theme().accent));

    const auto text_row = [&](std::string_view label, std::string_view key,
                              std::string &value, bool valid,
                              std::string_view tip) {
      const gui::FormRow row = form.row();
      ctx.label(row.label, label, ctx.theme().text);
      static_cast<void>(
          ctx.text_field(gui::ui_id(id_, key, salt), row.field, value));
      ctx.tooltip(row.field, tip);
      if (!valid) {
        const gui::Rect note = form.note();
        ctx.label(note, "Not an IP address", ctx.theme().warning);
        ctx.tooltip(note, "Four numbers 0-255 separated by dots");
      }
    };
    const auto port_row = [&](std::string_view label, std::string_view key,
                              core::u32 &value) {
      const gui::FormRow row = form.row();
      ctx.label(row.label, label, ctx.theme().text);
      double edited{};
      if (tracking_fields_.draw(ctx, gui::ui_id(id_, key, salt), row.field, "",
                                static_cast<double>(value), 1.0, edited, 0) &&
          std::isfinite(edited)) {
        value = static_cast<core::u32>(std::llround(std::clamp(
            edited, 0.0, static_cast<double>(scene::max_tracking_port))));
      }
    };
    const auto float_row = [&](std::string_view label, std::string_view key,
                               float &value, double speed, int decimals) {
      const gui::FormRow row = form.row();
      ctx.label(row.label, label, ctx.theme().text);
      double edited{};
      if (tracking_fields_.draw(ctx, gui::ui_id(id_, key, salt), row.field, "",
                                static_cast<double>(value), speed, edited,
                                decimals) &&
          std::isfinite(edited)) {
        value = static_cast<float>(edited);
      }
    };
    const auto choice_row = [&](std::string_view label, std::string_view key,
                                std::span<const std::string_view> choices,
                                std::size_t &selected) {
      const gui::FormRow row = form.row();
      ctx.label(row.label, label, ctx.theme().text);
      return ctx.dropdown(gui::ui_id(id_, key, salt), row.field, choices,
                          selected);
    };

    text_row("Name", "feeds.name", draft.name, true,
             "What a camera's feed list calls it");
    static_cast<void>(ctx.checkbox(gui::ui_id(id_, "feeds.enabled", salt),
                                   form.control(), settings.enabled,
                                   "Listening"));
    std::size_t protocol = protocol_index(settings.protocol);
    if (choice_row("Protocol", "feeds.protocol", feed_protocol_choices,
                   protocol)) {
      const scene::TrackerProtocolKind chosen = protocol_at(protocol);
      if (chosen == scene::TrackerProtocolKind::NatNet &&
          settings.protocol != chosen && settings.port == 0U) {
        // Motive's own defaults, so a stock install needs nothing typed.
        settings.port = scene::natnet_default_data_port;
        settings.command_port = scene::natnet_default_command_port;
      }
      settings.protocol = chosen;
    }
    const bool natnet =
        settings.protocol == scene::TrackerProtocolKind::NatNet;
    port_row(natnet ? "Data port" : "Port", "feeds.port", settings.port);
    if (natnet) {
      std::array<core::u8, 4U> parsed{};
      text_row("Motive IP", "feeds.server", draft.server,
               parse_server(draft.server, parsed),
               "The Motive machine's address. Leave empty to find it from "
               "the first frame.");
      port_row("Command port", "feeds.command", settings.command_port);
      text_row("Multicast", "feeds.multicast", draft.multicast,
               scene::parse_multicast_group(draft.multicast, parsed),
               "The group Motive streams to, from its Streaming pane. "
               "239.255.42.99 is its default.");
    }
    const TrackingFeedObservation *const observed =
        find_feed(state.feeds, draft.source.node);
    const bool faulted =
        observed != nullptr &&
        (observed->state == TrackingLinkState::BindFailed ||
         (!observed->last_error.empty() && observed->accepted == 0U &&
          observed->state != TrackingLinkState::Idle));
    {
      const gui::FormRow row = form.row();
      ctx.label(row.label, "Status", ctx.theme().text);
      ctx.label(row.field, feed_status_text(settings, observed),
                faulted ? ctx.theme().warning : ctx.theme().text_muted);
    }

    float_row("Sensor mm", "feeds.sensor", settings.sensor_height_mm, 0.05, 2);
    float_row("Sensor AR", "feeds.aspect", settings.sensor_aspect_ratio, 0.005,
              3);
    float_row("Focal mm", "feeds.focal", settings.fallback_focal_length_mm,
              0.5, 1);
    std::size_t axes =
        settings.calibration.axes == scene::TrackerAxes::ZUpRightHanded ? 1U
                                                                        : 0U;
    if (choice_row("Up axis", "feeds.axes", feed_axes_choices, axes)) {
      settings.calibration.axes = axes == 1U
                                      ? scene::TrackerAxes::ZUpRightHanded
                                      : scene::TrackerAxes::StageNative;
    }
    float_row("Heading turns", "feeds.heading", settings.calibration.heading,
              0.001, 4);
    float_row("Origin X", "feeds.origin-x", settings.calibration.origin.x,
              0.01, 3);
    float_row("Origin Y", "feeds.origin-y", settings.calibration.origin.y,
              0.01, 3);
    float_row("Origin Z", "feeds.origin-z", settings.calibration.origin.z,
              0.01, 3);
    float_row("Scale", "feeds.scale", settings.calibration.scale, 0.005, 3);
    settings.calibration = scene::clamped(settings.calibration);
  }
}

void ProjectSettingsPanel::open(
    const scene::EffectSettings &settings,
    const scene::RendererSettings &renderer,
    const std::optional<scene::LensProfile> &lens_profile,
    const scene::Environment &environment) {
  draft_ = scene::clamped(settings);
  renderer_draft_ = scene::clamped(renderer);
  environment_draft_ = environment;
  lens_draft_ =
      lens_profile.has_value()
          ? std::optional<scene::LensProfile>{scene::clamped(*lens_profile)}
          : std::nullopt;
  category_ = Category::Tracking;
  // Adopted from the state on the first frame drawn, like a feed added
  // while the dialog is open.
  feed_drafts_.clear();
  scroll_.reset();
  open_ = true;
}

ProjectSettingsFrame
ProjectSettingsPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const ProjectSettingsState &state) {
  ProjectSettingsFrame result{};
  if (!open_) {
    return result;
  }
  const gui::Metrics &m = ctx.theme().metrics;
  // Feeds follow the document: a removed one leaves the draft, and one added
  // (NEW FEED, or by undo) joins it as authored.
  std::erase_if(feed_drafts_, [&state](const FeedDraft &draft) {
    return std::none_of(state.tracking_sources.begin(),
                        state.tracking_sources.end(),
                        [&draft](const scene::TrackingSource &source) {
                          return source.node == draft.source.node;
                        });
  });
  for (const scene::TrackingSource &source : state.tracking_sources) {
    const bool drafted = std::any_of(
        feed_drafts_.begin(), feed_drafts_.end(),
        [&source](const FeedDraft &draft) {
          return draft.source.node == source.node;
        });
    if (!drafted) {
      feed_drafts_.push_back(FeedDraft{
          source, source.name, server_text(source.settings.server_address),
          scene::multicast_group_text(source.settings.multicast_group)});
    }
  }
  const ModalCard modal = begin_modal(
      ctx, bounds, project_settings_card_height, "PROJECT SETTINGS");

  // The intro and the category tabs stay put; only the category's pane
  // scrolls.
  gui::Rect body = modal.body;
  gui::FormLayout top{body, m};
  // A focused category is much quicker to audit in a dark control room than a
  // very tall scrolling form. Keep each tab visible and leave Apply/Cancel
  // fixed at the bottom of the modal.
  const gui::Rect tabs = top.control();
  static_cast<void>(gui::cut_top(body, top.extent()));
  ctx.panel(gui::inset(gui::cut_top(body, m.divider), m.inset, 0.0F),
            ctx.theme().divider);

  // Tracking setup and lens calibration share the modal; scene controls live
  // in the Inspector.
  constexpr std::size_t category_count = 3U;
  const auto tab = [&](Category category, std::string_view id,
                       std::string_view label, std::size_t index) {
    const gui::Rect tab_bounds =
        gui::column(tabs, index, category_count, m.gap);
    const bool selected = category_ == category;
    const gui::Interaction interaction = ctx.grab(gui::ui_id(id_, id), tab_bounds);
    const gui::UiColor fill =
        selected ? ctx.theme().selection
                 : (interaction.held && !interaction.released
                        ? ctx.theme().widget_active
                        : (interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().widget));
    ctx.panel(tab_bounds, fill);
    ctx.outline(tab_bounds,
                selected ? ctx.theme().accent : ctx.theme().divider);
    ctx.label_centered(tab_bounds, label,
                       selected ? ctx.theme().text : ctx.theme().text_muted);
    if (interaction.clicked && !selected) {
      // A numeric field can own the keyboard while it is being typed. Its
      // pane is disappearing, so explicitly give that focus back first.
      tracking_fields_.cancel(ctx);
      category_ = category;
      scroll_.reset();
    }
  };
  tab(Category::Tracking, "project.tab.tracking", "TRACKING", 0U);
  tab(Category::Feeds, "project.tab.feeds", "FEEDS", 1U);
  tab(Category::Lens, "project.tab.lens", "LENS", 2U);

  gui::FormLayout form{body, m, scroll_.begin(ctx, body)};

  if (category_ == Category::Tracking) {
    ctx.label(form.heading(), "INNER FRUSTUM", ctx.theme().accent);
    static_cast<void>(ctx.checkbox(
        gui::ui_id(id_, "icvfx.enabled"), form.control(),
        renderer_draft_.icvfx.inner_frustum,
        "Draw tracked cameras' plates on the wall"));

    const auto unsigned_row = [&](std::string_view label, std::string_view id,
                                  core::u32 &value, core::u32 maximum,
                                  std::string_view note) {
      const gui::FormRow row = form.row();
      ctx.label(row.label, label, ctx.theme().text);
      ctx.tooltip(row.label, note);
      double edited{};
      if (tracking_fields_.draw(ctx, gui::ui_id(id_, id), row.field, "",
                                static_cast<double>(value), 1.0, edited, 0) &&
          std::isfinite(edited)) {
        const double bounded =
            std::clamp(edited, 0.0, static_cast<double>(maximum));
        value = static_cast<core::u32>(std::llround(bounded));
      }
    };
    unsigned_row("Latency", "icvfx.latency",
                 renderer_draft_.icvfx.latency_frames,
                 scene::max_icvfx_latency_frames,
                 "Control frames ahead; prediction estimates the pose at this stamp.");
    static_cast<void>(ctx.checkbox(gui::ui_id(id_, "icvfx.predict"), form.control(),
                                   renderer_draft_.icvfx.pose_prediction,
                                   "Predict camera motion on the Director"));
    unsigned_row("Fit samples", "icvfx.prediction-window",
                 renderer_draft_.icvfx.prediction_window,
                 scene::max_pose_prediction_window,
                 "2-16 fresh samples. Longer fits smooth more and react slower.");
    renderer_draft_.icvfx.prediction_window =
        std::max(renderer_draft_.icvfx.prediction_window, 2U);
    unsigned_row("Jitter filter %", "icvfx.jitter-filter",
                 renderer_draft_.icvfx.jitter_filter_percent, 100U,
                 "0 keeps raw motion; 100 uses the fitted pose. See Live Data errors.");

    const gui::FormSection defaults = gui::draw_form_section(
        ctx, form, "NEW CAMERA DEFAULTS", ctx.theme().accent);
    ctx.tooltip(defaults.heading,
                "A camera keeps its own values once added; edit them in the "
                "Inspector.");
    unsigned_row("Margin", "icvfx.margin",
                 renderer_draft_.icvfx.camera_defaults.margin_px,
                 scene::max_icvfx_margin_px,
                 "Pixels of plate drawn beyond the camera's own frustum.");
    unsigned_row("Soft edge", "icvfx.soft-edge",
                 renderer_draft_.icvfx.camera_defaults.soft_edge_px,
                 scene::max_icvfx_soft_edge_px,
                 "Width of the fade from plate to the outer frustum.");
    unsigned_row("Plate scale", "icvfx.scale",
                 renderer_draft_.icvfx.camera_defaults.resolution_numerator,
                 scene::max_icvfx_resolution_scale,
                 "Plate resolution numerator; the denominator is below.");
    unsigned_row("Plate divisor", "icvfx.divisor",
                 renderer_draft_.icvfx.camera_defaults.resolution_denominator,
                 scene::max_icvfx_resolution_scale,
                 "Plate resolution denominator. Equal values render 1:1.");
  }

  if (category_ == Category::Feeds) {
    draw_feeds(ctx, form, state, result);
  }

  // Lens controls deliberately author only calibration. Zoom/focus/iris are
  // sampled controls carried beside the tracked pose, so making them sliders in
  // this dialog would tempt an operator to put live frame state in the project.
  if (category_ == Category::Lens) {
    ctx.label(form.heading(), "LENS CALIBRATION", ctx.theme().accent);
    const bool camera_available = state.camera_node != doc::NodeId{};
    const bool lens_enabled = lens_draft_.has_value();
    constexpr float lens_choice_width = 116.0F;
    const gui::FormRow projection = form.row();
    ctx.label(projection.label, "Projection",
              camera_available ? ctx.theme().text : ctx.theme().text_muted);
    gui::Rect choices = projection.field;
    if (preferences_button(ctx, gui::ui_id(id_, "lens.calibrated"),
                           gui::cut_left(choices, lens_choice_width),
                           "CALIBRATED", camera_available && !lens_enabled)) {
      lens_draft_ = scene::LensProfile{};
    }
    static_cast<void>(gui::cut_left(choices, m.gap));
    if (preferences_button(ctx, gui::ui_id(id_, "lens.legacy"),
                           gui::cut_left(choices, lens_choice_width),
                           "LEGACY FOV", camera_available && lens_enabled)) {
      lens_draft_.reset();
    }
    if (camera_available) {
      ctx.tooltip(projection.label,
                  "Sensor and distortion are calibration; live encoders are "
                  "status.");
    } else {
      ctx.label(form.note(), "No stage camera", ctx.theme().text_muted);
    }

    if (lens_draft_.has_value()) {
      scene::LensProfile &lens = *lens_draft_;
      // Two scalars a row, each a label, a slider and its value; the columns
      // are a card margin apart.
      constexpr float lens_label_width = 66.0F;
      constexpr float lens_value_width = 48.0F;
      constexpr float lens_track_height = 18.0F;
      const auto lens_scalar = [&](gui::Rect cell, std::string_view label,
                                   float &value, float minimum, float maximum,
                                   const char *format) {
        ctx.label(gui::cut_left(cell, lens_label_width), label,
                  ctx.theme().text);
        const gui::Rect shown = gui::cut_right(cell, lens_value_width);
        static_cast<void>(gui::cut_right(cell, m.gap));
        static_cast<void>(ctx.slider(
            gui::ui_id(id_, label), gui::centered(cell, cell.width, lens_track_height),
            value, minimum, maximum));
        std::array<char, 24U> text{};
        const int length = std::snprintf(text.data(), text.size(), format,
                                         static_cast<double>(value));
        ctx.label(shown,
                  std::string_view{text.data(),
                                   static_cast<std::size_t>(std::max(length, 0))},
                  ctx.theme().keyframe);
      };
      const auto lens_row = [&](std::string_view left_label, float &left_value,
                                float left_minimum, float left_maximum,
                                const char *left_format,
                                std::string_view right_label,
                                float &right_value, float right_minimum,
                                float right_maximum, const char *right_format) {
        const gui::Rect strip = form.control();
        lens_scalar(gui::column(strip, 0U, 2U, m.inset), left_label,
                    left_value, left_minimum, left_maximum, left_format);
        lens_scalar(gui::column(strip, 1U, 2U, m.inset), right_label,
                    right_value, right_minimum, right_maximum, right_format);
      };
      lens_row("Sensor W", lens.sensor_width_mm,
               scene::min_lens_sensor_extent_mm,
               scene::max_lens_sensor_extent_mm, "%.1f", "Sensor H",
               lens.sensor_height_mm, scene::min_lens_sensor_extent_mm,
               scene::max_lens_sensor_extent_mm, "%.1f");
      lens_row("Focal", lens.focal_length_mm, scene::min_lens_focal_length_mm,
               scene::max_lens_focal_length_mm, "%.1f", "Pupil m",
               lens.entrance_pupil_offset_m, -scene::max_lens_pupil_offset_m,
               scene::max_lens_pupil_offset_m, "%.3f");
      lens_row("K1", lens.distortion.k1,
               -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f", "K2",
               lens.distortion.k2, -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f");
      lens_row("K3", lens.distortion.k3,
               -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f", "P1",
               lens.distortion.p1, -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f");
      const gui::Rect last = form.control();
      lens_scalar(gui::column(last, 0U, 2U, m.inset), "P2", lens.distortion.p2,
                  -scene::max_lens_distortion_coefficient,
                  scene::max_lens_distortion_coefficient, "%.3f");
      std::array<char, 48U> calibration_text{};
      const int calibration_length =
          std::snprintf(calibration_text.data(), calibration_text.size(),
                        "Imported table: %u points", lens.calibration_count);
      ctx.label(gui::column(last, 1U, 2U, m.inset),
                std::string_view{
                    calibration_text.data(),
                    static_cast<std::size_t>(std::max(calibration_length, 0))},
                ctx.theme().text_muted);
    }
  }
  scroll_.end(ctx, form.extent());

  draft_ = scene::clamped(draft_);
  renderer_draft_ = scene::clamped(renderer_draft_);
  if (lens_draft_.has_value()) {
    lens_draft_ = scene::clamped(*lens_draft_);
  }

  const bool settings_changed = draft_ != state.settings ||
                                renderer_draft_ != state.renderer ||
                                environment_draft_ != state.environment ||
                                lens_draft_ != state.lens_profile;
  std::vector<ProjectFeedEdit> pending_feed_edits = feed_edits(state);
  const bool changed = settings_changed || !pending_feed_edits.empty();
  gui::Rect buttons = modal.footer;
  const gui::Rect defaults_button = gui::cut_left(buttons, m.button_width);
  const gui::Rect apply_button = gui::cut_right(buttons, m.button_width);
  static_cast<void>(gui::cut_right(buttons, m.gap));
  const gui::Rect cancel_button = gui::cut_right(buttons, m.button_width);
  const bool reset = preferences_button(
      ctx, gui::ui_id(id_, "project.reset"), defaults_button, "DEFAULTS",
      renderer_draft_.icvfx != scene::IcvfxSettings{} ||
          lens_draft_.has_value());
  const bool cancelled = preferences_button(
      ctx, gui::ui_id(id_, "project.cancel"), cancel_button, "CANCEL", true);
  const bool applied = preferences_button(ctx, gui::ui_id(id_, "project.apply"),
                                          apply_button, "APPLY", changed);
  const bool dismissed =
      end_modal(ctx, gui::ui_id(id_, "project.scrim"), bounds, modal);

  if (reset) {
    tracking_fields_.cancel(ctx);
    renderer_draft_.icvfx = scene::IcvfxSettings{};
    lens_draft_.reset();
    return result;
  }
  if (applied) {
    if (settings_changed) {
      result.apply = ProjectSettingsValues{
          draft_, renderer_draft_, environment_draft_, lens_draft_};
    }
    result.feed_edits = std::move(pending_feed_edits);
  }
  if (applied || cancelled || dismissed) {
    tracking_fields_.cancel(ctx);
    open_ = false;
  }
  return result;
}

} // namespace mediaserver::editor
