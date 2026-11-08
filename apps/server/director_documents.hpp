#pragma once

// The built-in documents the Director authors at start-up: the demo scene,
// the initial feed map, the edit scene and File > New Project.

#include "playback.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>

#include <string>

namespace mediaserver::app {

[[nodiscard]] doc::Node default_compositor_output_node(doc::NodeId id,
                                                       doc::NodeId scene);

[[nodiscard]] bool author_scene_document(cmd::DirectorScheduler &scheduler,
                                         cmd::FrameNumber apply_at);

[[nodiscard]] doc::Node editor_canvas_feed_map_node(core::u32 width,
                                                    core::u32 height);

[[nodiscard]] doc::Root new_project_document();

[[nodiscard]] bool author_feed_map_document(cmd::DirectorScheduler &scheduler,
                                            cmd::FrameNumber apply_at,
                                            InitialFeedMapPlan plan);

[[nodiscard]] std::string write_demo_clip();

[[nodiscard]] bool author_edit_document(cmd::DirectorScheduler &scheduler,
                                        cmd::FrameNumber apply_at,
                                        const std::string &clip_source);

} // namespace mediaserver::app
