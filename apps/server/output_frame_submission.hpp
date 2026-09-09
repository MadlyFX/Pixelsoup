#pragma once

#include <optional>

#include <mediaserver/core/types.hpp>
#include <mediaserver/show/presenter.hpp>

namespace mediaserver::app {

// Identifies the evaluated output, including local proxy/pose changes and the
// publication generation. Document version covers graphs, transport and edits;
// control frame also distinguishes an exact asset activation from a held image.
struct OutputFrameKey final {
    core::u64 control_frame{};
    core::u64 show_frame{};
    core::u64 document_version{};
    core::u64 scene_digest{};
    core::u64 transition_from_digest{};
    core::u64 transition_to_digest{};
    core::u64 feed_map_digest{};
    core::u64 asset_manifest_digest{};

    bool operator==(const OutputFrameKey &) const = default;
};

// Each physical output acknowledges its own submission. A slow neighbour must
// retry without making an already submitted output record the same position
// again. Housekeeping still pumps the window and retires completed GPU work.
class OutputFrameSubmission final {
  public:
    template <typename Present, typename Hold>
    [[nodiscard]] show::PresenterStatus present(const OutputFrameKey &key,
                                               Present &&submit,
                                               Hold &&hold) noexcept(
        noexcept(submit()) && noexcept(hold())) {
        if (submitted_ == key) {
            return hold();
        }
        const auto status = submit();
        // Skipped can mean that only the offscreen candidate was submitted.
        // It must get another opportunity to reach the connector, even when
        // the scene and timeline position have not changed.
        if (status == show::PresenterStatus::Presented) {
            submitted_ = key;
        } else {
            submitted_.reset();
        }
        return status;
    }

    // A replacement presenter has neither this image nor its temporal history.
    void reset() noexcept { submitted_.reset(); }

  private:
    std::optional<OutputFrameKey> submitted_;
};

} // namespace mediaserver::app
