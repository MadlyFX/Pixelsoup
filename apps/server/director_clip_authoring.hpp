#pragma once

// Clip timing and split command authoring.

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::app {

void write_clip_timing(doc::Node &node, const scene::ClipTiming &timing);

[[nodiscard]] bool schedule_clip_timing(cmd::DirectorScheduler &scheduler,
                                        const editor::ClipTimingEdit &edit,
                                        cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_clip_split(cmd::DirectorScheduler &scheduler,
                                       const editor::ClipSplitIntent &request,
                                       doc::NodeId id,
                                       cmd::FrameNumber apply_at,
                                       scene::MediaFrameRate show_rate);

} // namespace mediaserver::app
