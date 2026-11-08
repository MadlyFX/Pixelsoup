#pragma once

// The supporting surfaces around the editor's viewports and timeline.
//
// These panels deliberately keep document semantics out of the UI. The scene
// tree reports a selection request, the library reports the kind of item the
// operator wants to create, and the compositor/output monitor are presently
// rich visual models of the intended workflow. The shell remains responsible
// for turning either request into an authored command.
//
// Each panel has its own header; this one gathers them for the shell.

#include <mediaserver/editor/compositor_panel.hpp>
#include <mediaserver/editor/output_monitor_panel.hpp>
#include <mediaserver/editor/preferences_panel.hpp>
#include <mediaserver/editor/project_settings_panel.hpp>
#include <mediaserver/editor/scene_panel.hpp>
#include <mediaserver/editor/scene_settings_panel.hpp>
