#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::app {

enum class ClipPasteError {
    Empty,
    InvalidTargetScene,
    FrameOutOfRange,
    IdExhausted,
    InvalidSnapshot,
};

struct ClipPastePlan final {
    doc::Root document;
    std::vector<doc::Node> created;
    core::u64 next_node{};
};

// The Director's local, non-replicated clip clipboard. Copy captures immutable
// document nodes; Paste builds a replacement snapshot containing newly-id'd
// copies, which the caller schedules through the ordinary command stream.
class ClipClipboard final {
public:
    [[nodiscard]] bool copy(
        const doc::Root& document,
        std::span<const doc::NodeId> selection);

    [[nodiscard]] core::Result<ClipPastePlan, ClipPasteError> paste(
        const doc::Root& document,
        scene::SceneId target_scene,
        core::u64 playhead,
        core::u64 next_node,
        std::optional<doc::NodeId> target_lane = std::nullopt) const;

    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::optional<core::i64> anchor() const noexcept {
        return nodes_.empty() ? std::optional<core::i64>{}
                              : std::optional<core::i64>{anchor_};
    }
    [[nodiscard]] std::optional<core::i64> span_duration() const noexcept {
        return nodes_.empty() ? std::optional<core::i64>{}
                              : std::optional<core::i64>{span_duration_};
    }
    [[nodiscard]] bool single_source_lane() const noexcept {
        return !nodes_.empty() && !multiple_source_lanes_;
    }

private:
    std::vector<doc::Node> nodes_;
    core::i64 anchor_{};
    core::i64 span_duration_{};
    bool multiple_source_lanes_{};
};

} // namespace mediaserver::app
