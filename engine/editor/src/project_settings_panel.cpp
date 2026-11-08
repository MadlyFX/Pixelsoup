#include <mediaserver/editor/project_settings_panel.hpp>

#include "workspace_panel_helpers.hpp"

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

} // namespace

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
  const ModalCard modal = begin_modal(
      ctx, bounds, project_settings_card_height, "PROJECT SETTINGS");

  // The intro and the category tabs stay put; only the category's pane
  // scrolls.
  gui::Rect body = modal.body;
  gui::FormLayout top{body, m};
  ctx.label(top.caption(),
            "Saved in the project. Applies to every scene and every node.",
            ctx.theme().text_muted);
  // A focused category is much quicker to audit in a dark control room than a
  // very tall scrolling form. Keep each tab visible and leave Apply/Cancel
  // fixed at the bottom of the modal.
  const gui::Rect tabs = top.control();
  static_cast<void>(gui::cut_top(body, top.extent()));
  ctx.panel(gui::inset(gui::cut_top(body, m.divider), m.inset, 0.0F),
            ctx.theme().divider);

  // Tracking setup and lens calibration share the modal; scene controls live
  // in the Inspector.
  constexpr std::size_t category_count = 2U;
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
  tab(Category::Lens, "project.tab.lens", "LENS", 1U);

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
      double edited{};
      if (tracking_fields_.draw(ctx, gui::ui_id(id_, id), row.field, "",
                                static_cast<double>(value), 1.0, edited, 0) &&
          std::isfinite(edited)) {
        const double bounded =
            std::clamp(edited, 0.0, static_cast<double>(maximum));
        value = static_cast<core::u32>(std::llround(bounded));
      }
      ctx.label(form.note(), note, ctx.theme().text_muted);
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

    static_cast<void>(gui::draw_form_section(
        ctx, form, "NEW CAMERA DEFAULTS", ctx.theme().accent));
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
    ctx.label(form.caption(),
              "A camera keeps its own values once added; edit them in the "
              "Inspector.",
              ctx.theme().text_muted);
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
    ctx.label(form.note(),
              camera_available
                  ? "Sensor and distortion are calibration; live encoders are status."
                  : "No active stage camera is available for lens calibration.",
              ctx.theme().text_muted);

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

  const bool changed = draft_ != state.settings ||
                       renderer_draft_ != state.renderer ||
                       environment_draft_ != state.environment ||
                       lens_draft_ != state.lens_profile;
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
    result.apply = ProjectSettingsValues{
        draft_, renderer_draft_, environment_draft_, lens_draft_};
  }
  if (applied || cancelled || dismissed) {
    tracking_fields_.cancel(ctx);
    open_ = false;
  }
  return result;
}

} // namespace mediaserver::editor
