#include <mediaserver/editor/timeline_panel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

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
constexpr float minimum_pixels_per_frame = 0.02F;
constexpr float maximum_pixels_per_frame = 200.0F;

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

[[nodiscard]] bool same_rate(const EditorTimeline& timeline,
                             ShowRateRequest rate) noexcept {
    return timeline.rate_numerator == rate.numerator &&
           timeline.rate_denominator == rate.denominator;
}

[[nodiscard]] std::string rate_label(const EditorTimeline& timeline) {
    const auto found = std::find_if(
        common_rates.begin(), common_rates.end(),
        [&](const RateChoice& choice) { return same_rate(timeline, choice.rate); });
    if (found != common_rates.end()) {
        return std::string{found->label};
    }

    std::array<char, 48U> text{};
    const int length = timeline.rate_denominator == 1U
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

void draw_twirl(gui::UiContext& ctx, float cx, float cy, bool open,
                gui::UiColor color) {
    auto& draw = ctx.draw_list();
    if (open) {
        draw.add_triangle(cx - 4.0F, cy - 2.5F, cx + 4.0F, cy - 2.5F, cx,
                          cy + 3.0F, color);
    } else {
        draw.add_triangle(cx - 2.5F, cy - 4.0F, cx + 3.0F, cy, cx - 2.5F,
                          cy + 4.0F, color);
    }
}

void draw_diamond(gui::UiContext& ctx, float cx, float cy, float radius,
                  gui::UiColor color) {
    const std::array<gui::UiPoint, 4U> points{{
        {cx, cy - radius},
        {cx + radius, cy},
        {cx, cy + radius},
        {cx - radius, cy},
    }};
    ctx.draw_list().add_convex_poly(points, color);
}

void draw_circle(gui::UiContext& ctx, float cx, float cy, float radius,
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

[[nodiscard]] bool key_button(gui::UiContext& ctx, gui::UiId id,
                              const gui::Rect& bounds, bool animated,
                              bool keyed_here, gui::UiColor background) {
    const gui::Interaction interaction = ctx.grab(id, bounds);
    gui::UiColor color = keyed_here
        ? ctx.theme().playhead
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

[[nodiscard]] std::string layer_name(const ElementInfo& element) {
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

[[nodiscard]] const scene::ChannelKeyframes*
channel_for(std::span<const scene::ChannelKeyframes> keyframes,
            doc::NodeId node,
            scene::TransformChannel component) noexcept {
    for (const scene::ChannelKeyframes& entry : keyframes) {
        if (entry.target == node && entry.component == component) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<double>
sample_channel_at(const scene::ChannelKeyframes* channel,
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
    const std::size_t hi = static_cast<std::size_t>(
        std::distance(channel->frames.begin(), after));
    const std::size_t lo = hi - 1U;
    const scene::KeyInterpolation interpolation =
        lo < channel->interpolations.size()
            ? channel->interpolations[lo]
            : scene::KeyInterpolation::Linear;
    if (interpolation == scene::KeyInterpolation::Hold) {
        return channel->values[lo];
    }
    double t = static_cast<double>(wanted - channel->frames[lo]) /
               static_cast<double>(channel->frames[hi] - channel->frames[lo]);
    if (interpolation == scene::KeyInterpolation::Cubic) {
        t = t * t * (3.0 - 2.0 * t);
    }
    return channel->values[lo] +
           (channel->values[hi] - channel->values[lo]) * t;
}

[[nodiscard]] const std::vector<core::i64>*
frames_for(std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
           scene::TransformChannel component) noexcept {
    const scene::ChannelKeyframes* const channel =
        channel_for(keyframes, node, component);
    return channel != nullptr ? &channel->frames : nullptr;
}

[[nodiscard]] const ElementInfo*
find_element(std::span<const ElementInfo> elements, doc::NodeId node) noexcept {
    for (const ElementInfo& element : elements) {
        if (element.node == node) {
            return &element;
        }
    }
    return nullptr;
}

[[nodiscard]] const output::ProjectionSurface* find_surface(
    std::span<const output::ProjectionSurface> surfaces,
    doc::NodeId node) noexcept {
    for (const output::ProjectionSurface& surface : surfaces) {
        if (surface.node == node) {
            return &surface;
        }
    }
    return nullptr;
}

[[nodiscard]] double surface_component_value(
    const output::ProjectionSurface& surface,
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
    constexpr std::array<core::i64, 18U> nice{
        {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1'200, 1'800, 3'600, 7'200,
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
};

[[nodiscard]] core::u64 group_id(doc::NodeId node, core::u8 group) noexcept {
    return node.value() * 4U + static_cast<core::u64>(group);
}

// A clip the operator can edit on the timeline: a media placement with a real
// duration. An unplaced clip is the legacy always-on form and keeps the plain
// full-range layer bar, because there is no in or out point to drag.
[[nodiscard]] bool editable_clip(const ElementInfo& element) noexcept {
    return !element.locked && element.kind == ElementKind::Media &&
           element.timing.placed();
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
    return std::find(selection.begin(), selection.end(), node) !=
           selection.end();
}

bool TimelinePanel::key_selected(const SelectedKey& key) const noexcept {
    return std::find(selected_keys_.begin(), selected_keys_.end(), key) !=
           selected_keys_.end();
}

void TimelinePanel::reset_project_state() noexcept {
    pixels_per_frame_ = 0.0F;
    left_frame_ = 0.0;
    time_view_initialized_ = false;
    row_scroll_ = 0.0F;
    expanded_layers_.clear();
    expanded_groups_.clear();
    search_.clear();
    range_fields_ = NumberField{};
    selected_keys_.clear();
    dragging_key_ = false;
    drag_keys_.clear();
    drag_anchor_frame_ = 0;
    drag_delta_ = 0;
    snap_guide_.reset();
    rate_menu_open_ = false;
    timeline_focused_ = false;
    panning_ = false;
    marquee_tracking_ = false;
    marquee_active_ = false;
    marquee_start_x_ = 0.0F;
    marquee_start_y_ = 0.0F;
    marquee_shift_ = false;
    marquee_ctrl_ = false;
    marquee_base_.clear();
    clip_drag_ = ClipDrag::None;
    clip_drag_node_ = doc::NodeId{};
    clip_drag_origin_ = scene::ClipTiming{};
    clip_drag_proposed_ = scene::ClipTiming{};
    clip_drag_anchor_ = 0;
    clip_snap_guide_.reset();
}

TimelinePanelFrame
TimelinePanel::draw(gui::UiContext& ctx, const gui::Rect& bounds,
                    const EditorTimeline& timeline,
                    std::span<const scene::ChannelKeyframes> keyframes,
                    std::span<const ElementInfo> elements,
                    std::span<const doc::NodeId> selection,
                    doc::NodeId primary,
                    std::span<const scene::EffectInstance> effects,
                    std::span<const output::ProjectionSurface> surfaces,
                    doc::NodeId selected_surface) {
    TimelinePanelFrame frame{};
    ctx.panel(bounds, ctx.theme().panel);
    if (ctx.pointer_pressed()) {
        timeline_focused_ =
            bounds.contains(ctx.pointer_x(), ctx.pointer_y());
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
    const ElementInfo* const anchor_info = find_element(elements, anchor);

    const float preferred_list =
        std::clamp(bounds.width * 0.26F, 180.0F, 260.0F);
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
        const double span = std::max(
            static_cast<double>(last) - static_cast<double>(first), 1.0);
        pixels_per_frame_ = std::clamp(
            static_cast<float>(static_cast<double>(graph.width) / span),
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
        return graph.x +
               static_cast<float>((value - left_frame_) *
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
    for (const ElementInfo& element : elements) {
        const std::string name = layer_name(element);
        if (!matches(name, search_) &&
            !matches(kind_name(element.kind), search_)) {
            continue;
        }
        const bool transformable = !is_light(element.kind);
        rows.push_back(Row{Row::Kind::Layer, element.node, 0U, 0U,
                           name, kind_color(element.kind),
                           element.kind, 0U, transformable});
        if (!transformable ||
            expanded_layers_.count(element.node.value()) == 0U) {
            continue;
        }
        for (core::u8 group = 0U; group < 3U; ++group) {
            rows.push_back(
                Row{Row::Kind::Group, element.node, group, 0U,
                    std::string{group_names[static_cast<std::size_t>(group)]},
                    ctx.theme().text_muted, element.kind, 1U, true});
            if (expanded_groups_.count(group_id(element.node, group)) == 0U) {
                continue;
            }
            for (core::u8 component = 0U; component < 3U; ++component) {
                rows.push_back(
                    Row{Row::Kind::Component, element.node, group, component,
                        std::string{component_names[static_cast<std::size_t>(
                            group)][static_cast<std::size_t>(component)]},
                        ctx.theme().text_muted, element.kind, 2U, false});
            }
        }
        rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
                           "Opacity", ctx.theme().text_muted, element.kind,
                           1U, false, scene::PropertyChannel::Opacity,
                           element.node, element.style.opacity});
        for (const scene::EffectInstance& effect : effects) {
            if (effect.target != element.node || !effect.enabled) {
                continue;
            }
            const bool brightness =
                effect.kind == scene::EffectKind::Brightness;
            const scene::PropertyChannel property =
                brightness ? scene::PropertyChannel::Brightness
                           : scene::PropertyChannel::BlurRadius;
            const auto sampled = sample_channel_at(
                channel_for(keyframes, effect.source, property),
                timeline.show_frame);
            const double displayed_value =
                sampled.has_value() && std::isfinite(*sampled)
                    ? *sampled
                    : static_cast<double>(effect.value);
            rows.push_back(Row{
                Row::Kind::Property, element.node, 0U, 0U,
                brightness ? "Brightness" : "Blur Radius",
                ctx.theme().keyframe, element.kind, 1U, false,
                property, effect.source, displayed_value});
        }
    }
    for (const output::ProjectionSurface& surface : surfaces) {
        if (!matches(surface.name, search_) &&
            !matches("LED output physical surface", search_)) {
            continue;
        }
        Row surface_row{Row::Kind::Layer, surface.node, 0U, 0U,
                        surface.name.empty() ? "Surface" : surface.name,
                        ctx.theme().accent, ElementKind::Object, 0U, true};
        surface_row.surface = true;
        rows.push_back(std::move(surface_row));
        if (expanded_layers_.count(surface.node.value()) == 0U) {
            continue;
        }

        Row position{Row::Kind::Group, surface.node, 0U, 0U, "Position",
                     ctx.theme().text_muted, ElementKind::Object, 1U, true};
        position.surface = true;
        position.component_count = 3U;
        rows.push_back(std::move(position));
        if (expanded_groups_.count(group_id(surface.node, 0U)) != 0U) {
            for (core::u8 component = 0U; component < 3U; ++component) {
                Row one{Row::Kind::Component, surface.node, 0U, component,
                        std::string{component_names[0U][component]},
                        ctx.theme().text_muted, ElementKind::Object, 2U,
                        false};
                one.surface = true;
                rows.push_back(std::move(one));
            }
        }

        Row rotation{Row::Kind::Group, surface.node, 1U, 0U, "Rotation",
                     ctx.theme().text_muted, ElementKind::Object, 1U, true};
        rotation.surface = true;
        rotation.component_count = 1U;
        rows.push_back(std::move(rotation));
        if (expanded_groups_.count(group_id(surface.node, 1U)) != 0U) {
            Row yaw{Row::Kind::Component, surface.node, 1U, 0U, "Yaw",
                    ctx.theme().text_muted, ElementKind::Object, 2U, false};
            yaw.surface = true;
            rows.push_back(std::move(yaw));
        }
    }
    for (Row& row : rows) {
        if (row.surface) {
            const output::ProjectionSurface* const surface =
                find_surface(surfaces, row.node);
            row.locked = surface != nullptr && surface->locked;
        } else {
            const ElementInfo* const element = find_element(elements, row.node);
            row.locked = element != nullptr && element->locked;
        }
    }

    const auto target_locked = [&](doc::NodeId target) noexcept {
        if (const ElementInfo* const element = find_element(elements, target)) {
            return element->locked;
        }
        if (const output::ProjectionSurface* const surface =
                find_surface(surfaces, target)) {
            return surface->locked;
        }
        const auto effect = std::find_if(
            effects.begin(), effects.end(), [target](const auto& candidate) {
                return candidate.source == target;
            });
        if (effect != effects.end()) {
            const ElementInfo* const element =
                find_element(elements, effect->target);
            return element != nullptr && element->locked;
        }
        return false;
    };
    selected_keys_.erase(
        std::remove_if(selected_keys_.begin(), selected_keys_.end(),
                       [&](const SelectedKey& key) {
                           return target_locked(key.target);
                       }),
        selected_keys_.end());
    drag_keys_.erase(
        std::remove_if(drag_keys_.begin(), drag_keys_.end(),
                       [&](const SelectedKey& key) {
                           return target_locked(key.target);
                       }),
        drag_keys_.end());
    if (drag_keys_.empty()) {
        dragging_key_ = false;
        drag_delta_ = 0;
        snap_guide_.reset();
    }

    const float content_height = static_cast<float>(rows.size()) * row_height;
    const float maximum_row_scroll =
        std::max(content_height - rows_bounds.height, 0.0F);

    // Wheel gestures have the compositor convention: over the graph, wheel
    // zooms about the pointer and Shift+wheel pans; over the layer list it
    // scrolls rows. This keeps the two axes independent.
    if (bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_y() >= ruler.y && ctx.wheel() != 0.0F) {
        if (ctx.shift_held()) {
            left_frame_ -= static_cast<double>(ctx.wheel()) * 80.0 /
                           static_cast<double>(pixels_per_frame_);
        } else if (ctx.pointer_x() >= graph.x) {
            const double under_pointer = x_to_frame(ctx.pointer_x());
            const double factor =
                std::pow(1.15, static_cast<double>(ctx.wheel()));
            pixels_per_frame_ =
                std::clamp(static_cast<float>(
                               static_cast<double>(pixels_per_frame_) * factor),
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
        row_scroll_ -= ctx.pointer_dy();
    }
    if (ctx.middle_pointer_released()) {
        panning_ = false;
    }
    left_frame_ = std::clamp(left_frame_, static_cast<double>(first),
                             static_cast<double>(last));
    row_scroll_ = std::clamp(row_scroll_, 0.0F, maximum_row_scroll);

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
        position_text.data(), position_text.size(),
        "%02llu:%02llu:%02llu:%02llu", static_cast<unsigned long long>(hours),
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(display_frame));
    const gui::Rect timecode{control_x + 7.0F, control_y, 150.0F,
                             button_height};
    ctx.panel(timecode, ctx.theme().track);
    ctx.panel(gui::Rect{timecode.x, timecode.y, 2.0F, timecode.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{timecode.x + 10.0F, timecode.y, timecode.width - 10.0F,
                        timecode.height},
              std::string_view{
                  position_text.data(),
                  static_cast<std::size_t>(std::max(position_length, 0))},
              ctx.theme().keyframe);
    control_x += 164.0F;

    constexpr float rate_width = 82.0F;
    constexpr float rate_gap = 6.0F;
    const float range_x = toolbar.x + toolbar.width - 282.0F;
    const bool show_range = bounds.width >= 700.0F &&
        control_x + rate_width + rate_gap <= range_x;
    const float rate_x = show_range
        ? range_x - rate_width - rate_gap
        : toolbar.x + toolbar.width - rate_width - 6.0F;
    const bool show_rate = rate_x >= control_x + control_gap;
    const float timeline_tools_end = show_rate ? rate_x - control_gap
                                                : toolbar.x + toolbar.width;
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
            const double span = std::max(
                static_cast<double>(last) - static_cast<double>(first), 1.0);
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
    // Splice. A cut is only offered when the playhead is actually inside a
    // selected clip and has a frame on each side of it, because a split that
    // produced a zero-length half would be a delete wearing the wrong name.
    const core::i64 playhead = signed_frame(timeline.show_frame);
    std::vector<doc::NodeId> splittable;
    for (const ElementInfo& element : elements) {
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
        !is_light(anchor_info->kind) &&
        control_x + 62.0F < timeline_tools_end) {
        if (ctx.button(mix(id_, "clear keys"),
                       gui::Rect{control_x, control_y, 60.0F, button_height},
                       "CLEAR")) {
            // Transform and opacity belong to the selected layer. Effect
            // parameters belong to their first-class effect nodes, so clear
            // those addresses explicitly instead of inventing brightness and
            // blur channels on the layer itself.
            for (core::u8 component = 0U;
                 component <= static_cast<core::u8>(
                                  scene::PropertyChannel::Opacity);
                 ++component) {
                frame.keys.push_back(TimelineKeyEdit{
                    anchor, static_cast<scene::TransformChannel>(component), 0U,
                    0.0, true});
            }
            for (const scene::EffectInstance& effect : effects) {
                if (effect.target != anchor) {
                    continue;
                }
                const scene::PropertyChannel property =
                    effect.kind == scene::EffectKind::Brightness
                        ? scene::PropertyChannel::Brightness
                        : scene::PropertyChannel::BlurRadius;
                frame.keys.push_back(TimelineKeyEdit{
                    effect.source, property, 0U, 0.0, true});
            }
        }
    }

    gui::Rect rate_button{};
    gui::Rect rate_popup{};
    bool popup_pointer_blocked = false;
    if (show_rate) {
        rate_button = gui::Rect{rate_x, control_y, rate_width, button_height};
        if (ctx.button(mix(id_, "show rate"), rate_button,
                       rate_label(timeline))) {
            rate_menu_open_ = !rate_menu_open_;
        }
        constexpr float popup_width = 104.0F;
        constexpr float choice_height = 21.0F;
        rate_popup = gui::Rect{
            rate_button.x + rate_button.width - popup_width,
            toolbar.y + toolbar.height - 2.0F,
            popup_width,
            8.0F + choice_height * static_cast<float>(common_rates.size())};
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
        ctx.label(gui::Rect{range_x, control_y, 18.0F, button_height}, "IN",
                  ctx.theme().text_muted);
        double changed_first = 0.0;
        const bool first_changed = range_fields_.draw(
            ctx, mix(id_, "range first"),
            gui::Rect{range_x + 20.0F, control_y, 64.0F, button_height}, "",
            static_cast<double>(first), 1.0, changed_first, 0);
        ctx.label(gui::Rect{range_x + 91.0F, control_y, 24.0F, button_height},
                  "OUT", ctx.theme().text_muted);
        double changed_last = 0.0;
        const bool last_changed = range_fields_.draw(
            ctx, mix(id_, "range last"),
            gui::Rect{range_x + 118.0F, control_y, 64.0F, button_height}, "",
            static_cast<double>(last), 1.0, changed_last, 0);
        bool loop = timeline.loop;
        const bool loop_changed = ctx.checkbox(
            mix(id_, "loop"),
            gui::Rect{range_x + 192.0F, control_y, 84.0F, button_height}, loop,
            "LOOP");
        const bool range_changed =
            first_changed || last_changed || loop_changed;
        if (range_changed) {
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

    // The left side of the ruler doubles as the layer filter, leaving row and
    // graph coordinates unchanged from the established timeline layout.
    ctx.panel(ruler, ctx.theme().header);
    ctx.panel(gui::Rect{ruler.x, ruler.y, list_width, ruler.height},
              ctx.theme().row_alt);
    const gui::Rect search_bounds{ruler.x + 5.0F, ruler.y + 2.0F,
                                  std::max(list_width - 10.0F, 0.0F),
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
    draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 1.7F,
                ctx.theme().widget);
    ctx.draw_list().add_line(search_bounds.x + 11.0F, search_cy + 2.0F,
                             search_bounds.x + 14.0F, search_cy + 5.0F, 1.0F,
                             ctx.theme().text_muted);
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
                  std::string_view{label.data(), static_cast<std::size_t>(
                                                     std::max(length, 0))},
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
        const core::i64 pointer_frame = signed_frame(
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last));
        core::i64 proposed_delta = pointer_frame - drag_anchor_frame_;

        core::i64 earliest = drag_anchor_frame_;
        core::i64 latest = drag_anchor_frame_;
        for (const SelectedKey& key : drag_keys_) {
            earliest = std::min(earliest, key.frame);
            latest = std::max(latest, key.frame);
        }
        proposed_delta = std::clamp(proposed_delta, first_i - earliest,
                                    last_i - latest);

        if (snapping_ && !ctx.alt_held() && !drag_keys_.empty()) {
            float best_distance = snap_distance + 1.0F;
            core::i64 best_adjustment = 0;
            std::optional<core::i64> best_target;
            const auto consider = [&](core::i64 moving,
                                      core::i64 target) noexcept {
                const core::i64 adjustment = target - moving;
                const float distance =
                    std::fabs(static_cast<float>(adjustment) *
                              pixels_per_frame_);
                if (distance <= snap_distance && distance < best_distance) {
                    best_distance = distance;
                    best_adjustment = adjustment;
                    best_target = target;
                }
            };

            for (const SelectedKey& moving : drag_keys_) {
                const core::i64 proposed = moving.frame + proposed_delta;
                consider(proposed, signed_frame(timeline.show_frame));
                consider(proposed, first_i);
                consider(proposed, last_i);
                for (const scene::ChannelKeyframes& channel : keyframes) {
                    for (const core::i64 candidate : channel.frames) {
                        const SelectedKey identity{channel.target,
                                                   channel.component,
                                                   candidate};
                        if (std::find(drag_keys_.begin(), drag_keys_.end(),
                                      identity) == drag_keys_.end()) {
                            consider(proposed, candidate);
                        }
                    }
                }
            }
            if (best_target.has_value()) {
                proposed_delta = std::clamp(
                    proposed_delta + best_adjustment, first_i - earliest,
                    last_i - latest);
                snap_guide_ = best_target;
            }
        }
        drag_delta_ = proposed_delta;
    }

    if (marquee_tracking_ && ctx.pointer_down()) {
        const float travel = std::fabs(ctx.pointer_x() - marquee_start_x_) +
                             std::fabs(ctx.pointer_y() - marquee_start_y_);
        marquee_active_ = marquee_active_ || travel > marquee_threshold;
    }

    // Every boundary a clip gesture may land on: the show range, the playhead,
    // and the ends of every other clip. Butt-joining two clips and cutting on
    // the playhead are the two edits a video timeline is asked for constantly,
    // and neither should require pixel accuracy.
    const auto snap_targets = [&](doc::NodeId exclude) {
        std::vector<core::i64> targets{first_i, last_i + 1,
                                       signed_frame(timeline.show_frame)};
        for (const ElementInfo& element : elements) {
            if (element.node == exclude || !editable_clip(element)) {
                continue;
            }
            targets.push_back(element.timing.start);
            targets.push_back(element.timing.start + element.timing.duration);
        }
        return targets;
    };

    // Resolve a live clip gesture before anything is drawn, so the bar under
    // the pointer is the proposal rather than the placement the document last
    // committed - which during a drag is always a few frames behind.
    if (clip_drag_ != ClipDrag::None) {
        if (!ctx.pointer_down()) {
            clip_drag_ = ClipDrag::None;
            clip_drag_node_ = doc::NodeId{};
            clip_snap_guide_.reset();
        } else {
            const core::i64 pointer_frame = static_cast<core::i64>(
                std::llround(x_to_frame(ctx.pointer_x())));
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

            switch (clip_drag_) {
            case ClipDrag::Move: {
                core::i64 start = snapped(origin.start + delta);
                if (!clip_snap_guide_.has_value()) {
                    // The tail is as good a thing to butt against as the head.
                    const core::i64 tail = origin.start + delta +
                        origin.duration;
                    const core::i64 tail_snapped = snapped(tail);
                    if (clip_snap_guide_.has_value()) {
                        start = tail_snapped - origin.duration;
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
                const core::i64 start = std::clamp(
                    snapped(origin.start + delta), first_i, limit);
                const core::i64 shift = start - origin.start;
                proposed.start = start;
                proposed.duration = origin.duration - shift;
                proposed.source_in = std::max(origin.source_in + shift,
                                              core::i64{0});
                break;
            }
            case ClipDrag::TrimOut: {
                const core::i64 end = std::max(
                    snapped(origin.start + origin.duration + delta),
                    origin.start + 1);
                proposed.duration = end - origin.start;
                break;
            }
            case ClipDrag::FadeIn:
                proposed.fade_in = std::clamp(origin.fade_in + delta,
                                              core::i64{0}, origin.duration);
                break;
            case ClipDrag::FadeOut:
                proposed.fade_out = std::clamp(origin.fade_out - delta,
                                               core::i64{0}, origin.duration);
                break;
            case ClipDrag::None:
                break;
            }
            proposed.fade_in =
                std::clamp(proposed.fade_in, core::i64{0}, proposed.duration);
            proposed.fade_out =
                std::clamp(proposed.fade_out, core::i64{0}, proposed.duration);
            clip_drag_proposed_ = proposed;
            if (proposed != origin) {
                frame.clip_edits.push_back(
                    TimelineClipEdit{clip_drag_node_, proposed});
            }
        }
    }

    struct VisibleKeyPoint final {
        SelectedKey key;
        float x{};
        float y{};
    };
    std::vector<VisibleKeyPoint> visible_keys;

    ctx.push_clip(rows_bounds);
    for (std::size_t index = 0U; index < rows.size(); ++index) {
        const Row& row = rows[index];
        const float y = rows_bounds.y + static_cast<float>(index) * row_height -
                        row_scroll_;
        if (y + row_height <= rows_bounds.y ||
            y >= rows_bounds.y + rows_bounds.height) {
            continue;
        }
        const float centre_y = y + row_height * 0.5F;
        const bool is_selected = row.surface
            ? row.node == selected_surface
            : selected(row.node, selection);
        const gui::UiColor background =
            is_selected
                ? ctx.theme().selection
                : ((index % 2U) == 0U ? ctx.theme().row : ctx.theme().row_alt);
        ctx.panel(gui::Rect{rows_bounds.x, y, rows_bounds.width, row_height},
                  background);
        ctx.panel(gui::Rect{graph.x, y + row_height - 1.0F, graph.width, 1.0F},
                  ctx.theme().divider);

        const float text_x = rows_bounds.x + 8.0F +
                             static_cast<float>(row.indent) * indent_width;
        const core::u64 row_group = group_id(row.node, row.group);
        const bool open = row.kind == Row::Kind::Layer
                              ? expanded_layers_.count(row.node.value()) != 0U
                              : (row.kind == Row::Kind::Group
                                     ? expanded_groups_.count(row_group) != 0U
                                     : false);
        if (row.expandable) {
            draw_twirl(ctx, text_x + 4.0F, centre_y, open,
                       ctx.theme().text_muted);
        }
        const float name_x = text_x + 14.0F;
        if (row.kind == Row::Kind::Layer) {
            ctx.panel(gui::Rect{name_x, centre_y - 5.0F, 4.0F, 10.0F},
                      row.color);
            ctx.label(gui::Rect{name_x + 10.0F, y,
                                std::max(graph.x - name_x - 58.0F, 0.0F),
                                row_height},
                      row.label,
                      row.locked ? ctx.theme().text_muted : ctx.theme().text);
            ctx.label(gui::Rect{graph.x - 54.0F, y, 52.0F, row_height},
                      row.locked ? "LOCKED"
                                 : (row.surface ? "SURFACE" : "LAYER"),
                      row.locked ? ctx.theme().warning
                                 : ctx.theme().text_muted);
        } else {
            ctx.label(gui::Rect{name_x, y,
                                std::max(graph.x - name_x - 24.0F, 0.0F),
                                row_height},
                      row.label, row.color);
        }

        const float interaction_y = std::max(y, rows_bounds.y);
        const float interaction_bottom =
            std::min(y + row_height, rows_bounds.y + rows_bounds.height);
        const float interaction_height =
            std::max(interaction_bottom - interaction_y, 0.0F);

        // Property-local key controls: the group diamond keys all three axes;
        // a component diamond keys only that channel. A filled diamond means
        // there is already a key at the playhead, while a hollow bright one
        // means the channel is animated elsewhere.
        if (!row.locked && row.kind != Row::Kind::Layer) {
            const ElementInfo* const row_element =
                find_element(elements, row.node);
            const output::ProjectionSurface* const row_surface =
                row.surface ? find_surface(surfaces, row.node) : nullptr;
            if ((row_surface != nullptr) ||
                (row_element != nullptr && !is_light(row_element->kind))) {
                bool animated = false;
                bool keyed_here = true;
                const core::i64 show_frame = signed_frame(timeline.show_frame);
                const core::u8 first_component = row.kind == Row::Kind::Property
                    ? static_cast<core::u8>(row.property)
                    : static_cast<core::u8>(
                          row.group * 3U +
                          (row.kind == Row::Kind::Component ? row.component
                                                            : 0U));
                const core::u8 component_count = row.kind == Row::Kind::Group
                    ? row.component_count
                    : 1U;
                const doc::NodeId key_target =
                    row.kind == Row::Kind::Property ? row.property_target
                                                    : row.node;
                for (core::u8 offset = 0U; offset < component_count; ++offset) {
                    const auto component = static_cast<scene::TransformChannel>(
                        first_component + offset);
                    const scene::ChannelKeyframes* const authored =
                        channel_for(keyframes, key_target, component);
                    animated = animated ||
                               (authored != nullptr && !authored->frames.empty());
                    keyed_here = keyed_here && authored != nullptr &&
                                 std::find(authored->frames.begin(),
                                           authored->frames.end(), show_frame) !=
                                     authored->frames.end();
                }

                const gui::Rect key_bounds{
                    graph.x - 22.0F, interaction_y +
                                           std::max((interaction_height -
                                                     18.0F) *
                                                        0.5F,
                                                    0.0F),
                    18.0F, std::min(interaction_height, 18.0F)};
                if (key_button(ctx,
                               mix(id_, "row key",
                                   static_cast<core::u64>(index)),
                               key_bounds, animated, keyed_here, background)) {
                    for (core::u8 offset = 0U; offset < component_count;
                         ++offset) {
                        const auto component =
                            static_cast<scene::TransformChannel>(
                                first_component + offset);
                        frame.keys.push_back(TimelineKeyEdit{
                            key_target, component, timeline.show_frame,
                            row.kind == Row::Kind::Property
                                ? row.property_value
                                : (row_surface != nullptr
                                       ? surface_component_value(*row_surface,
                                                                 component)
                                       : component_value(*row_element,
                                                         component)),
                            false,
                            keyed_here});
                    }
                }
            }
        }

        const gui::Interaction label_it =
            ctx.grab(mix(id_, "row", static_cast<core::u64>(index)),
                     gui::Rect{rows_bounds.x, interaction_y, list_width,
                               interaction_height});
        if (label_it.clicked) {
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
                frame.selection = TimelineSelectionRequest{
                    row.node, ctx.shift_held() || ctx.ctrl_held(), row.surface};
            }
        }

        // The time lane: a clip bar for a placed media layer, a translucent
        // authored-span bar for every other layer, aggregate diamonds for
        // groups, and draggable diamonds for component channels.
        ctx.push_clip(gui::Rect{graph.x, y, graph.width, row_height});
        // Filled in for a placed clip row, and consulted by the lane
        // interaction below so that a press on the bar becomes a clip gesture
        // rather than a scrub.
        const ElementInfo* clip_element = nullptr;
        scene::ClipTiming clip_timing{};
        gui::Rect clip_bar{};
        float clip_fade_in_x{};
        float clip_fade_out_x{};
        if (row.kind == Row::Kind::Layer && !row.surface) {
            const ElementInfo* const layer_element =
                find_element(elements, row.node);
            if (layer_element != nullptr && editable_clip(*layer_element)) {
                clip_element = layer_element;
                clip_timing = clip_drag_ != ClipDrag::None &&
                        clip_drag_node_ == row.node
                    ? clip_drag_proposed_
                    : layer_element->timing;
            }
        }
        if (clip_element != nullptr) {
            const float x0 =
                frame_to_x(static_cast<double>(clip_timing.start));
            const float x1 = frame_to_x(
                static_cast<double>(clip_timing.start + clip_timing.duration));
            clip_bar = gui::Rect{x0, y + 3.0F, std::max(x1 - x0, 2.0F),
                                 row_height - 6.0F};
            const bool live = clip_drag_ != ClipDrag::None &&
                clip_drag_node_ == row.node;
            ctx.panel(clip_bar, alpha(row.color, live ? 0.42F : 0.30F));
            ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, clip_bar.width, 1.0F},
                      alpha(row.color, 0.85F));
            ctx.panel(gui::Rect{clip_bar.x, clip_bar.y + clip_bar.height - 1.0F,
                                clip_bar.width, 1.0F},
                      alpha(row.color, 0.85F));
            const float grab = edge_grab_width(clip_bar.width);
            ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                      alpha(row.color, 0.68F));
            ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab, clip_bar.y,
                                grab, clip_bar.height},
                      alpha(row.color, 0.68F));

            // The ramps, drawn the way an editor draws them: a diagonal from
            // the bar's foot at the cut to its head where the clip is fully in.
            clip_fade_in_x = frame_to_x(
                static_cast<double>(clip_timing.start + clip_timing.fade_in));
            clip_fade_out_x = frame_to_x(static_cast<double>(
                clip_timing.start + clip_timing.duration -
                clip_timing.fade_out));
            const gui::UiColor ramp = alpha(ctx.theme().keyframe, 0.9F);
            if (clip_timing.fade_in > 0) {
                ctx.draw_list().add_line(
                    clip_bar.x, clip_bar.y + clip_bar.height, clip_fade_in_x,
                    clip_bar.y, 1.0F, ramp);
            }
            if (clip_timing.fade_out > 0) {
                ctx.draw_list().add_line(
                    clip_fade_out_x, clip_bar.y, clip_bar.x + clip_bar.width,
                    clip_bar.y + clip_bar.height, 1.0F, ramp);
            }
            ctx.panel(gui::Rect{clip_fade_in_x - 3.0F, clip_bar.y, 6.0F, 5.0F},
                      ramp);
            ctx.panel(gui::Rect{clip_fade_out_x - 3.0F, clip_bar.y, 6.0F, 5.0F},
                      ramp);

            // Content sampled inside the bar and keyed to the source frame
            // under each pixel, so a trimmed or moved placement visibly covers
            // different source material rather than redrawing one pattern.
            const float begin = std::max(clip_bar.x, graph.x) + 2.0F;
            const float end = std::min(clip_bar.x + clip_bar.width,
                                       graph.x + graph.width) - 1.0F;
            for (float x = begin; x < end; x += 5.0F) {
                const auto local = static_cast<core::i64>(
                    std::llround(x_to_frame(x))) - clip_timing.start;
                const core::u64 pattern = row.node.value() * 17U +
                    static_cast<core::u64>(
                        std::max(local + clip_timing.source_in,
                                 core::i64{0})) * 29U;
                const float amplitude =
                    2.0F + static_cast<float>(pattern % 7U);
                ctx.panel(
                    gui::Rect{x, centre_y - amplitude * 0.5F, 1.0F, amplitude},
                    alpha(row.color, 0.62F));
            }
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
                const float end =
                    std::min(clip.x + clip.width, graph.x + graph.width);
                std::size_t sample = 0U;
                for (float x = begin + 2.0F; x < end - 1.0F; x += 5.0F) {
                    const core::u64 pattern =
                        row.node.value() * 17U + sample * 29U + sample * sample;
                    const float amplitude =
                        2.0F + static_cast<float>(pattern % 7U);
                    ctx.panel(gui::Rect{x, centre_y - amplitude * 0.5F, 1.0F,
                                        amplitude},
                              alpha(row.color, 0.78F));
                    ++sample;
                }
            }
        }

        const auto draw_frames = [&](const std::vector<core::i64>* frames,
                                     gui::UiColor color) {
            if (frames == nullptr) {
                return;
            }
            for (const core::i64 key : *frames) {
                draw_diamond(ctx, frame_to_x(static_cast<double>(key)),
                             centre_y, key_radius, color);
            }
        };

        const scene::ChannelKeyframes* component_channel = nullptr;
        const std::vector<core::i64>* component_frames = nullptr;
        std::vector<float> component_x;
        std::vector<float> component_y;
        scene::TransformChannel channel{};
        if (row.kind == Row::Kind::Component ||
            row.kind == Row::Kind::Property) {
            channel = row.kind == Row::Kind::Property
                ? row.property
                : static_cast<scene::TransformChannel>(row.group * 3U +
                                                       row.component);
            const doc::NodeId channel_target =
                row.kind == Row::Kind::Property ? row.property_target
                                                : row.node;
            component_channel =
                channel_for(keyframes, channel_target, channel);
            component_frames = component_channel != nullptr
                ? &component_channel->frames
                : nullptr;
            if (component_frames != nullptr) {
                component_x.reserve(component_frames->size());
                component_y.reserve(component_frames->size());

                double minimum_value =
                    std::numeric_limits<double>::infinity();
                double maximum_value =
                    -std::numeric_limits<double>::infinity();
                const bool has_values =
                    component_channel->values.size() ==
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
                    if (!graph_mode_ || !has_values ||
                        !std::isfinite(value_span) || value_span <= 1.0e-12) {
                        return centre_y;
                    }
                    const double value =
                        component_channel->values[key_index];
                    if (!std::isfinite(value)) {
                        return centre_y;
                    }
                    const double normal =
                        (maximum_value - value) / value_span;
                    return y + 4.0F +
                           static_cast<float>(normal) * (row_height - 8.0F);
                };

                float previous_x = 0.0F;
                float previous_y = centre_y;
                bool have_previous = false;
                for (std::size_t key_index = 0U;
                     key_index < component_frames->size(); ++key_index) {
                    const core::i64 key_frame = (*component_frames)[key_index];
                    const SelectedKey identity{
                        channel_target, channel, key_frame};
                    const auto moving =
                        std::find(drag_keys_.begin(), drag_keys_.end(), identity);
                    const core::i64 shown_frame =
                        dragging_key_ && moving != drag_keys_.end()
                            ? key_frame + drag_delta_
                            : key_frame;
                    const float key_x =
                        frame_to_x(static_cast<double>(shown_frame));
                    const float key_y = value_y(key_index);
                    if (graph_mode_ && have_previous) {
                        const auto interpolation =
                            component_channel->interpolations.size() ==
                                    component_channel->frames.size()
                                ? component_channel
                                      ->interpolations[key_index - 1U]
                                : scene::KeyInterpolation::Linear;
                        if (interpolation == scene::KeyInterpolation::Hold) {
                            ctx.draw_list().add_line(
                                previous_x, previous_y, key_x, previous_y,
                                1.0F, alpha(row.color, 0.72F));
                            ctx.draw_list().add_line(
                                key_x, previous_y, key_x, key_y, 1.0F,
                                alpha(row.color, 0.72F));
                        } else if (interpolation ==
                                   scene::KeyInterpolation::Cubic) {
                            float curve_x = previous_x;
                            float curve_y = previous_y;
                            constexpr std::size_t samples = 8U;
                            for (std::size_t sample = 1U; sample <= samples;
                                 ++sample) {
                                const float t = static_cast<float>(sample) /
                                                static_cast<float>(samples);
                                const float eased =
                                    t * t * (3.0F - 2.0F * t);
                                const float next_x =
                                    previous_x + (key_x - previous_x) * t;
                                const float next_y =
                                    previous_y + (key_y - previous_y) * eased;
                                ctx.draw_list().add_line(
                                    curve_x, curve_y, next_x, next_y, 1.0F,
                                    alpha(row.color, 0.72F));
                                curve_x = next_x;
                                curve_y = next_y;
                            }
                        } else {
                            ctx.draw_list().add_line(
                                previous_x, previous_y, key_x, key_y, 1.0F,
                                alpha(row.color, 0.72F));
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
                    draw_diamond(
                        ctx, key_x, key_y,
                        is_key_selected ? key_radius + 0.5F : key_radius,
                        is_key_selected ? ctx.theme().playhead
                                        : ctx.theme().keyframe);
                    if (!row.locked) {
                        visible_keys.push_back(
                            VisibleKeyPoint{identity, key_x, key_y});
                    }
                }
            }
        } else if (row.kind == Row::Kind::Group) {
            for (core::u8 component = 0U; component < row.component_count;
                 ++component) {
                const auto aggregate = static_cast<scene::TransformChannel>(
                    row.group * 3U + component);
                draw_frames(frames_for(keyframes, row.node, aggregate),
                            alpha(ctx.theme().keyframe, 0.58F));
            }
        }

        const gui::Interaction lane_it = ctx.grab(
            mix(id_, "lane", static_cast<core::u64>(index)),
            gui::Rect{graph.x, interaction_y, graph.width, interaction_height});
        if (!row.locked && lane_it.pressed && clip_element != nullptr &&
            clip_bar.contains(ctx.pointer_x(), ctx.pointer_y())) {
            // Fade handles first: they sit on the bar's top edge, and at a
            // zero-length ramp they share that corner with the trim strip.
            const float grab = edge_grab_width(clip_bar.width);
            const float px = ctx.pointer_x();
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
            clip_drag_node_ = row.node;
            clip_drag_origin_ = clip_timing;
            clip_drag_proposed_ = clip_timing;
            clip_drag_anchor_ =
                static_cast<core::i64>(std::llround(x_to_frame(px)));
            clip_snap_guide_.reset();
            marquee_tracking_ = false;
            marquee_active_ = false;
            frame.selection = TimelineSelectionRequest{
                row.node, ctx.shift_held() || ctx.ctrl_held(), false};
        } else if (lane_it.pressed) {
            bool hit_key = false;
            if ((row.kind == Row::Kind::Component ||
                 row.kind == Row::Kind::Property) &&
                component_frames != nullptr) {
                for (std::size_t key_index = 0U;
                     key_index < component_frames->size(); ++key_index) {
                    const core::i64 key_frame = (*component_frames)[key_index];
                    const float dx = component_x[key_index] - ctx.pointer_x();
                    const float dy = component_y[key_index] - ctx.pointer_y();
                    if (dx * dx + dy * dy <=
                        key_hit_radius * key_hit_radius) {
                        const SelectedKey hit{
                            row.kind == Row::Kind::Property
                                ? row.property_target
                                : row.node,
                            channel, key_frame};
                        bool begin_drag = true;
                        const auto selected_it = std::find(
                            selected_keys_.begin(), selected_keys_.end(), hit);
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

                        if (begin_drag) {
                            drag_keys_.clear();
                            for (const SelectedKey& selected_key :
                                 selected_keys_) {
                                const auto* const authored = frames_for(
                                    keyframes, selected_key.target,
                                    selected_key.component);
                                if (authored != nullptr &&
                                    std::find(authored->begin(), authored->end(),
                                              selected_key.frame) !=
                                        authored->end()) {
                                    drag_keys_.push_back(selected_key);
                                }
                            }
                            if (drag_keys_.empty()) {
                                drag_keys_.push_back(hit);
                            }
                            dragging_key_ = true;
                            drag_anchor_frame_ = key_frame;
                            drag_delta_ = 0;
                            marquee_tracking_ = false;
                            marquee_active_ = false;
                        }
                        hit_key = true;
                        break;
                    }
                }
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
                frame.seek_to_frame =
                    rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
            } else if (row.locked) {
                frame.seek_to_frame =
                    rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
            }
        }
        if (lane_it.held && !dragging_key_ && !marquee_active_ &&
            clip_drag_ == ClipDrag::None) {
            frame.seek_to_frame =
                rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
        }
        ctx.pop_clip();
    }

    ctx.pop_clip();

    if (marquee_tracking_ && marquee_active_ && ctx.pointer_down()) {
        const float left = std::max(
            std::min(marquee_start_x_, ctx.pointer_x()), graph.x);
        const float right = std::min(
            std::max(marquee_start_x_, ctx.pointer_x()),
            graph.x + graph.width);
        const float top = std::max(
            std::min(marquee_start_y_, ctx.pointer_y()), rows_bounds.y);
        const float bottom = std::min(
            std::max(marquee_start_y_, ctx.pointer_y()),
            rows_bounds.y + rows_bounds.height);
        const gui::Rect marquee{left, top, std::max(right - left, 0.0F),
                                std::max(bottom - top, 0.0F)};
        ctx.push_clip(rows_bounds);
        ctx.panel(marquee, alpha(ctx.theme().accent, 0.15F));
        ctx.draw_list().add_line(marquee.x, marquee.y,
                                 marquee.x + marquee.width, marquee.y, 1.0F,
                                 ctx.theme().accent);
        ctx.draw_list().add_line(
            marquee.x, marquee.y + marquee.height,
            marquee.x + marquee.width, marquee.y + marquee.height, 1.0F,
            ctx.theme().accent);
        ctx.draw_list().add_line(marquee.x, marquee.y, marquee.x,
                                 marquee.y + marquee.height, 1.0F,
                                 ctx.theme().accent);
        ctx.draw_list().add_line(
            marquee.x + marquee.width, marquee.y,
            marquee.x + marquee.width, marquee.y + marquee.height, 1.0F,
            ctx.theme().accent);
        ctx.pop_clip();
    }

    if (marquee_tracking_ && !ctx.pointer_down()) {
        if (marquee_active_) {
            const float left =
                std::min(marquee_start_x_, ctx.pointer_x());
            const float right =
                std::max(marquee_start_x_, ctx.pointer_x());
            const float top =
                std::min(marquee_start_y_, ctx.pointer_y());
            const float bottom =
                std::max(marquee_start_y_, ctx.pointer_y());
            selected_keys_ = marquee_base_;
            if (!marquee_shift_ && !marquee_ctrl_) {
                selected_keys_.clear();
            }
            for (const VisibleKeyPoint& point : visible_keys) {
                if (point.x < left || point.x > right || point.y < top ||
                    point.y > bottom) {
                    continue;
                }
                const auto found = std::find(selected_keys_.begin(),
                                             selected_keys_.end(), point.key);
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
        }
        marquee_tracking_ = false;
        marquee_active_ = false;
        marquee_base_.clear();
    }

    // Static playhead over every lane and the ruler. It is drawn after the rows
    // so it stays findable over a saturated clip colour.
    ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width,
                            ruler.height + rows_bounds.height});
    const float playhead_x =
        frame_to_x(static_cast<double>(timeline.show_frame));
    const std::optional<core::i64> guide = dragging_key_
        ? snap_guide_
        : (clip_drag_ != ClipDrag::None ? clip_snap_guide_
                                        : std::optional<core::i64>{});
    if (guide.has_value()) {
        const float guide_x = frame_to_x(static_cast<double>(*guide));
        if (guide_x >= graph.x && guide_x <= graph.x + graph.width) {
            ctx.draw_list().add_line(
                guide_x, ruler.y, guide_x,
                rows_bounds.y + rows_bounds.height, 1.0F,
                alpha(ctx.theme().accent, 0.72F));
        }
    }
    if (playhead_x >= graph.x && playhead_x <= graph.x + graph.width) {
        ctx.draw_list().add_line(playhead_x, ruler.y, playhead_x,
                                 rows_bounds.y + rows_bounds.height, 1.5F,
                                 ctx.theme().playhead);
        ctx.draw_list().add_triangle(playhead_x - 5.0F, ruler.y,
                                     playhead_x + 5.0F, ruler.y, playhead_x,
                                     ruler.y + 7.0F, ctx.theme().playhead);
    }
    ctx.pop_clip();

    // A slim non-interactive scroll thumb is enough here: wheel owns row
    // navigation, and the indicator makes a clipped layer stack obvious.
    if (maximum_row_scroll > 0.0F && rows_bounds.height > 0.0F) {
        const float fraction = rows_bounds.height / content_height;
        const float thumb_height =
            std::max(rows_bounds.height * fraction, 20.0F);
        const float travel = std::max(rows_bounds.height - thumb_height, 0.0F);
        const float position =
            maximum_row_scroll > 0.0F ? row_scroll_ / maximum_row_scroll : 0.0F;
        ctx.push_clip(rows_bounds);
        ctx.panel(gui::Rect{bounds.x + bounds.width - 4.0F,
                            rows_bounds.y + travel * position, 3.0F,
                            thumb_height},
                  ctx.theme().widget_hover);
        ctx.pop_clip();
    }

    // Commit the complete selection once, on release. Moving positive time is
    // authored from the last key backwards (and negative time first-key
    // forwards), preventing one destination from hiding another source key in
    // the same channel while the batch is applied.
    if (dragging_key_ && !ctx.pointer_down()) {
        if (drag_delta_ != 0) {
            std::vector<SelectedKey> authored = drag_keys_;
            std::sort(
                authored.begin(), authored.end(),
                [this](const SelectedKey& left,
                       const SelectedKey& right) noexcept {
                    if (left.target.value() != right.target.value()) {
                        return left.target.value() < right.target.value();
                    }
                    if (left.component != right.component) {
                        return static_cast<core::u8>(left.component) <
                               static_cast<core::u8>(right.component);
                    }
                    return drag_delta_ > 0 ? left.frame > right.frame
                                           : left.frame < right.frame;
                });
            for (const SelectedKey& key : authored) {
                frame.key_moves.push_back(TimelineKeyMoveEdit{
                    key.target, key.component, key.frame,
                    key.frame + drag_delta_});
            }
            for (SelectedKey& selected_key : selected_keys_) {
                if (std::find(drag_keys_.begin(), drag_keys_.end(),
                              selected_key) != drag_keys_.end()) {
                    selected_key.frame += drag_delta_;
                }
            }
        }
        dragging_key_ = false;
        drag_keys_.clear();
        drag_delta_ = 0;
        snap_guide_.reset();
    }

    if (popup_pointer_blocked) {
        ctx.pop_pointer_block();
    }
    if (rate_menu_open_) {
        ctx.panel(rate_popup, ctx.theme().background);
        ctx.panel(gui::Rect{rate_popup.x, rate_popup.y, rate_popup.width, 1.0F},
                  ctx.theme().divider);
        ctx.panel(gui::Rect{rate_popup.x, rate_popup.y, 1.0F,
                            rate_popup.height},
                  ctx.theme().divider);
        ctx.panel(gui::Rect{rate_popup.x + rate_popup.width - 1.0F,
                            rate_popup.y, 1.0F, rate_popup.height},
                  ctx.theme().divider);
        float choice_y = rate_popup.y + 4.0F;
        for (std::size_t index = 0U; index < common_rates.size(); ++index) {
            const RateChoice& choice = common_rates[index];
            const gui::Rect row{rate_popup.x + 4.0F, choice_y,
                                rate_popup.width - 8.0F, 21.0F};
            const gui::Interaction interaction =
                ctx.grab(mix(id_, "show rate choice", index), row);
            ctx.panel(row, same_rate(timeline, choice.rate)
                               ? ctx.theme().selection
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().row));
            ctx.label(gui::Rect{row.x + 7.0F, row.y,
                                std::max(row.width - 7.0F, 0.0F), row.height},
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

    frame.editing = range_fields_.live() || ctx.focus() == search_id ||
        rate_menu_open_;
    if (timeline_focused_ && !frame.editing && !selected_keys_.empty() &&
        ctx.key_pressed(gui::Key::Delete)) {
        for (const SelectedKey& key : selected_keys_) {
            if (key.frame >= 0) {
                frame.keys.push_back(TimelineKeyEdit{
                    key.target, key.component,
                    static_cast<core::u64>(key.frame), 0.0, false, true});
            }
        }
        selected_keys_.clear();
        drag_keys_.clear();
        dragging_key_ = false;
        frame.consume_delete = true;
    }

    ctx.pop_clip();
    return frame;
}

} // namespace mediaserver::editor
