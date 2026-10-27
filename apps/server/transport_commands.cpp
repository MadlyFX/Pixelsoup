#include "transport_commands.hpp"

#include <algorithm>

namespace mediaserver::app {

void apply_command(show::TransportAuthority &transport,
                   show::PresenterCommand command,
                   cmd::FrameNumber at) noexcept {
    const auto per_second = static_cast<core::u64>(
        show::frames_per_second(transport.anchor().rate));
    // The live range from the transport, so a range change from the editor is
    // respected by the step and jump clamps immediately.
    const auto first = transport.anchor().first_frame.value();
    const auto last = transport.anchor().last_frame.value();
    const auto current = transport.frame_at(at).value();

    const auto clamped = [first, last](core::u64 target) noexcept {
        return show::ShowFrame{std::min(std::max(target, first), last)};
    };
    const auto back = [current, first](core::u64 delta) noexcept {
        return current - first >= delta ? current - delta : first;
    };
    const auto forward = [current, last](core::u64 delta) noexcept {
        return last - current >= delta ? current + delta : last;
    };
    // Plays `direction` from where the show stands: turning it round first
    // when it runs the other way, and starting it when it is not running.
    const auto play = [&](show::TransportDirection direction) noexcept {
        if (transport.anchor().direction != direction) {
            static_cast<void>(transport.set_direction(direction, at));
        }
        if (transport.state() != show::TransportState::Playing) {
            static_cast<void>(transport.play(at));
        }
    };

    switch (command) {
    case show::PresenterCommand::TogglePlay:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(at));
        } else {
            play(show::TransportDirection::Forward);
        }
        return;
    case show::PresenterCommand::PlayForward:
        play(show::TransportDirection::Forward);
        return;
    case show::PresenterCommand::PlayReverse:
        play(show::TransportDirection::Reverse);
        return;
    case show::PresenterCommand::Pause:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(at));
        }
        return;
    case show::PresenterCommand::Go:
        // A show already running through its cues refuses GO, which is the
        // point: GO never skips content.
        static_cast<void>(transport.go(at));
        return;
    case show::PresenterCommand::StepBack:
    case show::PresenterCommand::StepForward:
    case show::PresenterCommand::JumpBack:
    case show::PresenterCommand::JumpForward:
    case show::PresenterCommand::GoToStart:
    case show::PresenterCommand::GoToEnd:
        break;
    case show::PresenterCommand::None:
    case show::PresenterCommand::Quit:
        return;
    }

    // Scrubbing is a paused operation: stepping while playing would be
    // immediately overwritten by the next boundary.
    if (transport.state() == show::TransportState::Playing) {
        static_cast<void>(transport.pause(at));
    }

    switch (command) {
    case show::PresenterCommand::StepBack:
        static_cast<void>(transport.seek(clamped(back(1U)), at));
        return;
    case show::PresenterCommand::StepForward:
        static_cast<void>(transport.seek(clamped(forward(1U)), at));
        return;
    case show::PresenterCommand::JumpBack:
        static_cast<void>(transport.seek(clamped(back(per_second)), at));
        return;
    case show::PresenterCommand::JumpForward:
        static_cast<void>(transport.seek(clamped(forward(per_second)), at));
        return;
    case show::PresenterCommand::GoToStart:
        static_cast<void>(transport.seek(transport.anchor().first_frame, at));
        return;
    case show::PresenterCommand::GoToEnd:
        static_cast<void>(transport.seek(transport.anchor().last_frame, at));
        return;
    default:
        return;
    }
}

} // namespace mediaserver::app
