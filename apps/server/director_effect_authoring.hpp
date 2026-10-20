#pragma once

// Effect stack command authoring.

#include "edit_coalescing_policy.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>

namespace mediaserver::app {

[[nodiscard]] doc::Node make_effect_node(doc::NodeId id,
                                         const editor::EffectIntent &effect);

[[nodiscard]] bool schedule_effect(cmd::DirectorScheduler &scheduler,
                                   const editor::EffectIntent &effect,
                                   doc::NodeId id, cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_film_grain_effect_edit(
    cmd::DirectorScheduler &scheduler,
    const editor::FilmGrainEffectEditIntent &request, cmd::FrameNumber apply_at,
    EditCoalescingPlan *coalescing = nullptr);

[[nodiscard]] bool
schedule_effect_delete(cmd::DirectorScheduler &scheduler,
                       const editor::EffectDeleteIntent &request,
                       cmd::FrameNumber apply_at);

} // namespace mediaserver::app
