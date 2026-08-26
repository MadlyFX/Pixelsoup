#include <mediaserver/show/switcher_authority.hpp>

#include <utility>

namespace mediaserver::show {

core::Result<SwitcherAuthority, SwitcherCommandError>
SwitcherAuthority::create(doc::NodeId program_scene,
                          doc::NodeId rehearsal_scene,
                          cmd::FrameNumber at) noexcept {
    if (program_scene == doc::NodeId{} || rehearsal_scene == doc::NodeId{}) {
        return core::Result<SwitcherAuthority, SwitcherCommandError>::failure(
            SwitcherCommandError::InvalidScene);
    }
    if (program_scene == rehearsal_scene) {
        return core::Result<SwitcherAuthority, SwitcherCommandError>::failure(
            SwitcherCommandError::SameScene);
    }
    SwitcherAnchor anchor{};
    anchor.program_scene = program_scene;
    anchor.rehearsal_scene = rehearsal_scene;
    anchor.transition_from = program_scene;
    anchor.transition_to = program_scene;
    anchor.anchor_control_frame = at;
    return core::Result<SwitcherAuthority, SwitcherCommandError>::success(
        SwitcherAuthority{anchor});
}

core::Result<SwitcherAnchor, SwitcherCommandError>
SwitcherAuthority::set_rehearsal(doc::NodeId scene,
                                 cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::ControlFrameRegression);
    }
    if (rehearsal_locked(at)) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::TransitionActive);
    }
    if (scene == doc::NodeId{}) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::InvalidScene);
    }
    if (scene == anchor_.program_scene) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::SameScene);
    }
    anchor_.rehearsal_scene = scene;
    return core::Result<SwitcherAnchor, SwitcherCommandError>::success(anchor_);
}

core::Result<SwitcherAnchor, SwitcherCommandError>
SwitcherAuthority::swap(cmd::FrameNumber at,
                        core::u64 duration_control_frames) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::ControlFrameRegression);
    }
    if (rehearsal_locked(at)) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::TransitionActive);
    }

    const doc::NodeId old_program = anchor_.program_scene;
    const doc::NodeId old_rehearsal = anchor_.rehearsal_scene;
    anchor_.program_scene = old_rehearsal;
    anchor_.rehearsal_scene = old_program;
    anchor_.transition_from = old_program;
    anchor_.transition_to = old_rehearsal;
    anchor_.anchor_control_frame = at;
    anchor_.duration_control_frames = duration_control_frames;
    if (!valid_switcher_anchor(anchor_)) {
        return core::Result<SwitcherAnchor, SwitcherCommandError>::failure(
            SwitcherCommandError::InvalidScene);
    }
    return core::Result<SwitcherAnchor, SwitcherCommandError>::success(anchor_);
}

} // namespace mediaserver::show
