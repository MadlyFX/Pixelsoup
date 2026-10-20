#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::app {

struct KeyAddress final {
    doc::NodeId target{};
    scene::PropertyChannel property{};
    core::i64 frame{};

    auto operator<=>(const KeyAddress&) const = default;
};

struct KeyEditReport final {
    std::size_t applied{};
    std::vector<doc::NodeId> locked_targets;
    std::vector<KeyAddress> incompatible;
};

struct KeyCopyReport final {
    std::size_t captured{};
    std::vector<doc::NodeId> locked_targets;
    std::vector<KeyAddress> incompatible;
};

enum class KeyPasteError {
    Empty,
    FrameOutOfRange,
};

struct KeyDocumentPlan final {
    doc::Root document;
    KeyEditReport report;
};

// Director-local key clipboard. Copy snapshots value, interpolation and
// tangents from the
// projected document; no local timestamp or clipboard payload enters command
// serialization. Paste keeps each target/property and anchors the earliest
// copied frame at the active Timeline playhead.
class KeyClipboard final {
public:
    // Public only so the translation-unit helper used by Alt-duplicate can
    // share the exact immutable clipboard payload. Callers author through the
    // copy/paste API rather than constructing snapshots themselves.
    struct Snapshot final {
        doc::NodeId source_channel{};
        KeyAddress address;
        // Preserve the authored numeric carrier as well as its evaluated
        // value. Integer channel values remain integers after Copy/Paste.
        doc::PropertyValue value{0.0};
        scene::KeyInterpolation interpolation{scene::KeyInterpolation::Linear};
        // The key's tangent properties as authored, in
        // schema::channel_key_tangent_suffixes order.
        std::array<std::optional<doc::PropertyValue>, 5U> tangent{};
    };

    [[nodiscard]] KeyCopyReport copy(const doc::Root& document,
                                     scene::SceneId source_scene,
                                     std::span<const KeyAddress> selection);

    [[nodiscard]] core::Result<KeyDocumentPlan, KeyPasteError>
    paste(const doc::Root& document, scene::SceneId active_scene,
          core::u64 playhead) const;

    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

private:
    std::vector<Snapshot> keys_;
    scene::SceneId source_scene_{};
    core::i64 anchor_{};

    friend core::Result<KeyDocumentPlan, KeyPasteError>
    duplicate_keys(const doc::Root&, scene::SceneId,
                   std::span<const KeyAddress>, core::i64);
};

// Plans one Alt-drag duplicate from command-time snapshots. All destinations
// use exactly one integer delta and the caller schedules the returned document
// replacement as one transaction.
[[nodiscard]] core::Result<KeyDocumentPlan, KeyPasteError>
duplicate_keys(const doc::Root& document, scene::SceneId active_scene,
               std::span<const KeyAddress> selection, core::i64 delta);

} // namespace mediaserver::app
