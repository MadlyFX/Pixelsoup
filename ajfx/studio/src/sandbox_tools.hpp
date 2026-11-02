#pragma once

#include "document.hpp"
#include "sandbox.hpp"
#include "stage_camera.hpp"
#include "widgets.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// The preview's sandbox tools: a strip of keys down the picture's left edge,
// and what a click in the picture does with the one that is on.
//
// - **Select** opens the block of the solid thing clicked.
// - **Drop a thing**, **Explode here** and **Push** write an event into the
//   page at the playhead's time (sandbox.hpp): a crate that appears there,
//   an Explosion, a Push away from the camera. One edit each, so one undo.
// - **The die** deals another random seed, to the layer being worked on or,
//   with none, to the whole page.
// Kept apart from the preview's renderer, as the handles are, so the UI tests
// drive it with synthetic input and no window.
namespace ajfx::studio {

enum class SandboxTool : std::uint8_t {
    Select,
    Drop,
    Explode,
    Push,
};

inline constexpr std::array<SandboxTool, 4U> sandbox_tools{SandboxTool::Select, SandboxTool::Drop,
                                                           SandboxTool::Explode, SandboxTool::Push};

// What the preview knows at the playhead, for a click to act on.
struct SandboxScene final {
    // Where the page's solid things stand now (StageBodies), in the
    // effect's space.
    std::span<const StageBody> bodies;
    // The floor a click lands on (stage_floor).
    float floor{0.0F};
    // The playhead, in seconds: when a tool's event happens.
    float seconds{0.0F};
    // Where a tool's block goes: the page's target layer.
    Uid layer{};
    // The block being worked on, whose material a dropped thing takes.
    Uid selected{};
};

// What a click did: the tool, and the block it made or picked (or, for a
// thing that is no block's, its layer alone).
struct SandboxResult final {
    SandboxTool tool{SandboxTool::Select};
    Uid layer{};
    Uid block{};
};

class SandboxTools final {
public:
    static constexpr float key_size = 30.0F;
    static constexpr float key_gap = 4.0F;

    // Draws the strip from its top left at (x, y) downwards: the four tools,
    // then the die, which re-rolls `reroll` (a layer) or, at 0, the page.
    // Draw it before the camera takes the view, so a press on a key is the
    // key's. Returns true on the frame the die re-rolled the page.
    bool draw_strip(gui::UiContext& ctx, float x, float y, Document& document, Uid reroll);
    // The view as the camera grabbed it this frame. A click on it (pressed
    // and let go without travelling, so not an orbit) acts with the tool
    // that is on.
    std::optional<SandboxResult> use_click(gui::UiContext& ctx, const gui::Interaction& view, const StageCamera& camera,
                                           Document& document, const SandboxScene& scene);

    [[nodiscard]] SandboxTool tool() const noexcept { return tool_; }
    void set_tool(SandboxTool tool) noexcept { tool_ = tool; }
    // Where a key was drawn last frame.
    [[nodiscard]] std::optional<gui::Rect> button(SandboxTool tool) const noexcept;
    [[nodiscard]] std::optional<gui::Rect> dice() const noexcept { return dice_; }

private:
    SandboxTool tool_{SandboxTool::Select};
    std::array<std::optional<gui::Rect>, 4U> buttons_{};
    std::optional<gui::Rect> dice_;
    std::string tip_;
    // Where the press on the view began, to tell a click from an orbit.
    std::optional<gui::UiPoint> press_;
};

// A click travelling further than this, in pixels, is a drag.
inline constexpr float sandbox_click_travel = 5.0F;

} // namespace ajfx::studio
