#include <mediaserver/show/transport_state.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace mediaserver::show {
namespace {

inline constexpr core::u64 max_document_integer =
    static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] const std::int64_t* find_integer(
    const doc::PropertyMap& properties,
    std::string_view key) noexcept {
    const auto entry = properties.find(key);
    if (entry == properties.end()) {
        return nullptr;
    }
    return std::get_if<std::int64_t>(&entry->second);
}

[[nodiscard]] bool read_u64(
    const doc::PropertyMap& properties,
    std::string_view key,
    core::u64& parsed) noexcept {
    const auto* const value = find_integer(properties, key);
    if (value == nullptr || *value < 0) {
        return false;
    }
    parsed = static_cast<core::u64>(*value);
    return true;
}

[[nodiscard]] bool read_u32(
    const doc::PropertyMap& properties,
    std::string_view key,
    core::u32& parsed) noexcept {
    core::u64 wide{};
    if (!read_u64(properties, key, wide)
        || wide > std::numeric_limits<core::u32>::max()) {
        return false;
    }
    parsed = static_cast<core::u32>(wide);
    return true;
}

[[nodiscard]] bool read_bool(
    const doc::PropertyMap& properties,
    std::string_view key,
    bool& parsed) noexcept {
    const auto entry = properties.find(key);
    if (entry == properties.end()) {
        return false;
    }
    const auto* const value = std::get_if<bool>(&entry->second);
    if (value == nullptr) {
        return false;
    }
    parsed = *value;
    return true;
}

[[nodiscard]] bool read_transport_state(
    const doc::PropertyMap& properties,
    TransportState& parsed) noexcept {
    core::u64 wide{};
    if (!read_u64(properties, transport_property::transport_state, wide)) {
        return false;
    }
    switch (wide) {
    case 0U:
        parsed = TransportState::Stopped;
        return true;
    case 1U:
        parsed = TransportState::Paused;
        return true;
    case 2U:
        parsed = TransportState::Playing;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool read_direction(
    const doc::PropertyMap& properties,
    TransportDirection& parsed) noexcept {
    core::u64 wide{};
    if (!read_u64(properties, transport_property::direction, wide)) {
        return false;
    }
    switch (wide) {
    case 0U:
        parsed = TransportDirection::Forward;
        return true;
    case 1U:
        parsed = TransportDirection::Reverse;
        return true;
    default:
        return false;
    }
}

// Inclusive frame count of a validated range. The validity rule forbids a
// range spanning the whole unsigned domain, so this cannot wrap to zero.
[[nodiscard]] core::u64 inclusive_span(const TransportAnchor& anchor) noexcept {
    return anchor.last_frame.value() - anchor.first_frame.value() + 1U;
}

// (offset + step) modulo span without ever forming the intermediate sum, which
// a span above half the unsigned domain would otherwise overflow.
[[nodiscard]] core::u64 wrap_forward(
    core::u64 offset,
    core::u64 step,
    core::u64 span) noexcept {
    const auto headroom = span - offset;
    return step >= headroom ? step - headroom : offset + step;
}

[[nodiscard]] core::u64 wrap_reverse(
    core::u64 offset,
    core::u64 step,
    core::u64 span) noexcept {
    return step <= offset ? offset - step : span - (step - offset);
}

} // namespace

bool valid_transport_anchor(const TransportAnchor& anchor) noexcept {
    if (anchor.rate.numerator == 0U || anchor.rate.denominator == 0U
        || anchor.rate.numerator > max_rate_numerator
        || anchor.rate.denominator > max_rate_denominator) {
        return false;
    }
    if (anchor.last_frame < anchor.first_frame
        || anchor.last_frame.value() - anchor.first_frame.value()
            == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    if (anchor.anchor_show_frame < anchor.first_frame
        || anchor.last_frame < anchor.anchor_show_frame) {
        return false;
    }
    // The frame a crossfade leaves has to exist for as long as it is shown.
    if (anchor.blend_frames != 0U
        && (anchor.blend_from < anchor.first_frame
            || anchor.last_frame < anchor.blend_from
            || anchor.blend_frames > max_document_integer
            || (anchor.blend_moves
                && anchor.blend_frames - 1U
                    > anchor.last_frame.value() - anchor.blend_from.value()))) {
        return false;
    }
    // Document integers are signed, so a value that could not survive the
    // round trip is refused at the authoring boundary rather than read back
    // as a different position.
    return anchor.anchor_control_frame.value() <= max_document_integer
        && anchor.last_frame.value() <= max_document_integer;
}

ShowFrame derive_show_frame(
    const TransportAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept {
    if (anchor.state != TransportState::Playing
        || !valid_transport_anchor(anchor)
        || control_frame <= anchor.anchor_control_frame) {
        return anchor.anchor_show_frame;
    }

    const auto delta =
        control_frame.value() - anchor.anchor_control_frame.value();
    const auto span = inclusive_span(anchor);
    const auto offset =
        anchor.anchor_show_frame.value() - anchor.first_frame.value();

    if (!anchor.loop) {
        // A finished show holds its terminal frame rather than running off the
        // end of the timeline, which is what the local clock does too.
        if (anchor.direction == TransportDirection::Forward) {
            const auto remaining = span - 1U - offset;
            return delta >= remaining
                ? anchor.last_frame
                : ShowFrame{anchor.anchor_show_frame.value() + delta};
        }
        return delta >= offset
            ? anchor.first_frame
            : ShowFrame{anchor.anchor_show_frame.value() - delta};
    }

    const auto step = delta % span;
    const auto wrapped = anchor.direction == TransportDirection::Forward
        ? wrap_forward(offset, step, span)
        : wrap_reverse(offset, step, span);
    return ShowFrame{anchor.first_frame.value() + wrapped};
}

std::optional<TransportBlend> derive_transport_blend(
    const TransportAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept {
    if (anchor.blend_frames == 0U || anchor.state != TransportState::Playing
        || control_frame < anchor.anchor_control_frame
        || !valid_transport_anchor(anchor)) {
        return std::nullopt;
    }
    const auto elapsed =
        control_frame.value() - anchor.anchor_control_frame.value();
    if (elapsed >= anchor.blend_frames) {
        return std::nullopt;
    }
    return TransportBlend{
        ShowFrame{anchor.blend_from.value() + (anchor.blend_moves ? elapsed : 0U)},
        elapsed,
        anchor.blend_frames};
}

bool reached_end(
    const TransportAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept {
    if (anchor.state != TransportState::Playing || anchor.loop
        || !valid_transport_anchor(anchor)) {
        return false;
    }
    const auto terminal = anchor.direction == TransportDirection::Forward
        ? anchor.last_frame
        : anchor.first_frame;
    return derive_show_frame(anchor, control_frame) == terminal;
}

doc::Node make_transport_node(const TransportAnchor& anchor) {
    const auto integer = [](core::u64 value) noexcept {
        return static_cast<std::int64_t>(
            value > max_document_integer ? max_document_integer : value);
    };

    doc::Node node{};
    node.id = transport_node_id;
    node.type = std::string{transport_node_type};
    node.properties.emplace(
        std::string{transport_property::anchor_control_frame},
        integer(anchor.anchor_control_frame.value()));
    node.properties.emplace(
        std::string{transport_property::anchor_show_frame},
        integer(anchor.anchor_show_frame.value()));
    node.properties.emplace(
        std::string{transport_property::transport_state},
        static_cast<std::int64_t>(anchor.state));
    node.properties.emplace(
        std::string{transport_property::direction},
        static_cast<std::int64_t>(anchor.direction));
    node.properties.emplace(
        std::string{transport_property::rate_numerator},
        static_cast<std::int64_t>(anchor.rate.numerator));
    node.properties.emplace(
        std::string{transport_property::rate_denominator},
        static_cast<std::int64_t>(anchor.rate.denominator));
    node.properties.emplace(
        std::string{transport_property::first_frame},
        integer(anchor.first_frame.value()));
    node.properties.emplace(
        std::string{transport_property::last_frame},
        integer(anchor.last_frame.value()));
    node.properties.emplace(std::string{transport_property::loop}, anchor.loop);
    node.properties.emplace(
        std::string{transport_property::cues_armed}, anchor.cues_armed);
    node.properties.emplace(
        std::string{transport_property::scheduled}, anchor.scheduled);
    if (anchor.blend_frames != 0U) {
        node.properties.emplace(
            std::string{transport_property::blend_frames},
            integer(anchor.blend_frames));
        node.properties.emplace(
            std::string{transport_property::blend_from},
            integer(anchor.blend_from.value()));
        node.properties.emplace(
            std::string{transport_property::blend_moves}, anchor.blend_moves);
    }
    return node;
}

std::optional<TransportAnchor> read_transport(const doc::Root& root) noexcept {
    const auto* const node = root.get(transport_node_id);
    if (node == nullptr
        || std::string_view{node->type} != transport_node_type) {
        return std::nullopt;
    }

    TransportAnchor anchor{};
    core::u64 control_frame{};
    core::u64 show_frame{};
    core::u64 first{};
    core::u64 last{};
    if (!read_u64(
            node->properties,
            transport_property::anchor_control_frame,
            control_frame)
        || !read_u64(
            node->properties,
            transport_property::anchor_show_frame,
            show_frame)
        || !read_transport_state(node->properties, anchor.state)
        || !read_direction(node->properties, anchor.direction)
        || !read_u32(
            node->properties,
            transport_property::rate_numerator,
            anchor.rate.numerator)
        || !read_u32(
            node->properties,
            transport_property::rate_denominator,
            anchor.rate.denominator)
        || !read_u64(node->properties, transport_property::first_frame, first)
        || !read_u64(node->properties, transport_property::last_frame, last)
        || !read_bool(node->properties, transport_property::loop, anchor.loop)) {
        return std::nullopt;
    }
    // Optional, but a value that is present has to be the right type: a
    // mistyped flag is a damaged node, not an armed one.
    if (node->properties.find(transport_property::cues_armed)
            != node->properties.end()
        && !read_bool(
            node->properties,
            transport_property::cues_armed,
            anchor.cues_armed)) {
        return std::nullopt;
    }
    if (node->properties.find(transport_property::scheduled)
            != node->properties.end()
        && !read_bool(
            node->properties,
            transport_property::scheduled,
            anchor.scheduled)) {
        return std::nullopt;
    }

    if (node->properties.find(transport_property::blend_frames)
        != node->properties.end()) {
        core::u64 blend_from{};
        if (!read_u64(
                node->properties,
                transport_property::blend_frames,
                anchor.blend_frames)
            || !read_u64(
                node->properties, transport_property::blend_from, blend_from)
            || !read_bool(
                node->properties,
                transport_property::blend_moves,
                anchor.blend_moves)) {
            return std::nullopt;
        }
        anchor.blend_from = ShowFrame{blend_from};
    }

    anchor.anchor_control_frame = cmd::FrameNumber{control_frame};
    anchor.anchor_show_frame = ShowFrame{show_frame};
    anchor.first_frame = ShowFrame{first};
    anchor.last_frame = ShowFrame{last};
    if (!valid_transport_anchor(anchor)) {
        return std::nullopt;
    }
    return anchor;
}

} // namespace mediaserver::show
