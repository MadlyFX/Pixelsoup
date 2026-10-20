#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// The Timeline's KEYS popup: fitting the selection or the whole animation,
// the active channel's visibility and its value at the playhead, and the
// rational time scale of the selected keys with its Director preview.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::draw_key_tools(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::optional<TimelineKeyScalePreview> &key_scale_preview =
      p.input.key_scale_preview;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &graph = p.layout.graph;
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;
  auto &key_tools_popup = p.key_tools_popup;
  auto &scale_numerator_id = p.scale_numerator_id;
  auto &scale_denominator_id = p.scale_denominator_id;
  auto &scale_user_anchor_id = p.scale_user_anchor_id;

  if (scratch.key_popup_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (key_tools_.open) {
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
                   keys_.lasso_mode ? "LASSO" : "BOX")) {
      keys_.lasso_mode = !keys_.lasso_mode;
      keys_.marquee_tracking = false;
      keys_.marquee_active = false;
      keys_.lasso_points.clear();
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
        time_axis_.fit(fitted_begin, fitted_span, graph.width);
        // And the values they hold, bottom to top of the Curves graph.
        double lowest = std::numeric_limits<double>::infinity();
        double highest = -std::numeric_limits<double>::infinity();
        for (const TimelineKeyAddress &key : keys) {
          const scene::ChannelKeyframes *const channel =
              channel_for(keyframes, key.target, key.property);
          if (channel == nullptr ||
              channel->values.size() != channel->frames.size()) {
            continue;
          }
          const auto found = std::find(channel->frames.begin(),
                                       channel->frames.end(), key.frame);
          if (found != channel->frames.end()) {
            const double value = channel->values[static_cast<std::size_t>(
                found - channel->frames.begin())];
            if (std::isfinite(value)) {
              lowest = std::min(lowest, value);
              highest = std::max(highest, value);
            }
          }
        }
        if (lowest <= highest) {
          graph_->fit_values(lowest, highest);
        }
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
      time_axis_.fit(static_cast<double>(first_i), span, graph.width);
      // Curves mode fits to what it already shows; otherwise the graph fits
      // when it is next drawn.
      if (graph_mode_ && !p.graph_channels.empty()) {
        graph_->fit(p.graph_channels, first_i, last_i);
      } else {
        graph_->request_fit();
      }
    }

    const float channel_y = tool_y + 28.0F;
    if (graph_->active().has_value()) {
      std::array<char, 128U> active_label{};
      const int active_length = std::snprintf(
          active_label.data(), active_label.size(), "%s - #%llu",
          channel_label(graph_->active()->property).data(),
          static_cast<unsigned long long>(graph_->active()->target.value()));
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, channel_y, 170.0F, 22.0F},
                std::string_view{
                    active_label.data(),
                    static_cast<std::size_t>(std::max(active_length, 0))},
                channel_color(graph_->active()->property));
      const bool hidden = !graph_->visible(*graph_->active());
      if (ctx.button(
              mix(id_, "active channel visibility"),
              gui::Rect{key_tools_popup.x + 178.0F, channel_y, 50.0F, 22.0F},
              hidden ? "SHOW" : "HIDE")) {
        if (hidden) {
          graph_->set_visible(*graph_->active(), true);
        } else {
          graph_->set_visible(*graph_->active(), false);
          keys_.selected.erase(
              std::remove_if(keys_.selected.begin(), keys_.selected.end(),
                             [&](const SelectedKey &key) {
                               return key.target == graph_->active()->target &&
                                      key.component ==
                                          graph_->active()->property;
                             }),
              keys_.selected.end());
          keys_.drag_keys.erase(
              std::remove_if(keys_.drag_keys.begin(), keys_.drag_keys.end(),
                             [&](const SelectedKey &key) {
                               return key.target == graph_->active()->target &&
                                      key.component ==
                                          graph_->active()->property;
                             }),
              keys_.drag_keys.end());
          if (keys_.drag_keys.empty()) {
            keys_.dragging = false;
            keys_.duplicate = false;
          }
        }
      }
      const scene::ChannelKeyframes *const active_keyframes = channel_for(
          keyframes, graph_->active()->target, graph_->active()->property);
      const std::optional<double> sampled =
          sample_channel_at(active_keyframes, timeline.show_frame);
      if (sampled.has_value() && std::isfinite(*sampled)) {
        double changed_value = *sampled;
        const double speed = std::max(std::fabs(*sampled) * 0.01, 0.01);
        if (key_tools_.value_field.draw(
                ctx, mix(id_, "playhead key value"),
                gui::Rect{key_tools_popup.x + 232.0F, channel_y,
                          key_tools_popup.width - 238.0F, 22.0F},
                "", *sampled, speed, changed_value, 4)) {
          frame.keys.push_back(TimelineKeyEdit{
              graph_->active()->target, graph_->active()->property,
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
              key_tools_.scale_open ? "SCALE*" : "SCALE")) {
        key_tools_.scale_open = !key_tools_.scale_open;
      }
    } else {
      const gui::Rect disabled{key_tools_popup.x + 6.0F, scale_button_y, 62.0F,
                               22.0F};
      ctx.panel(disabled, ctx.theme().track);
      ctx.label(disabled, "SCALE", ctx.theme().text_muted);
      key_tools_.scale_open = false;
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

    if (key_tools_.scale_open) {
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
                  key_tools_.scale_anchor == anchors[index].first
                      ? ctx.theme().selection
                      : (interaction.hovered ? ctx.theme().widget_hover
                                             : ctx.theme().widget));
        ctx.label(anchor_bounds, anchors[index].second,
                  key_tools_.scale_anchor == anchors[index].first
                      ? ctx.theme().text
                      : ctx.theme().text_muted);
        if (interaction.clicked) {
          key_tools_.scale_anchor = anchors[index].first;
        }
        anchor_x += anchor_width + 3.0F;
      }

      const float rational_y = anchor_y + 28.0F;
      ctx.label(gui::Rect{key_tools_popup.x + 6.0F, rational_y, 18.0F, 22.0F},
                "N", ctx.theme().text_muted);
      static_cast<void>(ctx.text_field(
          scale_numerator_id,
          gui::Rect{key_tools_popup.x + 24.0F, rational_y, 62.0F, 22.0F},
          key_tools_.scale_numerator));
      ctx.label(gui::Rect{key_tools_popup.x + 91.0F, rational_y, 18.0F, 22.0F},
                "D", ctx.theme().text_muted);
      static_cast<void>(ctx.text_field(
          scale_denominator_id,
          gui::Rect{key_tools_popup.x + 109.0F, rational_y, 62.0F, 22.0F},
          key_tools_.scale_denominator));
      ctx.label(gui::Rect{key_tools_popup.x + 177.0F, rational_y, 43.0F, 22.0F},
                "MARK", ctx.theme().text_muted);
      static_cast<void>(
          ctx.text_field(scale_user_anchor_id,
                         gui::Rect{key_tools_popup.x + 220.0F, rational_y,
                                   key_tools_popup.width - 226.0F, 22.0F},
                         key_tools_.scale_user_anchor));

      const std::optional<core::i64> numerator =
          parse_integer(key_tools_.scale_numerator);
      const std::optional<core::i64> denominator =
          parse_integer(key_tools_.scale_denominator);
      const std::optional<core::i64> user_anchor =
          parse_integer(key_tools_.scale_user_anchor);
      const bool rational_valid =
          numerator.has_value() && denominator.has_value() &&
          *denominator > 0 &&
          (key_tools_.scale_anchor != scene::KeyTimeAnchor::User ||
           user_anchor.has_value());
      std::optional<core::u64> signature;
      scene::KeyTimeScale scale{};
      if (rational_valid) {
        scale.numerator = *numerator;
        scale.denominator = *denominator;
        scale.anchor = key_tools_.scale_anchor;
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
          key_tools_.scale_open = false;
        }
      } else {
        ctx.panel(apply_bounds, ctx.theme().track);
        ctx.label(apply_bounds, "APPLY", ctx.theme().text_muted);
      }
    }
  }
}

} // namespace mediaserver::editor
