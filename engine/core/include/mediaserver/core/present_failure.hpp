#pragma once

#include <mediaserver/core/types.hpp>

#include <string>
#include <string_view>

namespace mediaserver::core {

// Why the GPU refused a command it was asked to record. Each of these repeats
// on every frame that asks for the same work, so a node that hits one keeps
// failing until whatever it draws changes - which is why it is reported to the
// operator rather than only counted.
enum class GpuRefusal : u8 {
    None = 0U,
    // The frame's fixed upload space was full, or one upload was larger than
    // a single upload page can hold.
    UploadSpaceFull = 1U,
    // A buffer was rewritten after an earlier draw in the same frame had
    // already read it in place, so changing it would change that draw too.
    BufferInUse = 2U,
};

// The step of presenting a frame that failed.
enum class PresentStep : u8 {
    None = 0U,
    // The output was not set up to draw what it was given.
    Setup = 1U,
    // Drawing the frame, outside any one map.
    Draw = 2U,
    // Drawing one map; `map_node` names it.
    DrawMap = 3U,
    // A map's projection surface could not be resolved.
    MapProjection = 4U,
    // Handing the recorded frame to the GPU.
    Submit = 5U,
};

// A node's failed presentation attempts, and the latest one's cause.
struct PresentFailure final {
    u64 failed_frames{};
    PresentStep step{PresentStep::None};
    GpuRefusal refusal{GpuRefusal::None};
    // The output map's document node for DrawMap, otherwise zero.
    u64 map_node{};
    // The show frame the latest failure was attempting.
    u64 show_frame{};
    // The latest attempt failed. Cleared by the next frame that presents.
    bool ongoing{};

    bool operator==(const PresentFailure&) const = default;
};

// Operator wording: what failed, not how the renderer is built.
[[nodiscard]] constexpr std::string_view
present_step_text(PresentStep step) noexcept {
    switch (step) {
    case PresentStep::None:
        return "nothing failed";
    case PresentStep::Setup:
        return "the output is not set up to draw this";
    case PresentStep::Draw:
        return "drawing the frame failed";
    case PresentStep::DrawMap:
        return "drawing a map failed";
    case PresentStep::MapProjection:
        return "a map's projection surface is unavailable";
    case PresentStep::Submit:
        return "the GPU did not accept the frame";
    }
    return "unknown step";
}

[[nodiscard]] constexpr std::string_view
gpu_refusal_text(GpuRefusal refusal) noexcept {
    switch (refusal) {
    case GpuRefusal::None:
        return {};
    case GpuRefusal::UploadSpaceFull:
        return "the frame ran out of GPU upload space";
    case GpuRefusal::BufferInUse:
        return "a GPU buffer was rewritten after an earlier map in the "
               "same frame had used it";
    }
    return "unknown GPU refusal";
}

// One line for a log: the step, the map's node id, the GPU's reason and the
// show frame. The Output Monitor names the map instead of numbering it.
[[nodiscard]] inline std::string
present_failure_text(const PresentFailure& failure) {
    std::string text{present_step_text(failure.step)};
    if (failure.map_node != 0U) {
        text += " (map node " + std::to_string(failure.map_node) + ")";
    }
    if (const auto reason = gpu_refusal_text(failure.refusal); !reason.empty()) {
        text += ": ";
        text += reason;
    }
    if (failure.show_frame != 0U) {
        text += ", show frame " + std::to_string(failure.show_frame);
    }
    return text;
}

} // namespace mediaserver::core
