#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/show/switcher_state.hpp>

namespace mediaserver::show {

enum class SwitcherCommandError : core::u8 {
    InvalidScene,
    SameScene,
    ControlFrameRegression,
    TransitionActive,
};

// Director-side state machine for Program/Rehearsal. The scene documents stay
// ordinary replicated nodes; this class only owns which scene is protected as
// Program and the exact transition anchor used when promoting Rehearsal.
class SwitcherAuthority final {
public:
    [[nodiscard]] static core::Result<SwitcherAuthority, SwitcherCommandError>
    create(doc::NodeId program_scene,
           doc::NodeId rehearsal_scene,
           cmd::FrameNumber at) noexcept;

    [[nodiscard]] core::Result<SwitcherAnchor, SwitcherCommandError>
    set_rehearsal(doc::NodeId scene, cmd::FrameNumber at) noexcept;

    [[nodiscard]] core::Result<SwitcherAnchor, SwitcherCommandError>
    swap(cmd::FrameNumber at, core::u64 duration_control_frames) noexcept;

    [[nodiscard]] SwitcherProgress progress_at(
        cmd::FrameNumber at) const noexcept {
        return derive_switcher_progress(anchor_, at);
    }

    [[nodiscard]] bool rehearsal_locked(cmd::FrameNumber at) const noexcept {
        return progress_at(at).active;
    }

    [[nodiscard]] const SwitcherAnchor& anchor() const noexcept {
        return anchor_;
    }

private:
    explicit SwitcherAuthority(const SwitcherAnchor& anchor) noexcept
        : anchor_(anchor) {}

    SwitcherAnchor anchor_{};
};

} // namespace mediaserver::show
