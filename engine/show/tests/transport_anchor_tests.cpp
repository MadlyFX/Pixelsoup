#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

void changing_rate_preserves_position_and_round_trips() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();
    SHOW_CHECK(transport.play(control(1U)));
    SHOW_CHECK(transport.frame_at(control(51U)) == ShowFrame{50U});

    const auto changed = transport.set_rate(rate_5994, control(51U));
    SHOW_CHECK(changed);
    SHOW_CHECK(changed.value().rate == rate_5994);
    SHOW_CHECK(changed.value().anchor_control_frame == control(51U));
    SHOW_CHECK(changed.value().anchor_show_frame == ShowFrame{50U});
    SHOW_CHECK(changed.value().state == TransportState::Playing);
    SHOW_CHECK(transport.frame_at(control(52U)) == ShowFrame{51U});

    mediaserver::doc::Root root;
    root = root.set(show::make_transport_node(transport.anchor()));
    const auto restored = show::read_transport(root);
    SHOW_CHECK(restored.has_value());
    SHOW_CHECK(restored->rate == rate_5994);

    const auto before = transport.anchor();
    SHOW_CHECK(!transport.set_rate(show::FrameRate{0U, 1U}, control(52U)));
    SHOW_CHECK(transport.anchor() == before);
    SHOW_CHECK(!transport.set_rate(rate_50, control(50U)));
    SHOW_CHECK(transport.anchor() == before);
}

void set_range_changes_length_and_loop_and_clamps_the_position() {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();

    // Park the show at 500, then shrink the range and turn on looping.
    SHOW_CHECK(transport.seek(ShowFrame{500U}, control(1U)));
    auto ranged =
        transport.set_range(ShowFrame{0U}, ShowFrame{120U}, true, control(2U));
    SHOW_CHECK(ranged);
    SHOW_CHECK(ranged.value().last_frame == ShowFrame{120U});
    SHOW_CHECK(ranged.value().loop);
    // The position was outside the new range, so it clamps to the last frame.
    SHOW_CHECK(ranged.value().anchor_show_frame == ShowFrame{120U});
    SHOW_CHECK(transport.anchor().first_frame == ShowFrame{0U});

    // Playing now wraps at the new, shorter length.
    SHOW_CHECK(transport.seek(ShowFrame{0U}, control(3U)));
    SHOW_CHECK(transport.play(control(4U)));
    SHOW_CHECK(transport.frame_at(control(4U + 120U)) == ShowFrame{120U});
    SHOW_CHECK(transport.frame_at(control(4U + 121U)) == ShowFrame{0U});

    // A degenerate range (last before first) is refused.
    SHOW_CHECK(
        !transport.set_range(ShowFrame{50U}, ShowFrame{10U}, false, control(200U)));
}

[[nodiscard]] show::TransportAuthority cued_transport(
    std::span<const show::CuePoint> cues,
    core::u64 last = 999U,
    bool loop = false) {
    const show::ShowClockConfig config{
        rate_50, ShowFrame{0U}, ShowFrame{last}, loop};
    auto created = show::TransportAuthority::create(config, control(0U));
    SHOW_CHECK(created);
    auto transport = std::move(created).value();
    SHOW_CHECK(transport.set_cues(cues, control(0U)));
    return transport;
}

// Settles every control frame in (from, through] and returns what each one
// presents, which is what a node sealed into that frame would draw.
[[nodiscard]] std::vector<core::u64> run_frames(
    show::TransportAuthority& transport,
    core::u64 from,
    core::u64 through) {
    std::vector<core::u64> presented;
    for (core::u64 frame = from + 1U; frame <= through; ++frame) {
        static_cast<void>(transport.settle(control(frame)));
        presented.push_back(transport.frame_at(control(frame)).value());
    }
    return presented;
}

void a_hold_cue_stops_on_its_own_frame_and_go_runs_to_the_next() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        show::CuePoint{ShowFrame{20U}, show::CueAction::Hold, 2U},
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.cue_status(control(0U)).next == 1U);
    SHOW_CHECK(transport.play(control(1U)));

    // Frame 10 is presented on control frame 11 and the hold lands on 12, so
    // the wall shows 9, 10, 10 - never an 11 that is then taken back.
    const auto presented = run_frames(transport, 1U, 14U);
    SHOW_CHECK(presented[8U] == 9U);   // control 10
    SHOW_CHECK(presented[9U] == 10U);  // control 11
    SHOW_CHECK(presented[10U] == 10U); // control 12: held
    SHOW_CHECK(presented[12U] == 10U); // control 14: still held
    SHOW_CHECK(transport.state() == TransportState::Paused);
    const auto held = transport.cue_status(control(14U));
    SHOW_CHECK(held.held == 1U);
    SHOW_CHECK(held.next == 2U);

    // GO leaves the held frame rather than stopping on it again.
    auto went = transport.go(control(15U));
    SHOW_CHECK(went);
    SHOW_CHECK(went.value().anchor_show_frame == ShowFrame{10U});
    const auto running = transport.cue_status(control(15U));
    SHOW_CHECK(running.held == 0U);
    SHOW_CHECK(running.next == 2U);
    SHOW_CHECK(running.frames_to_next == std::optional<core::u64>{10U});
    static_cast<void>(run_frames(transport, 15U, 26U));
    SHOW_CHECK(transport.frame_at(control(26U)) == ShowFrame{20U});
    SHOW_CHECK(transport.cue_status(control(26U)).held == 2U);

    // GO while a show is playing through its cues is not a skip.
    SHOW_CHECK(transport.go(control(27U)));
    SHOW_CHECK(!transport.go(control(28U)));
}

void a_loop_cue_repeats_its_section_until_go_lets_it_run_on() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        show::CuePoint{ShowFrame{14U}, show::CueAction::Loop, 2U},
        show::CuePoint{ShowFrame{30U}, show::CueAction::Hold, 3U},
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(transport.go(control(2U)));

    // 10 is the section start, so arriving back on it is not a hold.
    const auto presented = run_frames(transport, 2U, 14U);
    const std::vector<core::u64> expected{11U, 12U, 13U, 14U, 10U, 11U,
                                          12U, 13U, 14U, 10U, 11U, 12U};
    SHOW_CHECK(presented == expected);
    SHOW_CHECK(transport.state() == TransportState::Playing);
    SHOW_CHECK(transport.cue_status(control(14U)).looping == 2U);

    // GO releases the loop without moving the show: no new anchor, and the
    // section plays out once more into the rest of the show.
    const auto before = transport.anchor();
    auto released = transport.go(control(15U));
    SHOW_CHECK(released);
    SHOW_CHECK(transport.anchor() == before);
    SHOW_CHECK(transport.cue_status(control(15U)).looping == 0U);
    SHOW_CHECK(transport.cue_status(control(15U)).next == 3U);
    const auto run_on = run_frames(transport, 14U, 20U);
    const std::vector<core::u64> continued{13U, 14U, 15U, 16U, 17U, 18U};
    SHOW_CHECK(run_on == continued);
    static_cast<void>(run_frames(transport, 20U, 40U));
    SHOW_CHECK(transport.frame_at(control(40U)) == ShowFrame{30U});
    SHOW_CHECK(transport.cue_status(control(40U)).held == 3U);
}

void a_loop_with_no_earlier_cue_repeats_from_the_range_start() {
    const std::array cues{
        show::CuePoint{ShowFrame{3U}, show::CueAction::Loop, 9U},
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.play(control(1U)));
    const auto presented = run_frames(transport, 1U, 9U);
    const std::vector<core::u64> expected{1U, 2U, 3U, 0U, 1U, 2U, 3U, 0U};
    SHOW_CHECK(presented == expected);
}

void a_cue_added_behind_a_running_show_never_pulls_it_back() {
    auto transport = cued_transport({});
    SHOW_CHECK(transport.play(control(1U)));
    static_cast<void>(run_frames(transport, 1U, 50U));
    SHOW_CHECK(transport.frame_at(control(50U)) == ShowFrame{49U});

    const std::array behind{
        show::CuePoint{ShowFrame{20U}, show::CueAction::Hold, 4U},
        show::CuePoint{ShowFrame{60U}, show::CueAction::Hold, 5U},
    };
    SHOW_CHECK(transport.set_cues(behind, control(51U)));
    static_cast<void>(run_frames(transport, 50U, 70U));
    // Ran past 20 untouched and stopped on 60, the cue still ahead of it.
    SHOW_CHECK(transport.frame_at(control(70U)) == ShowFrame{60U});
    SHOW_CHECK(transport.cue_status(control(70U)).held == 5U);

    // Deleting the cue being held on ends the hold report.
    SHOW_CHECK(transport.set_cues({}, control(71U)));
    SHOW_CHECK(transport.cue_status(control(71U)).held == 0U);
}

void disarmed_cues_and_scrubbing_walk_straight_past() {
    const std::array cues{
        show::CuePoint{ShowFrame{5U}, show::CueAction::Hold, 1U},
    };
    auto transport = cued_transport(cues);
    auto disarmed = transport.set_cues_armed(false, control(1U));
    SHOW_CHECK(disarmed);
    SHOW_CHECK(!disarmed.value().cues_armed);
    SHOW_CHECK(transport.play(control(2U)));
    static_cast<void>(run_frames(transport, 2U, 12U));
    SHOW_CHECK(transport.frame_at(control(12U)) == ShowFrame{10U});
    SHOW_CHECK(transport.cue_status(control(12U)).next == 0U);

    // A seek onto a cue is the operator going there, not the show arriving.
    SHOW_CHECK(transport.set_cues_armed(true, control(13U)));
    SHOW_CHECK(transport.seek(ShowFrame{5U}, control(14U)));
    static_cast<void>(run_frames(transport, 14U, 20U));
    SHOW_CHECK(transport.state() == TransportState::Playing);
    SHOW_CHECK(transport.frame_at(control(20U)) == ShowFrame{11U});

    // Reverse play is a search, so it walks back over the cue too: 12 at
    // control 21, through 5 at control 28, to 2 at control 31.
    SHOW_CHECK(transport.set_direction(TransportDirection::Reverse,
                                       control(21U)));
    static_cast<void>(run_frames(transport, 21U, 31U));
    SHOW_CHECK(transport.frame_at(control(31U)) == ShowFrame{2U});
}

void a_looping_range_meets_its_cue_on_every_pass() {
    const std::array cues{
        show::CuePoint{ShowFrame{2U}, show::CueAction::Hold, 7U},
    };
    auto transport = cued_transport(cues, 4U, true);
    SHOW_CHECK(transport.seek(ShowFrame{3U}, control(1U)));
    SHOW_CHECK(transport.play(control(2U)));
    // 3, 4, wrap to 0, 1, 2 - held.
    static_cast<void>(run_frames(transport, 2U, 8U));
    SHOW_CHECK(transport.frame_at(control(8U)) == ShowFrame{2U});
    SHOW_CHECK(transport.state() == TransportState::Paused);
    // GO goes round the whole range and stops on the cue again.
    SHOW_CHECK(transport.go(control(9U)));
    static_cast<void>(run_frames(transport, 9U, 16U));
    SHOW_CHECK(transport.frame_at(control(15U)) == ShowFrame{2U});
    SHOW_CHECK(transport.state() == TransportState::Paused);
}

void armed_state_round_trips_and_settling_with_cues_allocates_nothing() {
    auto anchor = playing_at(1U, 2U, 0U, 10U, false);
    anchor.cues_armed = false;
    mediaserver::doc::Root root;
    root = root.set(show::make_transport_node(anchor));
    const auto decoded = show::read_transport(root);
    SHOW_CHECK(decoded.has_value());
    SHOW_CHECK(!decoded->cues_armed);

    // A transport written before cues existed reads as armed; a mistyped
    // flag is a damaged node.
    auto legacy = show::make_transport_node(anchor);
    legacy.properties.erase(std::string{show::transport_property::cues_armed});
    const auto upgraded = show::read_transport(root.set(legacy));
    SHOW_CHECK(upgraded.has_value() && upgraded->cues_armed);
    auto mistyped = show::make_transport_node(anchor);
    mistyped.properties.insert_or_assign(
        std::string{show::transport_property::cues_armed},
        static_cast<std::int64_t>(1));
    SHOW_CHECK(!show::read_transport(root.set(mistyped)));

    const std::array cues{
        show::CuePoint{ShowFrame{40U}, show::CueAction::Loop, 1U},
        show::CuePoint{ShowFrame{20U}, show::CueAction::Hold, 2U},
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.play(control(1U)));
    show::test::AllocationGuard guard;
    for (core::u64 frame = 2U; frame < 30U; ++frame) {
        static_cast<void>(transport.settle(control(frame)));
        static_cast<void>(transport.cue_status(control(frame)));
    }
    SHOW_CHECK(guard.quiet());
    // The sheet was sorted on the way in, so the earlier Hold acted first.
    SHOW_CHECK(transport.cue_status(control(29U)).held == 2U);
}

void cue_and_end_anchors_are_scheduled_and_operator_anchors_are_not() {
    const std::array cues{
        show::CuePoint{ShowFrame{5U}, show::CueAction::Hold, 1U},
        show::CuePoint{ShowFrame{9U}, show::CueAction::Loop, 2U},
    };
    auto transport = cued_transport(cues, 20U);
    auto played = transport.play(control(1U));
    SHOW_CHECK(played && !played.value().scheduled);
    const auto held = transport.settle(control(7U));
    SHOW_CHECK(held.has_value() && held->scheduled);
    SHOW_CHECK(transport.anchor().scheduled);

    // GO is the operator's: the outputs restart on it as on any action.
    auto went = transport.go(control(8U));
    SHOW_CHECK(went && !went.value().scheduled);
    static_cast<void>(run_frames(transport, 8U, 12U));
    SHOW_CHECK(transport.frame_at(control(12U)) == ShowFrame{9U});
    const auto looped = transport.settle(control(13U));
    SHOW_CHECK(looped.has_value() && looped->scheduled);
    SHOW_CHECK(looped->anchor_show_frame == ShowFrame{5U});

    // Every operator change clears it, including the transport settings.
    SHOW_CHECK(!transport.set_cues_armed(false, control(14U)).value().scheduled);
    static_cast<void>(transport.settle(control(15U)));
    SHOW_CHECK(transport.seek(ShowFrame{18U}, control(16U)));
    SHOW_CHECK(!transport.anchor().scheduled);
    // Running out of show is scheduled too.
    static_cast<void>(run_frames(transport, 16U, 22U));
    SHOW_CHECK(transport.state() == TransportState::Paused);
    SHOW_CHECK(transport.anchor().scheduled);

    mediaserver::doc::Root root;
    root = root.set(show::make_transport_node(transport.anchor()));
    const auto decoded = show::read_transport(root);
    SHOW_CHECK(decoded.has_value() && decoded->scheduled);
}

[[nodiscard]] show::CuePoint loop_cue(core::u64 frame, core::u64 id,
                                      core::u64 wait, core::u64 fade) noexcept {
    show::CuePoint cue{ShowFrame{frame}, show::CueAction::Loop, id};
    cue.wait_frames = wait;
    cue.fade_frames = fade;
    return cue;
}

void a_loop_with_a_wait_holds_its_frame_then_goes_round() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(14U, 2U, 3U, 0U),
        show::CuePoint{ShowFrame{30U}, show::CueAction::Hold, 3U},
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(transport.go(control(2U)));

    // 14 is presented on control 6 and held from 7; three frames later, on
    // control 10, the section starts again from the cue before it.
    const auto presented = run_frames(transport, 2U, 15U);
    const std::vector<core::u64> expected{11U, 12U, 13U, 14U, 14U, 14U, 14U,
                                          10U, 11U, 12U, 13U, 14U, 14U};
    SHOW_CHECK(presented == expected);
    SHOW_CHECK(transport.state() == TransportState::Paused);
    SHOW_CHECK(transport.anchor().scheduled);
    const auto waiting = transport.cue_status(control(16U));
    SHOW_CHECK(waiting.looping == 2U);
    SHOW_CHECK(waiting.held == 0U);
    SHOW_CHECK(waiting.frames_to_restart == std::optional<core::u64>{2U});

    // GO in the wait plays on from the cue frame, past the loop.
    SHOW_CHECK(transport.go(control(17U)));
    SHOW_CHECK(transport.cue_status(control(17U)).looping == 0U);
    SHOW_CHECK(!transport.cue_status(control(17U)).frames_to_restart);
    const auto run_on = run_frames(transport, 17U, 20U);
    const std::vector<core::u64> continued{15U, 16U, 17U};
    SHOW_CHECK(run_on == continued);
    static_cast<void>(run_frames(transport, 20U, 40U));
    SHOW_CHECK(transport.cue_status(control(40U)).held == 3U);
}

void a_crossfade_after_a_wait_dissolves_from_the_held_frame() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(14U, 2U, 3U, 4U),
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(transport.go(control(2U)));
    static_cast<void>(run_frames(transport, 2U, 10U));

    // The restart on control 10 opens with four frames mixing the held 14
    // into the section as it plays again from 10.
    const TransportAnchor restarted = transport.anchor();
    SHOW_CHECK(restarted.state == TransportState::Playing);
    SHOW_CHECK(restarted.blend_frames == 4U);
    SHOW_CHECK(restarted.blend_from == ShowFrame{14U});
    SHOW_CHECK(!restarted.blend_moves);
    const auto opening = show::derive_transport_blend(restarted, control(10U));
    SHOW_CHECK(opening.has_value() && opening->from == ShowFrame{14U});
    SHOW_CHECK(opening->mix() == 0.0);
    const auto halfway = show::derive_transport_blend(restarted, control(12U));
    SHOW_CHECK(halfway.has_value() && halfway->from == ShowFrame{14U});
    SHOW_CHECK(halfway->mix() == 0.5);
    SHOW_CHECK(!show::derive_transport_blend(restarted, control(14U)));
    SHOW_CHECK(!show::derive_transport_blend(restarted, control(9U)));

    // The crossfade travels with the anchor, so every node mixes alike.
    mediaserver::doc::Root root;
    root = root.set(show::make_transport_node(restarted));
    SHOW_CHECK(show::read_transport(root) == std::optional{restarted});
    auto mistyped = show::make_transport_node(restarted);
    mistyped.properties.insert_or_assign(
        std::string{show::transport_property::blend_moves},
        static_cast<std::int64_t>(0));
    SHOW_CHECK(!show::read_transport(root.set(mistyped)));

    // An operator action cuts it.
    SHOW_CHECK(transport.pause(control(11U)));
    SHOW_CHECK(transport.anchor().blend_frames == 0U);
}

void a_crossfade_without_a_wait_overlaps_the_passes() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(19U, 2U, 0U, 4U),
    };
    auto transport = cued_transport(cues);
    SHOW_CHECK(transport.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(transport.go(control(2U)));

    // 19 would be presented on control 11. The next pass starts four frames
    // earlier, on 8, and the end of this one fades out over it: 16..19 mix
    // into 10..13, and the frame after the cue is wholly the new pass.
    const auto presented = run_frames(transport, 2U, 12U);
    const std::vector<core::u64> expected{11U, 12U, 13U, 14U, 15U,
                                          10U, 11U, 12U, 13U, 14U};
    SHOW_CHECK(presented == expected);
    SHOW_CHECK(!show::derive_transport_blend(transport.anchor(), control(7U)));
    for (core::u64 frame = 8U; frame < 12U; ++frame) {
        const auto blend =
            show::derive_transport_blend(transport.anchor(), control(frame));
        SHOW_CHECK(blend.has_value());
        SHOW_CHECK(blend->from == ShowFrame{frame + 8U});
        SHOW_CHECK(blend->elapsed == frame - 8U && blend->frames == 4U);
    }
    SHOW_CHECK(!show::derive_transport_blend(transport.anchor(), control(12U)));
    SHOW_CHECK(transport.cue_status(control(12U)).looping == 2U);

    // Every pass overlaps the same way: 14 on control 12, so the next one
    // starts on 14, from 16.
    static_cast<void>(run_frames(transport, 12U, 14U));
    SHOW_CHECK(transport.frame_at(control(14U)) == ShowFrame{10U});
    const auto again =
        show::derive_transport_blend(transport.anchor(), control(14U));
    SHOW_CHECK(again.has_value() && again->from == ShowFrame{16U});

    // GO inside the overlap goes back to the pass fading out and plays on
    // past the cue from there.
    SHOW_CHECK(transport.go(control(15U)));
    SHOW_CHECK(transport.anchor().blend_frames == 0U);
    const auto run_on = run_frames(transport, 15U, 20U);
    const std::vector<core::u64> continued{18U, 19U, 20U, 21U, 22U};
    SHOW_CHECK(run_on == continued);
    SHOW_CHECK(transport.cue_status(control(20U)).looping == 0U);
}

void an_overlap_is_capped_and_a_late_arrival_fades_what_is_left() {
    const std::array cues{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(19U, 2U, 0U, 100U),
    };
    // Each pass keeps half of its ten frames: a 100 frame fade becomes 5.
    auto capped = cued_transport(cues);
    SHOW_CHECK(capped.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(capped.go(control(2U)));
    static_cast<void>(run_frames(capped, 2U, 12U));
    SHOW_CHECK(capped.anchor().blend_frames == 5U);
    SHOW_CHECK(capped.anchor().blend_from == ShowFrame{15U});

    // Played from 18 with a four frame fade, the show is already inside the
    // overlap: 19 would be presented on control 3, so one frame is left and
    // the new pass picks up where it would have been.
    const std::array short_fade{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(19U, 2U, 0U, 4U),
    };
    auto late = cued_transport(short_fade);
    SHOW_CHECK(late.seek(ShowFrame{18U}, control(1U)));
    SHOW_CHECK(late.play(control(2U)));
    static_cast<void>(late.settle(control(3U)));
    SHOW_CHECK(late.frame_at(control(3U)) == ShowFrame{13U});
    const auto left = show::derive_transport_blend(late.anchor(), control(3U));
    SHOW_CHECK(left.has_value() && left->from == ShowFrame{19U});
    SHOW_CHECK(left->frames == 1U);
    SHOW_CHECK(late.frame_at(control(4U)) == ShowFrame{14U});

    // Waiting and fading allocate nothing on the boundary.
    const std::array waited{
        show::CuePoint{ShowFrame{10U}, show::CueAction::Hold, 1U},
        loop_cue(14U, 2U, 3U, 4U),
    };
    auto quiet = cued_transport(waited);
    SHOW_CHECK(quiet.seek(ShowFrame{10U}, control(1U)));
    SHOW_CHECK(quiet.go(control(2U)));
    show::test::AllocationGuard guard;
    for (core::u64 frame = 3U; frame < 40U; ++frame) {
        static_cast<void>(quiet.settle(control(frame)));
        static_cast<void>(quiet.cue_status(control(frame)));
        static_cast<void>(
            show::derive_transport_blend(quiet.anchor(), control(frame)));
    }
    SHOW_CHECK(guard.quiet());
}

constexpr std::array<show::test::Case, 29U> cases{{
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
    {"changing rate preserves position and round trips",
     &changing_rate_preserves_position_and_round_trips},
    {"set range changes length and loop and clamps the position",
     &set_range_changes_length_and_loop_and_clamps_the_position},
    {"a hold cue stops on its own frame and GO runs to the next",
     &a_hold_cue_stops_on_its_own_frame_and_go_runs_to_the_next},
    {"a loop cue repeats its section until GO lets it run on",
     &a_loop_cue_repeats_its_section_until_go_lets_it_run_on},
    {"a loop with no earlier cue repeats from the range start",
     &a_loop_with_no_earlier_cue_repeats_from_the_range_start},
    {"a cue added behind a running show never pulls it back",
     &a_cue_added_behind_a_running_show_never_pulls_it_back},
    {"disarmed cues and scrubbing walk straight past",
     &disarmed_cues_and_scrubbing_walk_straight_past},
    {"a looping range meets its cue on every pass",
     &a_looping_range_meets_its_cue_on_every_pass},
    {"armed state round trips and settling with cues allocates nothing",
     &armed_state_round_trips_and_settling_with_cues_allocates_nothing},
    {"cue and end anchors are scheduled and operator anchors are not",
     &cue_and_end_anchors_are_scheduled_and_operator_anchors_are_not},
    {"a loop with a wait holds its frame then goes round",
     &a_loop_with_a_wait_holds_its_frame_then_goes_round},
    {"a crossfade after a wait dissolves from the held frame",
     &a_crossfade_after_a_wait_dissolves_from_the_held_frame},
    {"a crossfade without a wait overlaps the passes",
     &a_crossfade_without_a_wait_overlaps_the_passes},
    {"an overlap is capped and a late arrival fades what is left",
     &an_overlap_is_capped_and_a_late_arrival_fades_what_is_left},
}};

} // namespace

int main() {
    return show::test::run(cases);
}
