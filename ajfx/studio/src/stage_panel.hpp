#pragma once

#include "performance.hpp"
#include "scene.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string_view>

// The Stage panel: the tools that act in the viewport (select, place, drop,
// delete) and the setup tools for what stands on the stage (jitter, settle,
// reset). Drawn with Dear ImGui; it edits nothing itself, it says what was
// asked for and the caller does it, so the same panel is driven by a test
// with synthetic input.
namespace ajfx::studio {

enum class StageRequest : std::uint8_t { None, Jitter, Settle, Reset };

// The panel's controls, to say where each was drawn.
enum class StageControl : std::uint8_t { Select, Place, Drop, Delete, Settle, Jitter, Reset, Count };

// What the panel keeps between frames: its own settings, not the scene's.
struct StagePanelState final {
    // How far a jitter may shift a thing, metres, and tilt it, degrees.
    float shift{0.05F};
    float tilt{4.0F};
    // The jitter's seed; another one is another jitter.
    std::int32_t seed{1};
    // Where each control was drawn last frame, in the window's pixels (left,
    // top, right, bottom): what a test aims its clicks at.
    std::array<ImVec4, static_cast<std::size_t>(StageControl::Count)> where{};
};

struct StagePanelIn final {
    const Document* document{nullptr};
    // The Object picked in the scene tree, or 0.
    Uid kind{0U};
    // Whether a settle is under way, and the last thing the tools said.
    bool settling{false};
    std::string_view status;
};

// Draws the panel as a window of its own at `where` the first time it is
// seen. Sets `tool` when a tool is picked; returns what a setup button asked
// for this frame.
StageRequest draw_stage_panel(StagePanelState& state, const StagePanelIn& in, StageTool& tool, ImVec2 where);

} // namespace ajfx::studio
