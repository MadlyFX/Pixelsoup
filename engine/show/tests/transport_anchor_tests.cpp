#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "allocation_guard.hpp"
#include "show_test_support.hpp"

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;

using show::ShowFrame;
using show::TransportAnchor;
using show::TransportDirection;
using show::TransportState;

constexpr show::FrameRate rate_50{50U, 1U};
constexpr show::FrameRate rate_5994{60'000U, 1'001U};

[[nodiscard]] cmd::FrameNumber control(core::u64 value) noexcept {
    return cmd::FrameNumber{value};
}

[[nodiscard]] TransportAnchor playing_at(
    core::u64 anchor_control,
    core::u64 anchor_show,
    core::u64 first,
    core::u64 last,
    bool loop) noexcept {
    TransportAnchor anchor{};
    anchor.anchor_control_frame = control(anchor_control);
    anchor.anchor_show_frame = ShowFrame{anchor_show};
    anchor.state = TransportState::Playing;
    anchor.direction = TransportDirection::Forward;
    anchor.rate = rate_50;
    anchor.first_frame = ShowFrame{first};
    anchor.last_frame = ShowFrame{last};
    anchor.loop = loop;
    return anchor;
}

void a_playing_anchor_advances_one_show_frame_per_control_frame() {
    const auto anchor = playing_at(10U, 100U, 0U, 999U, false);

    SHOW_CHECK(show::derive_show_frame(anchor, control(10U)) == ShowFrame{100U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(11U)) == ShowFrame{101U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(60U)) == ShowFrame{150U});

    // A control frame at or before the anchor reads as the anchor position
    // rather than underflowing into the past.
    SHOW_CHECK(show::derive_show_frame(anchor, control(9U)) == ShowFrame{100U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(0U)) == ShowFrame{100U});
}

void a_paused_or_stopped_anchor_suppresses_the_whole_term() {
    auto anchor = playing_at(10U, 100U, 0U, 999U, false);
    anchor.state = TransportState::Paused;
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(10'000U)) == ShowFrame{100U});

    anchor.state = TransportState::Stopped;
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(10'000U)) == ShowFrame{100U});
}

void a_non_looping_show_holds_its_last_frame() {
    const auto anchor = playing_at(4U, 98U, 0U, 100U, false);

    SHOW_CHECK(show::derive_show_frame(anchor, control(6U)) == ShowFrame{100U});
    SHOW_CHECK(!show::reached_end(anchor, control(5U)));
    SHOW_CHECK(show::reached_end(anchor, control(6U)));
    // Holding is stable: every later control frame reports the same frame.
    SHOW_CHECK(show::derive_show_frame(anchor, control(7U)) == ShowFrame{100U});
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(1'000'000U))
        == ShowFrame{100U});
}

void a_looping_show_wraps_within_its_inclusive_range() {
    const auto anchor = playing_at(0U, 10U, 10U, 12U, true);

    SHOW_CHECK(show::derive_show_frame(anchor, control(0U)) == ShowFrame{10U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(1U)) == ShowFrame{11U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(2U)) == ShowFrame{12U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(3U)) == ShowFrame{10U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(4U)) == ShowFrame{11U});
    // A wrapped show never reports the end-of-show hold.
    SHOW_CHECK(!show::reached_end(anchor, control(3U)));

    // Millions of wraps later the phase is still exact, because the
    // derivation is one modulo rather than an accumulation.
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(3'000'002U)) == ShowFrame{12U});
}

void reverse_runs_backwards_and_stops_at_the_first_frame() {
    auto anchor = playing_at(100U, 20U, 5U, 50U, false);
    anchor.direction = TransportDirection::Reverse;

    SHOW_CHECK(show::derive_show_frame(anchor, control(101U)) == ShowFrame{19U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(114U)) == ShowFrame{6U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(115U)) == ShowFrame{5U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(900U)) == ShowFrame{5U});
    SHOW_CHECK(!show::reached_end(anchor, control(114U)));
    SHOW_CHECK(show::reached_end(anchor, control(115U)));

    anchor.loop = true;
    SHOW_CHECK(show::derive_show_frame(anchor, control(115U)) == ShowFrame{5U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(116U)) == ShowFrame{50U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(117U)) == ShowFrame{49U});
}

// The point of anchoring to the ordinal rather than to a clock: a node that
// evaluates a control frame late computes the same answer as one that
// evaluated it on time, and a node that skips ahead lands exactly where the
// frames it skipped would have taken it.
void derivation_is_independent_of_when_a_node_evaluates_it() {
    const auto anchor = playing_at(1U, 0U, 0U, 100'000U, false);

    core::u64 stepped = 0U;
    for (core::u64 frame = 1U; frame <= 500U; ++frame) {
        stepped = show::derive_show_frame(anchor, control(frame)).value();
    }
    SHOW_CHECK(stepped == 499U);
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(500U)) == ShowFrame{499U});
}

void a_huge_looping_span_never_overflows_the_wrap() {
    // The largest range a document integer can carry, which is also the
    // largest span the wrap has to survive.
    constexpr core::u64 last = 0x7FFF'FFFF'FFFF'FFFFULL;
    auto anchor = playing_at(0U, last - 1U, 0U, last, true);

    SHOW_CHECK(show::derive_show_frame(anchor, control(1U)) == ShowFrame{last});
    SHOW_CHECK(show::derive_show_frame(anchor, control(2U)) == ShowFrame{0U});
    SHOW_CHECK(show::derive_show_frame(anchor, control(3U)) == ShowFrame{1U});

    anchor.direction = TransportDirection::Reverse;
    SHOW_CHECK(
        show::derive_show_frame(anchor, control(1U)) == ShowFrame{last - 2U});
}

void an_out_of_domain_anchor_reads_as_absent_rather_than_as_a_position() {
    TransportAnchor anchor{};
    SHOW_CHECK(!show::valid_transport_anchor(anchor)); // zero rate

    anchor = playing_at(0U, 5U, 10U, 20U, false); // position below the range
    SHOW_CHECK(!show::valid_transport_anchor(anchor));

    anchor = playing_at(0U, 25U, 10U, 20U, false); // position above the range
    SHOW_CHECK(!show::valid_transport_anchor(anchor));

    anchor = playing_at(0U, 10U, 20U, 10U, false); // inverted range
    SHOW_CHECK(!show::valid_transport_anchor(anchor));

    anchor = playing_at(0U, 0U, 0U, 100U, false);
    anchor.rate = show::FrameRate{0U, 1U};
    SHOW_CHECK(!show::valid_transport_anchor(anchor));

    // An invalid anchor derives to its own recorded position instead of
    // computing arithmetic nobody can trust.
    SHOW_CHECK(show::derive_show_frame(anchor, control(99U)) == ShowFrame{0U});
}

void the_document_round_trip_preserves_the_anchor_exactly() {
    auto anchor = playing_at(4'321U, 1'234U, 100U, 99'999U, true);
    anchor.rate = rate_5994;
    anchor.direction = TransportDirection::Reverse;

    mediaserver::doc::Root root;
    root = root.set(show::make_transport_node(anchor));

    const auto decoded = show::read_transport(root);
    SHOW_CHECK(decoded.has_value());
    SHOW_CHECK(*decoded == anchor);
}

void a_missing_or_mistyped_transport_node_reads_as_absent() {
    mediaserver::doc::Root empty;
    SHOW_CHECK(!show::read_transport(empty));

    mediaserver::doc::Node foreign{};
    foreign.id = show::transport_node_id;
    foreign.type = "show.something-else";
    SHOW_CHECK(!show::read_transport(empty.set(foreign)));

    // A node of the right type missing a required property is refused rather
    // than defaulted: a partial anchor would make two nodes disagree.
    auto partial = show::make_transport_node(
        playing_at(1U, 2U, 0U, 10U, false));
    partial.properties.erase(
        std::string{show::transport_property::anchor_control_frame});
    SHOW_CHECK(!show::read_transport(empty.set(partial)));

    // So is one whose state is outside the enumerated domain.
    auto bad_state = show::make_transport_node(
        playing_at(1U, 2U, 0U, 10U, false));
    bad_state.properties.insert_or_assign(
        std::string{show::transport_property::transport_state},
        static_cast<std::int64_t>(7));
    SHOW_CHECK(!show::read_transport(empty.set(bad_state)));
}

// read_transport() and derive_show_frame() both run on the commit boundary, so
// neither may reach the allocator.
void reading_and_deriving_allocate_nothing() {
    mediaserver::doc::Root root;
    root = root.set(
        show::make_transport_node(playing_at(3U, 7U, 0U, 1'000U, true)));

    show::test::AllocationGuard guard;
    const auto anchor = show::read_transport(root);
    SHOW_CHECK(anchor.has_value());
    // 7 + 1000 wraps once through the inclusive 0..1000 range.
    const auto derived = show::derive_show_frame(*anchor, control(1'003U));
    SHOW_CHECK(derived == ShowFrame{6U});
    SHOW_CHECK(guard.quiet());
}

void the_authority_re_anchors_only_on_operator_action() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();

    SHOW_CHECK(transport.state() == TransportState::Stopped);
    SHOW_CHECK(transport.frame_at(control(500U)) == ShowFrame{0U});

    // Play anchors at the frame it will be sealed into, so that frame still
    // presents the position the operator was looking at.
    auto played = transport.play(control(1U));
    SHOW_CHECK(played);
    SHOW_CHECK(played.value().anchor_control_frame == control(1U));
    SHOW_CHECK(played.value().anchor_show_frame == ShowFrame{0U});
    SHOW_CHECK(transport.frame_at(control(1U)) == ShowFrame{0U});
    SHOW_CHECK(transport.frame_at(control(21U)) == ShowFrame{20U});

    // Play again is refused rather than silently re-cutting the anchor.
    SHOW_CHECK(!transport.play(control(30U)));

    // Pause freezes the position reached at the frame it lands on.
    auto paused = transport.pause(control(21U));
    SHOW_CHECK(paused);
    SHOW_CHECK(paused.value().anchor_show_frame == ShowFrame{20U});
    SHOW_CHECK(transport.frame_at(control(1'000U)) == ShowFrame{20U});

    // A seek is exact and, while paused, does not start the show.
    auto sought = transport.seek(ShowFrame{432U}, control(22U));
    SHOW_CHECK(sought);
    SHOW_CHECK(sought.value().state == TransportState::Paused);
    SHOW_CHECK(transport.frame_at(control(900U)) == ShowFrame{432U});
    SHOW_CHECK(!transport.seek(ShowFrame{1'000U}, control(23U)));

    // Resuming continues from exactly the sought frame.
    SHOW_CHECK(transport.play(control(23U)));
    SHOW_CHECK(transport.frame_at(control(28U)) == ShowFrame{437U});

    SHOW_CHECK(transport.stop(control(29U)));
    SHOW_CHECK(transport.state() == TransportState::Stopped);
    SHOW_CHECK(transport.frame_at(control(10'000U)) == ShowFrame{0U});
}

void the_authority_refuses_to_re_anchor_into_the_past() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, control(100U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();

    auto refused = transport.play(control(99U));
    SHOW_CHECK(!refused);
    SHOW_CHECK(
        refused.error() == show::TransportCommandError::ControlFrameRegression);
    SHOW_CHECK(transport.state() == TransportState::Stopped);

    SHOW_CHECK(transport.play(control(100U)));
}

void the_authority_holds_a_finished_show_exactly_once() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{5U}, false};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();
    SHOW_CHECK(transport.play(control(1U)));

    SHOW_CHECK(!transport.settle(control(5U)));
    SHOW_CHECK(transport.frame_at(control(5U)) == ShowFrame{4U});

    const auto held = transport.settle(control(6U));
    SHOW_CHECK(held.has_value());
    SHOW_CHECK(held->state == TransportState::Paused);
    SHOW_CHECK(held->anchor_show_frame == ShowFrame{5U});
    // Settling is idempotent: the anchor is already paused, so no further
    // command is authored.
    SHOW_CHECK(!transport.settle(control(7U)));
    SHOW_CHECK(transport.frame_at(control(1'000U)) == ShowFrame{5U});

    // A looping show never settles.
    const show::ShowClockConfig looping{
        rate_50, ShowFrame{0U}, ShowFrame{5U}, true};
    auto wrapped = show::TransportAuthority::create(looping, control(0U));
    SHOW_CHECK(wrapped);
    auto looper = std::move(wrapped).value();
    SHOW_CHECK(looper.play(control(1U)));
    SHOW_CHECK(!looper.settle(control(100U)));
    SHOW_CHECK(looper.frame_at(control(7U)) == ShowFrame{0U});
}

void reversing_keeps_the_position_and_turns_the_show_around() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();
    SHOW_CHECK(transport.play(control(1U)));
    SHOW_CHECK(transport.frame_at(control(51U)) == ShowFrame{50U});

    auto reversed =
        transport.set_direction(TransportDirection::Reverse, control(51U));
    SHOW_CHECK(reversed);
    SHOW_CHECK(reversed.value().anchor_show_frame == ShowFrame{50U});
    SHOW_CHECK(transport.frame_at(control(51U)) == ShowFrame{50U});
    SHOW_CHECK(transport.frame_at(control(61U)) == ShowFrame{40U});
    SHOW_CHECK(transport.frame_at(control(101U)) == ShowFrame{0U});
}

constexpr std::array<show::test::Case, 15U> cases{{
    {"a playing anchor advances one show frame per control frame",
     &a_playing_anchor_advances_one_show_frame_per_control_frame},
    {"a paused or stopped anchor suppresses the whole term",
     &a_paused_or_stopped_anchor_suppresses_the_whole_term},
    {"a non-looping show holds its last frame",
     &a_non_looping_show_holds_its_last_frame},
    {"a looping show wraps within its inclusive range",
     &a_looping_show_wraps_within_its_inclusive_range},
    {"reverse runs backwards and stops at the first frame",
     &reverse_runs_backwards_and_stops_at_the_first_frame},
    {"derivation is independent of when a node evaluates it",
     &derivation_is_independent_of_when_a_node_evaluates_it},
    {"a huge looping span never overflows the wrap",
     &a_huge_looping_span_never_overflows_the_wrap},
    {"an out-of-domain anchor reads as absent rather than as a position",
     &an_out_of_domain_anchor_reads_as_absent_rather_than_as_a_position},
    {"the document round trip preserves the anchor exactly",
     &the_document_round_trip_preserves_the_anchor_exactly},
    {"a missing or mistyped transport node reads as absent",
     &a_missing_or_mistyped_transport_node_reads_as_absent},
    {"reading and deriving allocate nothing",
     &reading_and_deriving_allocate_nothing},
    {"the authority re-anchors only on operator action",
     &the_authority_re_anchors_only_on_operator_action},
    {"the authority refuses to re-anchor into the past",
     &the_authority_refuses_to_re_anchor_into_the_past},
    {"the authority holds a finished show exactly once",
     &the_authority_holds_a_finished_show_exactly_once},
    {"reversing keeps the position and turns the show around",
     &reversing_keeps_the_position_and_turns_the_show_around},
}};

} // namespace

int main() {
    return show::test::run(cases);
}
