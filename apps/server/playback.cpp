#include "playback.hpp"

#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include <mediaserver/ui/sdl_presenter.hpp>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace mediaserver::app {
namespace {

using Clock = show::ShowClock::Clock;

// Shutdown latency ceiling. A boundary further away than this is slept to in
// steps so an interrupt is still observed promptly.
constexpr std::chrono::milliseconds max_sleep{10};

// Bound on the frames one Director iteration will seal after a late wake-up.
constexpr core::u64 max_director_catch_up = 8U;

// Barrier records are diagnostic slots that must be retired once their cohort
// completes; this is the backlog the Director tolerates before refusing to
// seal further.
constexpr std::size_t max_tracked_frames = 64U;

[[nodiscard]] bool parse_u64(std::string_view value, core::u64& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

void sleep_until_boundary(Clock::time_point boundary) noexcept {
    const auto now = Clock::now();
    if (boundary <= now) {
        std::this_thread::yield();
        return;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(boundary - now);
    std::this_thread::sleep_for(std::min(remaining, max_sleep));
}

[[nodiscard]] const char* transport_label(show::TransportState state) noexcept {
    switch (state) {
    case show::TransportState::Playing:
        return "playing";
    case show::TransportState::Paused:
        return "paused";
    case show::TransportState::Stopped:
        break;
    }
    return "stopped";
}

[[nodiscard]] const char* outcome_label(show::FrameCommitOutcome outcome) noexcept {
    switch (outcome) {
    case show::FrameCommitOutcome::Idle:
        return "idle";
    case show::FrameCommitOutcome::Ready:
        return "ready";
    case show::FrameCommitOutcome::Committed:
        return "committed";
    case show::FrameCommitOutcome::NotApproved:
        return "waiting";
    case show::FrameCommitOutcome::CheckpointPending:
        return "checkpoint";
    case show::FrameCommitOutcome::NeedsResync:
        return "resync";
    case show::FrameCommitOutcome::Unavailable:
        break;
    }
    return "disconnected";
}

[[nodiscard]] const char* presenter_error_text(
    show::PresenterError error) noexcept {
    switch (error) {
    case show::PresenterError::InvalidConfiguration:
        return "invalid surface configuration";
    case show::PresenterError::AllocationFailed:
        return "allocation failed";
    case show::PresenterError::DisplayUnavailable:
        break;
    }
    return "the platform refused a window";
}

[[nodiscard]] show::ShowClockConfig clock_config(
    const PlaybackOptions& options) noexcept {
    return show::ShowClockConfig{
        options.rate, options.first_frame, options.last_frame, options.loop};
}

[[nodiscard]] core::u64 status_interval(const PlaybackOptions& options) noexcept {
    if (options.status_interval != 0U) {
        return options.status_interval;
    }
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(options.rate));
    return per_second == 0U ? 1U : per_second;
}

// Builds the output for one role. A windowed request in a build without the
// shell is refused rather than silently downgraded: an operator who asked for a
// window and got a headless run would have no way to tell.
[[nodiscard]] std::unique_ptr<show::Presenter> make_presenter(
    const PlaybackOptions& options,
    const std::string& title,
    bool accepts_commands) {
    if (options.windowed) {
#if defined(MEDIASERVER_HAS_GUI)
        ui::SdlPresenterConfig config{};
        config.title = title;
        config.width = options.surface_width;
        config.height = options.surface_height;
        config.position_x = options.window_x;
        config.position_y = options.window_y;
        config.fullscreen = options.fullscreen;
        config.borderless = options.borderless;
        config.accepts_commands = accepts_commands;

        auto created = ui::SdlPresenter::create(config);
        if (!created) {
            std::cerr << "could not open the output window: "
                      << presenter_error_text(created.error()) << '\n';
            return nullptr;
        }
        return std::move(created).value();
#else
        static_cast<void>(title);
        static_cast<void>(accepts_commands);
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return nullptr;
#endif
    }

    auto created = show::HeadlessPresenter::create(
        options.surface_width, options.surface_height);
    if (!created) {
        std::cerr << "could not allocate the presentation surface: "
                  << presenter_error_text(created.error()) << '\n';
        return nullptr;
    }
    return std::move(created).value();
}

// Applies one operator command to the Director transport. Every command is
// expressed as a clock operation, so the position it produces is exact and the
// nodes learn about it through the ordinary sealed-frame path.
void apply_command(
    show::ShowClock& transport,
    show::PresenterCommand command,
    const PlaybackOptions& options,
    Clock::time_point now) noexcept {
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(options.rate));
    const auto first = options.first_frame.value();
    const auto last = options.last_frame.value();
    const auto current = transport.frame().value();

    const auto clamped = [first, last](core::u64 target) noexcept {
        return show::ShowFrame{std::min(std::max(target, first), last)};
    };
    const auto back = [current, first](core::u64 delta) noexcept {
        return current - first >= delta ? current - delta : first;
    };
    const auto forward = [current, last](core::u64 delta) noexcept {
        return last - current >= delta ? current + delta : last;
    };

    switch (command) {
    case show::PresenterCommand::TogglePlay:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(now));
        } else {
            static_cast<void>(transport.play(now));
        }
        return;
    case show::PresenterCommand::StepBack:
    case show::PresenterCommand::StepForward:
    case show::PresenterCommand::JumpBack:
    case show::PresenterCommand::JumpForward:
    case show::PresenterCommand::GoToStart:
        break;
    case show::PresenterCommand::None:
    case show::PresenterCommand::Quit:
        return;
    }

    // Scrubbing is a paused operation: stepping while playing would be
    // immediately overwritten by the next boundary.
    if (transport.state() == show::TransportState::Playing) {
        static_cast<void>(transport.pause(now));
    }

    switch (command) {
    case show::PresenterCommand::StepBack:
        static_cast<void>(transport.seek(clamped(back(1U)), now));
        return;
    case show::PresenterCommand::StepForward:
        static_cast<void>(transport.seek(clamped(forward(1U)), now));
        return;
    case show::PresenterCommand::JumpBack:
        static_cast<void>(transport.seek(clamped(back(per_second)), now));
        return;
    case show::PresenterCommand::JumpForward:
        static_cast<void>(transport.seek(clamped(forward(per_second)), now));
        return;
    case show::PresenterCommand::GoToStart:
        static_cast<void>(transport.seek(options.first_frame, now));
        return;
    default:
        return;
    }
}

} // namespace

bool window_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return true;
#else
    return false;
#endif
}

bool parse_frame_rate(std::string_view value, show::FrameRate& rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (
        !parse_u64(value.substr(0U, separator), numerator)
        || !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }

    if (numerator == 0U || numerator > show::max_rate_numerator
        || denominator == 0U || denominator > show::max_rate_denominator) {
        return false;
    }
    rate = show::FrameRate{
        static_cast<core::u32>(numerator), static_cast<core::u32>(denominator)};
    return true;
}

bool parse_show_range(
    std::string_view value,
    show::ShowFrame& first,
    show::ShowFrame& last) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    core::u64 begin{};
    core::u64 end{};
    if (!parse_u64(value.substr(0U, separator), begin)
        || !parse_u64(value.substr(separator + 1U), end) || end < begin
        || end - begin == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    first = show::ShowFrame{begin};
    last = show::ShowFrame{end};
    return true;
}

bool valid_playback_options(const PlaybackOptions& options) noexcept {
    return show::ShowClock::valid_configuration(clock_config(options))
        && options.surface_width != 0U && options.surface_height != 0U
        && options.surface_width <= 7'680U && options.surface_height <= 4'320U;
}

int run_node_playback(
    net::NodeSessionService& service,
    cmd::NodeCommandBus& node,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    // A node output window is input-inert on purpose: what it shows is
    // replicated state, so keyboard transport there would be a lie.
    auto presenter = make_presenter(
        options,
        "media-server node " + std::to_string(options.node_index),
        false);
    if (presenter == nullptr) {
        return 1;
    }

    net::NodeServiceFrameCommitSink sink{service, node};
    show::FrameBoundaryDriverConfig config{};
    config.clock = clock_config(options);
    config.mailbox_capacity = 16U;
    config.max_commits_per_tick = 8U;

    auto created =
        show::FrameBoundaryDriver::create(config, sink, Clock::now());
    if (!created) {
        std::cerr << "invalid node frame-boundary driver configuration\n";
        return 1;
    }
    auto driver = std::move(created).value();

    auto retirement = show::RootRetirementRing::create(32U);
    if (!retirement) {
        std::cerr << "could not allocate the node root retirement ring\n";
        return 1;
    }
    auto retired = std::move(retirement).value();

    // The node clock is a pacer, not the transport: it decides when a boundary
    // occurs, while what is presented comes from the committed document.
    if (!driver.play(Clock::now())) {
        std::cerr << "could not start the node frame pacer\n";
        return 1;
    }

    const auto interval = status_interval(options);
    show::PublishedFrame presented{};
    core::u64 boundaries = 0U;
    core::u64 presented_frames = 0U;
    core::u64 since_status = 0U;
    core::u64 present_failures = 0U;
    auto last_outcome = show::FrameCommitOutcome::Idle;
    auto closed = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
        auto ticked = driver.tick(now);
        if (!ticked) {
            // A stall long enough to invalidate the pacer is a local timing
            // fault, never a reason to present unapproved state. Re-anchor and
            // keep holding whatever the Director last approved.
            if (ticked.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(driver.seek(driver.clock().frame(), now));
                continue;
            }
            std::cerr << "node frame pacer refused a time observation (code "
                      << static_cast<unsigned>(ticked.error()) << ")\n";
            return 1;
        }

        const auto report = std::move(ticked).value();
        boundaries += report.boundaries;
        last_outcome = report.last_outcome;

        std::size_t skipped = 0U;
        show::PublishedFrame incoming{};
        if (driver.mailbox().consume_latest(incoming, skipped, &retired)) {
            // The outgoing root is parked rather than released here; the ring
            // is drained below, away from the boundary.
            static_cast<void>(retired.retire(std::move(presented.root)));
            presented = std::move(incoming);
            ++presented_frames;
        }

        const auto transport = presented.transport;
        show::TestPatternInput input{};
        input.show_frame =
            transport ? transport->show_frame : options.first_frame;
        input.control_frame = presented.control_frame;
        input.rate = transport ? transport->rate : options.rate;
        input.state =
            transport ? transport->state : show::TransportState::Stopped;
        input.node_index = options.node_index;
        input.held = report.held;
        input.needs_resync =
            report.last_outcome == show::FrameCommitOutcome::NeedsResync;

        switch (presenter->present(input)) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        since_status += report.boundaries;
        if (since_status >= interval) {
            since_status = 0U;
            std::cout << "node " << options.node_index
                      << " show-frame=" << input.show_frame.value()
                      << " control-frame=" << presented.control_frame.value()
                      << " version=" << presented.version.value()
                      << " transport=" << transport_label(input.state)
                      << " state=" << outcome_label(report.last_outcome)
                      << " boundaries=" << boundaries
                      << " presented=" << presented_frames
                      << " held=" << driver.counters().held_boundaries
                      << " skipped=" << skipped
                      << " overflows=" << driver.counters().mailbox_overflows
                      << " digest="
                      << show::frame_buffer_digest(presenter->surface()) << '\n'
                      << std::flush;
        }

        // Off-boundary housekeeping: this is the only place a superseded trie
        // is actually released.
        static_cast<void>(retired.drain());

        if (options.run_frames != 0U && boundaries >= options.run_frames) {
            break;
        }
        sleep_until_boundary(driver.clock().next_boundary_at());
    }

    std::cout << "node " << options.node_index << " stopping"
              << " show-frame="
              << (presented.transport ? presented.transport->show_frame.value()
                                      : options.first_frame.value())
              << " control-frame=" << presented.control_frame.value()
              << " version=" << presented.version.value()
              << " boundaries=" << boundaries
              << " presented=" << presented_frames
              << " committed=" << driver.counters().committed_frames
              << " held=" << driver.counters().held_boundaries
              << " checkpoints=" << driver.counters().checkpoint_activations
              << " present-failures=" << present_failures
              << " window=" << (presenter->windowed() ? "closed" : "headless")
              << " last=" << outcome_label(last_outcome) << '\n'
              << std::flush;
    return 0;
}

int run_director_show(
    cmd::DirectorScheduler& scheduler,
    net::DirectorSessionHub& hub,
    net::DirectorFrameBarrier& barrier,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    auto presenter = make_presenter(options, "media-server director", true);
    if (presenter == nullptr) {
        return 1;
    }

    const auto start = Clock::now();

    // Two clocks, deliberately. The pacer decides when a control frame is due
    // and free-runs regardless of transport state, so pausing never stops the
    // frame stream that carries transport changes and keeps the cohort's
    // acknowledgements flowing. The transport clock decides only where the show
    // is.
    auto paced = show::ShowClock::create(
        show::ShowClockConfig{
            options.rate,
            show::ShowFrame{0U},
            show::ShowFrame{std::numeric_limits<core::u64>::max() - 1U},
            false},
        start);
    auto created = show::ShowClock::create(clock_config(options), start);
    if (!paced || !created) {
        std::cerr << "invalid Director show clock configuration\n";
        return 1;
    }
    auto pacer = std::move(paced).value();
    auto transport = std::move(created).value();

    if (!pacer.play(start)) {
        std::cerr << "could not start the Director frame pacer\n";
        return 1;
    }
    if (options.autoplay && !transport.play(start)) {
        std::cerr << "could not start the Director transport\n";
        return 1;
    }

    std::deque<net::DirectorFrameBarrierId> tracked;
    std::optional<show::TransportSnapshot> authored;
    const auto interval = status_interval(options);
    core::u64 sealed = 0U;
    core::u64 since_status = 0U;
    core::u64 skipped_positions = 0U;
    core::u64 present_failures = 0U;
    auto closed = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();

        auto paced_tick = pacer.advance_to(now);
        auto transport_tick = transport.advance_to(now);
        if (!paced_tick || !transport_tick) {
            if ((!paced_tick
                 && paced_tick.error() == show::ShowClockError::TimeOverflow)
                || (!transport_tick
                    && transport_tick.error()
                        == show::ShowClockError::TimeOverflow)) {
                static_cast<void>(pacer.seek(pacer.frame(), now));
                static_cast<void>(transport.seek(transport.frame(), now));
                continue;
            }
            std::cerr << "Director show clock refused a time observation\n";
            return 1;
        }

        const auto advanced = std::move(transport_tick).value().advanced;
        const auto due =
            std::min(std::move(paced_tick).value().advanced, max_director_catch_up);
        if (advanced > 1U) {
            // The seal loop could not keep up, so the show stays on wall-clock
            // time and the intermediate positions are never published. This is
            // reported rather than smoothed: a media server holds timecode.
            skipped_positions += advanced - 1U;
        }

        for (core::u64 index = 0U; index < due; ++index) {
            if (tracked.size() >= max_tracked_frames) {
                std::cerr << "Director cohort backlog reached " << tracked.size()
                          << " unretired frames; refusing to seal further\n";
                return 1;
            }

            const auto through = scheduler.sealed_through().value();
            if (through == std::numeric_limits<core::u64>::max()) {
                std::cerr << "Director control frame numbering is exhausted\n";
                return 1;
            }
            const cmd::FrameNumber frame{through + 1U};

            // The show position travels as an ordinary command inside the
            // sealed frame, so every node derives it from the same revision.
            // Unchanged transport seals an empty frame instead, which keeps a
            // paused show cheap without stopping the stream.
            const show::TransportSnapshot snapshot{
                transport.frame(), transport.state(), options.rate};
            if (!authored || *authored != snapshot) {
                const auto command = show::schedule_transport_update(
                    scheduler,
                    snapshot,
                    frame,
                    cmd::UndoContextId{1U},
                    cmd::TransactionId{frame.value()});
                if (!command) {
                    std::cerr
                        << "could not author the transport update for frame "
                        << frame.value() << " (code "
                        << static_cast<unsigned>(command.error()) << ")\n";
                    return 1;
                }
                authored = snapshot;
            }

            auto sealed_frame = barrier.seal_and_track(hub, frame);
            if (!sealed_frame) {
                std::cerr << "Director could not seal frame " << frame.value()
                          << " (code "
                          << static_cast<unsigned>(sealed_frame.error().code)
                          << ")\n";
                return 1;
            }
            tracked.push_back(sealed_frame.value().barrier);
            ++sealed;
        }

        // Retire in order. A frame whose cohort has not finished stays, and so
        // does everything behind it, which is what bounds the backlog above.
        while (!tracked.empty() && barrier.retire(tracked.front())) {
            tracked.pop_front();
        }

        show::TestPatternInput input{};
        input.show_frame = transport.frame();
        input.control_frame = scheduler.sealed_through();
        input.rate = options.rate;
        input.state = transport.state();
        input.node_index = options.node_index;
        input.held = due == 0U;
        switch (presenter->present(input)) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        for (auto command = presenter->take_command();
             command != show::PresenterCommand::None;
             command = presenter->take_command()) {
            apply_command(transport, command, options, Clock::now());
        }

        since_status += due;
        if (due != 0U && since_status >= interval) {
            since_status = 0U;
            std::cout << "director show-frame=" << transport.frame().value()
                      << " control-frame=" << scheduler.sealed_through().value()
                      << " transport=" << transport_label(transport.state())
                      << " sealed=" << sealed
                      << " skipped-positions=" << skipped_positions
                      << " tracked=" << tracked.size()
                      << " sessions=" << hub.active_subscriptions()
                      << " released-through="
                      << barrier.released_through().through_frame.value() << '\n'
                      << std::flush;
        }

        if (options.run_frames != 0U && sealed >= options.run_frames) {
            break;
        }
        sleep_until_boundary(pacer.next_boundary_at());
    }

    std::cout << "director stopping"
              << " show-frame=" << transport.frame().value()
              << " control-frame=" << scheduler.sealed_through().value()
              << " transport=" << transport_label(transport.state())
              << " sealed=" << sealed
              << " skipped-positions=" << skipped_positions
              << " tracked=" << tracked.size()
              << " present-failures=" << present_failures
              << " window=" << (presenter->windowed() ? "closed" : "headless")
              << " released-through="
              << barrier.released_through().through_frame.value() << '\n'
              << std::flush;
    return 0;
}

} // namespace mediaserver::app
